#define _GNU_SOURCE 1
#ifndef I2A_DEVELOPMENT
#define I2A_DEVELOPMENT 1
#endif
#include <android/log.h>
#include <android/looper.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <plist/plist.h>
#include <time.h>
#include <zlib.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/file.h>
#include <sched.h>
#include <limits.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <sys/utsname.h>
#include <signal.h>
#include <stdarg.h>
#include <math.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "Block.h"
#include "Block_private.h"
#include "image.h"
#include "bundle.h"
#include "i2a/extension.h"
static struct I2AExtension runtime_extension;
static void initialize_extension(void);
_Static_assert(I2A_IMAGE_ABI == 2, "Image ABI mismatch");

extern unsigned char guest_text[], guest_data[], guest_links[], guest_gates[];
extern const unsigned char rebases[], binds[];
typedef uintptr_t Word;
typedef Word (*Method)(Word, const char *, Word, Word, Word, Word);
struct MethodRecord { const char *name, *types; Method implementation; };
static Method operations_method(Word,const char *,const char *,int);
static int in_image(Word);
static Word external_class(const char *);
static Word class_from_name(const char *);
static Word host_superclass(Word);
static size_t host_allocation_size(Word);
static uint32_t external_instance_size(Word);
static int class_layouts_ready;
static int class_ro_valid(Word,Word);
static uint32_t realize_class_layout(Word,unsigned);
static Word realize_class_from_swift(Word,Word);
static JNIEnv *attached_environment(const char *);
static Word image_data_normalize(Word);
static void schedule_queue_function(Word,void (*)(void *),void *,void (*)(void *));
static void cf_release(Word);
static uint32_t guest_image_count(void);
static const void *guest_image_header(uint32_t);
static const void *guest_section_data(const void *,const char *,const char *,unsigned long *);
static Word guest_native_address(Word);
#if I2A_DEVELOPMENT
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "I2AGuest", __VA_ARGS__)
#define LOG_LIMITED(limit, ...) do { static unsigned i2a_log_count; unsigned i2a_log_limit=(limit); \
    if (__atomic_load_n(&i2a_log_count,__ATOMIC_RELAXED)<i2a_log_limit && \
        __atomic_fetch_add(&i2a_log_count,1,__ATOMIC_RELAXED)<i2a_log_limit) LOG(__VA_ARGS__); } while (0)
#else
#define LOG(...) do { if (0) __android_log_print(ANDROID_LOG_INFO, "I2AGuest", __VA_ARGS__); } while (0)
#define LOG_LIMITED(limit, ...) do { if (0) { (void)(limit); LOG(__VA_ARGS__); } } while (0)
#endif
#define COUNT(array) (sizeof(array)/sizeof((array)[0]))

static _Thread_local jmp_buf boundary;
struct GuestCallBoundary { jmp_buf environment; struct GuestCallBoundary *previous; };
static _Thread_local struct GuestCallBoundary *active_guest_call_boundary;
static _Thread_local char status[512], stage[128];
static Word symbol_objects[COUNT(symbols)][4];
static Word import_addresses[COUNT(symbols)];
static const char *const extra_class_names[]={
    "_OBJC_CLASS_$_NSOrderedSet", "_OBJC_CLASS_$_NSMutableOrderedSet",
    "_OBJC_CLASS_$_NSMutableCharacterSet", "_OBJC_CLASS_$_NSURLResponse", "_OBJC_CLASS_$_NSHTTPURLResponse",
    "_OBJC_CLASS_$_NSURLProtectionSpace", "_OBJC_CLASS_$_NSURLAuthenticationChallenge"
    , "_OBJC_CLASS_$_CALayer", "_OBJC_CLASS_$__UILayoutGuide", "_OBJC_CLASS_$_NSTextCheckingResult", "_OBJC_CLASS_$_NSNotification"
    , "_OBJC_CLASS_$_AVAudioSessionRouteDescription", "_OBJC_CLASS_$_AVAudioSessionPortDescription"
    , "_OBJC_CLASS_$_CTCarrier"
    , "_OBJC_CLASS_$_NSURLSessionTask", "_OBJC_CLASS_$_NSURLSessionDataTask", "_OBJC_CLASS_$_NSURLSessionDownloadTask", "_OBJC_CLASS_$_NSURLSessionUploadTask", "_OBJC_CLASS_$_I2ASessionTransport"
    , "_OBJC_CLASS_$_NSEnumerator", "_OBJC_CLASS_$_NSDirectoryEnumerator"
    , "_OBJC_CLASS_$_I2AMetalDevice", "_OBJC_CLASS_$_I2AMetalCommandQueue", "_OBJC_CLASS_$_I2AMetalCommandBuffer"
    , "_OBJC_CLASS_$_CADisplayLink"
    , "_OBJC_CLASS_$_I2AMetalDescriptorArray"
    , "_OBJC_CLASS_$_I2AMetalBuffer", "_OBJC_CLASS_$_I2AMetalSamplerState", "_OBJC_CLASS_$_I2AMetalDepthStencilState", "_OBJC_CLASS_$_I2AMetalTexture"
    , "_OBJC_CLASS_$_I2AMetalLibrary", "_OBJC_CLASS_$_I2AMetalFunction", "_OBJC_CLASS_$_I2AMetalRenderPipelineState"
    , "_OBJC_CLASS_$_I2AMetalComputePipelineState"
    , "_OBJC_CLASS_$_I2AMetalBlitCommandEncoder"
    , "_OBJC_CLASS_$_MTLArgument", "_OBJC_CLASS_$_MTLStructType", "_OBJC_CLASS_$_MTLStructMember", "_OBJC_CLASS_$_MTLVertexAttribute", "_OBJC_CLASS_$_MTLRenderPipelineReflection"
    , "_OBJC_CLASS_$_MTLTextureDescriptor", "_OBJC_CLASS_$_MTLSamplerDescriptor", "_OBJC_CLASS_$_MTLDepthStencilDescriptor", "_OBJC_CLASS_$_MTLStencilDescriptor"
    , "_OBJC_CLASS_$_MTLRenderPipelineDescriptor", "_OBJC_CLASS_$_MTLRenderPipelineColorAttachmentDescriptor", "_OBJC_CLASS_$_MTLRenderPassDescriptor"
    , "_OBJC_CLASS_$_MTLRenderPassColorAttachmentDescriptor", "_OBJC_CLASS_$_MTLRenderPassDepthAttachmentDescriptor", "_OBJC_CLASS_$_MTLRenderPassStencilAttachmentDescriptor"
    , "_OBJC_CLASS_$_MTLVertexDescriptor", "_OBJC_CLASS_$_MTLVertexAttributeDescriptor", "_OBJC_CLASS_$_MTLVertexBufferLayoutDescriptor", "_OBJC_CLASS_$_MTLCompileOptions"
    , "_OBJC_CLASS_$_I2AMetalRenderCommandEncoder"
    , "_OBJC_CLASS_$_I2AMetalDrawable"
    , "_OBJC_CLASS_$_NSIndexSet", "_OBJC_CLASS_$_NSMutableIndexSet"
    , "_OBJC_CLASS_$_UINavigationBar", "_OBJC_CLASS_$_UIPopoverPresentationController", "_OBJC_CLASS_$_SKProductsResponse"
    , "_OBJC_CLASS_$_NSParagraphStyle"
    , "_OBJC_CLASS_$_CMDeviceMotion", "_OBJC_CLASS_$_CMAttitude", "_OBJC_CLASS_$_MotionSourceDelivery"
    , "_OBJC_CLASS_$_UITouch", "_OBJC_CLASS_$_UIEvent"
    , "_OBJC_CLASS_$_UIResponder", "_OBJC_CLASS_$_UIControl", "_OBJC_CLASS_$_UINavigationItem"
    , "_OBJC_CLASS_$_UIGestureRecognizer", "_OBJC_CLASS_$_NSFormatter"
    , "_OBJC_CLASS_$_ASIdentifierManager"
    , "_OBJC_CLASS_$_SKPayment", "_OBJC_CLASS_$_SKMutablePayment", "_OBJC_CLASS_$_SKPaymentTransaction"
    , "_OBJC_CLASS_$_NSMethodSignature", "_OBJC_CLASS_$_NSInvocation"
    , "_OBJC_CLASS_$_UILocalNotification", "_OBJC_CLASS_$_UNUserNotificationCenter", "_OBJC_CLASS_$_UNNotificationSettings"
    , "_OBJC_CLASS_$_UNNotificationContent", "_OBJC_CLASS_$_UNMutableNotificationContent", "_OBJC_CLASS_$_UNNotificationSound"
    , "_OBJC_CLASS_$_UNNotificationRequest", "_OBJC_CLASS_$_UNNotificationTrigger", "_OBJC_CLASS_$_UNTimeIntervalNotificationTrigger"
    , "_OBJC_CLASS_$_UNNotification", "_OBJC_CLASS_$_UNNotificationResponse"
    , "_OBJC_CLASS_$_NSNetService"
    , "_OBJC_CLASS_$_UITapGestureRecognizer", "_OBJC_CLASS_$_UIPanGestureRecognizer"
    , "_OBJC_CLASS_$_UIWebView", "_OBJC_CLASS_$_WKWebView", "_OBJC_CLASS_$_WKWebViewConfiguration", "_OBJC_CLASS_$_WKPreferences"
    , "_OBJC_CLASS_$_WKUserContentController", "_OBJC_CLASS_$_WKUserScript", "_OBJC_CLASS_$_WKNavigation", "_OBJC_CLASS_$_WKNavigationAction", "_OBJC_CLASS_$_WKFrameInfo"
    , "_OBJC_CLASS_$_UIScrollView", "_OBJC_CLASS_$_UITextView"
    , "_OBJC_CLASS_$_CLLocation", "_OBJC_CLASS_$_CLLocationManager", "_OBJC_CLASS_$_CLLocationSourceInformation", "_OBJC_CLASS_$_CLGeocoder", "_OBJC_CLASS_$_CLPlacemark"
    , "_OBJC_CLASS_$_CNPostalAddress", "_OBJC_CLASS_$_CNMutablePostalAddress"
    , "_OBJC_CLASS_$_UIImagePickerController", "_OBJC_CLASS_$_PHAsset", "_OBJC_CLASS_$_PHFetchResult", "_OBJC_CLASS_$_PHFetchOptions"
    , "_OBJC_CLASS_$_PHImageManager", "_OBJC_CLASS_$_PHImageRequestOptions", "_OBJC_CLASS_$_PHPhotoLibrary"
};
static Word extra_class_objects[COUNT(extra_class_names)][4];
static void initialize_guest_class(Word cls);
static void construct_guest_ivars(Word object,Word cls);
static void ui_application_start(Word,Word);
static Word controller_view(Word);
static Word window_controller(Word);
static void run_interface_preflight(void);
static Method views_method(Word,const char *,const char *,int);
static Method foundation_method(Word,const char *,const char *,int);
static Method framework_implementation(Word,const char *,const char *,int);
static Word object_superclass(Word);
static Word aslr, global_guard;
static int started;
static int application_ready;
#if I2A_DEVELOPMENT
static _Thread_local const char *sampling_selector;
static _Thread_local Word sampling_class;
#endif
static unsigned method_cache_epoch;
#if I2A_DEVELOPMENT
static int render_diagnostics_requested,network_diagnostics_requested,interface_diagnostics_requested,business_labels_requested;
#else
#define render_diagnostics_requested 0
#define network_diagnostics_requested 0
#define interface_diagnostics_requested 0
#define business_labels_requested 0
#endif
static char tls_diagnostics_directory[PATH_MAX];
#if I2A_DEVELOPMENT
JNIEXPORT void JNICALL HOST_JNI(HostActivity_configureNetworkDiagnostics)(JNIEnv *env,jobject object,jboolean enabled) { network_diagnostics_requested=enabled; }
JNIEXPORT void JNICALL HOST_JNI(HostActivity_configureInterfaceDiagnostics)(JNIEnv *env,jobject object,jboolean enabled) { interface_diagnostics_requested=enabled; }
JNIEXPORT void JNICALL HOST_JNI(HostActivity_configureLabelDiagnostics)(JNIEnv *env,jobject object,jboolean enabled) { business_labels_requested=enabled; }
#endif
static _Thread_local uint64_t render_cpu_work[14],render_cpu_calls[14];
static uint64_t render_diagnostic_clock(void) {
    if (!render_diagnostics_requested) return 0;
    struct timespec time; clock_gettime(CLOCK_MONOTONIC,&time);
    return (uint64_t)time.tv_sec*1000000000ULL+(uint64_t)time.tv_nsec;
}
static void render_diagnostic_cost(unsigned kind,uint64_t begin) {
    if (begin) { render_cpu_work[kind]+=render_diagnostic_clock()-begin; ++render_cpu_calls[kind]; }
}
static _Thread_local int may_unwind,fatal_guest_thread;
static JavaVM *java_vm;
static jobject activity;
static jmethodID preference_method, bundle_file_method, digest_method;
static jmethodID certificate_method, trust_method;
static jmethodID system_text_method;
static jmethodID schedule_operation_method;
static jmethodID bundle_exists_method;
static Word system_text(const char *);
static Word certificate_tag, trust_tag,reachability_tag,security_key_tag,secure_transport_tag;
static void secure_transport_release(Word);
static void trust_release(Word);
static char *files_directory;
static void record_failure(const char *,const char *);

static __attribute__((noreturn)) void stop(const char *kind, const char *name) {
    snprintf(status, sizeof(status), "%s\n%s %s", stage, kind, name);
    record_failure(kind,name);
    __android_log_print(ANDROID_LOG_ERROR,"I2AGuest","STOP %s | %s %s",stage,kind,name);
    /* Guest jobs may borrow this thread's stack until their original sync. */
    if(fatal_guest_thread)abort();
    if (active_guest_call_boundary) longjmp(active_guest_call_boundary->environment,1);
    if (may_unwind) longjmp(boundary, 1);
    abort();
}
static __attribute__((noreturn)) void propagate_guest_failure(void) {
    if(fatal_guest_thread)abort();
    if(active_guest_call_boundary)longjmp(active_guest_call_boundary->environment,1);
    if(may_unwind)longjmp(boundary,1);
    abort();
}

__attribute__((visibility("hidden"), noreturn)) void missing_call(unsigned index) {
    Word caller=(Word)__builtin_return_address(0);
    char detail[240];
    snprintf(detail,sizeof(detail),"%s caller=0x%lx",symbols[index],guest_native_address(caller ? caller-4 : 0));
    stop("Unimplemented import",detail);
}

static Word read_word(Word address) {
    Word result;
    memcpy(&result, (void *)address, sizeof(result));
    return result;
}

#include "ownership/lifetime.inc"
#include "objc/dynamic_state.inc"
#include "objc/layout_state.inc"

static const struct I2AImageSegment *image_segment_for(Word address,size_t bytes) {
    if (address<aslr || address-aslr>=IMAGE_END) return NULL;
    Word offset=address-aslr;
    size_t first=0,last=COUNT(guest_segments);
    while(first<last){size_t middle=first+(last-first)/2;if(guest_segments[middle].offset<=offset)first=middle+1;else last=middle;}
    if(!first)return NULL;
    const struct I2AImageSegment *segment=&guest_segments[first-1];
    if(offset-segment->offset<segment->memory_size && bytes<=segment->memory_size-(offset-segment->offset))return segment;
    return NULL;
}
static int in_image(Word address) {
    const struct I2AImageSegment *segment=image_segment_for(address,1);
    return segment && (segment->initial&PROT_READ);
}
#include "../diagnostics/cpu_sampling.inc"

static const char *host_symbol(Word object) {
    Word offset = object - (Word)symbol_objects;
    if (offset < sizeof(symbol_objects) && offset % sizeof(symbol_objects[0]) == 0)
        return symbols[offset / sizeof(symbol_objects[0])];
    offset=object-(Word)extra_class_objects;
    if (offset<sizeof(extra_class_objects) && offset%sizeof(extra_class_objects[0])==0)
        return extra_class_names[offset/sizeof(extra_class_objects[0])];
    return NULL;
}

static const char *class_name(Word cls) {
    const char *external = host_symbol(cls);
    if (external) return external;
    struct DynamicClass *dynamic=dynamic_class(cls);
    if (dynamic) return dynamic->name;
    if (!in_image(cls)) return NULL;
    Word ro = read_word(cls+32) & ~(Word)7;
    if (!class_ro_valid(cls,ro)) return NULL;
    Word name = read_word(ro+24);
    return in_image(name) ? (const char *)name : NULL;
}

#include "objc/selectors.inc"
#include "objc/method_lists.inc"

static Method method_list(Word list, const char *selector) {
    Word entry = find_method_record(list,selector);
    return entry ? (Method)read_word(entry+16) : NULL;
}

static int guest_class_object(Word object) {
    if (dynamic_class(object)) return 1;
    if(class_layouts_ready)return class_layout(object)!=NULL;
    if (!in_image(object)) return 0;
    const Word *classes=(const Word *)(aslr+CLASSES_OFFSET);
    for (unsigned index=0;index<CLASSES_COUNT;++index)
        if (classes[index]==object) return 1;
    return 0;
}

static struct MethodRecord *host_instance_record(const char *,const char *);
static struct MethodRecord *category_method_record(Word cls,const char *selector) {
    if(class_layouts_ready){
        unsigned index=category_buckets[class_pointer_bucket(cls,COUNT(category_buckets))];
        for(;index;index=category_methods[index].next){
            struct CategoryMethods *entry=&category_methods[index];
            if(entry->cls==cls){Word found=find_method_record(read_word(entry->list),selector);if(found)return (void *)found;}
        }
        return NULL;
    }
    const Word *categories=(const Word *)(aslr+CATEGORIES_OFFSET);
    for(unsigned pos=CATEGORIES_COUNT;pos>0;--pos){
        Word category=categories[pos-1],target=read_word(category+8);
        Word list=target==cls ? read_word(category+16) : target && read_word(target)==cls ? read_word(category+24) : 0;
        Word found=find_method_record(list,selector);if(found)return (void *)found;
    }
    return NULL;
}
static struct MethodRecord *owned_method_record(Word cls,const char *selector) {
    struct MethodRecord *record=added_method(cls,selector);
    if(!record)record=category_method_record(cls,selector);
    if(!record && in_image(cls))record=(void *)find_method_record(read_word((read_word(cls+32)&~(Word)7)+32),selector);
    if(!record && !dynamic_class(cls))record=host_instance_record(host_symbol(cls),selector);
    return record;
}
static Method lookup(Word cls, const char *selector) {
    for (unsigned depth=0; depth<64 && cls; ++depth) {
        struct MethodRecord *owned=owned_method_record(cls,selector);
        if(owned)return __atomic_load_n(&owned->implementation,__ATOMIC_ACQUIRE);
        if (dynamic_class(cls)) { cls=read_word(cls+8); continue; }
        if (!in_image(cls)) {
            const char *name=host_symbol(cls);
            if (!name || !strncmp(name,"_OBJC_METACLASS_",16) || !strcmp(name,"_OBJC_CLASS_$_NSObject")) break;
            cls=host_superclass(cls);
            continue;
        }
        cls = read_word(cls+8);
    }
    return NULL;
}

static Method lookup_external_class_method(Word cls,const char *selector) {
    for (unsigned depth=0;cls && depth<64;++depth) {
        struct MethodRecord *added=added_method(cls,selector);
        if (added) return added->implementation;
        const Word *categories=(void *)(aslr+CATEGORIES_OFFSET);
        for (unsigned i=CATEGORIES_COUNT;i>0;--i) {
            Word category=categories[i-1];
            if (read_word(category+8)==cls) {
                Method method=method_list(read_word(category+24),selector);
                if (method) return method;
            }
        }
        const char *name=host_symbol(cls);
        if (!name || !strcmp(name,"_OBJC_CLASS_$_NSObject")) break;
        cls=host_superclass(cls);
    }
    return NULL;
}

static const char *external_base(Word cls) {
    for (unsigned depth=0;depth<64 && cls;++depth) {
        const char *name=host_symbol(cls);
        if (name) return name;
        if (!in_image(cls) && !dynamic_class(cls)) return NULL;
        cls=read_word(cls+8);
    }
    return NULL;
}

static Word identity(Word value) { return value; }
static void noop(void) {}

static Word controller_navigation(Word);
static Word controller_storyboard(Word);
static Word controller_default_status_style(void) { return 0; }
static Word controller_default_status_hidden(void) { return 0; }
static Word interface_orientation(void);
static struct MethodRecord view_methods[] = {
    {"preferredStatusBarStyle", "q@:", (Method)controller_default_status_style},
    {"prefersStatusBarHidden", "B@:", (Method)controller_default_status_hidden},
    {"navigationController", "@@:", (Method)controller_navigation},
    {"storyboard", "@@:", (Method)controller_storyboard},
    {"interfaceOrientation", "q@:", (Method)interface_orientation},
    {"viewDidAppear:", "v@:B", (Method)noop}, {"viewDidDisappear:", "v@:B", (Method)noop},
    {"viewDidLoad", "v@:", (Method)noop}, {"viewWillAppear:", "v@:B", (Method)noop},
    {"viewWillDisappear:", "v@:B", (Method)noop}, {"viewDidLayoutSubviews", "v@:", (Method)noop}
};
static void application_set_delegate(Word,const char *,Word);
static void user_notification_set_delegate(Word,const char *,Word);
static Word user_notification_get_delegate(Word,const char *);
static struct MethodRecord application_methods[]={
    {"setDelegate:","v@:@",(Method)application_set_delegate}
};
static struct MethodRecord user_notification_methods[]={
    {"setDelegate:","v@:@",(Method)user_notification_set_delegate},
    {"delegate","@@:",(Method)user_notification_get_delegate}
};
static struct MethodRecord *host_instance_record(const char *name,const char *selector) {
    if (!name) return NULL;
    struct MethodRecord *records=NULL; unsigned count=0;
    if (!strcmp(name,"_OBJC_CLASS_$_UIViewController")) { records=view_methods; count=COUNT(view_methods); }
    else if (!strcmp(name,"_OBJC_CLASS_$_UIApplication")) { records=application_methods; count=COUNT(application_methods); }
    else if (!strcmp(name,"_OBJC_CLASS_$_UNUserNotificationCenter")) { records=user_notification_methods; count=COUNT(user_notification_methods); }
    for (unsigned i=0;i<count;++i) if (!strcmp(records[i].name,selector)) return &records[i];
    return NULL;
}

static Word instance_method(Word cls,const char *selector) {
    const Word *categories=(const Word *)(aslr+CATEGORIES_OFFSET);
    while (cls) {
        struct MethodRecord *added=added_method(cls,selector);
        if (added) return (Word)added;
        if (dynamic_class(cls)) { cls=read_word(cls+8); continue; }
        for (unsigned pos=CATEGORIES_COUNT;pos>0;--pos) {
            Word category=categories[pos-1];
            if (read_word(category+8)==cls) {
                Word method=find_method_record(read_word(category+16),selector);
                if (method) return method;
            }
        }
        const char *external=host_symbol(cls);
        if (external) {
            return (Word)host_instance_record(external,selector);
        }
        if (!in_image(cls)) return 0;
        Word ro=read_word(cls+32)&~(Word)7;
        Word method=find_method_record(read_word(ro+32),selector);
        if (method) return method;
        cls=read_word(cls+8);
    }
    return 0;
}

static Method get_imp(struct MethodRecord *record) { return record ? record->implementation : NULL; }

static Method set_imp(struct MethodRecord *record,Method imp) {
    if (!record || !imp) return NULL;
    Method previous=__atomic_exchange_n(&record->implementation,imp,__ATOMIC_ACQ_REL);
    __atomic_add_fetch(&method_cache_epoch,1,__ATOMIC_RELEASE);
    return previous;
}

struct HostObject {
    Word isa;
    Word content[4];
};

struct Observer {
    struct HostObject token;
    Word center,receiver,selector,name,object,queue,block,callback;
    int active;
    struct Observer *next;
};

static struct Observer *observers;
static pthread_mutex_t observers_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct HostObject center, bundle;
static Word bundle_dictionary;
static struct HostObject process_info, arguments_array, defaults;
static struct HostObject main_thread, application, main_window;
static Word application_object=(Word)&application;
static struct HostObject file_manager;
static struct HostObject bool_numbers[2];
static struct HostObject null_value;
static Word root_controller, app_delegate;
static Word key_window;
static _Thread_local Word active_pool;
struct MetalAutoreleasePool {
    Word parent,*objects;
    size_t count,capacity;
    Word *core_objects;
    size_t core_count,core_capacity;
    struct MetalAutoreleasePool *next;
};
static _Thread_local struct MetalAutoreleasePool *metal_pool_tokens;
static _Thread_local unsigned metal_pool_token_count;
static _Thread_local Word metal_return_object,metal_return_caller;

static Word metal_autorelease_return(Word object,Word caller) {
    const struct I2AImageSegment *segment=object ? image_segment_for(caller,8) : NULL;
    if (segment && (segment->initial&PROT_READ) && metal_object_lifetime(object)) {
        uint32_t instructions[2]; memcpy(instructions,(void *)caller,sizeof(instructions));
        if (instructions[0]==0xaa1d03fdU && (instructions[1]&0xfc000000U)==0x94000000U) {
            metal_return_object=object; metal_return_caller=caller+8; return object;
        }
    }
    return metal_object_autorelease(object);
}
static __attribute__((noinline)) Word metal_autorelease_return_value(Word object) {
    return metal_autorelease_return(object,(Word)__builtin_return_address(0));
}
static __attribute__((noinline)) Word metal_retain_autorelease_return_value(Word object) {
    return metal_autorelease_return(metal_object_retain(object),(Word)__builtin_return_address(0));
}

static Word pool_push(void) {
    struct MetalAutoreleasePool *token=metal_pool_tokens;
    if (token) { metal_pool_tokens=token->next; --metal_pool_token_count; }
    else token=calloc(1,sizeof(*token));
    if (!token) stop("Allocation failed","autorelease pool");
    token->parent=active_pool; token->next=NULL; token->count=token->core_count=0;
    active_pool=(Word)token;
    return active_pool;
}
static Word metal_object_autorelease(Word object) {
    if (!metal_object_lifetime(object) || !active_pool) return object;
    struct MetalAutoreleasePool *pool=(void *)active_pool;
    if (pool->count==pool->capacity) {
        size_t capacity=pool->capacity ? pool->capacity*2 : 32;
        if (capacity<pool->capacity || capacity>SIZE_MAX/sizeof(Word)) stop("Metal autorelease pool too large","autorelease");
        Word *objects=realloc(pool->objects,capacity*sizeof(*objects));
        if (!objects) stop("Allocation failed","Metal autorelease entries");
        pool->objects=objects; pool->capacity=capacity;
    }
    pool->objects[pool->count++]=object;
    return object;
}
static __attribute__((noinline)) Word metal_claim_autoreleased(Word object) {
    if (object && metal_return_object==object && metal_return_caller==(Word)__builtin_return_address(0)) {
        metal_return_object=metal_return_caller=0; return object;
    }
    if (active_pool) {
        struct MetalAutoreleasePool *pool=(void *)active_pool;
        if (pool->count && pool->objects[pool->count-1]==object) { --pool->count; return object; }
    }
    return metal_object_retain(object);
}
static __attribute__((noinline)) Word metal_unsafe_claim_autoreleased(Word object) {
    if (object && metal_return_object==object && metal_return_caller==(Word)__builtin_return_address(0)) {
        metal_return_object=metal_return_caller=0; metal_object_release(object);
    }
    return object;
}
static Word core_autorelease(Word object) {
    if(!object)stop("Null object","CFAutorelease");if(!active_pool)return object;
    struct MetalAutoreleasePool *pool=(void *)active_pool;
    if(pool->core_count==pool->core_capacity){size_t capacity=pool->core_capacity ? pool->core_capacity*2 : 16;if(capacity<pool->core_capacity || capacity>SIZE_MAX/sizeof(Word))stop("Autorelease pool too large","CFAutorelease");Word *values=realloc(pool->core_objects,capacity*sizeof(*values));if(!values)stop("Allocation failed","CFAutorelease");pool->core_objects=values;pool->core_capacity=capacity;}
    pool->core_objects[pool->core_count++]=object;return object;
}
static Word metal_retain_autorelease(Word object) { return metal_object_autorelease(metal_object_retain(object)); }
static void pool_pop(Word token) {
    if (!token || token!=active_pool) stop("Invalid token","autorelease pool");
    struct MetalAutoreleasePool *pool=(void *)token;
    active_pool=pool->parent;
    while(pool->core_count)cf_release(pool->core_objects[--pool->core_count]);
    while (pool->count) metal_object_release(pool->objects[--pool->count]);
    /* Warmed-up frame/nested pools reuse their entry arrays. Bound the per-
     * thread cache; an exceptional loading peak must not stay cached forever. */
    if (metal_pool_token_count<8 && pool->capacity<=4096 && pool->core_capacity<=4096) {
        pool->next=metal_pool_tokens; metal_pool_tokens=pool; ++metal_pool_token_count;
    } else { free(pool->objects); free(pool->core_objects); free(pool); }
}

static Word get_main_thread(Word cls) {
    main_thread.isa=cls;
    return (Word)&main_thread;
}

static void set_thread_name(Word thread,const char *selector,Word string) {
    if (!in_image(string) || read_word(string+8)!=0x7c8) stop("Unsupported thread name",selector);
    main_thread.content[0]=string;
    pthread_setname_np(pthread_self(),(const char *)read_word(string+16));
}

static Word standard_defaults(Word cls) {
    defaults.isa=cls;
    return (Word)&defaults;
}

static const char *string_bytes(Word string,const char *selector);

static int access_preference(Word key, int operation) {
    const char *key_text=string_bytes(key,"NSUserDefaults");
    JNIEnv *env=NULL;
    if ((*java_vm)->GetEnv(java_vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK) stop("Preferences", "detached thread");
    jstring name=(*env)->NewStringUTF(env,key_text);
    jint result=(*env)->CallIntMethod(env,activity,preference_method,name,operation);
    (*env)->DeleteLocalRef(env,name);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        stop("Preferences", "Android storage error");
    }
    if (operation>=0 && !result) stop("Preferences", "write failed");
    return result;
}

static Word read_bool(Word self,const char *selector,Word key) { return access_preference(key,-1); }
static void remove_default(Word self,const char *selector,Word key) { access_preference(key,2); }
static void set_bool(Word self,const char *selector,Word value,Word key) { access_preference(key,(unsigned char)value!=0); }

struct AslClient { char *facility; int mask, output_fd; };

static struct AslClient *asl_open_bridge(const char *ident, const char *facility, int options) {
    struct AslClient *client=calloc(1,sizeof(*client));
    if (client) {
        client->facility=strdup(facility ? facility : "I2AGuest");
        client->mask=0xff;
        client->output_fd=-1;
    }
    return client;
}

static int asl_filter_bridge(struct AslClient *client,int mask) {
    if (!client) return 0;
    int old=client->mask;
    client->mask=mask;
    return old;
}

static int asl_add_fd(struct AslClient *client,int fd) {
    if (!client) return -1;
    client->output_fd=fd;
    return 0;
}

__attribute__((used,noinline)) static int asl_emit(struct AslClient *client,Word message,unsigned level,const char *format,const char *const *args) {
    if (!client || level>7) return -1;
    if (!I2A_DEVELOPMENT) return 0;
    if (!(client->mask & (1U<<level))) return 0;
    const char *text=format;
    if (!strcmp(format,"%s")) text=args[0];
    else if (strchr(format,'%')) stop("Unsupported ASL format",format);
    if (!text) text="";
    __android_log_write(level<=3?ANDROID_LOG_ERROR:level==4?ANDROID_LOG_WARN:ANDROID_LOG_INFO,client->facility,text);
    if (client->output_fd>=0) dprintf(client->output_fd,"%s\n",text);
    return 0;
}

__attribute__((naked)) static void asl_darwin_log(void) { __asm__ volatile("mov x4,sp\n b asl_emit"); }

struct HostClassEntry { const char *name; Word object; };
static struct HostClassEntry host_class_index[I2A_HOST_SYMBOL_INDEX_CAPACITY];
static pthread_once_t host_class_once=PTHREAD_ONCE_INIT;
static unsigned host_class_index_ready;
static unsigned host_class_hash(const char *name) {
    unsigned hash=2166136261u;
    for (const unsigned char *p=(const unsigned char *)name;*p;++p) hash=(hash^*p)*16777619u;
    return hash & (COUNT(host_class_index)-1);
}
static Word host_class_find(const char *name) {
    unsigned slot=host_class_hash(name);
    while (host_class_index[slot].name) {
        if (!strcmp(host_class_index[slot].name,name)) return host_class_index[slot].object;
        slot=(slot+1)&(COUNT(host_class_index)-1);
    }
    return 0;
}
static void host_class_index_init(void) {
    static const struct { const char *name,*parent; } parents[]={
        {"UIResponder","NSObject"},{"UIView","UIResponder"},{"UIViewController","UIResponder"},{"UIApplication","UIResponder"},
        {"UIControl","UIView"},{"UIButton","UIControl"},{"UITextField","UIControl"},{"UISwitch","UIControl"},{"UISlider","UIControl"},{"UISegmentedControl","UIControl"},
        {"UILabel","UIView"},{"UIImageView","UIView"},{"UIWindow","UIView"},{"_UILayoutGuide","UIView"},{"UIScrollView","UIView"},{"UITableView","UIScrollView"},{"UITextView","UIScrollView"},{"UICollectionView","UIScrollView"},
        {"UIActivityIndicatorView","UIView"},{"UIProgressView","UIView"},{"UINavigationBar","UIView"},{"UIToolbar","UIView"},{"UISearchBar","UIView"},{"UITableViewCell","UIView"},{"UIVisualEffectView","UIView"},{"UIAlertView","UIView"},{"UIActionSheet","UIView"},
        {"UINavigationController","UIViewController"},{"UIImagePickerController","UINavigationController"},{"UITableViewController","UIViewController"},{"UICollectionViewController","UIViewController"},
        {"UIActivityViewController","UIViewController"},{"UIAlertController","UIViewController"},{"UISearchController","UIViewController"},{"UISplitViewController","UIViewController"},{"MTKView","UIView"},{"GLKView","UIView"},{"GLKViewController","UIViewController"},
        {"UIBlurEffect","UIVisualEffect"},{"UIPanGestureRecognizer","UIGestureRecognizer"},{"UITapGestureRecognizer","UIGestureRecognizer"},
        {"UNMutableNotificationContent","UNNotificationContent"},{"UNTimeIntervalNotificationTrigger","UNNotificationTrigger"},
        {"UIWebView","UIView"},{"WKWebView","UIView"},
        {"CAMetalLayer","CALayer"},{"CAGradientLayer","CALayer"},{"CAShapeLayer","CALayer"},{"CAEAGLLayer","CALayer"},
        {"NSDecimalNumber","NSNumber"},{"NSNumber","NSValue"},{"NSDirectoryEnumerator","NSEnumerator"},{"NSInvocationOperation","NSOperation"},{"NSBlockOperation","NSOperation"},{"SKMutablePayment","SKPayment"},
        {"NSDateFormatter","NSFormatter"},{"NSNumberFormatter","NSFormatter"},{"NSHTTPURLResponse","NSURLResponse"},
        {"CNMutablePostalAddress","CNPostalAddress"},
        {"NSURLSessionDataTask","NSURLSessionTask"},{"NSURLSessionDownloadTask","NSURLSessionTask"},{"NSURLSessionUploadTask","NSURLSessionDataTask"}
    };
    _Static_assert(COUNT(symbols)+COUNT(extra_class_names)<COUNT(host_class_index),"Host symbol index capacity");
    for (unsigned i=0;i<COUNT(symbols)+COUNT(extra_class_names);++i) {
        const char *name=i<COUNT(symbols) ? symbols[i] : extra_class_names[i-COUNT(symbols)];
        Word object=i<COUNT(symbols) ? (Word)symbol_objects[i] : (Word)extra_class_objects[i-COUNT(symbols)];
        unsigned slot=host_class_hash(name);
        while (host_class_index[slot].name && strcmp(host_class_index[slot].name,name)) slot=(slot+1)&(COUNT(host_class_index)-1);
         
        if (!host_class_index[slot].name) host_class_index[slot]=(struct HostClassEntry){name,object};
    }
    for (unsigned i=0;i<COUNT(host_class_index);++i) {
        struct HostClassEntry *entry=host_class_index+i;
        if (!entry->name || strncmp(entry->name,"_OBJC_CLASS_$_",14)) continue;
        const char *name=entry->name+14,*parent=!strcmp(name,"NSObject") ? NULL : "NSObject";
        size_t size=external_instance_size(entry->object);
        ((Word *)entry->object)[2]=size<sizeof(struct HostObject) ? sizeof(struct HostObject) : size;
        char mutable_parent[160],symbol[180];
        if (!strncmp(name,"NSMutable",9)) { snprintf(mutable_parent,sizeof(mutable_parent),"NS%s",name+9); parent=mutable_parent; }
        for (unsigned j=0;j<COUNT(parents);++j) if (!strcmp(name,parents[j].name)) { parent=parents[j].parent; break; }
        if (parent) { snprintf(symbol,sizeof(symbol),"_OBJC_CLASS_$_%s",parent); ((Word *)entry->object)[1]=host_class_find(symbol); }
    }
    __atomic_store_n(&host_class_index_ready,1,__ATOMIC_RELEASE);
}
static Word external_class(const char *name) {
     
    if (!__atomic_load_n(&host_class_index_ready,__ATOMIC_ACQUIRE)) pthread_once(&host_class_once,host_class_index_init);
    return host_class_find(name);
}
static Word host_superclass(Word cls) {
    if (!__atomic_load_n(&host_class_index_ready,__ATOMIC_ACQUIRE)) pthread_once(&host_class_once,host_class_index_init);
    const char *name=host_symbol(cls);
    return name && !strncmp(name,"_OBJC_CLASS_$_",14) ? read_word(cls+8) : 0;
}
static size_t host_allocation_size(Word cls) {
    if (!__atomic_load_n(&host_class_index_ready,__ATOMIC_ACQUIRE)) pthread_once(&host_class_once,host_class_index_init);
    return read_word(cls+16);
}

static Word process_singleton(Word cls) {
    process_info.isa=cls;
    return (Word)&process_info;
}

static Word string_from_class(Word cls) {
    const char *name=class_name(cls);
    if (!name) return 0;
    if (!strncmp(name,"_OBJC_CLASS_$_",14)) name+=14;
    struct HostObject *result=calloc(1,sizeof(*result));
    if (!result) stop("Allocation failed","NSStringFromClass");
    result->isa=external_class("_OBJC_CLASS_$_NSString");
    result->content[0]=0x7c8;
    result->content[1]=(Word)name;
    result->content[2]=strlen(name);
    return (Word)result;
}

static Word process_arguments(void) {
    arguments_array.isa=external_class("_OBJC_CLASS_$_NSArray");
    return (Word)&arguments_array;
}

static Word argv_contains(Word self,const char *selector,Word string) {
    if (!in_image(string) || read_word(string+8)!=0x7c8) stop("Unsupported argument string",selector);
    return !strcmp((const char *)read_word(string+16),GUEST_EXECUTABLE);
}

static Word class_from_string(Word string) {
    if (!string) return 0;
    return class_from_name(string_bytes(string,"NSClassFromString"));
}

static Word guest_object(Word cls) {
    struct DynamicClass *dynamic=dynamic_class(cls);
    if (!in_image(cls) && !dynamic) stop("Invalid Objective-C allocation class",class_name(cls) ? class_name(cls) : "Unregistered class");
    initialize_guest_class(cls);
    uint32_t bytes;
    if (dynamic) bytes=(uint32_t)dynamic->size;
    else { Word ro=read_word(cls+32)&~(Word)7; memcpy(&bytes,(void *)(ro+8),sizeof(bytes)); }
    if (bytes<sizeof(Word)) stop("Invalid class size",class_name(cls));
    Word *object=calloc(1,bytes);
    if (!object) stop("Allocation failed",class_name(cls));
    *object=cls;
    construct_guest_ivars((Word)object,cls);
    return (Word)object;
}

static Word application_main(int argc,char **argv,Word principal,Word delegate_name) {
    ui_application_start(principal,delegate_name);
    application_ready=1;
    run_interface_preflight();
    LOG("Original UIKit startup complete; Android Looper owns continuation");
     
    longjmp(boundary,2);
}

static Word shared_app(void) { if(!application.isa)application.isa=external_class("_OBJC_CLASS_$_UIApplication");return application_object; }
static Word get_delegate(void) { return app_delegate; }
static Word get_window(void) { return key_window; }
#if I2A_HAS_EXTENSION
static Word get_root(void) { return key_window ? window_controller(key_window) : 0; }
#endif
static void set_delegate(Word object,const char *selector,Word delegate) { app_delegate=delegate; }
static void store_strong(Word *slot,Word value) {
    Word previous=*slot;
    if (previous==value) return;
    metal_object_retain(value); *slot=value; metal_object_release(previous);
}

/* Only ObjC atomic-property access uses these short, sharded locks. Render
 * bindings and resource retain/release never take a property lock. Protect the
 * getter's retain against a concurrent setter dropping the last owner. */
static pthread_mutex_t metal_property_locks[64]={[0 ... 63]=PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t *metal_property_lock(Word *slot) { return &metal_property_locks[((Word)slot>>3)&63]; }
static Word get_property(Word object,const char *selector,intptr_t offset,unsigned char atomic) {
    Word *slot=(Word *)(object+offset);
    if (!atomic) return *slot;
    pthread_mutex_t *lock=metal_property_lock(slot);
    pthread_mutex_lock(lock); Word value=metal_object_retain(*slot); pthread_mutex_unlock(lock);
    return metal_object_autorelease(value);
}

static void set_atomic_property(Word object,const char *selector,Word value,intptr_t offset) {
    Word *slot=(Word *)(object+offset);
    metal_object_retain(value);
    pthread_mutex_t *lock=metal_property_lock(slot);
    pthread_mutex_lock(lock); Word previous=*slot; *slot=value; pthread_mutex_unlock(lock);
    metal_object_release(previous);
}

static void set_property(Word object,const char *selector,Word value,intptr_t offset) {
    store_strong((Word *)(object+offset),value);
}

struct Sequence {
    Word isa, count, mutation;
    size_t capacity;
    Word *items;
};

static Word new_array(Word cls,const char *selector,const Word *objects,size_t count) {
    if (count>SIZE_MAX/sizeof(Word)) stop("Array too large",selector);
    struct Sequence *array=calloc(1,sizeof(*array));
    if (!array) stop("Allocation failed",selector);
    array->isa=cls;
    array->count=array->capacity=count;
    if (count) {
        array->items=malloc(count*sizeof(Word));
        if (!array->items) stop("Allocation failed",selector);
        memcpy(array->items,objects,count*sizeof(Word));
    }
    return (Word)array;
}

static Word array_count(struct HostObject *array) { return array->content[0]; }

static Word array_at(struct Sequence *array,const char *selector,size_t index) {
    if (index>=array->count) stop("Array index out of bounds",selector);
    return array->items[index];
}

static Word array_last(struct HostObject *array,const char *selector) {
    size_t count=array_count(array);
    return count ? array_at((void *)array,selector,count-1) : 0;
}

static Word new_string(size_t length) {
    if (length>SIZE_MAX-sizeof(struct HostObject)-1) stop("String too large","NSString");
    struct HostObject *string=calloc(1,sizeof(*string)+length+1);
    if (!string) stop("Allocation failed","NSString");
    string->isa=external_class("_OBJC_CLASS_$_NSString");
    string->content[0]=0x7c8;
    string->content[1]=(Word)(string+1);
    string->content[2]=length;
    return (Word)string;
}

static Word copy_string(const char *text) {
    size_t length=strlen(text);
    Word string=new_string(length);
    memcpy((void *)read_word(string+16),text,length);
    return string;
}

#include "../frameworks/foundation/unicode.inc"

static Word number_bool(Word cls,const char *selector,Word value) {
    struct HostObject *number=&bool_numbers[(unsigned char)value!=0];
    number->isa=cls;
    number->content[0]=number==&bool_numbers[1];
    return (Word)number;
}

#include "../frameworks/foundation/decimal_value.inc"
static Word number_value(struct HostObject *number) { return number->content[0]; }

static double numeric_double(struct HostObject *number) {
    if (number->isa==decimal_number_class) return decimal_double_value(decimal_load(number));
    if (!number->content[1]) return (double)(intptr_t)number->content[0];
    double value;
    memcpy(&value,&number->content[0],sizeof(value));
    return value;
}

static Word numeric_integer(struct HostObject *number) {
    if (number->isa==decimal_number_class) return decimal_integer_value(decimal_load(number));
    return number->content[1] ? (Word)(intptr_t)numeric_double(number) : number_value(number);
}

static Word numeric_bool(struct HostObject *number) { return numeric_double(number)!=0; }

static Word number_integer(Word value) {
    struct HostObject *number=calloc(1,sizeof(*number));
    if (!number) stop("Allocation failed","NSNumber");
    number->isa=external_class("_OBJC_CLASS_$_NSNumber");
    number->content[0]=value;
    return (Word)number;
}

static Word read_preference_object(Word self,const char *selector,Word key) {
    int value=access_preference(key,-2);
    return value<0 ? 0 : number_bool(external_class("_OBJC_CLASS_$_NSNumber"),selector,value);
}

struct MapEntry { Word key, value; };
struct GuestMap {
    Word isa, count, mutation;
    size_t capacity;
    struct MapEntry *entries;
};

struct Enumeration { Word cursor, *items, *mutations, extra[5]; };

static Word array_enumerate(struct Sequence *array,const char *selector,struct Enumeration *state,Word *buffer,size_t capacity) {
    if (state->cursor>array->count) stop("Invalid enumeration cursor",selector);
    size_t count=array->count-state->cursor;
    if (count>capacity) count=capacity;
    state->items=array->items ? array->items+state->cursor : NULL;
    state->mutations=&array->mutation;
    state->cursor+=count;
    return count;
}

static Word empty_map(Word cls) {
    struct GuestMap *map=calloc(1,sizeof(*map));
    if (!map) stop("Allocation failed","NSDictionary");
    map->isa=cls;
    return (Word)map;
}

static size_t map_index(struct GuestMap *map,Word key) {
    Word string_class=external_class("_OBJC_CLASS_$_NSString");
    for (size_t index=0;index<map->count;++index) {
        Word stored=map->entries[index].key;
        if (key==stored) return index;
        if (key && read_word(key)==string_class && read_word(stored)==string_class &&
            !strcmp(string_bytes(key,"dictionary key"),string_bytes(stored,"dictionary key"))) return index;
    }
    return map->count;
}

static Word map_get(struct GuestMap *map,const char *selector,Word key) {
    size_t index=map_index(map,key);
    return index<map->count ? map->entries[index].value : 0;
}

static void map_set(struct GuestMap *map,const char *selector,Word value,Word key) {
    if (!key || !value) stop("Nil key/value",selector);
    size_t index=map_index(map,key);
    if (index==map->count) {
        if (map->count==map->capacity) {
            size_t capacity=map->capacity ? map->capacity*2 : 8;
            struct MapEntry *entries=realloc(map->entries,capacity*sizeof(*entries));
            if (!entries) stop("Allocation failed",selector);
            map->entries=entries;
            map->capacity=capacity;
        }
        ++map->count;
    }
    map->entries[index]=(struct MapEntry){key,value};
    ++map->mutation;
}

static Word map_from_objects(Word cls,const char *selector,Word *values,Word *keys,size_t count) {
    struct GuestMap *map=(struct GuestMap *)empty_map(cls);
    for (size_t index=0;index<count;++index) map_set(map,selector,values[index],keys[index]);
    return (Word)map;
}

static Word map_count(struct GuestMap *map) { return map->count; }

static Word map_copy(struct GuestMap *map,const char *selector) {
    int mutable=!strcmp(selector,"mutableCopy");
    struct GuestMap *copy=(struct GuestMap *)empty_map(external_class(mutable ? "_OBJC_CLASS_$_NSMutableDictionary" : "_OBJC_CLASS_$_NSDictionary"));
    if (map->count) {
        copy->entries=malloc(map->count*sizeof(*copy->entries));
        if (!copy->entries) stop("Allocation failed","NSDictionary copy");
        memcpy(copy->entries,map->entries,map->count*sizeof(*copy->entries));
        copy->count=copy->capacity=map->count;
    }
    return (Word)copy;
}

static Word map_enumerate(struct GuestMap *map,const char *selector,struct Enumeration *state,Word *buffer,size_t capacity) {
    size_t count=map->count-state->cursor;
    if (count>capacity) count=capacity;
    for (size_t index=0;index<count;++index) buffer[index]=map->entries[state->cursor+index].key;
    state->cursor+=count;
    state->items=buffer;
    state->mutations=&map->mutation;
    return count;
}

static Word bundle_identifier(void) { return copy_string(BUNDLE_ID); }

static Word lowercase_string(Word self,const char *selector) {
    const char *text=string_bytes(self,selector);
    Word result=copy_string(text);
    unsigned char *bytes=(unsigned char *)read_word(result+16);
    while (*bytes) {
        if (*bytes>=128) stop("Unsupported non-ASCII case conversion",selector);
        if (*bytes>='A' && *bytes<='Z') *bytes+='a'-'A';
        ++bytes;
    }
    return result;
}

static Word string_from_utf8(Word cls,const char *selector,const char *value) {
    if (!value) stop("Null UTF-8 input",selector);
    return text_decode(cls,value,strlen(value),4);
}

static Word mutable_string(Word cls) {
    Word result=new_string(0);
    ((struct HostObject *)result)->isa=cls;
    return result;
}

static void append_string(struct HostObject *object,const char *selector,Word suffix) {
    struct TextView first=text_view((Word)object,selector),second=text_view(suffix,selector);
    if (second.length>SIZE_MAX/2-first.length) stop("String too large",selector);
    size_t length=first.length+second.length;
    uint16_t *units=malloc((length+1)*sizeof(*units));
    if (!units) stop("Allocation failed",selector);
    memcpy(units,first.units,first.length*2); memcpy(units+first.length,second.units,second.length*2);
    assign_text(object,units,length,selector);
    free(units); free(first.owned); free(second.owned);
}

static Word data_with_bytes(Word cls,const char *selector,const void *bytes,size_t length) {
    if (length>SIZE_MAX-sizeof(struct HostObject) || (length && !bytes)) stop("Invalid data length",selector);
    struct HostObject *data=calloc(1,sizeof(*data)+length);
    if (!data) stop("Allocation failed",selector);
    data->isa=cls;
    data->content[0]=length;
    if (length) memcpy(data+1,bytes,length);
    if (cls==external_class("_OBJC_CLASS_$_NSMutableData")) {
        data->content[1]=(Word)(data+1); data->content[2]=length; data->content[3]=0x4d444154;
    }
    return (Word)data;
}

static Word data_buffer(struct HostObject *object) { return object->content[3]==0x4d444154 || object->content[3]==0x4e444154 ? object->content[1] : (Word)(object+1); }

static Word encoded_string(Word self,const char *selector,unsigned encoding) {
    struct TextView text=text_view(self,selector);
    unsigned char *bytes=NULL;
    size_t length=0;
    if (encoding==4) bytes=(unsigned char *)text_utf8(text.units,text.length,&length);
    else if (encoding==1 || encoding==5) {
        bytes=malloc(text.length+1);
        if (!bytes) stop("Allocation failed",selector);
        length=text.length;
        for (size_t i=0;i<text.length;++i) {
            if (text.units[i]>(encoding==1 ? 127 : 255)) { free(bytes); bytes=NULL; break; }
            bytes[i]=(unsigned char)text.units[i];
        }
    } else if (encoding==10 || encoding==0x90000100 || encoding==0x94000100) {
        size_t bom=encoding==10 ? 2 : 0;
        if (text.length>(SIZE_MAX-bom)/2) stop("String too large",selector);
        length=text.length*2+bom;
        bytes=malloc(length+1);
        if (!bytes) stop("Allocation failed",selector);
        if (bom) { bytes[0]=0xff; bytes[1]=0xfe; }
        for (size_t i=0;i<text.length;++i) {
            bytes[bom+2*i]=(unsigned char)(encoding==0x90000100 ? text.units[i]>>8 : text.units[i]);
            bytes[bom+2*i+1]=(unsigned char)(encoding==0x90000100 ? text.units[i] : text.units[i]>>8);
        }
    } else if (encoding==0x8c000100 || encoding==0x98000100 || encoding==0x9c000100) {
        size_t bom=encoding==0x8c000100 ? 4 : 0;
        if (text.length>(SIZE_MAX-bom-1)/4) stop("String too large",selector);
        bytes=malloc(text.length*4+bom+1); if (!bytes) stop("Allocation failed",selector);
        length=bom; if (bom) { bytes[0]=0xff; bytes[1]=0xfe; bytes[2]=bytes[3]=0; }
        for (size_t i=0;i<text.length;++i) {
            uint32_t scalar=text.units[i];
            if (scalar>=0xd800 && scalar<=0xdbff && i+1<text.length && text.units[i+1]>=0xdc00 && text.units[i+1]<=0xdfff)
                scalar=0x10000+((scalar-0xd800)<<10)+(text.units[++i]-0xdc00);
            else if (scalar>=0xd800 && scalar<=0xdfff) { free(bytes); bytes=NULL; break; }
            for (unsigned b=0;b<4;++b) bytes[length++]=(unsigned char)(scalar>>((encoding==0x98000100 ? 3-b : b)*8));
        }
    } else { free(text.owned); stop("Unsupported string encoding",selector); }
    Word result=bytes ? data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),selector,bytes,length) : 0;
    free(bytes); free(text.owned);
    return result;
}

static Word map_keys(struct GuestMap *map,const char *selector) {
    Word *keys=calloc(map->count,sizeof(*keys));
    if (!keys && map->count) stop("Allocation failed",selector);
    for (size_t index=0;index<map->count;++index) keys[index]=map->entries[index].key;
    Word array=new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,keys,map->count);
    free(keys);
    return array;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static double property_list_date(plist_t node) {
    int32_t seconds=0,microseconds=0;
    plist_get_date_val(node,&seconds,&microseconds);
    return 978307200.0+seconds+microseconds*1e-6;
}
static plist_t property_list_new_date(double epoch) {
    double value=epoch-978307200.0,integral=floor(value);
    if (!isfinite(value) || integral<INT32_MIN || integral>INT32_MAX) return NULL;
    return plist_new_date((int32_t)integral,(int32_t)((value-integral)*1000000));
}
#pragma clang diagnostic pop

static Word plist_object(plist_t node,unsigned depth) {
    if (!node || depth>64) stop("Invalid plist nesting","Foundation");
    switch (plist_get_node_type(node)) {
        case PLIST_NULL:
            null_value.isa=external_class("_OBJC_CLASS_$_NSNull");
            return (Word)&null_value;
        case PLIST_STRING: {
            uint64_t length;
            const char *value=plist_get_string_ptr(node,&length);
            return text_decode(external_class("_OBJC_CLASS_$_NSString"),value,length,4);
        }
        case PLIST_BOOLEAN: {
            uint8_t value;
            plist_get_bool_val(node,&value);
            return number_bool(external_class("_OBJC_CLASS_$_NSNumber"),"plist",value);
        }
        case PLIST_UINT: {
            uint64_t value;
            plist_get_uint_val(node,&value);
            return number_integer(value);
        }
        case PLIST_REAL: {
            double value;
            plist_get_real_val(node,&value);
            struct HostObject *number=(void *)number_integer(0);
            memcpy(number->content,&value,sizeof(value));
            number->content[1]=1;
            return (Word)number;
        }
        case PLIST_DATE: {
            struct HostObject *date=calloc(1,sizeof(*date));
            if (!date) stop("Allocation failed","Plist date");
            date->isa=external_class("_OBJC_CLASS_$_NSDate");
            double epoch=property_list_date(node);
            memcpy(date->content,&epoch,sizeof(epoch));
            return (Word)date;
        }
        case PLIST_DATA: {
            uint64_t size;
            const char *bytes=plist_get_data_ptr(node,&size);
            return data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),"plist",bytes,size);
        }
        case PLIST_ARRAY: {
            uint32_t count=plist_array_get_size(node);
            Word *items=calloc(count,sizeof(*items));
            if (!items && count) stop("Allocation failed","plist array");
            for (uint32_t index=0;index<count;++index) items[index]=plist_object(plist_array_get_item(node,index),depth+1);
            Word result=new_array(external_class("_OBJC_CLASS_$_NSArray"),"plist",items,count);
            free(items);
            return result;
        }
        case PLIST_DICT: {
            struct GuestMap *map=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
            plist_dict_iter iter=NULL;
            plist_dict_new_iter(node,&iter);
            for (;;) {
                char *key=NULL;
                plist_t value=NULL;
                plist_dict_next_item(node,iter,&key,&value);
                if (!value) break;
                map_set(map,"plist",plist_object(value,depth+1),copy_string(key));
                plist_mem_free(key);
            }
            plist_dict_free_iter(iter);
            return (Word)map;
        }
        default:
            stop("Unsupported plist type","Foundation");
    }
}

static Method find_implementation(Word,const char *);
static int compare_ascii_strings(const void *left,const void *right) {
    const char *first=string_bytes(*(const Word *)left,"compare:"),*second=string_bytes(*(const Word *)right,"compare:");
    for (const unsigned char *p=(const unsigned char *)first;*p;++p) if (*p>=128) stop("Unsupported non-ASCII collation","compare:");
    for (const unsigned char *p=(const unsigned char *)second;*p;++p) if (*p>=128) stop("Unsupported non-ASCII collation","compare:");
    return strcmp(first,second);
}
static int compare_selected_objects(const void *left,const void *right,void *context) {
    const char *selector=context; Word first=*(const Word *)left,second=*(const Word *)right;
    Method compare=find_implementation(first,selector);
    if (!compare) stop("Missing collection comparison selector",selector);
    intptr_t result=(intptr_t)compare(first,selector,second,0,0,0);
    return result<0 ? -1 : result>0;
}
static void sort_object_values(Word *items,size_t count,int (*compare)(const void *,const void *,void *),void *context,const char *selector) {
    if (count<2) return;
    Word *scratch=malloc(count*sizeof(*scratch)); if (!scratch) stop("Allocation failed",selector);
    for (size_t width=1;width<count;) {
        for (size_t begin=0;begin<count;) {
            size_t middle=count-begin<width ? count : begin+width,end=count-middle<width ? count : middle+width;
            size_t a=begin,b=middle,out=begin;
            while (a<middle || b<end) scratch[out++]=b==end || (a<middle && compare(items+a,items+b,context)<=0) ? items[a++] : items[b++];
            begin=end;
        }
        memcpy(items,scratch,count*sizeof(*items));
        if (width>count/2) break;
        width*=2;
    }
    free(scratch);
}

static Word sorted_array(struct Sequence *array,const char *selector,const char *comparison) {
    if (!comparison) stop("Nil collection comparison selector",selector);
    size_t count=array->count;
    struct Sequence *copy=(void *)new_array(array->isa,selector,array->items,count);
    sort_object_values(copy->items,count,compare_selected_objects,(void *)comparison,selector);
    return (Word)copy;
}

struct CoreUuid { Word tag; unsigned char bytes[16]; unsigned references; };
static Word allocator_default,uuid_tag,http_message_tag,runloop_source_tag,device_rgb_space_tag;
static Word device_rgb_space=(Word)&device_rgb_space_tag;
static Word device_gray_space_tag,device_gray_space=(Word)&device_gray_space_tag;
static int drawing_resource_release(Word),drawing_resource_retain(Word);
static int http_message_release(Word),http_message_retain(Word);
static int runloop_source_release(Word),runloop_source_retain(Word);

static struct CoreUuid *create_uuid(Word allocator) {
    if (allocator) stop("Unsupported allocator","CFUUIDCreate");
    struct CoreUuid *uuid=malloc(sizeof(*uuid));
    if (!uuid) stop("Allocation failed","CFUUIDCreate");
    uuid->tag=(Word)&uuid_tag;
    uuid->references=1;
    arc4random_buf(uuid->bytes,sizeof(uuid->bytes));
    uuid->bytes[6]=(uuid->bytes[6]&15)|64;
    uuid->bytes[8]=(uuid->bytes[8]&63)|128;
    return uuid;
}

static Word uuid_string(Word allocator,const struct CoreUuid *uuid) {
    if (allocator || !uuid || uuid->tag!=(Word)&uuid_tag) stop("Unsupported arguments","CFUUIDCreateString");
    static const char hex[]="0123456789ABCDEF";
    Word result=new_string(36);
    char *dest=(char *)read_word(result+16);
    for (unsigned index=0;index<16;++index) {
        if (index==4 || index==6 || index==8 || index==10) *dest++='-';
        *dest++=hex[uuid->bytes[index]>>4];
        *dest++=hex[uuid->bytes[index]&15];
    }
    return result;
}

static void cf_release(Word object) {
    if (!object) stop("Null object","CFRelease");
    if (read_word(object)==(Word)&uuid_tag) {
        struct CoreUuid *uuid=(void *)object;
        if (!__atomic_sub_fetch(&uuid->references,1,__ATOMIC_ACQ_REL)) free(uuid);
    }
    else if (read_word(object)==(Word)&secure_transport_tag) secure_transport_release(object);
    else if (read_word(object)==(Word)&trust_tag) trust_release(object);
    else if (object==(Word)&device_rgb_space || object==(Word)&device_gray_space) return;
    else if (drawing_resource_release(object)) return;
    else if (http_message_release(object) || runloop_source_release(object)) return;
    else if (read_word(object)==(Word)&certificate_tag || read_word(object)==(Word)&reachability_tag || read_word(object)==(Word)&security_key_tag) return;
    else if (!class_name(read_word(object))) stop("Unsupported object","CFRelease");
}

static int class_inherits(Word cls,Word target) {
    if (!target) return 0;
    for (unsigned depth=0;cls && depth<64;++depth) {
        if (cls==target) return 1;
        if (in_image(cls) || dynamic_class(cls)) cls=read_word(cls+8);
        else cls=host_superclass(cls);
    }
    return 0;
}
static Word is_kind_of(Word self,const char *selector,Word target) {
    return class_inherits(read_word(self),target);
}
static Word is_subclass_of(Word cls,const char *selector,Word target) { return class_inherits(cls,target); }

static Word replace_string(Word self,const char *selector,Word old,Word replacement) {
    const char *source=string_bytes(self,selector), *needle=string_bytes(old,selector);
    const char *insert=string_bytes(replacement,selector);
    size_t old_length=strlen(needle), new_length=strlen(insert), count=0;
    if (!old_length) return self;
    for (const char *pos=source;(pos=strstr(pos,needle));pos+=old_length) ++count;
    if (!count) return self;
    size_t kept=strlen(source)-count*old_length;
    if (new_length>(SIZE_MAX-sizeof(struct HostObject)-kept-1)/count) stop("String too large",selector);
    Word string=new_string(kept+count*new_length);
    char *dest=(char *)read_word(string+16);
    const char *match;
    while ((match=strstr(source,needle))) {
        size_t prefix=(size_t)(match-source);
        memcpy(dest,source,prefix);
        memcpy(dest+prefix,insert,new_length);
        dest+=prefix+new_length;
        source=match+old_length;
    }
    strcpy(dest,source);
    return string;
}

static Word file_url(Word path) {
    struct HostObject *url=calloc(1,sizeof(*url));
    if (!url) stop("Allocation failed","NSURL");
    url->isa=external_class("_OBJC_CLASS_$_NSURL");
    url->content[0]=path;
    url->content[1]=1;
    return (Word)url;
}

static Word parsed_url_path(struct HostObject *url);
static Word url_path(struct HostObject *url) { return parsed_url_path(url); }

static Word append_component(struct HostObject *url,const char *selector,Word component) {
    const char *base=string_bytes(url_path(url),selector), *leaf=string_bytes(component,selector);
    if (!*leaf) return (Word)url;
    if (strchr(leaf,'/') || !strcmp(leaf,"..") || !strcmp(leaf,".")) stop("Unsupported path component",leaf);
    size_t length=strlen(base)+1+strlen(leaf);
    Word path=new_string(length);
    snprintf((char *)read_word(path+16),length+1,"%s/%s",base,leaf);
    return file_url(path);
}

static Word append_typed_component(struct HostObject *url,const char *selector,Word component,unsigned char directory) {
    Word result=append_component(url,selector,component);
    if (result!=(Word)url) ((struct HostObject *)result)->content[1]=directory!=0;
    return result;
}

static Word append_extension(struct HostObject *url,const char *selector,Word extension) {
    const char *base=string_bytes(url_path(url),selector), *suffix=string_bytes(extension,selector);
    if (!*suffix || strchr(suffix,'/')) return 0;
    size_t length=strlen(base)+1+strlen(suffix);
    Word path=new_string(length);
    snprintf((char *)read_word(path+16),length+1,"%s.%s",base,suffix);
    return file_url(path);
}

static Word default_manager(Word cls) {
    file_manager.isa=cls;
    return (Word)&file_manager;
}

static Word directory_urls(Word self,const char *selector,Word directory,Word domains) {
    if (directory!=14 || domains!=1) stop("Unsupported directory/domain",selector);
    Word url=file_url(copy_string(files_directory));
    return new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,&url,1);
}

static Word file_exists(Word self,const char *selector,Word path,unsigned char *is_directory) {
    const char *name=string_bytes(path,selector);
    struct stat info;
    int exists=stat(name,&info)==0;
    if (is_directory) *is_directory=exists && S_ISDIR(info.st_mode);
    LOG("NSFileManager %s exists=%d",name,exists);
    return exists;
}

static const char *private_path(struct HostObject *url,const char *selector) {
    const char *name=string_bytes(url_path(url),selector);
    size_t root=strlen(files_directory);
    if (strncmp(name,files_directory,root) || name[root]!='/' || strstr(name,"/../") || !strcmp(strrchr(name,'/'),"/..")) stop("Unsupported private path",name);
    return name;
}

static Word make_directory(Word self,const char *selector,struct HostObject *url,Word intermediates,Word attributes,Word *error) {
    const char *name=private_path(url,selector);
    if (attributes) stop("Unsupported directory attributes",selector);
    char *path=strdup(name);
    if (!path) stop("Allocation failed",selector);
    int success;
    char *part=path+strlen(files_directory)+1;
    do {
        char *slash=intermediates ? strchr(part,'/') : NULL;
        if (slash) *slash='\0';
        struct stat info;
        success=mkdir(path,0700)==0 || (intermediates && errno==EEXIST && stat(path,&info)==0 && S_ISDIR(info.st_mode));
        if (!slash) break;
        *slash='/';
        part=slash+1;
    } while (success);
    free(path);
    if (!success && error) stop("Directory creation failed",name);
    return success;
}

static Word write_dictionary(struct GuestMap *map,const char *selector,struct HostObject *url,Word atomic) {
    const char *path=private_path(url,selector);
    plist_t dict=plist_new_dict();
    Word strings=external_class("_OBJC_CLASS_$_NSString");
    for (size_t index=0;index<map->count;++index) {
        const struct MapEntry *entry=map->entries+index;
        plist_t item;
        if (read_word(entry->value)==strings) item=plist_new_string(string_bytes(entry->value,selector));
        else if (entry->value==(Word)&bool_numbers[0] || entry->value==(Word)&bool_numbers[1]) item=plist_new_bool(entry->value==(Word)&bool_numbers[1]);
        else if (read_word(entry->value)==external_class("_OBJC_CLASS_$_NSNumber")) {
            struct HostObject *number=(void *)entry->value;
            item=number->content[1] ? plist_new_real(numeric_double(number)) : plist_new_int((int64_t)number->content[0]);
        }
        else stop("Unsupported plist value",selector);
        if (plist_dict_set_item(dict,string_bytes(entry->key,selector),item)!=PLIST_ERR_SUCCESS) stop("Plist dictionary error",selector);
    }
    char *temporary=NULL;
    if (atomic) {
        if (asprintf(&temporary,"%s.XXXXXX",path)<0) stop("Allocation failed",selector);
        int fd=mkstemp(temporary);
        if (fd<0) { free(temporary); plist_free(dict); return 0; }
        close(fd);
    }
    int success=plist_write_to_file(dict,atomic ? temporary : path,PLIST_FORMAT_BINARY,PLIST_OPT_NONE)==PLIST_ERR_SUCCESS;
    if (atomic) {
        if (success) success=rename(temporary,path)==0;
        if (!success) unlink(temporary);
        free(temporary);
    }
    plist_free(dict);
    LOG("plist write %s entries=%lu ok=%d",path,map->count,success);
    return success;
}

static Word read_dictionary(Word cls,const char *selector,struct HostObject *url) {
    const char *path=private_path(url,selector);
    plist_t dict=NULL;
    if (plist_read_from_file(path,&dict,NULL)!=PLIST_ERR_SUCCESS) return 0;
    if (plist_get_node_type(dict)!=PLIST_DICT) { plist_free(dict); return 0; }
    struct GuestMap *map=(void *)plist_object(dict,0);
    map->isa=cls;
    plist_free(dict);
    LOG("plist read %s entries=%lu",path,map->count);
    return (Word)map;
}

static Word class_of(Word object) { return read_word(object); }

__attribute__((used,noinline)) static Method super_lookup(const Word *pair,const char *selector) {
    Word start=pair[1];
    if (!in_image(start) && !dynamic_class(start) && !host_symbol(start)) stop("Unsupported superclass",selector);
    Word parent=read_word(start+8);
    Method result=lookup(parent,selector);
    if (result) return result;
    int class_receiver=guest_class_object(pair[0]) || host_symbol(pair[0])!=NULL;
    const char *receiver=class_name(parent);
    char class_symbol[256];
    if(receiver && !strncmp(receiver,"_OBJC_METACLASS_$_",sizeof("_OBJC_METACLASS_$_")-1)){
        snprintf(class_symbol,sizeof(class_symbol),"_OBJC_CLASS_$_%s",receiver+sizeof("_OBJC_METACLASS_$_")-1);receiver=class_symbol;
    }
    Word parent_class=class_receiver ? (receiver ? class_from_name(receiver) : 0) : parent;
    struct HostObject prototype={.isa=parent_class};
    const char *base=parent_class ? external_base(parent_class) : NULL;
    result=parent_class ? framework_implementation(class_receiver ? parent_class : (Word)&prototype,selector,base ? base : receiver,class_receiver) : NULL;
    if (result) return result;
    if(!class_receiver && pair[0]==application_object){
        if(!strcmp(selector,"delegate"))return (Method)get_delegate;
        if(!strcmp(selector,"setDelegate:"))return (Method)set_delegate;
        if(!strcmp(selector,"keyWindow"))return (Method)get_window;
        result=framework_implementation(pair[0],selector,base ? base : receiver,0);if(result)return result;
    }
    if (!strcmp(selector,"class")) return class_receiver ? (Method)identity : (Method)class_of;
    if (!strcmp(selector,"superclass")) return (Method)object_superclass;
    if (!class_receiver && !strcmp(selector,"isKindOfClass:")) return (Method)is_kind_of;
    if (guest_class_object(pair[0]) && (!strcmp(selector,"allocWithZone:") || !strcmp(selector,"alloc"))) return (Method)guest_object;
    if (parent && !strcmp(selector,"init")) return (Method)identity;
    stop("Unimplemented superclass method",selector);
}

__attribute__((naked)) static void message_super2(void) {
    __asm__ volatile(
        "sub sp,sp,#208\n stp x0,x1,[sp]\n stp x2,x3,[sp,#16]\n stp x4,x5,[sp,#32]\n"
        "stp x6,x7,[sp,#48]\n stp x8,x30,[sp,#64]\n stp q0,q1,[sp,#80]\n stp q2,q3,[sp,#112]\n"
        "stp q4,q5,[sp,#144]\n stp q6,q7,[sp,#176]\n bl super_lookup\n mov x16,x0\n"
        "ldp q6,q7,[sp,#176]\n ldp q4,q5,[sp,#144]\n ldp q2,q3,[sp,#112]\n ldp q0,q1,[sp,#80]\n"
        "ldp x8,x30,[sp,#64]\n ldp x6,x7,[sp,#48]\n ldp x4,x5,[sp,#32]\n ldp x2,x3,[sp,#16]\n"
        "ldp x0,x1,[sp]\n ldr x0,[x0]\n add sp,sp,#208\n br x16");
}

static Word format_argument(const unsigned char **arguments,size_t size) {
    Word result=0;
    uintptr_t aligned=((uintptr_t)*arguments+7)&~(uintptr_t)7;
    memcpy(&result,(void *)aligned,size);
    *arguments=(const unsigned char *)aligned+8;
    return result;
}

static void print_object(FILE *stream,Word object) {
    if (!object) { fputs("(null)",stream); return; }
    int class_receiver=host_symbol(object)!=NULL || guest_class_object(object);
    Word cls=class_receiver ? object : read_word(object);
    const char *name=class_name(cls);
    const char *display=name && !strncmp(name,"_OBJC_CLASS_$_",14) ? name+14 : name;
    if (class_receiver) { fputs(display ? display : "NSObject",stream); return; }
    if (cls==external_class("_OBJC_CLASS_$_NSString") || cls==external_class("_OBJC_CLASS_$_NSMutableString")) {
        fputs(string_bytes(object,"description"),stream);
        return;
    }
    if (cls==decimal_number_class) {
        char text[192]; decimal_format(decimal_load((void *)object),text); fputs(text,stream);
    }
    else if (name && !strcmp(name,"_OBJC_CLASS_$_NSNumber")) {
        struct HostObject *number=(void *)object;
        if (number->content[1]) fprintf(stream,"%.17g",numeric_double(number));
        else fprintf(stream,"%lld",(long long)number->content[0]);
    }
    else fprintf(stream,"<%s: 0x%lx>",display ? display : "NSObject",object);
}

#include "../frameworks/foundation/foundation_format.inc"

__attribute__((naked)) static void darwin_format(void) {
    __asm__ volatile("mov x3,sp\n b foundation_format");
}

__attribute__((used,noinline)) static void foundation_log(Word format,const unsigned char *arguments) {
    if (!I2A_DEVELOPMENT) return;
    Word result=foundation_format(0,"NSLog",format,arguments);
    LOG("NSLog %s",string_bytes(result,"NSLog"));
    free((void *)result);
}

__attribute__((naked)) static void darwin_nslog(void) { __asm__ volatile("mov x1,sp\n b foundation_log"); }

static Word main_bundle(Word cls) {
    bundle.isa=cls;
    return (Word)&bundle;
}

static Word bundle_info(void) {
    if (!bundle_dictionary) {
        plist_t info=NULL;
        if (plist_from_memory((const char *)bundle_plist,sizeof(bundle_plist),&info,NULL)!=PLIST_ERR_SUCCESS)
            stop("Invalid bundle plist","Info.plist");
        bundle_dictionary=plist_object(info,0);
        plist_free(info);
        LOG("Original bundle Info.plist loaded (%lu keys)",map_count((void *)bundle_dictionary));
    }
    return bundle_dictionary;
}

static Word singleton_center(Word cls) {
    center.isa = cls;
    return (Word)&center;
}

static Word make_observer(Word self,Word receiver,Word action,Word name,Word object,Word queue,Word block,Word callback) {
    struct Observer *observer = calloc(1,sizeof(*observer));
    if (!observer) stop("Allocation failed","Notification observer");
    observer->token.isa=external_class("_OBJC_CLASS_$_NSObject");
    observer->center=self; observer->queue=queue; observer->callback=callback; observer->active=1;
    observer->block=block ? (Word)_Block_copy((void *)block) : 0;
    observer->receiver=receiver;
    observer->selector=action;
    observer->name=name;
    observer->object=object;
    pthread_mutex_lock(&observers_mutex);
    observer->next=observers;
    observers=observer;
    pthread_mutex_unlock(&observers_mutex);
    return (Word)observer;
}
static Word add_observer(Word self,const char *selector,Word receiver,Word action,Word name,Word object) {
    return make_observer(self,receiver,action,name,object,0,0,0);
}

static Word add_block_observer(Word self,const char *selector,Word name,Word object,Word queue,Word block) {
    return make_observer(self,0,0,name,object,queue,block,0);
}

static Word new_date(Word cls) {
    struct HostObject *object=calloc(1,sizeof(*object));
    if (!object) stop("Allocation failed","NSDate");
    object->isa=cls;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME,&now);
    double seconds=now.tv_sec + now.tv_nsec*1e-9;
    memcpy(object->content,&seconds,sizeof(seconds));
    return (Word)object;
}

static double date_seconds(Word object) {
    double value;
    memcpy(&value,((struct HostObject *)object)->content,sizeof(value));
    return value;
}

static Method find_implementation(Word object,const char *selector);
static int io_open(const char *,int,mode_t);
static ssize_t io_read(int,void *,size_t);
static ssize_t io_pread(int,void *,size_t,off_t);
static ssize_t io_readv(int,const struct iovec *,int);
static off_t io_seek(int,off_t,int);
static int io_close(int);
static int io_fstat(int,struct stat *);
static int io_stat(const char *,struct stat *);
static void *io_mmap(void *,size_t,int,int,int,off_t);
static int io_dup(int);
static int io_dup2(int,int);
static int bundle_fd_duplicate(int,int);
static Word bundle_data(Word,const char *);
static Word string_from_java(JNIEnv *,jstring,const char *);
static AAssetManager *bundle_assets;
static jobject bundle_assets_owner;
static struct stat bundle_archive_info;
static const char *bundle_relative(const char *path) {
    if (!path) return NULL;
    if (*path=='/') ++path;
    size_t length=sizeof(GUEST_BUNDLE_NAME)-1;
    if (strncmp(path,GUEST_BUNDLE_NAME,length)) return NULL;
    if (!path[length]) return path+length;
    return path[length]=='/' ? path+length+1 : NULL;
}
#include "../frameworks/foundation/foundation.inc"
#include "../frameworks/network/security.inc"
#include "../frameworks/network/secure_transport.inc"
#include "../frameworks/device/platform.inc"
#include "../frameworks/foundation/calendar.inc"
#include "darwin/filesystem.inc"
#include "../frameworks/foundation/url.inc"
#include "../frameworks/foundation/url_components.inc"
#include "../frameworks/foundation/data.inc"
#include "darwin/filesystem_url.inc"
#include "../frameworks/foundation/sqlite_bridge.inc"
#include "../frameworks/foundation/streams.inc"
#include "../frameworks/foundation/characters.inc"
#include "../frameworks/foundation/scanner.inc"
#include "../frameworks/foundation/serialization.inc"
#include "../frameworks/foundation/preferences.inc"
#include "../frameworks/foundation/regular_expression.inc"
#include "objc/metadata.inc"
#include "../frameworks/network/network.inc"
#include "../frameworks/network/connection.inc"
#include "objc/objc.inc"
#include "scheduling/operations.inc"
#include "darwin/stdio.inc"
#include "darwin/logging.inc"
#include "../frameworks/foundation/corefoundation.inc"
#include "darwin/atomics.inc"
#include "scheduling/threads.inc"
#include "darwin/cxx.inc"
#include "darwin/malloc_zones.inc"
#include "darwin/nonlocal.inc"
#include "../diagnostics/backtrace.inc"
#include "../diagnostics/crash.inc"
#include "darwin/libsystem.inc"
#include "darwin/kernel.inc"
#include "darwin/mach_semaphores.inc"
#include "darwin/dyld.inc"
#include "darwin/signals.inc"
#include "objc/exceptions.inc"
#include "scheduling/dispatch_loop.inc"
#include "scheduling/scheduling.inc"
#include "../frameworks/foundation/locale.inc"
#include "../frameworks/device/telephony.inc"
#include "../frameworks/foundation/objects.inc"
#include "../frameworks/foundation/number_formatter.inc"
#include "../frameworks/foundation/decimal_number.inc"
#include "../frameworks/foundation/assertion_handler.inc"
#include "../frameworks/foundation/directory_enumerator.inc"
#include "../frameworks/network/http.inc"
#include "../frameworks/foundation/uniform_types.inc"
#include "scheduling/runloop_source.inc"
#include "../frameworks/foundation/pointer_collections.inc"
#include "../frameworks/foundation/kvo.inc"
#include "../frameworks/foundation/notifications.inc"
#include "../frameworks/foundation/notification_queue.inc"
#include "../frameworks/network/reachability.inc"
#include "darwin/network_interfaces.inc"
#include "darwin/sockets.inc"
#include "../frameworks/uikit/system.inc"
#include "../frameworks/uikit/views.inc"

#include "../frameworks/uikit/touch.inc"
#include "../frameworks/uikit/text.inc"
#include "../frameworks/uikit/input.inc"
#include "../frameworks/uikit/labels.inc"
#include "../frameworks/uikit/platform.inc"
#include "../frameworks/uikit/tables.inc"
#include "../frameworks/uikit/share.inc"
#include "../frameworks/uikit/photos.inc"
#include "../frameworks/uikit/alerts.inc"
#include "../frameworks/uikit/storyboards.inc"
#include "../frameworks/foundation/graphics.inc"
#include "../frameworks/uikit/display_link.inc"
#include "../frameworks/device/motion.inc"
#include "../frameworks/foundation/apple_services.inc"
#include "../frameworks/device/location.inc"
#include "../frameworks/network/session.inc"
#include "../frameworks/device/audio_session.inc"
#include "../frameworks/device/audio_player.inc"
#include "../frameworks/device/audio_unit.inc"
#include "../frameworks/device/audio_file.inc"
#include "../frameworks/device/audio_queue.inc"
#include "../frameworks/foundation/keyed_archive.inc"
#include "../frameworks/network/keychain.inc"
#include "../frameworks/network/rsa.inc"
#include "objc/dynamic.inc"
#include "../frameworks/foundation/constants.inc"
#include "objc/layout.inc"
#include "../frameworks/helpshift.inc"

static Method framework_implementation(Word object,const char *selector,const char *receiver,int class_receiver) {
    Method found;
    found=invocation_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=net_service_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=decimal_number_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=assertion_handler_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=directory_enumerator_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=cookie_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=response_cache_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=cookie_storage_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=foundation_collection_io_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=scheduling_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=locale_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=telephony_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=foundation_value_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=number_formatter_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=pointer_collections_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=location_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=url_components_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=url_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=filesystem_url_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=streams_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=characters_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=scanner_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=preferences_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=regular_expression_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=keyed_archive_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_text_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_input_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_paragraph_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_tables_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_labels_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_platform_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=photos_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_share_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_alerts_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=storyboards_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_system_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=notifications_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=notification_queue_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=graphics_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=display_link_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=motion_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=views_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=ui_touch_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=apple_services_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=audio_session_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=audio_player_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=data_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=serialization_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=metadata_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=operations_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=calendar_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=filesystem_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=platform_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=network_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=session_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=connection_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=exceptions_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=foundation_method(object,selector,receiver,class_receiver);
    if (found) return found;
    found=kvo_method(object,selector,receiver,class_receiver);
    if (found) return found;
    return NULL;
}

static Method find_implementation_uncached(Word object, const char *selector) {
    if (!object) return NULL;
    const char *external = host_symbol(object);
    Method found = external ? lookup_external_class_method(object,selector) : lookup(read_word(object), selector);
    if (found && runtime_extension.wrap_method) found=runtime_extension.wrap_method(found);
    if (found) return found;
    if (!strcmp(selector,"superclass")) return (Method)object_superclass;
    if (!external && !strcmp(selector,"isKindOfClass:")) return (Method)is_kind_of;
    int class_receiver=external!=NULL || guest_class_object(object);
    if (!strcmp(selector,"class")) return class_receiver ? (Method)identity : (Method)class_of;
    if ((!strcmp(selector,"alloc") || !strcmp(selector,"allocWithZone:")) && !external && class_receiver) return (Method)guest_object;
    if (!class_receiver && !strcmp(selector,"init")) {
        const char *base=external_base(read_word(object));
        if (base && !strcmp(base,"_OBJC_CLASS_$_NSObject")) return (Method)identity;
    }
    const char *receiver = external ? external : class_name(class_receiver ? object : read_word(object));
    found=framework_implementation(object,selector,receiver,class_receiver);
    if (found) return found;
    if (receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSArray")) {
        if (external && !strcmp(selector,"arrayWithObjects:count:")) return (Method)new_array;
        if (!external && object!=(Word)&arguments_array) {
            if (!strcmp(selector,"count")) return (Method)array_count;
            if (!strcmp(selector,"lastObject")) return (Method)array_last;
            if (!strcmp(selector,"sortedArrayUsingSelector:")) return (Method)sorted_array;
            if (!strcmp(selector,"countByEnumeratingWithState:objects:count:")) return (Method)array_enumerate;
            if (!strcmp(selector,"objectAtIndex:") || !strcmp(selector,"objectAtIndexedSubscript:")) return (Method)array_at;
        }
    }
    if (receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary"))) {
        if (external && !strcmp(selector,"dictionary")) return (Method)empty_map;
        if (external && !strcmp(selector,"dictionaryWithContentsOfURL:")) return (Method)read_dictionary;
        if (external && !strcmp(selector,"dictionaryWithObjects:forKeys:count:")) return (Method)map_from_objects;
        if (!external) {
            if (!strcmp(selector,"allKeys")) return (Method)map_keys;
            if (!strcmp(selector,"copy") || !strcmp(selector,"mutableCopy")) return (Method)map_copy;
            if (!strcmp(selector,"writeToURL:atomically:")) return (Method)write_dictionary;
            if (!strcmp(selector,"count")) return (Method)map_count;
            if (!strcmp(selector,"objectForKey:") || !strcmp(selector,"objectForKeyedSubscript:")) return (Method)map_get;
            if (!strcmp(selector,"countByEnumeratingWithState:objects:count:")) return (Method)map_enumerate;
            if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary") && (!strcmp(selector,"setObject:forKey:") || !strcmp(selector,"setValue:forKey:"))) return (Method)map_set;
        }
    }
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSFileManager") && !strcmp(selector,"defaultManager")) return (Method)default_manager;
    if (object==(Word)&file_manager) {
        if (!strcmp(selector,"createDirectoryAtURL:withIntermediateDirectories:attributes:error:")) return (Method)make_directory;
        if (!strcmp(selector,"URLsForDirectory:inDomains:")) return (Method)directory_urls;
        if (!strcmp(selector,"fileExistsAtPath:isDirectory:")) return (Method)file_exists;
    }
    if (!external && receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSURL")) {
        if (!strcmp(selector,"path")) return (Method)url_path;
        if (!strcmp(selector,"URLByAppendingPathComponent:")) return (Method)append_component;
        if (!strcmp(selector,"URLByAppendingPathComponent:isDirectory:")) return (Method)append_typed_component;
        if (!strcmp(selector,"URLByAppendingPathExtension:")) return (Method)append_extension;
    }
    if (!external && receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSString") && !strcmp(selector,"stringByReplacingOccurrencesOfString:withString:")) return (Method)replace_string;
    if (!external && receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSString") && !strcmp(selector,"lowercaseString")) return (Method)lowercase_string;
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSString") && !strcmp(selector,"stringWithUTF8String:")) return (Method)string_from_utf8;
    if (receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString")) {
        if (external && !strcmp(selector,"string")) return (Method)mutable_string;
        if (!external && !strcmp(selector,"appendString:")) return (Method)append_string;
    }
    if (!external && receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString")) && !strcmp(selector,"dataUsingEncoding:")) return (Method)encoded_string;
    if (receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSData")) {
        if (external && !strcmp(selector,"dataWithBytes:length:")) return (Method)data_with_bytes;
        if (!external && !strcmp(selector,"length")) return (Method)array_count;
        if (!external && !strcmp(selector,"bytes")) return (Method)data_buffer;
    }
    if (external && !strcmp(external,"_OBJC_CLASS_$_UIApplication") && !strcmp(selector,"sharedApplication")) return (Method)shared_app;
    if (object==application_object && !strcmp(selector,"delegate")) return (Method)get_delegate;
    if (object==application_object && !strcmp(selector,"setDelegate:")) return (Method)set_delegate;
    if (object==application_object && !strcmp(selector,"keyWindow")) return (Method)get_window;
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSThread") && !strcmp(selector,"mainThread")) return (Method)get_main_thread;
    if (object==(Word)&main_thread && !strcmp(selector,"setName:")) return (Method)set_thread_name;
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSUserDefaults") && !strcmp(selector,"standardUserDefaults")) return (Method)standard_defaults;
    if (object==(Word)&defaults) {
        if (!strcmp(selector,"objectForKey:")) return (Method)read_preference_object;
        if (!strcmp(selector,"boolForKey:")) return (Method)read_bool;
        if (!strcmp(selector,"removeObjectForKey:")) return (Method)remove_default;
        if (!strcmp(selector,"setBool:forKey:")) return (Method)set_bool;
    }
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSNumber") && !strcmp(selector,"numberWithBool:")) return (Method)number_bool;
    if (!external && receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSNumber")) {
        if (!strcmp(selector,"boolValue")) return (Method)numeric_bool;
        if (!strcmp(selector,"intValue") || !strcmp(selector,"integerValue")) return (Method)numeric_integer;
    }
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSProcessInfo") && !strcmp(selector,"processInfo")) return (Method)process_singleton;
    if (object==(Word)&process_info && !strcmp(selector,"arguments")) return (Method)process_arguments;
    if (object==(Word)&arguments_array && !strcmp(selector,"containsObject:")) return (Method)argv_contains;
    if (external && !strcmp(external,"_OBJC_CLASS_$_NSString") && !strcmp(selector,"stringWithFormat:")) return (Method)darwin_format;
    if (receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSBundle")) {
        if (external && !strcmp(selector,"mainBundle")) return (Method)main_bundle;
        if (object==(Word)&bundle && !strcmp(selector,"bundleIdentifier")) return (Method)bundle_identifier;
        if (object==(Word)&bundle && !strcmp(selector,"infoDictionary")) return (Method)bundle_info;
    }
    if (receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSNotificationCenter")) {
        if (external && !strcmp(selector,"defaultCenter")) return (Method)singleton_center;
        if (!external && !strcmp(selector,"addObserver:selector:name:object:")) return (Method)add_observer;
        if (!external && !strcmp(selector,"addObserverForName:object:queue:usingBlock:")) return (Method)add_block_observer;
    }
    if (receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSDate")) {
        if (external && !strcmp(selector,"date")) return (Method)new_date;
        if (!external && !strcmp(selector,"timeIntervalSince1970")) return (Method)date_seconds;
    }
    return NULL;
}

static Method find_implementation(Word object,const char *selector) {
#if I2A_DEVELOPMENT
    if (!__atomic_load_n(&cpu_sampling_active,__ATOMIC_RELAXED)) return find_implementation_uncached(object,selector);
    const char *saved_selector=sampling_selector; Word saved_class=sampling_class;
    sampling_selector=selector; sampling_class=object ? (host_symbol(object) ? object : read_word(object)) : 0;
#endif
    Method result=find_implementation_uncached(object,selector);
#if I2A_DEVELOPMENT
    sampling_selector=saved_selector; sampling_class=saved_class;
#endif
    return result;
}

__attribute__((used,noinline)) static Method i2a_lookup(Word object,const char *selector,Word caller) {
    uint64_t diagnostic_start=render_diagnostic_clock();
     
    struct GraphicsMethodEntry { Word cls,receiver; const char *selector; Method method; unsigned epoch; };
    static _Thread_local struct GraphicsMethodEntry graphics_cache[512];
    struct GraphicsMethodEntry *entry=NULL;
    unsigned epoch=__atomic_load_n(&method_cache_epoch,__ATOMIC_ACQUIRE);
    Word cls=0,receiver_key=object==(Word)&bundle ? object : 0;
    if (in_image((Word)selector) && !host_symbol(object)) {
        cls=read_word(object);
        entry=&graphics_cache[((cls>>5)^(receiver_key>>5)^((Word)selector>>2))&511];
        if (entry->cls==cls && entry->receiver==receiver_key && entry->selector==selector && entry->epoch==epoch) {
            render_diagnostic_cost(3,diagnostic_start); return entry->method;
        }
    }
    if (!host_symbol(object)) initialize_guest_class(guest_class_object(object) ? object : read_word(object));
    Method found=find_implementation(object,selector);
    if (found && entry) {
        const char *name=host_symbol(cls);
        if (name && (!strncmp(name,"_OBJC_CLASS_$_I2AMetal",22) || !strncmp(name,"_OBJC_CLASS_$_MTL",17) || !strcmp(name,"_OBJC_CLASS_$_NSBundle")))
            *entry=(struct GraphicsMethodEntry){cls,receiver_key,selector,found,epoch};
    }
    render_diagnostic_cost(3,diagnostic_start);
    if (found) return found;
    const char *external=host_symbol(object);
    const char *receiver=external ? external : class_name(read_word(object));
    char detail[256];
    snprintf(detail,sizeof(detail),"[%s %s] caller=0x%lx", receiver ? receiver : "unknown", selector,guest_native_address(caller ? caller-4 : 0));
    stop("Unimplemented message", detail);
}

__attribute__((naked)) static void message_send(void) {
    __asm__ volatile(
        "cbnz x0, 1f\n movi v0.16b,#0\n movi v1.16b,#0\n movi v2.16b,#0\n movi v3.16b,#0\n mov x1,xzr\n ret\n"
        "1: sub sp,sp,#224\n stp x0,x1,[sp]\n stp x2,x3,[sp,#16]\n stp x4,x5,[sp,#32]\n"
        "stp x6,x7,[sp,#48]\n stp x8,x30,[sp,#64]\n stp q0,q1,[sp,#80]\n stp q2,q3,[sp,#112]\n"
        "stp q4,q5,[sp,#144]\n stp q6,q7,[sp,#176]\n mov x2,x30\n bl i2a_lookup\n mov x16,x0\n"
        "ldp q6,q7,[sp,#176]\n ldp q4,q5,[sp,#144]\n ldp q2,q3,[sp,#112]\n ldp q0,q1,[sp,#80]\n"
        "ldp x8,x30,[sp,#64]\n ldp x6,x7,[sp,#48]\n ldp x4,x5,[sp,#32]\n ldp x2,x3,[sp,#16]\n"
        "ldp x0,x1,[sp]\n add sp,sp,#224\n br x16");
}

/* The objc_opt_* entry points are message-send optimizations. Route through
 * ordinary dispatch so custom overrides, +initialize and nil semantics survive. */
static Word objc_optimized_send(Word object,const char *selector,Word argument,Word caller) {
    if (!object) return 0;
    Method method=i2a_lookup(object,selector,caller);
    return method(object,selector,argument,0,0,0);
}
static Word objc_opt_class(Word object) { return objc_optimized_send(object,"class",0,(Word)__builtin_return_address(0)); }
static Word objc_opt_self(Word object) { return objc_optimized_send(object,"self",0,(Word)__builtin_return_address(0)); }
static Word objc_opt_new(Word object) { return objc_optimized_send(object,"new",0,(Word)__builtin_return_address(0)); }
static Word objc_opt_kind(Word object,Word cls) { return objc_optimized_send(object,"isKindOfClass:",cls,(Word)__builtin_return_address(0)); }
static Word objc_opt_responds(Word object,const char *selector) { return objc_optimized_send(object,"respondsToSelector:",(Word)selector,(Word)__builtin_return_address(0)); }
static Word objc_alloc_bridge(Word cls) { return objc_optimized_send(cls,"alloc",0,(Word)__builtin_return_address(0)); }
static Word objc_alloc_zone_bridge(Word cls) { return objc_optimized_send(cls,"allocWithZone:",0,(Word)__builtin_return_address(0)); }
static Word objc_alloc_init_bridge(Word cls) {
    Word caller=(Word)__builtin_return_address(0),object=objc_optimized_send(cls,"alloc",0,caller);
    return objc_optimized_send(object,"init",0,caller);
}
static int guest_atexit(void (*callback)(void *), void *arg, void *dso) {
    extern int __cxa_atexit(void (*)(void *), void *, void *);
    extern void *__dso_handle;
    return __cxa_atexit(callback,arg,&__dso_handle);
}

static void fill_pattern(void *output,const void *pattern,size_t bytes,size_t width) {
    if (!bytes) return;
    size_t filled=bytes<width ? bytes : width;
    memmove(output,pattern,filled);
    while (filled<bytes) {
        size_t remaining=bytes-filled;
        size_t count=filled<remaining ? filled : remaining;
        memcpy((unsigned char *)output+filled,output,count);
        filled+=count;
    }
}

static void pattern16(void *output,const void *pattern,size_t bytes) { fill_pattern(output,pattern,bytes,16); }
static void pattern4(void *output,const void *pattern,size_t bytes) { fill_pattern(output,pattern,bytes,4); }

static void zero_bytes(void *output,size_t size) { memset(output,0,size); }

static uint64_t absolute_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000000ULL + now.tv_nsec;
}

static int timebase_info(uint32_t *scale) {
    if (!scale) return 4;
    scale[0]=1;
    scale[1]=1;
    return 0;
}

struct MachArchitecture {
    const char *name;
    int32_t cpu, subtype, byte_order;
    const char *description;
};

static const struct MachArchitecture *local_architecture(void) {
    static const struct MachArchitecture arch={"arm64",0x0100000c,0,1,"ARM64"};
    return &arch;
}

static const struct MachArchitecture *architecture_for_cpu(int32_t cpu,int32_t subtype) {
    const struct MachArchitecture *arch=local_architecture();
    return cpu==arch->cpu && (subtype==arch->subtype || subtype==-1) ? arch : NULL;
}

struct GuestExport { struct GuestExport *next; char *name; Word address,storage[4]; };
static struct GuestExport *guest_exports;
static pthread_mutex_t guest_export_mutex=PTHREAD_MUTEX_INITIALIZER;
static Word guest_symbol(intptr_t handle,const char *name) {
    if (!name || !*name || strlen(name)>=191) { guest_library_fail("dlsym","invalid symbol"); return 0; }
#ifdef I2A_GUEST_COLLECTION
    int embedded=embedded_library_handle(handle);
    if(embedded>=0){
        pthread_mutex_lock(&guest_library_mutex);
        int valid=embedded_library_handles[embedded].references!=0;
        pthread_mutex_unlock(&guest_library_mutex);
        if(!valid){guest_library_fail("dlsym","closed embedded handle");return 0;}
        char symbol[256];snprintf(symbol,sizeof(symbol),"_%s",name);
        Word result=guest_collection_export(symbol,embedded);
        if(!result)guest_library_fail("dlsym",name);
        return result;
    }
#endif
    pthread_once(&guest_library_once,guest_library_initialize);
    int main_only=handle==-5 || handle==-3;
    int global_scope=handle==-2;
    if (handle!=-2 && !main_only) {
        pthread_mutex_lock(&guest_library_mutex);
        struct GuestLibrary *library=guest_library_handle(handle);
        int valid=library && library->references;
        int exported=valid && guest_library_exports(library,name);
        global_scope=valid && library==guest_libraries;
        pthread_mutex_unlock(&guest_library_mutex);
        if (!valid) { guest_library_fail("dlsym","invalid handle"); return 0; }
        if (!exported) { guest_library_fail("dlsym",name); return 0; }
    }
    if (!strcmp(name,"_mh_execute_header")) return aslr;
#ifdef I2A_GUEST_COLLECTION
    char native_name[256];snprintf(native_name,sizeof(native_name),"_%s",name);
    Word native_export=guest_collection_export(native_name,main_only ? 0 : -1);
    if(native_export)return native_export;
#endif
    if (global_scope) {
        if (!strncmp(name,"OBJC_CLASS_$_",13) || !strncmp(name,"OBJC_METACLASS_$_",17)) {
            Word storage[4]={0},defined=guest_find_export(name,1,storage);
            if (defined) return defined;
        }
        pthread_mutex_lock(&guest_library_mutex);
        int visible=guest_library_owner(name)->global;
        pthread_mutex_unlock(&guest_library_mutex);
        if (!visible) { guest_library_fail("dlsym",name); return 0; }
    }
    pthread_mutex_lock(&guest_export_mutex);
    for (struct GuestExport *entry=guest_exports;entry;entry=entry->next) {
        if (!strcmp(entry->name,name) && !main_only) {
            Word address=entry->address; pthread_mutex_unlock(&guest_export_mutex); return address;
        }
    }
    struct GuestExport *entry=calloc(1,sizeof(*entry));
    Word address=entry ? guest_find_export(name,main_only,entry->storage) : 0;
    if (address && !main_only) {
        entry->name=strdup(name);
        if (entry->name) { entry->address=address; entry->next=guest_exports; guest_exports=entry; }
        else { free(entry); address=0; }
    } else free(entry);
    pthread_mutex_unlock(&guest_export_mutex);
    if (!address) guest_library_fail("dlsym",name);
    else LOG("Darwin dynamic export resolved: %s",name);
    return address;
}

static int guest_dladdr(Word address,Dl_info *result) {
    if (!in_image(address)) return 0;
#ifdef I2A_GUEST_COLLECTION
    int image=guest_image_index(address);if(image<0)return 0;
    result->dli_fname=guest_image_name(image);
    result->dli_fbase=(void *)guest_image_header(image);
#else
    result->dli_fname=GUEST_IMAGE_PATH;
    result->dli_fbase=(void *)aslr;
#endif
    result->dli_sname=address==aslr ? "_mh_execute_header" : NULL;
    result->dli_saddr=address==aslr ? (void *)aslr : NULL;
    return 1;
}

static int android_open_flags(int flags) {
    int known=3|4|8|16|32|64|128|256|512|1024|2048|0x20000|0x100000|0x1000000;
    if ((flags&3)==3 || (flags&~known)) { errno=EINVAL; return -1; }
    int translated=(flags&3)|O_CLOEXEC;
    if (flags&4) translated|=O_NONBLOCK;
    if (flags&8) translated|=O_APPEND;
    if (flags&64) translated|=O_ASYNC;
    if (flags&128) translated|=O_SYNC;
    if (flags&256) translated|=O_NOFOLLOW;
    if (flags&512) translated|=O_CREAT;
    if (flags&1024) translated|=O_TRUNC;
    if (flags&2048) translated|=O_EXCL;
    if (flags&0x20000) translated|=O_NOCTTY;
    if (flags&0x100000) translated|=O_DIRECTORY;
    return translated;
}

__attribute__((used,noinline)) static int open_guest_file(const char *path,int flags,const unsigned char *arguments) {
    int translated=android_open_flags(flags);
    if (translated<0) return -1;
    if ((flags&(3|512|1024)) && !writable_sandbox_path(path)) return -1;
    uint32_t mode=0;
    if (flags&512) memcpy(&mode,arguments,sizeof(mode));
    int fd=io_open(path,translated,mode);
    if (fd>=0 && (flags&(16|32))) {
        int lock=(flags&32 ? LOCK_EX : LOCK_SH)|(flags&4 ? LOCK_NB : 0);
        if (flock(fd,lock)) { int code=errno; io_close(fd); errno=code; return -1; }
    }
    LOG("open %s fd=%d",path,fd);
    return fd;
}

__attribute__((naked)) static void darwin_open(void) { __asm__ volatile("mov x2,sp\n b open_guest_file"); }

__attribute__((used,noinline)) static int protected_open(const char *path,int flags,int protection_class,int protection_flags,const unsigned char *arguments) {
    if (protection_class<0 || protection_class>7 || protection_flags) { errno=ENOTSUP; return -1; }
    return open_guest_file(path,flags,arguments);
}
__attribute__((naked)) static void darwin_protected_open(void) { __asm__ volatile("mov x4,sp\n b protected_open"); }

__attribute__((used,noinline)) static int guest_fcntl(int fd,int command,const unsigned char *arguments) {
    if (command==1) return fcntl(fd,F_GETFD);
    if (command==3) {
        int flags=fcntl(fd,F_GETFL);
        if (flags<0) return -1;
        int guest=flags&O_ACCMODE;
        if (flags&O_NONBLOCK) guest|=4;
        if (flags&O_APPEND) guest|=8;
        if (flags&O_ASYNC) guest|=64;
        if (flags&O_SYNC) guest|=128;
        return guest;
    }
    int argument;
    memcpy(&argument,arguments,sizeof(argument));
    if (command==0) return bundle_fd_duplicate(fd,fcntl(fd,F_DUPFD,argument));
    if (command==2) return fcntl(fd,F_SETFD,argument);
    if (command==4) {
        int flags=android_open_flags(argument);
        return flags<0 ? -1 : fcntl(fd,F_SETFL,flags);
    }
    errno=ENOTSUP;
    return -1;
}
__attribute__((naked)) static void darwin_fcntl(void) { __asm__ volatile("mov x2,sp\n b guest_fcntl"); }

struct DarwinStatfs {
    uint32_t block_size; int32_t io_size;
    uint64_t blocks, free_blocks, available_blocks, files, free_files;
    int32_t fsid[2]; uint32_t owner, type, flags, subtype;
    char type_name[16], mounted_on[1024], mounted_from[1024];
    uint32_t reserved[8];
};
static void translate_statfs(const struct statfs *native,struct DarwinStatfs *output) {
    _Static_assert(sizeof(*output)==2168,"iOS statfs layout");
    struct statfs info=*native;
    memset(output,0,sizeof(*output));
    output->block_size=info.f_bsize; output->io_size=info.f_bsize;
    output->blocks=info.f_blocks; output->free_blocks=info.f_bfree; output->available_blocks=info.f_bavail;
    output->files=info.f_files; output->free_files=info.f_ffree;
    memcpy(output->fsid,&info.f_fsid,sizeof(output->fsid));
    output->owner=getuid();
    const char *name=info.f_type==0xef53 ? "ext4" : info.f_type==0x01021994 ? "tmpfs" : info.f_type==0xf2f52010 ? "f2fs" : "unknown";
    snprintf(output->type_name,sizeof(output->type_name),"%s",name);
}
static int guest_statfs(const char *path,struct DarwinStatfs *output) {
    if(!output){errno=EFAULT;return -1;}struct statfs info;if(statfs(path,&info))return -1;translate_statfs(&info,output);return 0;
}
static int guest_rmdir(const char *path) {
    return writable_sandbox_path(path) ? rmdir(path) : -1;
}
static int guest_mkdir(const char *path,mode_t mode) {
    return writable_sandbox_path(path) ? mkdir(path,mode) : -1;
}
static int guest_utimes(const char *path,const struct DarwinSocketTimeval *times) {
    if(!writable_sandbox_path(path))return -1;
    struct timeval converted[2];
    if(times)for(unsigned i=0;i<2;++i){
        if(times[i].microseconds<0 || times[i].microseconds>=1000000){errno=EINVAL;return -1;}
        converted[i]=(struct timeval){times[i].seconds,times[i].microseconds};
    }
    return utimes(path,times ? converted : NULL);
}
static int guest_fstatfs(int descriptor,struct DarwinStatfs *output) {
    if(!output){errno=EFAULT;return -1;}struct statfs info;if(fstatfs(descriptor,&info))return -1;translate_statfs(&info,output);return 0;
}

struct DarwinTimeval { int64_t seconds; int32_t microseconds, padding; };
static int guest_gettimeofday(struct DarwinTimeval *output,void *timezone) {
    if (timezone) { errno=EINVAL; return -1; }
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME,&value)) return -1;
    if (output) *output=(struct DarwinTimeval){value.tv_sec,value.tv_nsec/1000,0};
    return 0;
}
static int *guest_errno(void) { return &errno; }

struct DarwinStat {
    int32_t device;
    uint16_t mode, links;
    uint64_t inode;
    uint32_t uid, gid, special_device, padding;
    struct timespec access, modified, changed, birth;
    int64_t size, blocks;
    int32_t block_size, flags, generation, spare;
    int64_t reserved[2];
};

static void guest_stat_assign(const struct stat *info,struct DarwinStat *output){
    *output=(struct DarwinStat){.device=info->st_dev,.mode=info->st_mode,.links=info->st_nlink,.inode=info->st_ino,.uid=info->st_uid,.gid=info->st_gid,.special_device=info->st_rdev,.access=info->st_atim,.modified=info->st_mtim,.changed=info->st_ctim,.size=info->st_size,.blocks=info->st_blocks,.block_size=info->st_blksize};
}
static int guest_stat_path(const char *path,struct DarwinStat *output){struct stat info;if(io_stat(path,&info))return -1;guest_stat_assign(&info,output);return 0;}
static int guest_lstat_path(const char *path,struct DarwinStat *output){struct stat info;if(bundle_relative(path) ? io_stat(path,&info) : lstat(path,&info))return -1;guest_stat_assign(&info,output);return 0;}
static int guest_access(const char *path,int mode){if(!bundle_relative(path))return access(path,mode);if(mode&~(R_OK|W_OK|X_OK)){errno=EINVAL;return -1;}struct stat info;if(io_stat(path,&info))return -1;if((mode&W_OK) || ((mode&X_OK) && !S_ISDIR(info.st_mode))){errno=EACCES;return -1;}return 0;}
static int guest_fstat(int fd,struct DarwinStat *output) {
    _Static_assert(sizeof(*output)==144 && offsetof(struct DarwinStat,size)==96,"iOS stat64 layout");
    struct stat native;
    if (io_fstat(fd,&native)) return -1;
    guest_stat_assign(&native,output);
    return 0;
}

static void *guest_mmap(void *address,size_t length,int protection,int flags,int fd,int64_t offset) {
    int known=1|2|16|0x100|0x200|0x400|0x800|0x1000;
    if ((flags&~known) || (protection&~7) || (flags&3)==0 || (flags&3)==3) { errno=EINVAL; return MAP_FAILED; }
    int translated=flags&3;
    if (flags&16) translated|=MAP_FIXED;
    if (flags&0x1000) translated|=MAP_ANONYMOUS;
    void *result=io_mmap(address,length,protection,translated,fd,offset);
    LOG_LIMITED(8,"mmap size=%lu protection=%d flags=0x%x fd=%d result=%p",length,protection,flags,fd,result);
    return result;
}

static int guest_madvise(void *address,size_t length,int advice) {
    if (advice==5) advice=MADV_FREE;
    return madvise(address,length,advice);
}

static unsigned char *guest_digest(const char *algorithm,const void *input,uint32_t length,unsigned char *output,int expected) {
    static _Thread_local unsigned char fallback[64];
    if (!output) output=fallback;
    if (length>INT32_MAX) stop("Input too large",algorithm);
    JNIEnv *env=attached_environment(algorithm);
    jstring name=(*env)->NewStringUTF(env,algorithm);
    jbyteArray bytes=(*env)->NewByteArray(env,length);
    if (!name || !bytes) stop("Allocation failed",algorithm);
    if (length) (*env)->SetByteArrayRegion(env,bytes,0,length,input);
    jbyteArray result=(*env)->CallObjectMethod(env,activity,digest_method,name,bytes);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        stop("Digest error",algorithm);
    }
    if (!result || (*env)->GetArrayLength(env,result)!=expected) stop("Unexpected digest length",algorithm);
    (*env)->GetByteArrayRegion(env,result,0,expected,(jbyte *)output);
    (*env)->DeleteLocalRef(env,result);
    (*env)->DeleteLocalRef(env,bytes);
    (*env)->DeleteLocalRef(env,name);
    return output;
}

static unsigned char *guest_sha1(const void *input,uint32_t length,unsigned char *output) {
    return guest_digest("SHA-1",input,length,output,20);
}

static unsigned char *guest_sha256(const void *input,uint32_t length,unsigned char *output) {
    return guest_digest("SHA-256",input,length,output,32);
}
static unsigned char *guest_md5(const void *input,uint32_t length,unsigned char *output) { return guest_digest("MD5",input,length,output,16); }
static void guest_hmac(unsigned algorithm,const void *key,size_t key_length,const void *input,size_t length,void *output) {
    const char *names[]={"HmacSHA1","HmacMD5","HmacSHA256","HmacSHA384","HmacSHA512","HmacSHA224"};
    const unsigned sizes[]={20,16,32,48,64,28};
    if (algorithm>=COUNT(names) || key_length>INT32_MAX || length>INT32_MAX || !output || (!key && key_length) || (!input && length)) stop("Invalid HMAC arguments","CCHmac");
    JNIEnv *env=attached_environment("CCHmac"); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"hmac","(Ljava/lang/String;[B[B)[B");
    jstring name=(*env)->NewStringUTF(env,names[algorithm]); jbyteArray key_bytes=(*env)->NewByteArray(env,(jsize)key_length),data=(*env)->NewByteArray(env,(jsize)length);
    if (!name || !key_bytes || !data) stop("Allocation failed","CCHmac");
    if (key_length) (*env)->SetByteArrayRegion(env,key_bytes,0,(jsize)key_length,key);
    if (length) (*env)->SetByteArrayRegion(env,data,0,(jsize)length,input);
    jbyteArray digest=(*env)->CallObjectMethod(env,activity,method,name,key_bytes,data);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Native HMAC provider failed",names[algorithm]); }
    if (!digest || (*env)->GetArrayLength(env,digest)!=(jsize)sizes[algorithm]) stop("Invalid HMAC result",names[algorithm]);
    (*env)->GetByteArrayRegion(env,digest,0,(jsize)sizes[algorithm],output);
    (*env)->DeleteLocalRef(env,digest); (*env)->DeleteLocalRef(env,data); (*env)->DeleteLocalRef(env,key_bytes); (*env)->DeleteLocalRef(env,name); (*env)->DeleteLocalRef(env,cls);
}

#include "../frameworks/foundation/geometry.inc"
#include "../frameworks/foundation/crypto.inc"
#include "darwin/compression.inc"
static Word resolve_basic_import(const char *name) {
    if (!strcmp(name,"_objc_opt_class")) return (Word)objc_opt_class;
    if (!strcmp(name,"_objc_opt_self")) return (Word)objc_opt_self;
    if (!strcmp(name,"_objc_opt_new")) return (Word)objc_opt_new;
    if (!strcmp(name,"_objc_opt_isKindOfClass")) return (Word)objc_opt_kind;
    if (!strcmp(name,"_objc_opt_respondsToSelector")) return (Word)objc_opt_responds;
    if (!strcmp(name,"_objc_alloc")) return (Word)objc_alloc_bridge;
    if (!strcmp(name,"_objc_allocWithZone")) return (Word)objc_alloc_zone_bridge;
    if (!strcmp(name,"_objc_alloc_init")) return (Word)objc_alloc_init_bridge;
    if (!strcmp(name,"_fstatfs")) return (Word)guest_fstatfs;
    if (!strcmp(name,"_rmdir")) return (Word)guest_rmdir;
    if (!strcmp(name,"_mkdir")) return (Word)guest_mkdir;
    if (!strcmp(name,"_utimes")) return (Word)guest_utimes;
    if (!strcmp(name,"_pwrite")) return (Word)pwrite;
    if (!strcmp(name,"_fsync")) return (Word)fsync;
    if (!strcmp(name,"_fdatasync")) return (Word)fdatasync;
    if (!strcmp(name,"_ftruncate")) return (Word)ftruncate;
    if (!strcmp(name,"_arc4random_buf")) return (Word)arc4random_buf;
    return 0;
}
static Word resolve_import(unsigned index) {
    const char *name = symbols[index];
    Word basic=resolve_basic_import(name);if(basic)return basic;
    Word selector=resolve_selector_import(name);if(selector)return selector;
    Word compression=resolve_compression_import(name);if(compression)return compression;
    Word exception_import=resolve_exception_import(index);
    if (exception_import) return exception_import;
    Word geometry=geometry_export(name); if (geometry) return geometry;
    Word accessibility=resolve_accessibility_import(name); if(accessibility)return accessibility;
    Word crypto=resolve_crypto_import(name); if (crypto) return crypto;
    Word http_message=resolve_http_message_import(name); if (http_message) return http_message;
    Word stream=resolve_stream_import(name);if(stream)return stream;
    Word runloop_source=resolve_runloop_source_import(name); if (runloop_source) return runloop_source;
    Word uniform_type=resolve_uniform_type_import(name); if (uniform_type) return uniform_type;
    Word socket_import=resolve_socket_import(name);
    if (socket_import) return socket_import;
    Word constant=resolve_constant(index);
    if (constant) return constant;
    if (!strcmp(name,"_CFRunLoopGetMain")) return (Word)core_main_runloop;
    if (!strcmp(name,"_CFRunLoopGetCurrent")) return (Word)core_current_runloop;
    Word atomic_import=resolve_atomic_import(name);
    if (atomic_import) return atomic_import;
    Word thread_import=resolve_thread_import(name);
    if (thread_import) return thread_import;
    Word trace_import=resolve_backtrace_import(name);
    if (trace_import) return trace_import;
    Word system_import=resolve_libsystem_import(name);
    if (system_import) return system_import;
    Word malloc_import=resolve_malloc_zone_import(name);
    if (malloc_import) return malloc_import;
    Word cpp_import=resolve_cpp_import(name);
    if (cpp_import) return cpp_import;
    Word audio_import=resolve_audio_unit_import(name);
    if (audio_import) return audio_import;
    Word file_audio_import=resolve_audio_file_import(name); if (file_audio_import) return file_audio_import;
    Word queue_audio_import=resolve_audio_queue_import(name); if (queue_audio_import) return queue_audio_import;
    Word sound_import=resolve_audio_services_import(name);
    if (sound_import) return sound_import;
    Word semaphore_import=resolve_semaphore_import(name);
    if (semaphore_import) return semaphore_import;
    Word kernel_import=resolve_kernel_import(name);
    if (kernel_import) return kernel_import;
    Word dyld_import=resolve_dyld_import(name);
    if (dyld_import) return dyld_import;
    Word signal_import=resolve_signal_import(name);
    if (signal_import) return signal_import;
    Word core_import=resolve_core_import(name);
    if (core_import) return core_import;
    Word notification_import=resolve_notification_import(name);
    if (notification_import) return notification_import;
    Word reachability_import=resolve_reachability_import(name);
    if (reachability_import) return reachability_import;
    Word interfaces_import=resolve_interfaces_import(name);
    if (interfaces_import) return interfaces_import;
    Word objc_import=resolve_objc_import(name);
    if (objc_import) return objc_import;
    Word dynamic_import=resolve_dynamic_import(name);
    if (dynamic_import) return dynamic_import;
    Word stdio_import=resolve_stdio_import(name);
    if (stdio_import) return stdio_import;
    Word logging_import=resolve_logging_import(name);
    if (logging_import) return logging_import;
    Word sqlite_import=resolve_sqlite_import(name);
    if (sqlite_import) return sqlite_import;
    Word keychain_import=resolve_keychain_import(name);
    if (keychain_import) return keychain_import;
    Word rsa_import=resolve_rsa_import(name);
    if (rsa_import) return rsa_import;
    Word tls_import=resolve_secure_transport_import(name);
    if (tls_import) return tls_import;
    if (!strcmp(name,"_MTLCreateSystemDefaultDevice")) return (Word)metal_default_device;
    if (!strcmp(name,"_NSLog")) return (Word)darwin_nslog;
    if (!strcmp(name,"_NSSearchPathForDirectoriesInDomains")) return (Word)search_directories;
    if (!strcmp(name,"_NSTemporaryDirectory")) return (Word)temporary_directory;
    if (!strcmp(name,"_NSHomeDirectory")) return (Word)home_directory;
    if (!strcmp(name,"_gettimeofday")) return (Word)guest_gettimeofday;
    if (!strcmp(name,"_inet_addr")) return (Word)inet_addr;
    if (!strcmp(name,"_gethostbyname")) return (Word)gethostbyname;
    if (!strcmp(name,"___error")) return (Word)guest_errno;
    const char *const file_constants[]={"_NSFileCreationDate","_NSFileImmutable","_NSFilePathErrorKey","_NSFilePosixPermissions",
        "_NSFileSize","_NSFileSystemFreeSize","_NSFileSystemSize"};
    for (unsigned pos=0;pos<COUNT(file_constants);++pos) {
        if (!strcmp(name,file_constants[pos])) {
            symbol_objects[index][0]=copy_string(name+1);
            return (Word)symbol_objects[index];
        }
    }
    if (!strcmp(name,"_UIImagePNGRepresentation")) return (Word)image_png;
    if (!strcmp(name,"_UIImageWriteToSavedPhotosAlbum")) return (Word)image_write_album;
    if (!strcmp(name,"_objc_enumerationMutation")) return (Word)enumeration_mutation;
    if (!strcmp(name,"_SecCertificateCreateWithData")) return (Word)certificate_create;
    if (!strcmp(name,"_SecCertificateCopyData")) return (Word)certificate_copy_data;
    if (!strcmp(name,"_SecTrustGetCertificateCount")) return (Word)trust_certificate_count;
    if (!strcmp(name,"_SecTrustGetCertificateAtIndex")) return (Word)trust_certificate_at;
    if (!strcmp(name,"_SecTrustSetAnchorCertificates")) return (Word)trust_anchors;
    if (!strcmp(name,"_SecTrustSetAnchorCertificatesOnly")) return (Word)trust_only_anchors;
    if (!strcmp(name,"_SecTrustEvaluate")) return (Word)trust_evaluate;
    if (!strcmp(name,"_sysctlbyname")) return (Word)system_control;
    if (!strcmp(name,"_objc_initWeak") || !strcmp(name,"_objc_storeWeak")) return (Word)store_weak;
    if (!strcmp(name,"_objc_loadWeak")) return (Word)load_weak;
    if (!strcmp(name,"_objc_loadWeakRetained")) return (Word)load_weak_retained;
    if (!strcmp(name,"_objc_moveWeak")) return (Word)move_weak;
    if (!strcmp(name,"_objc_destroyWeak")) return (Word)destroy_weak;
    if (!strcmp(name,"_CC_SHA1")) return (Word)guest_sha1;
    if (!strcmp(name,"_CC_SHA256")) return (Word)guest_sha256;
    if (!strcmp(name,"_CC_MD5")) return (Word)guest_md5;
    if (!strcmp(name,"_CCHmac")) return (Word)guest_hmac;
    if (!strcmp(name,"_NXGetLocalArchInfo")) return (Word)local_architecture;
    if (!strcmp(name,"_NXGetArchInfoFromCpuType")) return (Word)architecture_for_cpu;
    if (!strcmp(name,"_dlsym")) return (Word)guest_symbol;
    if (!strcmp(name,"_dladdr")) return (Word)guest_dladdr;
    if (!strcmp(name,"_open")) return (Word)darwin_open;
    if (!strcmp(name,"_open_dprotected_np")) return (Word)darwin_protected_open;
    if (!strcmp(name,"_statfs")) return (Word)guest_statfs;
    if (!strcmp(name,"_fcntl")) return (Word)darwin_fcntl;
    if (!strcmp(name,"_fstat")) return (Word)guest_fstat;
    if (!strcmp(name,"_stat")) return (Word)guest_stat_path;
    if (!strcmp(name,"_lstat")) return (Word)guest_lstat_path;
    if (!strcmp(name,"_access")) return (Word)guest_access;
    if (!strcmp(name,"_read")) return (Word)io_read;
    if (!strcmp(name,"_pread")) return (Word)io_pread;
    if (!strcmp(name,"_readv")) return (Word)io_readv;
    if (!strcmp(name,"_lseek")) return (Word)io_seek;
    if (!strcmp(name,"_dup")) return (Word)io_dup;
    if (!strcmp(name,"_dup2")) return (Word)io_dup2;
    if (!strcmp(name,"_mmap")) return (Word)guest_mmap;
    if (!strcmp(name,"_madvise")) return (Word)guest_madvise;
    if (!strcmp(name,"_munmap")) return (Word)munmap;
    if (!strcmp(name,"_close")) return (Word)io_close;
    if (!strcmp(name,"_objc_msgSendSuper2")) return (Word)message_super2;
    if (!strcmp(name,"_objc_retainAutoreleaseReturnValue")) return (Word)metal_retain_autorelease_return_value;
    if (!strcmp(name,"_objc_unsafeClaimAutoreleasedReturnValue")) return (Word)metal_unsafe_claim_autoreleased;
    if (!strcmp(name,"_UIApplicationMain")) return (Word)application_main;
    if (!strcmp(name,"_objc_storeStrong")) return (Word)store_strong;
    if (!strcmp(name,"_objc_getProperty")) return (Word)get_property;
    if (!strcmp(name,"_objc_setProperty_atomic")) return (Word)set_atomic_property;
    if (!strcmp(name,"_objc_setProperty_nonatomic")) return (Word)set_property;
    if (!strcmp(name,"_objc_retainAutorelease")) return (Word)metal_retain_autorelease;
    if (!strcmp(name,"_objc_autoreleasePoolPush")) return (Word)pool_push;
    if (!strcmp(name,"_objc_autoreleasePoolPop")) return (Word)pool_pop;
    if (!strcmp(name,"_NSStringFromClass")) return (Word)string_from_class;
    if (!strcmp(name,"_bzero")) return (Word)zero_bytes;
    if (!strcmp(name,"_mach_timebase_info")) return (Word)timebase_info;
    if (!strcmp(name,"_mach_absolute_time")) return (Word)absolute_time;
    if (!strcmp(name,"_pthread_mutex_init")) return (Word)guest_mutex_init;
    if (!strcmp(name,"_pthread_rwlock_init")) return (Word)guest_rwlock_init;
    if (!strcmp(name,"_memset_pattern16")) return (Word)pattern16;
    if (!strcmp(name,"_memset_pattern4")) return (Word)pattern4;
    if (!strncmp(name,"__Zn",4) || !strncmp(name,"__Zd",4) || !strcmp(name,"__ZSt7nothrow") || !strncmp(name,"___cxa_guard_",13) ||
        !strcmp(name,"__ZSt13set_terminatePFvvE") || !strcmp(name,"__ZSt13get_terminatev") || !strcmp(name,"__ZSt9terminatev") ||
        !strcmp(name,"__ZSt14set_unexpectedPFvvE") || !strcmp(name,"__ZSt14get_unexpectedv") || !strcmp(name,"__ZSt18uncaught_exceptionv")) {
        static void *cxx;
        if (!cxx) cxx=dlopen("libc++_shared.so",RTLD_NOW|RTLD_LOCAL);
        if (!cxx) stop("Loading NDK libc++",dlerror());
        void *target=dlsym(cxx,name+1);
        if (target) return (Word)target;
    }
    if (!strcmp(name,"_asl_open")) return (Word)asl_open_bridge;
    if (!strcmp(name,"_asl_set_filter")) return (Word)asl_filter_bridge;
    if (!strcmp(name,"_asl_log")) return (Word)asl_darwin_log;
    if (!strcmp(name,"_asl_add_output_file")) return (Word)asl_add_fd;
    if (!strcmp(name,"_class_getInstanceMethod")) return (Word)instance_method;
    if (!strcmp(name,"_method_getImplementation")) return (Word)get_imp;
    if (!strcmp(name,"_method_setImplementation")) return (Word)set_imp;
    if (!strcmp(name,"_NSClassFromString")) return (Word)class_from_string;
    if (!strcmp(name,"___CFConstantStringClassReference")) return external_class("_OBJC_CLASS_$_NSString");
    if (!strcmp(name,"_kCFAllocatorDefault")) return (Word)&allocator_default;
    if (!strcmp(name,"_CFUUIDCreate")) return (Word)create_uuid;
    if (!strcmp(name,"_CFUUIDCreateString")) return (Word)uuid_string;
    if (!strcmp(name,"_CFRelease")) return (Word)cf_release;
    if (!strcmp(name,"_NSFileProtectionKey") || !strcmp(name,"_NSFileProtectionNone")) {
        symbol_objects[index][0]=copy_string(name+1);
        return (Word)symbol_objects[index];
    }
    if (!strcmp(name,"_NSCalendarIdentifierGregorian") || !strcmp(name,"_NSGregorianCalendar")) {
        symbol_objects[index][0]=copy_string("gregorian");
        return (Word)symbol_objects[index];
    }
    if (!strncmp(name,"_dispatch_",10) || !strncmp(name,"__dispatch_",11)) {
        void *target=dispatch_export(name+1);
        if (target) return (Word)target;
    }
    if (!strcmp(name,"__NSConcreteStackBlock")) return (Word)_NSConcreteStackBlock;
    if (!strcmp(name,"__NSConcreteGlobalBlock")) return (Word)_NSConcreteGlobalBlock;
    if (!strcmp(name,"__Block_object_assign")) return (Word)_Block_object_assign;
    if (!strcmp(name,"__Block_object_dispose")) return (Word)_Block_object_dispose;
    if (!strcmp(name,"_objc_retainBlock")) return (Word)_Block_copy;
    if (!strcmp(name, "_objc_msgSend")) return (Word)message_send;
    if (!strcmp(name, "___cxa_atexit")) return (Word)guest_atexit;
    if (!strcmp(name, "___stack_chk_guard")) return (Word)&global_guard;
    if (!strcmp(name,"_objc_retainAutoreleasedReturnValue")) return (Word)metal_claim_autoreleased;
    if (!strcmp(name,"_objc_retain")) return (Word)metal_object_retain;
    if (!strcmp(name,"_objc_autoreleaseReturnValue")) return (Word)metal_autorelease_return_value;
    if (!strcmp(name,"_objc_release")) return (Word)metal_object_release;
    const char *const compatible[] = {"malloc","calloc","realloc","free","memcpy","memset","memmove","memcmp","memchr",
                                      "strlen","strnlen","strcpy","strncpy","strcmp","strncmp","strdup","strchr","strrchr","strstr",
                                      "strcat","strncat","strlcpy","strspn","strpbrk","strtok","strtok_r","strtod","strtof","strtol","strtoul","strtoll","strtoull","strcoll",
                                      "atoi","atol","atof","strerror","qsort","bsearch","getenv","rand","srand","drand48","arc4random","arc4random_uniform",
                                      "__memcpy_chk","__memmove_chk","__memset_chk","__strcpy_chk","__strncpy_chk","__strcat_chk","__strncat_chk","__strlcpy_chk",
                                      "getpid","getppid","getuid","geteuid","write","remove","rename","unlink",
                                      "fopen","fclose","fread","fwrite","fflush","fseek","fseeko","ftell","ftello",
                                      "clearerr","feof","ferror","fgets","getc","ungetc","puts","rewind","tmpfile","setvbuf","dup","mkstemp",
                                      "time","difftime","clock","gmtime_r","localtime","localtime_r","mktime","strftime","usleep",
                                      "deflate","deflateEnd","deflateInit2_","inflate","inflateEnd","inflateInit2_","gzopen","gzclose","gzwrite","gzerror",
                                      "mprotect","getpagesize","posix_memalign",
                                      "acos","acosf","asin","asinf","atan","atanf","atan2","atan2f","cos","cosf","cosh","sin","sinf","sinh","tan","tanf","tanh",
                                      "sqrt","sqrtf","pow","powf","exp","expf","exp2","exp2f","log","logf","log2","log2f","log10","log10f",
                                      "ceil","ceilf","floor","floorf","round","roundf","lroundf","llrint","fabs","fabsf","fmod","fmodf","modf","frexp","frexpf","ldexp","ldexpf","nextafter",
                                      "abort","exit","__stack_chk_fail"};
    for (unsigned pos=0;pos<COUNT(compatible);++pos) {
        if (!strcmp(name+1,compatible[pos])) {
            void *symbol = dlsym(RTLD_DEFAULT,compatible[pos]);
            if (symbol) return (Word)symbol;
        }
    }
    if (functions[index]) return (Word)guest_gates + 8*index;
    if (!strncmp(name,"_OBJC_CLASS_$_",14) || !strncmp(name,"_OBJC_METACLASS_$_",18)) return (Word)symbol_objects[index];
    if (strstr(name,"Notification") && strncmp(name,"_OBJC_",6)) {
        symbol_objects[index][0]=(Word)&symbol_objects[index][1];
        return (Word)symbol_objects[index];
    }
    return 0;
}

static Word guest_find_export(const char *name,int main_only,Word storage[4]) {
    if (!strncmp(name,"OBJC_CLASS_$_",13) || !strncmp(name,"OBJC_METACLASS_$_",17)) {
        int meta=!strncmp(name,"OBJC_METACLASS_$_",17);
        Word cls=class_from_name(name+(meta ? 17 : 13));
        if (main_only && cls && !in_image(cls) && !dynamic_class(cls)) return 0;
        return cls && meta ? read_word(cls) : cls;
    }
    if (main_only) return 0;
    char imported[192]; snprintf(imported,sizeof(imported),"_%s",name);
    for (unsigned index=0;index<COUNT(symbols);++index) {
        if (strcmp(symbols[index],imported)) continue;
        Word address=import_addresses[index];
        if (address && (!functions[index] || address!=(Word)guest_gates+8*index)) return address;
        break;
    }
    Word constant=constant_export(imported,storage);
    if (constant) return constant;
    Word (*const resolvers[])(const char *)={
        resolve_basic_import,resolve_selector_import,resolve_compression_import,
        geometry_export,resolve_accessibility_import,resolve_crypto_import,resolve_core_import,resolve_keychain_import,resolve_rsa_import,resolve_secure_transport_import,
        resolve_atomic_import,
        resolve_audio_unit_import,resolve_audio_file_import,resolve_audio_queue_import,resolve_audio_services_import,resolve_objc_import,resolve_dynamic_import,
        resolve_http_message_import,resolve_stream_import,resolve_runloop_source_import,resolve_uniform_type_import,resolve_reachability_import,
        resolve_notification_import,resolve_interfaces_import,resolve_socket_import,resolve_thread_import,
        resolve_libsystem_import,resolve_malloc_zone_import,resolve_cpp_import,resolve_semaphore_import,
        resolve_kernel_import,resolve_dyld_import,resolve_signal_import,resolve_stdio_import,resolve_logging_import,resolve_sqlite_import
    };
    for (unsigned i=0;i<COUNT(resolvers);++i) { Word address=resolvers[i](imported); if (address) return address; }
    return 0;
}

#include "darwin/image_mapping.inc"
static void fixups(void) {
     
    const Block_callbacks_RR callbacks={sizeof(callbacks),metal_block_retain,metal_block_release,metal_block_destruct};
    _Block_use_RR2(&callbacks);
    aslr = (Word)guest_text;
    if ((Word)guest_data-aslr != GUEST_DATA_OFFSET || (Word)guest_links-aslr != GUEST_LINKS_OFFSET) stop("Invalid ELF", "segment layout");
    for (unsigned i=0;i<COUNT(guest_segments);++i) {
        const struct I2AImageSegment *segment=guest_segments+i;
        if ((Word)segment->address-aslr!=segment->offset) stop("Invalid ELF","mapped segment layout");
    }
    protect_guest_images(1);
    arc4random_buf(&global_guard,sizeof(global_guard));
    for (unsigned index=0;index<REBASE_COUNT;++index) {
        const unsigned char *record=rebases+12*index;
        uint32_t offset;
        memcpy(&offset,record,4);
        const struct I2AImageSegment *segment=image_segment_for(aslr+offset,8);
        if (!segment || !(segment->maximum&PROT_WRITE)) stop("Invalid image relocation","rebase target");
        Word pointer = aslr+read_word((Word)record+4);
        memcpy(guest_text+offset,&pointer,8);
    }
#ifdef I2A_GUEST_COLLECTION
    for (unsigned i=0;i<COUNT(guest_absolute_binds);++i) {
        Word address=aslr+guest_absolute_binds[i].offset;
        const struct I2AImageSegment *segment=image_segment_for(address,8);
        if (!segment || !(segment->maximum&PROT_WRITE)) stop("Invalid image relocation","absolute binding target");
        memcpy((void *)address,&guest_absolute_binds[i].value,8);
    }
#endif
    Word *addresses=import_addresses;
    for (unsigned index=0;index<COUNT(symbols);++index) addresses[index]=resolve_import(index);
    decimal_number_initialize();
#if I2A_DEVELOPMENT
    char inventory_path[PATH_MAX];
    snprintf(inventory_path,sizeof(inventory_path),"%s/compatibility-imports.tsv",files_directory);
    JNIEnv *inventory_env=attached_environment("Import inventory");
    jclass inventory_class=(*inventory_env)->GetObjectClass(inventory_env,activity);
    jmethodID inventory_directory=(*inventory_env)->GetMethodID(inventory_env,inventory_class,"compatibilityDirectory","()Ljava/lang/String;");
    jstring inventory_folder=(*inventory_env)->CallObjectMethod(inventory_env,activity,inventory_directory);
    if (!(*inventory_env)->ExceptionCheck(inventory_env) && inventory_folder) {
        const char *folder=(*inventory_env)->GetStringUTFChars(inventory_env,inventory_folder,NULL);
        if (folder) { snprintf(inventory_path,sizeof(inventory_path),"%s/compatibility-imports.tsv",folder); (*inventory_env)->ReleaseStringUTFChars(inventory_env,inventory_folder,folder); }
    } else (*inventory_env)->ExceptionClear(inventory_env);
    if (inventory_folder) (*inventory_env)->DeleteLocalRef(inventory_env,inventory_folder);
    (*inventory_env)->DeleteLocalRef(inventory_env,inventory_class);
    FILE *inventory=fopen(inventory_path,"w");
    unsigned missing_functions=0,missing_data=0;
    if (inventory) fputs("symbol\tkind\tbinding\n",inventory);
    for (unsigned index=0;index<COUNT(symbols);++index) {
        int missing=functions[index] ? addresses[index]==(Word)guest_gates+8*index : addresses[index]==0;
        if (missing) { if (functions[index]) ++missing_functions; else ++missing_data; }
        const char *kind=functions[index] ? "stub-function" : !strncmp(symbols[index],"_OBJC_",6) ? "objc-identity" : "data-or-indirect";
        if (inventory) fprintf(inventory,"%s\t%s\t%s\n",symbols[index],kind,missing ? "missing" : "bound");
    }
    if (inventory) fclose(inventory);
    LOG("Import preflight: %u unresolved stub functions, %u other unresolved symbols (bound does not imply full API semantics)",missing_functions,missing_data);
#endif
    for (unsigned index=0;index<BIND_COUNT;++index) {
        const uint32_t *record=(const uint32_t *)(binds+16*index);
        const struct I2AImageSegment *segment=image_segment_for(aslr+record[0],8);
        if (!segment || !(segment->maximum&PROT_WRITE)) stop("Invalid image relocation","binding target");
        Word pointer = addresses[record[1]]+read_word((Word)record+8);
        memcpy(guest_text+record[0],&pointer,8);
    }
    protect_guest_images(0);
    LOG("Mach-O loaded with %zu mapped segments, %d rebases, %d bindings (guest base=%p)",COUNT(guest_segments),REBASE_COUNT,BIND_COUNT,(void *)aslr);
}

static void run_loads(unsigned classes_first,unsigned classes_count,unsigned categories_first,unsigned categories_count) {
    for (unsigned kind=0;kind<2;++kind) {
        const Word *entries=(Word *)(aslr+(kind?LOAD_CATEGORIES_OFFSET:LOAD_CLASSES_OFFSET));
        unsigned first=kind?categories_first:classes_first,count=kind?categories_count:classes_count;
        for (unsigned index=first;index<first+count;++index) {
            Word entry=entries[index];
            Word cls=kind?read_word(entry+8):entry;
            if(in_image(cls))realize_class_layout(cls,0);
            Word ro=kind?0:(read_word(read_word(cls)+32)&~(Word)7);
            Method load=method_list(kind?read_word(entry+24):read_word(ro+32),"load");
            if (load) {
                const char *name=kind?(const char *)read_word(entry):class_name(cls);
                snprintf(stage,sizeof(stage),"+[%s load] 0x%lx",name,guest_native_address((Word)load));
                LOG("%s",stage);
                load(cls,"load",0,0,0,0);
            }
        }
    }
}

#if I2A_DEVELOPMENT
#include "../diagnostics/interface_audit.inc"
#else
static void run_interface_preflight(void) {}
#endif
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_startGuest)(JNIEnv *env,jobject instance,jstring files_path) {
    if (!started && setjmp(boundary)==0) {
        may_unwind=1;
        (*env)->GetJavaVM(env,&java_vm);
        activity=(*env)->NewGlobalRef(env,instance);
        jclass host_type=(*env)->GetObjectClass(env,instance);
        jmethodID assets_method=(*env)->GetMethodID(env,host_type,"getAssets","()Landroid/content/res/AssetManager;");
        jobject assets=(*env)->CallObjectMethod(env,instance,assets_method);
        bundle_assets_owner=(*env)->NewGlobalRef(env,assets);bundle_assets=AAssetManager_fromJava(env,assets);
        jmethodID apk_method=(*env)->GetMethodID(env,host_type,"installedApkPath","()Ljava/lang/String;");jstring apk_path=(*env)->CallObjectMethod(env,instance,apk_method);
        if(apk_path){const char *apk=(*env)->GetStringUTFChars(env,apk_path,NULL);if(apk){stat(apk,&bundle_archive_info);(*env)->ReleaseStringUTFChars(env,apk_path,apk);}(*env)->DeleteLocalRef(env,apk_path);}
        (*env)->DeleteLocalRef(env,assets);(*env)->DeleteLocalRef(env,host_type);
        if(!bundle_assets)stop("Asset manager unavailable","Android bundle");
        preference_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"preference","(Ljava/lang/String;I)I");
        bundle_file_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"bundleFile","(Ljava/lang/String;)Ljava/lang/String;");
        bundle_exists_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"bundleExists","(Ljava/lang/String;)Z");
        digest_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"digest","(Ljava/lang/String;[B)[B");
        certificate_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"certificate","([B)Ljava/security/cert/X509Certificate;");
        trust_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"validateTrust","([Ljava/security/cert/X509Certificate;[Ljava/security/cert/X509Certificate;Z)Z");
        system_text_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"systemText","(Ljava/lang/String;)Ljava/lang/String;");
        schedule_operation_method=(*env)->GetMethodID(env,(*env)->GetObjectClass(env,instance),"scheduleOperation","(JZ)V");
        started=1;
        snprintf(stage,sizeof(stage),"%s (%u)",GUEST_APP_NAME,GUEST_APP_BUILD);
        const char *path=(*env)->GetStringUTFChars(env,files_path,NULL);
        if (!path) return NULL;
        files_directory=strdup(path);
        (*env)->ReleaseStringUTFChars(env,files_path,path);
        if (!files_directory) stop("Allocation failed","Android files directory");
        fixups();
        realize_class_layouts();
        initialize_helpshift_bridge();
        initialize_extension();
        initialize_dispatch_loop();
        Word *inits=(Word *)(aslr+INIT_OFFSET);
        char *argv[]={GUEST_EXECUTABLE,NULL};
        char *empty[]={NULL};
#ifdef I2A_GUEST_COLLECTION
        for(unsigned image=0;image<COUNT(guest_initialization_order);++image){
            LOG("Initializing guest image: %s",guest_image_name(guest_initialization_order[image].image));
            run_loads(guest_initialization_order[image].classes_first,guest_initialization_order[image].classes_count,
                      guest_initialization_order[image].categories_first,guest_initialization_order[image].categories_count);
            unsigned first=guest_initialization_order[image].init_first,count=guest_initialization_order[image].init_count;
            for(unsigned index=first;index<first+count;++index){
                snprintf(stage,sizeof(stage),"Initializer %u/%u: %s",index-first+1,count,guest_image_name(guest_initialization_order[image].image));
                LOG("%s",stage);
                ((void (*)(int,char **,char **,char **))(inits[index]))(1,argv,empty,empty);
            }
        }
#else
        run_loads(0,LOAD_CLASSES_COUNT,0,LOAD_CATEGORIES_COUNT);
        for (unsigned index=0;index<INIT_COUNT;++index) {
            snprintf(stage,sizeof(stage),"Initializer %u/%d at 0x%lx",index+1,INIT_COUNT,inits[index]-aslr+GUEST_IMAGE_BASE);
            ((void (*)(int,char **,char **,char **))(inits[index]))(1,argv,empty,empty);
        }
#endif
        snprintf(stage,sizeof(stage),"LC_MAIN at 0x%lx",GUEST_IMAGE_BASE+ENTRY);
        LOG("%s",stage);
        int code=((int (*)(int,char **))(aslr+ENTRY))(1,argv);
        snprintf(status,sizeof(status),"Guest exited with code %d",code);
    }
    may_unwind=0;
    if (application_ready) return NULL;
    return (*env)->NewStringUTF(env,status);
}

#if I2A_HAS_EXTENSION
static struct I2ADisplay extension_display(void) {
    struct DisplayState state=display_snapshot();
    return (struct I2ADisplay){state.width,state.height,state.scale};
}
static Word extension_first_responder(void) { return text_first_responder; }
static Word extension_empty_products(Word identifiers) {
    return store_empty_products(identifiers);
}
#endif
static void initialize_extension(void) {
#if I2A_HAS_EXTENSION
    static struct I2ARuntimeAPI api;
    api=(struct I2ARuntimeAPI){I2A_RUNTIME_INTERFACE,sizeof(api),aslr,get_root,
        extension_first_responder,extension_display,class_name,class_from_name,find_implementation,
        attributed_string,copy_string,extension_empty_products,stop};
    const struct I2AExtension *selected=i2a_extension_initialize(&api);
    if (!selected || selected->version!=I2A_RUNTIME_INTERFACE || selected->size!=sizeof(*selected))
        stop("Incompatible external runtime interface","extension");
    runtime_extension=*selected;
#endif
}
