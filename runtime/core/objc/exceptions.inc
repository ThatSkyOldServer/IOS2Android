/* ObjC exceptions use the same zero-cost C++ exception ABI on Darwin and
 * Android. The guest's LSDA/landing pads are retained by guest_unwind.py.
 * objc4's objc_typeinfo has libc++abi's __shim_type_info vtable shape:
 * two destructors, two reserved slots, then can_catch. Its matcher adjusts
 * the thrown payload to the ObjC object before entering a typed catch. */
#include <stddef.h>
#include <unwind.h>

struct GuestExceptionType { const Word *vtable; const char *name; Word cls; };
struct GuestThrownException { Word object; struct GuestExceptionType type; };
struct GuestExceptionTrace { int count; void *frames[64]; Word addresses,symbols; };
static const Word exception_type_vtable[13];
static struct GuestExceptionType exception_id_type={exception_type_vtable+2,"id",0};
static void *(*exception_allocate)(size_t);
static void (*exception_throw_native)(void *,void *,void (*)(void *));
static void *(*exception_begin_native)(void *);
static struct GuestExceptionType *(*exception_current_type)(void);
typedef void (*GuestTerminateHandler)(void);
static GuestTerminateHandler exception_previous_terminate;
static pthread_once_t exception_abi_once=PTHREAD_ONCE_INIT;
static _Thread_local unsigned exception_terminating;

static Word exception_abi_symbol(const char *name) {
    Word result=cpp_export(name);
    if (!result) stop("Missing C++ exception ABI",name);
    return result;
}
static _Bool exception_type_catch(const struct GuestExceptionType *caught,
                                 const struct GuestExceptionType *thrown,void **adjusted) {
    if (!thrown || thrown->vtable!=exception_type_vtable+2 || !adjusted || !*adjusted) return 0;
    Word object=((struct GuestThrownException *)*adjusted)->object;
    if (caught!=&exception_id_type && (!caught->cls || !object || !is_kind_of(object,"exception type",caught->cls))) return 0;
    *adjusted=(void *)object;
    return 1;
}
static _Bool exception_type_false(void) { return 0; }
static __attribute__((noreturn)) void exception_type_invalid(void) { stop("Invalid exception RTTI operation","Objective-C typeinfo"); }
static const Word exception_type_vtable[13]={
    0,(Word)&exception_id_type,(Word)noop,(Word)noop,(Word)noop,(Word)noop,
    (Word)exception_type_catch,(Word)exception_type_false,(Word)exception_type_false,
    (Word)exception_type_invalid,(Word)exception_type_invalid,(Word)exception_type_invalid,(Word)exception_type_invalid
};

static __attribute__((noreturn)) void exception_uncaught_terminate(void) {
    struct GuestExceptionType *type=exception_current_type ? exception_current_type() : NULL;
    if (!exception_terminating++ && type && type->vtable==exception_type_vtable+2) {
        struct GuestThrownException *thrown=(void *)((unsigned char *)type- offsetof(struct GuestThrownException,type));
        Word object=thrown->object,handler=guest_get_uncaught_exception_handler();
        if (object && read_word(object)==external_class("_OBJC_CLASS_$_NSException")) {
            struct HostObject *value=(void *)object;
            record_failure(value->content[0] ? string_bytes(value->content[0],"exception name") : "NSException",
                value->content[1] ? string_bytes(value->content[1],"exception reason") : "Uncaught Objective-C exception");
            __android_log_print(ANDROID_LOG_ERROR,"I2AGuest","Uncaught Objective-C exception %s: %s",
                value->content[0] ? string_bytes(value->content[0],"exception name") : "NSException",
                value->content[1] ? string_bytes(value->content[1],"exception reason") : "");
        }
        if (handler) ((void (*)(Word))handler)(object);
    }
    if (exception_previous_terminate) exception_previous_terminate();
    abort();
}
static void exception_initialize_abi(void) {
    exception_allocate=(void *(*)(size_t))exception_abi_symbol("__cxa_allocate_exception");
    exception_throw_native=(void (*)(void *,void *,void (*)(void *)))exception_abi_symbol("__cxa_throw");
    exception_begin_native=(void *(*)(void *))exception_abi_symbol("__cxa_begin_catch");
    exception_current_type=(struct GuestExceptionType *(*)(void))exception_abi_symbol("__cxa_current_exception_type");
    GuestTerminateHandler (*set_terminate)(GuestTerminateHandler)=(void *)exception_abi_symbol("_ZSt13set_terminatePFvvE");
    exception_previous_terminate=set_terminate(exception_uncaught_terminate);
}
static void exception_release_payload(void *payload) {
    Word object=((struct GuestThrownException *)payload)->object;
    Method release=find_implementation(object,"release");
    if (release) release(object,"release",0,0,0,0);
}
static void exception_capture_trace(Word object) {
    if (!object || read_word(object)!=external_class("_OBJC_CLASS_$_NSException")) return;
    struct HostObject *exception=(void *)object;
    if (exception->content[3]) return;
    struct GuestExceptionTrace *trace=calloc(1,sizeof(*trace));
    if (!trace) return;
    trace->count=darwin_backtrace(trace->frames,COUNT(trace->frames));
    if (!__atomic_compare_exchange_n(&exception->content[3],&(Word){0},(Word)trace,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) free(trace);
}
static __attribute__((noreturn,noinline)) void guest_exception_throw(Word object) {
    pthread_once(&exception_abi_once,exception_initialize_abi);
    struct GuestThrownException *payload=exception_allocate(sizeof(*payload));
    Method retain=find_implementation(object,"retain");
    if (retain) retain(object,"retain",0,0,0,0);
    payload->object=object;
    payload->type=(struct GuestExceptionType){exception_type_vtable+2,object ? class_name(read_word(object)) : "nil",object ? read_word(object) : 0};
    exception_capture_trace(object);
#if I2A_DEVELOPMENT
    if (interface_diagnostics_requested && object && read_word(object)==external_class("_OBJC_CLASS_$_NSException")) {
        struct HostObject *value=(void *)object;
        LOG("[DIAG-exception] Throw %s: %s",value->content[0] ? string_bytes(value->content[0],"exception") : "NSException",
            value->content[1] ? string_bytes(value->content[1],"exception") : "");
    }
#endif
    exception_throw_native(payload,&payload->type,exception_release_payload);
    __builtin_unreachable();
}
static Word guest_exception_begin_catch(void *native_exception) {
    Word object=(Word)exception_begin_native(native_exception);
#if I2A_DEVELOPMENT
    if (interface_diagnostics_requested) {
        struct GuestExceptionType *type=exception_current_type();
        if (type && type->vtable==exception_type_vtable+2)
            LOG("[DIAG-exception] Entered original catch for %s",type->name ? type->name : "nil");
    }
#endif
    return object;
}

static Word exception_init(struct HostObject *object,const char *selector,Word name,Word reason,Word info) {
    if (!name) stop("Invalid NSException name",selector);
    object->content[0]=copy_string(string_bytes(name,selector));
    object->content[1]=reason ? copy_string(string_bytes(reason,selector)) : 0;
    object->content[2]=info;
    return (Word)object;
}
static Word exception_create(Word cls,const char *selector,Word name,Word reason,Word info) {
    return exception_init((void *)host_allocate(cls),selector,name,reason,info);
}
static Word exception_name(struct HostObject *object) { return object->content[0]; }
static Word exception_reason(struct HostObject *object) { return object->content[1]; }
static Word exception_info(struct HostObject *object) { return object->content[2]; }
static Word exception_description(struct HostObject *object) {
    Word arguments[]={object->content[0],object->content[1]};
    return foundation_format(0,"NSException description",copy_string("%@: %@"),(const void *)arguments);
}
static Word exception_stack(struct HostObject *object,const char *selector) {
    struct GuestExceptionTrace *trace=(void *)__atomic_load_n(&object->content[3],__ATOMIC_ACQUIRE);
    if (!trace) return sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    int symbols=!strcmp(selector,"callStackSymbols");
    Word *cached=symbols ? &trace->symbols : &trace->addresses;
    Word existing=__atomic_load_n(cached,__ATOMIC_ACQUIRE);
    if (existing) return existing;
    Word values[COUNT(trace->frames)];
    char **names=symbols ? darwin_backtrace_symbols(trace->frames,trace->count) : NULL;
    if (symbols && !names) stop("Allocation failed","Exception call stack symbols");
    for (int i=0;i<trace->count;++i) values[i]=symbols ? copy_string(names[i]) : number_integer((Word)trace->frames[i]);
    free(names);
    Word result=new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,values,trace->count);
    __atomic_store_n(cached,result,__ATOMIC_RELEASE);
    return result;
}
static void exception_raise(Word object) { guest_exception_throw(object); }
__attribute__((used,noinline,noreturn)) static void exception_raise_format(Word cls,const char *selector,Word name,Word format,const unsigned char *arguments) {
    Word reason=foundation_format(0,selector,format,arguments);
    guest_exception_throw(exception_create(cls,selector,name,reason,0));
}
__attribute__((naked)) static void exception_raise_varargs(void) { __asm__ volatile("mov x4,sp\n b exception_raise_format"); }
static __attribute__((noreturn)) void object_unrecognized(Word object,const char *selector,const char *missing) {
    int type=host_symbol(object)!=NULL || guest_class_object(object);
    const char *name=class_name(type ? object : read_word(object));if(name && !strncmp(name,"_OBJC_CLASS_$_",14))name+=14;
    char *message=NULL;
    if(asprintf(&message,"%c[%s %s]: unrecognized selector sent to %s 0x%lx",type ? '+' : '-',name ? name : "NSObject",missing ? missing : "(null)",type ? "class" : "instance",object)<0)stop("Allocation failed",selector);
    Word reason=copy_string(message);free(message);
    guest_exception_throw(exception_create(external_class("_OBJC_CLASS_$_NSException"),selector,copy_string("NSInvalidArgumentException"),reason,0));
}
static Method exceptions_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if(receiver && !strcmp(selector,"doesNotRecognizeSelector:"))return (Method)object_unrecognized;
    if (!receiver || strcmp(receiver,"_OBJC_CLASS_$_NSException")) return NULL;
    if (class_receiver) {
        if (!strcmp(selector,"exceptionWithName:reason:userInfo:")) return (Method)exception_create;
        if (!strcmp(selector,"raise:format:")) return (Method)exception_raise_varargs;
        if (!strcmp(selector,"raise:format:arguments:")) return (Method)exception_raise_format;
    } else {
        if (!strcmp(selector,"initWithName:reason:userInfo:")) return (Method)exception_init;
        if (!strcmp(selector,"name")) return (Method)exception_name;
        if (!strcmp(selector,"reason")) return (Method)exception_reason;
        if (!strcmp(selector,"userInfo")) return (Method)exception_info;
        if (!strcmp(selector,"description")) return (Method)exception_description;
        if (!strcmp(selector,"raise")) return (Method)exception_raise;
        if (!strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:")) return (Method)identity;
        if (!strcmp(selector,"callStackReturnAddresses") || !strcmp(selector,"callStackSymbols")) return (Method)exception_stack;
    }
    return NULL;
}

static Word resolve_exception_import(unsigned index) {
    const char *name=symbols[index];
    if (!strcmp(name,"_OBJC_EHTYPE_id")) return (Word)&exception_id_type;
    if (!strncmp(name,"_OBJC_EHTYPE_$_",15)) {
        char cls[256];
        if (snprintf(cls,sizeof(cls),"_OBJC_CLASS_$_%s",name+15)>=(int)sizeof(cls)) stop("Exception class name too long",name);
        symbol_objects[index][0]=(Word)(exception_type_vtable+2);
        symbol_objects[index][1]=(Word)(name+15);
        symbol_objects[index][2]=external_class(cls);
        return (Word)symbol_objects[index];
    }
    if (!strcmp(name,"_objc_ehtype_vtable")) return (Word)exception_type_vtable;
    if (!strcmp(name,"_objc_exception_throw")) { pthread_once(&exception_abi_once,exception_initialize_abi); return (Word)guest_exception_throw; }
    if (!strcmp(name,"_objc_begin_catch")) { pthread_once(&exception_abi_once,exception_initialize_abi); return (Word)guest_exception_begin_catch; }
    if (!strcmp(name,"_objc_end_catch")) return exception_abi_symbol("__cxa_end_catch");
    if (!strcmp(name,"_objc_exception_rethrow")) return exception_abi_symbol("__cxa_rethrow");
    if (!strcmp(name,"_objc_terminate")) return exception_abi_symbol("_ZSt9terminatev");
    if (!strcmp(name,"___objc_personality_v0") || !strcmp(name,"___gxx_personality_v0")) return exception_abi_symbol("__gxx_personality_v0");
    if (!strcmp(name,"__Unwind_Resume")) return (Word)_Unwind_Resume;
    if (!strcmp(name,"__Unwind_Resume_or_Rethrow")) return (Word)_Unwind_Resume_or_Rethrow;
    static const char *const cxx[]={"___cxa_allocate_exception","___cxa_free_exception","___cxa_throw","___cxa_begin_catch",
        "___cxa_end_catch","___cxa_rethrow","___cxa_current_exception_type","___cxa_demangle","___cxa_pure_virtual"};
    for (unsigned i=0;i<COUNT(cxx);++i) if (!strcmp(name,cxx[i])) return exception_abi_symbol(name+1);
    return 0;
}
