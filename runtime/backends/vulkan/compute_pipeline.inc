/* Workgroup defaults and specialization IDs come from the compiled module. */
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalComputeVariant { uint32_t size[3]; VkPipeline pipeline; struct MetalComputeVariant *next; };
struct MetalCompute {
    struct MetalResource object;
    struct MetalFunction *function;
    struct MetalDescriptorLayouts *descriptor_layouts;
    VkPipelineLayout layout;
    pthread_mutex_t mutex;
    struct MetalComputeVariant *variants;
    struct MetalLifetime lifetime;
};
static VkResult metal_compute_variant(struct MetalCompute *compute,const uint32_t size[3],VkPipeline *output) {
    uint64_t count=1;
    for (unsigned i=0;i<3;++i) {
        if (!size[i] || size[i]>vulkan.properties.limits.maxComputeWorkGroupSize[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (compute->function->spec->workgroup_ids[i]==UINT32_MAX && size[i]!=compute->function->spec->workgroup[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (count>vulkan.properties.limits.maxComputeWorkGroupInvocations/size[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        count*=size[i];
    }
    if (count>vulkan.properties.limits.maxComputeWorkGroupInvocations) return VK_ERROR_FEATURE_NOT_PRESENT;
    pthread_mutex_lock(&compute->mutex);
    for (struct MetalComputeVariant *variant=compute->variants;variant;variant=variant->next)
        if (!memcmp(variant->size,size,sizeof(variant->size))) { *output=variant->pipeline; pthread_mutex_unlock(&compute->mutex); return VK_SUCCESS; }
    struct MetalComputeVariant *variant=calloc(1,sizeof(*variant));
    if (!variant) { pthread_mutex_unlock(&compute->mutex); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memcpy(variant->size,size,sizeof(variant->size));
    VkSpecializationMapEntry map[3]; unsigned entries=0;
    for (unsigned i=0;i<3;++i) if (compute->function->spec->workgroup_ids[i]!=UINT32_MAX) {
        uint32_t id=compute->function->spec->workgroup_ids[i]; unsigned j=0;
        for (;j<entries;++j) if (map[j].constantID==id) break;
        if (j<entries) {
            if (size[map[j].offset/sizeof(uint32_t)]!=size[i]) { free(variant);pthread_mutex_unlock(&compute->mutex);return VK_ERROR_FEATURE_NOT_PRESENT; }
        } else map[entries++]=(VkSpecializationMapEntry){id,i*sizeof(uint32_t),sizeof(uint32_t)};
    }
    VkSpecializationInfo specialization={.mapEntryCount=entries,.pMapEntries=map,.dataSize=3*sizeof(uint32_t),.pData=size};
    VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=compute->layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=compute->function->module,.pName=compute->function->spec->spirv_entry,.pSpecializationInfo=&specialization}};
    VkResult result=metal_cached_compile(1,1,&info,&variant->pipeline);
    if (result==VK_SUCCESS) { variant->next=compute->variants; compute->variants=variant; *output=variant->pipeline; }
    else { if (variant->pipeline) VKD(vkDestroyPipeline)(vulkan.device,variant->pipeline,NULL); free(variant); }
    pthread_mutex_unlock(&compute->mutex); return result;
}
static void metal_compute_dispose(struct MetalCompute *compute) {
    struct MetalComputeVariant *variant=compute->variants;
    while (variant) { struct MetalComputeVariant *next=variant->next; VKD(vkDestroyPipeline)(vulkan.device,variant->pipeline,NULL); free(variant); variant=next; }
    if (compute->layout) VKD(vkDestroyPipelineLayout)(vulkan.device,compute->layout,NULL);
    metal_descriptor_layouts_release(compute->descriptor_layouts);
    pthread_mutex_destroy(&compute->mutex); free(compute);
}
static void metal_compute_destroy(Word value) { metal_compute_dispose((void *)value); }
static Word metal_compute_new(Word device,const char *selector,struct MetalFunction *function,Word *error) {
    if (error) *error=0;
    if (!function || function->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalFunction") || function->spec->stage!=3)
        return metal_error(error,"MTLComputePipelineErrorDomain",1,"Invalid compute function");
    struct MetalCompute *compute=calloc(1,sizeof(*compute));
    if (!compute) return metal_error(error,"MTLComputePipelineErrorDomain",1,"Cannot allocate compute pipeline");
    pthread_mutex_init(&compute->mutex,NULL);
    compute->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalComputePipelineState"),.device=device};
    compute->function=function; VkResult result=VK_SUCCESS;
    compute->descriptor_layouts=calloc(1,sizeof(*compute->descriptor_layouts));
    if (!compute->descriptor_layouts) { metal_compute_dispose(compute);return metal_error(error,"MTLComputePipelineErrorDomain",1,"Cannot allocate compute layouts"); }
    compute->descriptor_layouts->references=1;
    for (unsigned stage=0;stage<3 && result==VK_SUCCESS;++stage) {
        unsigned count=stage==2 ? function->spec->binding_count : 0;
        VkDescriptorSetLayoutBinding *bindings=calloc(count+1,sizeof(*bindings));
        VkSampler *samplers=calloc(count+1,sizeof(*samplers));
        if (!bindings || !samplers) { free(bindings); free(samplers); result=VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        for (unsigned i=0;i<count;++i) {
            const struct ShaderBindingSpec *spec=&shader_bindings[function->spec->first_binding+i];
            bindings[i]=(VkDescriptorSetLayoutBinding){.binding=spec->binding,.descriptorType=(VkDescriptorType)spec->type,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
            if (spec->literal_sampler) {
                result=metal_literal_sampler(spec->literal_sampler,&samplers[i]);
                if (result!=VK_SUCCESS) break;
                bindings[i].pImmutableSamplers=&samplers[i];
            }
        }
        VkDescriptorSetLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=count,.pBindings=bindings};
        if (result==VK_SUCCESS) result=VKD(vkCreateDescriptorSetLayout)(vulkan.device,&info,NULL,&compute->descriptor_layouts->sets[stage]);
        free(bindings); free(samplers);
    }
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=3,.pSetLayouts=compute->descriptor_layouts->sets};
    if (result==VK_SUCCESS) result=VKD(vkCreatePipelineLayout)(vulkan.device,&layout,NULL,&compute->layout);
    VkPipeline pipeline=VK_NULL_HANDLE;
    if (result==VK_SUCCESS) result=metal_compute_variant(compute,function->spec->workgroup,&pipeline);
    if (result!=VK_SUCCESS) {
        char detail[256]; snprintf(detail,sizeof(detail),"Vulkan compute pipeline failed result=%d (%s)",result,function->spec->name);
        metal_compute_dispose(compute); return metal_error(error,"MTLComputePipelineErrorDomain",1,detail);
    }
    LOG("Vulkan compute pipeline created: %s; workgroup dimensions await original dispatch",function->spec->name);
    metal_lifetime_init((Word)compute,__builtin_offsetof(struct MetalCompute,lifetime),metal_compute_destroy);
    return (Word)compute;
}
struct MetalComputeReflection { Word isa,arguments; };
static Word metal_compute_new_reflection(Word device,const char *selector,struct MetalFunction *function,Word options,Word *reflection,Word *error) {
    if (reflection) *reflection=0;
    if (options&~3UL) return metal_error(error,"MTLComputePipelineErrorDomain",1,"Unsupported compute reflection options");
    Word pipeline=metal_compute_new(device,selector,function,error);
    if (pipeline && reflection && options) {
        struct MetalComputeReflection *info=calloc(1,sizeof(*info));
        if (!info) { metal_object_release(pipeline);return metal_error(error,"MTLComputePipelineErrorDomain",1,"Cannot allocate compute reflection"); }
        info->isa=external_class("_OBJC_CLASS_$_MTLComputePipelineReflection");info->arguments=function->arguments;*reflection=(Word)info;
    }
    return pipeline;
}
static Word metal_compute_reflection_arguments(struct MetalComputeReflection *info) { return info->arguments; }
static Word metal_compute_max_threads(void) { return vulkan.properties.limits.maxComputeWorkGroupInvocations; }
static Method metal_compute_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice") && !strcmp(selector,"newComputePipelineStateWithFunction:error:")) return (Method)metal_compute_new;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice") && !strcmp(selector,"newComputePipelineStateWithFunction:options:reflection:error:")) return (Method)metal_compute_new_reflection;
    if (!strcmp(receiver,"_OBJC_CLASS_$_MTLComputePipelineReflection") && !strcmp(selector,"arguments")) return (Method)metal_compute_reflection_arguments;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalComputePipelineState")) {
        if (!strcmp(selector,"device")) return (Method)metal_resource_device;
        if (!strcmp(selector,"label")) return (Method)metal_resource_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
        if (!strcmp(selector,"maxTotalThreadsPerThreadgroup")) return (Method)metal_compute_max_threads;
    }
    return NULL;
}
#undef VKD
