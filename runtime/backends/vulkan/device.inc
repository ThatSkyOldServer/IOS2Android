 
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
enum MetalShaderRequirement {
    METAL_SHADER_VERTEX_STORES=1U<<0, METAL_SHADER_FRAGMENT_STORES=1U<<1,
    METAL_SHADER_IMAGE_READ=1U<<2, METAL_SHADER_IMAGE_WRITE=1U<<3,
    METAL_SHADER_STORAGE_FORMATS=1U<<4, METAL_SHADER_FLOAT64=1U<<5,
    METAL_SHADER_INT64=1U<<6, METAL_SHADER_INT16=1U<<7,
    METAL_SHADER_SAMPLE_RATE=1U<<8, METAL_SHADER_FEATURE_MASK=(1U<<9)-1,
    METAL_SHADER_STORAGE_WRITES=1U<<16
};
static struct {
    int attempted,available,pvrtc;
    void *library;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkGetDeviceProcAddr get_device_proc;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceFeatures features;
    uint32_t shader_requirements;
    uint32_t api_version;
    VkPhysicalDeviceDepthStencilResolveProperties depth_resolve;
    PFN_vkCreateRenderPass2 create_render_pass2;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t timestamp_bits;
    PFN_vkGetCalibratedTimestampsEXT calibrated_timestamps;
    VkTimeDomainEXT host_time_domain;
    VkSurfaceKHR surface;
    ANativeWindow *window;
    unsigned window_generation;
    char error[256];
} vulkan;
static pthread_mutex_t vulkan_mutex=PTHREAD_MUTEX_INITIALIZER;
static int vulkan_start(void) {
    pthread_mutex_lock(&vulkan_mutex);
    if (vulkan.attempted) { int result=vulkan.available; pthread_mutex_unlock(&vulkan_mutex); return result; }
    vulkan.attempted=1;
    VkResult result=VK_ERROR_INITIALIZATION_FAILED;
    const char *operation="load libvulkan.so";
    vulkan.library=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if (!vulkan.library) goto failed;
    vulkan.get_instance_proc=(PFN_vkGetInstanceProcAddr)dlsym(vulkan.library,"vkGetInstanceProcAddr");
    if (!vulkan.get_instance_proc) goto failed;
    PFN_vkEnumerateInstanceExtensionProperties instance_extensions=(PFN_vkEnumerateInstanceExtensionProperties)vulkan.get_instance_proc(NULL,"vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_instance=(PFN_vkCreateInstance)vulkan.get_instance_proc(NULL,"vkCreateInstance");
    if (!instance_extensions || !create_instance) goto failed;
    operation="enumerate instance extensions";
    uint32_t extension_count=0;
    result=instance_extensions(NULL,&extension_count,NULL);
    if (result!=VK_SUCCESS || extension_count>4096) goto failed;
    VkExtensionProperties *extensions=calloc(extension_count ? extension_count : 1,sizeof(*extensions));
    if (!extensions) goto failed;
    result=instance_extensions(NULL,&extension_count,extensions);
    int surface_extension=0,android_extension=0;
    for (uint32_t i=0;i<extension_count && result==VK_SUCCESS;++i) {
        if (!strcmp(extensions[i].extensionName,VK_KHR_SURFACE_EXTENSION_NAME)) surface_extension=1;
        if (!strcmp(extensions[i].extensionName,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) android_extension=1;
    }
    free(extensions);
    operation="VK_KHR_surface / VK_KHR_android_surface";
    if (!surface_extension || !android_extension) goto failed;
    const char *instance_names[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    uint32_t loader_version=VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate_version=(PFN_vkEnumerateInstanceVersion)vulkan.get_instance_proc(NULL,"vkEnumerateInstanceVersion");
    if (enumerate_version && enumerate_version(&loader_version)!=VK_SUCCESS) loader_version=VK_API_VERSION_1_0;
    vulkan.api_version=loader_version<VK_API_VERSION_1_2 ? loader_version : VK_API_VERSION_1_2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName=GUEST_APP_NAME,.applicationVersion=GUEST_APP_BUILD,.apiVersion=vulkan.api_version};
    VkInstanceCreateInfo instance={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledExtensionCount=2,.ppEnabledExtensionNames=instance_names};
    operation="vkCreateInstance"; result=create_instance(&instance,NULL,&vulkan.instance);
    if (result!=VK_SUCCESS) goto failed;
#define VK_INSTANCE(type,name) PFN_##type name=(PFN_##type)vulkan.get_instance_proc(vulkan.instance,#type)
    VK_INSTANCE(vkEnumeratePhysicalDevices,enumerate_devices);
    VK_INSTANCE(vkGetPhysicalDeviceProperties,properties);
    VK_INSTANCE(vkGetPhysicalDeviceMemoryProperties,memory);
    VK_INSTANCE(vkGetPhysicalDeviceFeatures,features);
    VK_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties,queue_properties);
    VK_INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR,presentation);
    VK_INSTANCE(vkCreateAndroidSurfaceKHR,create_surface);
    VK_INSTANCE(vkEnumerateDeviceExtensionProperties,device_extensions);
    VK_INSTANCE(vkCreateDevice,create_device);
    if (!enumerate_devices || !properties || !memory || !features || !queue_properties || !presentation || !create_surface || !device_extensions || !create_device) goto failed;
    uint32_t count=0;
    operation="vkEnumeratePhysicalDevices"; result=enumerate_devices(vulkan.instance,&count,NULL);
    LOG("Vulkan physical device enumeration: result=%d count=%u",result,count);
    if (result!=VK_SUCCESS || !count || count>32) goto failed;
    VkPhysicalDevice devices[32];
    result=enumerate_devices(vulkan.instance,&count,devices);
    if (result!=VK_SUCCESS) goto failed;
    pthread_mutex_lock(&graphics.mutex);
    vulkan.window=graphics.window;
    vulkan.window_generation=graphics.window_generation;
    if (vulkan.window) ANativeWindow_acquire(vulkan.window);
    pthread_mutex_unlock(&graphics.mutex);
    operation="Android Surface";
    if (!vulkan.window) goto failed;
    VkAndroidSurfaceCreateInfoKHR surface={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=vulkan.window};
    operation="vkCreateAndroidSurfaceKHR"; result=create_surface(vulkan.instance,&surface,NULL,&vulkan.surface);
    if (result!=VK_SUCCESS) goto failed;
    for (uint32_t i=0;i<count;++i) {
        VkPhysicalDeviceProperties prop;
        properties(devices[i],&prop);
        LOG("Vulkan device candidate: %s api=%u.%u.%u vendor=0x%x device=0x%x",prop.deviceName,VK_VERSION_MAJOR(prop.apiVersion),VK_VERSION_MINOR(prop.apiVersion),VK_VERSION_PATCH(prop.apiVersion),prop.vendorID,prop.deviceID);
        uint32_t queue_count=0;
        queue_properties(devices[i],&queue_count,NULL);
        if (!queue_count || queue_count>256) continue;
        VkQueueFamilyProperties families[256]; queue_properties(devices[i],&queue_count,families);
        uint32_t selected=UINT32_MAX;
        for (uint32_t j=0;j<queue_count;++j) {
            VkBool32 present=VK_FALSE;
            if (families[j].queueCount && (families[j].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT) && presentation(devices[i],j,vulkan.surface,&present)==VK_SUCCESS && present) { selected=j; break; }
        }
        if (selected!=UINT32_MAX) vulkan.timestamp_bits=families[selected].timestampValidBits;
        if (selected==UINT32_MAX) continue;
        uint32_t ext_count=0;
        if (device_extensions(devices[i],NULL,&ext_count,NULL)!=VK_SUCCESS || ext_count>4096) continue;
        VkExtensionProperties *device_ext=calloc(ext_count ? ext_count : 1,sizeof(*device_ext));
        if (!device_ext) continue;
        int swapchain=0,pvrtc=0,renderpass2=0,depth_resolve=0,calibrated=0;
        if (device_extensions(devices[i],NULL,&ext_count,device_ext)==VK_SUCCESS)
            for (uint32_t j=0;j<ext_count;++j) {
                if (!strcmp(device_ext[j].extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchain=1;
                if (!strcmp(device_ext[j].extensionName,VK_IMG_FORMAT_PVRTC_EXTENSION_NAME)) pvrtc=1;
                if (!strcmp(device_ext[j].extensionName,VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) renderpass2=1;
                if (!strcmp(device_ext[j].extensionName,VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) depth_resolve=1;
                if (!strcmp(device_ext[j].extensionName,VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) calibrated=1;
            }
        free(device_ext);
        if (!swapchain) continue;
        VkPhysicalDeviceFeatures supported; features(devices[i],&supported);
        VkPhysicalDeviceFeatures enabled={.samplerAnisotropy=supported.samplerAnisotropy,.independentBlend=supported.independentBlend,.textureCompressionETC2=supported.textureCompressionETC2,
            .textureCompressionASTC_LDR=supported.textureCompressionASTC_LDR,.textureCompressionBC=supported.textureCompressionBC,.fillModeNonSolid=supported.fillModeNonSolid,
            .depthClamp=supported.depthClamp,.depthBiasClamp=supported.depthBiasClamp,
            .alphaToOne=supported.alphaToOne,.sampleRateShading=supported.sampleRateShading,
            .shaderFloat64=supported.shaderFloat64,.shaderInt64=supported.shaderInt64,.shaderInt16=supported.shaderInt16,
            .shaderStorageImageExtendedFormats=supported.shaderStorageImageExtendedFormats,.imageCubeArray=supported.imageCubeArray,
            .shaderStorageImageReadWithoutFormat=supported.shaderStorageImageReadWithoutFormat,
            .shaderStorageImageWriteWithoutFormat=supported.shaderStorageImageWriteWithoutFormat,
            .fragmentStoresAndAtomics=supported.fragmentStoresAndAtomics,
            .vertexPipelineStoresAndAtomics=supported.vertexPipelineStoresAndAtomics};
        float priority=1;
        VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=selected,.queueCount=1,.pQueuePriorities=&priority};
        uint32_t api=prop.apiVersion<vulkan.api_version ? prop.apiVersion : vulkan.api_version;
        int resolve_core=api>=VK_API_VERSION_1_2;
        int resolve_extension=!resolve_core && api>=VK_API_VERSION_1_1 && renderpass2 && depth_resolve;
        const char *names[5]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};unsigned name_count=1;
        if (pvrtc) names[name_count++]=VK_IMG_FORMAT_PVRTC_EXTENSION_NAME;
        if (resolve_extension) { names[name_count++]=VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;names[name_count++]=VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME; }
        if(calibrated)names[name_count++]=VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue,.enabledExtensionCount=name_count,.ppEnabledExtensionNames=names,.pEnabledFeatures=&enabled};
        operation="vkCreateDevice"; result=create_device(devices[i],&info,NULL,&vulkan.device);
        if (result!=VK_SUCCESS) continue;
        vulkan.physical=devices[i]; vulkan.properties=prop; vulkan.features=enabled; vulkan.queue_family=selected; vulkan.pvrtc=pvrtc;
        vulkan.shader_requirements=(enabled.vertexPipelineStoresAndAtomics ? METAL_SHADER_VERTEX_STORES : 0) |
            (enabled.fragmentStoresAndAtomics ? METAL_SHADER_FRAGMENT_STORES : 0) |
            (enabled.shaderStorageImageReadWithoutFormat ? METAL_SHADER_IMAGE_READ : 0) |
            (enabled.shaderStorageImageWriteWithoutFormat ? METAL_SHADER_IMAGE_WRITE : 0) |
            (enabled.shaderStorageImageExtendedFormats ? METAL_SHADER_STORAGE_FORMATS : 0) |
            (enabled.shaderFloat64 ? METAL_SHADER_FLOAT64 : 0) | (enabled.shaderInt64 ? METAL_SHADER_INT64 : 0) |
            (enabled.shaderInt16 ? METAL_SHADER_INT16 : 0) | (enabled.sampleRateShading ? METAL_SHADER_SAMPLE_RATE : 0);
        memory(vulkan.physical,&vulkan.memory);
        vulkan.get_device_proc=(PFN_vkGetDeviceProcAddr)vulkan.get_instance_proc(vulkan.instance,"vkGetDeviceProcAddr");
        if(calibrated && vulkan.timestamp_bits==64) {
            PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains=(PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
            uint32_t domain_count=0;
            if(domains && domains(vulkan.physical,&domain_count,NULL)==VK_SUCCESS && domain_count<=16) {
                VkTimeDomainEXT available[16];int device_clock=0,host_clock=0;
                if(domains(vulkan.physical,&domain_count,available)==VK_SUCCESS)for(unsigned d=0;d<domain_count;++d) {
                    device_clock|=available[d]==VK_TIME_DOMAIN_DEVICE_EXT;
                    host_clock|=available[d]==VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
                }
                if(device_clock && host_clock) {
                    vulkan.host_time_domain=VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
                    vulkan.calibrated_timestamps=(PFN_vkGetCalibratedTimestampsEXT)vulkan.get_device_proc(vulkan.device,"vkGetCalibratedTimestampsEXT");
                }
            }
        }
        if (resolve_core || resolve_extension) {
            PFN_vkGetPhysicalDeviceProperties2 query_properties=(PFN_vkGetPhysicalDeviceProperties2)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceProperties2");
            vulkan.create_render_pass2=(PFN_vkCreateRenderPass2)vulkan.get_device_proc(vulkan.device,resolve_core ? "vkCreateRenderPass2" : "vkCreateRenderPass2KHR");
            if (query_properties && vulkan.create_render_pass2) {
                vulkan.depth_resolve=(VkPhysicalDeviceDepthStencilResolveProperties){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
                VkPhysicalDeviceProperties2 properties2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&vulkan.depth_resolve};
                query_properties(vulkan.physical,&properties2);
            } else vulkan.create_render_pass2=NULL;
        }
        PFN_vkGetDeviceQueue get_queue=(PFN_vkGetDeviceQueue)vulkan.get_device_proc(vulkan.device,"vkGetDeviceQueue");
        get_queue(vulkan.device,selected,0,&vulkan.queue);
        vulkan.available=1;
        snprintf(crash_gpu_context,sizeof(crash_gpu_context),"gpu=%s vendor=0x%x device=0x%x api=%u.%u.%u driver=0x%x max_texture=%u storage_extended=%u storage_write_without_format=%u independent_blend=%u",
            prop.deviceName,prop.vendorID,prop.deviceID,VK_VERSION_MAJOR(prop.apiVersion),VK_VERSION_MINOR(prop.apiVersion),VK_VERSION_PATCH(prop.apiVersion),
            prop.driverVersion,prop.limits.maxImageDimension2D,enabled.shaderStorageImageExtendedFormats,enabled.shaderStorageImageWriteWithoutFormat,enabled.independentBlend);
        __atomic_store_n(&crash_gpu_ready,1,__ATOMIC_RELEASE);
        LOG("Vulkan Android device ready: %s, graphics/present queue=%u, maxTexture=%u",prop.deviceName,selected,prop.limits.maxImageDimension2D);
        pthread_mutex_unlock(&vulkan_mutex); return 1;
    }
    operation="graphics/presentation device with VK_KHR_swapchain";
failed:
    snprintf(vulkan.error,sizeof(vulkan.error),"%s unavailable (VkResult=%d)",operation,result);
    LOG("Vulkan probe: %s",vulkan.error);
    if (vulkan.instance) {
        VK_INSTANCE(vkDestroySurfaceKHR,destroy_surface);
        VK_INSTANCE(vkDestroyInstance,destroy_instance);
        if (vulkan.surface && destroy_surface) destroy_surface(vulkan.instance,vulkan.surface,NULL);
        if (destroy_instance) destroy_instance(vulkan.instance,NULL);
        vulkan.instance=VK_NULL_HANDLE; vulkan.surface=VK_NULL_HANDLE;
    }
    if (vulkan.window) { ANativeWindow_release(vulkan.window); vulkan.window=NULL; }
    pthread_mutex_unlock(&vulkan_mutex);
    record_failure("Vulkan initialization failed",vulkan.error);return 0;
#undef VK_INSTANCE
}
