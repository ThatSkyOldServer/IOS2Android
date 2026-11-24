static Word guest_symbol(intptr_t,const char *);
static Word guest_find_export(const char *,int,Word [4]);
struct GuestLibrary { const char *name,*prefixes; unsigned references,global,resident,nodelete; };
static struct GuestLibrary guest_libraries[]={
    {"process","",0,1},{"libSystem","",0,1},
    {"Foundation","NS",0,0},{"CoreFoundation","CF,kCF",0,0},
    {"Security","Sec,kSec",0,0},{"CoreGraphics","CG,kCG",0,0},
    {"UIKit","UI",0,0},{"QuartzCore","CA,kCA",0,0},
    {"AudioToolbox","Audio,kAudio",0,0},{"AVFoundation","AV,kAV",0,0},
    {"AdSupport","AS",0,0},{"StoreKit","SK",0,0},
    {"CoreTelephony","CTTelephony,CTCarrier,CTCall,CTRadio,kCTRadio",0,0},{"CoreMotion","CM",0,0},
    {"SystemConfiguration","SC,kSC",0,0},{"CFNetwork","CF,kCF",0,0},
    {"MobileCoreServices","UT,kUT",0,0},{"UniformTypeIdentifiers","UT,kUT",0,0},
    {"libobjc","objc_,class_,object_,method_,sel_,protocol_",0,0},
    {"libc++","_Z",0,0},{"libsqlite3","sqlite3_",0,0}
};
static pthread_mutex_t guest_library_mutex=PTHREAD_MUTEX_INITIALIZER;
#ifdef I2A_GUEST_COLLECTION
struct EmbeddedLibraryHandle { unsigned references; };
static struct EmbeddedLibraryHandle embedded_library_handles[COUNT(guest_images)];
static int embedded_library_path(const char *path) {
    if(!path)return -1;
    for(unsigned i=0;i<COUNT(guest_images);++i){
        const char *relative=guest_images[i].path;size_t n=strlen(path),m=strlen(relative);
        if(!strcmp(path,guest_images[i].install_name) || !strcmp(path,relative) ||
           (n>m && path[n-m-1]=='/' && !strcmp(path+n-m,relative)))return (int)i;
    }
    return -1;
}
static int embedded_library_handle(intptr_t handle) {
    for(unsigned i=0;i<COUNT(guest_images);++i)if(handle==(intptr_t)&embedded_library_handles[i])return (int)i;
    return -1;
}
static Word guest_collection_export(const char *,int);
#endif
static _Thread_local char guest_library_error[384];
static _Thread_local int guest_library_error_pending;
static void guest_library_fail(const char *operation,const char *detail) {
    snprintf(guest_library_error,sizeof(guest_library_error),"%s: %s",operation,detail ? detail : "invalid argument");
    guest_library_error_pending=1;
}
static const char *guest_library_last_error(void) {
    if (!guest_library_error_pending) return NULL;
    guest_library_error_pending=0; return guest_library_error;
}
static struct GuestLibrary *guest_library_handle(intptr_t handle) {
    for (unsigned i=0;i<COUNT(guest_libraries);++i)
        if (handle==(intptr_t)&guest_libraries[i]) return &guest_libraries[i];
    return NULL;
}
static struct GuestLibrary *guest_library_path(const char *path) {
    if (!path) return &guest_libraries[0];
    for (unsigned i=1;i<COUNT(guest_libraries);++i) {
        char name[256];
        snprintf(name,sizeof(name),"/System/Library/Frameworks/%s.framework/%s",guest_libraries[i].name,guest_libraries[i].name);
        if (!strcmp(path,name)) return &guest_libraries[i];
    }
    if (!strcmp(path,"/usr/lib/libSystem.B.dylib") || !strcmp(path,"/usr/lib/libc.dylib")) return &guest_libraries[1];
    if (!strcmp(path,"/usr/lib/libobjc.A.dylib")) return &guest_libraries[18];
    if (!strcmp(path,"/usr/lib/libc++.1.dylib")) return &guest_libraries[19];
    if (!strcmp(path,"/usr/lib/libsqlite3.dylib")) return &guest_libraries[20];
    if (!strcmp(path,GUEST_IMAGE_PATH) || !strcmp(path,GUEST_EXECUTABLE)) return &guest_libraries[0];
    return NULL;
}
static pthread_once_t guest_library_once=PTHREAD_ONCE_INIT;
static void guest_library_initialize(void) {
    guest_libraries[0].resident=guest_libraries[1].resident=1;
    for (unsigned i=0;guest_dependencies[i];++i) {
        struct GuestLibrary *library=guest_library_path(guest_dependencies[i]);
        if (library) library->resident=library->global=1;
    }
}
static void *guest_library_open(const char *path,int mode) {
    if ((mode&~(1|2|4|8|16|128|256)) || (mode&3)==3 || (mode&12)==12) {
        guest_library_fail("dlopen","invalid Darwin mode"); return NULL;
    }
#ifdef I2A_GUEST_COLLECTION
    int embedded=embedded_library_path(path);
    if(embedded>=0){
        pthread_mutex_lock(&guest_library_mutex);
        ++embedded_library_handles[embedded].references;
        pthread_mutex_unlock(&guest_library_mutex);
        return &embedded_library_handles[embedded];
    }
#endif
    struct GuestLibrary *library=guest_library_path(path);
    if (!library) { guest_library_fail("dlopen",path); return NULL; }
    pthread_once(&guest_library_once,guest_library_initialize);
    pthread_mutex_lock(&guest_library_mutex);
    if ((mode&16) && !library->references && !library->resident && !library->nodelete) {
        pthread_mutex_unlock(&guest_library_mutex); guest_library_fail("dlopen","image is not loaded"); return NULL;
    }
    if (library->references==UINT_MAX) {
        pthread_mutex_unlock(&guest_library_mutex); guest_library_fail("dlopen","reference limit"); return NULL;
    }
    ++library->references; if (!(mode&4)) library->global=1;
    if (mode&128) library->nodelete=1;
    pthread_mutex_unlock(&guest_library_mutex);
    LOG("Darwin framework opened: %s",library->name);
    return library;
}
static int guest_library_close(void *handle) {
    pthread_mutex_lock(&guest_library_mutex);
#ifdef I2A_GUEST_COLLECTION
    int embedded=embedded_library_handle((intptr_t)handle);
    if(embedded>=0){
        unsigned *references=&embedded_library_handles[embedded].references;
        int valid=*references!=0;if(valid)--*references;
        pthread_mutex_unlock(&guest_library_mutex);
        if(!valid)guest_library_fail("dlclose","invalid handle");
        return valid ? 0 : -1;
    }
#endif
    struct GuestLibrary *library=guest_library_handle((intptr_t)handle);
    if (!library || !library->references) {
        pthread_mutex_unlock(&guest_library_mutex); guest_library_fail("dlclose","invalid handle"); return -1;
    }
    --library->references; if (!library->references && !library->resident && !library->nodelete) library->global=0;
    pthread_mutex_unlock(&guest_library_mutex); return 0;
}
static int guest_library_preflight(const char *path) {
#ifdef I2A_GUEST_COLLECTION
    if(embedded_library_path(path)>=0)return 1;
#endif
    return guest_library_path(path)!=NULL;
}
static int guest_library_matches(const struct GuestLibrary *library,const char *name) {
    if (!library->prefixes[0]) return 1;
    if (!strncmp(name,"OBJC_CLASS_$_",13)) name+=13;
    else if (!strncmp(name,"OBJC_METACLASS_$_",17)) name+=17;
    for (const char *prefix=library->prefixes;*prefix;) {
        const char *end=strchr(prefix,','); size_t size=end ? (size_t)(end-prefix) : strlen(prefix);
        if (!strncmp(name,prefix,size)) return 1;
        if (!end) break; prefix=end+1;
    }
    return 0;
}
static struct GuestLibrary *guest_library_owner(const char *name) {
    for (unsigned i=2;i<COUNT(guest_libraries);++i) if (guest_library_matches(&guest_libraries[i],name)) return &guest_libraries[i];
    return &guest_libraries[1];
}
static int guest_library_exports(const struct GuestLibrary *library,const char *name) {
    return library==guest_libraries || (library==guest_libraries+1 ? guest_library_owner(name)==library : guest_library_matches(library,name));
}

#ifdef I2A_GUEST_COLLECTION
static uint32_t guest_image_count(void) { return COUNT(guest_images); }
static const void *guest_image_header(uint32_t index) { return index<COUNT(guest_images) ? guest_images[index].address : NULL; }
static intptr_t guest_image_slide(uint32_t index) { return index<COUNT(guest_images) ? (intptr_t)((Word)guest_images[index].address-guest_images[index].native_base) : 0; }
static const char *guest_image_name(uint32_t index) { return index<COUNT(guest_images) ? guest_images[index].path : NULL; }
static int guest_image_index(Word address) {
    size_t first=0,last=COUNT(guest_images);
    while(first<last){size_t middle=first+(last-first)/2;if((Word)guest_images[middle].address<=address)first=middle+1;else last=middle;}
    return first && address-(Word)guest_images[first-1].address<guest_images[first-1].size ? (int)(first-1) : -1;
}
static Word guest_collection_export(const char *name,int image) {
    size_t first=0,last=COUNT(guest_defined_exports);
    while(first<last){size_t middle=first+(last-first)/2;if(strcmp(guest_defined_exports[middle].name,name)<0)first=middle+1;else last=middle;}
    for(size_t i=first;i<COUNT(guest_defined_exports) && !strcmp(guest_defined_exports[i].name,name);++i){
        const struct I2AGuestExport *entry=guest_defined_exports+i;
        if(image>=0 && entry->image!=(unsigned)image)continue;
        if(entry->kind==0)return (Word)guest_images[entry->image].address+entry->value;
        if(entry->kind==2)return entry->value;
        stop("Unsupported thread-local export",name);
    }
    return 0;
}
#else
static uint32_t guest_image_count(void) { return 1; }
static const void *guest_image_header(uint32_t index) { return index ? NULL : (void *)aslr; }
static intptr_t guest_image_slide(uint32_t index) { return index ? 0 : (intptr_t)(aslr-GUEST_IMAGE_BASE); }
static const char *guest_image_name(uint32_t index) { return index ? NULL : GUEST_IMAGE_PATH; }
#endif
static Word guest_native_address(Word address) {
    if(!in_image(address))return 0;
#ifdef I2A_GUEST_COLLECTION
    int image=guest_image_index(address);
    return image<0 ? 0 : guest_images[image].native_base+address-(Word)guest_images[image].address;
#else
    return GUEST_IMAGE_BASE+address-aslr;
#endif
}
typedef void (*ImageCallback)(const void *,intptr_t);
static void guest_register_add_image(ImageCallback callback) {
    if (callback) for(unsigned i=0;i<guest_image_count();++i) callback(guest_image_header(i),guest_image_slide(i));
}
static void guest_register_remove_image(ImageCallback callback) {
     
}

static const void *guest_section64(const void *header,const char *segment,const char *section) {
    if (!header || !segment || !section) return NULL;
    const uint32_t *words=header;
    if (words[0]!=0xfeedfacf) return NULL;
    size_t bytes=words[5];
    const unsigned char *command=(const unsigned char *)header+32,*end=command+bytes;
    for (uint32_t i=0;i<words[4] && (size_t)(end-command)>=8;++i) {
        uint32_t type,size;
        memcpy(&type,command,4); memcpy(&size,command+4,4);
        if (size<8 || size>(size_t)(end-command)) return NULL;
        if (type==0x19 && size>=72 && !strncmp((const char *)command+8,segment,16)) {
            uint32_t count;
            memcpy(&count,command+64,4);
            if (count>(size-72)/80) return NULL;
            for (uint32_t j=0;j<count;++j) {
                const unsigned char *entry=command+72+80*j;
                if (!strncmp((const char *)entry,section,16)) return entry;
            }
        }
        command+=size;
    }
    return NULL;
}
static const void *guest_section_data(const void *header,const char *segment,const char *section,unsigned long *size) {
    if(size)*size=0;
    const unsigned char *record=guest_section64(header,segment,section);
    if(!record)return NULL;
    uint64_t address,length;memcpy(&address,record+32,8);memcpy(&length,record+40,8);
    for(unsigned i=0;i<guest_image_count();++i)if(guest_image_header(i)==header){
        Word pointer=address+guest_image_slide(i);
        if(!length || !image_segment_for(pointer,length))return NULL;
        if(size)*size=length;return (void *)pointer;
    }
    return NULL;
}
static const void *guest_segment_data(const void *header,const char *segment,unsigned long *size) {
    if(size)*size=0;
    if(!header || !segment || !image_segment_for((Word)header,32))return NULL;
    const uint32_t *words=header;
    if(words[0]!=0xfeedfacf || !image_segment_for((Word)header,32+(size_t)words[5]))return NULL;
    intptr_t slide=0;int known=0;
    for(unsigned i=0;i<guest_image_count();++i)if(guest_image_header(i)==header){slide=guest_image_slide(i);known=1;break;}
    if(!known)return NULL;
    const unsigned char *command=(const unsigned char *)header+32,*end=command+words[5];
    for(unsigned i=0;i<words[4] && (size_t)(end-command)>=8;++i){
        uint32_t kind,bytes;memcpy(&kind,command,4);memcpy(&bytes,command+4,4);
        if(bytes<8 || bytes>(size_t)(end-command))return NULL;
        if(kind==0x19 && bytes>=72 && !strncmp((const char *)command+8,segment,16)){
            uint64_t address,length;memcpy(&address,command+24,8);memcpy(&length,command+32,8);
            Word pointer=address+slide;if(!length || !image_segment_for(pointer,length))return NULL;
            if(size)*size=length;return (void *)pointer;
        }
        command+=bytes;
    }
    return NULL;
}
static const void *guest_section32(const void *header,const char *segment,const char *section) {
    if (header && *(const uint32_t *)header==0xfeedfacf) return NULL;
    stop("Unsupported 32-bit Mach-O image","getsectbynamefromheader");
}
static int guest_library_name_equal(const char *path,size_t length,const char *wanted) {
    const char *leaf=path;
    for (size_t i=0;i<length;++i) if (path[i]=='/') leaf=path+i+1;
    size_t size=length-(size_t)(leaf-path);
    if (strlen(wanted)==size && !memcmp(leaf,wanted,size)) return 1;
    if (size>6 && !memcmp(leaf+size-6,".dylib",6)) {
        size-=6;
        if (size>3 && !memcmp(leaf,"lib",3)) { leaf+=3; size-=3; }
        if (strlen(wanted)==size && !memcmp(leaf,wanted,size)) return 1;
    }
    return 0;
}
static int32_t guest_dylib_version(const void *header,const char *name,int linked) {
    if (!header || !name || !*name) return -1;
    const uint32_t *words=header;
    if (words[0]!=0xfeedfacf || words[5]>IMAGE_END-32) return -1;
    const unsigned char *command=(const unsigned char *)header+32,*end=command+words[5];
    for (uint32_t i=0;i<words[4] && (size_t)(end-command)>=8;++i) {
        uint32_t kind,size; memcpy(&kind,command,4); memcpy(&size,command+4,4);
        if (size<8 || size>(size_t)(end-command)) return -1;
        kind&=0x7fffffff;
        if ((linked ? kind==0xc || kind==0x18 || kind==0x1f || kind==0x23 : kind==0xd) && size>=24) {
            uint32_t offset,version; memcpy(&offset,command+8,4); memcpy(&version,command+16,4);
            if (offset<24 || offset>=size) return -1;
            const char *path=(const char *)command+offset; size_t length=strnlen(path,size-offset);
            if (length==size-offset) return -1;
            if (guest_library_name_equal(path,length,name)) return (int32_t)version;
        }
        command+=size;
    }
    return -1;
}
static int32_t guest_link_library_version(const char *name) { return guest_dylib_version((const void *)aslr,name,1); }
static int32_t guest_runtime_library_version(const char *name) {
    for (uint32_t i=0;i<guest_image_count();++i) {
        int32_t version=guest_dylib_version(guest_image_header(i),name,0);
        if (version!=-1) return version;
    }
    return -1;
}
static const void *guest_main_image_header(void) { return guest_image_header(0); }
static Word resolve_dyld_import(const char *name) {
    if (!strcmp(name,"__NSGetMachExecuteHeader")) return (Word)guest_main_image_header;
    if (!strcmp(name,"_getsegmentdata")) return (Word)guest_segment_data;
    if (!strcmp(name,"_getsectiondata")) return (Word)guest_section_data;
    if (!strcmp(name,"_NSVersionOfLinkTimeLibrary")) return (Word)guest_link_library_version;
    if (!strcmp(name,"_NSVersionOfRunTimeLibrary")) return (Word)guest_runtime_library_version;
    if (!strcmp(name,"_dlopen")) return (Word)guest_library_open;
    if (!strcmp(name,"_dlsym")) return (Word)guest_symbol;
    if (!strcmp(name,"_dlclose")) return (Word)guest_library_close;
    if (!strcmp(name,"_dlerror")) return (Word)guest_library_last_error;
    if (!strcmp(name,"_dlopen_preflight")) return (Word)guest_library_preflight;
    if (!strcmp(name,"__dyld_image_count")) return (Word)guest_image_count;
    if (!strcmp(name,"__dyld_get_image_header")) return (Word)guest_image_header;
    if (!strcmp(name,"__dyld_get_image_vmaddr_slide")) return (Word)guest_image_slide;
    if (!strcmp(name,"__dyld_get_image_name")) return (Word)guest_image_name;
    if (!strcmp(name,"__dyld_register_func_for_add_image")) return (Word)guest_register_add_image;
    if (!strcmp(name,"__dyld_register_func_for_remove_image")) return (Word)guest_register_remove_image;
    if (!strcmp(name,"_getsectbynamefromheader_64")) return (Word)guest_section64;
    if (!strcmp(name,"_getsectbynamefromheader")) return (Word)guest_section32;
    return 0;
}
