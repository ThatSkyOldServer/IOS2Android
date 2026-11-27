 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalDiagnosticCapture { struct MetalBuffer buffer; uint32_t width,height,format; const char *name; struct MetalDiagnosticCapture *next; };
static struct MetalBuffer *metal_math_buffer(struct MetalCommand *command) {
#if I2A_DEVELOPMENT && I2A_SHADER_DIAGNOSTIC_COUNT
    if (!command->math_diagnostics) {
        struct MetalBuffer *buffer=command->pool_owner->math_diagnostics;
        if (!buffer) {
            buffer=calloc(1,sizeof(*buffer));
            size_t size=((1+I2A_SHADER_DIAGNOSTIC_COUNT*8)*sizeof(uint32_t)+15)&~(size_t)15;
            if (!buffer || metal_buffer_allocate(buffer,size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,0)!=VK_SUCCESS)
                stop("Cannot allocate shader diagnostic buffer","diagnostic build");
            command->pool_owner->math_diagnostics=buffer;
        }
        memset(buffer->mapped,0,buffer->length);
        *(uint32_t *)buffer->mapped=__atomic_load_n(&shader_diagnostics_requested,__ATOMIC_ACQUIRE)!=0;
        command->math_diagnostics=buffer;
    }
    return command->math_diagnostics;
#else
    stop("Diagnostic shader in normal package","shader binding");
#endif
}
#if I2A_DEVELOPMENT && I2A_SHADER_DIAGNOSTIC_COUNT
struct ShaderMathObservation {
    uint64_t samples,domain,nonfinite,last_report_ns;
    float x,y,value;
    uint32_t flags;
    unsigned observed;
};
static struct ShaderMathObservation shader_math_observations[I2A_SHADER_DIAGNOSTIC_COUNT];
static pthread_mutex_t shader_math_observation_lock=PTHREAD_MUTEX_INITIALIZER;
static void metal_math_emit_observation(unsigned index,uint64_t now) {
    struct ShaderMathObservation *row=&shader_math_observations[index];
    const struct ShaderDiagnosticSite *site=&shader_diagnostic_sites[index];
    LOG("[BUSINESS-shader] site=%u shader=%s op=%s line=%u samples=%llu domain=%llu nonfinite=%llu x=%g y=%g value=%g flags=%u",index,
        site->shader,site->operation,site->line,(unsigned long long)row->samples,(unsigned long long)row->domain,
        (unsigned long long)row->nonfinite,row->x,row->y,row->value,row->flags);
    row->samples=row->domain=row->nonfinite=0;
    row->flags=0; row->observed=1; row->last_report_ns=now;
}
static void metal_math_flush_observations(void) {
    struct timespec time; clock_gettime(CLOCK_MONOTONIC,&time);
    uint64_t now=(uint64_t)time.tv_sec*1000000000ULL+(uint64_t)time.tv_nsec;
    pthread_mutex_lock(&shader_math_observation_lock);
    for (unsigned i=0;i<I2A_SHADER_DIAGNOSTIC_COUNT;++i)
        if (shader_math_observations[i].domain || shader_math_observations[i].nonfinite) metal_math_emit_observation(i,now);
    pthread_mutex_unlock(&shader_math_observation_lock);
}
#endif
#if I2A_DEVELOPMENT
static void metal_math_configure_observation(int enabled) {
#if I2A_SHADER_DIAGNOSTIC_COUNT
    if (enabled && !__atomic_load_n(&shader_diagnostics_requested,__ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&shader_math_observation_lock);
        memset(shader_math_observations,0,sizeof(shader_math_observations));
        pthread_mutex_unlock(&shader_math_observation_lock);
    }
#endif
    __atomic_store_n(&shader_diagnostics_requested,enabled,__ATOMIC_RELEASE);
#if I2A_SHADER_DIAGNOSTIC_COUNT
    if (!enabled) metal_math_flush_observations();
#endif
    LOG("[BUSINESS-shader-control] enabled=%u sites=%u",enabled!=0,I2A_SHADER_DIAGNOSTIC_COUNT);
}
#endif
static void metal_math_complete(struct MetalCommand *command) {
#if I2A_DEVELOPMENT && I2A_SHADER_DIAGNOSTIC_COUNT
    struct MetalBuffer *buffer=command->math_diagnostics;
    if (!buffer) return;
    uint32_t *words=buffer->mapped;
    struct timespec time; clock_gettime(CLOCK_MONOTONIC,&time);
    uint64_t now=(uint64_t)time.tv_sec*1000000000ULL+(uint64_t)time.tv_nsec;
    pthread_mutex_lock(&shader_math_observation_lock);
    for (unsigned i=0;words[0] && i<I2A_SHADER_DIAGNOSTIC_COUNT;++i) {
        uint32_t *source=words+1+i*8;
        if (!source[0]) continue;
        struct ShaderMathObservation *row=&shader_math_observations[i];
        unsigned first_issue=(source[1] || source[2]) && !(row->domain || row->nonfinite);
        if (first_issue) {
            memcpy(&row->x,source+4,4); memcpy(&row->y,source+5,4); memcpy(&row->value,source+6,4);
            row->flags=source[7];
        }
        row->samples+=source[0]; row->domain+=source[1]; row->nonfinite+=source[2];
        // Report initial coverage, then aggregate issues without a lifetime cap.
        if (!row->observed || ((row->domain || row->nonfinite) &&
                (now-row->last_report_ns>=1000000000ULL || !__atomic_load_n(&shader_diagnostics_requested,__ATOMIC_ACQUIRE))))
            metal_math_emit_observation(i,now);
    }
    pthread_mutex_unlock(&shader_math_observation_lock);
    command->math_diagnostics=NULL;
#endif
}
static void metal_math_trace_render(struct MetalRenderEncoder *encoder,struct MetalFunction *function) {
#if I2A_DEVELOPMENT && I2A_SHADER_DIAGNOSTIC_COUNT
    if(!__atomic_load_n(&shader_diagnostics_requested,__ATOMIC_ACQUIRE))return;
    static struct {uint64_t time;uint32_t width,height;unsigned remaining;} observed[COUNT(shader_specs)];
    struct timespec clock;clock_gettime(CLOCK_MONOTONIC,&clock);
    uint64_t now=(uint64_t)clock.tv_sec*1000000000ULL+(uint64_t)clock.tv_nsec;
    size_t index=(size_t)(function->spec-shader_specs);
    uint32_t width=encoder->state->width,height=encoder->state->height;
    pthread_mutex_lock(&shader_math_observation_lock);
    if(observed[index].width!=width || observed[index].height!=height) {
        observed[index].width=width;observed[index].height=height;observed[index].remaining=4;
    }
    int report=observed[index].remaining || now-observed[index].time>=1000000000ULL;
    if(observed[index].remaining)--observed[index].remaining;
    if(report)observed[index].time=now;
    pthread_mutex_unlock(&shader_math_observation_lock);
    if(!report)return;
    unsigned stage_index=function->spec->stage==1 ? 0 : 1;
    LOG("[DIAG-shader-watch] shader=%s target=%ux%u viewport=%g,%g,%g,%g command=%p draw=%u",function->spec->name,width,height,
        encoder->state->viewport.x,encoder->state->viewport.y,encoder->state->viewport.width,encoder->state->viewport.height,
        (void *)encoder->command,encoder->state->draws);
    for(unsigned i=0;i<encoder->state->attachment_count;++i) {
        struct MetalTexture *texture=encoder->state->attachments[i];
        const VkAttachmentDescription *description=&encoder->state->descriptions[i];
        const VkClearValue *clear=&encoder->state->clear[i];
        LOG("[DIAG-shader-watch] shader=%s attachment=%u object=%p format=%u load=%u store=%u started=%u clear=%g,%g,%g,%g",function->spec->name,
            i,(void *)texture,texture->format.metal,description->loadOp,description->storeOp,encoder->state->started,
            clear->color.float32[0],clear->color.float32[1],clear->color.float32[2],clear->color.float32[3]);
    }
    for(unsigned i=0;i<function->spec->binding_count;++i) {
        const struct ShaderBindingSpec *binding=&shader_bindings[function->spec->first_binding+i];
        if(binding->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && binding->metal_index<128) {
            struct MetalTexture *texture=encoder->state->textures[stage_index][binding->metal_index];
            if(texture)LOG("[DIAG-shader-watch] shader=%s texture=%u object=%p format=%u extent=%lux%lux%lu mips=%lu",function->spec->name,
                binding->metal_index,(void *)texture,texture->format.metal,texture->width,texture->height,texture->depth,texture->mips);
        } else if(binding->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && binding->metal_index<31) {
            struct MetalBoundBuffer *use=&encoder->state->buffers[stage_index][binding->metal_index];
            if(!use->buffer || !use->buffer->mapped || use->offset>use->buffer->length || binding->metal_size>use->buffer->length-use->offset)continue;
            char words[64*9+1];size_t used=0,count=binding->metal_size/4;if(count>64)count=64;
            for(size_t j=0;j<count;++j){uint32_t value;memcpy(&value,(char *)use->buffer->mapped+use->offset+j*4,4);used+=(size_t)snprintf(words+used,sizeof(words)-used,"%08x ",value);}
            words[used]=0;
            LOG("[DIAG-shader-watch] shader=%s buffer=%u offset=%llu bytes=%u words=%s",function->spec->name,binding->metal_index,
                (unsigned long long)use->offset,binding->metal_size,words);
        }
    }
#endif
}
static void metal_diagnostic_pass_begin(struct MetalRenderEncoder *encoder) {
#if I2A_DEVELOPMENT
    struct MetalCommand *command=encoder->command;
    if(!command->diagnostic_pass_enabled)return;
    if(command->diagnostic_pass_count==METAL_DIAGNOSTIC_PASSES){++command->diagnostic_pass_overflow;return;}
    unsigned index=command->diagnostic_pass_count++;
    struct MetalPassTiming *sample=&command->diagnostic_pass[index];
    sample->width=encoder->state->width;sample->height=encoder->state->height;sample->first_draw=encoder->state->draws;
    sample->resumed=encoder->state->started;
    sample->snapshot_regions=encoder->state->pending_snapshot_regions;
    sample->snapshot_color_bytes=encoder->state->pending_snapshot_color_bytes;
    encoder->state->pending_snapshot_regions=0;encoder->state->pending_snapshot_color_bytes=0;
    snprintf(sample->label,sizeof(sample->label),"%s",encoder->object.label ? string_bytes(encoder->object.label,"GPU pass label") : "unlabelled");
    struct MetalPipeline *pipeline=encoder->state->pipeline;
    sample->shader=pipeline ? (pipeline->fragment ? pipeline->fragment->spec->name : pipeline->vertex->spec->name) : "clear-only";
    for(unsigned i=0;i<encoder->state->attachment_count;++i) {
        const VkAttachmentDescription *attachment=&encoder->state->descriptions[i];
        VkImageAspectFlags aspects=encoder->state->attachments[i]->format.aspects;
        sample->loads+=encoder->state->started || ((aspects&~VK_IMAGE_ASPECT_STENCIL_BIT) && attachment->loadOp==VK_ATTACHMENT_LOAD_OP_LOAD) ||
            ((aspects&VK_IMAGE_ASPECT_STENCIL_BIT) && attachment->stencilLoadOp==VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    encoder->state->diagnostic_pass=index+1;
    VKD(vkCmdWriteTimestamp)(command->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,command->pool_owner->pass_timestamps,index*2);
#endif
}
static void metal_diagnostic_pass_draw(struct MetalRenderEncoder *encoder) {
#if I2A_DEVELOPMENT
    if(!encoder->state->diagnostic_pass)return;
    struct MetalPassTiming *sample=&encoder->command->diagnostic_pass[encoder->state->diagnostic_pass-1];
    struct MetalPipeline *pipeline=encoder->state->pipeline;
    const char *shader=pipeline->fragment ? pipeline->fragment->spec->name : pipeline->vertex->spec->name;
    if(shader!=sample->shader && strcmp(shader,sample->shader))sample->mixed=1;
#endif
}
static void metal_diagnostic_pass_end(struct MetalRenderEncoder *encoder,unsigned final) {
#if I2A_DEVELOPMENT
    if(!encoder->state->diagnostic_pass)return;
    unsigned index=encoder->state->diagnostic_pass-1;encoder->state->diagnostic_pass=0;
    struct MetalCommand *command=encoder->command;
    struct MetalPassTiming *sample=&command->diagnostic_pass[index];sample->closed=1;sample->draws=encoder->state->draws-sample->first_draw;
    for(unsigned i=0;i<encoder->state->attachment_count;++i) {
        VkImageAspectFlags aspects=encoder->state->attachments[i]->format.aspects;
        const VkAttachmentDescription *attachment=&encoder->state->descriptions[i];
        VkAttachmentStoreOp store=final ? encoder->state->final_store[i] : attachment->storeOp;
        VkAttachmentStoreOp stencil=final ? encoder->state->final_stencil_store[i] : attachment->stencilStoreOp;
        sample->stores+=((aspects&~VK_IMAGE_ASPECT_STENCIL_BIT) && store==VK_ATTACHMENT_STORE_OP_STORE) ||
            ((aspects&VK_IMAGE_ASPECT_STENCIL_BIT) && stencil==VK_ATTACHMENT_STORE_OP_STORE);
    }
    VKD(vkCmdWriteTimestamp)(command->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,command->pool_owner->pass_timestamps,index*2+1);
#endif
}
static void metal_diagnostic_command_begin(struct MetalCommand *command) {
    if (!metal_diagnostic_enabled || !vulkan.timestamp_bits) return;
    struct MetalCommandPool *pool=command->pool_owner;
    /* Public GPU timing already records the same boundary on capable devices. */
    if (!command->gpu_timestamps && !pool->timestamps) {
        VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        if (VKD(vkCreateQueryPool)(vulkan.device,&info,NULL,&pool->timestamps)!=VK_SUCCESS) return;
    }
    if (!command->gpu_timestamps) {
        VKD(vkCmdResetQueryPool)(command->command,pool->timestamps,0,2);
        VKD(vkCmdWriteTimestamp)(command->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool->timestamps,0);
    }
    command->diagnostic_timestamps=1;
#if I2A_DEVELOPMENT
    if(__atomic_fetch_add(&metal_pass_sample_cursor,1,__ATOMIC_RELAXED)%17==0) {
        if(!pool->pass_timestamps) {
            VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=METAL_DIAGNOSTIC_PASSES*2};
            if(VKD(vkCreateQueryPool)(vulkan.device,&info,NULL,&pool->pass_timestamps)!=VK_SUCCESS)return;
        }
        VKD(vkCmdResetQueryPool)(command->command,pool->pass_timestamps,0,METAL_DIAGNOSTIC_PASSES*2);
        command->diagnostic_pass_enabled=1;
    }
#endif
}
static void metal_diagnostic_command_end(struct MetalCommand *command) {
    if (command->math_diagnostics) {
        VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=command->math_diagnostics->buffer,.size=VK_WHOLE_SIZE};
        VKD(vkCmdPipelineBarrier)(command->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
    }
    if (command->diagnostic_timestamps && !command->gpu_timestamps)
        VKD(vkCmdWriteTimestamp)(command->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,command->pool_owner->timestamps,1);
}
static void metal_diagnostic_submitted(struct MetalCommand *command) {
#if I2A_DEVELOPMENT
    /* Consecutive submissions through a present marker form a submission group,
     * not an assumed application's frame. Completion callbacks may arrive out
     * of order, so retire a group only after every member's fence is complete. */
    if (!command->diagnostic_timestamps && !metal_diagnostic_enabled) return;
    pthread_mutex_lock(&metal_present_timing_mutex);
    uint64_t generation=metal_present_timing_generation;
    struct MetalPresentTiming *group=&metal_present_timings[generation%COUNT(metal_present_timings)];
    if (group->generation==generation || !group->pending) {
        if (group->generation!=generation) *group=(struct MetalPresentTiming){.generation=generation,.start=UINT64_MAX,.valid=1};
        command->diagnostic_present_generation=generation;++group->pending;++group->commands;
        command->diagnostic_submission_index=group->commands;
        if (!command->diagnostic_timestamps) group->valid=0;
        if (command->drawable) group->closed=1;
    }
    if (command->drawable) ++metal_present_timing_generation;
    pthread_mutex_unlock(&metal_present_timing_mutex);
#endif
}
static void metal_diagnostic_present_complete(struct MetalCommand *command,uint64_t start,uint64_t end) {
#if I2A_DEVELOPMENT
    uint64_t generation=command->diagnostic_present_generation;
    if (!generation) return;
    if (generation%60==0 && end>=start && end) {
        LOG("[DIAG-render] GPU command interval: group=%llu index=%u present=%u label=%s startTick=%llu endTick=%llu span=%.3fms",
            (unsigned long long)generation,command->diagnostic_submission_index,command->drawable!=NULL,
            command->object.content[2] ? string_bytes(command->object.content[2],"GPU command label") : "unlabelled",
            (unsigned long long)start,(unsigned long long)end,(end-start)*vulkan.properties.limits.timestampPeriod*1e-6);
    }
    pthread_mutex_lock(&metal_present_timing_mutex);
    struct MetalPresentTiming *group=&metal_present_timings[generation%COUNT(metal_present_timings)];
    if (group->generation==generation) {
        if (end<start || !end) group->valid=0;
        else {
            if (start<group->start) group->start=start;if (end>group->end) group->end=end;
            group->sum+=end-start;
        }
        if (group->pending) --group->pending;
        if (!group->pending && group->closed && group->valid && generation%60==0) {
            LOG("[DIAG-render] GPU present group: id=%llu commands=%u envelope=%.3fms summedSpans=%.3fms startTick=%llu endTick=%llu",
                (unsigned long long)generation,group->commands,(group->end-group->start)*vulkan.properties.limits.timestampPeriod*1e-6,
                group->sum*vulkan.properties.limits.timestampPeriod*1e-6,(unsigned long long)group->start,(unsigned long long)group->end);
        }
    }
    pthread_mutex_unlock(&metal_present_timing_mutex);
#endif
}
static void metal_diagnostic_command_complete(struct MetalCommand *command) {
    metal_math_complete(command);
#if I2A_DEVELOPMENT
    if(command->diagnostic_pass_count) {
        uint64_t ticks[METAL_DIAGNOSTIC_PASSES*2];
        if(VKD(vkGetQueryPoolResults)(vulkan.device,command->pool_owner->pass_timestamps,0,command->diagnostic_pass_count*2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            uint64_t mask=vulkan.timestamp_bits>=64 ? UINT64_MAX : (1ULL<<vulkan.timestamp_bits)-1;
            pthread_mutex_lock(&metal_pass_sample_mutex);
            for(unsigned i=0;i<command->diagnostic_pass_count;++i) {
                struct MetalPassTiming *sample=&command->diagnostic_pass[i];if(!sample->closed)continue;
                unsigned index=0;
                while(index<COUNT(metal_pass_samples)-1 && metal_pass_samples[index].samples &&
                    (metal_pass_samples[index].shader!=sample->shader || metal_pass_samples[index].width!=sample->width || metal_pass_samples[index].height!=sample->height ||
                    metal_pass_samples[index].mixed!=sample->mixed || strcmp(metal_pass_samples[index].label,sample->label)))++index;
                struct MetalPassSummary *summary=&metal_pass_samples[index];
                if(!summary->samples) {
                    summary->shader=index==COUNT(metal_pass_samples)-1 ? "other passes" : sample->shader;
                    snprintf(summary->label,sizeof(summary->label),"%s",sample->label);summary->width=sample->width;summary->height=sample->height;summary->mixed=sample->mixed;
                }
                summary->nanoseconds+=(uint64_t)(((ticks[i*2+1]-ticks[i*2])&mask)*vulkan.properties.limits.timestampPeriod);
                summary->snapshot_regions+=sample->snapshot_regions;summary->snapshot_color_bytes+=sample->snapshot_color_bytes;
                summary->resumed+=sample->resumed;
                summary->draws+=sample->draws;summary->loads+=sample->loads;summary->stores+=sample->stores;++summary->samples;
            }
            pthread_mutex_unlock(&metal_pass_sample_mutex);
        }
        if(command->diagnostic_pass_overflow)LOG_LIMITED(4,"[DIAG-render] GPU pass samples truncated: dropped=%u",command->diagnostic_pass_overflow);
    }
#endif
    if (command->diagnostic_timestamps) {
        uint64_t ticks[2];
        VkQueryPool pool=command->gpu_timestamps ? command->pool_owner->gpu_timestamps : command->pool_owner->timestamps;
        if (VKD(vkGetQueryPoolResults)(vulkan.device,pool,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            metal_diagnostic_present_complete(command,ticks[0],ticks[1]);
            uint64_t mask=vulkan.timestamp_bits>=64 ? UINT64_MAX : (1ULL<<vulkan.timestamp_bits)-1;
            uint64_t duration=(uint64_t)(((ticks[1]-ticks[0])&mask)*vulkan.properties.limits.timestampPeriod);
            metal_diagnostic_add(2,duration);
#if I2A_DEVELOPMENT
            if(metal_diagnostic_enabled && (__atomic_fetch_add(&metal_command_sample_cursor,1,__ATOMIC_RELAXED)&31U)==0) {
                char label[64];snprintf(label,sizeof(label),"%s",command->object.content[2] ? string_bytes(command->object.content[2],"GPU command label") : "unlabelled");
                pthread_mutex_lock(&metal_command_sample_mutex);
                unsigned index=0;while(index<COUNT(metal_command_samples)-1 && metal_command_samples[index].samples && strcmp(metal_command_samples[index].label,label))++index;
                struct MetalCommandSample *sample=&metal_command_samples[index];
                if(!sample->samples)snprintf(sample->label,sizeof(sample->label),"%s",index==COUNT(metal_command_samples)-1 ? "other labels" : label);
                sample->nanoseconds+=duration;sample->operations+=command->recorded_operations;++sample->samples;
                pthread_mutex_unlock(&metal_command_sample_mutex);
            }
#endif
        } else metal_diagnostic_present_complete(command,0,0);
    } else metal_diagnostic_present_complete(command,0,0);
    while (command->captures) {
        struct MetalDiagnosticCapture *capture=command->captures; command->captures=capture->next;
        char path[PATH_MAX]; snprintf(path,sizeof(path),"%s/%s-%ux%u-vk%u.raw",metal_diagnostic_directory,capture->name,capture->width,capture->height,capture->format);
        FILE *output=fopen(path,"wb");
        if (output) { size_t size=fwrite(capture->buffer.mapped,1,capture->buffer.length,output); fclose(output); LOG("[DIAG-render] GPU target captured: %s bytes=%zu",path,size); }
        metal_buffer_dispose(&capture->buffer); free(capture);
    }
}
static void metal_diagnostic_capture(struct MetalRenderEncoder *encoder) {
    if (!metal_diagnostic_readback || __atomic_load_n(&metal_diagnostic_frames,__ATOMIC_RELAXED)<60 || !encoder->state->pipeline || !encoder->state->pipeline->fragment || !encoder->state->color_count) return;
    const struct ShaderSpec *spec=encoder->state->pipeline->fragment->spec;
    unsigned index=(unsigned)(spec-shader_specs);
    static unsigned taken[COUNT(shader_specs)],total;
    if (__atomic_load_n(&total,__ATOMIC_RELAXED)>=16 || __atomic_exchange_n(&taken[index],1,__ATOMIC_RELAXED)) return;
    unsigned attachment=encoder->state->colors[0].attachment;
    if (attachment==VK_ATTACHMENT_UNUSED) return;
    struct MetalTexture *texture=encoder->state->attachments[attachment];
    if (texture->mips!=1 || texture->layers!=1) return;
    unsigned pixel_bytes=texture->format.format==VK_FORMAT_R16G16B16A16_SFLOAT ? 8 :
        texture->format.format==VK_FORMAT_R8G8B8A8_UNORM || texture->format.format==VK_FORMAT_R8G8B8A8_SRGB ||
        texture->format.format==VK_FORMAT_B8G8R8A8_UNORM || texture->format.format==VK_FORMAT_B8G8R8A8_SRGB ? 4 : 0;
    if (!pixel_bytes) { LOG("[BUSINESS-capture] Unsupported format: %s format=%u",spec->name,texture->format.format); return; }
    if (__atomic_fetch_add(&total,1,__ATOMIC_RELAXED)>=16) return;
    struct MetalDiagnosticCapture *capture=calloc(1,sizeof(*capture)); if (!capture) return;
    size_t size=texture->width*texture->height*pixel_bytes;
    if (metal_buffer_allocate(&capture->buffer,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT)!=VK_SUCCESS) { free(capture); return; }
    capture->width=(uint32_t)texture->width; capture->height=(uint32_t)texture->height; capture->format=texture->format.format; capture->name=spec->name;
    metal_render_transition(encoder,texture,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={capture->width,capture->height,1}};
    VKD(vkCmdCopyImageToBuffer)(encoder->command->command,texture->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,capture->buffer.buffer,1,&copy);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=capture->buffer.buffer,.size=VK_WHOLE_SIZE};
    VKD(vkCmdPipelineBarrier)(encoder->command->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
    capture->next=encoder->command->captures; encoder->command->captures=capture;
}
static void metal_diagnostic_present(void) {
    if (!metal_diagnostic_enabled) return;
    __atomic_fetch_add(&metal_diagnostic_frames,1,__ATOMIC_RELAXED);
    static uint64_t start; static unsigned frames; uint64_t now=metal_diagnostic_time();
    if (!start) start=now;
    ++frames; if (now-start<5000000000ULL) return;
    uint64_t memory[6]; for (unsigned i=0;i<6;++i) memory[i]=__atomic_load_n(&metal_memory_census[i],__ATOMIC_RELAXED);
    LOG("[DIAG-memory] bufferBacking=%llu textureBacking=%llu descriptorObjects=%llu renderEncoders=%llu commandObjects=%llu guestBuffers=%llu",
        (unsigned long long)memory[0],(unsigned long long)memory[1],(unsigned long long)memory[2],(unsigned long long)memory[3],(unsigned long long)memory[4],(unsigned long long)memory[5]);
    uint64_t ns[3],calls[3]; for (unsigned i=0;i<3;++i) {
        ns[i]=__atomic_exchange_n(&metal_diagnostic_ns[i],0,__ATOMIC_RELAXED);
        calls[i]=__atomic_exchange_n(&metal_diagnostic_calls[i],0,__ATOMIC_RELAXED);
    }
    LOG("[DIAG-render] Work per presented frame: frames=%u CPU pipeline=%.3fms descriptors=%.3fms draws=%llu summedGPUCommandSpans=%.3fms commands=%llu timestampBits=%u",frames,
        ns[0]*1e-6/frames,ns[1]*1e-6/frames,(unsigned long long)calls[1],ns[2]*1e-6/frames,(unsigned long long)calls[2],vulkan.timestamp_bits);
#if I2A_DEVELOPMENT
    struct MetalPassSummary passes[COUNT(metal_pass_samples)];
    pthread_mutex_lock(&metal_pass_sample_mutex);memcpy(passes,metal_pass_samples,sizeof(passes));memset(metal_pass_samples,0,sizeof(metal_pass_samples));pthread_mutex_unlock(&metal_pass_sample_mutex);
    for(unsigned i=0;i<COUNT(passes);++i)if(passes[i].samples)
        LOG("[DIAG-render] GPU pass sample: label=%s firstShader=%s extent=%ux%u mixed=%u samples=%u meanSpan=%.3fms meanDraws=%.1f meanLoads=%.1f meanStores=%.1f stride=17 resumed=%u snapshotRegions=%llu snapshotColorBytes=%llu",passes[i].label,passes[i].shader,passes[i].width,passes[i].height,passes[i].mixed,passes[i].samples,passes[i].nanoseconds*1e-6/passes[i].samples,(double)passes[i].draws/passes[i].samples,(double)passes[i].loads/passes[i].samples,(double)passes[i].stores/passes[i].samples,passes[i].resumed,(unsigned long long)passes[i].snapshot_regions,(unsigned long long)passes[i].snapshot_color_bytes);
    struct MetalCommandSample samples[COUNT(metal_command_samples)];
    pthread_mutex_lock(&metal_command_sample_mutex);memcpy(samples,metal_command_samples,sizeof(samples));memset(metal_command_samples,0,sizeof(metal_command_samples));pthread_mutex_unlock(&metal_command_sample_mutex);
    for(unsigned i=0;i<COUNT(samples);++i)if(samples[i].samples)
        LOG("[DIAG-render] GPU command sample: label=%s samples=%u meanSpan=%.3fms meanOperations=%.1f stride=32",samples[i].label,samples[i].samples,samples[i].nanoseconds*1e-6/samples[i].samples,(double)samples[i].operations/samples[i].samples);
    uint64_t reused=__atomic_exchange_n(&metal_descriptor_cache_counts[0],0,__ATOMIC_RELAXED);
    uint64_t created=__atomic_exchange_n(&metal_descriptor_cache_counts[1],0,__ATOMIC_RELAXED);
    LOG("[DIAG-render] Descriptor snapshots: reused=%llu created=%llu",(unsigned long long)reused,(unsigned long long)created);
    uint64_t planned=__atomic_exchange_n(&metal_layout_counts[0],0,__ATOMIC_RELAXED);
    uint64_t pass_begins=__atomic_exchange_n(&metal_layout_counts[1],0,__ATOMIC_RELAXED);
    LOG("[DIAG-render] Render recording: plannedInitialLayouts=%llu passBegins=%llu",(unsigned long long)planned,(unsigned long long)pass_begins);
#endif
    start=now; frames=0;
}
#undef VKD
