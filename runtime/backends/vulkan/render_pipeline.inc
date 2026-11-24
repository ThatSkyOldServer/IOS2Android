 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalPipelineKey {
    VkPrimitiveTopology topology;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPolygonMode polygon;
    unsigned clamp;
    struct MetalDepthState *depth;
};
struct MetalPipelineVariant { struct MetalPipelineKey key; VkPipeline pipeline; struct MetalPipelineVariant *next; };
struct MetalPipeline {
    struct MetalResource object;
    struct MetalFunction *vertex,*fragment;
    VkDescriptorSetLayout sets[2];
    unsigned dynamic_uniform,dynamic_storage,dynamic_count,*dynamic_slots[2],descriptor_counts[6];
    VkPipelineLayout layout;
    VkPipeline compute_fallback;
    VkRenderPass pass;
    VkVertexInputBindingDescription bindings[31];
    VkVertexInputAttributeDescription attributes[31];
    uint32_t binding_count,attribute_count,color_count;
    VkPipelineColorBlendAttachmentState blend[8];
    VkFormat colors[8],depth_format;
    VkSampleCountFlagBits samples;
    unsigned rasterization,alpha_coverage,alpha_one;
    pthread_mutex_t mutex;
    struct MetalPipelineVariant *variants;
    struct MetalLifetime lifetime;
};
struct MetalPipelineReflection { Word isa,vertex,fragment; };
static VkPipelineCache metal_pipeline_cache;
static pthread_mutex_t metal_pipeline_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t metal_pipeline_save_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint64_t metal_pipeline_generation,metal_pipeline_saved_generation;
static unsigned metal_pipeline_save_pending,metal_pipeline_cache_initialized;
static int metal_pipeline_cache_valid(const void *data,size_t size){
    if(size<sizeof(VkPipelineCacheHeaderVersionOne))return 0;VkPipelineCacheHeaderVersionOne header;memcpy(&header,data,sizeof(header));
    return header.headerSize>=sizeof(header) && header.headerSize<=size && header.headerVersion==VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendorID==vulkan.properties.vendorID && header.deviceID==vulkan.properties.deviceID && !memcmp(header.pipelineCacheUUID,vulkan.properties.pipelineCacheUUID,VK_UUID_SIZE);
}
static void metal_pipeline_cache_initialize(void){
    if(metal_pipeline_cache_initialized)return;metal_pipeline_cache_initialized=1;const char *directory=shader_cache_root();void *data=NULL;size_t size=0;char *path=NULL;
    if(directory && asprintf(&path,"%s/pipelines.bin",directory)>=0){int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd>=0){struct stat info;if(!fstat(fd,&info) && info.st_size>0 && (uint64_t)info.st_size<=SIZE_MAX){size=(size_t)info.st_size;data=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);if(data==MAP_FAILED)data=NULL;}close(fd);if(!data || !metal_pipeline_cache_valid(data,size)){if(data)munmap(data,size);data=NULL;size=0;unlink(path);}}free(path);}
    VkPipelineCacheCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,.initialDataSize=data ? size : 0,.pInitialData=data};
    VkResult result=VKD(vkCreatePipelineCache)(vulkan.device,&info,NULL,&metal_pipeline_cache);
    if(result!=VK_SUCCESS && data){info.initialDataSize=0;info.pInitialData=NULL;result=VKD(vkCreatePipelineCache)(vulkan.device,&info,NULL,&metal_pipeline_cache);}
    if(data)munmap(data,size);
    if(result!=VK_SUCCESS){metal_pipeline_cache=VK_NULL_HANDLE;LOG("Vulkan pipeline disk cache unavailable: result=%d",result);}
    else LOG("Vulkan pipeline cache initialized: %zu cached bytes",info.initialDataSize);
}
static void metal_pipeline_request_save(void){
    if(!shader_cache_directory || __atomic_exchange_n(&metal_pipeline_save_pending,1,__ATOMIC_ACQ_REL))return;
    JNIEnv *env=attached_environment("Pipeline cache persistence");static jmethodID method;if(!method){jclass type=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,type,"schedulePipelineCacheSave","()V");(*env)->DeleteLocalRef(env,type);}(*env)->CallVoidMethod(env,activity,method);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);__atomic_store_n(&metal_pipeline_save_pending,0,__ATOMIC_RELEASE);LOG("Vulkan pipeline cache save scheduling failed");}
}
static VkResult metal_cached_compile(unsigned compute,uint32_t count,const void *info,VkPipeline *output){
    pthread_mutex_lock(&metal_pipeline_cache_mutex);metal_pipeline_cache_initialize();VkResult result=vulkan_compile_pipeline(compute,metal_pipeline_cache,count,info,output);
    if(result==VK_SUCCESS && metal_pipeline_cache)++metal_pipeline_generation;pthread_mutex_unlock(&metal_pipeline_cache_mutex);
    if(result==VK_SUCCESS)metal_pipeline_request_save();return result;
}
JNIEXPORT void JNICALL HOST_JNI(HostActivity_persistPipelineCache)(JNIEnv *env,jobject instance){
    pthread_mutex_lock(&metal_pipeline_save_mutex);pthread_mutex_lock(&metal_pipeline_cache_mutex);
    uint64_t generation=metal_pipeline_generation;size_t size=0;void *data=NULL;VkResult result=VK_SUCCESS;
    if(shader_cache_directory && metal_pipeline_cache && generation!=metal_pipeline_saved_generation){result=VKD(vkGetPipelineCacheData)(vulkan.device,metal_pipeline_cache,&size,NULL);if(result==VK_SUCCESS && size){data=malloc(size);if(data)result=VKD(vkGetPipelineCacheData)(vulkan.device,metal_pipeline_cache,&size,data);else result=VK_ERROR_OUT_OF_HOST_MEMORY;}}
    pthread_mutex_unlock(&metal_pipeline_cache_mutex);int saved=0;
    if(data && result==VK_SUCCESS){char *path=NULL;if(asprintf(&path,"%s/pipelines.bin",shader_cache_directory)>=0){
#ifdef I2A_CACHE_STORE
        saved=I2A_CACHE_STORE(path,data,size);
#else
        char *temporary=NULL;if(asprintf(&temporary,"%s/pipelines-XXXXXX.tmp",shader_cache_directory)>=0){int fd=mkstemps(temporary,4);if(fd>=0){size_t done=0;while(done<size){ssize_t written=write(fd,(char *)data+done,size-done);if(written<0 && errno==EINTR)continue;if(written<=0)break;done+=(size_t)written;}int closed=close(fd);if(done==size && !closed && !rename(temporary,path))saved=1;else LOG("Vulkan pipeline cache write failed: %s",strerror(errno));unlink(temporary);}free(temporary);}
#endif
    }free(path);}
    else if(result!=VK_SUCCESS)LOG("Vulkan pipeline cache serialization failed: result=%d",result);
    free(data);pthread_mutex_lock(&metal_pipeline_cache_mutex);if(saved){metal_pipeline_saved_generation=generation;LOG("Vulkan pipeline cache saved: %zu bytes",size);}int changed=saved && generation!=metal_pipeline_generation;__atomic_store_n(&metal_pipeline_save_pending,0,__ATOMIC_RELEASE);pthread_mutex_unlock(&metal_pipeline_cache_mutex);pthread_mutex_unlock(&metal_pipeline_save_mutex);
    if(changed)metal_pipeline_request_save();
}
static struct { uint64_t bits; VkSampler sampler; } metal_literal_samplers[16];
static pthread_mutex_t metal_literal_sampler_mutex=PTHREAD_MUTEX_INITIALIZER;

static VkResult metal_literal_sampler(uint64_t bits,VkSampler *output) {

    if (bits!=0x8000000000080a92ULL && bits!=0x8000000000080000ULL && bits!=0x8000000000080049ULL &&
        bits!=0x8000000000080a49ULL && bits!=0x8000000000088049ULL) return VK_ERROR_FEATURE_NOT_PRESENT;
    pthread_mutex_lock(&metal_literal_sampler_mutex);
    unsigned slot=COUNT(metal_literal_samplers);
    for (unsigned i=0;i<COUNT(metal_literal_samplers);++i) {
        if (metal_literal_samplers[i].bits==bits) { *output=metal_literal_samplers[i].sampler; pthread_mutex_unlock(&metal_literal_sampler_mutex); return VK_SUCCESS; }
        if (!metal_literal_samplers[i].bits && slot==COUNT(metal_literal_samplers)) slot=i;
    }
    if (slot==COUNT(metal_literal_samplers)) { pthread_mutex_unlock(&metal_literal_sampler_mutex); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkSamplerCreateInfo info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=(VkFilter)((bits>>9)&3),.minFilter=(VkFilter)((bits>>11)&3),
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.maxAnisotropy=1,.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,.maxLod=0};
    const VkSamplerAddressMode address[]={VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    info.addressModeU=address[bits&3]; info.addressModeV=address[(bits>>3)&3]; info.addressModeW=address[(bits>>6)&3];
     
    VkResult result=VKD(vkCreateSampler)(vulkan.device,&info,NULL,output);
    if (result==VK_SUCCESS) { metal_literal_samplers[slot].bits=bits; metal_literal_samplers[slot].sampler=*output; }
    pthread_mutex_unlock(&metal_literal_sampler_mutex); return result;
}

static VkFormat metal_pipeline_format(Word pixel) {
     
    if (pixel>=552 && pixel<=555) return VK_FORMAT_R16G16B16A16_SFLOAT;
    for (unsigned i=0;i<COUNT(metal_formats);++i) if (metal_formats[i].metal==pixel) return metal_depth_stencil_backing(metal_formats[i].format);
    return VK_FORMAT_UNDEFINED;
}
static VkFormat metal_vertex_format(Word format) {
    static const VkFormat formats[]={
        VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8B8_UINT,VK_FORMAT_R8G8B8A8_UINT,
        VK_FORMAT_R8G8_SINT,VK_FORMAT_R8G8B8_SINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8B8_SNORM,VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R16G16_UINT,VK_FORMAT_R16G16B16_UINT,VK_FORMAT_R16G16B16A16_UINT,
        VK_FORMAT_R16G16_SINT,VK_FORMAT_R16G16B16_SINT,VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16B16_SNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16B16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_A2B10G10R10_SNORM_PACK32,VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_UNDEFINED,VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SNORM,
        VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_E5B9G9R9_UFLOAT_PACK32
    };
    return format<COUNT(formats) ? formats[format] : VK_FORMAT_UNDEFINED;
}
static int metal_blend_factor(Word value,VkBlendFactor *factor) {
    static const VkBlendFactor factors[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,VK_BLEND_FACTOR_CONSTANT_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
    if (value>=COUNT(factors)) return 0;
    *factor=factors[value]; return 1;
}
static int metal_pipeline_interfaces(struct MetalFunction *vertex,struct MetalFunction *fragment,char *reason,size_t capacity) {
    if (!fragment) return 1;
    for (unsigned i=0;i<fragment->spec->input_count;++i) {
        const struct ShaderInterfaceSpec *input=&shader_inputs[fragment->spec->first_input+i],*output=NULL;
        for (unsigned j=0;j<vertex->spec->output_count;++j) {
            const struct ShaderInterfaceSpec *candidate=&shader_outputs[vertex->spec->first_output+j];
            if (candidate->location==input->location) { output=candidate; break; }
        }
        if (!output || strcmp(input->type,output->type)) {
            snprintf(reason,capacity,"Stage interface mismatch at location %u (%s): %s + %s",input->location,input->type,vertex->spec->name,fragment->spec->name);
            return 0;
        }
    }
    return 1;
}
static int metal_pipeline_vertex_input(struct MetalPipeline *pipeline,struct MetalDescriptor *vertex,char *reason,size_t capacity) {
    unsigned used[31]={0};
    PFN_vkGetPhysicalDeviceFormatProperties properties=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
    for (unsigned i=0;i<pipeline->vertex->spec->input_count;++i) {
        const struct ShaderInterfaceSpec *input=&shader_inputs[pipeline->vertex->spec->first_input+i];
        struct MetalDescriptorArray *array=vertex ? (void *)vertex->attributes : NULL;
        struct MetalDescriptor *attribute=array && input->location<array->count ? (void *)array->items[input->location] : NULL;
        if (!attribute || !metal_property(attribute,"format")) {
            snprintf(reason,capacity,"Missing vertex descriptor for %s location=%u",input->name,input->location); return 0;
        }
        Word binding=metal_property(attribute,"bufferIndex"),offset=metal_property(attribute,"offset");
        VkFormat format=metal_vertex_format(metal_property(attribute,"format"));
        VkFormatProperties supported={0}; if (format) properties(vulkan.physical,format,&supported);
        if (binding>=31 || binding>=vulkan.properties.limits.maxVertexInputBindings || offset>vulkan.properties.limits.maxVertexInputAttributeOffset ||
            input->location>=vulkan.properties.limits.maxVertexInputAttributes || !(supported.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            snprintf(reason,capacity,"Unsupported vertex input %s format=%lu binding=%lu offset=%lu",input->name,metal_property(attribute,"format"),binding,offset); return 0;
        }
        pipeline->attributes[pipeline->attribute_count++]=(VkVertexInputAttributeDescription){input->location,(uint32_t)binding,format,(uint32_t)offset};
        used[binding]=1;
    }
    for (unsigned binding=0;binding<31;++binding) if (used[binding]) {
        struct MetalDescriptorArray *array=vertex ? (void *)vertex->layouts : NULL;
        struct MetalDescriptor *layout=array ? (void *)array->items[binding] : NULL;
        if (!layout) { snprintf(reason,capacity,"Missing vertex buffer layout %u",binding); return 0; }
        Word step=metal_property(layout,"stepFunction"),rate=metal_property(layout,"stepRate"),stride=metal_property(layout,"stride");
        if (step>2 || (step!=0 && rate!=1) || stride>vulkan.properties.limits.maxVertexInputBindingStride) {
            snprintf(reason,capacity,"Unsupported vertex layout binding=%u step=%lu rate=%lu stride=%lu",binding,step,rate,stride); return 0;
        }
        pipeline->bindings[pipeline->binding_count++]=(VkVertexInputBindingDescription){binding,step ? (uint32_t)stride : 0,
            step==2 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    return 1;
}
static VkDescriptorType metal_pipeline_binding_type(const struct MetalPipeline *pipeline,unsigned type) {
    if (type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && pipeline->dynamic_uniform) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    if (type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && pipeline->dynamic_storage) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    return (VkDescriptorType)type;
}
static unsigned metal_descriptor_kind_index(VkDescriptorType type) {
    return type==VK_DESCRIPTOR_TYPE_SAMPLER ? 0 : type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 1 : type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? 2 :
        type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ? 4 : type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ? 5 : 3;
}
static VkResult metal_pipeline_layout(struct MetalPipeline *pipeline) {
    unsigned uniform=0,storage=0;
    for (unsigned stage_index=0;stage_index<2;++stage_index) {
        struct MetalFunction *function=stage_index ? pipeline->fragment : pipeline->vertex;
        for (unsigned i=0;function && i<function->spec->binding_count;++i) {
            unsigned type=shader_bindings[function->spec->first_binding+i].type;
            uniform+=type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; storage+=type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
    }
     
    pipeline->dynamic_uniform=uniform && uniform<=vulkan.properties.limits.maxDescriptorSetUniformBuffersDynamic;
    pipeline->dynamic_storage=storage && storage<=vulkan.properties.limits.maxDescriptorSetStorageBuffersDynamic;
    if (I2A_SHADER_DIAGNOSTIC_COUNT) pipeline->dynamic_storage=0;
    for (unsigned stage_index=0;stage_index<2;++stage_index) {
        struct MetalFunction *function=stage_index ? pipeline->fragment : pipeline->vertex;
        unsigned count=function ? function->spec->binding_count : 0;
        VkDescriptorSetLayoutBinding *bindings=calloc(count ? count : 1,sizeof(*bindings));
        VkSampler *samplers=calloc(count ? count : 1,sizeof(*samplers));
        pipeline->dynamic_slots[stage_index]=calloc(count ? count : 1,sizeof(unsigned));
        if (!bindings || !samplers || !pipeline->dynamic_slots[stage_index]) { free(bindings); free(samplers); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        unsigned dynamic_base=pipeline->dynamic_count;
        for (unsigned i=0;i<count;++i) {
            const struct ShaderBindingSpec *binding=&shader_bindings[function->spec->first_binding+i];
            VkDescriptorType type=metal_pipeline_binding_type(pipeline,binding->type);
            ++pipeline->descriptor_counts[metal_descriptor_kind_index(type)];
            bindings[i]=(VkDescriptorSetLayoutBinding){.binding=binding->binding,.descriptorType=type,
                .descriptorCount=1,.stageFlags=function->compute_fallback ? VK_SHADER_STAGE_COMPUTE_BIT : stage_index ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT};
            if (type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                 
                unsigned slot=dynamic_base+1;
                for (unsigned j=0;j<count;++j) {
                    const struct ShaderBindingSpec *other=&shader_bindings[function->spec->first_binding+j];
                    VkDescriptorType other_type=metal_pipeline_binding_type(pipeline,other->type);
                    if (other->binding<binding->binding && (other_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || other_type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) ++slot;
                }
                pipeline->dynamic_slots[stage_index][i]=slot; ++pipeline->dynamic_count;
            }
            if (binding->sampler_bits) {
                VkResult result=metal_literal_sampler(binding->sampler_bits,&samplers[i]);
                if (result!=VK_SUCCESS) { free(bindings); free(samplers); return result; }
                bindings[i].pImmutableSamplers=&samplers[i];
            }
        }
        VkDescriptorSetLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=count,.pBindings=bindings};
        VkResult result=VKD(vkCreateDescriptorSetLayout)(vulkan.device,&info,NULL,&pipeline->sets[stage_index]);
        free(bindings); free(samplers); if (result!=VK_SUCCESS) return result;
    }
    VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=pipeline->sets};
    return VKD(vkCreatePipelineLayout)(vulkan.device,&info,NULL,&pipeline->layout);
}
static VkResult metal_pipeline_pass(struct MetalPipeline *pipeline) {
    VkAttachmentDescription attachments[9]={0}; VkAttachmentReference colors[8],depth={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    unsigned count=0;
    for (unsigned i=0;i<pipeline->color_count;++i) {
        colors[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        if (!pipeline->colors[i]) continue;
        colors[i].attachment=count;
        attachments[count++]=(VkAttachmentDescription){.format=pipeline->colors[i],.samples=pipeline->samples,
            .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    if (pipeline->depth_format) {
        depth.attachment=count;
        attachments[count++]=(VkAttachmentDescription){.format=pipeline->depth_format,.samples=pipeline->samples,
            .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=pipeline->color_count,.pColorAttachments=colors,
        .pDepthStencilAttachment=pipeline->depth_format ? &depth : NULL};
    VkRenderPassCreateInfo info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=count,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&subpass};
    return VKD(vkCreateRenderPass)(vulkan.device,&info,NULL,&pipeline->pass);
}
static int metal_pipeline_key_equal(const struct MetalPipelineKey *a,const struct MetalPipelineKey *b) {
    return a->topology==b->topology && a->cull==b->cull && a->front==b->front && a->polygon==b->polygon && a->clamp==b->clamp && a->depth==b->depth;
}
static VkResult metal_pipeline_variant(struct MetalPipeline *pipeline,const struct MetalPipelineKey *key,VkPipeline *output) {
    pthread_mutex_lock(&pipeline->mutex);
    for (struct MetalPipelineVariant *variant=pipeline->variants;variant;variant=variant->next) if (metal_pipeline_key_equal(&variant->key,key)) {
        *output=variant->pipeline; pthread_mutex_unlock(&pipeline->mutex); return VK_SUCCESS;
    }
    struct MetalPipelineVariant *variant=calloc(1,sizeof(*variant));
    if (!variant) { pthread_mutex_unlock(&pipeline->mutex); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    variant->key=*key;
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=pipeline->vertex->module,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=pipeline->fragment ? pipeline->fragment->module : VK_NULL_HANDLE,.pName="main"}
    };
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=pipeline->binding_count,.pVertexBindingDescriptions=pipeline->bindings,
        .vertexAttributeDescriptionCount=pipeline->attribute_count,.pVertexAttributeDescriptions=pipeline->attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=key->topology};
    VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable=key->clamp,.rasterizerDiscardEnable=!pipeline->rasterization,.polygonMode=key->polygon,.cullMode=key->cull,.frontFace=key->front,
        .depthBiasEnable=VK_TRUE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo multisample={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=pipeline->samples,.alphaToCoverageEnable=pipeline->alpha_coverage,.alphaToOneEnable=pipeline->alpha_one};
    VkPipelineDepthStencilStateCreateInfo depth=key->depth ? key->depth->state : (VkPipelineDepthStencilStateCreateInfo){
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthCompareOp=VK_COMPARE_OP_ALWAYS,.maxDepthBounds=1};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=pipeline->color_count,.pAttachments=pipeline->blend};
    const VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=COUNT(dynamic_states),.pDynamicStates=dynamic_states};
    VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=pipeline->fragment ? 2 : 1,.pStages=stages,
        .pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,.pMultisampleState=&multisample,
        .pDepthStencilState=&depth,.pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=pipeline->layout,.renderPass=pipeline->pass,.subpass=0};
    VkResult result=metal_cached_compile(0,1,&info,&variant->pipeline);
    if (result==VK_SUCCESS) {
         
        metal_object_retain((Word)variant->key.depth);
        variant->next=pipeline->variants; pipeline->variants=variant; *output=variant->pipeline;
    }
    else { if (variant->pipeline) VKD(vkDestroyPipeline)(vulkan.device,variant->pipeline,NULL); free(variant); }
    pthread_mutex_unlock(&pipeline->mutex); return result;
}
static void metal_pipeline_dispose(struct MetalPipeline *pipeline) {
    if (pipeline->compute_fallback) VKD(vkDestroyPipeline)(vulkan.device,pipeline->compute_fallback,NULL);
    struct MetalPipelineVariant *variant=pipeline->variants;
    while (variant) {
        struct MetalPipelineVariant *next=variant->next;
        VKD(vkDestroyPipeline)(vulkan.device,variant->pipeline,NULL);
        metal_object_release((Word)variant->key.depth); free(variant); variant=next;
    }
    if (pipeline->pass) VKD(vkDestroyRenderPass)(vulkan.device,pipeline->pass,NULL);
    if (pipeline->layout) VKD(vkDestroyPipelineLayout)(vulkan.device,pipeline->layout,NULL);
    for (unsigned i=0;i<2;++i) {
        if (pipeline->sets[i]) VKD(vkDestroyDescriptorSetLayout)(vulkan.device,pipeline->sets[i],NULL);
        free(pipeline->dynamic_slots[i]);
    }
    pthread_mutex_destroy(&pipeline->mutex); free(pipeline);
}
static void metal_pipeline_destroy(Word value) { metal_pipeline_dispose((void *)value); }
static Word metal_pipeline_new(Word device,const char *selector,struct MetalDescriptor *descriptor,Word options,Word *reflection,Word *error) {
    if (error) *error=0; if (reflection) *reflection=0;
    if (!descriptor || descriptor->kind!=MD_PIPELINE || (options&~3UL))
        return metal_error(error,"MTLRenderPipelineErrorDomain",1,"Invalid or unsupported pipeline descriptor/options");
    struct MetalPipeline *pipeline=calloc(1,sizeof(*pipeline));
    if (!pipeline) return metal_error(error,"MTLRenderPipelineErrorDomain",1,"Cannot allocate pipeline");
    pthread_mutex_init(&pipeline->mutex,NULL);
    pipeline->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalRenderPipelineState"),.device=device,
        .label=metal_property(descriptor,"label")};
    pipeline->vertex=(void *)metal_property(descriptor,"vertexFunction"); pipeline->fragment=(void *)metal_property(descriptor,"fragmentFunction");
    char reason[384]="Invalid vertex/fragment function";
    if (!pipeline->vertex || pipeline->vertex->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalFunction") || pipeline->vertex->spec->stage!=1 ||
        (pipeline->fragment && (pipeline->fragment->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalFunction") || pipeline->fragment->spec->stage!=2))) goto failed;
    if (!metal_pipeline_interfaces(pipeline->vertex,pipeline->fragment,reason,sizeof(reason)) ||
        !metal_pipeline_vertex_input(pipeline,(void *)metal_property(descriptor,"vertexDescriptor"),reason,sizeof(reason))) goto failed;
    Word samples=metal_property(descriptor,"sampleCount");
    if (!samples || samples>64 || (samples&(samples-1))) { snprintf(reason,sizeof(reason),"Invalid sample count %lu",samples); goto failed; }
    pipeline->samples=(VkSampleCountFlagBits)samples;
    pipeline->rasterization=metal_property(descriptor,"rasterizationEnabled")!=0;
    pipeline->alpha_coverage=metal_property(descriptor,"alphaToCoverageEnabled")!=0;
    pipeline->alpha_one=metal_property(descriptor,"alphaToOneEnabled")!=0;
    if (pipeline->alpha_one && !vulkan.features.alphaToOne) { snprintf(reason,sizeof(reason),"Alpha-to-one unsupported by Vulkan device"); goto failed; }
    struct MetalDescriptorArray *colors=(void *)descriptor->colors;
    PFN_vkGetPhysicalDeviceFormatProperties query=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
    for (unsigned i=0;colors && i<colors->count;++i) {
        struct MetalDescriptor *color=(void *)colors->items[i]; if (!color || !metal_property(color,"pixelFormat")) continue;
        Word pixel=metal_property(color,"pixelFormat"); VkFormat format=metal_pipeline_format(pixel); VkFormatProperties supported={0};
        if (format) query(vulkan.physical,format,&supported);
        VkPipelineColorBlendAttachmentState *blend=&pipeline->blend[i]; blend->blendEnable=metal_property(color,"blendingEnabled")!=0;
        if (!format || !(supported.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || (blend->blendEnable && !(supported.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))) {
            snprintf(reason,sizeof(reason),"Unsupported pipeline color format %lu at attachment %u",pixel,i); goto failed;
        }
        Word write=metal_property(color,"writeMask"),rgb=metal_property(color,"rgbBlendOperation"),alpha=metal_property(color,"alphaBlendOperation");
        if ((write&~15UL) || rgb>4 || alpha>4 || !metal_blend_factor(metal_property(color,"sourceRGBBlendFactor"),&blend->srcColorBlendFactor) ||
            !metal_blend_factor(metal_property(color,"destinationRGBBlendFactor"),&blend->dstColorBlendFactor) ||
            !metal_blend_factor(metal_property(color,"sourceAlphaBlendFactor"),&blend->srcAlphaBlendFactor) ||
            !metal_blend_factor(metal_property(color,"destinationAlphaBlendFactor"),&blend->dstAlphaBlendFactor)) {
            snprintf(reason,sizeof(reason),"Unsupported color blend state at attachment %u",i); goto failed;
        }
        blend->colorBlendOp=(VkBlendOp)rgb; blend->alphaBlendOp=(VkBlendOp)alpha;
        blend->colorWriteMask=((write&8)>>3)|((write&4)>>1)|((write&2)<<1)|((write&1)<<3);
        pipeline->colors[i]=format; pipeline->color_count=i+1;
    }
    if (pipeline->color_count>vulkan.properties.limits.maxColorAttachments || (pipeline->color_count && !(samples&vulkan.properties.limits.framebufferColorSampleCounts))) {
        snprintf(reason,sizeof(reason),"Color attachment count/sample count exceeds device limits"); goto failed;
    }
    if (!vulkan.features.independentBlend) for (unsigned i=1;i<pipeline->color_count;++i)
        if (memcmp(&pipeline->blend[0],&pipeline->blend[i],sizeof(pipeline->blend[0]))) { snprintf(reason,sizeof(reason),"Independent color blending unavailable"); goto failed; }
    Word depth=metal_property(descriptor,"depthAttachmentPixelFormat"),stencil=metal_property(descriptor,"stencilAttachmentPixelFormat");
    if (depth && stencil && depth!=stencil) { snprintf(reason,sizeof(reason),"Separate depth and stencil formats require conversion"); goto failed; }
    if (depth || stencil) {
        pipeline->depth_format=metal_pipeline_format(depth ? depth : stencil); VkFormatProperties supported={0};
        if (pipeline->depth_format) query(vulkan.physical,pipeline->depth_format,&supported);
        if (!(supported.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) || (depth && !(samples&vulkan.properties.limits.framebufferDepthSampleCounts)) ||
            (stencil && !(samples&vulkan.properties.limits.framebufferStencilSampleCounts))) {
            snprintf(reason,sizeof(reason),"Unsupported depth/stencil format %lu/%lu samples=%lu",depth,stencil,samples); goto failed;
        }
    }
    VkResult result=metal_pipeline_layout(pipeline);
    if (pipeline->vertex->compute_fallback) {
        if (pipeline->rasterization || pipeline->fragment || pipeline->binding_count) {
            snprintf(reason,sizeof(reason),"Compute substitution requires a verified non-rasterizing vertex pipeline"); goto failed;
        }
        VkComputePipelineCreateInfo compute={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=pipeline->vertex->module,.pName="main"},.layout=pipeline->layout};
        if (result==VK_SUCCESS) result=metal_cached_compile(1,1,&compute,&pipeline->compute_fallback);
    } else if (result==VK_SUCCESS) result=metal_pipeline_pass(pipeline);
    struct MetalPipelineKey key={.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.front=VK_FRONT_FACE_CLOCKWISE,.polygon=VK_POLYGON_MODE_FILL};
    VkPipeline initial=VK_NULL_HANDLE;
    if (result==VK_SUCCESS && !pipeline->compute_fallback) result=metal_pipeline_variant(pipeline,&key,&initial);
    if (result!=VK_SUCCESS) { snprintf(reason,sizeof(reason),"Vulkan pipeline creation failed result=%d (%s)",result,pipeline->vertex->spec->name); goto failed; }
    if (reflection && (options&3)) {
        struct MetalPipelineReflection *info=calloc(1,sizeof(*info));
        if (!info) { snprintf(reason,sizeof(reason),"Cannot allocate pipeline reflection"); goto failed; }
        info->isa=external_class("_OBJC_CLASS_$_MTLRenderPipelineReflection"); info->vertex=pipeline->vertex->arguments;
        info->fragment=pipeline->fragment ? pipeline->fragment->arguments : new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,NULL,0);
        *reflection=(Word)info;
    }
    metal_lifetime_init((Word)pipeline,__builtin_offsetof(struct MetalPipeline,lifetime),metal_pipeline_destroy);
    LOG("Vulkan graphics pipeline created: %s + %s; vertex inputs=%u colors=%u samples=%u",pipeline->vertex->spec->name,
        pipeline->fragment ? pipeline->fragment->spec->name : "(no fragment)",pipeline->attribute_count,pipeline->color_count,pipeline->samples);
    return (Word)pipeline;
failed:
    metal_pipeline_dispose(pipeline); return metal_error(error,"MTLRenderPipelineErrorDomain",1,reason);
}
static Word metal_pipeline_simple(Word device,const char *selector,struct MetalDescriptor *descriptor,Word *error) { return metal_pipeline_new(device,selector,descriptor,0,NULL,error); }
static Word metal_pipeline_reflection_arguments(struct MetalPipelineReflection *info,const char *selector) { return !strcmp(selector,"vertexArguments") ? info->vertex : info->fragment; }
static Method metal_pipeline_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice")) {
        if (!strcmp(selector,"newRenderPipelineStateWithDescriptor:options:reflection:error:")) return (Method)metal_pipeline_new;
        if (!strcmp(selector,"newRenderPipelineStateWithDescriptor:error:")) return (Method)metal_pipeline_simple;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalRenderPipelineState")) {
        if (!strcmp(selector,"device")) return (Method)metal_resource_device;
        if (!strcmp(selector,"label")) return (Method)metal_resource_label;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_MTLRenderPipelineReflection") && (!strcmp(selector,"vertexArguments") || !strcmp(selector,"fragmentArguments"))) return (Method)metal_pipeline_reflection_arguments;
    return NULL;
}
#undef VKD
