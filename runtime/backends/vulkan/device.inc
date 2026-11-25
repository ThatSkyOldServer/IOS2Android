 
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
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
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t timestamp_bits;
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
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName=GUEST_APP_NAME,.applicationVersion=GUEST_APP_BUILD,.apiVersion=VK_API_VERSION_1_0};
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
            if (families[j].queueCount && (families[j].queueFlags&VK_QUEUE_GRAPHICS_BIT) && presentation(devices[i],j,vulkan.surface,&present)==VK_SUCCESS && present) { selected=j; break; }
        }
        if (selected!=UINT32_MAX) vulkan.timestamp_bits=families[selected].timestampValidBits;
        if (selected==UINT32_MAX) continue;
        uint32_t ext_count=0;
        if (device_extensions(devices[i],NULL,&ext_count,NULL)!=VK_SUCCESS || ext_count>4096) continue;
        VkExtensionProperties *device_ext=calloc(ext_count ? ext_count : 1,sizeof(*device_ext));
        if (!device_ext) continue;
        int swapchain=0,pvrtc=0;
        if (device_extensions(devices[i],NULL,&ext_count,device_ext)==VK_SUCCESS)
            for (uint32_t j=0;j<ext_count;++j) {
                if (!strcmp(device_ext[j].extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchain=1;
                if (!strcmp(device_ext[j].extensionName,VK_IMG_FORMAT_PVRTC_EXTENSION_NAME)) pvrtc=1;
            }
        free(device_ext);
        if (!swapchain) continue;
        VkPhysicalDeviceFeatures supported; features(devices[i],&supported);
        VkPhysicalDeviceFeatures enabled={.samplerAnisotropy=supported.samplerAnisotropy,.independentBlend=supported.independentBlend,.textureCompressionETC2=supported.textureCompressionETC2,
            .textureCompressionASTC_LDR=supported.textureCompressionASTC_LDR,.textureCompressionBC=supported.textureCompressionBC,.fillModeNonSolid=supported.fillModeNonSolid,
            .depthClamp=supported.depthClamp,.shaderStorageImageExtendedFormats=supported.shaderStorageImageExtendedFormats,.imageCubeArray=supported.imageCubeArray,
            .shaderStorageImageWriteWithoutFormat=supported.shaderStorageImageWriteWithoutFormat,
            .fragmentStoresAndAtomics=I2A_SHADER_DIAGNOSTICS && supported.fragmentStoresAndAtomics,
            .vertexPipelineStoresAndAtomics=supported.vertexPipelineStoresAndAtomics};
        float priority=1;
        VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=selected,.queueCount=1,.pQueuePriorities=&priority};
        const char *names[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_IMG_FORMAT_PVRTC_EXTENSION_NAME};
        VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue,.enabledExtensionCount=pvrtc ? 2 : 1,.ppEnabledExtensionNames=names,.pEnabledFeatures=&enabled};
        operation="vkCreateDevice"; result=create_device(devices[i],&info,NULL,&vulkan.device);
        if (result!=VK_SUCCESS) continue;
        vulkan.physical=devices[i]; vulkan.properties=prop; vulkan.features=enabled; vulkan.queue_family=selected; vulkan.pvrtc=pvrtc;
        memory(vulkan.physical,&vulkan.memory);
        vulkan.get_device_proc=(PFN_vkGetDeviceProcAddr)vulkan.get_instance_proc(vulkan.instance,"vkGetDeviceProcAddr");
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
