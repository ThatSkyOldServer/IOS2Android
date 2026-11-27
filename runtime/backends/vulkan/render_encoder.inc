 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
static _Thread_local struct MetalCommandPool *metal_render_locked_queue;
static void metal_render_stop(const char *reason,const char *detail) {
    if (metal_render_locked_queue) {
        pthread_mutex_unlock(&metal_render_locked_queue->mutex); metal_render_locked_queue=NULL;
    }
    stop(reason,detail);
}
static void metal_render_lock(struct MetalCommand *command) { pthread_mutex_lock(&command->pool_owner->mutex); metal_render_locked_queue=command->pool_owner; }
static void metal_render_unlock(struct MetalCommand *command) { metal_render_locked_queue=NULL; pthread_mutex_unlock(&command->pool_owner->mutex); }
struct MetalBoundBuffer { struct MetalBuffer *buffer; VkDeviceSize offset; };
struct MetalDescriptorPoolChunk {
    VkDescriptorPool pool; VkDescriptorSet allocated_sets[64];
    struct MetalDescriptorSnapshot *snapshots[32];
    struct MetalDescriptorLayouts *layouts;
    const struct ShaderSpec *vertex,*fragment;
    unsigned sets,allocated,limit,dynamic_uniform,dynamic_storage;
    struct MetalDescriptorPoolChunk *next;
};
struct MetalDescriptorValue { uint64_t resource,offset,range; };
struct MetalDescriptorSnapshot {
    struct MetalPipeline *pipeline;
    VkDescriptorSet sets[2];
    uint64_t hash;
    unsigned count;
    struct MetalDescriptorSnapshot *next;
    struct MetalDescriptorValue values[];
};
struct MetalRenderSnapshot { struct MetalTexture *source,*texture; struct MetalRenderEncoder *encoder; unsigned generation; struct MetalRenderSnapshot *next; };
static struct MetalRenderSnapshot *metal_snapshot_cache;
static size_t metal_snapshot_cache_bytes;
static unsigned metal_snapshot_cache_count;
static pthread_mutex_t metal_snapshot_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalDescriptorPoolChunk *metal_descriptor_pool_cache;
static unsigned metal_descriptor_pool_cache_count;
static pthread_mutex_t metal_descriptor_pool_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalRenderSnapshot *metal_snapshot_cached(struct MetalTexture *source) {
    pthread_mutex_lock(&metal_snapshot_cache_mutex);
    struct MetalRenderSnapshot **slot=&metal_snapshot_cache;
    while (*slot) {
        struct MetalTexture *texture=(*slot)->texture;
        if (texture->format.metal==source->format.metal && texture->type==source->type && texture->width==source->width && texture->height==source->height &&
            texture->depth==source->depth && texture->mips==source->mips && texture->samples==source->samples && texture->array_length==source->array_length) {
            struct MetalRenderSnapshot *entry=*slot; *slot=entry->next;
            --metal_snapshot_cache_count; metal_snapshot_cache_bytes-=texture->allocation;
            pthread_mutex_unlock(&metal_snapshot_cache_mutex);entry->next=NULL;return entry;
        }
        slot=&(*slot)->next;
    }
    pthread_mutex_unlock(&metal_snapshot_cache_mutex); return NULL;
}
static void metal_snapshot_return(struct MetalRenderSnapshot *entry) {
    struct MetalTexture *texture=entry->texture;
    pthread_mutex_lock(&metal_snapshot_cache_mutex);
    if (metal_snapshot_cache_count>=32 || texture->allocation>64*1024*1024-metal_snapshot_cache_bytes) {
        pthread_mutex_unlock(&metal_snapshot_cache_mutex); free(entry); metal_object_release((Word)texture); return;
    }
    entry->source=NULL;entry->encoder=NULL;entry->next=metal_snapshot_cache;metal_snapshot_cache=entry;
    ++metal_snapshot_cache_count; metal_snapshot_cache_bytes+=texture->allocation;
    pthread_mutex_unlock(&metal_snapshot_cache_mutex);
}
static size_t metal_snapshot_trim(void) {
    pthread_mutex_lock(&metal_snapshot_cache_mutex);
    struct MetalRenderSnapshot *entries=metal_snapshot_cache;
    size_t bytes=metal_snapshot_cache_bytes;
    metal_snapshot_cache=NULL;metal_snapshot_cache_bytes=0;metal_snapshot_cache_count=0;
    pthread_mutex_unlock(&metal_snapshot_cache_mutex);
    while(entries) {
        struct MetalRenderSnapshot *next=entries->next;
        metal_object_release((Word)entries->texture);free(entries);entries=next;
    }
    return bytes;
}
static int metal_descriptor_pool_matches(const struct MetalDescriptorPoolChunk *chunk,const struct MetalPipeline *pipeline) {
    /* Shader metadata is immutable and describes every binding and immutable
     * sampler. Equal metadata and dynamic types yield compatible set layouts. */
    return chunk->vertex==pipeline->vertex->spec && chunk->fragment==(pipeline->fragment ? pipeline->fragment->spec : NULL) &&
        chunk->dynamic_uniform==pipeline->dynamic_uniform && chunk->dynamic_storage==pipeline->dynamic_storage;
}
static struct MetalDescriptorPoolChunk *metal_descriptor_pool_acquire(const struct MetalPipeline *pipeline) {
    pthread_mutex_lock(&metal_descriptor_pool_cache_mutex);
    struct MetalDescriptorPoolChunk **slot=&metal_descriptor_pool_cache;
    while (*slot) {
        if (metal_descriptor_pool_matches(*slot,pipeline)) {
            struct MetalDescriptorPoolChunk *chunk=*slot; *slot=chunk->next; --metal_descriptor_pool_cache_count;
            pthread_mutex_unlock(&metal_descriptor_pool_cache_mutex); chunk->next=NULL; chunk->sets=0; return chunk;
        }
        slot=&(*slot)->next;
    }
    pthread_mutex_unlock(&metal_descriptor_pool_cache_mutex);
    struct MetalDescriptorPoolChunk *chunk=calloc(1,sizeof(*chunk)); if (!chunk) return NULL;
    chunk->vertex=pipeline->vertex->spec;chunk->fragment=pipeline->fragment ? pipeline->fragment->spec : NULL;
    chunk->dynamic_uniform=pipeline->dynamic_uniform;chunk->dynamic_storage=pipeline->dynamic_storage;chunk->limit=64;
    const unsigned *counts=pipeline->descriptor_counts;
    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_SAMPLER,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorPoolSize sizes[7]; unsigned used=0;
    for (unsigned i=0;i<COUNT(types);++i) if (counts[i]) sizes[used++]=(VkDescriptorPoolSize){types[i],32*counts[i]};
    VkDescriptorPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=64,.poolSizeCount=used,.pPoolSizes=sizes};
    if (VKD(vkCreateDescriptorPool)(vulkan.device,&info,NULL,&chunk->pool)!=VK_SUCCESS) { free(chunk); return NULL; }
    /* Cached sets can outlive their original pipeline. Keep their allocating
     * layouts alive, including when a compatible pipeline borrows the pool. */
    chunk->layouts=pipeline->descriptor_layouts;__atomic_add_fetch(&chunk->layouts->references,1,__ATOMIC_RELAXED);
    metal_memory_add(3,sizeof(*chunk));
    return chunk;
}
static void metal_descriptor_pool_return(struct MetalDescriptorPoolChunk *chunk) {
    /* The command fence has passed. Reuse allocated sets and snapshot storage;
     * rewrite active bindings before the next recording uses them. */
    pthread_mutex_lock(&metal_descriptor_pool_cache_mutex);
    if (metal_descriptor_pool_cache_count>=128) {
        pthread_mutex_unlock(&metal_descriptor_pool_cache_mutex); VKD(vkDestroyDescriptorPool)(vulkan.device,chunk->pool,NULL);
        metal_descriptor_layouts_release(chunk->layouts);
        for(unsigned i=0;i<COUNT(chunk->snapshots);++i)if(chunk->snapshots[i]) {
            struct MetalDescriptorSnapshot *entry=chunk->snapshots[i];
            metal_memory_sub(3,sizeof(*entry)+entry->count*sizeof(*entry->values));free(entry);
        }
        metal_memory_sub(3,sizeof(*chunk));free(chunk); return;
    }
    chunk->sets=0; chunk->next=metal_descriptor_pool_cache; metal_descriptor_pool_cache=chunk; ++metal_descriptor_pool_cache_count;
    pthread_mutex_unlock(&metal_descriptor_pool_cache_mutex);
}
struct MetalPassBundleKey {
    VkImageViewCreateInfo views[9]; VkAttachmentDescription descriptions[9]; VkAttachmentReference colors[8],depth;
    VkAttachmentStoreOp final_store[9],final_stencil_store[9];
    uint32_t color_count,attachment_count,width,height; VkFormat depth_format;
};
struct MetalPassBundle { struct MetalPassBundleKey key; VkImageView views[9]; VkRenderPass first_pass,load_pass,final_first_pass,final_load_pass; VkFramebuffer framebuffer; struct MetalPassBundle *next; };
static pthread_mutex_t metal_pass_bundle_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalPassBundle *metal_pass_bundles;
static unsigned metal_pass_bundle_count;
static void metal_resolve_cache_invalidate(struct MetalTexture *);
static void metal_pass_bundle_destroy(struct MetalPassBundle *entry) {
    if (entry->framebuffer) VKD(vkDestroyFramebuffer)(vulkan.device,entry->framebuffer,NULL);
    if (entry->first_pass) VKD(vkDestroyRenderPass)(vulkan.device,entry->first_pass,NULL);
    if (entry->load_pass) VKD(vkDestroyRenderPass)(vulkan.device,entry->load_pass,NULL);
    if (entry->final_first_pass) VKD(vkDestroyRenderPass)(vulkan.device,entry->final_first_pass,NULL);
    if (entry->final_load_pass) VKD(vkDestroyRenderPass)(vulkan.device,entry->final_load_pass,NULL);
    for (unsigned i=0;i<entry->key.attachment_count;++i) if (entry->views[i]) VKD(vkDestroyImageView)(vulkan.device,entry->views[i],NULL);
    free(entry);
}
static void metal_pass_bundle_return(struct MetalPassBundle *entry) {
    pthread_mutex_lock(&metal_pass_bundle_mutex);
    if (metal_pass_bundle_count>=256) { pthread_mutex_unlock(&metal_pass_bundle_mutex); metal_pass_bundle_destroy(entry); return; }
    entry->next=metal_pass_bundles; metal_pass_bundles=entry; ++metal_pass_bundle_count;
    pthread_mutex_unlock(&metal_pass_bundle_mutex);
}
static void metal_render_cache_invalidate(struct MetalTexture *texture) {
    /* Free-list entries have passed their consumer fence. Retire views and
     * framebuffers before the backing image itself is destroyed. */
    metal_resolve_cache_invalidate(texture);
    pthread_mutex_lock(&metal_pass_bundle_mutex);
    struct MetalPassBundle **slot=&metal_pass_bundles;
    while (*slot) {
        struct MetalPassBundle *entry=*slot; unsigned found=0;
        for (unsigned i=0;i<entry->key.attachment_count;++i) if (entry->key.views[i].image==texture->image) found=1;
        if (found) { *slot=entry->next; --metal_pass_bundle_count; metal_pass_bundle_destroy(entry); }
        else slot=&entry->next;
    }
    pthread_mutex_unlock(&metal_pass_bundle_mutex);
}
enum MetalRenderDynamic { METAL_DYNAMIC_VIEWPORT=1, METAL_DYNAMIC_SCISSOR=2,
    METAL_DYNAMIC_BLEND=4, METAL_DYNAMIC_BIAS=8, METAL_DYNAMIC_ALL=15 };
/* A full initial snapshot may be reused only for unmodified components within
 * this encoder. Dirty bounds are the union of the actual writable scissors. */
struct MetalColorHistory { struct MetalTexture *source; unsigned required,dirty; VkRect2D bounds; };
struct MetalRenderState {
    struct MetalColorHistory history;
    struct MetalPipeline *pipeline;
    struct MetalPipelineKey key;
    struct MetalBoundBuffer buffers[2][31];
    struct MetalTexture *textures[2][128],*attachments[9];
    struct MetalSampler *samplers[2][16];
    VkImageView views[9]; VkRenderPass first_pass,load_pass; VkFramebuffer framebuffer;
    VkImageViewCreateInfo view_descriptions[9]; struct MetalPassBundle *pass_bundle;
    VkAttachmentDescription descriptions[9]; VkAttachmentReference colors[8],depth;
    VkAttachmentStoreOp final_store[9],final_stencil_store[9];
    VkClearValue clear[9]; VkViewport viewport; VkRect2D scissor;
    uint32_t color_count,attachment_count,width,height;
    VkFormat color_formats[8],depth_format;
    VkSampleCountFlagBits samples;
    struct MetalRenderResolve *resolves;
    unsigned begun,started,debug_depth,draws;
    unsigned deferred_store,segment_load;
    VkCommandBuffer render_command;
#if I2A_DEVELOPMENT
    unsigned diagnostic_pass,pending_snapshot_regions;
    uint64_t pending_snapshot_color_bytes;
#endif
    float blend[4],bias,slope,clamp;
    uint32_t front_reference,back_reference;
    struct MetalDescriptorPoolChunk *pools;
    struct MetalDescriptorSnapshot *descriptors[32];
    unsigned descriptor_count;
    VkPipeline bound_pipeline;
    struct MetalPipeline *variant_owner;
    struct MetalPipelineKey variant_key;
    VkPipeline variant_pipeline;
    struct MetalBoundBuffer bound_vertices[31];
    VkBuffer bound_index;
    VkDeviceSize bound_index_offset;
    Word bound_index_type;
    unsigned dynamic_set,dynamic_dirty,viewport_empty,stencil_dirty,pending_storage;
    VkPipelineLayout bound_descriptor_layout;
    VkDescriptorSet bound_descriptor_sets[2];
    uint32_t bound_dynamic_offsets[128];
    unsigned bound_dynamic_count;
    struct MetalRenderState *next_free;
};
struct MetalRenderEncoder {
    struct MetalResource object;
    struct MetalCommand *command;
    struct MetalRenderState *state;
    unsigned ended;
    struct MetalRenderEncoder *next;
    struct MetalLifetime lifetime;
};
static void metal_render_destroy(Word value) {metal_memory_sub(3,sizeof(struct MetalRenderEncoder));free((void *)value);}
/* The ObjC identity can outlive endEncoding and completion. Only the large
 * recording state belongs to the GPU submission; recycle it after its fence
 * and completion handlers. Keep identity/device/label usable for the client. */
static pthread_mutex_t metal_render_state_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalRenderState *metal_render_state_free;
static unsigned metal_render_state_count;
static struct MetalRenderState *metal_render_state_acquire(void) {
    pthread_mutex_lock(&metal_render_state_mutex);
    struct MetalRenderState *state=metal_render_state_free;
    if (state) { metal_render_state_free=state->next_free; --metal_render_state_count; }
    pthread_mutex_unlock(&metal_render_state_mutex);
    if (state) memset(state,0,sizeof(*state));
    else { state=calloc(1,sizeof(*state)); if (state) metal_memory_add(3,sizeof(*state)); }
    return state;
}
static void metal_render_state_return(struct MetalRenderState *state) {
    pthread_mutex_lock(&metal_render_state_mutex);
    if (metal_render_state_count<128) {
        state->next_free=metal_render_state_free; metal_render_state_free=state; ++metal_render_state_count;
        pthread_mutex_unlock(&metal_render_state_mutex);
    } else { pthread_mutex_unlock(&metal_render_state_mutex); metal_memory_sub(3,sizeof(*state)); free(state); }
}
static void metal_render_check(struct MetalRenderEncoder *encoder,const char *selector) {
    if (encoder->ended || encoder->command->active_encoder!=(Word)encoder || metal_command_status(&encoder->command->object)>=2)
        metal_render_stop("Metal render encoder is not recording",selector);
}
static void metal_render_result(VkResult result,const char *operation) {
    if (result!=VK_SUCCESS) { char detail[160]; snprintf(detail,sizeof(detail),"%s result=%d",operation,result); metal_render_stop("Vulkan render operation failed",detail); }
}
static void metal_render_pass_bundle(struct MetalRenderEncoder *encoder) {
    uint64_t diagnostic=render_diagnostic_clock(); struct MetalPassBundleKey key={0};
    memcpy(key.views,encoder->state->view_descriptions,sizeof(key.views)); memcpy(key.descriptions,encoder->state->descriptions,sizeof(key.descriptions));
    memcpy(key.final_store,encoder->state->final_store,sizeof(key.final_store));memcpy(key.final_stencil_store,encoder->state->final_stencil_store,sizeof(key.final_stencil_store));
    memcpy(key.colors,encoder->state->colors,sizeof(key.colors)); key.depth=encoder->state->depth;
    key.color_count=encoder->state->color_count; key.attachment_count=encoder->state->attachment_count; key.width=encoder->state->width; key.height=encoder->state->height; key.depth_format=encoder->state->depth_format;
    pthread_mutex_lock(&metal_pass_bundle_mutex); struct MetalPassBundle **slot=&metal_pass_bundles,*entry=NULL;
    while (*slot) { if (!memcmp(&(*slot)->key,&key,sizeof(key))) { entry=*slot; *slot=entry->next; --metal_pass_bundle_count; break; } slot=&(*slot)->next; }
    pthread_mutex_unlock(&metal_pass_bundle_mutex); unsigned reused=entry!=NULL;
    if (!entry) {
        entry=calloc(1,sizeof(*entry)); if (!entry) metal_render_stop("Allocation failed","Render pass bundle"); entry->key=key;
        for (unsigned i=0;i<key.attachment_count;++i) metal_render_result(VKD(vkCreateImageView)(vulkan.device,&key.views[i],NULL,&entry->views[i]),"vkCreateImageView attachment");
        VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=key.color_count,.pColorAttachments=key.colors,.pDepthStencilAttachment=key.depth_format ? &key.depth : NULL};
        VkRenderPassCreateInfo info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=key.attachment_count,.pAttachments=key.descriptions,.subpassCount=1,.pSubpasses=&subpass,
            .dependencyCount=1,.pDependencies=&metal_attachment_dependency};
        metal_render_result(VKD(vkCreateRenderPass)(vulkan.device,&info,NULL,&entry->first_pass),"vkCreateRenderPass first");
        for (unsigned i=0;i<key.attachment_count;++i) { key.descriptions[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; if (encoder->state->attachments[i]->format.aspects&VK_IMAGE_ASPECT_STENCIL_BIT) key.descriptions[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD; }
        metal_render_result(VKD(vkCreateRenderPass)(vulkan.device,&info,NULL,&entry->load_pass),"vkCreateRenderPass continuation");
        if (encoder->state->deferred_store) {
            for (unsigned i=0;i<key.attachment_count;++i) {
                key.descriptions[i].storeOp=key.final_store[i];key.descriptions[i].stencilStoreOp=key.final_stencil_store[i];
            }
            metal_render_result(VKD(vkCreateRenderPass)(vulkan.device,&info,NULL,&entry->final_load_pass),"vkCreateRenderPass final continuation");
            for (unsigned i=0;i<key.attachment_count;++i) {
                key.descriptions[i].loadOp=entry->key.descriptions[i].loadOp;
                key.descriptions[i].stencilLoadOp=entry->key.descriptions[i].stencilLoadOp;
            }
            metal_render_result(VKD(vkCreateRenderPass)(vulkan.device,&info,NULL,&entry->final_first_pass),"vkCreateRenderPass final");
        }
        VkFramebufferCreateInfo framebuffer={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=entry->first_pass,.attachmentCount=key.attachment_count,.pAttachments=entry->views,.width=key.width,.height=key.height,.layers=1};
        metal_render_result(VKD(vkCreateFramebuffer)(vulkan.device,&framebuffer,NULL,&entry->framebuffer),"vkCreateFramebuffer");
    }
    entry->next=NULL; encoder->state->pass_bundle=entry; memcpy(encoder->state->views,entry->views,sizeof(encoder->state->views));
    encoder->state->first_pass=entry->first_pass; encoder->state->load_pass=entry->load_pass; encoder->state->framebuffer=entry->framebuffer;
    render_diagnostic_cost(10,diagnostic);
    static unsigned hits,misses;
    if (render_diagnostics_requested) {
        if (reused) __atomic_add_fetch(&hits,1,__ATOMIC_RELAXED); else __atomic_add_fetch(&misses,1,__ATOMIC_RELAXED);
        unsigned total=__atomic_load_n(&hits,__ATOMIC_RELAXED)+__atomic_load_n(&misses,__ATOMIC_RELAXED);
        if (total%1024==0) LOG("[DIAG-render] Render pass bundle cache: hits=%u misses=%u",__atomic_load_n(&hits,__ATOMIC_RELAXED),__atomic_load_n(&misses,__ATOMIC_RELAXED));
    }
}
#include "render_pass.inc"
static void metal_render_transition_stages(struct MetalRenderEncoder *encoder,struct MetalTexture *texture,VkImageLayout layout,VkAccessFlags access,VkPipelineStageFlags stages) {
    /* GENERAL can be shared by sampled and storage bindings. Only the
     * read-only layout tracks visibility separately for each reader stage. */
    if (layout==VK_IMAGE_LAYOUT_GENERAL) stages=metal_image_stages(layout);
    struct MetalCommandTexture *use=metal_command_texture(encoder->command,texture);
    if (!use->layout_used) {
        use->layout_used=1;
         
        use->initial=use->layout=layout;use->initial_access=access;use->initial_stages=stages;
        use->read_stages=layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? stages : 0;
#if I2A_DEVELOPMENT
        if (metal_diagnostic_enabled) __atomic_fetch_add(&metal_layout_counts[0],1,__ATOMIC_RELAXED);
#endif
        return;
    }
    /* Attachment-to-attachment hazards use the render-pass dependency. A
     * repeated resolve/copy destination still needs transfer write ordering. */
    unsigned additional_reader=use->layout==layout && layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && (stages&~use->read_stages);
    if (use->layout==layout && layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && !additional_reader) return;
    /* Submission records the prologue after recording the main buffer. Readers of
     * the initial, unmodified contents can share that first dependency. A later
     * read following a write or layout change still needs the inline barrier. */
    if (additional_reader && !use->layout_changed && use->initial==layout && use->initial_access==VK_ACCESS_SHADER_READ_BIT) {
        use->initial_stages|=stages;use->read_stages|=stages;
        return;
    }
    metal_render_close_pass(encoder);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=additional_reader ? VK_ACCESS_MEMORY_WRITE_BIT : metal_image_access(use->layout),
        .dstAccessMask=access,.oldLayout=use->layout,.newLayout=layout,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=texture->image,.subresourceRange={texture->format.aspects,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
    VKD(vkCmdPipelineBarrier)(encoder->command->command,additional_reader ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : metal_image_stages(use->layout),stages,0,0,NULL,0,NULL,1,&barrier);
    if (use->layout!=layout) use->layout_changed=1;
    use->read_stages=layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? (additional_reader ? use->read_stages|stages : stages) : 0;
    use->layout=layout;
}
static void metal_render_transition(struct MetalRenderEncoder *encoder,struct MetalTexture *texture,VkImageLayout layout,VkAccessFlags access) {
    metal_render_transition_stages(encoder,texture,layout,access,metal_image_stages(layout));
}
static void metal_render_begin(struct MetalRenderEncoder *encoder) {
    if (encoder->state->begun) return;
    for (unsigned i=0;i<encoder->state->attachment_count;++i) {
        struct MetalTexture *texture=encoder->state->attachments[i];
        VkImageLayout layout=texture->format.aspects==VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        metal_render_transition(encoder,texture,layout,texture->format.aspects==VK_IMAGE_ASPECT_COLOR_BIT ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=encoder->state->started ? encoder->state->load_pass : encoder->state->first_pass,
        .framebuffer=encoder->state->framebuffer,.renderArea={{0,0},{encoder->state->width,encoder->state->height}},.clearValueCount=encoder->state->attachment_count,.pClearValues=encoder->state->clear};
    metal_diagnostic_pass_begin(encoder);
    encoder->state->segment_load=encoder->state->started;
    if (encoder->state->deferred_store) metal_render_segment_begin(encoder);
    else {
        encoder->state->render_command=encoder->command->command;
        VKD(vkCmdBeginRenderPass)(encoder->command->command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    }
#if I2A_DEVELOPMENT
    if (metal_diagnostic_enabled) __atomic_fetch_add(&metal_layout_counts[1],1,__ATOMIC_RELAXED);
#endif
    encoder->state->begun=encoder->state->started=1;
}
#include "render_resolve.inc"
static VkClearColorValue metal_render_clear_color(struct MetalTexture *texture,const double values[4]) {
    VkClearColorValue clear={0};unsigned integer=0;
    switch (texture->format.format) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: integer=1;break;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT: integer=2;break;
    default: break;
    }
    for (unsigned i=0;i<4;++i) {
        if (integer && !isfinite(values[i])) metal_render_stop("Nonfinite integer attachment clear","renderCommandEncoderWithDescriptor:");
        if (integer==1) clear.uint32[i]=values[i]<=0 ? 0 : values[i]>=UINT32_MAX ? UINT32_MAX : (uint32_t)values[i];
        else if (integer==2) clear.int32[i]=values[i]<=INT32_MIN ? INT32_MIN : values[i]>=INT32_MAX ? INT32_MAX : (int32_t)values[i];
        else clear.float32[i]=(float)values[i];
    }
    return clear;
}
static unsigned metal_render_attachment(struct MetalRenderEncoder *encoder,struct MetalDescriptor *descriptor,int is_depth) {
    struct MetalTexture *texture=(void *)metal_property(descriptor,"texture");
    if (!texture || texture->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalTexture")) metal_render_stop("Invalid render attachment texture","renderCommandEncoderWithDescriptor:");
    Word level=metal_property(descriptor,"level"),slice=metal_property(descriptor,"slice"),plane=metal_property(descriptor,"depthPlane");
    Word load=metal_property(descriptor,"loadAction"),store=metal_property(descriptor,"storeAction");
    if (level>=texture->mips || slice>=texture->layers || plane || texture->type==7 || load>2 || store>3 ||
        !(texture->native_usage&(is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ||
        (is_depth ? !(texture->format.aspects&(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)) : texture->format.aspects!=VK_IMAGE_ASPECT_COLOR_BIT) ||
        (encoder->state->attachment_count && texture->samples!=encoder->state->samples))
        metal_render_stop("Unsupported render attachment subresource, resolve or action","renderCommandEncoderWithDescriptor:");
    uint32_t width=(uint32_t)(texture->width>>level),height=(uint32_t)(texture->height>>level);
    if (!width) width=1; if (!height) height=1;
    if (width<encoder->state->width) encoder->state->width=width; if (height<encoder->state->height) encoder->state->height=height;
    unsigned index=encoder->state->attachment_count++;
    encoder->state->attachments[index]=texture; encoder->state->samples=(VkSampleCountFlagBits)texture->samples;
     
    metal_command_texture(encoder->command,texture);
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=texture->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=texture->format.format,
        .subresourceRange={texture->format.aspects,(uint32_t)level,1,(uint32_t)slice,1}};
    encoder->state->view_descriptions[index]=view;
    const VkAttachmentLoadOp loads[]={VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_LOAD_OP_CLEAR};
    VkImageLayout layout=is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    encoder->state->descriptions[index]=(VkAttachmentDescription){.format=texture->format.format,.samples=encoder->state->samples,
        .loadOp=loads[load],.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=layout,.finalLayout=layout};
    encoder->state->final_store[index]=store==0 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    encoder->state->final_stencil_store[index]=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (is_depth) encoder->state->clear[index].depthStencil=(VkClearDepthStencilValue){(float)descriptor->clear_depth,(uint32_t)metal_property(descriptor,"clearStencil")};
    else encoder->state->clear[index].color=metal_render_clear_color(texture,descriptor->clear_color);
    if (!is_depth) metal_render_resolve_attachment(encoder,descriptor,index,VK_IMAGE_ASPECT_COLOR_BIT);
     
    return index;
}
static Word metal_render_encoder(struct MetalCommand *command,const char *selector,struct MetalDescriptor *descriptor) {
    if (!descriptor || descriptor->kind!=MD_PASS) metal_render_stop("Invalid Metal render-pass descriptor",selector);
    struct MetalRenderEncoder *encoder=calloc(1,sizeof(*encoder)); if (!encoder) return 0;
    encoder->state=metal_render_state_acquire();
    if (!encoder->state) { free(encoder); return 0; }
    metal_memory_add(3,sizeof(*encoder));
    encoder->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalRenderCommandEncoder"),.device=metal_command_device(&command->object)};
    encoder->command=(void *)metal_object_retain((Word)command);encoder->state->width=encoder->state->height=UINT32_MAX;encoder->state->samples=VK_SAMPLE_COUNT_1_BIT;
    encoder->state->key=(struct MetalPipelineKey){.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.front=VK_FRONT_FACE_CLOCKWISE,.polygon=VK_POLYGON_MODE_FILL};
    encoder->state->depth=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    pthread_mutex_lock(&command->mutex);
    if (command->object.content[1]>=2 || command->active_encoder) {pthread_mutex_unlock(&command->mutex);metal_render_state_return(encoder->state);metal_memory_sub(3,sizeof(*encoder));metal_object_release((Word)command);free(encoder);metal_render_stop("Command cannot create a render encoder",selector);}
    command->active_encoder=(Word)encoder; encoder->next=command->render_encoders; command->render_encoders=encoder;
    pthread_mutex_unlock(&command->mutex);
    struct MetalDescriptorArray *colors=(void *)descriptor->colors;
    for (unsigned i=0;i<8;++i) {
        encoder->state->colors[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        struct MetalDescriptor *color=colors && i<colors->count ? (void *)colors->items[i] : NULL;
        if (!color || !metal_property(color,"texture")) continue;
        unsigned index=metal_render_attachment(encoder,color,0);
        encoder->state->colors[i].attachment=index; encoder->state->color_formats[i]=encoder->state->attachments[index]->format.format; encoder->state->color_count=i+1;
    }
    struct MetalDescriptor *depth=(void *)descriptor->depth,*stencil=(void *)descriptor->stencil;
    struct MetalTexture *depth_texture=depth ? (void *)metal_property(depth,"texture") : NULL,*stencil_texture=stencil ? (void *)metal_property(stencil,"texture") : NULL;
    if (depth_texture && stencil_texture && depth_texture!=stencil_texture) metal_render_stop("Separate depth/stencil render attachments require conversion",selector);
    if (depth_texture || stencil_texture) {
        unsigned index=metal_render_attachment(encoder,depth_texture ? depth : stencil,1);
        encoder->state->depth.attachment=index; encoder->state->depth_format=encoder->state->attachments[index]->format.format;
        if (stencil_texture) {
            Word load=metal_property(stencil,"loadAction");
            if (load>2 || metal_property(stencil,"storeAction")>3 || metal_property(stencil,"level")!=metal_property(depth_texture ? depth : stencil,"level") ||
                metal_property(stencil,"slice")!=metal_property(depth_texture ? depth : stencil,"slice")) metal_render_stop("Unsupported stencil action or subresource",selector);
            const VkAttachmentLoadOp loads[]={VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_LOAD_OP_CLEAR};
            encoder->state->descriptions[index].stencilLoadOp=loads[load]; encoder->state->clear[index].depthStencil.stencil=(uint32_t)metal_property(stencil,"clearStencil");
            encoder->state->final_stencil_store[index]=metal_property(stencil,"storeAction")==0 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        }
        if (!depth_texture) encoder->state->final_store[index]=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (depth_texture) metal_render_resolve_attachment(encoder,depth,index,VK_IMAGE_ASPECT_DEPTH_BIT);
        if (stencil_texture) metal_render_resolve_attachment(encoder,stencil,index,VK_IMAGE_ASPECT_STENCIL_BIT);
    }
    if (!encoder->state->attachment_count) { encoder->state->width=encoder->state->height=1; }
    /* Resolves run after this pass and need the multisample source. Readback
     * likewise needs stored pixels. Internal splits always preserve both aspects. */
    for (unsigned i=0;i<encoder->state->attachment_count;++i) {
        unsigned preserve=metal_diagnostic_readback;
        for (struct MetalRenderResolve *resolve=encoder->state->resolves;resolve;resolve=resolve->next)
            if (resolve->attachment==i) preserve=1;
        VkImageAspectFlags aspects=encoder->state->attachments[i]->format.aspects;
        if (preserve) encoder->state->final_store[i]=encoder->state->final_stencil_store[i]=VK_ATTACHMENT_STORE_OP_STORE;
        if (((aspects&~VK_IMAGE_ASPECT_STENCIL_BIT) && encoder->state->final_store[i]==VK_ATTACHMENT_STORE_OP_DONT_CARE) ||
            ((aspects&VK_IMAGE_ASPECT_STENCIL_BIT) && encoder->state->final_stencil_store[i]==VK_ATTACHMENT_STORE_OP_DONT_CARE)) encoder->state->deferred_store=1;
    }
    metal_render_pass_bundle(encoder);
    encoder->state->viewport=(VkViewport){0,0,(float)encoder->state->width,(float)encoder->state->height,0,1}; encoder->state->scissor=(VkRect2D){{0,0},{encoder->state->width,encoder->state->height}};
    LOG_LIMITED(32,"Vulkan render encoder ready: %ux%u colorSlots=%u attachments=%u",encoder->state->width,encoder->state->height,encoder->state->color_count,encoder->state->attachment_count);
    metal_lifetime_init((Word)encoder,__builtin_offsetof(struct MetalRenderEncoder,lifetime),metal_render_destroy);
    return metal_retain_autorelease((Word)encoder);
}
static void metal_render_end(struct MetalRenderEncoder *encoder,const char *selector) {
    metal_render_check(encoder,selector); metal_render_lock(encoder->command);
    metal_render_begin(encoder); metal_render_finish_pass(encoder,!metal_diagnostic_readback);
    metal_render_resolve(encoder);
    metal_diagnostic_capture(encoder);
    /* Attachment dependencies belong to their image transitions and the next
     * render pass. Shader storage writes also need visibility to buffer users. */
    if (encoder->state->pending_storage) metal_render_storage_barrier(encoder);
    metal_render_unlock(encoder->command); encoder->ended=1; encoder->command->active_encoder=0; ++encoder->command->recorded_operations;
    metal_object_release((Word)encoder->command);
}
static void metal_render_pipeline(struct MetalRenderEncoder *encoder,const char *selector,struct MetalPipeline *pipeline) {
    metal_render_check(encoder,selector);
    if (!pipeline || pipeline->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalRenderPipelineState")) metal_render_stop("Invalid render pipeline state",selector);
    if (pipeline->color_count!=encoder->state->color_count || pipeline->depth_format!=encoder->state->depth_format || pipeline->samples!=encoder->state->samples ||
        memcmp(pipeline->colors,encoder->state->color_formats,sizeof(pipeline->colors))) metal_render_stop("Render pipeline and attachments are incompatible",selector);
    if (encoder->state->pipeline!=pipeline) metal_command_use(encoder->command,(Word)pipeline);
    encoder->state->pipeline=pipeline;
}
static void metal_render_state(struct MetalRenderEncoder *encoder,const char *selector,Word value) {
    metal_render_check(encoder,selector);
    if (!strcmp(selector,"setDepthStencilState:")) {
        if (value && read_word(value)!=external_class("_OBJC_CLASS_$_I2AMetalDepthStencilState")) metal_render_stop("Invalid depth/stencil state",selector);
        if (value && encoder->state->key.depth!=(void *)value) metal_command_use(encoder->command,value);
        encoder->state->key.depth=(void *)value; return;
    }
    if (!strcmp(selector,"setCullMode:")) { if (value>2) metal_render_stop("Invalid cull mode",selector); encoder->state->key.cull=(VkCullModeFlags)value; }
    else if (!strcmp(selector,"setFrontFacingWinding:")) { if (value>1) metal_render_stop("Invalid front winding",selector); encoder->state->key.front=value ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; }
    else if (!strcmp(selector,"setTriangleFillMode:")) { if (value>1 || (value && !vulkan.features.fillModeNonSolid)) metal_render_stop("Unsupported triangle fill",selector); encoder->state->key.polygon=value ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; }
    else { if (value>1 || (value && !vulkan.features.depthClamp)) metal_render_stop("Unsupported depth clipping",selector); encoder->state->key.clamp=(unsigned)value; }
}
static void metal_render_buffer(struct MetalRenderEncoder *encoder,const char *selector,struct MetalBuffer *buffer,size_t offset,Word index) {
    metal_render_check(encoder,selector);
    if (index>=31 || (buffer && (buffer->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalBuffer") || offset>buffer->length))) metal_render_stop("Invalid render buffer binding",selector);
    unsigned stage_index=strstr(selector,"Fragment")!=NULL;
    if (buffer && encoder->state->buffers[stage_index][index].buffer!=buffer) metal_command_use(encoder->command,(Word)buffer);
    encoder->state->buffers[stage_index][index]=(struct MetalBoundBuffer){buffer,offset};
}
static void metal_render_texture(struct MetalRenderEncoder *encoder,const char *selector,struct MetalTexture *texture,Word index) {
    metal_render_check(encoder,selector);
    if (index>=128 || (texture && (texture->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalTexture") || !(texture->native_usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT))))) metal_render_stop("Invalid render texture binding",selector);
    unsigned stage_index=strstr(selector,"Fragment")!=NULL;
    if (texture && encoder->state->textures[stage_index][index]!=texture) metal_command_use(encoder->command,(Word)texture);
    encoder->state->textures[stage_index][index]=texture;
}
static void metal_render_sampler(struct MetalRenderEncoder *encoder,const char *selector,struct MetalSampler *sampler,Word index) {
    metal_render_check(encoder,selector);
    if (index>=16 || (sampler && sampler->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalSamplerState"))) metal_render_stop("Invalid render sampler binding",selector);
    unsigned stage_index=strstr(selector,"Fragment")!=NULL;
    if (sampler && encoder->state->samplers[stage_index][index]!=sampler) metal_command_use(encoder->command,(Word)sampler);
    encoder->state->samplers[stage_index][index]=sampler;
}
static int metal_render_feedback(struct MetalRenderEncoder *encoder,struct MetalTexture *texture) {
    for (unsigned i=0;i<encoder->state->attachment_count;++i) if (texture==encoder->state->attachments[i]) return 1;
    return 0;
}
static int metal_color_history_eligible(const struct MetalRenderState *state,const struct MetalTexture *source) {
    if (state->color_count!=1 || state->colors[0].attachment==VK_ATTACHMENT_UNUSED ||
        source!=state->attachments[state->colors[0].attachment]) return 0;
    /* No undefined initial data, subresource aliasing, resolve or block alignment
     * assumptions. Everything outside this simple case takes the full path. */
    return source->type==2 && source->samples==1 && source->layers==1 && source->depth==1 && source->mips==1 &&
        source->format.aspects==VK_IMAGE_ASPECT_COLOR_BIT && source->format.width==1 && source->format.height==1 &&
        source->width==state->width && source->height==state->height &&
        state->descriptions[state->colors[0].attachment].loadOp==VK_ATTACHMENT_LOAD_OP_LOAD;
}
static void metal_color_history_written(struct MetalRenderState *state,int nonempty) {
    struct MetalColorHistory *history=&state->history;
    if (!history->source || !nonempty || !state->pipeline->rasterization) return;
    unsigned mask=state->pipeline->blend[0].colorWriteMask;
    if (!mask) return;
    /* Vulkan scissors here are nonnegative and their sums fit INT32_MAX (setter).
     * Unlike the viewport, the scissor also bounds wide lines and point sprites. */
    VkRect2D r=state->scissor;
    uint32_t x=r.offset.x,y=r.offset.y,right=x+r.extent.width,bottom=y+r.extent.height;
    if (right>state->width) right=state->width;
    if (bottom>state->height) bottom=state->height;
    if (x>=right || y>=bottom) return;
    if (history->dirty) {
        VkRect2D old=history->bounds;uint32_t ox=old.offset.x,oy=old.offset.y;
        if (ox<x) x=ox;
        if (oy<y) y=oy;
        if (right<ox+old.extent.width) right=ox+old.extent.width;
        if (bottom<oy+old.extent.height) bottom=oy+old.extent.height;
    }
    history->bounds=(VkRect2D){{(int32_t)x,(int32_t)y},{right-x,bottom-y}};history->dirty|=mask;
}
static void metal_render_prepare_snapshots(struct MetalRenderEncoder *encoder) {
    struct MetalColorHistory *history=&encoder->state->history;
    unsigned required=0;
    for (unsigned stage_index=0;stage_index<2;++stage_index) {
        struct MetalFunction *function=stage_index ? encoder->state->pipeline->fragment : encoder->state->pipeline->vertex;
        for (unsigned i=0;function && i<function->spec->binding_count;++i) {
            const struct ShaderBindingSpec *binding=&shader_bindings[function->spec->first_binding+i];
            if (binding->type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || binding->metal_index>=128) continue;
            struct MetalTexture *source=encoder->state->textures[stage_index][binding->metal_index];
            if (!source || !metal_render_feedback(encoder,source)) continue;
            if (!history->source && metal_color_history_eligible(encoder->state,source)) history->source=source;
            /* Union both stages and every binding of the same source before the
             * first descriptor can decide to reuse a partially stale snapshot. */
            if (history->source==source) required|=source->sampled_identity && binding->sampled_components ? binding->sampled_components : 15U;
            struct MetalRenderSnapshot *snapshot=encoder->command->snapshots;
            while (snapshot && snapshot->source!=source) snapshot=snapshot->next;
            if (snapshot) continue;
            snapshot=metal_snapshot_cached(source);
            int reused=snapshot!=NULL;
            size_t reclaimed=0;
            if(!snapshot) {
                snapshot=calloc(1,sizeof(*snapshot));if(!snapshot)metal_render_stop("Allocation failed","Render feedback snapshot");
                struct MetalDescriptor *description=(void *)metal_descriptor_create(MD_TEXTURE);
                const char *names[]={"pixelFormat","textureType","width","height","depth","mipmapLevelCount","sampleCount","arrayLength","usage","resourceOptions"};
                Word values[]={source->format.metal,source->type,source->width,source->height,source->depth,source->mips,source->samples,source->array_length,1,0x20};
                for(unsigned j=0;j<COUNT(names);++j)metal_property_assign(description,names[j],values[j]);
                snapshot->texture=(void *)metal_texture_new(source->object.device,"Render feedback snapshot",description);
                reclaimed=metal_texture_creation_reclaimed;
                metal_object_release((Word)description);
            }
            if(!snapshot->texture) {
                char detail[384];snprintf(detail,sizeof(detail),"shader=%s result=%d Metal=%u VkFormat=%d type=%lu extent=%lux%lux%lu mips=%lu samples=%lu layers=%lu usage=0x%x source_allocation=%llu reclaimed_idle_bytes=%zu",
                    function->spec->name,metal_texture_creation_result,source->format.metal,source->format.format,source->type,source->width,source->height,source->depth,source->mips,source->samples,source->layers,
                    source->native_usage,(unsigned long long)source->allocation,reclaimed);
                free(snapshot);metal_render_stop("Cannot allocate native attachment snapshot",detail);
            }
            snapshot->source=source;snapshot->encoder=NULL;snapshot->generation=UINT_MAX;snapshot->next=encoder->command->snapshots;encoder->command->snapshots=snapshot;
            metal_command_texture(encoder->command,snapshot->texture);
            LOG_LIMITED(24,"Vulkan attachment snapshot %s: %s texture=%u %lux%lu aspects=0x%x",reused ? "reused" : "allocated",function->spec->name,binding->metal_index,source->width,source->height,source->format.aspects);
        }
    }
    if (history->source) history->required=required;
}
static struct MetalTexture *metal_render_snapshot(struct MetalRenderEncoder *encoder,struct MetalTexture *source) {
    struct MetalRenderSnapshot *snapshot=encoder->command->snapshots;
    while (snapshot && snapshot->source!=source) snapshot=snapshot->next;
    if (!snapshot) metal_render_stop("Missing prepared attachment snapshot","draw");
    if(snapshot->encoder==encoder && snapshot->generation==encoder->state->draws)return snapshot->texture;
    unsigned incremental=snapshot->encoder==encoder;
    struct MetalColorHistory *history=&encoder->state->history;
    int tracked=history->source==source && history->required && !encoder->state->pipeline->compute_fallback;
    if (incremental && tracked && !(history->dirty&history->required)) {
        /* Other components may still be stale. Keep their dirty bounds for a
         * later draw with a different read mask, even if this draw can reuse. */
        snapshot->generation=encoder->state->draws;
        return snapshot->texture;
    }
    /* Capture history before the first attachment load can discard it.
     * Explicit clears still define the sampled attachment's initial contents. */
    metal_render_close_pass(encoder);
    metal_render_transition(encoder,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT);
    metal_render_transition(encoder,snapshot->texture,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT);
    unsigned copied=0;
    for (uint32_t level=0;level<source->mips;++level) {
        uint32_t width=(uint32_t)(source->width>>level),height=(uint32_t)(source->height>>level),depth=(uint32_t)(source->depth>>level);
        if (!width) width=1; if (!height) height=1; if (!depth) depth=1;
        for (unsigned aspect=1;aspect<=VK_IMAGE_ASPECT_STENCIL_BIT;aspect<<=1) if (source->format.aspects&aspect) {
            /* After the first complete snapshot, only this encoder's attachment
             * subresources can change. Before its first draw, a clear replaces
             * that subresource entirely; copying the old bytes serves no use. */
            uint32_t first=UINT32_MAX;
            for (uint32_t layer=0;layer<=(uint32_t)source->layers;++layer) {
                unsigned attachment=0,cleared=0;
                for (unsigned i=0;layer<source->layers && i<encoder->state->attachment_count;++i) {
                    VkImageSubresourceRange range=encoder->state->view_descriptions[i].subresourceRange;
                    if (encoder->state->attachments[i]!=source || !(range.aspectMask&aspect) || range.baseMipLevel!=level || range.baseArrayLayer!=layer) continue;
                    attachment=1;
                    const VkAttachmentDescription *description=&encoder->state->descriptions[i];
                    cleared|=!encoder->state->started && (aspect==VK_IMAGE_ASPECT_STENCIL_BIT ? description->stencilLoadOp : description->loadOp)==VK_ATTACHMENT_LOAD_OP_CLEAR;
                }
                unsigned copy=layer<source->layers && (!incremental || attachment) && !cleared;
                if (copy && first==UINT32_MAX) first=layer;
                if (!copy && first!=UINT32_MAX) {
                    VkImageCopy region={.srcSubresource={aspect,level,first,layer-first},.dstSubresource={aspect,level,first,layer-first},.extent={width,height,depth}};
                    if (tracked && incremental) {
                        region.srcOffset=region.dstOffset=(VkOffset3D){history->bounds.offset.x,history->bounds.offset.y,0};
                        region.extent=(VkExtent3D){history->bounds.extent.width,history->bounds.extent.height,1};
                    }
                    VKD(vkCmdCopyImage)(encoder->command->command,source->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,snapshot->texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
#if I2A_DEVELOPMENT
                    if (encoder->command->diagnostic_pass_enabled) {
                        ++encoder->state->pending_snapshot_regions;
                        if (aspect==VK_IMAGE_ASPECT_COLOR_BIT) {
                            uint64_t blocks_x=(region.extent.width+source->format.width-1)/source->format.width;
                            uint64_t blocks_y=(region.extent.height+source->format.height-1)/source->format.height;
                            encoder->state->pending_snapshot_color_bytes+=blocks_x*blocks_y*region.extent.depth*(layer-first)*source->samples*source->format.bytes;
                        }
                    }
#endif
                    first=UINT32_MAX;copied=1;
                }
            }
        }
    }
    if (!encoder->state->started) {
        for (unsigned i=0;i<encoder->state->attachment_count;++i) {
            if (encoder->state->attachments[i]!=source) continue;
            const VkAttachmentDescription *description=&encoder->state->descriptions[i];
            VkImageSubresourceRange range=encoder->state->view_descriptions[i].subresourceRange;
            VkImageAspectFlags clear=0;
            if (description->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) clear=range.aspectMask&~VK_IMAGE_ASPECT_STENCIL_BIT;
            if (description->stencilLoadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) clear|=range.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT;
            if (!clear) continue;
            VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT};
            if (copied) VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,NULL,0,NULL);
            range.aspectMask=clear;
            if (clear==VK_IMAGE_ASPECT_COLOR_BIT)
                VKD(vkCmdClearColorImage)(encoder->command->command,snapshot->texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&encoder->state->clear[i].color,1,&range);
            else
                VKD(vkCmdClearDepthStencilImage)(encoder->command->command,snapshot->texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&encoder->state->clear[i].depthStencil,1,&range);
        }
    }
    /* The descriptor consumer transitions the copied image from TRANSFER_DST
     * directly to its required layout and shader stage before binding it. */
    if (history->source==source) history->dirty=0;
    snapshot->encoder=encoder;snapshot->generation=encoder->state->draws;++encoder->command->recorded_operations;
    return snapshot->texture;
}
static void metal_render_descriptors(struct MetalRenderEncoder *encoder) {
    struct MetalPipeline *pipeline=encoder->state->pipeline;
    uint32_t dynamic_offsets[128]={0};
    if (pipeline->dynamic_count>COUNT(dynamic_offsets)) metal_render_stop("Graphics dynamic descriptor count exceeds backend capacity","draw");
    unsigned stages[128]; struct MetalDescriptorValue values[128];
    VkWriteDescriptorSet writes[128]; VkDescriptorBufferInfo buffer_infos[128]; VkDescriptorImageInfo image_infos[128]; unsigned write_count=0;
    for (unsigned stage_index=0;stage_index<2;++stage_index) {
        struct MetalFunction *function=stage_index ? encoder->state->pipeline->fragment : encoder->state->pipeline->vertex;
        for (unsigned i=0;function && i<function->spec->binding_count;++i) {
            const struct ShaderBindingSpec *binding=&shader_bindings[function->spec->first_binding+i];
            if (binding->literal_sampler) continue;
            if (write_count>=COUNT(writes)) metal_render_stop("Graphics descriptor count exceeds backend capacity",function->spec->name);
            VkDescriptorBufferInfo *buffer_info=&buffer_infos[write_count]; VkDescriptorImageInfo *image_info=&image_infos[write_count];
            *buffer_info=(VkDescriptorBufferInfo){0}; *image_info=(VkDescriptorImageInfo){0};
            VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=binding->binding,
                .descriptorCount=1,.descriptorType=metal_pipeline_binding_type(pipeline,binding->type)};
            if (binding->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && binding->metal_index==UINT32_MAX-1) {
                struct MetalBuffer *diagnostic=metal_math_buffer(encoder->command);
                metal_math_trace_render(encoder,function);
                *buffer_info=(VkDescriptorBufferInfo){diagnostic->buffer,0,diagnostic->length}; write.pBufferInfo=buffer_info;
            } else if (binding->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || binding->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                if (binding->metal_index>=31) metal_render_stop("Invalid shader buffer index",function->spec->name);
                struct MetalBoundBuffer *use=&encoder->state->buffers[stage_index][binding->metal_index];
                VkDeviceSize alignment=binding->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? vulkan.properties.limits.minUniformBufferOffsetAlignment : vulkan.properties.limits.minStorageBufferOffsetAlignment;
                if (!use->buffer || use->offset>use->buffer->length || !binding->size || binding->metal_size>use->buffer->length-use->offset ||
                    binding->size>use->buffer->native_length-use->offset || use->offset%alignment) {
                    char detail[240]; snprintf(detail,sizeof(detail),"%s buffer=%u offset=%llu metalSize=%u vkRange=%u length=%zu nativeLength=%zu alignment=%llu",function->spec->name,binding->metal_index,
                        (unsigned long long)use->offset,binding->metal_size,binding->size,use->buffer ? use->buffer->length : 0,use->buffer ? use->buffer->native_length : 0,
                        (unsigned long long)alignment); metal_render_stop("Missing, undersized or unaligned shader buffer",detail);
                }
                *buffer_info=(VkDescriptorBufferInfo){use->buffer->buffer,use->offset,binding->size}; write.pBufferInfo=buffer_info;
                unsigned slot=pipeline->dynamic_slots[stage_index][i];
                if (slot && use->offset<=UINT32_MAX) {
                    dynamic_offsets[slot-1]=(uint32_t)use->offset; buffer_info->offset=0;
                }
                 
#if I2A_DEVELOPMENT
                if (metal_diagnostic_readback && runtime_extension.shader_buffer_diagnostic && use->buffer->mapped) {
                    float viewport[]={encoder->state->viewport.x,encoder->state->viewport.y,encoder->state->viewport.width,encoder->state->viewport.height};
                    runtime_extension.shader_buffer_diagnostic(function->spec->name,(char *)use->buffer->mapped+use->offset,binding->metal_size,viewport);
                }
#endif
            } else if (binding->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || binding->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                struct MetalTexture *texture=binding->metal_index<128 ? encoder->state->textures[stage_index][binding->metal_index] : NULL;
                if (!texture) metal_render_stop("Missing active shader texture",function->spec->name);
                unsigned storage=binding->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (!(texture->native_usage&(storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT))) metal_render_stop("Incompatible graphics texture usage",function->spec->name);
                if (metal_render_feedback(encoder,texture)) {
                    if (storage) metal_render_stop("A graphics storage image cannot also be a render attachment",function->spec->name);
                    texture=metal_render_snapshot(encoder,texture);
                }
                VkImageLayout layout=pipeline->descriptor_counts[6] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkPipelineStageFlags stages=pipeline->compute_fallback ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT :
                    stage_index ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
                metal_render_transition_stages(encoder,texture,layout,VK_ACCESS_SHADER_READ_BIT|(storage ? VK_ACCESS_SHADER_WRITE_BIT : 0),stages);
                *image_info=(VkDescriptorImageInfo){VK_NULL_HANDLE,storage ? texture->storage_view : texture->sampled_view,layout}; write.pImageInfo=image_info;
            } else if (binding->type==VK_DESCRIPTOR_TYPE_SAMPLER) {
                struct MetalSampler *sampler=binding->metal_index<16 ? encoder->state->samplers[stage_index][binding->metal_index] : NULL;
                if (!sampler) metal_render_stop("Missing active shader sampler",function->spec->name);
                image_info->sampler=sampler->sampler; write.pImageInfo=image_info;
            } else metal_render_stop("Unsupported graphics descriptor type",function->spec->name);
            values[write_count]=write.pBufferInfo ?
                (struct MetalDescriptorValue){(uint64_t)(uintptr_t)buffer_info->buffer,buffer_info->offset,buffer_info->range} :
                (struct MetalDescriptorValue){binding->type==VK_DESCRIPTOR_TYPE_SAMPLER ? (uint64_t)(uintptr_t)image_info->sampler : (uint64_t)(uintptr_t)image_info->imageView,0,0};
            stages[write_count]=stage_index; writes[write_count++]=write;
        }
    }
    /* Descriptor snapshots are immutable until this command's fence completes.
     * Binding offsets travel with the draw; textures still perform their normal
     * transitions and feedback copies even when their descriptor set is reused. */
    uint64_t hash=(uint64_t)(uintptr_t)pipeline;
    for (unsigned i=0;i<write_count;++i) {
        hash=(hash^values[i].resource)*UINT64_C(1099511628211);
        hash=(hash^values[i].offset)*UINT64_C(1099511628211);
        hash=(hash^values[i].range)*UINT64_C(1099511628211);
    }
    /* Aligned handles and offsets have little entropy in the low bucket bits. */
    hash=(hash^(hash>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    hash=(hash^(hash>>27))*UINT64_C(0x94d049bb133111eb);
    hash^=hash>>31;
    unsigned bucket=(unsigned)hash&(COUNT(encoder->state->descriptors)-1);
    struct MetalDescriptorSnapshot *snapshot=encoder->state->descriptors[bucket];
    while (snapshot && (snapshot->hash!=hash || snapshot->pipeline!=pipeline || snapshot->count!=write_count ||
        memcmp(snapshot->values,values,write_count*sizeof(*values)))) snapshot=snapshot->next;
#if I2A_DEVELOPMENT
    if (metal_diagnostic_enabled) __atomic_fetch_add(&metal_descriptor_cache_counts[snapshot ? 0 : 1],1,__ATOMIC_RELAXED);
#endif
    VkDescriptorSet sets[2];
    if (snapshot) memcpy(sets,snapshot->sets,sizeof(sets));
    else {
        struct MetalDescriptorPoolChunk *chunk=encoder->state->pools;
        int fits=chunk && chunk->sets+2<=chunk->limit && metal_descriptor_pool_matches(chunk,pipeline);
        if (!fits) {
            chunk=metal_descriptor_pool_acquire(pipeline); if (!chunk) metal_render_stop("Allocation failed","Render descriptor pool");
            chunk->next=encoder->state->pools; encoder->state->pools=chunk;
        }
        if(chunk->sets<chunk->allocated)memcpy(sets,chunk->allocated_sets+chunk->sets,sizeof(sets));
        else {
            VkDescriptorSetAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=chunk->pool,
                .descriptorSetCount=2,.pSetLayouts=chunk->layouts->sets};
            VkResult result=VKD(vkAllocateDescriptorSets)(vulkan.device,&allocate,sets);
            if ((result==VK_ERROR_OUT_OF_POOL_MEMORY || result==VK_ERROR_FRAGMENTED_POOL) && chunk->sets) {
                chunk->limit=chunk->allocated;
                chunk=metal_descriptor_pool_acquire(pipeline);if(!chunk)metal_render_stop("Allocation failed","Render descriptor pool");
                chunk->next=encoder->state->pools;encoder->state->pools=chunk;allocate.descriptorPool=chunk->pool;allocate.pSetLayouts=chunk->layouts->sets;
                if(chunk->allocated){memcpy(sets,chunk->allocated_sets,sizeof(sets));result=VK_SUCCESS;}
                else result=VKD(vkAllocateDescriptorSets)(vulkan.device,&allocate,sets);
            }
            metal_render_result(result,"vkAllocateDescriptorSets");
            if(chunk->sets==chunk->allocated){memcpy(chunk->allocated_sets+chunk->allocated,sets,sizeof(sets));chunk->allocated+=2;}
        }
        chunk->sets+=2;
        unsigned pair=chunk->sets/2-1;
        struct MetalDescriptorSnapshot *entry=chunk->snapshots[pair];
        for(unsigned i=0;i<write_count;++i)writes[i].dstSet=sets[stages[i]];
        if(write_count)VKD(vkUpdateDescriptorSets)(vulkan.device,write_count,writes,0,NULL);
        if(!entry && encoder->state->descriptor_count<128) {
            size_t bytes=sizeof(*entry)+write_count*sizeof(*values);entry=malloc(bytes);
            if(entry){chunk->snapshots[pair]=entry;metal_memory_add(3,bytes);}
        }
        if(entry) {
            *entry=(struct MetalDescriptorSnapshot){.pipeline=pipeline,.sets={sets[0],sets[1]},.hash=hash,.count=write_count};
            memcpy(entry->values,values,write_count*sizeof(*values));
            if(encoder->state->descriptor_count<128) {
                entry->next=encoder->state->descriptors[bucket];encoder->state->descriptors[bucket]=entry;
                ++encoder->state->descriptor_count;
            }
        }
    }
    if (!encoder->state->pipeline->compute_fallback) metal_render_begin(encoder);
    struct MetalRenderState *state=encoder->state;
    if (pipeline->compute_fallback || state->bound_descriptor_layout!=pipeline->layout ||
        memcmp(state->bound_descriptor_sets,sets,sizeof(sets)) || state->bound_dynamic_count!=pipeline->dynamic_count ||
        memcmp(state->bound_dynamic_offsets,dynamic_offsets,pipeline->dynamic_count*sizeof(*dynamic_offsets))) {
        VKD(vkCmdBindDescriptorSets)(pipeline->compute_fallback ? encoder->command->command : state->render_command,pipeline->compute_fallback ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->layout,0,2,sets,pipeline->dynamic_count,dynamic_offsets);
        if (!pipeline->compute_fallback) {
            state->bound_descriptor_layout=pipeline->layout;memcpy(state->bound_descriptor_sets,sets,sizeof(sets));
            state->bound_dynamic_count=pipeline->dynamic_count;memcpy(state->bound_dynamic_offsets,dynamic_offsets,pipeline->dynamic_count*sizeof(*dynamic_offsets));
        }
    }
}
static void metal_render_prepare_draw(struct MetalRenderEncoder *encoder,Word primitive) {
    uint64_t diagnostic_start=metal_diagnostic_time();
    const VkPrimitiveTopology topologies[]={VK_PRIMITIVE_TOPOLOGY_POINT_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    if (!encoder->state->pipeline || primitive>=COUNT(topologies)) metal_render_stop("Missing render pipeline or invalid primitive","draw");
    encoder->state->key.topology=topologies[primitive]; VkPipeline pipeline;
    if (encoder->state->variant_owner==encoder->state->pipeline && metal_pipeline_key_equal(&encoder->state->variant_key,&encoder->state->key))
        pipeline=encoder->state->variant_pipeline;
    else {
        metal_render_result(metal_pipeline_variant(encoder->state->pipeline,&encoder->state->key,&pipeline),"vkCreateGraphicsPipelines draw variant");
        encoder->state->variant_owner=encoder->state->pipeline;encoder->state->variant_key=encoder->state->key;encoder->state->variant_pipeline=pipeline;
    }
    uint64_t diagnostic_pipeline=metal_diagnostic_time();
    if (encoder->state->pending_storage) metal_render_storage_barrier(encoder);
    else if ((encoder->state->pipeline->vertex->requirements|(encoder->state->pipeline->fragment ? encoder->state->pipeline->fragment->requirements : 0))&METAL_SHADER_STORAGE_WRITES) {
        /* A buffer read in an earlier pass must finish before this shader can
         * overwrite it. Image layout tracking alone cannot order buffer WAR. */
        metal_render_close_pass(encoder);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
    }
    metal_render_descriptors(encoder);
    uint64_t diagnostic_descriptors=metal_diagnostic_time();
    metal_diagnostic_pass_draw(encoder);
    if (diagnostic_start) {
        metal_diagnostic_add(0,diagnostic_pipeline-diagnostic_start);
        metal_diagnostic_add(1,diagnostic_descriptors-diagnostic_pipeline);
    }
    VkCommandBuffer command=encoder->state->render_command;
    if (encoder->state->bound_pipeline!=pipeline) { VKD(vkCmdBindPipeline)(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); encoder->state->bound_pipeline=pipeline; }
    for (unsigned i=0;i<encoder->state->pipeline->binding_count;++i) {
        uint32_t index=encoder->state->pipeline->bindings[i].binding; struct MetalBoundBuffer *use=&encoder->state->buffers[0][index];
        if (!use->buffer || use->offset>=use->buffer->length) metal_render_stop("Missing active vertex buffer",encoder->state->pipeline->vertex->spec->name);
        if (encoder->state->bound_vertices[index].buffer!=use->buffer || encoder->state->bound_vertices[index].offset!=use->offset) {
            VKD(vkCmdBindVertexBuffers)(command,index,1,&use->buffer->buffer,&use->offset); encoder->state->bound_vertices[index]=*use;
        }
    }
    if (!encoder->state->dynamic_set || encoder->state->dynamic_dirty) {
        unsigned dirty=encoder->state->dynamic_set ? encoder->state->dynamic_dirty : METAL_DYNAMIC_ALL;
        if (dirty&METAL_DYNAMIC_VIEWPORT) VKD(vkCmdSetViewport)(command,0,1,&encoder->state->viewport);
        if (dirty&METAL_DYNAMIC_SCISSOR) {
            VkRect2D scissor=encoder->state->scissor;
            if (encoder->state->viewport_empty) scissor=(VkRect2D){{0,0},{0,0}};
            VKD(vkCmdSetScissor)(command,0,1,&scissor);
        }
        if (dirty&METAL_DYNAMIC_BLEND) VKD(vkCmdSetBlendConstants)(command,encoder->state->blend);
        if (dirty&METAL_DYNAMIC_BIAS) VKD(vkCmdSetDepthBias)(command,encoder->state->bias,encoder->state->clamp,encoder->state->slope);
        encoder->state->dynamic_dirty=0;
    }
    if (!encoder->state->dynamic_set || encoder->state->stencil_dirty) {
        VKD(vkCmdSetStencilReference)(command,VK_STENCIL_FACE_FRONT_BIT,encoder->state->front_reference); VKD(vkCmdSetStencilReference)(command,VK_STENCIL_FACE_BACK_BIT,encoder->state->back_reference);
        encoder->state->stencil_dirty=0;
    }
    encoder->state->dynamic_set=1;
}
static void metal_render_storage_barrier(struct MetalRenderEncoder *encoder) {
    metal_render_close_pass(encoder);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT};
    VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    encoder->state->pending_storage=0;
}
static void metal_render_storage_written(struct MetalRenderEncoder *encoder) {
    struct MetalPipeline *pipeline=encoder->state->pipeline;
    encoder->state->pending_storage=((pipeline->vertex->requirements|(pipeline->fragment ? pipeline->fragment->requirements : 0))&METAL_SHADER_STORAGE_WRITES)!=0;
}
static void metal_render_draw(struct MetalRenderEncoder *encoder,const char *selector,Word primitive,size_t start,size_t count) {
    metal_render_check(encoder,selector); if (start>UINT32_MAX || count>UINT32_MAX) metal_render_stop("Vertex draw range exceeds Vulkan limits",selector);
    if (!encoder->state->pipeline) metal_render_stop("Missing render pipeline",selector);
    if (encoder->state->pipeline->compute_fallback) {
        if (start || count!=1 || primitive!=0) metal_render_stop("Unexpected compute substitution range",selector);
        metal_render_lock(encoder->command); metal_render_close_pass(encoder);
        metal_render_descriptors(encoder);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
        VKD(vkCmdBindPipeline)(encoder->command->command,VK_PIPELINE_BIND_POINT_COMPUTE,encoder->state->pipeline->compute_fallback);
        VKD(vkCmdDispatch)(encoder->command->command,1,1,1);
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
        VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        encoder->state->pending_storage=0;
        metal_render_unlock(encoder->command); ++encoder->state->draws; ++encoder->command->recorded_operations;
        LOG_LIMITED(1,"Verified non-rasterizing vertex executed as one compute invocation");
        return;
    }
    metal_render_prepare_snapshots(encoder);
    metal_render_lock(encoder->command);
    metal_render_prepare_draw(encoder,primitive); VKD(vkCmdDraw)(encoder->state->render_command,(uint32_t)count,1,(uint32_t)start,0);
    metal_color_history_written(encoder->state,count!=0);
    metal_render_storage_written(encoder);
    metal_render_unlock(encoder->command); ++encoder->state->draws; ++encoder->command->recorded_operations;
    LOG_LIMITED(32,"Vulkan draw recorded: %s vertices=%zu",encoder->state->pipeline->vertex->spec->name,count);
}
static void metal_render_draw_indexed_instances(struct MetalRenderEncoder *encoder,const char *selector,Word primitive,size_t count,Word type,struct MetalBuffer *buffer,size_t offset,size_t instances) {
    metal_render_check(encoder,selector); size_t element=type ? 4 : 2;
    if (type>1 || !buffer || buffer->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalBuffer") || offset>buffer->length || offset%element || count>(buffer->length-offset)/element || count>UINT32_MAX || instances>UINT32_MAX)
        metal_render_stop("Invalid index buffer or draw range",selector);
    if (!encoder->state->pipeline) metal_render_stop("Missing render pipeline",selector);
    if (encoder->state->bound_index!=buffer->buffer) metal_command_use(encoder->command,(Word)buffer);
    metal_render_prepare_snapshots(encoder);
    metal_render_lock(encoder->command);
    metal_render_prepare_draw(encoder,primitive);
    if (encoder->state->bound_index!=buffer->buffer || encoder->state->bound_index_offset!=offset || encoder->state->bound_index_type!=type) {
        VKD(vkCmdBindIndexBuffer)(encoder->state->render_command,buffer->buffer,offset,type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        encoder->state->bound_index=buffer->buffer; encoder->state->bound_index_offset=offset; encoder->state->bound_index_type=type;
    }
    VKD(vkCmdDrawIndexed)(encoder->state->render_command,(uint32_t)count,(uint32_t)instances,0,0,0);
    metal_color_history_written(encoder->state,count!=0 && instances!=0);
    metal_render_storage_written(encoder);
    metal_render_unlock(encoder->command); ++encoder->state->draws; ++encoder->command->recorded_operations;
    LOG_LIMITED(32,"Vulkan indexed draw recorded: %s indices=%zu instances=%zu",encoder->state->pipeline->vertex->spec->name,count,instances);
}
static void metal_render_draw_indexed(struct MetalRenderEncoder *encoder,const char *selector,Word primitive,size_t count,Word type,struct MetalBuffer *buffer,size_t offset) {
    metal_render_draw_indexed_instances(encoder,selector,primitive,count,type,buffer,offset,1);
}
static void metal_render_stencil(struct MetalRenderEncoder *encoder,const char *selector,uint32_t front,uint32_t back) {
    metal_render_check(encoder,selector); if (encoder->state->front_reference!=front || encoder->state->back_reference!=back) encoder->state->stencil_dirty=1;
    encoder->state->front_reference=front; encoder->state->back_reference=back;
}
static void metal_render_debug_push(struct MetalRenderEncoder *encoder,const char *selector,Word name) { metal_render_check(encoder,selector); if (!name) metal_render_stop("Nil render debug group",selector); ++encoder->state->debug_depth; }
static void metal_render_debug_pop(struct MetalRenderEncoder *encoder,const char *selector) { metal_render_check(encoder,selector); if (!encoder->state->debug_depth) metal_render_stop("Unbalanced render debug group",selector); --encoder->state->debug_depth; }
static void metal_render_cleanup(struct MetalCommand *command) {
    /* Command objects remain addressable by completion blocks; transient native
     * rendering objects are released only after their real GPU fence finishes. */
    for (struct MetalRenderEncoder *encoder=command->render_encoders;encoder;encoder=encoder->next) {
        metal_render_resolve_cleanup(encoder);
        if (encoder->state->pass_bundle) { metal_pass_bundle_return(encoder->state->pass_bundle); encoder->state->pass_bundle=NULL; }
        encoder->state->framebuffer=VK_NULL_HANDLE; encoder->state->first_pass=encoder->state->load_pass=VK_NULL_HANDLE;
        memset(encoder->state->views,0,sizeof(encoder->state->views));
        memset(encoder->state->descriptors,0,sizeof(encoder->state->descriptors));
        while (encoder->state->pools) { struct MetalDescriptorPoolChunk *chunk=encoder->state->pools; encoder->state->pools=chunk->next; metal_descriptor_pool_return(chunk); }
        metal_render_state_return(encoder->state); encoder->state=NULL;
    }
}
static void metal_snapshot_cleanup(struct MetalCommand *command) {
    while(command->snapshots) {
        struct MetalRenderSnapshot *snapshot=command->snapshots;command->snapshots=snapshot->next;
        metal_snapshot_return(snapshot);
    }
}
static void metal_render_release_encoders(struct MetalCommand *command) {
    while(command->render_encoders) {
        struct MetalRenderEncoder *encoder=command->render_encoders;
        command->render_encoders=encoder->next;metal_object_release((Word)encoder);
    }
}
#include "render_dynamic.inc"
static Method metal_render_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalCommandBuffer") && !strcmp(selector,"renderCommandEncoderWithDescriptor:")) return (Method)metal_render_encoder;
    if (strcmp(receiver,"_OBJC_CLASS_$_I2AMetalRenderCommandEncoder")) return NULL;
    if (!strcmp(selector,"device")) return (Method)metal_resource_device;
    if (!strcmp(selector,"label")) return (Method)metal_resource_label;
    if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
    Method dynamic=metal_render_dynamic_method(selector);
    if (dynamic) return dynamic;
    if (!strcmp(selector,"endEncoding")) return (Method)metal_render_end;
    if (!strcmp(selector,"setRenderPipelineState:")) return (Method)metal_render_pipeline;
    if (!strcmp(selector,"setDepthStencilState:") || !strcmp(selector,"setCullMode:") || !strcmp(selector,"setFrontFacingWinding:") || !strcmp(selector,"setTriangleFillMode:") || !strcmp(selector,"setDepthClipMode:")) return (Method)metal_render_state;
    if (!strcmp(selector,"setVertexBuffer:offset:atIndex:") || !strcmp(selector,"setFragmentBuffer:offset:atIndex:")) return (Method)metal_render_buffer;
    if (!strcmp(selector,"setVertexTexture:atIndex:") || !strcmp(selector,"setFragmentTexture:atIndex:")) return (Method)metal_render_texture;
    if (!strcmp(selector,"setVertexSamplerState:atIndex:") || !strcmp(selector,"setFragmentSamplerState:atIndex:")) return (Method)metal_render_sampler;
    if (!strcmp(selector,"setStencilFrontReferenceValue:backReferenceValue:")) return (Method)metal_render_stencil;
    if (!strcmp(selector,"drawPrimitives:vertexStart:vertexCount:")) return (Method)metal_render_draw;
    if (!strcmp(selector,"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:")) return (Method)metal_render_draw_indexed;
    if (!strcmp(selector,"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:")) return (Method)metal_render_draw_indexed_instances;
    if (!strcmp(selector,"pushDebugGroup:")) return (Method)metal_render_debug_push;
    if (!strcmp(selector,"popDebugGroup")) return (Method)metal_render_debug_pop;
    if (!strcmp(selector,"insertDebugSignpost:")) return (Method)noop;
    return NULL;
}
#undef VKD
