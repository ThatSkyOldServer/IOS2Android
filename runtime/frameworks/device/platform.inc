 

static struct HostObject device, locale;

static Word system_text(const char *key) {
    JNIEnv *env=attached_environment(key);
    jstring name=(*env)->NewStringUTF(env,key);
    jstring value=(*env)->CallObjectMethod(env,activity,system_text_method,name);
    (*env)->DeleteLocalRef(env,name);
    if (!value) return 0;
    const char *text=(*env)->GetStringUTFChars(env,value,NULL);
    if (!text) stop("System query failed",key);
    Word result=copy_string(text);
    (*env)->ReleaseStringUTFChars(env,value,text);
    (*env)->DeleteLocalRef(env,value);
    return result;
}

static Word current_device(Word cls) { device.isa=cls; return (Word)&device; }
static Word current_locale(Word cls) { locale.isa=cls; return (Word)&locale; }
static Word device_model(void) { return system_text("model"); }
static Word locale_identifier(void) { return system_text("locale"); }
static Word system_version(void) { return copy_string("11.0"); }
static Word system_name(void) { return copy_string("iOS compatibility on Android"); }
struct SystemVersion { long major, minor, patch; };
static struct SystemVersion operating_system_version(void) { return (struct SystemVersion){11,0,0}; }
static Word version_at_least(Word self,const char *selector,struct SystemVersion wanted) {
    return wanted.major<11 || (wanted.major==11 && wanted.minor<=0 && wanted.patch<=0);
}

static int system_control(const char *name,void *old,size_t *old_size,const void *new_value,size_t new_size) {
    if (new_value || new_size) { errno=EPERM; return -1; }
    if (!old_size) { errno=EINVAL; return -1; }
    const void *value=NULL;
    size_t size=0;
    Word text=0;
    int processors=(int)sysconf(_SC_NPROCESSORS_ONLN);
    int pagesize=(int)sysconf(_SC_PAGESIZE);
    int cputype=0x0100000c,cpusubtype=0;
    uint64_t memory=(uint64_t)sysconf(_SC_PHYS_PAGES)*(uint64_t)sysconf(_SC_PAGESIZE);
    if (!strcmp(name,"hw.machine") || !strcmp(name,"hw.model")) {
        text=device_model(); value=string_bytes(text,"sysctlbyname"); size=strlen(value)+1;
    } else if (!strcmp(name,"hw.ncpu") || !strcmp(name,"hw.activecpu")) { value=&processors; size=sizeof(processors); }
    else if (!strcmp(name,"hw.memsize")) { value=&memory; size=sizeof(memory); }
    else if (!strcmp(name,"hw.pagesize")) { value=&pagesize; size=sizeof(pagesize); }
    else if (!strcmp(name,"hw.cputype")) { value=&cputype; size=sizeof(cputype); }
    else if (!strcmp(name,"hw.cpusubtype")) { value=&cpusubtype; size=sizeof(cpusubtype); }
    else if (!strcmp(name,"kern.ostype")) { value="Darwin"; size=strlen(value)+1; }
    else if (!strcmp(name,"kern.osrelease")) { value="17.0.0"; size=strlen(value)+1; }
    else if (!strcmp(name,"kern.osversion")) { value="15A372"; size=strlen(value)+1; }
    else if (!strcmp(name,"kern.version")) { value="Darwin 17 API compatibility on Android"; size=strlen(value)+1; }
    else { errno=ENOENT; return -1; }
    size_t capacity=*old_size;
    *old_size=size;
    if (old && capacity<size) { errno=ENOMEM; return -1; }
    if (old) memcpy(old,value,size);
    return 0;
}

static Method platform_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIDevice")) {
        if (class_receiver && !strcmp(selector,"currentDevice")) return (Method)current_device;
        if (!class_receiver && !strcmp(selector,"systemVersion")) return (Method)system_version;
        if (!class_receiver && !strcmp(selector,"systemName")) return (Method)system_name;
        if (!class_receiver && (!strcmp(selector,"model") || !strcmp(selector,"name"))) return (Method)device_model;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSLocale")) {
        if (class_receiver && (!strcmp(selector,"currentLocale") || !strcmp(selector,"systemLocale"))) return (Method)current_locale;
        if (!class_receiver && !strcmp(selector,"localeIdentifier")) return (Method)locale_identifier;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSProcessInfo")) {
        if (!strcmp(selector,"operatingSystemVersion")) return (Method)operating_system_version;
        if (!strcmp(selector,"isOperatingSystemAtLeastVersion:")) return (Method)version_at_least;
    }
    return NULL;
}
