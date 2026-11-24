/* Darwin ARM64 libc++ uses the alternate 24-byte string representation and a
 * 128-byte mbstate_t. NDK strings/fpos cannot be substituted by renaming alone.
 * ios_base (136), basic_ios (152), and streambuf (64) retain the observed layout;
 * only their compatible narrow-character operations use NDK implementations. */
struct GuestCppString { union { unsigned char small[24]; struct { char *data; size_t size,capacity; } large; }; };
_Static_assert(sizeof(struct GuestCppString)==24,"Darwin string layout");
#define CPP_LONG_BIT (UINT64_C(1)<<63)
static int cpp_string_long(const struct GuestCppString *value) { return (value->large.capacity&CPP_LONG_BIT)!=0; }
static size_t cpp_string_size(const struct GuestCppString *value) { return cpp_string_long(value) ? value->large.size : value->small[23]; }
static char *cpp_string_data(const struct GuestCppString *value) { return cpp_string_long(value) ? value->large.data : (char *)value->small; }
static void cpp_string_length(struct GuestCppString *value,size_t length) {
    if (cpp_string_long(value)) value->large.size=length; else value->small[23]=(unsigned char)length;
    cpp_string_data(value)[length]=0;
}
static void cpp_string_init(struct GuestCppString *value,const char *bytes,size_t length) {
    if (length>=CPP_LONG_BIT-16 || (!bytes && length)) stop("Invalid std::string length","C++ string construction");
    if (length<23) {
        memset(value,0,sizeof(*value));
        if (length) memcpy(value->small,bytes,length);
        value->small[23]=(unsigned char)length;
    } else {
        size_t capacity=(length+16)&~(size_t)15;
        char *data=malloc(capacity);
        if (!data) stop("Allocation failed","C++ string construction");
        memcpy(data,bytes,length); data[length]=0;
        value->large.data=data; value->large.size=length; value->large.capacity=capacity|CPP_LONG_BIT;
    }
}
static struct GuestCppString *cpp_string_destroy(struct GuestCppString *value) {
    if (cpp_string_long(value)) free(value->large.data);
    memset(value,0,sizeof(*value));
    return value;
}
static void cpp_string_reserve(struct GuestCppString *value,size_t length) {
    size_t old_size=cpp_string_size(value),old_capacity=cpp_string_long(value) ? (value->large.capacity&~CPP_LONG_BIT) : 23;
    if (length<old_capacity) return;
    if (length>=CPP_LONG_BIT-16) stop("std::string length_error","C++ string reserve");
    size_t capacity=old_capacity>=(CPP_LONG_BIT-16)/2 ? length+1 : old_capacity*2;
    if (capacity<=length) capacity=length+1;
    capacity=(capacity+15)&~(size_t)15;
    char *data=malloc(capacity);
    if (!data) stop("Allocation failed","C++ string reserve");
    memcpy(data,cpp_string_data(value),old_size+1);
    if (cpp_string_long(value)) free(value->large.data);
    value->large.data=data; value->large.size=old_size; value->large.capacity=capacity|CPP_LONG_BIT;
}
static void cpp_string_fill(struct GuestCppString *value,size_t length,char character) {
    memset(value,0,sizeof(*value)); cpp_string_reserve(value,length);
    memset(cpp_string_data(value),(unsigned char)character,length); cpp_string_length(value,length);
}
static void cpp_string_resize(struct GuestCppString *value,size_t length,char character) {
    size_t previous=cpp_string_size(value); cpp_string_reserve(value,length);
    if (length>previous) memset(cpp_string_data(value)+previous,(unsigned char)character,length-previous);
    cpp_string_length(value,length);
}
static void cpp_string_push(struct GuestCppString *value,char character) {
    size_t previous=cpp_string_size(value);
    if (previous>=CPP_LONG_BIT-16) stop("std::string length_error","push_back");
    cpp_string_reserve(value,previous+1); cpp_string_data(value)[previous]=character; cpp_string_length(value,previous+1);
}
static struct GuestCppString *cpp_string_copy(struct GuestCppString *value,const struct GuestCppString *other) { cpp_string_init(value,cpp_string_data(other),cpp_string_size(other)); return value; }
static struct GuestCppString *cpp_string_substring(struct GuestCppString *value,const struct GuestCppString *other,size_t start,size_t length,const void *allocator) {
    size_t size=cpp_string_size(other);
    if (start>size) stop("std::string out_of_range","substring constructor");
    if (length>size-start) length=size-start;
    cpp_string_init(value,cpp_string_data(other)+start,length);
    return value;
}
static struct GuestCppString *cpp_string_assign(struct GuestCppString *value,const struct GuestCppString *other) {
    if (value!=other) { struct GuestCppString copy; cpp_string_copy(&copy,other); cpp_string_destroy(value); *value=copy; }
    return value;
}
static struct GuestCppString *cpp_string_assign_bytes(struct GuestCppString *value,const char *bytes,size_t length) {
    struct GuestCppString copy;cpp_string_init(&copy,bytes,length);cpp_string_destroy(value);*value=copy;return value;
}
static struct GuestCppString *cpp_string_assign_cstring(struct GuestCppString *value,const char *bytes) {
    if(!bytes)stop("Null std::string source","assign");return cpp_string_assign_bytes(value,bytes,strlen(bytes));
}
static float cpp_stof(const struct GuestCppString *value,size_t *position) {
    const char *source=cpp_string_data(value); char *end;
    int saved=errno; errno=0; float result=strtof(source,&end); int error=errno; errno=saved;
    if (source==end) stop("std::invalid_argument","stof");
    if (error==ERANGE) stop("std::out_of_range","stof");
    if (position) *position=(size_t)(end-source);
    return result;
}
static void cpp_mutex_lock(struct DarwinMutex *mutex) { if (guest_mutex_lock(mutex)) stop("std::mutex lock failed","C++ mutex"); }
static unsigned char cpp_mutex_trylock(struct DarwinMutex *mutex) { return guest_mutex_trylock(mutex)==0; }
static void cpp_mutex_unlock(struct DarwinMutex *mutex) { if (guest_mutex_unlock(mutex)) stop("std::mutex unlock failed","C++ mutex"); }
static Word cpp_mutex_destroy(struct DarwinMutex *mutex) { if (guest_mutex_destroy(mutex)) stop("std::mutex destroy failed","C++ mutex"); return (Word)mutex; }
static void *cpp_library;
static Word cpp_export(const char *symbol) {
    if (!cpp_library) cpp_library=dlopen("libc++_shared.so",RTLD_NOW|RTLD_LOCAL);
    if (!cpp_library) stop("Loading NDK libc++",dlerror());
    return (Word)dlsym(cpp_library,symbol);
}
 
static Word cpp_structor_targets[13];
#define CPP_THIS_ONE(tag,slot) static Word tag(Word self) { ((void (*)(Word))cpp_structor_targets[slot])(self); return self; }
#define CPP_THIS_TWO(tag,slot) static Word tag(Word self,Word argument) { ((void (*)(Word,Word))cpp_structor_targets[slot])(self,argument); return self; }
CPP_THIS_ONE(cpp_streambuf_construct,0)
CPP_THIS_ONE(cpp_streambuf_destruct,1)
CPP_THIS_TWO(cpp_locale_construct,2)
CPP_THIS_ONE(cpp_locale_destruct,3)
CPP_THIS_ONE(cpp_ios_destruct,4)
CPP_THIS_ONE(cpp_istream_destruct,5)
CPP_THIS_ONE(cpp_ostream_destruct,6)
CPP_THIS_TWO(cpp_ostream_base_destruct,7)
CPP_THIS_ONE(cpp_iostream_destruct,8)
CPP_THIS_TWO(cpp_iostream_base_destruct,9)
CPP_THIS_TWO(cpp_ostream_sentry_construct,10)
CPP_THIS_ONE(cpp_ostream_sentry_destruct,11)
static Word cpp_istream_sentry_construct(Word self,Word stream,unsigned char noskipws) {
    ((void (*)(Word,Word,unsigned char))cpp_structor_targets[12])(self,stream,noskipws); return self;
}
#undef CPP_THIS_ONE
#undef CPP_THIS_TWO
static Word cpp_structor_bridge(const char *symbol) {
    static const struct { const char *name; Word thunk; } entries[]={
        {"_ZNSt6__ndk115basic_streambufIcNS_11char_traitsIcEEEC2Ev",(Word)cpp_streambuf_construct},
        {"_ZNSt6__ndk115basic_streambufIcNS_11char_traitsIcEEED2Ev",(Word)cpp_streambuf_destruct},
        {"_ZNSt6__ndk16localeC1ERKS0_",(Word)cpp_locale_construct},
        {"_ZNSt6__ndk16localeD1Ev",(Word)cpp_locale_destruct},
        {"_ZNSt6__ndk19basic_iosIcNS_11char_traitsIcEEED2Ev",(Word)cpp_ios_destruct},
        {"_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEED1Ev",(Word)cpp_istream_destruct},
        {"_ZNSt6__ndk113basic_ostreamIcNS_11char_traitsIcEEED1Ev",(Word)cpp_ostream_destruct},
        {"_ZNSt6__ndk113basic_ostreamIcNS_11char_traitsIcEEED2Ev",(Word)cpp_ostream_base_destruct},
        {"_ZNSt6__ndk114basic_iostreamIcNS_11char_traitsIcEEED1Ev",(Word)cpp_iostream_destruct},
        {"_ZNSt6__ndk114basic_iostreamIcNS_11char_traitsIcEEED2Ev",(Word)cpp_iostream_base_destruct},
        {"_ZNSt6__ndk113basic_ostreamIcNS_11char_traitsIcEEE6sentryC1ERS3_",(Word)cpp_ostream_sentry_construct},
        {"_ZNSt6__ndk113basic_ostreamIcNS_11char_traitsIcEEE6sentryD1Ev",(Word)cpp_ostream_sentry_destruct},
        {"_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE6sentryC1ERS3_b",(Word)cpp_istream_sentry_construct}
    };
    for (unsigned i=0;i<COUNT(entries);++i) if (!strcmp(symbol,entries[i].name)) {
        cpp_structor_targets[i]=cpp_export(symbol); return cpp_structor_targets[i] ? entries[i].thunk : 0;
    }
    return 0;
}
struct GuestCppPosition { unsigned char state[128]; int64_t offset; };
_Static_assert(sizeof(struct GuestCppPosition)==136,"Darwin fpos layout");
static struct GuestCppPosition cpp_failed_seek(void) { return (struct GuestCppPosition){.offset=-1}; }
static Word cpp_ios(Word stream) {
    Word table=read_word(stream);
    int64_t offset=(int64_t)read_word(table-24);
    return stream+offset;
}
static unsigned cpp_ios_flags(Word ios) { unsigned value; memcpy(&value,(void *)(ios+32),sizeof(value)); return value; }
static void cpp_ios_clear(Word ios,unsigned flags) {
    ((void (*)(Word,unsigned))cpp_export("_ZNSt6__ndk18ios_base5clearEj"))(ios,flags);
}
static int cpp_istream_sentry(Word stream) {
    unsigned char good=0;
    ((void (*)(void *,Word,unsigned char))cpp_export("_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE6sentryC1ERS3_b"))(&good,stream,1);
    return good!=0;
}
static struct GuestCppPosition cpp_seek_streambuf(Word buffer,int64_t offset,int direction) {
    if (!buffer) return cpp_failed_seek();
    Word target=read_word(read_word(buffer)+4*sizeof(Word));
    if (!target) stop("Null streambuf seekoff","C++ stream");
     
    return ((struct GuestCppPosition (*)(Word,int64_t,int,unsigned))target)(buffer,offset,direction,8);
}
static struct GuestCppPosition cpp_tellg(Word stream) {
    if (!cpp_istream_sentry(stream)) return cpp_failed_seek();
    Word ios=cpp_ios(stream);
    if (cpp_ios_flags(ios)&5) return cpp_failed_seek();
    return cpp_seek_streambuf(read_word(ios+40),0,1);
}
static Word cpp_seekg(Word stream,int64_t offset,int direction) {
    Word ios=cpp_ios(stream); cpp_ios_clear(ios,cpp_ios_flags(ios)&~2U);
    if (cpp_istream_sentry(stream) && !(cpp_ios_flags(ios)&5)) {
        struct GuestCppPosition result=cpp_seek_streambuf(read_word(ios+40),offset,direction);
        if (result.offset==-1) cpp_ios_clear(ios,cpp_ios_flags(ios)|4U);
    }
    return stream;
}
static __attribute__((noreturn)) void cpp_length_error(void) { stop("C++ exception requires guest unwinding","std::length_error"); }
static __attribute__((noreturn)) void cpp_out_of_range(void) { stop("C++ exception requires guest unwinding","std::out_of_range"); }

static Word resolve_cpp_import(const char *name) {
    if (!strcmp(name,"___cxa_demangle")) return cpp_export("__cxa_demangle");
    static const char *const rtti[]={"__ZTIPKc","__ZTISt9bad_alloc","__ZTISt9exception",
        "__ZTVN10__cxxabiv117__class_type_infoE","__ZTVN10__cxxabiv119__pointer_type_infoE","__ZTVN10__cxxabiv121__vmi_class_type_infoE"};
    for (unsigned i=0;i<COUNT(rtti);++i) if (!strcmp(name,rtti[i])) return cpp_export(name+1);
    static const char string_prefix[]="__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
    if (!strncmp(name,string_prefix,sizeof(string_prefix)-1)) {
        const char *suffix=name+sizeof(string_prefix)-1;
        if (!strcmp(suffix,"6__initEPKcm")) return (Word)cpp_string_init;
        if (!strcmp(suffix,"6__initEmc")) return (Word)cpp_string_fill;
        if (!strcmp(suffix,"6resizeEmc")) return (Word)cpp_string_resize;
        if (!strcmp(suffix,"9push_backEc")) return (Word)cpp_string_push;
        if (!strcmp(suffix,"C1ERKS5_")) return (Word)cpp_string_copy;
        if (!strcmp(suffix,"C1ERKS5_mmRKS4_")) return (Word)cpp_string_substring;
        if (!strcmp(suffix,"D1Ev")) return (Word)cpp_string_destroy;
        if (!strcmp(suffix,"aSERKS5_")) return (Word)cpp_string_assign;
        if (!strcmp(suffix,"6assignEPKc") || !strcmp(suffix,"aSEPKc")) return (Word)cpp_string_assign_cstring;
        if (!strcmp(suffix,"6assignEPKcm")) return (Word)cpp_string_assign_bytes;
        return 0;
    }
    if (!strcmp(name,"__ZNSt3__14stofERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEPm")) return (Word)cpp_stof;
    if (!strcmp(name,"__ZNSt3__15mutex4lockEv")) return (Word)cpp_mutex_lock;
    if (!strcmp(name,"__ZNSt3__15mutex8try_lockEv")) return (Word)cpp_mutex_trylock;
    if (!strcmp(name,"__ZNSt3__15mutex6unlockEv")) return (Word)cpp_mutex_unlock;
    if (!strcmp(name,"__ZNSt3__15mutexD1Ev")) return (Word)cpp_mutex_destroy;
    if (!strcmp(name,"__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv") || !strcmp(name,"__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv")) return (Word)cpp_length_error;
    if (!strcmp(name,"__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv")) return (Word)cpp_out_of_range;
    if (!strcmp(name,"__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5tellgEv")) return (Word)cpp_tellg;
    if (!strcmp(name,"__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5seekgExNS_8ios_base7seekdirE")) return (Word)cpp_seekg;
    if (!strcmp(name,"__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekoffExNS_8ios_base7seekdirEj") ||
        !strcmp(name,"__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekposENS_4fposI11__mbstate_tEEj")) return (Word)cpp_failed_seek;
    if (!strcmp(name,"__ZNSt3__17codecvtIcc11__mbstate_tE2idE")) return cpp_export("_ZNSt6__ndk17codecvtIcc9mbstate_tE2idE");
    int safe=strstr(name,"St3__18ios_base") || strstr(name,"St3__16locale") ||
        strstr(name,"St3__113basic_ostreamIc") || strstr(name,"St3__113basic_istreamIc") ||
        strstr(name,"St3__114basic_iostreamIc") || strstr(name,"St3__115basic_streambufIc") ||
        strstr(name,"St3__19basic_iosIc") || strstr(name,"St3__15ctypeIcE2idE") || strstr(name,"St3__16__sortI");
    if (!safe) return 0;
    const char *position=strstr(name,"St3__1");
    if (!position || name[0]!='_' || strstr(name,"fpos") || strstr(name,"mbstate") || strstr(name,"basic_string")) return 0;
    char symbol[384]; size_t prefix=(size_t)(position-name-1);
    if (prefix+9+strlen(position+6)+1>sizeof(symbol)) return 0;
    memcpy(symbol,name+1,prefix);
    snprintf(symbol+prefix,sizeof(symbol)-prefix,"St6__ndk1%s",position+6);
    Word structor=cpp_structor_bridge(symbol);
    if (structor) return structor;
    return cpp_export(symbol);
}
