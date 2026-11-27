/* Resolve only at Metal endEncoding; internal pass breaks must keep MSAA data. */
struct MetalDepthResolveKey {
    VkImageViewCreateInfo source,target;
    VkSampleCountFlagBits samples;
    VkResolveModeFlagBits depth,stencil;
    uint32_t width,height;
};
struct MetalDepthResolveBundle {
    struct MetalDepthResolveKey key;
    VkImageView views[2];VkRenderPass pass;VkFramebuffer framebuffer;
    struct MetalDepthResolveBundle *next;
};
struct MetalRenderResolve {
    unsigned attachment;
    struct MetalTexture *target;
    VkImageViewCreateInfo view;
    VkResolveModeFlagBits depth,stencil;
    struct MetalDepthResolveBundle *bundle;
    struct MetalRenderResolve *next;
};
static pthread_mutex_t metal_resolve_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct MetalDepthResolveBundle *metal_resolve_cache;
static unsigned metal_resolve_count;
static void metal_resolve_bundle_destroy(struct MetalDepthResolveBundle *bundle) {
    if (bundle->framebuffer) VKD(vkDestroyFramebuffer)(vulkan.device,bundle->framebuffer,NULL);
    if (bundle->pass) VKD(vkDestroyRenderPass)(vulkan.device,bundle->pass,NULL);
    for (unsigned i=0;i<2;++i) if (bundle->views[i]) VKD(vkDestroyImageView)(vulkan.device,bundle->views[i],NULL);
    metal_memory_sub(3,sizeof(*bundle));free(bundle);
}
static void metal_resolve_cache_invalidate(struct MetalTexture *texture) {
    pthread_mutex_lock(&metal_resolve_mutex);
    struct MetalDepthResolveBundle **slot=&metal_resolve_cache;
    while (*slot) {
        struct MetalDepthResolveBundle *bundle=*slot;
        if (bundle->key.source.image==texture->image || bundle->key.target.image==texture->image) {
            *slot=bundle->next;--metal_resolve_count;metal_resolve_bundle_destroy(bundle);
        } else slot=&bundle->next;
    }
    pthread_mutex_unlock(&metal_resolve_mutex);
}
static struct MetalDepthResolveBundle *metal_resolve_bundle_acquire(struct MetalRenderEncoder *encoder,struct MetalRenderResolve *resolve) {
    struct MetalDepthResolveKey key={0};
    key.source=encoder->state->view_descriptions[resolve->attachment];key.target=resolve->view;
    key.samples=encoder->state->samples;key.depth=resolve->depth;key.stencil=resolve->stencil;
    key.width=encoder->state->width;key.height=encoder->state->height;
    if (!vulkan.create_render_pass2 || (key.depth&vulkan.depth_resolve.supportedDepthResolveModes)!=key.depth ||
        (key.stencil&vulkan.depth_resolve.supportedStencilResolveModes)!=key.stencil)
        metal_render_stop("Requested depth/stencil resolve mode is unavailable","endEncoding");
    if ((key.source.subresourceRange.aspectMask&(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT))==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT) && key.depth!=key.stencil &&
        (!vulkan.depth_resolve.independentResolve && !((!key.depth || !key.stencil) && vulkan.depth_resolve.independentResolveNone)))
        metal_render_stop("Independent depth/stencil resolve modes are unavailable","endEncoding");
    pthread_mutex_lock(&metal_resolve_mutex);
    struct MetalDepthResolveBundle **slot=&metal_resolve_cache;
    while (*slot && memcmp(&(*slot)->key,&key,sizeof(key))) slot=&(*slot)->next;
    struct MetalDepthResolveBundle *bundle=*slot;
    if (bundle) { *slot=bundle->next;bundle->next=NULL;--metal_resolve_count; }
    pthread_mutex_unlock(&metal_resolve_mutex);
    if (bundle) return bundle;
    bundle=calloc(1,sizeof(*bundle));if (!bundle) metal_render_stop("Cannot allocate depth resolve bundle","endEncoding");
    metal_memory_add(3,sizeof(*bundle));bundle->key=key;
    VkResult result=VKD(vkCreateImageView)(vulkan.device,&key.source,NULL,&bundle->views[0]);
    if (result==VK_SUCCESS) result=VKD(vkCreateImageView)(vulkan.device,&key.target,NULL,&bundle->views[1]);
    VkAttachmentDescription2 attachments[2]={
        {.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,.format=key.source.format,.samples=key.samples,
         .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
        {.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,.format=key.target.format,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=key.depth ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp=key.stencil ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference2 source={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=0,.layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.aspectMask=key.source.subresourceRange.aspectMask};
    VkAttachmentReference2 target={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=1,.layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.aspectMask=key.target.subresourceRange.aspectMask};
    VkSubpassDescriptionDepthStencilResolve depth={.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode=key.depth,.stencilResolveMode=key.stencil,.pDepthStencilResolveAttachment=&target};
    VkSubpassDescription2 subpass={.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,.pNext=&depth,.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.pDepthStencilAttachment=&source};
    VkSubpassDependency2 dependency={.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,
        .srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.dstStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo2 pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,.attachmentCount=2,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
    if (result==VK_SUCCESS) result=vulkan.create_render_pass2(vulkan.device,&pass,NULL,&bundle->pass);
    VkFramebufferCreateInfo frame={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=bundle->pass,.attachmentCount=2,.pAttachments=bundle->views,.width=key.width,.height=key.height,.layers=1};
    if (result==VK_SUCCESS) result=VKD(vkCreateFramebuffer)(vulkan.device,&frame,NULL,&bundle->framebuffer);
    if (result!=VK_SUCCESS) { metal_resolve_bundle_destroy(bundle);metal_render_result(result,"depth/stencil resolve setup"); }
    return bundle;
}
static void metal_render_resolve_attachment(struct MetalRenderEncoder *encoder,struct MetalDescriptor *descriptor,unsigned index,VkImageAspectFlags aspect) {
    Word store=metal_property(descriptor,"storeAction");
    if (store<2) return;
    struct MetalTexture *source=encoder->state->attachments[index],*target=(void *)metal_property(descriptor,"resolveTexture");
    Word level=metal_property(descriptor,"resolveLevel"),slice=metal_property(descriptor,"resolveSlice");
    if (source->samples<=1 || !target || target->object.isa!=external_class("_OBJC_CLASS_$_I2AMetalTexture") ||
        target==source || target->samples!=1 || target->format.format!=source->format.format || level>=target->mips || slice>=target->layers ||
        target->type==7 || metal_property(descriptor,"resolveDepthPlane") || !(source->format.aspects&aspect) || !(target->format.aspects&aspect) ||
        (aspect!=VK_IMAGE_ASPECT_COLOR_BIT && !(target->native_usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        metal_render_stop("Invalid multisample resolve target","renderCommandEncoderWithDescriptor:");
    uint32_t source_level=encoder->state->view_descriptions[index].subresourceRange.baseMipLevel;
    Word width=target->width>>level,height=target->height>>level;if (!width) width=1;if (!height) height=1;
    if (width!=(source->width>>source_level) || height!=(source->height>>source_level)) metal_render_stop("Resolve target dimensions differ from source","renderCommandEncoderWithDescriptor:");
    struct MetalRenderResolve *resolve=encoder->state->resolves;
    while (resolve && resolve->attachment!=index) resolve=resolve->next;
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=target->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=target->format.format,
        .subresourceRange={target->format.aspects,(uint32_t)level,1,(uint32_t)slice,1}};
    if (resolve && (resolve->target!=target || memcmp(&resolve->view,&view,sizeof(view)))) metal_render_stop("Separate depth/stencil resolve targets require conversion","renderCommandEncoderWithDescriptor:");
    if (!resolve) {
        resolve=calloc(1,sizeof(*resolve));if (!resolve) metal_render_stop("Cannot allocate resolve state","renderCommandEncoderWithDescriptor:");
        resolve->attachment=index;resolve->target=target;resolve->view=view;
        resolve->next=encoder->state->resolves;encoder->state->resolves=resolve;
        metal_command_texture(encoder->command,target);
    }
    if (aspect==VK_IMAGE_ASPECT_DEPTH_BIT) {
        Word mode=metal_property(descriptor,"depthResolveFilter");
        const VkResolveModeFlagBits modes[]={VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,VK_RESOLVE_MODE_MIN_BIT,VK_RESOLVE_MODE_MAX_BIT};
        if (mode>=COUNT(modes)) metal_render_stop("Unsupported Metal depth resolve filter","renderCommandEncoderWithDescriptor:");
        resolve->depth=modes[mode];
    } else if (aspect==VK_IMAGE_ASPECT_STENCIL_BIT) {
        Word mode=metal_property(descriptor,"stencilResolveFilter");
        if (mode>1 || (mode==1 && resolve->depth!=VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))
            metal_render_stop("Stencil depth-selected sample resolve is unavailable for this depth filter","renderCommandEncoderWithDescriptor:");
        resolve->stencil=VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    }
}
static void metal_render_resolve(struct MetalRenderEncoder *encoder) {
    for (struct MetalRenderResolve *resolve=encoder->state->resolves;resolve;resolve=resolve->next) {
        struct MetalTexture *source=encoder->state->attachments[resolve->attachment],*target=resolve->target;
        if (source->format.aspects==VK_IMAGE_ASPECT_COLOR_BIT) {
            metal_render_transition(encoder,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT);
            metal_render_transition(encoder,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageSubresourceRange from=encoder->state->view_descriptions[resolve->attachment].subresourceRange,to=resolve->view.subresourceRange;
            VkImageResolve region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,from.baseMipLevel,from.baseArrayLayer,1},
                .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,to.baseMipLevel,to.baseArrayLayer,1},.extent={encoder->state->width,encoder->state->height,1}};
            VKD(vkCmdResolveImage)(encoder->command->command,source->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        } else {
            metal_render_transition(encoder,source,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
            metal_render_transition(encoder,target,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            resolve->bundle=metal_resolve_bundle_acquire(encoder,resolve);
            VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=resolve->bundle->pass,.framebuffer=resolve->bundle->framebuffer,
                .renderArea={{0,0},{encoder->state->width,encoder->state->height}}};
            VKD(vkCmdBeginRenderPass)(encoder->command->command,&begin,VK_SUBPASS_CONTENTS_INLINE);VKD(vkCmdEndRenderPass)(encoder->command->command);
        }
        ++encoder->command->recorded_operations;
    }
}
static void metal_render_resolve_cleanup(struct MetalRenderEncoder *encoder) {
    while (encoder->state->resolves) {
        struct MetalRenderResolve *resolve=encoder->state->resolves;encoder->state->resolves=resolve->next;
        struct MetalDepthResolveBundle *bundle=resolve->bundle;
        if (bundle) {
            pthread_mutex_lock(&metal_resolve_mutex);
            if (metal_resolve_count<64) { bundle->next=metal_resolve_cache;metal_resolve_cache=bundle;++metal_resolve_count;bundle=NULL; }
            pthread_mutex_unlock(&metal_resolve_mutex);if (bundle) metal_resolve_bundle_destroy(bundle);
        }
        free(resolve);
    }
}
