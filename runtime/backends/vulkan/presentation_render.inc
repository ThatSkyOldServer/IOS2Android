#include "presentation-shaders.inc"
struct MetalPresentationRenderer {
    VkRenderPass pass;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkSampler sampler;
    VkDescriptorPool pool;
    unsigned count;
    VkImageView *views,*sources;
    VkFramebuffer *frames;
    VkDescriptorSet *sets;
};
static void metal_presentation_renderer_destroy(struct MetalPresentationRenderer *state) {
    for(unsigned i=0;i<state->count;++i) {
        if(state->frames && state->frames[i])VKD(vkDestroyFramebuffer)(vulkan.device,state->frames[i],NULL);
        if(state->views && state->views[i])VKD(vkDestroyImageView)(vulkan.device,state->views[i],NULL);
    }
    if(state->pipeline)VKD(vkDestroyPipeline)(vulkan.device,state->pipeline,NULL);
    if(state->pool)VKD(vkDestroyDescriptorPool)(vulkan.device,state->pool,NULL);
    if(state->layout)VKD(vkDestroyPipelineLayout)(vulkan.device,state->layout,NULL);
    if(state->set_layout)VKD(vkDestroyDescriptorSetLayout)(vulkan.device,state->set_layout,NULL);
    if(state->sampler)VKD(vkDestroySampler)(vulkan.device,state->sampler,NULL);
    if(state->pass)VKD(vkDestroyRenderPass)(vulkan.device,state->pass,NULL);
    free(state->views);free(state->sources);free(state->frames);free(state->sets);
    memset(state,0,sizeof(*state));
}
static VkResult metal_presentation_renderer_create(struct MetalPresentationRenderer *state,VkFormat format,
        const VkImage *images,unsigned count,VkExtent2D extent,const char **operation) {
    VkResult result=VK_SUCCESS;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkDescriptorSetLayout *layouts=NULL;
    state->count=count;
    state->views=calloc(count,sizeof(*state->views));state->sources=calloc(count,sizeof(*state->sources));
    state->frames=calloc(count,sizeof(*state->frames));state->sets=calloc(count,sizeof(*state->sets));
    layouts=calloc(count,sizeof(*layouts));
    if(!state->views || !state->sources || !state->frames || !state->sets || !layouts) {
        *operation="presentation image storage";result=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;
    }
#define PRESENT_CREATE(name,...) do { *operation=#name; result=VKD(name)(__VA_ARGS__); if(result!=VK_SUCCESS)goto failed; } while(0)
    VkAttachmentDescription attachment={.format=format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
    PRESENT_CREATE(vkCreateRenderPass,vulkan.device,&pass,NULL,&state->pass);
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    PRESENT_CREATE(vkCreateDescriptorSetLayout,vulkan.device,&set,NULL,&state->set_layout);
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,8};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&state->set_layout,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    PRESENT_CREATE(vkCreatePipelineLayout,vulkan.device,&layout,NULL,&state->layout);
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    PRESENT_CREATE(vkCreateSampler,vulkan.device,&sampler,NULL,&state->sampler);
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(metal_present_vert),.pCode=metal_present_vert};
    PRESENT_CREATE(vkCreateShaderModule,vulkan.device,&shader,NULL,&vertex);
    shader.codeSize=sizeof(metal_present_frag);shader.pCode=metal_present_frag;
    PRESENT_CREATE(vkCreateShaderModule,vulkan.device,&shader,NULL,&fragment);
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,
        .cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState output={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&output};
    const VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic_states};
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,
        .pMultisampleState=&samples,.pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=state->layout,.renderPass=state->pass,.basePipelineIndex=-1};
    *operation="vkCreateGraphicsPipelines presentation";
    result=vulkan_compile_pipeline(0,VK_NULL_HANDLE,1,&pipeline,&state->pipeline);if(result!=VK_SUCCESS)goto failed;
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,count};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=count,.poolSizeCount=1,.pPoolSizes=&size};
    PRESENT_CREATE(vkCreateDescriptorPool,vulkan.device,&pool,NULL,&state->pool);
    for(unsigned i=0;i<count;++i)layouts[i]=state->set_layout;
    VkDescriptorSetAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=state->pool,.descriptorSetCount=count,.pSetLayouts=layouts};
    PRESENT_CREATE(vkAllocateDescriptorSets,vulkan.device,&allocate,state->sets);
    free(layouts);layouts=NULL;
    for(unsigned i=0;i<count;++i) {
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        PRESENT_CREATE(vkCreateImageView,vulkan.device,&view,NULL,&state->views[i]);
        VkFramebufferCreateInfo frame={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=state->pass,.attachmentCount=1,.pAttachments=&state->views[i],
            .width=extent.width,.height=extent.height,.layers=1};
        PRESENT_CREATE(vkCreateFramebuffer,vulkan.device,&frame,NULL,&state->frames[i]);
    }
    VKD(vkDestroyShaderModule)(vulkan.device,vertex,NULL);VKD(vkDestroyShaderModule)(vulkan.device,fragment,NULL);
    return VK_SUCCESS;
failed:
    free(layouts);
    if(vertex)VKD(vkDestroyShaderModule)(vulkan.device,vertex,NULL);
    if(fragment)VKD(vkDestroyShaderModule)(vulkan.device,fragment,NULL);
    metal_presentation_renderer_destroy(state);return result;
#undef PRESENT_CREATE
}
static void metal_presentation_renderer_record(struct MetalPresentationRenderer *state,struct MetalCommand *command,
        struct MetalTexture *texture,unsigned index,VkExtent2D extent,VkFormat format,VkSurfaceTransformFlagBitsKHR transform) {
    VkDescriptorImageInfo image={state->sampler,texture->sampled_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=state->sets[index],.dstBinding=0,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
    if(state->sources[index]!=texture->sampled_view) {
        VKD(vkUpdateDescriptorSets)(vulkan.device,1,&write,0,NULL);state->sources[index]=texture->sampled_view;
    }
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=state->pass,.framebuffer=state->frames[index],.renderArea={{0,0},extent}};
    VKD(vkCmdBeginRenderPass)(command->command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    VKD(vkCmdBindPipeline)(command->command,VK_PIPELINE_BIND_POINT_GRAPHICS,state->pipeline);
    VKD(vkCmdBindDescriptorSets)(command->command,VK_PIPELINE_BIND_POINT_GRAPHICS,state->layout,0,1,&state->sets[index],0,NULL);
    uint32_t push[2]={format!=VK_FORMAT_B8G8R8A8_SRGB && format!=VK_FORMAT_R8G8B8A8_SRGB,(uint32_t)transform};
    VKD(vkCmdPushConstants)(command->command,state->layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),push);
    VkViewport viewport={0,0,(float)extent.width,(float)extent.height,0,1};VkRect2D scissor={{0,0},extent};
    VKD(vkCmdSetViewport)(command->command,0,1,&viewport);VKD(vkCmdSetScissor)(command->command,0,1,&scissor);
    VKD(vkCmdDraw)(command->command,3,1,0,0);VKD(vkCmdEndRenderPass)(command->command);
}
