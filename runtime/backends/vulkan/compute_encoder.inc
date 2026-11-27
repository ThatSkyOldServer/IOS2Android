/* Compute descriptors and recording state are reused only after GPU completion. */
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalComputePool {
    const struct ShaderSpec *spec;
    struct MetalDescriptorLayouts *layouts;
    VkDescriptorPool pool; VkDescriptorSet sets[32]; unsigned used,allocated;
    struct MetalComputePool *next;
};
struct MetalComputeState {
    struct MetalCompute *pipeline;
    struct MetalBoundBuffer buffers[31];
    struct MetalTexture *textures[128];
    struct MetalSampler *samplers[16];
    struct MetalComputePool *pools;
    VkDescriptorSet descriptors; VkPipeline bound_pipeline;
    struct MetalCompute *variant_owner;uint32_t variant_size[3];VkPipeline variant_pipeline;
    unsigned dirty,debug_depth;
    struct MetalComputeState *next;
};
struct MetalComputeEncoder {
    struct MetalResource object;
    struct MetalCommand *command;
    struct MetalComputeState *state;
    unsigned ended;
    struct MetalComputeEncoder *next;
    struct MetalLifetime lifetime;
};
struct MetalSize { Word width,height,depth; };
_Static_assert(sizeof(struct MetalSize)==24,"Metal threadgroup size ABI");
static pthread_mutex_t metal_compute_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalComputePool *metal_compute_pool_cache;
static struct MetalComputeState *metal_compute_state_cache;
static unsigned metal_compute_pool_count,metal_compute_state_count;
static struct MetalComputePool *metal_compute_pool_acquire(struct MetalCompute *pipeline) {
    const struct ShaderSpec *spec=pipeline->function->spec;
    pthread_mutex_lock(&metal_compute_cache_mutex);
    struct MetalComputePool **slot=&metal_compute_pool_cache;
    while (*slot && (*slot)->spec!=spec) slot=&(*slot)->next;
    struct MetalComputePool *pool=*slot;
    if (pool) { *slot=pool->next;--metal_compute_pool_count; }
    pthread_mutex_unlock(&metal_compute_cache_mutex);
    if (pool) { pool->next=NULL;pool->used=0;return pool; }
    pool=calloc(1,sizeof(*pool));if (!pool) return NULL;
    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_SAMPLER,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    unsigned counts[COUNT(types)]={0};
    for (unsigned i=0;i<spec->binding_count;++i) {
        const struct ShaderBindingSpec *binding=&shader_bindings[spec->first_binding+i];
        for (unsigned j=0;j<COUNT(types);++j) if (binding->type==types[j]) ++counts[j];
    }
    VkDescriptorPoolSize sizes[COUNT(types)];unsigned count=0;
    for (unsigned i=0;i<COUNT(types);++i) if (counts[i]) sizes[count++]=(VkDescriptorPoolSize){types[i],32*counts[i]};
    VkDescriptorPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=32,.poolSizeCount=count,.pPoolSizes=sizes};
    if (VKD(vkCreateDescriptorPool)(vulkan.device,&info,NULL,&pool->pool)!=VK_SUCCESS) { free(pool);return NULL; }
    pool->spec=spec;pool->layouts=pipeline->descriptor_layouts;
    __atomic_add_fetch(&pool->layouts->references,1,__ATOMIC_RELAXED);metal_memory_add(3,sizeof(*pool));
    return pool;
}
static void metal_compute_pool_return(struct MetalComputePool *pool) {
    pthread_mutex_lock(&metal_compute_cache_mutex);
    if (metal_compute_pool_count<128) {
        pool->next=metal_compute_pool_cache;metal_compute_pool_cache=pool;++metal_compute_pool_count;
        pthread_mutex_unlock(&metal_compute_cache_mutex);return;
    }
    pthread_mutex_unlock(&metal_compute_cache_mutex);
    VKD(vkDestroyDescriptorPool)(vulkan.device,pool->pool,NULL);metal_descriptor_layouts_release(pool->layouts);
    metal_memory_sub(3,sizeof(*pool));free(pool);
}
static void metal_compute_encoder_destroy(Word value) { metal_memory_sub(3,sizeof(struct MetalComputeEncoder));free((void *)value); }
static Word metal_compute_encoder(struct MetalCommand *command,const char *selector) {
    struct MetalComputeEncoder *encoder=calloc(1,sizeof(*encoder));if (!encoder) return 0;
    pthread_mutex_lock(&metal_compute_cache_mutex);
    struct MetalComputeState *state=metal_compute_state_cache;
    if (state) { metal_compute_state_cache=state->next;--metal_compute_state_count; }
    pthread_mutex_unlock(&metal_compute_cache_mutex);
    if (state) memset(state,0,sizeof(*state));
    else { state=calloc(1,sizeof(*state));if (state) metal_memory_add(3,sizeof(*state)); }
    if (!state) { free(encoder);return 0; }
    encoder->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalComputeCommandEncoder"),.device=metal_command_device(&command->object)};
    encoder->state=state;state->dirty=1;
    pthread_mutex_lock(&command->mutex);
    if (metal_command_status(&command->object)>=2 || command->active_encoder) {
        pthread_mutex_unlock(&command->mutex);metal_memory_sub(3,sizeof(*state));free(state);free(encoder);
        stop("Command cannot create a compute encoder",selector);
    }
    encoder->command=(void *)metal_object_retain((Word)command);
    command->active_encoder=(Word)encoder;encoder->next=command->compute_encoders;command->compute_encoders=encoder;
    pthread_mutex_unlock(&command->mutex);
    metal_lifetime_init((Word)encoder,__builtin_offsetof(struct MetalComputeEncoder,lifetime),metal_compute_encoder_destroy);
    metal_memory_add(3,sizeof(*encoder));return metal_retain_autorelease((Word)encoder);
}
static void metal_compute_check(struct MetalComputeEncoder *encoder,const char *selector) {
    if (encoder->ended || encoder->command->active_encoder!=(Word)encoder || metal_command_status(&encoder->command->object)>=2)
        metal_render_stop("Metal compute encoder is not recording",selector);
}
static void metal_compute_set_pipeline(struct MetalComputeEncoder *encoder,const char *selector,struct MetalCompute *pipeline) {
    metal_compute_check(encoder,selector);
    if (!pipeline || pipeline->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalComputePipelineState")) metal_render_stop("Invalid compute pipeline",selector);
    if (encoder->state->pipeline!=pipeline) {
        metal_command_use(encoder->command,(Word)pipeline);encoder->state->pipeline=pipeline;encoder->state->dirty=1;
    }
}
static void metal_compute_set_buffer(struct MetalComputeEncoder *encoder,const char *selector,struct MetalBuffer *buffer,size_t offset,Word index) {
    metal_compute_check(encoder,selector);
    if (index>=31 || (buffer && (buffer->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalBuffer") || offset>buffer->length)))
        metal_render_stop("Invalid compute buffer binding",selector);
    struct MetalBoundBuffer *bound=&encoder->state->buffers[index];
    if (bound->buffer!=buffer || bound->offset!=offset) {
        if (bound->buffer!=buffer) metal_command_use(encoder->command,(Word)buffer);
        *bound=(struct MetalBoundBuffer){buffer,offset};encoder->state->dirty=1;
    }
}
static void metal_compute_set_offset(struct MetalComputeEncoder *encoder,const char *selector,size_t offset,Word index) {
    metal_compute_check(encoder,selector);
    if (index>=31 || !encoder->state->buffers[index].buffer) metal_render_stop("Missing compute buffer for offset",selector);
    metal_compute_set_buffer(encoder,selector,encoder->state->buffers[index].buffer,offset,index);
}
static void metal_compute_set_texture(struct MetalComputeEncoder *encoder,const char *selector,struct MetalTexture *texture,Word index) {
    metal_compute_check(encoder,selector);
    if (index>=128 || (texture && texture->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalTexture"))) metal_render_stop("Invalid compute texture binding",selector);
    if (encoder->state->textures[index]!=texture) {
        metal_command_use(encoder->command,(Word)texture);encoder->state->textures[index]=texture;encoder->state->dirty=1;
    }
}
static void metal_compute_set_sampler(struct MetalComputeEncoder *encoder,const char *selector,struct MetalSampler *sampler,Word index) {
    metal_compute_check(encoder,selector);
    if (index>=16 || (sampler && sampler->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalSamplerState"))) metal_render_stop("Invalid compute sampler binding",selector);
    if (encoder->state->samplers[index]!=sampler) {
        metal_command_use(encoder->command,(Word)sampler);encoder->state->samplers[index]=sampler;encoder->state->dirty=1;
    }
}
static void metal_compute_set_bytes(struct MetalComputeEncoder *encoder,const char *selector,const void *bytes,size_t length,Word index) {
    metal_compute_check(encoder,selector);
    if (!bytes || !length || length>4096 || index>=31) metal_render_stop("Invalid inline compute bytes",selector);
    struct MetalBuffer *buffer=(void *)metal_buffer_bytes(encoder->object.device,selector,bytes,length,0);
    if (!buffer) metal_render_stop("Cannot allocate inline compute bytes",selector);
    /* This buffer is owned by the bridge even for unretained guest commands. */
    metal_command_hold(encoder->command,(Word)buffer);metal_compute_set_buffer(encoder,selector,buffer,0,index);metal_object_release((Word)buffer);
}
static void metal_compute_transition(struct MetalCommand *command,struct MetalTexture *texture) {
    struct MetalCommandTexture *use=metal_command_texture(command,texture);
    if (!use->layout_used) {
        use->layout_used=1;use->initial=use->layout=VK_IMAGE_LAYOUT_GENERAL;
        use->initial_access=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;use->initial_stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;return;
    }
    if (use->layout==VK_IMAGE_LAYOUT_GENERAL) return;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,.oldLayout=use->layout,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=texture->image,
        .subresourceRange={texture->format.aspects,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
    VKD(vkCmdPipelineBarrier)(command->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
    use->layout=VK_IMAGE_LAYOUT_GENERAL;use->layout_changed=1;use->read_stages=0;
}
static void metal_compute_descriptors(struct MetalComputeEncoder *encoder) {
    struct MetalComputeState *state=encoder->state;struct MetalCompute *pipeline=state->pipeline;
    /* No other encoder can run before endEncoding. Unchanged bindings retain
     * their descriptors and GENERAL image layouts throughout this encoder. */
    if (!state->dirty && state->descriptors) return;
    const struct ShaderSpec *spec=pipeline->function->spec;
    VkWriteDescriptorSet writes[128];VkDescriptorBufferInfo buffers[128];VkDescriptorImageInfo images[128];unsigned count=0;
    for (unsigned i=0;i<spec->binding_count;++i) {
        const struct ShaderBindingSpec *binding=&shader_bindings[spec->first_binding+i];
        if (binding->literal_sampler) continue;
        if (count>=COUNT(writes)) metal_render_stop("Compute binding count exceeds backend capacity",spec->name);
        VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=binding->binding,
            .descriptorCount=1,.descriptorType=(VkDescriptorType)binding->type};
        if (binding->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || binding->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            struct MetalBoundBuffer use={0};
            if (binding->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && binding->metal_index==UINT32_MAX-1) use.buffer=metal_math_buffer(encoder->command);
            else if (binding->metal_index<31) use=state->buffers[binding->metal_index];
            if (!use.buffer || use.offset>=use.buffer->length) metal_render_stop("Missing compute buffer",spec->name);
            VkDeviceSize range=binding->size ? binding->size : use.buffer->length-use.offset;
            VkDeviceSize alignment=binding->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? vulkan.properties.limits.minUniformBufferOffsetAlignment : vulkan.properties.limits.minStorageBufferOffsetAlignment;
            VkDeviceSize maximum=binding->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? vulkan.properties.limits.maxUniformBufferRange : vulkan.properties.limits.maxStorageBufferRange;
            if (use.offset%alignment || binding->metal_size>use.buffer->length-use.offset || range>use.buffer->native_length-use.offset || range>maximum)
                metal_render_stop("Undersized or unaligned compute buffer",spec->name);
            buffers[count]=(VkDescriptorBufferInfo){use.buffer->buffer,use.offset,range};write.pBufferInfo=&buffers[count];
        } else if (binding->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || binding->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            struct MetalTexture *texture=binding->metal_index<128 ? state->textures[binding->metal_index] : NULL;
            unsigned storage=binding->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (!texture || !(texture->native_usage&(storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT))) metal_render_stop("Missing or incompatible compute texture",spec->name);
            metal_compute_transition(encoder->command,texture);
            images[count]=(VkDescriptorImageInfo){VK_NULL_HANDLE,storage ? texture->storage_view : texture->sampled_view,VK_IMAGE_LAYOUT_GENERAL};write.pImageInfo=&images[count];
        } else if (binding->type==VK_DESCRIPTOR_TYPE_SAMPLER) {
            struct MetalSampler *sampler=binding->metal_index<16 ? state->samplers[binding->metal_index] : NULL;
            if (!sampler) metal_render_stop("Missing compute sampler",spec->name);
            images[count]=(VkDescriptorImageInfo){.sampler=sampler->sampler};write.pImageInfo=&images[count];
        } else metal_render_stop("Unsupported compute descriptor type",spec->name);
        writes[count++]=write;
    }
    if (state->dirty || !state->descriptors) {
        struct MetalComputePool *pool=state->pools;
        if (!pool || pool->used==COUNT(pool->sets) || pool->spec!=spec) {
            pool=metal_compute_pool_acquire(pipeline);if (!pool) metal_render_stop("Cannot allocate compute descriptor pool",spec->name);
            pool->next=state->pools;state->pools=pool;
        }
        if (pool->used==pool->allocated) {
            VkDescriptorSetAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool->pool,
                .descriptorSetCount=1,.pSetLayouts=&pool->layouts->sets[2]};
            metal_render_result(VKD(vkAllocateDescriptorSets)(vulkan.device,&allocate,&pool->sets[pool->allocated]),"vkAllocateDescriptorSets compute");++pool->allocated;
        }
        state->descriptors=pool->sets[pool->used++];
        for (unsigned i=0;i<count;++i) writes[i].dstSet=state->descriptors;
        if (count) VKD(vkUpdateDescriptorSets)(vulkan.device,count,writes,0,NULL);
        VKD(vkCmdBindDescriptorSets)(encoder->command->command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->layout,2,1,&state->descriptors,0,NULL);
        state->dirty=0;
    }
}
static void metal_compute_dispatch(struct MetalComputeEncoder *encoder,const char *selector,struct MetalSize groups,struct MetalSize threads) {
    metal_compute_check(encoder,selector);
    if (!encoder->state->pipeline) metal_render_stop("Missing compute pipeline",selector);
    const Word counts[]={groups.width,groups.height,groups.depth},sizes[]={threads.width,threads.height,threads.depth};
    uint32_t size[3];
    for (unsigned i=0;i<3;++i) {
        if (counts[i]>vulkan.properties.limits.maxComputeWorkGroupCount[i] || !sizes[i] || sizes[i]>UINT32_MAX) metal_render_stop("Compute dispatch exceeds device limits",selector);
        size[i]=(uint32_t)sizes[i];
    }
    if (!groups.width || !groups.height || !groups.depth) return;
    VkPipeline pipeline;
    if (encoder->state->variant_owner==encoder->state->pipeline && !memcmp(encoder->state->variant_size,size,sizeof(size)))
        pipeline=encoder->state->variant_pipeline;
    else {
        metal_render_result(metal_compute_variant(encoder->state->pipeline,size,&pipeline),"compute workgroup pipeline");
        encoder->state->variant_owner=encoder->state->pipeline;memcpy(encoder->state->variant_size,size,sizeof(size));encoder->state->variant_pipeline=pipeline;
    }
    metal_render_lock(encoder->command);
    metal_compute_descriptors(encoder);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
    if (encoder->state->bound_pipeline!=pipeline) {
        VKD(vkCmdBindPipeline)(encoder->command->command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);encoder->state->bound_pipeline=pipeline;
    }
    VKD(vkCmdDispatch)(encoder->command->command,(uint32_t)groups.width,(uint32_t)groups.height,(uint32_t)groups.depth);
    metal_render_unlock(encoder->command);++encoder->command->recorded_operations;
}
static void metal_compute_end(struct MetalComputeEncoder *encoder,const char *selector) {
    metal_compute_check(encoder,selector);metal_render_lock(encoder->command);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT};
    VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    metal_render_unlock(encoder->command);encoder->ended=1;encoder->command->active_encoder=0;metal_object_release((Word)encoder->command);
}
static void metal_compute_cleanup(struct MetalCommand *command) {
    for (struct MetalComputeEncoder *encoder=command->compute_encoders;encoder;encoder=encoder->next) {
        struct MetalComputeState *state=encoder->state;if (!state) continue;
        while (state->pools) { struct MetalComputePool *pool=state->pools;state->pools=pool->next;metal_compute_pool_return(pool); }
        encoder->state=NULL;
        pthread_mutex_lock(&metal_compute_cache_mutex);
        if (metal_compute_state_count<128) {
            state->next=metal_compute_state_cache;metal_compute_state_cache=state;++metal_compute_state_count;state=NULL;
        }
        pthread_mutex_unlock(&metal_compute_cache_mutex);
        if (state) { metal_memory_sub(3,sizeof(*state));free(state); }
    }
}
static void metal_compute_release_encoders(struct MetalCommand *command) {
    while (command->compute_encoders) {
        struct MetalComputeEncoder *encoder=command->compute_encoders;command->compute_encoders=encoder->next;metal_object_release((Word)encoder);
    }
}
static void metal_compute_debug_push(struct MetalComputeEncoder *encoder,const char *selector,Word name) { metal_compute_check(encoder,selector);if (!name) metal_render_stop("Nil compute debug group",selector);++encoder->state->debug_depth; }
static void metal_compute_debug_pop(struct MetalComputeEncoder *encoder,const char *selector) { metal_compute_check(encoder,selector);if (!encoder->state->debug_depth) metal_render_stop("Unbalanced compute debug group",selector);--encoder->state->debug_depth; }
static Method metal_compute_encoder_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalCommandBuffer") && !strcmp(selector,"computeCommandEncoder")) return (Method)metal_compute_encoder;
    if (strcmp(receiver,"_OBJC_CLASS_$_I2AMetalComputeCommandEncoder")) return NULL;
    if (!strcmp(selector,"device")) return (Method)metal_resource_device;
    if (!strcmp(selector,"label")) return (Method)metal_resource_label;
    if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
    if (!strcmp(selector,"setComputePipelineState:")) return (Method)metal_compute_set_pipeline;
    if (!strcmp(selector,"setBuffer:offset:atIndex:")) return (Method)metal_compute_set_buffer;
    if (!strcmp(selector,"setBufferOffset:atIndex:")) return (Method)metal_compute_set_offset;
    if (!strcmp(selector,"setBytes:length:atIndex:")) return (Method)metal_compute_set_bytes;
    if (!strcmp(selector,"setTexture:atIndex:")) return (Method)metal_compute_set_texture;
    if (!strcmp(selector,"setSamplerState:atIndex:")) return (Method)metal_compute_set_sampler;
    if (!strcmp(selector,"dispatchThreadgroups:threadsPerThreadgroup:")) return (Method)metal_compute_dispatch;
    if (!strcmp(selector,"endEncoding")) return (Method)metal_compute_end;
    if (!strcmp(selector,"pushDebugGroup:")) return (Method)metal_compute_debug_push;
    if (!strcmp(selector,"popDebugGroup")) return (Method)metal_compute_debug_pop;
    if (!strcmp(selector,"insertDebugSignpost:")) return (Method)noop;
    return NULL;
}
#undef VKD
