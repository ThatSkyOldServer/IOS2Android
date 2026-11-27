 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalCommandResource { Word object; size_t slot; };
struct MetalCommandPool {
    VkCommandPool native; VkCommandBuffer render,prologue; VkFence fence; VkQueryPool timestamps,gpu_timestamps;
    pthread_mutex_t mutex; struct MetalCommandPool *next;
    struct MetalCommandResource *resources;
    struct MetalBuffer *math_diagnostics;
    size_t *resource_buckets,resource_capacity,resource_count;
    VkCommandBuffer *render_segments;
    unsigned render_segment_count,render_segment_used;
#if I2A_DEVELOPMENT
    VkQueryPool pass_timestamps;
#endif
};
struct MetalQueue {
    struct HostObject object;
    pthread_mutex_t mutex;
    VkCommandPool pool;
    Word submission_queue,completion_queue;
    struct Sequence *sample_handlers;
    size_t max_buffers;
    int background_pid,skip_render,execution_enabled,profiling_enabled;
    unsigned stat_enabled;
    Word stat_options,stat_locations;
    struct MetalCommandPool *free_pools;
    unsigned pool_count;
    struct MetalCommand *pending_head,*pending_tail;
    unsigned draining;
    struct MetalLifetime lifetime;
};
struct MetalCommand {
    struct HostObject object;
    VkCommandBuffer command,prologue;
    struct MetalCommandPool *pool_owner;
    unsigned retained_references;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    VkFence fence;
    Word error,active_encoder;
    unsigned gpu_done,handlers_done,callback_failed,recorded_operations;
    struct MetalCommandTexture *textures,*texture_buckets[64];
    struct MetalCommandHandler *scheduled,*completed;
    struct MetalRenderEncoder *render_encoders;
    struct MetalComputeEncoder *compute_encoders;
    struct MetalRenderSnapshot *snapshots;
    struct MetalDrawable *drawable;
    struct MetalDiagnosticCapture *captures;
    struct MetalBuffer *math_diagnostics;
    unsigned diagnostic_timestamps;
    unsigned gpu_timestamps,gpu_time_valid;
    double gpu_start,gpu_end;
#if I2A_DEVELOPMENT
    unsigned diagnostic_pass_enabled,diagnostic_pass_count,diagnostic_pass_overflow;
    uint64_t diagnostic_present_generation;
    unsigned diagnostic_submission_index;
    struct MetalPassTiming diagnostic_pass[METAL_DIAGNOSTIC_PASSES];
#endif
    struct MetalCommand *queue_previous,*queue_next;
    unsigned queued;
    struct MetalLifetime lifetime;
};
#include "gpu_timing.inc"
static void metal_command_destroy(Word);
static void metal_queue_destroy(Word);
static void metal_buffer_dispose(struct MetalBuffer *);
static void metal_queue_drain(struct MetalQueue *);
static void metal_queue_unlink_locked(struct MetalQueue *queue,struct MetalCommand *command) {
    if (!command->queued) return;
    if (command->queue_previous) command->queue_previous->queue_next=command->queue_next;
    else queue->pending_head=command->queue_next;
    if (command->queue_next) command->queue_next->queue_previous=command->queue_previous;
    else queue->pending_tail=command->queue_previous;
    command->queue_previous=command->queue_next=NULL; command->queued=0;
}
/* The caller holds the command mutex. An uncommitted reservation is removed
 * by its destructor; commit's existing completion reference pins ready work. */
static void metal_enqueue_locked(struct MetalCommand *command) {
    struct MetalQueue *queue=(void *)command->object.content[0];
    pthread_mutex_lock(&queue->mutex);
    command->queue_previous=queue->pending_tail; command->queued=1;
    if (queue->pending_tail) queue->pending_tail->queue_next=command;
    else queue->pending_head=command;
    queue->pending_tail=command;
    __atomic_store_n(&command->object.content[1],1,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&queue->mutex);
}
static pthread_once_t metal_completion_once=PTHREAD_ONCE_INIT;
static void *metal_completion_queue;
static void (*metal_completion_enqueue)(void *,void *,void (*)(void *));
static void metal_completion_initialize(void) {
    void *(*global_queue)(long,unsigned long)=dispatch_export("dispatch_get_global_queue");
    metal_completion_enqueue=dispatch_export("dispatch_async_f");
    if(global_queue && metal_completion_enqueue)metal_completion_queue=global_queue(0,0);
}
static size_t metal_command_resource_hash(Word object,size_t capacity) {
    return ((object>>4)^(object>>17))*2654435761U&(capacity-1);
}
static void metal_command_hold(struct MetalCommand *command,Word object) {
    if (!object) return;
    /* A recording pool has exactly one command owner. Offset-only rebinding
     * skips this call; repeated resources probe in O(1), retain only once.
     * Warmed-up pools reuse both arrays instead of allocating per draw/frame. */
    struct MetalCommandPool *pool=command->pool_owner;
    if (pool->resource_capacity) {
        size_t slot=metal_command_resource_hash(object,pool->resource_capacity);
        while (pool->resource_buckets[slot]) {
            if (pool->resources[pool->resource_buckets[slot]-1].object==object) return;
            slot=(slot+1)&(pool->resource_capacity-1);
        }
    }
    if (pool->resource_count>=pool->resource_capacity/2) {
        size_t old=pool->resource_capacity,capacity=old ? old*2 : 64;
        if (capacity<old || capacity>SIZE_MAX/sizeof(size_t) || capacity/2>SIZE_MAX/sizeof(*pool->resources))
            stop("Metal command ownership set too large","recording");
        size_t *buckets=calloc(capacity,sizeof(*buckets));
        struct MetalCommandResource *resources=realloc(pool->resources,capacity/2*sizeof(*resources));
        if (!buckets || !resources) {
            free(buckets); if (resources) pool->resources=resources;
            stop("Allocation failed","Metal command ownership set");
        }
        pool->resources=resources;
        for (size_t i=0;i<pool->resource_count;++i) {
            size_t slot=metal_command_resource_hash(resources[i].object,capacity);
            while (buckets[slot]) slot=(slot+1)&(capacity-1);
            buckets[slot]=i+1; resources[i].slot=slot;
        }
        free(pool->resource_buckets); pool->resource_buckets=buckets; pool->resource_capacity=capacity;
        metal_memory_add(3,(capacity-old)*sizeof(*buckets)+(capacity-old)/2*sizeof(*resources));
    }
    size_t slot=metal_command_resource_hash(object,pool->resource_capacity);
    while (pool->resource_buckets[slot]) slot=(slot+1)&(pool->resource_capacity-1);
    size_t index=pool->resource_count++;
    pool->resources[index]=(struct MetalCommandResource){metal_object_retain(object),slot};
    pool->resource_buckets[slot]=index+1;
}
static void metal_command_use(struct MetalCommand *command,Word object) {
    if (command->retained_references) metal_command_hold(command,object);
}
static void metal_command_release_resources(struct MetalCommand *command) {
    struct MetalCommandPool *pool=command->pool_owner;
    /* Visit only this command's distinct resources, not the allocated table
     * capacity or any global registry. The real GPU fence has already passed. */
    while (pool->resource_count) {
        struct MetalCommandResource *use=&pool->resources[--pool->resource_count];
        pool->resource_buckets[use->slot]=0;
        Word object=use->object; use->object=0; metal_object_release(object);
    }
}
static Word metal_queue_create(Word device,size_t maximum) {
    if (!maximum) return 0;
    struct MetalQueue *queue=calloc(1,sizeof(*queue));
    if (!queue) return 0;
    queue->object.isa=external_class("_OBJC_CLASS_$_I2AMetalCommandQueue");
    queue->object.content[0]=device; queue->max_buffers=maximum;
    queue->background_pid=getpid(); queue->execution_enabled=1;
    pthread_mutex_init(&queue->mutex,NULL);
    queue->sample_handlers=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    if (vulkan.available) {
        pthread_once(&metal_completion_once,metal_completion_initialize);
        if(!metal_completion_queue || !metal_completion_enqueue)stop("Metal completion queue unavailable","libdispatch");
        VkCommandPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=vulkan.queue_family};
        PFN_vkCreateCommandPool create=(PFN_vkCreateCommandPool)vulkan.get_device_proc(vulkan.device,"vkCreateCommandPool");
        VkResult result=create(vulkan.device,&info,NULL,&queue->pool);
        if (result!=VK_SUCCESS) {
            LOG("Vulkan command pool creation failed: %d",result);
            pthread_mutex_destroy(&queue->mutex); free(queue->sample_handlers->items); free(queue->sample_handlers); free(queue); return 0;
        }
        LOG("Vulkan command queue pool created (family=%u)",vulkan.queue_family);
        struct MetalCommandPool *pool=calloc(1,sizeof(*pool));
        if (!pool) stop("Allocation failed","Metal command pool ownership");
        pool->native=queue->pool; pthread_mutex_init(&pool->mutex,NULL); queue->free_pools=pool; queue->pool_count=1;
    }
    metal_lifetime_init((Word)queue,__builtin_offsetof(struct MetalQueue,lifetime),metal_queue_destroy);
    return (Word)queue;
}
static Word metal_new_queue(Word device) { return metal_queue_create(device,64); }
static Word metal_new_queue_limit(Word device,const char *selector,size_t count) { return metal_queue_create(device,count); }
static Word metal_queue_device(struct HostObject *queue) { return queue->content[0]; }
static struct MetalCommandPool *metal_pool_acquire(struct MetalQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    struct MetalCommandPool *pool=queue->free_pools;
    if (pool) queue->free_pools=pool->next;
    pthread_mutex_unlock(&queue->mutex);
    if (pool) { pool->next=NULL; return pool; }
    pool=calloc(1,sizeof(*pool)); if (!pool) return NULL;
    VkCommandPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=vulkan.queue_family};
    PFN_vkCreateCommandPool create=(PFN_vkCreateCommandPool)vulkan.get_device_proc(vulkan.device,"vkCreateCommandPool");
    if (create(vulkan.device,&info,NULL,&pool->native)!=VK_SUCCESS) { free(pool); return NULL; }
    pthread_mutex_init(&pool->mutex,NULL);
    pthread_mutex_lock(&queue->mutex); unsigned count=++queue->pool_count; pthread_mutex_unlock(&queue->mutex);
    if (count<=16) LOG("Vulkan independent recording pool cached: %u",count);
    return pool;
}
static void metal_pool_return(struct MetalQueue *queue,struct MetalCommandPool *pool) {
    pool->render_segment_used=0;
    pthread_mutex_lock(&queue->mutex); pool->next=queue->free_pools; queue->free_pools=pool; pthread_mutex_unlock(&queue->mutex);
}
static void metal_pool_destroy(struct MetalQueue *queue,struct MetalCommandPool *pool) {
    if(pool->timestamps)VKD(vkDestroyQueryPool)(vulkan.device,pool->timestamps,NULL);
    if(pool->gpu_timestamps)VKD(vkDestroyQueryPool)(vulkan.device,pool->gpu_timestamps,NULL);
#if I2A_DEVELOPMENT
    if(pool->pass_timestamps)VKD(vkDestroyQueryPool)(vulkan.device,pool->pass_timestamps,NULL);
#endif
    if(pool->fence)VKD(vkDestroyFence)(vulkan.device,pool->fence,NULL);
    VKD(vkDestroyCommandPool)(vulkan.device,pool->native,NULL);
    metal_memory_sub(3,pool->render_segment_count*sizeof(*pool->render_segments));free(pool->render_segments);
    if(pool->math_diagnostics){metal_buffer_dispose(pool->math_diagnostics);free(pool->math_diagnostics);}
    metal_memory_sub(3,pool->resource_capacity*(sizeof(*pool->resource_buckets)+sizeof(*pool->resources)/2));
    free(pool->resource_buckets);free(pool->resources);
    pthread_mutex_lock(&queue->mutex);
    if(queue->pool==pool->native)queue->pool=VK_NULL_HANDLE;
    if(queue->pool_count)--queue->pool_count;
    pthread_mutex_unlock(&queue->mutex);
    pthread_mutex_destroy(&pool->mutex);free(pool);
}
static void metal_queue_destroy(Word value) {
    struct MetalQueue *queue=(void *)value;
    /* Every live command retains its queue, including pending reservations and
     * completion work. The final release therefore owns only idle pools. */
    while (queue->free_pools) {
        struct MetalCommandPool *pool=queue->free_pools;queue->free_pools=pool->next;
        metal_pool_destroy(queue,pool);
    }
    for (size_t i=0;queue->sample_handlers && i<queue->sample_handlers->count;++i)
        _Block_release((void *)queue->sample_handlers->items[i]);
    if (queue->sample_handlers) { free(queue->sample_handlers->items);free(queue->sample_handlers); }
    void (*release)(void *)=dispatch_export("dispatch_release");
    if (release) {
        if (queue->submission_queue) release((void *)queue->submission_queue);
        if (queue->completion_queue) release((void *)queue->completion_queue);
    }
    metal_object_release(queue->object.content[1]);
    pthread_mutex_destroy(&queue->mutex);free(queue);
}
static Word metal_command_buffer(struct MetalQueue *queue,const char *selector) {
    uint64_t diagnostic_begin=render_diagnostic_clock();
    struct MetalCommand *buffer=calloc(1,sizeof(*buffer));
    if (!buffer) return 0;
    buffer->object.isa=external_class("_OBJC_CLASS_$_I2AMetalCommandBuffer");
    buffer->object.content[0]=metal_object_retain((Word)queue);
    buffer->retained_references=strcmp(selector,"commandBufferWithUnretainedReferences")!=0;
    pthread_mutex_init(&buffer->mutex,NULL); pthread_cond_init(&buffer->condition,NULL);
    if (vulkan.available) {
        buffer->pool_owner=metal_pool_acquire(queue);
        if (!buffer->pool_owner) { metal_object_release((Word)queue);pthread_cond_destroy(&buffer->condition);pthread_mutex_destroy(&buffer->mutex);free(buffer);return 0; }
        VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=buffer->pool_owner->native,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VkCommandBufferBeginInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        pthread_mutex_lock(&buffer->pool_owner->mutex);
        buffer->command=buffer->pool_owner->render;
        VkResult result=buffer->command ? VK_SUCCESS : VKD(vkAllocateCommandBuffers)(vulkan.device,&allocate,&buffer->command);
        if (result==VK_SUCCESS) buffer->pool_owner->render=buffer->command;
        if (result==VK_SUCCESS) result=VKD(vkBeginCommandBuffer)(buffer->command,&info);
        if (result==VK_SUCCESS) {metal_gpu_time_begin(buffer);metal_diagnostic_command_begin(buffer);}
        if (result!=VK_SUCCESS && buffer->command) { VKD(vkFreeCommandBuffers)(vulkan.device,buffer->pool_owner->native,1,&buffer->command); buffer->pool_owner->render=VK_NULL_HANDLE; }
        pthread_mutex_unlock(&buffer->pool_owner->mutex);
        if (result!=VK_SUCCESS) { metal_pool_return(queue,buffer->pool_owner); LOG("Vulkan command buffer allocation/begin failed: %d",result);metal_object_release((Word)queue);pthread_cond_destroy(&buffer->condition);pthread_mutex_destroy(&buffer->mutex);free(buffer);return 0; }
    }
    metal_lifetime_init((Word)buffer,__builtin_offsetof(struct MetalCommand,lifetime),metal_command_destroy);
    metal_memory_add(4,sizeof(*buffer));
    render_diagnostic_cost(12,diagnostic_begin);return metal_object_autorelease((Word)buffer);
}
static Word metal_command_queue(struct HostObject *buffer) { return buffer->content[0]; }
static Word metal_command_device(struct HostObject *buffer) { return metal_queue_device((void *)buffer->content[0]); }
static Word metal_command_status(struct HostObject *buffer) { return __atomic_load_n(&buffer->content[1],__ATOMIC_ACQUIRE); }
static Word metal_command_retains(struct MetalCommand *buffer) { return buffer->retained_references; }
static void metal_enqueue(struct MetalCommand *buffer,const char *selector) {
    pthread_mutex_lock(&buffer->mutex);
    Word status=buffer->object.content[1];
    if (!status) metal_enqueue_locked(buffer);
    pthread_mutex_unlock(&buffer->mutex);
    if (status>=2) stop("Cannot enqueue a committed command buffer",selector);
}
static Word metal_queue_label(struct HostObject *queue) { return queue->content[1]; }
static void metal_queue_set_label(struct HostObject *queue,const char *selector,Word label) { queue->content[1]=label ? text_copy(label,"copy") : 0; }
static Word metal_command_label(struct HostObject *buffer) { return buffer->content[2]; }
static void metal_command_set_label(struct HostObject *buffer,const char *selector,Word label) { buffer->content[2]=label ? text_copy(label,"copy") : 0; }
static Word metal_queue_conforms(Word object,const char *selector,Word protocol) {
    if (in_image(protocol) && in_image(read_word(protocol+8))) {
        const char *name=(const char *)read_word(protocol+8);
        if (!strcmp(name,"MTLCommandQueue") || !strcmp(name,"MTLCommandQueueSPI") || !strcmp(name,"NSObject")) return 1;
    }
    return conforms_protocol(object,selector,protocol);
}
static void metal_add_perf_handler(struct MetalQueue *queue,const char *selector,Word block) {
    if (!block) stop("Nil Metal performance callback",selector);
    Word copy=(Word)_Block_copy((void *)block);
    if (!copy) stop("Allocation failed",selector);
    pthread_mutex_lock(&queue->mutex);
    sequence_add(queue->sample_handlers,selector,copy);
    pthread_mutex_unlock(&queue->mutex);
    LOG("Original Metal performance sample handler retained; delivery awaits measured GPU samples");
}
 
static Word metal_queue_priority(void) { return 0; }
static Word metal_queue_set_priority(Word object,const char *selector,Word priority) { return priority==0; }
static Word metal_queue_set_priority_offset(Word object,const char *selector,Word priority,unsigned short offset) { return priority==0 && offset==0; }
static void metal_queue_dispatch(struct MetalQueue *queue,const char *selector,Word value) {
    void (*retain)(void *)=dispatch_export("dispatch_retain");
    void (*release)(void *)=dispatch_export("dispatch_release");
    if (value) retain((void *)value);
    pthread_mutex_lock(&queue->mutex);
    Word *slot=!strcmp(selector,"setSubmissionQueue:") ? &queue->submission_queue : &queue->completion_queue;
    Word previous=*slot; *slot=value;
    pthread_mutex_unlock(&queue->mutex);
    if (previous) release((void *)previous);
}
static Word metal_queue_qos(struct MetalQueue *queue,const char *selector) {
    unsigned (*qos)(void *,int *)=dispatch_export("dispatch_queue_get_qos_class");
    int relative=0; unsigned quality=0;
    pthread_mutex_lock(&queue->mutex);
    if (queue->submission_queue) quality=qos((void *)queue->submission_queue,&relative);
    pthread_mutex_unlock(&queue->mutex);
    return !strcmp(selector,"qosRelativePriority") ? (Word)(intptr_t)relative : quality;
}
static Word metal_queue_property(struct MetalQueue *queue,const char *selector) {
    pthread_mutex_lock(&queue->mutex);
    Word value=!strcmp(selector,"backgroundTrackingPID") ? (Word)queue->background_pid :
        !strcmp(selector,"maxCommandBufferCount") ? queue->max_buffers :
        !strcmp(selector,"skipRender") ? queue->skip_render :
        !strcmp(selector,"executionEnabled") ? queue->execution_enabled : queue->profiling_enabled;
    pthread_mutex_unlock(&queue->mutex); return value;
}
static void metal_queue_set_property(struct MetalQueue *queue,const char *selector,Word value) {
    pthread_mutex_lock(&queue->mutex);
    if (!strcmp(selector,"setBackgroundTrackingPID:")) queue->background_pid=(int)value;
    else if (!strcmp(selector,"setSkipRender:")) queue->skip_render=(unsigned char)value!=0;
    else if (!strcmp(selector,"setExecutionEnabled:")) queue->execution_enabled=(unsigned char)value!=0;
    else queue->profiling_enabled=(unsigned char)value!=0;
    pthread_mutex_unlock(&queue->mutex);
}
 
static Word metal_queue_stat(struct MetalQueue *queue,const char *selector) {
    pthread_mutex_lock(&queue->mutex);
    Word value=!strcmp(selector,"isStatEnabled") ? queue->stat_enabled :
        (!strcmp(selector,"StatOptions") || !strcmp(selector,"getStatOptions")) ? queue->stat_options : queue->stat_locations;
    pthread_mutex_unlock(&queue->mutex); return value;
}
static void metal_queue_set_stat(struct MetalQueue *queue,const char *selector,Word value) {
    pthread_mutex_lock(&queue->mutex);
    if (!strcmp(selector,"setStatEnabled:")) queue->stat_enabled=(unsigned char)value!=0;
    else if (!strcmp(selector,"setStatOptions:")) queue->stat_options=value;
    else queue->stat_locations=value;
    pthread_mutex_unlock(&queue->mutex);
}
static void metal_finish_gl(void *unused) { glFinish(); }
static void metal_queue_finish(Word object,const char *selector) {
    if (vulkan.available) {
        PFN_vkQueueWaitIdle wait=(PFN_vkQueueWaitIdle)vulkan.get_device_proc(vulkan.device,"vkQueueWaitIdle");
        pthread_mutex_lock(&vulkan_mutex);
        VkResult result=wait(vulkan.queue);
        pthread_mutex_unlock(&vulkan_mutex);
        if (result!=VK_SUCCESS) stop("Vulkan queue wait failed",selector);
    } else graphics_run(metal_finish_gl,NULL);
}
static void metal_debug_boundary(void *unused) { glFlush(); }
static void metal_queue_boundary(void) { if (!vulkan.available) graphics_run(metal_debug_boundary,NULL); }
static Method metal_queue_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalCommandQueue")) {
        if (!strcmp(selector,"device")) return (Method)metal_queue_device;
        if (!strcmp(selector,"commandBuffer") || !strcmp(selector,"commandBufferWithUnretainedReferences")) return (Method)metal_command_buffer;
        if (!strcmp(selector,"insertDebugCaptureBoundary")) return (Method)metal_queue_boundary;
        if (!strcmp(selector,"conformsToProtocol:")) return (Method)metal_queue_conforms;
        if (!strcmp(selector,"label")) return (Method)metal_queue_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_queue_set_label;
        if (!strcmp(selector,"addPerfSampleHandler:")) return (Method)metal_add_perf_handler;
        if (!strcmp(selector,"getGPUPriority") || !strcmp(selector,"getBackgroundGPUPriority")) return (Method)metal_queue_priority;
        if (!strcmp(selector,"setGPUPriority:") || !strcmp(selector,"setBackgroundGPUPriority:")) return (Method)metal_queue_set_priority;
        if (!strcmp(selector,"setGPUPriority:offset:") || !strcmp(selector,"setBackgroundGPUPriority:offset:")) return (Method)metal_queue_set_priority_offset;
        if (!strcmp(selector,"setSubmissionQueue:") || !strcmp(selector,"setCompletionQueue:")) return (Method)metal_queue_dispatch;
        if (!strcmp(selector,"qosClass") || !strcmp(selector,"qosRelativePriority")) return (Method)metal_queue_qos;
        if (!strcmp(selector,"backgroundTrackingPID") || !strcmp(selector,"maxCommandBufferCount") || !strcmp(selector,"skipRender") || !strcmp(selector,"executionEnabled") || !strcmp(selector,"isProfilingEnabled")) return (Method)metal_queue_property;
        if (!strcmp(selector,"setBackgroundTrackingPID:") || !strcmp(selector,"setSkipRender:") || !strcmp(selector,"setExecutionEnabled:") || !strcmp(selector,"setProfilingEnabled:")) return (Method)metal_queue_set_property;
        if (!strcmp(selector,"finish")) return (Method)metal_queue_finish;
        if (!strcmp(selector,"isStatEnabled") || !strcmp(selector,"StatOptions") || !strcmp(selector,"getStatOptions") || !strcmp(selector,"StatLocations") || !strcmp(selector,"getStatLocations")) return (Method)metal_queue_stat;
        if (!strcmp(selector,"setStatEnabled:") || !strcmp(selector,"setStatOptions:") || !strcmp(selector,"setStatLocations:")) return (Method)metal_queue_set_stat;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalCommandBuffer")) {
        if ((!strcmp(selector,"GPUStartTime") || !strcmp(selector,"GPUEndTime")) && vulkan.calibrated_timestamps) {
            return (Method)metal_gpu_time;
        }
        if (!strcmp(selector,"commandQueue")) return (Method)metal_command_queue;
        if (!strcmp(selector,"device")) return (Method)metal_command_device;
        if (!strcmp(selector,"status")) return (Method)metal_command_status;
        if (!strcmp(selector,"enqueue")) return (Method)metal_enqueue;
        if (!strcmp(selector,"retainedReferences")) return (Method)metal_command_retains;
        if (!strcmp(selector,"label")) return (Method)metal_command_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_command_set_label;
    }
    return NULL;
}
#undef VKD
