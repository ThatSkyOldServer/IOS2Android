 
#define VKD(name) ({ static PFN_##name cached; PFN_##name fn=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); if (!fn) { fn=(PFN_##name)vulkan.get_device_proc(vulkan.device,#name); __atomic_store_n(&cached,fn,__ATOMIC_RELEASE); } fn; })
struct MetalFormat { unsigned metal; VkFormat format; unsigned bytes,width,height; VkImageAspectFlags aspects; };
static const struct MetalFormat metal_formats[]={
    {1,VK_FORMAT_R8_UNORM,1,1,1},
    {10,VK_FORMAT_R8_UNORM,1,1,1},{12,VK_FORMAT_R8_SNORM,1,1,1},{13,VK_FORMAT_R8_UINT,1,1,1},{14,VK_FORMAT_R8_SINT,1,1,1},
    {20,VK_FORMAT_R16_UNORM,2,1,1},{22,VK_FORMAT_R16_SNORM,2,1,1},{23,VK_FORMAT_R16_UINT,2,1,1},{24,VK_FORMAT_R16_SINT,2,1,1},{25,VK_FORMAT_R16_SFLOAT,2,1,1},
    {30,VK_FORMAT_R8G8_UNORM,2,1,1},{32,VK_FORMAT_R8G8_SNORM,2,1,1},{33,VK_FORMAT_R8G8_UINT,2,1,1},{34,VK_FORMAT_R8G8_SINT,2,1,1},
    {53,VK_FORMAT_R32_UINT,4,1,1},{54,VK_FORMAT_R32_SINT,4,1,1},{55,VK_FORMAT_R32_SFLOAT,4,1,1},
    {60,VK_FORMAT_R16G16_UNORM,4,1,1},{62,VK_FORMAT_R16G16_SNORM,4,1,1},{63,VK_FORMAT_R16G16_UINT,4,1,1},{64,VK_FORMAT_R16G16_SINT,4,1,1},{65,VK_FORMAT_R16G16_SFLOAT,4,1,1},
    {70,VK_FORMAT_R8G8B8A8_UNORM,4,1,1},{71,VK_FORMAT_R8G8B8A8_SRGB,4,1,1},{72,VK_FORMAT_R8G8B8A8_SNORM,4,1,1},{73,VK_FORMAT_R8G8B8A8_UINT,4,1,1},{74,VK_FORMAT_R8G8B8A8_SINT,4,1,1},
    {80,VK_FORMAT_B8G8R8A8_UNORM,4,1,1},{81,VK_FORMAT_B8G8R8A8_SRGB,4,1,1},
    {90,VK_FORMAT_A2B10G10R10_UNORM_PACK32,4,1,1},{91,VK_FORMAT_A2B10G10R10_UINT_PACK32,4,1,1},
    {92,VK_FORMAT_B10G11R11_UFLOAT_PACK32,4,1,1},{93,VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,4,1,1},{94,VK_FORMAT_A2R10G10B10_UNORM_PACK32,4,1,1},
    {103,VK_FORMAT_R32G32_UINT,8,1,1},{104,VK_FORMAT_R32G32_SINT,8,1,1},{105,VK_FORMAT_R32G32_SFLOAT,8,1,1},
    {110,VK_FORMAT_R16G16B16A16_UNORM,8,1,1},{112,VK_FORMAT_R16G16B16A16_SNORM,8,1,1},{113,VK_FORMAT_R16G16B16A16_UINT,8,1,1},{114,VK_FORMAT_R16G16B16A16_SINT,8,1,1},{115,VK_FORMAT_R16G16B16A16_SFLOAT,8,1,1},
    {123,VK_FORMAT_R32G32B32A32_UINT,16,1,1},{124,VK_FORMAT_R32G32B32A32_SINT,16,1,1},{125,VK_FORMAT_R32G32B32A32_SFLOAT,16,1,1},
    {130,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,8,4,4},{131,VK_FORMAT_BC1_RGBA_SRGB_BLOCK,8,4,4},
    {132,VK_FORMAT_BC2_UNORM_BLOCK,16,4,4},{133,VK_FORMAT_BC2_SRGB_BLOCK,16,4,4},{134,VK_FORMAT_BC3_UNORM_BLOCK,16,4,4},{135,VK_FORMAT_BC3_SRGB_BLOCK,16,4,4},
    {140,VK_FORMAT_BC4_UNORM_BLOCK,8,4,4},{141,VK_FORMAT_BC4_SNORM_BLOCK,8,4,4},{142,VK_FORMAT_BC5_UNORM_BLOCK,16,4,4},{143,VK_FORMAT_BC5_SNORM_BLOCK,16,4,4},
    {150,VK_FORMAT_BC6H_UFLOAT_BLOCK,16,4,4},{151,VK_FORMAT_BC6H_SFLOAT_BLOCK,16,4,4},{152,VK_FORMAT_BC7_UNORM_BLOCK,16,4,4},{153,VK_FORMAT_BC7_SRGB_BLOCK,16,4,4},
    {160,VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG,8,8,4},{161,VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG,8,8,4},
    {162,VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG,8,4,4},{163,VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG,8,4,4},
    {164,VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG,8,8,4},{165,VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG,8,8,4},
    {166,VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG,8,4,4},{167,VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG,8,4,4},
    {170,VK_FORMAT_EAC_R11_UNORM_BLOCK,8,4,4},{172,VK_FORMAT_EAC_R11_SNORM_BLOCK,8,4,4},{174,VK_FORMAT_EAC_R11G11_UNORM_BLOCK,16,4,4},{176,VK_FORMAT_EAC_R11G11_SNORM_BLOCK,16,4,4},
    {178,VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,16,4,4},{179,VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,16,4,4},
    {180,VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,8,4,4},{181,VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,8,4,4},{182,VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,8,4,4},{183,VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,8,4,4},
#define ASTC_PAIR(m,w,h) {m,VK_FORMAT_ASTC_##w##x##h##_SRGB_BLOCK,16,w,h},{m+18,VK_FORMAT_ASTC_##w##x##h##_UNORM_BLOCK,16,w,h}
    ASTC_PAIR(186,4,4),ASTC_PAIR(187,5,4),ASTC_PAIR(188,5,5),ASTC_PAIR(189,6,5),ASTC_PAIR(190,6,6),
    ASTC_PAIR(192,8,5),ASTC_PAIR(193,8,6),ASTC_PAIR(194,8,8),ASTC_PAIR(195,10,5),ASTC_PAIR(196,10,6),
    ASTC_PAIR(197,10,8),ASTC_PAIR(198,10,10),ASTC_PAIR(199,12,10),ASTC_PAIR(200,12,12),
#undef ASTC_PAIR
    {250,VK_FORMAT_D16_UNORM,2,1,1,VK_IMAGE_ASPECT_DEPTH_BIT},
    {252,VK_FORMAT_D32_SFLOAT,4,1,1,VK_IMAGE_ASPECT_DEPTH_BIT},
    {253,VK_FORMAT_S8_UINT,1,1,1,VK_IMAGE_ASPECT_STENCIL_BIT},
    {255,VK_FORMAT_D24_UNORM_S8_UINT,4,1,1,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT},
    {260,VK_FORMAT_D32_SFLOAT_S8_UINT,8,1,1,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT}
    , {552,VK_FORMAT_R16G16B16A16_SFLOAT,8,1,1},{553,VK_FORMAT_R16G16B16A16_SFLOAT,8,1,1}
    , {554,VK_FORMAT_R16G16B16A16_SFLOAT,8,1,1},{555,VK_FORMAT_R16G16B16A16_SFLOAT,8,1,1}
};
static VkFormat metal_depth_stencil_backing(VkFormat requested) {
    if (requested!=VK_FORMAT_D32_SFLOAT_S8_UINT) return requested;
    static VkFormat cached;
    VkFormat selected=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if (selected) return selected;
    PFN_vkGetPhysicalDeviceFormatProperties query=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties supported;
    query(vulkan.physical,requested,&supported);
    selected=requested;
    if (!(supported.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        query(vulkan.physical,VK_FORMAT_D24_UNORM_S8_UINT,&supported);
        VkFormatFeatureFlags required=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        /* D24S8 reduces GPU depth precision; preserve the Metal CPU layout. */
        if ((supported.optimalTilingFeatures&required)==required) selected=VK_FORMAT_D24_UNORM_S8_UINT;
    }
    __atomic_store_n(&cached,selected,__ATOMIC_RELEASE);
    return selected;
}
struct MetalTexture {
    struct MetalResource object;
    pthread_mutex_t mutex;
    struct MetalFormat format;
    Word type,width,height,depth,mips,samples,array_length,usage,layers;
    VkImage image;
    VkImageView view,sampled_view,storage_view;
    VkDeviceMemory memory;
    VkDeviceSize allocation;
    VkImageLayout layout;
    VkPipelineStageFlags read_stages;
    VkImageUsageFlags native_usage;
    unsigned decode_pvrtc,sampled_identity;
    struct MetalLifetime lifetime;
};
struct MetalRegion { size_t x,y,z,width,height,depth; };
static void metal_render_cache_invalidate(struct MetalTexture *);
static void metal_texture_dispose(struct MetalTexture *texture) {
    if (texture->allocation) metal_memory_sub(1,texture->allocation);
    if (texture->image) metal_render_cache_invalidate(texture);
    if (texture->sampled_view && texture->sampled_view!=texture->view) VKD(vkDestroyImageView)(vulkan.device,texture->sampled_view,NULL);
    if (texture->storage_view) VKD(vkDestroyImageView)(vulkan.device,texture->storage_view,NULL);
    if (texture->view) VKD(vkDestroyImageView)(vulkan.device,texture->view,NULL);
    if (texture->image) VKD(vkDestroyImage)(vulkan.device,texture->image,NULL);
    if (texture->memory) VKD(vkFreeMemory)(vulkan.device,texture->memory,NULL);
    pthread_mutex_destroy(&texture->mutex); free(texture);
}
static void metal_texture_destroy(Word value) { metal_texture_dispose((void *)value); }
static _Thread_local VkResult metal_texture_creation_result;
static _Thread_local size_t metal_texture_creation_reclaimed;
static Word metal_texture_new(Word device,const char *selector,struct MetalDescriptor *descriptor) {
    metal_texture_creation_result=VK_ERROR_INITIALIZATION_FAILED;
    metal_texture_creation_reclaimed=0;
    if (!descriptor || descriptor->kind!=MD_TEXTURE) return 0;
    if (!vulkan.available) stop("Texture backend requires Vulkan",selector);
    Word pixel=metal_property(descriptor,"pixelFormat");
    const struct MetalFormat *format=NULL;
    for (unsigned i=0;i<COUNT(metal_formats);++i) if (metal_formats[i].metal==pixel) { format=&metal_formats[i]; break; }
    if (!format) { char detail[80]; snprintf(detail,sizeof(detail),"Metal pixelFormat=%lu",pixel); stop("Unimplemented Vulkan texture format",detail); }
    struct MetalFormat fallback;
    int pvrtc=pixel>=160 && pixel<=167;
    if (pvrtc && !vulkan.pvrtc) {
        fallback=(struct MetalFormat){(unsigned)pixel,(pixel&1) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,4,1,1,VK_IMAGE_ASPECT_COLOR_BIT};
        format=&fallback;
    }
    if (format->format==VK_FORMAT_D32_SFLOAT_S8_UINT) {
        fallback=*format; fallback.format=metal_depth_stencil_backing(format->format);
        format=&fallback;
    }
    struct MetalTexture *texture=calloc(1,sizeof(*texture));if(!texture){metal_texture_creation_result=VK_ERROR_OUT_OF_HOST_MEMORY;return 0;}
    texture->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalTexture"),.device=device,
        .label=metal_property(descriptor,"label"),.options=metal_property(descriptor,"resourceOptions")};
    pthread_mutex_init(&texture->mutex,NULL); texture->format=*format;
    texture->decode_pvrtc=pvrtc && !vulkan.pvrtc ? ((pixel&2) ? 4 : 2) : 0;
    if (!texture->format.aspects) texture->format.aspects=VK_IMAGE_ASPECT_COLOR_BIT;
    texture->type=metal_property(descriptor,"textureType"); texture->width=metal_property(descriptor,"width");
    texture->height=metal_property(descriptor,"height"); texture->depth=metal_property(descriptor,"depth");
    texture->mips=metal_property(descriptor,"mipmapLevelCount"); texture->samples=metal_property(descriptor,"sampleCount");
    texture->array_length=metal_property(descriptor,"arrayLength"); texture->usage=metal_property(descriptor,"usage");
    texture->layers=texture->array_length;
    VkImageType type=VK_IMAGE_TYPE_2D; VkImageViewType view=VK_IMAGE_VIEW_TYPE_2D; VkImageCreateFlags flags=0;
    switch (texture->type) {
    case 0: type=VK_IMAGE_TYPE_1D; view=VK_IMAGE_VIEW_TYPE_1D; break;
    case 1: type=VK_IMAGE_TYPE_1D; view=VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case 2: case 4: break;
    case 3: case 8: view=VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case 5: view=VK_IMAGE_VIEW_TYPE_CUBE; flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; texture->layers*=6; break;
    case 6: view=VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; texture->layers*=6; break;
    case 7: type=VK_IMAGE_TYPE_3D; view=VK_IMAGE_VIEW_TYPE_3D; break;
    default: metal_texture_dispose(texture); stop("Unimplemented Metal texture type",selector);
    }
    Word storage=(texture->object.options>>4)&15;
    Word maximum=texture->width>texture->height ? texture->width : texture->height;
    if (texture->depth>maximum) maximum=texture->depth;
    unsigned max_mips=0;
    for (Word extent=maximum;extent;extent>>=1) ++max_mips;
    int invalid=texture->mips>max_mips ||
        ((texture->type==5 || texture->type==6) && texture->array_length>UINT32_MAX/6) || !texture->width || !texture->height || !texture->depth || !texture->mips || !texture->array_length ||
        texture->width>UINT32_MAX || texture->height>UINT32_MAX || texture->depth>UINT32_MAX || texture->layers>UINT32_MAX ||
        !texture->samples || texture->samples>64 || (texture->samples&(texture->samples-1)) ||
        (type==VK_IMAGE_TYPE_1D && (texture->height!=1 || texture->depth!=1)) ||
        (type==VK_IMAGE_TYPE_2D && texture->depth!=1) || (type==VK_IMAGE_TYPE_3D && texture->array_length!=1) ||
        ((texture->type==0 || texture->type==2 || texture->type==4 || texture->type==5) && texture->array_length!=1) ||
        ((texture->type==5 || texture->type==6) && texture->width!=texture->height) ||
        (texture->samples>1 && texture->type!=4 && texture->type!=8) || (texture->samples>1 && texture->mips!=1) ||
        (texture->type==6 && !vulkan.features.imageCubeArray) || storage>3 || (texture->usage&~0x17UL);
    if(invalid) {
        char detail[256];snprintf(detail,sizeof(detail),"Metal=%lu type=%lu extent=%lux%lux%lu mips=%lu layers=%lu samples=%lu options=0x%lx usage=0x%lx",
            pixel,texture->type,texture->width,texture->height,texture->depth,texture->mips,texture->layers,texture->samples,texture->object.options,texture->usage);
        metal_texture_creation_result=VK_ERROR_FORMAT_NOT_SUPPORTED;record_failure("Unsupported Metal texture description",detail);metal_texture_dispose(texture);return 0;
    }
    if (pvrtc && (texture->depth!=1 || texture->samples!=1 || (texture->width&(texture->width-1)) || (texture->height&(texture->height-1)) || (texture->usage&6))) {
        metal_texture_dispose(texture); stop("PVRTC requires power-of-two sampled 2D images",selector);
    }
    VkFormatProperties properties;
    PFN_vkGetPhysicalDeviceFormatProperties get_properties=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
    get_properties(vulkan.physical,format->format,&properties);
    VkFormatFeatureFlags features=properties.optimalTilingFeatures;
    VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((texture->usage&1) || (!texture->usage && (features&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))) usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((texture->usage&2) || (!texture->usage && texture->samples==1 && (features&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))) usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if ((texture->usage&4) || (!texture->usage && (features&(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))))
        usage|=texture->format.aspects==VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (texture->usage&16) flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageFormatProperties limits;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_limits=(PFN_vkGetPhysicalDeviceImageFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceImageFormatProperties");
    VkResult result=get_limits(vulkan.physical,format->format,type,VK_IMAGE_TILING_OPTIMAL,usage,flags,&limits);
    if (result!=VK_SUCCESS || texture->width>limits.maxExtent.width || texture->height>limits.maxExtent.height || texture->depth>limits.maxExtent.depth || texture->mips>limits.maxMipLevels || texture->layers>limits.maxArrayLayers || !(limits.sampleCounts&texture->samples)) {
        metal_texture_creation_result=result==VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result;
        char detail[256];snprintf(detail,sizeof(detail),"Metal=%lu VkFormat=%d result=%d extent=%lux%lux%lu mips=%lu layers=%lu samples=%lu usage=0x%x",
            pixel,format->format,result,texture->width,texture->height,texture->depth,texture->mips,texture->layers,texture->samples,usage);
        record_failure("Vulkan texture format/extent unavailable",detail);
        LOG("Vulkan format/extent unavailable: Metal=%lu VkFormat=%d result=%d",pixel,format->format,result); metal_texture_dispose(texture); return 0;
    }
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=flags,.imageType=type,.format=format->format,
        .extent={(uint32_t)texture->width,(uint32_t)texture->height,(uint32_t)texture->depth},.mipLevels=(uint32_t)texture->mips,
        .arrayLayers=(uint32_t)texture->layers,.samples=(VkSampleCountFlagBits)texture->samples,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    const char *operation="vkCreateImage";
    VkMemoryRequirements requirements={0};int memory_type=-1;VkResult recovery=VK_SUCCESS;
    result=VKD(vkCreateImage)(vulkan.device,&image_info,NULL,&texture->image);
    if (result==VK_SUCCESS) {
        VKD(vkGetImageMemoryRequirements)(vulkan.device,texture->image,&requirements);
        memory_type=metal_memory_type(requirements.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(memory_type<0){operation="texture memory type";result=VK_ERROR_FEATURE_NOT_PRESENT;}
        else {
            VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=(uint32_t)memory_type};
            operation="vkAllocateMemory texture";result=VKD(vkAllocateMemory)(vulkan.device,&allocation,NULL,&texture->memory);
            if(result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                metal_texture_creation_reclaimed=metal_memory_reclaim(&recovery);
                if(metal_texture_creation_reclaimed && recovery==VK_SUCCESS)result=VKD(vkAllocateMemory)(vulkan.device,&allocation,NULL,&texture->memory);
            }
            if(result==VK_SUCCESS) {
                texture->allocation=requirements.size;
                metal_memory_add(1,requirements.size);
                operation="vkBindImageMemory";result=VKD(vkBindImageMemory)(vulkan.device,texture->image,texture->memory,0);
            }
        }
    }
    if (result==VK_SUCCESS) {
        VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=texture->image,.viewType=view,.format=format->format,
            .subresourceRange={texture->format.aspects,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
        if (pixel==1) view_info.components=(VkComponentMapping){VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
        if (pvrtc && pixel<164) view_info.components.a=VK_COMPONENT_SWIZZLE_ONE;
        texture->sampled_identity=view_info.components.r==VK_COMPONENT_SWIZZLE_IDENTITY && view_info.components.g==VK_COMPONENT_SWIZZLE_IDENTITY &&
            view_info.components.b==VK_COMPONENT_SWIZZLE_IDENTITY && view_info.components.a==VK_COMPONENT_SWIZZLE_IDENTITY;
        operation="vkCreateImageView texture";result=VKD(vkCreateImageView)(vulkan.device,&view_info,NULL,&texture->view);
        texture->sampled_view=texture->view;
        if (result==VK_SUCCESS && texture->format.aspects==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)) {
            view_info.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            operation="vkCreateImageView depth sample";result=VKD(vkCreateImageView)(vulkan.device,&view_info,NULL,&texture->sampled_view);
        }
        if (result==VK_SUCCESS && (usage&VK_IMAGE_USAGE_STORAGE_BIT)) {
            view_info.subresourceRange.levelCount=1;
            view_info.components=(VkComponentMapping){0};
            operation="vkCreateImageView storage";result=VKD(vkCreateImageView)(vulkan.device,&view_info,NULL,&texture->storage_view);
        }
    }
    if(result!=VK_SUCCESS) {
        metal_texture_creation_result=result;
        uint32_t heap=memory_type<0 ? UINT32_MAX : vulkan.memory.memoryTypes[memory_type].heapIndex;
        char detail[512];snprintf(detail,sizeof(detail),"operation=%s result=%d Metal=%lu VkFormat=%d extent=%lux%lux%lu mips=%lu layers=%lu usage=0x%x required_bytes=%llu memory_type=%d heap=%u heap_bytes=%llu max_allocations=%u decoded_pvrtc=%u released_idle_bytes=%zu recovery_result=%d",
            operation,result,pixel,format->format,texture->width,texture->height,texture->depth,texture->mips,texture->layers,usage,
            (unsigned long long)requirements.size,memory_type,heap,heap==UINT32_MAX ? 0ULL : (unsigned long long)vulkan.memory.memoryHeaps[heap].size,
            vulkan.properties.limits.maxMemoryAllocationCount,texture->decode_pvrtc,metal_texture_creation_reclaimed,recovery);
        record_failure("Vulkan texture allocation failed",detail);metal_texture_dispose(texture);return 0;
    }
    texture->native_usage=usage;
    metal_lifetime_init((Word)texture,__builtin_offsetof(struct MetalTexture,lifetime),metal_texture_destroy);
    LOG_LIMITED(32,"Vulkan texture created: Metal=%lu %lux%lux%lu mips=%lu layers=%lu",pixel,texture->width,texture->height,texture->depth,texture->mips,texture->layers);
    if (texture->decode_pvrtc) LOG("PVRTC%u upload decoder selected: %lux%lu, Vulkan RGBA8%s backing",texture->decode_pvrtc,texture->width,texture->height,(pixel&1) ? " sRGB" : "");
    metal_texture_creation_result=VK_SUCCESS;
    return (Word)texture;
}
struct MetalTextureUpload { struct MetalTexture *texture; struct MetalBuffer *staging; VkBufferImageCopy region; VkImageLayout final_layout; };
static void metal_record_texture_upload(VkCommandBuffer command,void *context) {
    struct MetalTextureUpload *upload=context; struct MetalTexture *texture=upload->texture;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=texture->layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=texture->image,
        .subresourceRange={texture->format.aspects,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
    VkPipelineStageFlags source=texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VKD(vkCmdPipelineBarrier)(command,source,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VKD(vkCmdCopyBufferToImage)(command,upload->staging->buffer,texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload->region);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=upload->final_layout;
    VKD(vkCmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
}
static void metal_texture_replace(struct MetalTexture *texture,const char *selector,struct MetalRegion region,Word mip,Word slice,const void *bytes,Word row_stride,Word image_stride) {
    if (mip>=texture->mips || slice>=texture->layers || texture->samples!=1 || !bytes || !region.width || !region.height || !region.depth)
        stop("Invalid Metal texture update",selector);
    size_t width=texture->width>>mip,height=texture->height>>mip,depth=texture->depth>>mip;
    if (!width) width=1; if (!height) height=1; if (!depth) depth=1;
    if (region.x>width || region.width>width-region.x || region.y>height || region.height>height-region.y || region.z>depth || region.depth>depth-region.z)
        stop("Metal texture region out of bounds",selector);
    struct MetalFormat format=texture->format;
    int pvrtc=format.metal>=160 && format.metal<=167;
    size_t pvrtc_size=0;
    if (pvrtc) {
        unsigned bits=(format.metal&2) ? 4 : 2;
        size_t stored_width=width<(bits==2 ? 16U : 8U) ? (bits==2 ? 16U : 8U) : width,stored_height=height<8 ? 8 : height;
        if (region.x || region.y || region.z || region.width!=width || region.height!=height || region.depth!=1 ||
            stored_width>SIZE_MAX/stored_height || stored_width*stored_height>SIZE_MAX/bits) stop("PVRTC update must cover an entire mip",selector);
        pvrtc_size=stored_width*stored_height*bits/8;
        if ((row_stride && row_stride!=stored_width*bits/8) || (image_stride && image_stride<pvrtc_size)) stop("Invalid PVRTC upload pitch",selector);
    }
    if (format.aspects==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)) stop("Packed depth/stencil upload requires aspect conversion",selector);
    if (region.x%format.width || region.y%format.height ||
        (region.width%format.width && region.x+region.width!=width) || (region.height%format.height && region.y+region.height!=height))
        stop("Misaligned compressed texture region",selector);
    size_t columns=(region.width+format.width-1)/format.width,rows=(region.height+format.height-1)/format.height;
    if (columns>SIZE_MAX/format.bytes) stop("Texture upload too large",selector);
    size_t packed_row=columns*format.bytes;
    if (rows>SIZE_MAX/packed_row || region.depth>SIZE_MAX/(packed_row*rows)) stop("Texture upload too large",selector);
    size_t packed_image=packed_row*rows,total=packed_image*region.depth;
    if (pvrtc && !texture->decode_pvrtc) { rows=1; packed_row=packed_image=total=pvrtc_size; row_stride=0; image_stride=0; }
    if (texture->decode_pvrtc) { row_stride=packed_row; image_stride=packed_image; }
    if (!row_stride) row_stride=packed_row;
    if (row_stride<packed_row || rows>SIZE_MAX/row_stride) stop("Invalid texture row pitch",selector);
    if (!image_stride) image_stride=row_stride*rows;
    if (image_stride<(rows-1)*row_stride+packed_row || region.depth>SIZE_MAX/image_stride) stop("Invalid texture image pitch",selector);
    uint64_t diagnostic_prepare=render_diagnostic_clock();
    struct MetalStaging *slot=metal_staging_acquire(total);
    if (!slot) stop("Vulkan texture staging allocation failed",selector);
    struct MetalBuffer *staging=&slot->buffer;
    VkResult result=VK_SUCCESS;
    if (texture->decode_pvrtc) {
        extern int i2a_decode_pvrtc(const void *,size_t,uint32_t,uint32_t,uint32_t,int,void *);
        if (!i2a_decode_pvrtc(bytes,pvrtc_size,texture->decode_pvrtc,(uint32_t)width,(uint32_t)height,format.metal<164,staging->mapped)) {
            metal_staging_release(slot); stop("PVRTC decode failed",selector);
        }
        LOG_LIMITED(24,"PVRTC decoded for GPU upload: %zu -> %zu bytes mip=%lu slice=%lu",pvrtc_size,total,mip,slice);
    } else for (size_t z=0;z<region.depth;++z) for (size_t y=0;y<rows;++y)
        memcpy((char *)staging->mapped+z*packed_image+y*packed_row,(const char *)bytes+z*image_stride+y*row_stride,packed_row);
    render_diagnostic_cost(9,diagnostic_prepare);
    struct MetalTextureUpload upload={.texture=texture,.staging=staging,
        .region={.imageSubresource={format.aspects,(uint32_t)mip,(uint32_t)slice,1},
            .imageOffset={(int32_t)region.x,(int32_t)region.y,(int32_t)region.z},.imageExtent={(uint32_t)region.width,(uint32_t)region.height,(uint32_t)region.depth}},
        .final_layout=(texture->native_usage&VK_IMAGE_USAGE_SAMPLED_BIT) ?
            (format.aspects==VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) : VK_IMAGE_LAYOUT_GENERAL};
    pthread_mutex_lock(&texture->mutex);
    result=metal_upload_record(metal_record_texture_upload,&upload,slot,(Word)texture);
    if (result==VK_SUCCESS) {
        texture->layout=upload.final_layout;
        texture->read_stages=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    pthread_mutex_unlock(&texture->mutex);
    if (result!=VK_SUCCESS) stop("Vulkan texture upload failed",selector);
    LOG_LIMITED(24,"Vulkan texture upload recorded: %zu bytes mip=%lu slice=%lu",total,mip,slice);
}
static void metal_texture_replace_2d(struct MetalTexture *texture,const char *selector,struct MetalRegion region,Word mip,const void *bytes,Word stride) {
    metal_texture_replace(texture,selector,region,mip,0,bytes,stride,0);
}
/* Synchronous Metal getBytes, only when the guest asks for CPU pixels. One
 * lazily-created transfer pool/fence and a bounded reusable read buffer; no
 * frame hook, queue-idle wait, staging-upload changes or extra worker. */
static struct {
    pthread_mutex_t mutex;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    struct MetalBuffer buffer;
} metal_texture_reader={.mutex=PTHREAD_MUTEX_INITIALIZER};
static VkResult metal_texture_reader_prepare(size_t length) {
    VkResult result=VK_SUCCESS;
    if (!metal_texture_reader.pool) {
        VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=vulkan.queue_family};
        result=VKD(vkCreateCommandPool)(vulkan.device,&pool,NULL,&metal_texture_reader.pool);
        VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=metal_texture_reader.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        if (result==VK_SUCCESS) result=VKD(vkAllocateCommandBuffers)(vulkan.device,&allocate,&metal_texture_reader.command);
        VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (result==VK_SUCCESS) result=VKD(vkCreateFence)(vulkan.device,&fence,NULL,&metal_texture_reader.fence);
        if (result!=VK_SUCCESS) {
            if (metal_texture_reader.pool) VKD(vkDestroyCommandPool)(vulkan.device,metal_texture_reader.pool,NULL);
            metal_texture_reader.pool=VK_NULL_HANDLE; metal_texture_reader.command=VK_NULL_HANDLE;
            return result;
        }
    }
    if (metal_texture_reader.buffer.length<length) {
        metal_buffer_dispose(&metal_texture_reader.buffer); metal_texture_reader.buffer.length=0;
        size_t capacity=256;
        while (capacity<length && capacity<=SIZE_MAX/2) capacity*=2;
        if (capacity<length) return VK_ERROR_OUT_OF_HOST_MEMORY;
        result=metal_buffer_allocate(&metal_texture_reader.buffer,capacity,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (result!=VK_SUCCESS) return result;
    }
    result=VKD(vkResetCommandPool)(vulkan.device,metal_texture_reader.pool,0);
    if (result==VK_SUCCESS) result=VKD(vkResetFences)(vulkan.device,1,&metal_texture_reader.fence);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (result==VK_SUCCESS) result=VKD(vkBeginCommandBuffer)(metal_texture_reader.command,&begin);
    return result;
}
static void metal_texture_get_bytes(struct MetalTexture *texture,const char *selector,void *bytes,Word row_stride,Word image_stride,
        struct MetalRegion region,Word mip,Word slice) {
    if (!region.width || !region.height || !region.depth) return;
    if (!bytes || mip>=texture->mips || slice>=texture->layers || texture->samples!=1)
        stop("Invalid Metal texture read",selector);
    size_t width=texture->width>>mip,height=texture->height>>mip,depth=texture->depth>>mip;
    if (!width) width=1; if (!height) height=1; if (!depth) depth=1;
    if (region.x>width || region.width>width-region.x || region.y>height || region.height>height-region.y || region.z>depth || region.depth>depth-region.z)
        stop("Metal texture read region out of bounds",selector);
    struct MetalFormat format=texture->format;
    if (texture->decode_pvrtc) stop("CPU compressed PVRTC read requires original compressed backing",selector);
    if (region.x%format.width || region.y%format.height ||
        (region.width%format.width && region.x+region.width!=width) || (region.height%format.height && region.y+region.height!=height))
        stop("Misaligned compressed texture read",selector);
    size_t columns=(region.width+format.width-1)/format.width,rows=(region.height+format.height-1)/format.height;
    if (columns>SIZE_MAX/format.bytes) stop("Texture read too large",selector);
    size_t packed_row=columns*format.bytes;
    if (rows>SIZE_MAX/packed_row || region.depth>SIZE_MAX/(packed_row*rows)) stop("Texture read too large",selector);
    size_t packed_image=packed_row*rows,total=packed_image*region.depth;
    if (format.metal>=160 && format.metal<=167) {
        unsigned bits=(format.metal&2) ? 4 : 2;
        size_t stored_width=width<(bits==2 ? 16U : 8U) ? (bits==2 ? 16U : 8U) : width,stored_height=height<8 ? 8 : height;
        if (region.x || region.y || region.z || region.width!=width || region.height!=height || region.depth!=1 ||
            stored_width>SIZE_MAX/stored_height || stored_width*stored_height>SIZE_MAX/bits)
            stop("PVRTC read must cover an entire mip",selector);
        total=packed_image=packed_row=stored_width*stored_height*bits/8; rows=1; row_stride=image_stride=0;
    }
    if (!row_stride) row_stride=packed_row;
    if (row_stride<packed_row || rows>SIZE_MAX/row_stride) stop("Invalid texture read row pitch",selector);
    if (!image_stride) image_stride=row_stride*rows;
    if (image_stride<(rows-1)*row_stride+packed_row || region.depth>SIZE_MAX/image_stride)
        stop("Invalid texture read image pitch",selector);
    int combined=format.aspects==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);
    size_t pixels=0,depth_plane=0;
    if (combined) {
        pixels=total/format.bytes;
        if (pixels>SIZE_MAX/5) stop("Depth/stencil read too large",selector);
        depth_plane=pixels*4; total=depth_plane+pixels;
    }
    pthread_mutex_lock(&metal_texture_reader.mutex);
    metal_object_retain((Word)texture);
    pthread_mutex_lock(&texture->mutex);
    VkResult result=metal_upload_flush();
    if (result==VK_SUCCESS) result=metal_texture_reader_prepare(total);
    VkImageLayout final_layout=texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : texture->layout;
    if (result==VK_SUCCESS) {
        VkCommandBuffer command=metal_texture_reader.command;
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=texture->layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=texture->image,
            .subresourceRange={format.aspects,0,(uint32_t)texture->mips,0,(uint32_t)texture->layers}};
        VKD(vkCmdPipelineBarrier)(command,texture->layout==VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy copies[2]={{.imageSubresource={combined ? VK_IMAGE_ASPECT_DEPTH_BIT : format.aspects,(uint32_t)mip,(uint32_t)slice,1},
            .imageOffset={(int32_t)region.x,(int32_t)region.y,(int32_t)region.z},
            .imageExtent={(uint32_t)region.width,(uint32_t)region.height,(uint32_t)region.depth}}};
        if (combined) { copies[1]=copies[0]; copies[1].bufferOffset=depth_plane; copies[1].imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT; }
        VKD(vkCmdCopyImageToBuffer)(command,texture->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,metal_texture_reader.buffer.buffer,combined ? 2 : 1,copies);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout=final_layout;
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=metal_texture_reader.buffer.buffer,.size=VK_WHOLE_SIZE};
        VKD(vkCmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,1,&barrier);
        result=VKD(vkEndCommandBuffer)(command);
        if (result==VK_SUCCESS) {
            VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
            pthread_mutex_lock(&vulkan_mutex);
            result=VKD(vkQueueSubmit)(vulkan.queue,1,&submit,metal_texture_reader.fence);
            pthread_mutex_unlock(&vulkan_mutex);
            if (result==VK_SUCCESS) texture->layout=final_layout;
        }
    }
    pthread_mutex_unlock(&texture->mutex);
    if (result==VK_SUCCESS) result=VKD(vkWaitForFences)(vulkan.device,1,&metal_texture_reader.fence,VK_TRUE,UINT64_MAX);
    if (result==VK_SUCCESS && !(metal_texture_reader.buffer.memory_flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=metal_texture_reader.buffer.memory,.size=VK_WHOLE_SIZE};
        result=VKD(vkInvalidateMappedMemoryRanges)(vulkan.device,1,&range);
    }
    if (result==VK_SUCCESS) {
        const unsigned char *mapped=metal_texture_reader.buffer.mapped;
        if (!combined && row_stride==packed_row && image_stride==packed_image) memcpy(bytes,mapped,total);
        else for (size_t z=0;z<region.depth;++z) for (size_t y=0;y<rows;++y) {
            unsigned char *out=(unsigned char *)bytes+z*image_stride+y*row_stride;
            if (!combined) memcpy(out,mapped+z*packed_image+y*packed_row,packed_row);
            else for (size_t x=0;x<region.width;++x) {
                size_t index=(z*region.height+y)*region.width+x;
                uint32_t value; memcpy(&value,mapped+index*4,4);
                if (format.metal==255) { value=(value&0xffffffU)|((uint32_t)mapped[depth_plane+index]<<24); memcpy(out+x*4,&value,4); }
                else {
                    if (format.format==VK_FORMAT_D24_UNORM_S8_UINT) {
                        float depth_value=(float)(value&0xffffffU)/16777215.0f;
                        memcpy(out+x*8,&depth_value,4);
                    } else memcpy(out+x*8,&value,4);
                    out[x*8+4]=mapped[depth_plane+index]; memset(out+x*8+5,0,3);
                }
            }
        }
    }
    if (metal_texture_reader.buffer.allocation>64*1024*1024 && (result==VK_SUCCESS || result==VK_ERROR_DEVICE_LOST)) {
        metal_buffer_dispose(&metal_texture_reader.buffer); metal_texture_reader.buffer.length=0;
    }
    metal_object_release((Word)texture);
    pthread_mutex_unlock(&metal_texture_reader.mutex);
    if (result!=VK_SUCCESS) stop("Vulkan texture CPU read failed",selector);
}
static void metal_texture_get_bytes_2d(struct MetalTexture *texture,const char *selector,void *bytes,Word row_stride,struct MetalRegion region,Word mip) {
    metal_texture_get_bytes(texture,selector,bytes,row_stride,0,region,mip,0);
}
static Word metal_texture_property(struct MetalTexture *texture,const char *selector) {
    if (!strcmp(selector,"pixelFormat")) return texture->format.metal;
    if (!strcmp(selector,"textureType")) return texture->type;
    if (!strcmp(selector,"width")) return texture->width;
    if (!strcmp(selector,"height")) return texture->height;
    if (!strcmp(selector,"depth")) return texture->depth;
    if (!strcmp(selector,"mipmapLevelCount")) return texture->mips;
    if (!strcmp(selector,"sampleCount")) return texture->samples;
    if (!strcmp(selector,"arrayLength")) return texture->array_length;
    if (!strcmp(selector,"usage")) return texture->usage;
    if (!strcmp(selector,"storageMode")) return (texture->object.options>>4)&15;
    if (!strcmp(selector,"cpuCacheMode")) return texture->object.options&15;
    if (!strcmp(selector,"allocatedSize")) return texture->allocation;
    return 0;
}
static Method metal_textures_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice") && !strcmp(selector,"newTextureWithDescriptor:")) return (Method)metal_texture_new;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalTexture")) {
        if (!strcmp(selector,"device")) return (Method)metal_resource_device;
        if (!strcmp(selector,"label")) return (Method)metal_resource_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
        if (!strcmp(selector,"replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:")) return (Method)metal_texture_replace;
        if (!strcmp(selector,"replaceRegion:mipmapLevel:withBytes:bytesPerRow:")) return (Method)metal_texture_replace_2d;
        if (!strcmp(selector,"getBytes:bytesPerRow:fromRegion:mipmapLevel:")) return (Method)metal_texture_get_bytes_2d;
        if (!strcmp(selector,"getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:")) return (Method)metal_texture_get_bytes;
        const char *properties[]={"pixelFormat","textureType","width","height","depth","mipmapLevelCount","sampleCount","arrayLength","usage","storageMode","cpuCacheMode","allocatedSize","framebufferOnly"};
        for (unsigned i=0;i<COUNT(properties);++i) if (!strcmp(selector,properties[i])) return (Method)metal_texture_property;
    }
    return NULL;
}
#undef VKD
