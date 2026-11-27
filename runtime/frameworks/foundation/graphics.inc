 
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <android/native_window_jni.h>

struct GraphicsJob { void (*run)(void *); void *data; int done; struct GraphicsJob *next; };
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int launched,ready,available;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface offscreen;
    ANativeWindow *window;
    unsigned window_generation;
    int width,height,major,minor,max_texture,max_samples;
    char renderer[256],version[256],error[256];
    struct GraphicsJob *first,*last;
} graphics={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};

static void graphics_initialize(void) {
    graphics.display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major=0,minor=0,count=0;
    if (graphics.display==EGL_NO_DISPLAY || !eglInitialize(graphics.display,&major,&minor)) goto failed;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) goto failed;
    const EGLint config[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    if (!eglChooseConfig(graphics.display,config,&graphics.config,1,&count) || count!=1) goto failed;
    const EGLint context[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    graphics.context=eglCreateContext(graphics.display,graphics.config,EGL_NO_CONTEXT,context);
    if (graphics.context==EGL_NO_CONTEXT) goto failed;
    const EGLint surface[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    graphics.offscreen=eglCreatePbufferSurface(graphics.display,graphics.config,surface);
    if (graphics.offscreen==EGL_NO_SURFACE || !eglMakeCurrent(graphics.display,graphics.offscreen,graphics.offscreen,graphics.context)) goto failed;
    glGetIntegerv(GL_MAJOR_VERSION,&graphics.major); glGetIntegerv(GL_MINOR_VERSION,&graphics.minor);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&graphics.max_texture); glGetIntegerv(GL_MAX_SAMPLES,&graphics.max_samples);
    const char *renderer=(const char *)glGetString(GL_RENDERER),*version=(const char *)glGetString(GL_VERSION);
    snprintf(graphics.renderer,sizeof(graphics.renderer),"%s",renderer ? renderer : "unknown");
    snprintf(graphics.version,sizeof(graphics.version),"%s",version ? version : "unknown");
    if (graphics.major<3 || glGetError()!=GL_NO_ERROR) goto failed;
    graphics.available=1;
    LOG("Android GPU context: EGL %d.%d, %s, renderer=%s, maxTexture=%d, maxSamples=%d",major,minor,graphics.version,graphics.renderer,graphics.max_texture,graphics.max_samples);
    return;
failed:
    snprintf(graphics.error,sizeof(graphics.error),"EGL/GLES initialization failed (EGL=0x%x)",eglGetError());
    LOG("%s",graphics.error);
    if (graphics.display!=EGL_NO_DISPLAY) {
        eglMakeCurrent(graphics.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (graphics.offscreen!=EGL_NO_SURFACE) eglDestroySurface(graphics.display,graphics.offscreen);
        if (graphics.context!=EGL_NO_CONTEXT) eglDestroyContext(graphics.display,graphics.context);
        eglTerminate(graphics.display);
    }
}
static void *graphics_worker(void *unused) {
    pthread_setname_np(pthread_self(),"I2A GLES");
    graphics_initialize();
    pthread_mutex_lock(&graphics.mutex);
    graphics.ready=1; pthread_cond_broadcast(&graphics.condition);
    for (;;) {
        while (!graphics.first) pthread_cond_wait(&graphics.condition,&graphics.mutex);
        struct GraphicsJob *job=graphics.first;
        graphics.first=job->next; if (!graphics.first) graphics.last=NULL;
        pthread_mutex_unlock(&graphics.mutex);
        job->run(job->data);
        pthread_mutex_lock(&graphics.mutex);
        job->done=1; pthread_cond_broadcast(&graphics.condition);
    }
    return NULL;
}
static int graphics_start(void) {
    pthread_mutex_lock(&graphics.mutex);
    if (!graphics.launched) {
        graphics.launched=1;
        int error=pthread_create(&graphics.thread,NULL,graphics_worker,NULL);
        if (error) {
            graphics.ready=1; snprintf(graphics.error,sizeof(graphics.error),"GPU worker: %s",strerror(error));
        } else pthread_detach(graphics.thread);
    }
    while (!graphics.ready) pthread_cond_wait(&graphics.condition,&graphics.mutex);
    int result=graphics.available;
    pthread_mutex_unlock(&graphics.mutex); return result;
}
static void graphics_run(void (*run)(void *),void *data) {
    if (!graphics_start()) stop("Android GPU unavailable",graphics.error);
    if (pthread_equal(pthread_self(),graphics.thread)) { run(data); return; }
    struct GraphicsJob job={.run=run,.data=data};
    pthread_mutex_lock(&graphics.mutex);
    if (graphics.last) graphics.last->next=&job; else graphics.first=&job;
    graphics.last=&job; pthread_cond_broadcast(&graphics.condition);
    while (!job.done) pthread_cond_wait(&graphics.condition,&graphics.mutex);
    pthread_mutex_unlock(&graphics.mutex);
}
JNIEXPORT void JNICALL HOST_JNI(HostActivity_setRenderSurface)(JNIEnv *env,jobject instance,jobject surface,jint width,jint height) {
    ANativeWindow *window=surface ? ANativeWindow_fromSurface(env,surface) : NULL;
    pthread_mutex_lock(&graphics.mutex);
    ANativeWindow *old=graphics.window;
    graphics.window=window; graphics.width=width; graphics.height=height; ++graphics.window_generation;
    pthread_mutex_unlock(&graphics.mutex);
    if (old) ANativeWindow_release(old);
    LOG("Android render Surface %s %dx%d",window ? "available" : "removed",width,height);
}

#include "../../backends/vulkan/device.inc"

struct MetalLayer { Word object,device; Word pixel_format; struct GuestSize size; unsigned framebuffer_only,transaction,asynchronous; struct MetalLayer *next; };
static struct MetalLayer *metal_layers;
static struct MetalLayer *metal_layer(Word object) {
    for (struct MetalLayer *layer=metal_layers;layer;layer=layer->next) if (layer->object==object) return layer;
    struct MetalLayer *layer=calloc(1,sizeof(*layer));
    if (!layer) stop("Allocation failed","CAMetalLayer");
    struct ViewState *view=view_state(object);
    layer->object=object; layer->pixel_format=80; layer->framebuffer_only=1;
    layer->size=(struct GuestSize){view->bounds.width*view->scale,view->bounds.height*view->scale};
    layer->next=metal_layers; metal_layers=layer; return layer;
}
static struct HostObject metal_device;
static Word metal_default_device(void) {
    int use_vulkan=vulkan_start();
    if (use_vulkan) {
        snprintf(graphics.renderer,sizeof(graphics.renderer),"%s",vulkan.properties.deviceName);
        graphics.max_texture=vulkan.properties.limits.maxImageDimension2D;
    } else if (!graphics_start()) { LOG("MTL compatibility device unavailable: %s",graphics.error); return 0; }
    metal_device.isa=external_class("_OBJC_CLASS_$_I2AMetalDevice");
    metal_device.content[0]=use_vulkan;
    LOG("MTL compatibility device created using Android %s (command translation remains incremental)",use_vulkan ? "Vulkan" : "GLES");
    return (Word)&metal_device;
}
static Word metal_device_name(void) { return copy_string(graphics.renderer); }
static Word metal_layer_device(Word object) { return metal_layer(object)->device; }
static void metal_set_device(Word object,const char *selector,Word device) {
    if (device && device!=(Word)&metal_device) stop("Unknown Metal device",selector);
    metal_layer(object)->device=device;
}
static Word metal_layer_integer(Word object,const char *selector) {
    struct MetalLayer *layer=metal_layer(object);
    if (!strcmp(selector,"pixelFormat")) return layer->pixel_format;
    if (!strcmp(selector,"framebufferOnly")) return layer->framebuffer_only;
    if (!strcmp(selector,"presentsWithTransaction")) return layer->transaction;
    return layer->asynchronous;
}
static void metal_layer_set_integer(Word object,const char *selector,Word value) {
    struct MetalLayer *layer=metal_layer(object);
    if (!strcmp(selector,"setPixelFormat:")) {
        int xr=value>=552 && value<=555;
        if (value!=80 && value!=81 && !xr) stop("Unsupported drawable pixel format",selector);
        if (xr) {
            if (!vulkan.available) stop("XR drawable requires a floating-point presentation path",selector);
            PFN_vkGetPhysicalDeviceFormatProperties query=(PFN_vkGetPhysicalDeviceFormatProperties)vulkan.get_instance_proc(vulkan.instance,"vkGetPhysicalDeviceFormatProperties");
            VkFormatProperties properties; query(vulkan.physical,VK_FORMAT_R16G16B16A16_SFLOAT,&properties);
            VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((properties.optimalTilingFeatures&required)!=required) stop("XR floating-point backing unavailable",selector);
            LOG("CAMetalLayer pixelFormat=%lu: RGBA16F backing supported; XR presentation conversion pending",value);
        }
        layer->pixel_format=value;
    } else if (!strcmp(selector,"setFramebufferOnly:")) layer->framebuffer_only=value!=0;
    else if (!strcmp(selector,"setPresentsWithTransaction:")) {
        if (value) stop("Unsupported CoreAnimation transaction presentation",selector);
        layer->transaction=0;
    } else layer->asynchronous=value!=0;
}
static struct GuestSize metal_drawable_size(Word object) { return metal_layer(object)->size; }
static void metal_set_drawable_size(Word object,const char *selector,struct GuestSize size) {
    if (!isfinite(size.width) || !isfinite(size.height) || size.width<0 || size.height<0 || size.width>graphics.max_texture || size.height>graphics.max_texture)
        stop("Invalid drawable size",selector);
    metal_layer(object)->size=size;
}
#include "../../diagnostics/metal_diagnostic_state.inc"
#include "../../backends/vulkan/command_queue.inc"
#include "../metal/descriptors.inc"
#include "../../backends/vulkan/resources.inc"
#include "../../backends/vulkan/textures.inc"
#include "../metal/shaders.inc"
#include "../../backends/vulkan/compiler.inc"
#include "../../backends/vulkan/render_pipeline.inc"
#include "../../backends/vulkan/compute_pipeline.inc"
#include "../../backends/vulkan/command_buffer.inc"
#include "../../backends/vulkan/render_encoder.inc"
#include "../../diagnostics/metal_diagnostics.inc"
#include "../../backends/vulkan/compute_encoder.inc"
#include "../../backends/vulkan/presentation.inc"
static Method graphics_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    Method present=metal_present_method(object,selector,receiver,class_receiver);
    if (present) return present;
    Method render=metal_render_method(object,selector,receiver,class_receiver);
    if (render) return render;
    Method compute_encoder=metal_compute_encoder_method(object,selector,receiver,class_receiver);
    if (compute_encoder) return compute_encoder;
    Method command=metal_commands_method(object,selector,receiver,class_receiver);
    if (command) return command;
    Method compute=metal_compute_method(object,selector,receiver,class_receiver);
    if (compute) return compute;
    Method pipeline=metal_pipeline_method(object,selector,receiver,class_receiver);
    if (pipeline) return pipeline;
    Method shader=metal_shaders_method(object,selector,receiver,class_receiver);
    if (shader) return shader;
    Method texture=metal_textures_method(object,selector,receiver,class_receiver);
    if (texture) return texture;
    Method resource=metal_resources_method(object,selector,receiver,class_receiver);
    if (resource) return resource;
    Method queue=metal_queue_method(object,selector,receiver,class_receiver);
    if (queue) return queue;
    Method descriptor=metal_descriptors_method(object,selector,receiver,class_receiver);
    if (descriptor) return descriptor;
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice")) {
        if (!strcmp(selector,"name")) return (Method)metal_device_name;
        if (!strcmp(selector,"newCommandQueue")) return (Method)metal_new_queue;
        if (!strcmp(selector,"newCommandQueueWithMaxCommandBufferCount:")) return (Method)metal_new_queue_limit;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_CAMetalLayer")) {
        if (!strcmp(selector,"device")) return (Method)metal_layer_device;
        if (!strcmp(selector,"setDevice:")) return (Method)metal_set_device;
        if (!strcmp(selector,"pixelFormat") || !strcmp(selector,"framebufferOnly") || !strcmp(selector,"presentsWithTransaction") || !strcmp(selector,"drawsAsynchronously")) return (Method)metal_layer_integer;
        if (!strcmp(selector,"setPixelFormat:") || !strcmp(selector,"setFramebufferOnly:") || !strcmp(selector,"setPresentsWithTransaction:") || !strcmp(selector,"setDrawsAsynchronously:")) return (Method)metal_layer_set_integer;
        if (!strcmp(selector,"drawableSize")) return (Method)metal_drawable_size;
        if (!strcmp(selector,"setDrawableSize:")) return (Method)metal_set_drawable_size;
    }
    return NULL;
}
