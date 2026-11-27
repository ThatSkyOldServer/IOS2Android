 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
#include "presentation_render.inc"
struct MetalDrawable { Word isa; struct MetalTexture *texture; uint32_t index; unsigned presented,scheduled,retired; Word identifier; VkResult result; struct MetalLifetime lifetime; };
static void metal_drawable_retire(struct MetalDrawable *);
static void metal_drawable_destroy(Word value) {
    struct MetalDrawable *drawable=(void *)value;
    metal_drawable_retire(drawable);
    metal_object_release((Word)drawable->texture); free(drawable);
}
static struct {
    pthread_mutex_t mutex;
    VkSwapchainKHR swapchain;
    VkFence acquire_fence;
    VkFormat format;
    VkFormat source_format;
    Word source_pixel_format;
    VkExtent2D extent;
    VkSurfaceTransformFlagBitsKHR transform,current_transform;
    VkSurfaceTransformFlagsKHR supported_transforms;
    VkImageUsageFlags supported_usage;
    struct MetalPresentationRenderer renderer;
    uint32_t count;
    VkImage *images;
    VkSemaphore *finished;
    unsigned *presented;
    struct MetalTexture **backings;
    Word next_identifier;
    unsigned outstanding,dirty;
    VkResult last_failure;
    const char *last_operation;
} metal_presentation={.mutex=PTHREAD_MUTEX_INITIALIZER};

static void metal_drawable_retire(struct MetalDrawable *drawable) {
    if (__atomic_load_n(&drawable->retired,__ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&metal_presentation.mutex);
    if (!__atomic_load_n(&drawable->retired,__ATOMIC_RELAXED)) {
        /* An acquired image that was abandoned needs a new swapchain before
         * another acquisition can reuse its slot. Completion retires it once. */
        if (!drawable->presented || (drawable->result!=VK_SUCCESS && drawable->result!=VK_SUBOPTIMAL_KHR))
            __atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);
        if (metal_presentation.outstanding) --metal_presentation.outstanding;
        __atomic_store_n(&drawable->retired,1,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&metal_presentation.mutex);
}

static VkResult metal_surface_refresh(void) {
    pthread_mutex_lock(&graphics.mutex);
    ANativeWindow *window=graphics.window; unsigned generation=graphics.window_generation;
    if (window) ANativeWindow_acquire(window);
    pthread_mutex_unlock(&graphics.mutex);
    if (!window) return VK_NOT_READY;
    if (generation==vulkan.window_generation && !__atomic_load_n(&metal_presentation.dirty,__ATOMIC_ACQUIRE)) { ANativeWindow_release(window); return VK_SUCCESS; }
    if (metal_presentation.outstanding) { ANativeWindow_release(window); return VK_NOT_READY; }
    pthread_mutex_lock(&vulkan_mutex);
    VkResult result=VKD(vkQueueWaitIdle)(vulkan.queue);
    if (result==VK_SUCCESS) {
        metal_presentation_renderer_destroy(&metal_presentation.renderer);
        if (metal_presentation.swapchain) VKD(vkDestroySwapchainKHR)(vulkan.device,metal_presentation.swapchain,NULL);
        if (metal_presentation.acquire_fence) VKD(vkDestroyFence)(vulkan.device,metal_presentation.acquire_fence,NULL);
        for (unsigned i=0;i<metal_presentation.count;++i) {
            struct MetalTexture *backing=metal_presentation.backings[i];
            metal_presentation.backings[i]=NULL; metal_object_release((Word)backing);
            if (metal_presentation.finished[i]) VKD(vkDestroySemaphore)(vulkan.device,metal_presentation.finished[i],NULL);
            metal_presentation.finished[i]=VK_NULL_HANDLE; metal_presentation.presented[i]=0; metal_presentation.images[i]=VK_NULL_HANDLE;
        }
        free(metal_presentation.images);metal_presentation.images=NULL;
        free(metal_presentation.finished);metal_presentation.finished=NULL;
        free(metal_presentation.presented);metal_presentation.presented=NULL;
        free(metal_presentation.backings);metal_presentation.backings=NULL;
        metal_presentation.swapchain=VK_NULL_HANDLE; metal_presentation.acquire_fence=VK_NULL_HANDLE; metal_presentation.count=0;
        PFN_vkDestroySurfaceKHR destroy=(PFN_vkDestroySurfaceKHR)vulkan.get_instance_proc(vulkan.instance,"vkDestroySurfaceKHR");
        if (vulkan.surface) destroy(vulkan.instance,vulkan.surface,NULL);
        vulkan.surface=VK_NULL_HANDLE;
        if (vulkan.window) ANativeWindow_release(vulkan.window);
        vulkan.window=window; window=NULL;
        PFN_vkCreateAndroidSurfaceKHR create=(PFN_vkCreateAndroidSurfaceKHR)vulkan.get_instance_proc(vulkan.instance,"vkCreateAndroidSurfaceKHR");
        VkAndroidSurfaceCreateInfoKHR info={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=vulkan.window};
        result=create(vulkan.instance,&info,NULL,&vulkan.surface);
        if (result==VK_SUCCESS) { vulkan.window_generation=generation; __atomic_store_n(&metal_presentation.dirty,0,__ATOMIC_RELEASE); }
        LOG("Vulkan Surface refreshed: generation=%u result=%d",generation,result);
    }
    pthread_mutex_unlock(&vulkan_mutex);
    if (window) ANativeWindow_release(window);
    return result;
}
static VkResult metal_swapchain_create(struct MetalLayer *layer,const char **operation) {
    VkSurfaceCapabilitiesKHR capabilities;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR query=(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats=(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceSurfaceFormatsKHR");
    *operation="vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
    VkResult result=query(vulkan.physical,vulkan.surface,&capabilities); if (result!=VK_SUCCESS) return result;
    metal_presentation.current_transform=capabilities.currentTransform;
    metal_presentation.supported_transforms=capabilities.supportedTransforms;
    metal_presentation.supported_usage=capabilities.supportedUsageFlags;
    VkSurfaceTransformFlagBitsKHR transform=capabilities.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ?
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : capabilities.currentTransform;
    if(!(capabilities.supportedTransforms&transform) || ((uint32_t)transform&~255U)) {
        *operation="supported Surface transform";return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkFormat source_format=VK_FORMAT_UNDEFINED;
    for(unsigned i=0;i<COUNT(metal_formats);++i)if(metal_formats[i].metal==layer->pixel_format) {source_format=metal_formats[i].format;break;}
    PFN_vkGetPhysicalDeviceFormatProperties format_query=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties source_properties={0};
    metal_presentation.source_format=source_format;
    if(source_format!=VK_FORMAT_UNDEFINED)format_query(vulkan.physical,source_format,&source_properties);
    *operation="vkGetPhysicalDeviceSurfaceFormatsKHR";
    uint32_t count=0; result=formats(vulkan.physical,vulkan.surface,&count,NULL);
    if(result!=VK_SUCCESS)return result;
    if(!count || count>256)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkSurfaceFormatKHR available[256]; result=formats(vulkan.physical,vulkan.surface,&count,available); if (result!=VK_SUCCESS) return result;
    VkSurfaceFormatKHR selected={VK_FORMAT_UNDEFINED,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    const VkFormat preferred[]={VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM};
    VkImageUsageFlags usage=0;
    for(unsigned method=0;method<2 && !usage;++method)for(unsigned choice=0;choice<COUNT(preferred) && !usage;++choice) {
        if(!method && (choice>=2 || source_format!=preferred[choice] || transform!=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR || !(source_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)))continue;
        for(unsigned i=0;i<count;++i)if(available[i].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (available[i].format==preferred[choice] || available[i].format==VK_FORMAT_UNDEFINED)) {
            VkFormatProperties properties;format_query(vulkan.physical,preferred[choice],&properties);
            VkImageUsageFlags required=method ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkFormatFeatureFlags feature=method ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_BLIT_DST_BIT;
            if((capabilities.supportedUsageFlags&required) && (properties.optimalTilingFeatures&feature)) {
                selected=(VkSurfaceFormatKHR){preferred[choice],available[i].colorSpace};usage=required;break;
            }
        }
    }
    *operation="supported swapchain format/usage";
    if(!usage)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkExtent2D extent=capabilities.currentExtent;
    if (extent.width==UINT32_MAX) {
        extent=(VkExtent2D){(uint32_t)graphics.width,(uint32_t)graphics.height};
        if (extent.width<capabilities.minImageExtent.width) extent.width=capabilities.minImageExtent.width;
        if (extent.width>capabilities.maxImageExtent.width) extent.width=capabilities.maxImageExtent.width;
        if (extent.height<capabilities.minImageExtent.height) extent.height=capabilities.minImageExtent.height;
        if (extent.height>capabilities.maxImageExtent.height) extent.height=capabilities.maxImageExtent.height;
    }
    uint32_t image_count=capabilities.minImageCount;
    if(image_count<UINT32_MAX)++image_count;
    if (capabilities.maxImageCount && image_count>capabilities.maxImageCount) image_count=capabilities.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha&alpha)) alpha=(VkCompositeAlphaFlagBitsKHR)(capabilities.supportedCompositeAlpha&(~capabilities.supportedCompositeAlpha+1));
     
    VkSwapchainCreateInfoKHR info={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=vulkan.surface,.minImageCount=image_count,
        .imageFormat=selected.format,.imageColorSpace=selected.colorSpace,.imageExtent=extent,.imageArrayLayers=1,.imageUsage=usage,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=transform,.compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    /* Publish only a complete acquisition/presentation set. A partial swapchain
     * used to survive a failed fence/semaphore allocation, so the next request
     * skipped creation and passed VK_NULL_HANDLE to vkResetFences. */
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkFence acquire_fence=VK_NULL_HANDLE;
    VkImage *images=NULL;
    VkSemaphore *finished=NULL;
    unsigned *presented=NULL;
    struct MetalTexture **backings=NULL;
    struct MetalPresentationRenderer renderer={0};
    unsigned created_semaphores=0;
    *operation="vkCreateSwapchainKHR";
    result=VKD(vkCreateSwapchainKHR)(vulkan.device,&info,NULL,&swapchain);
    if (result!=VK_SUCCESS) return result;
    *operation="vkGetSwapchainImagesKHR";
    /* minImageCount is a lower bound, not the number returned by the driver.
     * Enumerate the actual set before allocating per-image resources. */
    for(unsigned attempt=0;attempt<4;++attempt) {
        count=0;result=VKD(vkGetSwapchainImagesKHR)(vulkan.device,swapchain,&count,NULL);
        if(result!=VK_SUCCESS || !count) { if(result==VK_SUCCESS)result=VK_ERROR_INITIALIZATION_FAILED;goto failed; }
        free(images);images=calloc(count,sizeof(*images));
        if(!images) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;}
        result=VKD(vkGetSwapchainImagesKHR)(vulkan.device,swapchain,&count,images);
        if(result!=VK_INCOMPLETE)break;
    }
    if(result!=VK_SUCCESS || !count) {if(result==VK_SUCCESS)result=VK_ERROR_INITIALIZATION_FAILED;goto failed;}
    *operation="swapchain image storage";
    finished=calloc(count,sizeof(*finished));presented=calloc(count,sizeof(*presented));backings=calloc(count,sizeof(*backings));
    if(!finished || !presented || !backings) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;}
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    *operation="vkCreateFence acquisition";
    result=VKD(vkCreateFence)(vulkan.device,&fence,NULL,&acquire_fence); if (result!=VK_SUCCESS) { acquire_fence=VK_NULL_HANDLE; goto failed; }
    VkSemaphoreCreateInfo semaphore={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    *operation="vkCreateSemaphore presentation";
    for (;created_semaphores<count;++created_semaphores) {
        result=VKD(vkCreateSemaphore)(vulkan.device,&semaphore,NULL,&finished[created_semaphores]);
        if (result!=VK_SUCCESS) goto failed;
    }
    if(usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
        result=metal_presentation_renderer_create(&renderer,selected.format,images,count,extent,operation);
        if(result!=VK_SUCCESS)goto failed;
    }
    metal_presentation.swapchain=swapchain; metal_presentation.acquire_fence=acquire_fence;
    metal_presentation.count=count; metal_presentation.extent=extent; metal_presentation.format=selected.format;
    metal_presentation.source_format=source_format;metal_presentation.source_pixel_format=layer->pixel_format;
    metal_presentation.transform=transform;metal_presentation.renderer=renderer;
    metal_presentation.images=images;metal_presentation.finished=finished;
    metal_presentation.presented=presented;metal_presentation.backings=backings;
    LOG("Vulkan Android swapchain ready: %ux%u format=%u images=%u surfaceTransform=0x%x preTransform=0x%x supported=0x%x",extent.width,extent.height,selected.format,count,capabilities.currentTransform,transform,capabilities.supportedTransforms);
    return VK_SUCCESS;
failed:
    metal_presentation_renderer_destroy(&renderer);
    for (unsigned i=0;i<created_semaphores;++i) VKD(vkDestroySemaphore)(vulkan.device,finished[i],NULL);
    if (acquire_fence) VKD(vkDestroyFence)(vulkan.device,acquire_fence,NULL);
    VKD(vkDestroySwapchainKHR)(vulkan.device,swapchain,NULL);
    free(images);free(finished);free(presented);free(backings);
    return result;
}
static Word metal_presentation_fail(const char *operation,VkResult result) {
    int report=result!=VK_NOT_READY && result!=VK_TIMEOUT && result!=VK_ERROR_OUT_OF_DATE_KHR && result!=VK_ERROR_SURFACE_LOST_KHR &&
        (metal_presentation.last_failure!=result || !metal_presentation.last_operation || strcmp(metal_presentation.last_operation,operation));
    metal_presentation.last_failure=result;metal_presentation.last_operation=operation;
    char detail[384];
    if(report)snprintf(detail,sizeof(detail),"operation=%s result=%d swapchain_ready=%u format=%d source_format=%d supported_usage=0x%x supported_transforms=0x%x current_transform=0x%x",
        operation,result,metal_presentation.swapchain!=VK_NULL_HANDLE,metal_presentation.format,metal_presentation.source_format,
        metal_presentation.supported_usage,metal_presentation.supported_transforms,metal_presentation.current_transform);
    pthread_mutex_unlock(&metal_presentation.mutex);
    if(report)record_failure("Vulkan presentation unavailable",detail);
    return 0;
}
static Word metal_next_drawable(Word object,const char *selector) {
    if (!vulkan.available) return 0;
    struct MetalLayer *layer=metal_layer(object);
    if (!layer->device || layer->size.width<1 || layer->size.height<1 || layer->size.width>UINT32_MAX || layer->size.height>UINT32_MAX) return 0;
    pthread_mutex_lock(&metal_presentation.mutex);
    if(metal_presentation.swapchain && layer->pixel_format!=metal_presentation.source_pixel_format)__atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);
    const char *operation="refresh Android Surface";
    VkResult result=metal_surface_refresh();
    if (result==VK_SUCCESS && !metal_presentation.swapchain) result=metal_swapchain_create(layer,&operation);
    uint32_t index=0;
    uint64_t diagnostic_acquire=render_diagnostic_clock();
    if(result==VK_SUCCESS){operation="vkResetFences acquisition";result=VKD(vkResetFences)(vulkan.device,1,&metal_presentation.acquire_fence);}
    if(result==VK_SUCCESS){operation="vkAcquireNextImageKHR";result=VKD(vkAcquireNextImageKHR)(vulkan.device,metal_presentation.swapchain,1000000000ULL,VK_NULL_HANDLE,metal_presentation.acquire_fence,&index);}
    if (result==VK_SUBOPTIMAL_KHR) result=VK_SUCCESS;
    int acquired=result==VK_SUCCESS;
    /* The acquisition fence establishes ownership before recording. Reacquiring
     * the same image also guarantees its prior present semaphore was consumed. */
    if(result==VK_SUCCESS){operation="vkWaitForFences acquisition";result=VKD(vkWaitForFences)(vulkan.device,1,&metal_presentation.acquire_fence,VK_TRUE,UINT64_MAX);}
    render_diagnostic_cost(6,diagnostic_acquire);
    if (result!=VK_SUCCESS) {
        if (acquired || result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_ERROR_SURFACE_LOST_KHR) __atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);
        return metal_presentation_fail(operation,result);
    }
    if(index>=metal_presentation.count) {__atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);return metal_presentation_fail("swapchain image index",VK_ERROR_INITIALIZATION_FAILED);}
    struct MetalTexture *backing=metal_presentation.backings[index];
    if (backing && (backing->width!=(Word)layer->size.width || backing->height!=(Word)layer->size.height || backing->format.metal!=layer->pixel_format)) {
        /* Drop only the presentation cache's owner. A retained drawable/texture
         * wrapper and recorded GPU commands keep their own references. */
        metal_presentation.backings[index]=NULL;
        if(metal_presentation.renderer.sources)metal_presentation.renderer.sources[index]=VK_NULL_HANDLE;
        metal_object_release((Word)backing);backing=NULL;
    }
    if (!backing) {
        struct MetalDescriptor *description=(void *)metal_descriptor_create(MD_TEXTURE);
        metal_property_assign(description,"pixelFormat",layer->pixel_format);
        metal_property_assign(description,"width",(Word)layer->size.width); metal_property_assign(description,"height",(Word)layer->size.height);
        metal_property_assign(description,"resourceOptions",0x20); metal_property_assign(description,"usage",5);
        backing=(void *)metal_texture_new(layer->device,"Vulkan drawable backing",description); metal_object_release((Word)description);
        metal_presentation.backings[index]=backing;
    }
    if(!backing) {__atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);return metal_presentation_fail("drawable backing texture",metal_texture_creation_result);}
    struct MetalDrawable *drawable=calloc(1,sizeof(*drawable));
    if(!drawable) {__atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);return metal_presentation_fail("drawable allocation",VK_ERROR_OUT_OF_HOST_MEMORY);}
    *drawable=(struct MetalDrawable){.isa=external_class("_OBJC_CLASS_$_I2AMetalDrawable"),.texture=(void *)metal_object_retain((Word)backing),.index=index,.identifier=++metal_presentation.next_identifier};
    metal_lifetime_init((Word)drawable,__builtin_offsetof(struct MetalDrawable,lifetime),metal_drawable_destroy);
    ++metal_presentation.outstanding;
    metal_presentation.last_failure=VK_SUCCESS;metal_presentation.last_operation=NULL;
    pthread_mutex_unlock(&metal_presentation.mutex); return metal_object_autorelease((Word)drawable);
}
static Word metal_drawable_texture(struct MetalDrawable *drawable) { return (Word)drawable->texture; }
static Word metal_drawable_id(struct MetalDrawable *drawable) { return drawable->identifier; }
static void metal_command_present(struct MetalCommand *command,const char *selector,struct MetalDrawable *drawable) {
    if (!drawable || drawable->isa!=external_class("_OBJC_CLASS_$_I2AMetalDrawable"))
        stop("Invalid drawable or presentation command state",selector);
    pthread_mutex_lock(&command->mutex);
    int invalid=command->drawable || metal_command_status(&command->object)>=2 ||
        __atomic_exchange_n(&drawable->scheduled,1,__ATOMIC_ACQ_REL);
    if (!invalid) command->drawable=(void *)metal_object_retain((Word)drawable);
    pthread_mutex_unlock(&command->mutex);
    if (invalid) stop("Invalid drawable or presentation command state",selector);
}
static void metal_command_record_presentation(struct MetalCommand *command) {
    struct MetalDrawable *drawable=command->drawable;
    if (!drawable) return;
    /* presentDrawable schedules presentation. Record its final copy after all
     * guest encoders have ended, so later commands still contribute pixels. */
    struct MetalTexture *texture=drawable->texture;
    struct MetalCommandTexture *use=metal_command_texture(command,texture);
    use->layout_used=1;
    int rendered=metal_presentation.renderer.pipeline!=VK_NULL_HANDLE;
    pthread_mutex_lock(&command->pool_owner->mutex);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=metal_image_access(use->layout),
        .dstAccessMask=rendered ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=use->layout,
        .newLayout=rendered ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=texture->image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VKD(vkCmdPipelineBarrier)(command->command,metal_image_stages(use->layout),rendered ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    barrier.srcAccessMask=0;barrier.dstAccessMask=rendered ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;barrier.image=metal_presentation.images[drawable->index];
    barrier.oldLayout=__atomic_load_n(&metal_presentation.presented[drawable->index],__ATOMIC_ACQUIRE) ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout=rendered ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VKD(vkCmdPipelineBarrier)(command->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,rendered ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(rendered)metal_presentation_renderer_record(&metal_presentation.renderer,command,texture,drawable->index,metal_presentation.extent,metal_presentation.format,metal_presentation.transform);
    else if (texture->format.format==metal_presentation.format && texture->width==metal_presentation.extent.width &&
        texture->height==metal_presentation.extent.height && metal_presentation.transform==VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .extent={metal_presentation.extent.width,metal_presentation.extent.height,1}};
        VKD(vkCmdCopyImage)(command->command,texture->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    }
    else {
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)texture->width,(int32_t)texture->height,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)metal_presentation.extent.width,(int32_t)metal_presentation.extent.height,1}}};
        VKD(vkCmdBlitImage)(command->command,texture->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    }
    if (!rendered) {
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VKD(vkCmdPipelineBarrier)(command->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    }
    pthread_mutex_unlock(&command->pool_owner->mutex);
    use->layout=rendered ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ++command->recorded_operations;
}
static VkSemaphore metal_drawable_signal(struct MetalCommand *command) { return command->drawable ? metal_presentation.finished[command->drawable->index] : VK_NULL_HANDLE; }
static VkResult metal_drawable_present(struct MetalCommand *command) {
    struct MetalDrawable *drawable=command->drawable; if (!drawable) return VK_SUCCESS;
    VkSemaphore signal=metal_drawable_signal(command);
    VkPresentInfoKHR info={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&signal,
        .swapchainCount=1,.pSwapchains=&metal_presentation.swapchain,.pImageIndices=&drawable->index};
    uint64_t diagnostic_present=render_diagnostic_clock();
    VkResult result=VKD(vkQueuePresentKHR)(vulkan.queue,&info);
     
    if (result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_ERROR_SURFACE_LOST_KHR) __atomic_store_n(&metal_presentation.dirty,1,__ATOMIC_RELEASE);
    render_diagnostic_cost(5,diagnostic_present);
    if (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR) metal_diagnostic_present();
    drawable->result=result;
    drawable->presented=1; __atomic_store_n(&metal_presentation.presented[drawable->index],1,__ATOMIC_RELEASE);
#if I2A_DEVELOPMENT
    static unsigned logged;
    if (__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<16) LOG("Vulkan drawable queued for presentation: id=%lu result=%d",drawable->identifier,result);
    static uint64_t interval_start; static unsigned frames;
    struct timespec time; clock_gettime(CLOCK_MONOTONIC,&time);
    uint64_t now=(uint64_t)time.tv_sec*1000000000ULL+(uint64_t)time.tv_nsec;
    if (!interval_start) interval_start=now;
    ++frames;
    if (now-interval_start>=2000000000ULL) {
        LOG("Vulkan presentation rate: %.2f fps (%u frames in %.3fs)",frames*1e9/(double)(now-interval_start),frames,(now-interval_start)*1e-9);
        frames=0; interval_start=now;
    }
#endif
    return result;
}
static void metal_first_frame_deliver(void *unused) {
    JNIEnv *env=attached_environment("Guest application first frame"); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"firstFrameRendered","()V");
    (*env)->CallVoidMethod(env,activity,method); (*env)->DeleteLocalRef(env,cls);
    LOG("Guest application first frame reached its Vulkan fence and presentation queue");
}
static void metal_first_frame_complete(struct MetalCommand *command) {
    static unsigned delivered;
    if (command->drawable && command->drawable->presented && (command->drawable->result==VK_SUCCESS || command->drawable->result==VK_SUBOPTIMAL_KHR) &&
        !__atomic_exchange_n(&delivered,1,__ATOMIC_ACQ_REL)) schedule_main_function(metal_first_frame_deliver,NULL);
}
static void metal_drawable_complete(struct MetalCommand *command) {
    if (!command->drawable) return;
    struct MetalDrawable *drawable=command->drawable; command->drawable=NULL;
    metal_drawable_retire(drawable);
    metal_object_release((Word)drawable);
}
static Method metal_present_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_CAMetalLayer") && !strcmp(selector,"nextDrawable")) return (Method)metal_next_drawable;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalCommandBuffer") && !strcmp(selector,"presentDrawable:")) return (Method)metal_command_present;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDrawable")) {
        if (!strcmp(selector,"texture")) return (Method)metal_drawable_texture;
        if (!strcmp(selector,"drawableID")) return (Method)metal_drawable_id;
    }
    return NULL;
}
#undef VKD
