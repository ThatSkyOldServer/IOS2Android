 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalResource { Word isa,device,label,options; };
struct MetalSampler { struct MetalResource object; VkSampler sampler; VkSamplerCreateInfo description; struct MetalLifetime lifetime; };
struct MetalDepthState { struct MetalResource object; VkPipelineDepthStencilStateCreateInfo state; struct MetalLifetime lifetime; };
static void metal_sampler_destroy(Word value) {
    struct MetalSampler *sampler=(void *)value;
    if (sampler->sampler) VKD(vkDestroySampler)(vulkan.device,sampler->sampler,NULL);
    free(sampler);
}
static void metal_depth_destroy(Word value) { free((void *)value); }
struct MetalBuffer {
    struct MetalResource object;
    size_t length,native_length;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize allocation;
    VkMemoryPropertyFlags memory_flags;
    void *mapped;
    struct MetalLifetime lifetime;
};
static size_t metal_snapshot_trim(void);
static size_t metal_memory_reclaim(VkResult *);
static int metal_memory_type(uint32_t available,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) {
    int best=-1,score=-1;
    for (uint32_t i=0;i<vulkan.memory.memoryTypeCount;++i) {
        VkMemoryPropertyFlags flags=vulkan.memory.memoryTypes[i].propertyFlags;
        if (!(available&(1U<<i)) || (flags&required)!=required) continue;
        int rank=__builtin_popcount(flags&preferred);
        if (rank>score) { best=(int)i; score=rank; }
    }
    return best;
}
static void metal_buffer_dispose(struct MetalBuffer *buffer) {
    if (buffer->allocation) { metal_memory_sub(0,buffer->allocation); buffer->allocation=0; }
    if (buffer->mapped) VKD(vkUnmapMemory)(vulkan.device,buffer->memory);
    if (buffer->buffer) VKD(vkDestroyBuffer)(vulkan.device,buffer->buffer,NULL);
    if (buffer->memory) VKD(vkFreeMemory)(vulkan.device,buffer->memory,NULL);
    buffer->buffer=VK_NULL_HANDLE; buffer->memory=VK_NULL_HANDLE; buffer->mapped=NULL;
}
static void metal_buffer_destroy(Word value) {
    struct MetalBuffer *buffer=(void *)value;
    metal_buffer_dispose(buffer); metal_memory_sub(5,1); free(buffer);
}
static VkResult metal_buffer_allocate(struct MetalBuffer *buffer,size_t length,VkBufferUsageFlags usage,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) {
    uint64_t diagnostic_allocate=render_diagnostic_clock();
    if (length>SIZE_MAX-15) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
     
    size_t native_length=(length+15)&~(size_t)15;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=native_length,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkResult result=VKD(vkCreateBuffer)(vulkan.device,&info,NULL,&buffer->buffer);
    if (result!=VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    VKD(vkGetBufferMemoryRequirements)(vulkan.device,buffer->buffer,&requirements);
    int type=metal_memory_type(requirements.memoryTypeBits,required,preferred);
    if (type<0) { metal_buffer_dispose(buffer); return VK_ERROR_FEATURE_NOT_PRESENT; }
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=(uint32_t)type};
    result=VKD(vkAllocateMemory)(vulkan.device,&allocation,NULL,&buffer->memory);
    size_t reclaimed=0;VkResult recovery=VK_SUCCESS;
    if(result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        reclaimed=metal_memory_reclaim(&recovery);
        if(reclaimed && recovery==VK_SUCCESS)result=VKD(vkAllocateMemory)(vulkan.device,&allocation,NULL,&buffer->memory);
    }
    if(result!=VK_SUCCESS) {
        uint32_t heap=vulkan.memory.memoryTypes[type].heapIndex;
        char detail[320];snprintf(detail,sizeof(detail),"operation=vkAllocateMemory buffer result=%d required_bytes=%llu memory_type=%d heap=%u heap_bytes=%llu max_allocations=%u released_idle_bytes=%zu recovery_result=%d",
            result,(unsigned long long)requirements.size,type,heap,(unsigned long long)vulkan.memory.memoryHeaps[heap].size,
            vulkan.properties.limits.maxMemoryAllocationCount,reclaimed,recovery);
        record_failure("Vulkan buffer allocation failed",detail);
    }
    if (result==VK_SUCCESS) result=VKD(vkBindBufferMemory)(vulkan.device,buffer->buffer,buffer->memory,0);
    if (result==VK_SUCCESS && (required&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        result=VKD(vkMapMemory)(vulkan.device,buffer->memory,0,VK_WHOLE_SIZE,0,&buffer->mapped);
    if (result!=VK_SUCCESS) { metal_buffer_dispose(buffer); return result; }
    buffer->allocation=requirements.size; buffer->length=length; buffer->native_length=native_length;
    metal_memory_add(0,requirements.size);
    if (buffer->mapped && native_length>length) memset((unsigned char *)buffer->mapped+length,0,native_length-length);
    buffer->memory_flags=vulkan.memory.memoryTypes[type].propertyFlags;
    render_diagnostic_cost(11,diagnostic_allocate);
    return VK_SUCCESS;
}
/* Reuse mapped upload memory only after the real immediate-transfer fence has
 * completed. Borrowers own separate slots, so worker uploads cannot overwrite
 * bytes still in use by another command. The cache is bounded to 64 MiB. */
struct MetalStaging { struct MetalBuffer buffer; size_t capacity; int cached; Word destination; struct MetalStaging *next; };
static pthread_mutex_t metal_staging_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalStaging *metal_staging_free;
static size_t metal_staging_bytes; static unsigned metal_staging_count;
static struct MetalStaging *metal_staging_acquire(size_t length) {
    pthread_mutex_lock(&metal_staging_mutex);
    struct MetalStaging **best=NULL;
    size_t wanted=256; while (wanted<length && wanted<=SIZE_MAX/2) wanted*=2;
    for (struct MetalStaging **slot=&metal_staging_free;*slot;slot=&(*slot)->next)
        if ((*slot)->capacity>=length && (*slot)->capacity<=wanted*4 && (!best || (*slot)->capacity<(*best)->capacity)) best=slot;
    struct MetalStaging *result=best ? *best : NULL;
    if (result) { *best=result->next; result->next=NULL; }
    pthread_mutex_unlock(&metal_staging_mutex);
    if (!result) {
        result=calloc(1,sizeof(*result)); if (!result) return NULL;
        size_t capacity=256;
        while (capacity<length && capacity<=SIZE_MAX/2) capacity*=2;
        if (capacity<length || metal_buffer_allocate(&result->buffer,capacity,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,0)!=VK_SUCCESS) { free(result); return NULL; }
        result->capacity=capacity;
        pthread_mutex_lock(&metal_staging_mutex);
         
        while (metal_staging_free && metal_staging_bytes+result->buffer.allocation>64*1024*1024) {
            struct MetalStaging **largest=&metal_staging_free;
            for (struct MetalStaging **slot=&metal_staging_free;*slot;slot=&(*slot)->next) if ((*slot)->capacity>(*largest)->capacity) largest=slot;
            struct MetalStaging *retired=*largest; *largest=retired->next; --metal_staging_count; metal_staging_bytes-=retired->buffer.allocation;
            metal_buffer_dispose(&retired->buffer); free(retired);
        }
        if (capacity<=16*1024*1024 && metal_staging_count<512 && metal_staging_bytes+result->buffer.allocation<=64*1024*1024) {
            result->cached=1; ++metal_staging_count; metal_staging_bytes+=result->buffer.allocation;
        }
        pthread_mutex_unlock(&metal_staging_mutex);
    }
    result->buffer.length=length;
    return result;
}
static void metal_staging_release(struct MetalStaging *slot) {
    Word destination=slot->destination; slot->destination=0;
    metal_object_release(destination);
    if (!slot->cached) { metal_buffer_dispose(&slot->buffer); free(slot); return; }
    pthread_mutex_lock(&metal_staging_mutex); slot->next=metal_staging_free; metal_staging_free=slot; pthread_mutex_unlock(&metal_staging_mutex);
}
static size_t metal_staging_trim(void) {
    pthread_mutex_lock(&metal_staging_mutex);
    struct MetalStaging *entries=metal_staging_free;metal_staging_free=NULL;
    size_t bytes=0;
    for(struct MetalStaging *entry=entries;entry;entry=entry->next) {
        bytes+=entry->buffer.allocation;--metal_staging_count;metal_staging_bytes-=entry->buffer.allocation;
    }
    pthread_mutex_unlock(&metal_staging_mutex);
    while(entries) {struct MetalStaging *next=entries->next;metal_buffer_dispose(&entries->buffer);free(entries);entries=next;}
    return bytes;
}
/* Uploads only feed later GPU work. Record them together, submit before any
 * guest command, and retain staging bytes until the real upload fence signals.
 * Queue order plus the existing transfer barriers establish GPU visibility.
 * Eight batches bound work in flight; backpressure waits only at that limit. */
struct MetalUploadBatch { VkCommandPool pool; VkCommandBuffer command; VkFence fence; struct MetalStaging *staging; size_t bytes; unsigned operations,submitted; };
static struct { pthread_mutex_t mutex; struct MetalUploadBatch batches[8],*active; } metal_uploads={.mutex=PTHREAD_MUTEX_INITIALIZER};
static void metal_upload_retire(struct MetalUploadBatch *batch) {
    while (batch->staging) { struct MetalStaging *slot=batch->staging; batch->staging=slot->next; metal_staging_release(slot); }
    batch->submitted=batch->operations=0; batch->bytes=0;
}
static VkResult metal_upload_flush_locked(void) {
    struct MetalUploadBatch *batch=metal_uploads.active;
    if (!batch || !batch->operations) return VK_SUCCESS;
    VkResult result=VKD(vkEndCommandBuffer)(batch->command);
    if (result==VK_SUCCESS) {
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&batch->command};
        uint64_t lock=render_diagnostic_clock(); pthread_mutex_lock(&vulkan_mutex); render_diagnostic_cost(8,lock);
        result=VKD(vkQueueSubmit)(vulkan.queue,1,&submit,batch->fence); pthread_mutex_unlock(&vulkan_mutex);
    }
    metal_uploads.active=NULL;
    if (result==VK_SUCCESS) {
        batch->submitted=1;
        static unsigned logged;
        if (render_diagnostics_requested && __atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<16) LOG("[DIAG-render] Upload batch submitted: operations=%u bytes=%zu",batch->operations,batch->bytes);
    } else metal_upload_retire(batch);
    return result;
}
static VkResult metal_upload_begin_locked(void) {
    struct MetalUploadBatch *chosen=NULL;
    for (unsigned i=0;i<COUNT(metal_uploads.batches);++i) {
        struct MetalUploadBatch *batch=&metal_uploads.batches[i];
        if (batch->submitted) {
            VkResult status=VKD(vkGetFenceStatus)(vulkan.device,batch->fence);
            if (status==VK_SUCCESS) metal_upload_retire(batch);
            else if (status!=VK_NOT_READY) return status;
        }
        if (!batch->submitted && !chosen) chosen=batch;
    }
    if (!chosen) {
        chosen=&metal_uploads.batches[0];
        VkResult status=VKD(vkWaitForFences)(vulkan.device,1,&chosen->fence,VK_TRUE,UINT64_MAX);
        if (status!=VK_SUCCESS) return status;
        metal_upload_retire(chosen);
    }
    VkResult result=VK_SUCCESS;
    if (!chosen->pool) {
        VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=vulkan.queue_family};
        result=VKD(vkCreateCommandPool)(vulkan.device,&pool,NULL,&chosen->pool);
        VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=chosen->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        if (result==VK_SUCCESS) result=VKD(vkAllocateCommandBuffers)(vulkan.device,&allocate,&chosen->command);
        VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (result==VK_SUCCESS) result=VKD(vkCreateFence)(vulkan.device,&fence,NULL,&chosen->fence);
        if (result!=VK_SUCCESS) {
            if (chosen->pool) VKD(vkDestroyCommandPool)(vulkan.device,chosen->pool,NULL);
            chosen->pool=VK_NULL_HANDLE; chosen->command=VK_NULL_HANDLE; return result;
        }
    }
    result=VKD(vkResetCommandPool)(vulkan.device,chosen->pool,0);
    if (result==VK_SUCCESS) result=VKD(vkResetFences)(vulkan.device,1,&chosen->fence);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (result==VK_SUCCESS) result=VKD(vkBeginCommandBuffer)(chosen->command,&begin);
    if (result==VK_SUCCESS) metal_uploads.active=chosen;
    return result;
}
static VkResult metal_upload_record(void (*record)(VkCommandBuffer,void *),void *context,struct MetalStaging *slot,Word destination) {
    uint64_t diagnostic=render_diagnostic_clock();
    /* The existing upload-fence retirement owns both the staging bytes and the
     * destination. Reuse the slot's storage: no extra allocation per upload. */
    slot->destination=metal_object_retain(destination);
    pthread_mutex_lock(&metal_uploads.mutex);
    VkResult result=metal_uploads.active ? VK_SUCCESS : metal_upload_begin_locked();
    if (result==VK_SUCCESS) {
        struct MetalUploadBatch *batch=metal_uploads.active;
        record(batch->command,context);
        slot->next=batch->staging; batch->staging=slot; batch->bytes+=slot->buffer.length; ++batch->operations;
        if (batch->operations>=64 || batch->bytes>=8*1024*1024) result=metal_upload_flush_locked();
    } else metal_staging_release(slot);
    pthread_mutex_unlock(&metal_uploads.mutex); render_diagnostic_cost(7,diagnostic); return result;
}
static VkResult metal_upload_flush(void) {
    uint64_t diagnostic=render_diagnostic_clock();
    pthread_mutex_lock(&metal_uploads.mutex); VkResult result=metal_upload_flush_locked(); pthread_mutex_unlock(&metal_uploads.mutex);
    render_diagnostic_cost(7,diagnostic); return result;
}
static size_t metal_memory_reclaim(VkResult *result) {
    size_t bytes=0;
    pthread_mutex_lock(&metal_uploads.mutex);
    *result=metal_upload_flush_locked();
    VkFence fences[COUNT(metal_uploads.batches)];unsigned count=0;
    for(unsigned i=0;i<COUNT(metal_uploads.batches);++i)
        if(metal_uploads.batches[i].submitted)fences[count++]=metal_uploads.batches[i].fence;
    if(*result==VK_SUCCESS && count)*result=VKD(vkWaitForFences)(vulkan.device,count,fences,VK_TRUE,1000000000ULL);
    if(*result==VK_SUCCESS)for(unsigned i=0;i<COUNT(metal_uploads.batches);++i) {
        struct MetalUploadBatch *batch=&metal_uploads.batches[i];if(!batch->submitted)continue;
        for(struct MetalStaging *slot=batch->staging;slot;slot=slot->next)if(!slot->cached)bytes+=slot->buffer.allocation;
        metal_upload_retire(batch);
    }
    pthread_mutex_unlock(&metal_uploads.mutex);
    return bytes+metal_staging_trim()+metal_snapshot_trim();
}
struct MetalBufferCopy { struct MetalBuffer *from,*to; };
static void metal_record_buffer_copy(VkCommandBuffer command,void *context) {
    struct MetalBufferCopy *copy=context;
    VkBufferCopy region={.size=copy->from->length};
    VKD(vkCmdCopyBuffer)(command,copy->from->buffer,copy->to->buffer,1,&region);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=copy->to->buffer,.size=VK_WHOLE_SIZE};
    VKD(vkCmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,1,&barrier,0,NULL);
}
static VkResult metal_copy_buffer_upload(struct MetalStaging *slot,struct MetalBuffer *to) {
    struct MetalBufferCopy copy={&slot->buffer,to}; return metal_upload_record(metal_record_buffer_copy,&copy,slot,(Word)to);
}
static Word metal_buffer_new(Word device,const char *selector,size_t length,Word options) {
    unsigned storage=(options>>4)&15,cache=options&15,hazards=(options>>8)&3;
    if (!vulkan.available) stop("Buffer backend requires Vulkan",selector);
    if (!length || (options&~0x3ffUL) || storage>2 || cache>1 || hazards>2) {
        LOG("Invalid Metal buffer request: length=%zu options=0x%lx",length,options); return 0;
    }
    struct MetalBuffer *buffer=calloc(1,sizeof(*buffer)); if (!buffer) return 0;
    buffer->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalBuffer"),.device=device,.options=options};
    VkBufferUsageFlags usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkMemoryPropertyFlags required=storage==2 ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags preferred=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (storage!=2 && cache==0) preferred|=VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkResult result=metal_buffer_allocate(buffer,length,usage,required,preferred);
    if (result!=VK_SUCCESS) { LOG("Vulkan buffer allocation failed: %d length=%zu options=0x%lx",result,length,options); free(buffer); return 0; }
    metal_memory_add(5,1);
    metal_lifetime_init((Word)buffer,__builtin_offsetof(struct MetalBuffer,lifetime),metal_buffer_destroy);
    LOG_LIMITED(24,"Vulkan Metal buffer: %zu bytes storage=%u mapped=%d memoryFlags=0x%x",length,storage,buffer->mapped!=NULL,buffer->memory_flags);
    return (Word)buffer;
}
static Word metal_buffer_bytes(Word device,const char *selector,const void *bytes,size_t length,Word options) {
    if (!bytes && length) return 0;
    struct MetalBuffer *buffer=(void *)metal_buffer_new(device,selector,length,options);
    if (!buffer) return 0;
    if (buffer->mapped) memcpy(buffer->mapped,bytes,length);
    else {
        struct MetalStaging *slot=metal_staging_acquire(length);
        VkResult result=slot ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
        if (slot) { memcpy(slot->buffer.mapped,bytes,length); result=metal_copy_buffer_upload(slot,buffer); }
        if (result!=VK_SUCCESS) { LOG("Vulkan initial buffer upload failed: %d",result); metal_object_release((Word)buffer); return 0; }
    }
    return (Word)buffer;
}
static Word metal_resource_device(struct MetalResource *object) { return object->device; }
static Word metal_resource_label(struct MetalResource *object) { return object->label; }
static void metal_resource_set_label(struct MetalResource *object,const char *selector,Word label) { object->label=label ? text_copy(label,"copy") : 0; }
static Word metal_buffer_contents(struct MetalBuffer *buffer) { return (Word)buffer->mapped; }
static Word metal_buffer_length(struct MetalBuffer *buffer) { return buffer->length; }
static Word metal_buffer_property(struct MetalBuffer *buffer,const char *selector) {
    if (!strcmp(selector,"storageMode")) return (buffer->object.options>>4)&15;
    if (!strcmp(selector,"cpuCacheMode")) return buffer->object.options&15;
    if (!strcmp(selector,"hazardTrackingMode")) return (buffer->object.options>>8)&3;
    return buffer->allocation;
}
static void metal_buffer_did_modify(struct MetalBuffer *buffer,const char *selector,struct GuestRange range) {
    if (!buffer->mapped || range.location>buffer->length || range.length>buffer->length-range.location) stop("Invalid Metal buffer modification",selector);
     
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static int metal_sampler_address(Word mode,VkSamplerAddressMode *output) {
    switch (mode) {
    case 0: *output=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return 1;
    case 2: *output=VK_SAMPLER_ADDRESS_MODE_REPEAT; return 1;
    case 3: *output=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return 1;
    case 4: case 5: *output=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return 1;
    default: return 0;
    }
}
static Word metal_sampler_new(Word device,const char *selector,struct MetalDescriptor *descriptor) {
    if (!descriptor || descriptor->kind!=MD_SAMPLER) return 0;
    if (!vulkan.available) stop("Sampler backend requires Vulkan",selector);
    Word min=metal_property(descriptor,"minFilter"),mag=metal_property(descriptor,"magFilter"),mip=metal_property(descriptor,"mipFilter");
    Word anisotropy=metal_property(descriptor,"maxAnisotropy"),compare=metal_property(descriptor,"compareFunction");
    Word normalized=metal_property(descriptor,"normalizedCoordinates");
    VkSamplerCreateInfo info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.minFilter=(VkFilter)min,.magFilter=(VkFilter)mag,
        .mipmapMode=mip==2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .minLod=mip ? descriptor->lod_min : 0,.maxLod=mip ? descriptor->lod_max : 0,
        .compareEnable=compare!=7,.compareOp=(VkCompareOp)compare,.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates=!normalized,.maxAnisotropy=(float)anisotropy,.anisotropyEnable=anisotropy>1};
    if (min>1 || mag>1 || mip>2 || compare>7 || anisotropy<1 || anisotropy>16 || !isfinite(info.minLod) || !isfinite(info.maxLod) || info.maxLod<info.minLod ||
        !metal_sampler_address(metal_property(descriptor,"sAddressMode"),&info.addressModeU) ||
        !metal_sampler_address(metal_property(descriptor,"tAddressMode"),&info.addressModeV) ||
        !metal_sampler_address(metal_property(descriptor,"rAddressMode"),&info.addressModeW)) {
        LOG("Invalid/unsupported Metal sampler descriptor: min=%lu mag=%lu mip=%lu aniso=%lu compare=%lu normalized=%lu lod=%g..%g address=%lu/%lu/%lu",
            min,mag,mip,anisotropy,compare,normalized,info.minLod,info.maxLod,metal_property(descriptor,"sAddressMode"),metal_property(descriptor,"tAddressMode"),metal_property(descriptor,"rAddressMode")); return 0;
    }
    if (anisotropy>1 && (!vulkan.features.samplerAnisotropy || anisotropy>vulkan.properties.limits.maxSamplerAnisotropy)) {
        LOG("Requested sampler anisotropy %lu exceeds Vulkan support",anisotropy); return 0;
    }
    if (!normalized && (min!=mag || mip!=0 || anisotropy>1 || info.compareEnable ||
        (info.addressModeU!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && info.addressModeU!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ||
        (info.addressModeV!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && info.addressModeV!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER))) return 0;
    struct MetalSampler *sampler=calloc(1,sizeof(*sampler)); if (!sampler) return 0;
    sampler->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalSamplerState"),.device=device,.label=metal_property(descriptor,"label")};
    sampler->description=info;
    VkResult result=VKD(vkCreateSampler)(vulkan.device,&info,NULL,&sampler->sampler);
    if (result!=VK_SUCCESS) { LOG("Vulkan sampler creation failed: %d",result); free(sampler); return 0; }
    metal_lifetime_init((Word)sampler,__builtin_offsetof(struct MetalSampler,lifetime),metal_sampler_destroy);
    LOG_LIMITED(12,"Vulkan sampler created: min=%lu mag=%lu mip=%lu anisotropy=%lu",min,mag,mip,anisotropy);
    return (Word)sampler;
}
static VkStencilOpState metal_stencil_state(struct MetalDescriptor *descriptor) {
    VkStencilOpState state={.compareOp=VK_COMPARE_OP_ALWAYS,.compareMask=UINT32_MAX,.writeMask=UINT32_MAX};
    if (!descriptor) return state;
    Word fail=metal_property(descriptor,"stencilFailureOperation"),depth_fail=metal_property(descriptor,"depthFailureOperation"),pass=metal_property(descriptor,"depthStencilPassOperation"),compare=metal_property(descriptor,"stencilCompareFunction");
    if (fail>7 || depth_fail>7 || pass>7 || compare>7) stop("Invalid stencil descriptor","newDepthStencilStateWithDescriptor:");
    state.failOp=(VkStencilOp)fail; state.depthFailOp=(VkStencilOp)depth_fail; state.passOp=(VkStencilOp)pass; state.compareOp=(VkCompareOp)compare;
    state.compareMask=(uint32_t)metal_property(descriptor,"readMask"); state.writeMask=(uint32_t)metal_property(descriptor,"writeMask");
    return state;
}
static Word metal_depth_new(Word device,const char *selector,struct MetalDescriptor *descriptor) {
    if (!descriptor || descriptor->kind!=MD_DEPTH) return 0;
    Word compare=metal_property(descriptor,"depthCompareFunction"),write=metal_property(descriptor,"depthWriteEnabled");
    if (compare>7) return 0;
    struct MetalDepthState *state=calloc(1,sizeof(*state)); if (!state) return 0;
    state->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalDepthStencilState"),.device=device,.label=metal_property(descriptor,"label")};
    state->state=(VkPipelineDepthStencilStateCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=compare!=7 || write,.depthWriteEnable=write!=0,.depthCompareOp=(VkCompareOp)compare,
        .stencilTestEnable=descriptor->front || descriptor->back,.minDepthBounds=0,.maxDepthBounds=1,
        .front=metal_stencil_state((void *)descriptor->front),.back=metal_stencil_state((void *)descriptor->back)};
    metal_lifetime_init((Word)state,__builtin_offsetof(struct MetalDepthState,lifetime),metal_depth_destroy);
    return (Word)state;
}
static Method metal_resources_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice")) {
        if (!strcmp(selector,"newSamplerStateWithDescriptor:")) return (Method)metal_sampler_new;
        if (!strcmp(selector,"newDepthStencilStateWithDescriptor:")) return (Method)metal_depth_new;
        if (!strcmp(selector,"newBufferWithLength:options:")) return (Method)metal_buffer_new;
        if (!strcmp(selector,"newBufferWithBytes:length:options:")) return (Method)metal_buffer_bytes;
    }
    int buffer=!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalBuffer");
    if (buffer || !strcmp(receiver,"_OBJC_CLASS_$_I2AMetalSamplerState") || !strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDepthStencilState")) {
        if (!strcmp(selector,"device")) return (Method)metal_resource_device;
        if (!strcmp(selector,"label")) return (Method)metal_resource_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
        if (buffer) {
            if (!strcmp(selector,"contents")) return (Method)metal_buffer_contents;
            if (!strcmp(selector,"length")) return (Method)metal_buffer_length;
            if (!strcmp(selector,"storageMode") || !strcmp(selector,"cpuCacheMode") || !strcmp(selector,"hazardTrackingMode") || !strcmp(selector,"allocatedSize")) return (Method)metal_buffer_property;
            if (!strcmp(selector,"didModifyRange:")) return (Method)metal_buffer_did_modify;
        }
    }
    return NULL;
}
#undef VKD
