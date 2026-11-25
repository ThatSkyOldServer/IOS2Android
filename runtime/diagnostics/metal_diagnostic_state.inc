 
#if I2A_DEVELOPMENT
static int metal_diagnostic_enabled,metal_diagnostic_readback;
static int shader_diagnostics_requested;
static void metal_math_configure_observation(int enabled);
static uint64_t metal_descriptor_cache_counts[2];
static uint64_t metal_layout_counts[2];
#else
#define metal_diagnostic_enabled 0
#define metal_diagnostic_readback 0
#endif
static char metal_diagnostic_directory[PATH_MAX];
static uint64_t metal_diagnostic_ns[3],metal_diagnostic_calls[3];
static unsigned metal_diagnostic_frames;
static uint64_t metal_memory_census[6];  
static void metal_memory_add(unsigned kind,uint64_t bytes) { if (I2A_DEVELOPMENT) __atomic_add_fetch(&metal_memory_census[kind],bytes,__ATOMIC_RELAXED); }
static void metal_memory_sub(unsigned kind,uint64_t bytes) { if (I2A_DEVELOPMENT) __atomic_sub_fetch(&metal_memory_census[kind],bytes,__ATOMIC_RELAXED); }
struct MetalCommand;
struct MetalBuffer;
static struct MetalBuffer *metal_math_buffer(struct MetalCommand *);
static void metal_math_complete(struct MetalCommand *);
struct MetalRenderEncoder;
struct MetalFunction;
static void metal_math_trace_render(struct MetalRenderEncoder *,struct MetalFunction *);
static void metal_diagnostic_command_begin(struct MetalCommand *);
static void metal_diagnostic_command_end(struct MetalCommand *);
static void metal_diagnostic_command_complete(struct MetalCommand *);
static void metal_diagnostic_capture(struct MetalRenderEncoder *);
static void metal_diagnostic_present(void);
static uint64_t metal_diagnostic_time(void) {
    if (!metal_diagnostic_enabled) return 0;
    struct timespec time; clock_gettime(CLOCK_MONOTONIC,&time);
    return (uint64_t)time.tv_sec*1000000000ULL+(uint64_t)time.tv_nsec;
}
static void metal_diagnostic_add(unsigned category,uint64_t duration) {
    if (!metal_diagnostic_enabled) return;
    __atomic_fetch_add(&metal_diagnostic_ns[category],duration,__ATOMIC_RELAXED);
    __atomic_fetch_add(&metal_diagnostic_calls[category],1,__ATOMIC_RELAXED);
}
#if I2A_DEVELOPMENT
JNIEXPORT void JNICALL HOST_JNI(HostActivity_configureShaderDiagnostics)(JNIEnv *env,jobject object,jboolean enabled) {
    metal_math_configure_observation(enabled);
}
JNIEXPORT void JNICALL HOST_JNI(HostActivity_configureRenderDiagnostics)(JNIEnv *env,jobject object,jboolean enabled,jstring directory) {
    const char *path=(*env)->GetStringUTFChars(env,directory,NULL);
    if (path) {
        snprintf(metal_diagnostic_directory,sizeof(metal_diagnostic_directory),"%s",path);
        snprintf(tls_diagnostics_directory,sizeof(tls_diagnostics_directory),"%s",path);
        (*env)->ReleaseStringUTFChars(env,directory,path);
        metal_diagnostic_enabled=enabled;
        metal_diagnostic_readback=enabled;
        render_diagnostics_requested=enabled;
    }
}
JNIEXPORT void JNICALL HOST_JNI(HostActivity_configurePerformanceSampling)(JNIEnv *env,jobject object,jboolean enabled) {
    metal_diagnostic_readback=0;
    metal_diagnostic_enabled=enabled; render_diagnostics_requested=enabled;
    LOG("[DIAG-render] Live performance sampling %s (target readback disabled)",enabled ? "enabled" : "disabled");
}
#endif
