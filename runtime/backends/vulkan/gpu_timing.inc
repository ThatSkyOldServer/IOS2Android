/* Metal GPU times use host monotonic seconds. Raw device ticks alone cannot
 * implement that contract; expose the properties only with calibrated clocks. */
static void metal_gpu_time_begin(struct MetalCommand *command) {
    if(!vulkan.calibrated_timestamps)return;
    struct MetalCommandPool *pool=command->pool_owner;
    if(!pool->gpu_timestamps) {
        VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        if(VKD(vkCreateQueryPool)(vulkan.device,&info,NULL,&pool->gpu_timestamps)!=VK_SUCCESS)return;
    }
    VKD(vkCmdResetQueryPool)(command->command,pool->gpu_timestamps,0,2);
    VKD(vkCmdWriteTimestamp)(command->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool->gpu_timestamps,0);
    command->gpu_timestamps=1;
}
static void metal_gpu_time_end(struct MetalCommand *command) {
    if(command->gpu_timestamps)VKD(vkCmdWriteTimestamp)(command->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,command->pool_owner->gpu_timestamps,1);
}
static void metal_gpu_time_complete(struct MetalCommand *command) {
    if(!command->gpu_timestamps)return;
    uint64_t ticks[2];
    if(VKD(vkGetQueryPoolResults)(vulkan.device,command->pool_owner->gpu_timestamps,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS || ticks[1]<ticks[0])return;
    static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    static uint64_t host,device,calibrated_at;static int valid;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    uint64_t nanos=(uint64_t)now.tv_sec*1000000000ULL+now.tv_nsec;
    pthread_mutex_lock(&mutex);
    if(!calibrated_at || nanos-calibrated_at>=1000000000ULL) {
        VkCalibratedTimestampInfoEXT info[2]={
            {.sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,.timeDomain=VK_TIME_DOMAIN_DEVICE_EXT},
            {.sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,.timeDomain=vulkan.host_time_domain}};
        uint64_t values[2],deviation=0;
        valid=vulkan.calibrated_timestamps(vulkan.device,2,info,values,&deviation)==VK_SUCCESS && deviation<=1000000ULL;
        if(valid){device=values[0];host=values[1];}
        calibrated_at=nanos;
    }
    if(valid) {
        double start=(double)host+((double)ticks[0]-(double)device)*vulkan.properties.limits.timestampPeriod;
        double end=(double)host+((double)ticks[1]-(double)device)*vulkan.properties.limits.timestampPeriod;
        if(start>0 && end>=start && end<=(double)nanos+1000000.0) {
            command->gpu_start=start*1e-9;command->gpu_end=end*1e-9;
            __atomic_store_n(&command->gpu_time_valid,1,__ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&mutex);
}
static double metal_gpu_time(struct MetalCommand *command,const char *selector) {
    if(!__atomic_load_n(&command->gpu_time_valid,__ATOMIC_ACQUIRE))return 0;
    return !strcmp(selector,"GPUStartTime") ? command->gpu_start : command->gpu_end;
}
