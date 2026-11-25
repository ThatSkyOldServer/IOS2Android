/* Darwin arm64 spills variadic arguments on the stack. Ordinary Android
 * variadic functions cannot be bound directly to these guest entry points. */

static Word formatted_c_string(const char *format,const unsigned char *arguments) {
    Word text=copy_string(format);
    Word output=foundation_format(0,"printf",text,arguments);
    free((void *)text);
    return output;
}

static int guest_vsnprintf(char *destination,size_t capacity,const char *format,const unsigned char *arguments) {
    Word output=formatted_c_string(format,arguments);
    const char *bytes=string_bytes(output,"snprintf");
    size_t length=strlen(bytes);
    if (length>INT32_MAX) { free((void *)output); errno=EOVERFLOW; return -1; }
    if (capacity) {
        if (!destination) { free((void *)output); errno=EINVAL; return -1; }
        size_t copy=length<capacity-1 ? length : capacity-1;
        memcpy(destination,bytes,copy); destination[copy]=0;
    }
    free((void *)output);
    return (int)length;
}

__attribute__((used,noinline)) static int guest_vsprintf(char *destination,const char *format,const unsigned char *arguments) {
    return guest_vsnprintf(destination,SIZE_MAX,format,arguments);
}

__attribute__((used,noinline)) static int guest_fprintf(FILE *stream,const char *format,const unsigned char *arguments) {
    Word output=formatted_c_string(format,arguments);
    const char *bytes=string_bytes(output,"fprintf");
    size_t length=strlen(bytes);
    int result=length>INT32_MAX || fwrite(bytes,1,length,stream)!=length ? -1 : (int)length;
    free((void *)output);
    return result;
}

__attribute__((used,noinline)) static int guest_printf(const char *format,const unsigned char *arguments) {
    return guest_fprintf(stdout,format,arguments);
}

__attribute__((used,noinline)) static int guest_sprintf_checked(char *destination,int flag,size_t capacity,const char *format,const unsigned char *arguments) {
    Word output=formatted_c_string(format,arguments);
    const char *bytes=string_bytes(output,"sprintf_chk");
    size_t length=strlen(bytes);
    if (length>=capacity) stop("Fortified formatting exceeds buffer",format);
    memcpy(destination,bytes,length+1);
    free((void *)output);
    return (int)length;
}

/* Every scanf variadic argument is a pointer. Apple arm64 stores these in
 * consecutive eight-byte stack slots; Android AAPCS64 va_list can consume the
 * same slots with its register-save areas marked exhausted. Native v*scanf
 * handles all formats, suppression, positional arguments and %n itself. */
struct AndroidScanArguments { void *stack,*gr_top,*vr_top; int gr_offset,vr_offset; };
static void scan_arguments(va_list *native,const unsigned char *guest) {
    struct AndroidScanArguments arguments={.stack=(void *)guest};
    _Static_assert(sizeof(va_list)==sizeof(arguments),"Android AAPCS64 va_list ABI");
    memcpy(native,&arguments,sizeof(arguments));
}

__attribute__((used,noinline)) static int guest_sscanf(const char *source,const char *format,const unsigned char *arguments) {
    va_list native; scan_arguments(&native,arguments);
    int result=vsscanf(source,format,native); va_end(native); return result;
}

__attribute__((used,noinline)) static int guest_fscanf(FILE *source,const char *format,const unsigned char *arguments) {
    va_list native; scan_arguments(&native,arguments);
    int result=vfscanf(source,format,native); va_end(native); return result;
}
__attribute__((used,noinline)) static int guest_scanf(const char *format,const unsigned char *arguments) { return guest_fscanf(stdin,format,arguments); }

__attribute__((naked)) static void darwin_snprintf(void) { __asm__ volatile("mov x3,sp\n b guest_vsnprintf"); }
__attribute__((naked)) static void darwin_sprintf(void) { __asm__ volatile("mov x2,sp\n b guest_vsprintf"); }
__attribute__((naked)) static void darwin_fprintf(void) { __asm__ volatile("mov x2,sp\n b guest_fprintf"); }
__attribute__((naked)) static void darwin_printf(void) { __asm__ volatile("mov x1,sp\n b guest_printf"); }
__attribute__((naked)) static void darwin_sprintf_checked(void) { __asm__ volatile("mov x4,sp\n b guest_sprintf_checked"); }
__attribute__((naked)) static void darwin_sscanf(void) { __asm__ volatile("mov x2,sp\n b guest_sscanf"); }
__attribute__((naked)) static void darwin_fscanf(void) { __asm__ volatile("mov x2,sp\n b guest_fscanf"); }
__attribute__((naked)) static void darwin_scanf(void) { __asm__ volatile("mov x1,sp\n b guest_scanf"); }

struct BundleStream { FILE *stream; int fd; struct BundleStream *next; };
static struct BundleStream *bundle_streams;
static pthread_mutex_t bundle_streams_mutex=PTHREAD_MUTEX_INITIALIZER;
static int bundle_stream_read(void *context,char *bytes,int length){return (int)io_read(((struct BundleStream *)context)->fd,bytes,(size_t)length);}
static fpos_t bundle_stream_seek(void *context,fpos_t offset,int origin){return io_seek(((struct BundleStream *)context)->fd,offset,origin);}
static int bundle_stream_close(void *context){
    struct BundleStream *entry=context;pthread_mutex_lock(&bundle_streams_mutex);struct BundleStream **slot=&bundle_streams;
    while(*slot && *slot!=entry)slot=&(*slot)->next;if(*slot)*slot=entry->next;pthread_mutex_unlock(&bundle_streams_mutex);
    int result=io_close(entry->fd);free(entry);return result;
}
static FILE *guest_fdopen(int fd,const char *mode){
    struct BundleFile *file=bundle_fd_acquire(fd);if(!file)return fdopen(fd,mode);bundle_file_release(file);
    if(!mode || *mode!='r' || strchr(mode,'+')){errno=EACCES;return NULL;}
    struct BundleStream *entry=calloc(1,sizeof(*entry));if(!entry){errno=ENOMEM;return NULL;}entry->fd=fd;
    entry->stream=funopen(entry,bundle_stream_read,NULL,bundle_stream_seek,bundle_stream_close);
    if(!entry->stream){free(entry);return NULL;}pthread_mutex_lock(&bundle_streams_mutex);entry->next=bundle_streams;bundle_streams=entry;pthread_mutex_unlock(&bundle_streams_mutex);return entry->stream;
}
static int guest_fileno(FILE *stream){pthread_mutex_lock(&bundle_streams_mutex);struct BundleStream *entry=bundle_streams;while(entry && entry->stream!=stream)entry=entry->next;int fd=entry ? entry->fd : -1;pthread_mutex_unlock(&bundle_streams_mutex);return fd>=0 ? fd : fileno(stream);}
static FILE *guest_fopen(const char *path,const char *mode) {
    if (!path || !mode || !*mode) { errno=EINVAL; return NULL; }
    int writing=*mode!='r' || strchr(mode,'+')!=NULL;
    if (writing && !writable_sandbox_path(path)) return NULL;
    FILE *stream;
    if(bundle_relative(path)){int fd=io_open(path,O_RDONLY|O_CLOEXEC,0);if(fd<0)return NULL;stream=guest_fdopen(fd,mode);if(!stream){int code=errno;io_close(fd);errno=code;}}
    else stream=fopen(path,mode);
    LOG_LIMITED(16,"fopen %s mode=%s result=%p",path,mode,stream);
    return stream;
}
static FILE *guest_freopen(const char *path,const char *mode,FILE *stream) {
    if (!mode || !*mode) { errno=EINVAL; return NULL; }
    if (!path) return freopen(NULL,mode,stream);
    int writing=*mode!='r' || strchr(mode,'+')!=NULL;
    if (writing && !writable_sandbox_path(path)) return NULL;
    char *native=native_path(path);
    if (!native) return NULL;
    FILE *result=freopen(native,mode,stream); free(native); return result;
}
static FILE *guest_standard_streams[3];
static Word resolve_stdio_import(const char *name) {
    if (!strcmp(name,"_popen")) return (Word)popen;
    if (!strcmp(name,"_pclose")) return (Word)pclose;
    if (!strcmp(name,"___stdinp")) { guest_standard_streams[0]=stdin; return (Word)&guest_standard_streams[0]; }
    if (!strcmp(name,"___stdoutp")) { guest_standard_streams[1]=stdout; return (Word)&guest_standard_streams[1]; }
    if (!strcmp(name,"___stderrp")) { guest_standard_streams[2]=stderr; return (Word)&guest_standard_streams[2]; }
    if (!strcmp(name,"_fopen")) return (Word)guest_fopen;
    if (!strcmp(name,"_freopen")) return (Word)guest_freopen;
    if (!strcmp(name,"_fdopen")) return (Word)guest_fdopen;
    if (!strcmp(name,"_fileno")) return (Word)guest_fileno;
    if (!strcmp(name,"_fputs")) return (Word)fputs;
    if (!strcmp(name,"_fputc")) return (Word)fputc;
    if (!strcmp(name,"_putc")) return (Word)putc;
    if (!strcmp(name,"_putchar")) return (Word)putchar;
    if (!strcmp(name,"_vsnprintf")) return (Word)guest_vsnprintf;
    if (!strcmp(name,"_vsprintf")) return (Word)guest_vsprintf;
    if (!strcmp(name,"_snprintf")) return (Word)darwin_snprintf;
    if (!strcmp(name,"_sprintf")) return (Word)darwin_sprintf;
    if (!strcmp(name,"___sprintf_chk")) return (Word)darwin_sprintf_checked;
    if (!strcmp(name,"_fprintf")) return (Word)darwin_fprintf;
    if (!strcmp(name,"_printf")) return (Word)darwin_printf;
    if (!strcmp(name,"_sscanf")) return (Word)darwin_sscanf;
    if (!strcmp(name,"_fscanf")) return (Word)darwin_fscanf;
    if (!strcmp(name,"_scanf")) return (Word)darwin_scanf;
    if (!strcmp(name,"_vsscanf")) return (Word)guest_sscanf;
    if (!strcmp(name,"_vfscanf")) return (Word)guest_fscanf;
    if (!strcmp(name,"_vscanf")) return (Word)guest_scanf;
    return 0;
}
