/* Vulkan submissions, actual fence completion and original Metal blocks.
 * Texture layout changes are published only when their command is submitted. */
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalCommandTexture { struct MetalTexture *texture; VkImageLayout initial,layout; VkAccessFlags initial_access; unsigned layout_used; struct MetalCommandTexture *next,*hash_next; };
struct MetalCommandHandler { Word block; struct MetalCommandHandler *next; };
struct MetalBlitEncoder { struct MetalResource object; struct MetalCommand *command; unsigned ended,debug_depth; struct MetalLifetime lifetime; };
static _Thread_local struct MetalCommand *metal_completing_command;
static void metal_render_cleanup(struct MetalCommand *);
static void metal_render_release_encoders(struct MetalCommand *);
static void metal_snapshot_cleanup(struct MetalCommand *);
static VkSemaphore metal_drawable_signal(struct MetalCommand *);
static VkResult metal_drawable_present(struct MetalCommand *);
static void metal_first_frame_complete(struct MetalCommand *);
static void metal_drawable_complete(struct MetalCommand *);
static void metal_command_destroy(Word value) {
    struct MetalCommand *command=(void *)value;
    struct MetalQueue *queue=(void *)command->object.content[0];
    if(command->pool_owner) {
        struct MetalCommandPool *pool=command->pool_owner;
        pthread_mutex_lock(&pool->mutex);VkResult result=VKD(vkResetCommandPool)(vulkan.device,pool->native,0);pthread_mutex_unlock(&pool->mutex);
        metal_render_cleanup(command);
        while(command->textures){struct MetalCommandTexture *next=command->textures->next;free(command->textures);command->textures=next;}
        metal_command_release_resources(command);metal_snapshot_cleanup(command);command->pool_owner=NULL;
        if(result==VK_SUCCESS)metal_pool_return(queue,pool);
        else metal_pool_destroy(queue,pool);
    }
    metal_render_release_encoders(command);
    struct MetalCommandHandler *handlers[]={command->scheduled,command->completed};
    for(unsigned i=0;i<COUNT(handlers);++i)while(handlers[i]) {
        struct MetalCommandHandler *next=handlers[i]->next;_Block_release((void *)handlers[i]->block);free(handlers[i]);handlers[i]=next;
    }
    metal_drawable_complete(command);metal_object_release((Word)queue);
    pthread_cond_destroy(&command->condition);pthread_mutex_destroy(&command->mutex);
    metal_memory_sub(4,sizeof(*command));free(command);
}

static struct MetalCommandTexture *metal_command_texture(struct MetalCommand *command,struct MetalTexture *texture) {
    size_t bucket=metal_command_resource_hash((Word)texture,COUNT(command->texture_buckets));
    for(struct MetalCommandTexture *use=command->texture_buckets[bucket];use;use=use->hash_next)
        if(use->texture==texture)return use;
    /* Keep address order for submission locks; repeated recording lookups use
     * the command-local index instead of walking the ordered journal again. */
    struct MetalCommandTexture **slot=&command->textures;
    while (*slot && (Word)(*slot)->texture<(Word)texture) slot=&(*slot)->next;
    struct MetalCommandTexture *use=calloc(1,sizeof(*use));
    if (!use) stop("Allocation failed","Metal texture layout journal");
    use->texture=texture;
    metal_command_use(command,(Word)texture);
     
    use->initial=use->layout=VK_IMAGE_LAYOUT_GENERAL;
    use->initial_access=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    use->next=*slot; *slot=use;
    use->hash_next=command->texture_buckets[bucket];command->texture_buckets[bucket]=use;
    return use;
}
static void metal_command_add_handler(struct MetalCommand *command,const char *selector,Word block) {
    if (!block) stop("Nil Metal command handler",selector);
    struct MetalCommandHandler *handler=calloc(1,sizeof(*handler));
    if (!handler) stop("Allocation failed",selector);
    handler->block=(Word)_Block_copy((void *)block);
    pthread_mutex_lock(&command->mutex);
    if (command->object.content[1]>=2) { pthread_mutex_unlock(&command->mutex); _Block_release((void *)handler->block); free(handler); stop("Command handler added after commit",selector); }
    struct MetalCommandHandler **tail=!strcmp(selector,"addScheduledHandler:") ? &command->scheduled : &command->completed;
    while (*tail) tail=&(*tail)->next;
    *tail=handler; pthread_mutex_unlock(&command->mutex);
}
static int metal_command_handlers(struct MetalCommand *command,struct MetalCommandHandler *handlers) {
    int failed=0;
    while (handlers) {
        struct MetalCommandHandler *next=handlers->next;
        may_unwind=1; snprintf(stage,sizeof(stage),"Original Metal command completion handler");
        Word parent_pool=active_pool; pool_push();
        int error=setjmp(boundary);
        if (!error) ((void (*)(Word,Word))read_word(handlers->block+16))(handlers->block,(Word)command);
        else { LOG("Metal command callback stopped: %s",status); failed=1; }
        while (active_pool!=parent_pool) pool_pop(active_pool);
        may_unwind=0;
        _Block_release((void *)handlers->block); free(handlers); handlers=next;
    }
    return failed;
}
static void metal_command_complete(void *context) {
    struct MetalCommand *command=context;
    struct MetalQueue *queue=(void *)command->object.content[0];
    metal_completing_command=command;
    unsigned failed=0,retire_safe=1;
    if (metal_command_status(&command->object)==3) {
        failed|=metal_command_handlers(command,command->scheduled); command->scheduled=NULL;
        VkResult result=VKD(vkWaitForFences)(vulkan.device,1,&command->fence,VK_TRUE,UINT64_MAX);
        if (result==VK_SUCCESS) metal_diagnostic_command_complete(command);
        if (result!=VK_SUCCESS) {
            retire_safe=result==VK_ERROR_DEVICE_LOST;
            char detail[128]; snprintf(detail,sizeof(detail),"Vulkan command fence failed: %d",result);
            metal_error(&command->error,"MTLCommandBufferErrorDomain",1,detail);
        }
        __atomic_store_n(&command->object.content[1],result==VK_SUCCESS ? 4 : 5,__ATOMIC_RELEASE);
    } else {
        struct MetalCommandHandler *handler=command->scheduled;
        while (handler) { struct MetalCommandHandler *next=handler->next; _Block_release((void *)handler->block); free(handler); handler=next; }
        command->scheduled=NULL;
    }
    pthread_mutex_lock(&command->mutex); command->gpu_done=retire_safe; pthread_mutex_unlock(&command->mutex);
    if (metal_command_status(&command->object)==4) metal_first_frame_complete(command);
    failed|=metal_command_handlers(command,command->completed); command->completed=NULL;
    if (retire_safe) {
        struct MetalCommandPool *pool=command->pool_owner;
        pthread_mutex_lock(&pool->mutex);
        /* The owning pool keeps these immutable handles. Its reset happens only
         * after the real fence and original completion callbacks have finished. */
        command->command=VK_NULL_HANDLE;
        command->prologue=VK_NULL_HANDLE;
        VkResult reset=VKD(vkResetCommandPool)(vulkan.device,pool->native,0);
        pthread_mutex_unlock(&pool->mutex);
        metal_render_cleanup(command);
        struct MetalCommandTexture *use=command->textures;
        while (use) { struct MetalCommandTexture *next=use->next; free(use); use=next; }
        command->textures=NULL;
        memset(command->texture_buckets,0,sizeof(command->texture_buckets));
        metal_command_release_resources(command);
        metal_snapshot_cleanup(command);
        /* Do not publish the recording pool until its previous ownership set is
         * empty; another recording thread may acquire it immediately. */
        command->pool_owner=NULL;
        if(reset==VK_SUCCESS)metal_pool_return(queue,pool);
        else {
            metal_pool_destroy(queue,pool);
            char detail[80];snprintf(detail,sizeof(detail),"vkResetCommandPool result=%d",reset);
            record_failure("Vulkan command pool reset failed",detail);
        }
        command->fence=VK_NULL_HANDLE;
    } else LOG("Vulkan ownership remains pinned: command fence did not establish safe retirement");
#if I2A_DEVELOPMENT
    static unsigned logged;
    if (__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<24)
        LOG("Vulkan Metal command completed: operations=%u status=%lu",command->recorded_operations,metal_command_status(&command->object));
#endif
    pthread_mutex_lock(&command->mutex);
    command->callback_failed=failed; command->handlers_done=1; pthread_cond_broadcast(&command->condition);
    pthread_mutex_unlock(&command->mutex); metal_completing_command=NULL;
    if (retire_safe) metal_drawable_complete(command);
    if(retire_safe)metal_object_release((Word)command);
}
static int metal_command_texture_barrier(const struct MetalCommandTexture *use) {
    /* Writes transition out of SHADER_READ_ONLY; that transition already makes
     * them visible to later shader reads on this queue. Read-after-read needs
     * neither another layout transition nor a full pipeline dependency. */
    return use->layout_used && !(use->texture->layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        use->initial==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && use->initial_access==VK_ACCESS_SHADER_READ_BIT);
}
static void metal_command_commit(struct MetalCommand *command,const char *selector) {
    uint64_t diagnostic_commit=render_diagnostic_clock();
    struct MetalQueue *queue=(void *)command->object.content[0];
    if (!vulkan.available || !queue->execution_enabled) stop("Metal submission backend unavailable or queue disabled",selector);
    pthread_mutex_lock(&command->mutex);
    if (command->object.content[1]>=2 || command->active_encoder) {
        pthread_mutex_unlock(&command->mutex); stop("Command already committed or encoder still active",selector);
    }
    __atomic_store_n(&command->object.content[1],2,__ATOMIC_RELEASE);
     
    unsigned needs_prologue=0;
    for (struct MetalCommandTexture *use=command->textures;use;use=use->next) if(use->layout_used) {
        pthread_mutex_lock(&use->texture->mutex);
        needs_prologue|=metal_command_texture_barrier(use);
    }
     
    if (metal_upload_flush()!=VK_SUCCESS) stop("Vulkan upload batch submission failed",selector);
    pthread_mutex_lock(&command->pool_owner->mutex);
    metal_diagnostic_command_end(command);
    VkResult result=VKD(vkEndCommandBuffer)(command->command);
    if (result==VK_SUCCESS && needs_prologue) {
        VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command->pool_owner->native,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        command->prologue=command->pool_owner->prologue;
        if (!command->prologue) result=VKD(vkAllocateCommandBuffers)(vulkan.device,&allocate,&command->prologue);
        if (result==VK_SUCCESS) command->pool_owner->prologue=command->prologue;
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (result==VK_SUCCESS) result=VKD(vkBeginCommandBuffer)(command->prologue,&begin);
        if (result==VK_SUCCESS) {
             
            VkImageMemoryBarrier barriers[64];
            uint32_t count=0;
            VkPipelineStageFlags sources=0;
            for (struct MetalCommandTexture *use=command->textures;use;use=use->next) {
                if(!metal_command_texture_barrier(use))continue;
                struct MetalTexture *texture=use->texture;
                barriers[count++]=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask=texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask=use->initial_access,.oldLayout=texture->layout,.newLayout=use->initial,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=texture->image,
                    .subresourceRange={texture->format.aspects,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
                sources|=texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                if (count==COUNT(barriers)) {
                    VKD(vkCmdPipelineBarrier)(command->prologue,sources,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,count,barriers);
                    count=0; sources=0;
                }
            }
            if(count)VKD(vkCmdPipelineBarrier)(command->prologue,sources,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,count,barriers);
            result=VKD(vkEndCommandBuffer)(command->prologue);
        }
    }
    pthread_mutex_unlock(&command->pool_owner->mutex);
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (result==VK_SUCCESS) {
        command->fence=command->pool_owner->fence;
        if (command->fence) result=VKD(vkResetFences)(vulkan.device,1,&command->fence);
        else { result=VKD(vkCreateFence)(vulkan.device,&fence,NULL,&command->fence); if (result==VK_SUCCESS) command->pool_owner->fence=command->fence; }
    }
    VkResult presented=VK_SUCCESS;
    if (result==VK_SUCCESS) {
        VkCommandBuffer commands[2]={command->prologue,command->command};
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=command->prologue ? 2 : 1,.pCommandBuffers=commands+(command->prologue ? 0 : 1)};
        VkSemaphore signal=metal_drawable_signal(command);
        if (signal) { submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&signal; }
        uint64_t diagnostic_lock=render_diagnostic_clock();
        pthread_mutex_lock(&vulkan_mutex);
        render_diagnostic_cost(8,diagnostic_lock);
        uint64_t diagnostic_submit=render_diagnostic_clock();
        result=VKD(vkQueueSubmit)(vulkan.queue,1,&submit,command->fence);
        render_diagnostic_cost(4,diagnostic_submit);
        if (result==VK_SUCCESS && command->drawable) {
            presented=metal_drawable_present(command);
            if (presented!=VK_SUCCESS && presented!=VK_SUBOPTIMAL_KHR) LOG("Vulkan presentation failed: %d",presented);
        }
        pthread_mutex_unlock(&vulkan_mutex);
    }
    if(presented!=VK_SUCCESS && presented!=VK_SUBOPTIMAL_KHR && presented!=VK_ERROR_OUT_OF_DATE_KHR && presented!=VK_ERROR_SURFACE_LOST_KHR) {
        char detail[96];snprintf(detail,sizeof(detail),"vkQueuePresentKHR result=%d",presented);record_failure("Vulkan queue presentation failed",detail);
    }
    for (struct MetalCommandTexture *use=command->textures;use;use=use->next) if(use->layout_used) {
        if (result==VK_SUCCESS) use->texture->layout=use->layout;
        pthread_mutex_unlock(&use->texture->mutex);
    }
    if (result!=VK_SUCCESS) {
        char detail[160]; snprintf(detail,sizeof(detail),"Vulkan command submission failed: %d",result);
        metal_error(&command->error,"MTLCommandBufferErrorDomain",1,detail);
    }
    __atomic_store_n(&command->object.content[1],result==VK_SUCCESS ? 3 : 5,__ATOMIC_RELEASE);
    pthread_cond_broadcast(&command->condition); pthread_mutex_unlock(&command->mutex);
    metal_object_retain((Word)command);
    metal_completion_enqueue(metal_completion_queue,command,metal_command_complete);
    render_diagnostic_cost(13,diagnostic_commit);
}
static void metal_command_wait(struct MetalCommand *command,const char *selector) {
    if (metal_completing_command==command) return;
    uint64_t diagnostic_start=render_diagnostic_clock();
    pthread_mutex_lock(&command->mutex);
    if (command->object.content[1]<2) { pthread_mutex_unlock(&command->mutex); stop("Waiting on an uncommitted Metal command",selector); }
    int completed=!strcmp(selector,"waitUntilCompleted");
    while (completed ? !command->handlers_done : metal_command_status(&command->object)<3) pthread_cond_wait(&command->condition,&command->mutex);
    int failed=command->callback_failed;
    pthread_mutex_unlock(&command->mutex);
    render_diagnostic_cost(2,diagnostic_start);
    if (completed && failed) stop("Original Metal completion callback failed",selector);
}
static Word metal_command_error(struct MetalCommand *command) { return command->error; }
static void metal_blit_destroy(Word value) {free((void *)value);}
static Word metal_blit_encoder(struct MetalCommand *command,const char *selector) {
    struct MetalBlitEncoder *encoder=calloc(1,sizeof(*encoder));
    if (!encoder) return 0;
    encoder->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalBlitCommandEncoder"),.device=metal_command_device(&command->object)};
    encoder->command=(void *)metal_object_retain((Word)command);
    pthread_mutex_lock(&command->mutex);
    if (command->object.content[1]>=2 || command->active_encoder) {pthread_mutex_unlock(&command->mutex);metal_object_release((Word)command);free(encoder);stop("Command cannot create a blit encoder",selector);}
    command->active_encoder=(Word)encoder;pthread_mutex_unlock(&command->mutex);
    metal_lifetime_init((Word)encoder,__builtin_offsetof(struct MetalBlitEncoder,lifetime),metal_blit_destroy);
    return metal_retain_autorelease((Word)encoder);
}
static void metal_blit_check(struct MetalBlitEncoder *encoder,const char *selector) {
    if (encoder->ended || encoder->command->active_encoder!=(Word)encoder || metal_command_status(&encoder->command->object)>=2)
        stop("Metal blit encoder is not recording",selector);
}
static void metal_blit_end(struct MetalBlitEncoder *encoder,const char *selector) {
    metal_blit_check(encoder,selector);
    encoder->ended=1; encoder->command->active_encoder=0;
    metal_object_release((Word)encoder->command);metal_object_release((Word)encoder);
}
static void metal_blit_mipmaps(struct MetalBlitEncoder *encoder,const char *selector,struct MetalTexture *texture) {
    metal_blit_check(encoder,selector);
    if (!texture || texture->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalTexture") || texture->samples!=1 || texture->format.aspects!=VK_IMAGE_ASPECT_COLOR_BIT)
        stop("Invalid mipmap-generation texture",selector);
    if (texture->mips==1) return;
    VkFormatProperties properties;
    PFN_vkGetPhysicalDeviceFormatProperties query=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
    query(vulkan.physical,texture->format.format,&properties);
    VkFormatFeatureFlags required=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures&required)!=required) stop("Vulkan linear mipmap generation unsupported for this format",selector);
    struct MetalCommandTexture *use=metal_command_texture(encoder->command,texture);
    use->layout_used=1;
    VkCommandBuffer command=encoder->command->command;
    pthread_mutex_lock(&encoder->command->pool_owner->mutex);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=use->layout==VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=use->layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=texture->image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
    VKD(vkCmdPipelineBarrier)(command,use->layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    uint32_t width=(uint32_t)texture->width,height=(uint32_t)texture->height,depth=(uint32_t)texture->depth;
    for (unsigned level=0;level<texture->mips;++level) {
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.subresourceRange.baseMipLevel=level; barrier.subresourceRange.levelCount=1;
        VKD(vkCmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        if (level+1==texture->mips) break;
        uint32_t next_width=width>1 ? width/2 : 1,next_height=height>1 ? height/2 : 1,next_depth=depth>1 ? depth/2 : 1;
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,(uint32_t)texture->layers},
            .srcOffsets={{0,0,0},{(int32_t)width,(int32_t)height,(int32_t)depth}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level+1,0,(uint32_t)texture->layers},
            .dstOffsets={{0,0,0},{(int32_t)next_width,(int32_t)next_height,(int32_t)next_depth}}};
        VKD(vkCmdBlitImage)(command,texture->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
        width=next_width; height=next_height; depth=next_depth;
    }
    VkImageLayout final_layout=(texture->native_usage&VK_IMAGE_USAGE_SAMPLED_BIT) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout=final_layout;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
    barrier.subresourceRange.baseMipLevel=0; barrier.subresourceRange.levelCount=(uint32_t)texture->mips;
    VKD(vkCmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
    pthread_mutex_unlock(&encoder->command->pool_owner->mutex);
    use->layout=final_layout; ++encoder->command->recorded_operations;
#if I2A_DEVELOPMENT
    if (render_diagnostics_requested) {
        static unsigned traces;
        if (__atomic_fetch_add(&traces,1,__ATOMIC_RELAXED)<16)
            LOG("Vulkan mipmap generation recorded: %lux%lu levels=%lu layers=%lu",texture->width,texture->height,texture->mips,texture->layers);
    }
#endif
}
static void metal_blit_debug_push(struct MetalBlitEncoder *encoder,const char *selector,Word text) { metal_blit_check(encoder,selector); if (!text) stop("Nil debug group",selector); ++encoder->debug_depth; }
static void metal_blit_debug_pop(struct MetalBlitEncoder *encoder,const char *selector) { metal_blit_check(encoder,selector); if (!encoder->debug_depth) stop("Unbalanced debug group",selector); --encoder->debug_depth; }
static Method metal_commands_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalCommandBuffer")) {
        if (!strcmp(selector,"commit")) return (Method)metal_command_commit;
        if (!strcmp(selector,"waitUntilCompleted") || !strcmp(selector,"waitUntilScheduled")) return (Method)metal_command_wait;
        if (!strcmp(selector,"addCompletedHandler:") || !strcmp(selector,"addScheduledHandler:")) return (Method)metal_command_add_handler;
        if (!strcmp(selector,"error")) return (Method)metal_command_error;
        if (!strcmp(selector,"blitCommandEncoder")) return (Method)metal_blit_encoder;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalBlitCommandEncoder")) {
        if (!strcmp(selector,"device")) return (Method)metal_resource_device;
        if (!strcmp(selector,"label")) return (Method)metal_resource_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
        if (!strcmp(selector,"endEncoding")) return (Method)metal_blit_end;
        if (!strcmp(selector,"generateMipmapsForTexture:")) return (Method)metal_blit_mipmaps;
        if (!strcmp(selector,"pushDebugGroup:")) return (Method)metal_blit_debug_push;
        if (!strcmp(selector,"popDebugGroup")) return (Method)metal_blit_debug_pop;
        if (!strcmp(selector,"insertDebugSignpost:")) return (Method)noop;
    }
    return NULL;
}
#undef VKD
