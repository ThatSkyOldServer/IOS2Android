/* The guest's syslog variadic ABI is Apple ARM64, not Android va_list. */
static pthread_mutex_t syslog_mutex=PTHREAD_MUTEX_INITIALIZER;
static char syslog_ident[128];
static int syslog_options,syslog_mask=255;
static void guest_openlog(const char *ident,int options,int facility) {
    pthread_mutex_lock(&syslog_mutex);
    snprintf(syslog_ident,sizeof(syslog_ident),"%s",ident ? ident : "");
    syslog_options=options;
    pthread_mutex_unlock(&syslog_mutex);
}
static void guest_closelog(void) { guest_openlog(NULL,0,0); }
static int guest_setlogmask(int mask) {
    pthread_mutex_lock(&syslog_mutex); int previous=syslog_mask;
    if (mask) syslog_mask=mask;
    pthread_mutex_unlock(&syslog_mutex); return previous;
}
__attribute__((used,noinline)) static void guest_syslog(int priority,const char *format,const unsigned char *arguments) {
    if (!I2A_DEVELOPMENT) return;
    int saved=errno,level=priority&7,options;
    char ident[128];
    pthread_mutex_lock(&syslog_mutex);
    int enabled=syslog_mask&(1<<level); options=syslog_options;
    memcpy(ident,syslog_ident,sizeof(ident));
    pthread_mutex_unlock(&syslog_mutex);
    if (!enabled) return;
    char *expanded=NULL; size_t size=0;
    FILE *output=open_memstream(&expanded,&size);
    if (!output) { errno=saved; return; }
    for (const char *p=format;*p;++p) {
        if (p[0]=='%' && p[1]=='%') { fputs("%%",output); ++p; }
        else if (p[0]=='%' && p[1]=='m') {
            for (const char *error=strerror(saved);*error;++error) { fputc(*error,output); if (*error=='%') fputc('%',output); }
            ++p;
        } else fputc(*p,output);
    }
    if (fclose(output)) { free(expanded); errno=saved; return; }
    Word text=formatted_c_string(expanded,arguments); free(expanded);
    const char *bytes=string_bytes(text,"syslog");
    int native_level=level<=3 ? ANDROID_LOG_ERROR : level==4 ? ANDROID_LOG_WARN : level==7 ? ANDROID_LOG_DEBUG : ANDROID_LOG_INFO;
    if (options&1) __android_log_print(native_level,"I2AGuest","syslog %s[%d]: %s",ident,getpid(),bytes);
    else __android_log_print(native_level,"I2AGuest","syslog %s%s%s",ident,*ident ? ": " : "",bytes);
    if (options&32) fprintf(stderr,"%s%s%s\n",ident,*ident ? ": " : "",bytes);
    free((void *)text); errno=saved;
}
__attribute__((naked)) static void darwin_syslog(void) { __asm__ volatile("mov x2,sp\n b guest_syslog"); }
__attribute__((used,noinline)) static int guest_asl_message(int level,const char *format,const unsigned char *arguments) {
    if(level<0 || level>7 || !format)return -1;
    guest_syslog(level,format,arguments);return 0;
}
__attribute__((naked)) static int darwin_asl_message(int level,const char *format,...) { __asm__ volatile("mov x2,sp\n b guest_asl_message"); }
static Word resolve_logging_import(const char *name) {
    if (!strcmp(name,"_asl_log_message")) return (Word)darwin_asl_message;
    if (!strcmp(name,"_syslog")) return (Word)darwin_syslog;
    if (!strcmp(name,"_vsyslog")) return (Word)guest_syslog;
    if (!strcmp(name,"_openlog")) return (Word)guest_openlog;
    if (!strcmp(name,"_closelog")) return (Word)guest_closelog;
    if (!strcmp(name,"_setlogmask")) return (Word)guest_setlogmask;
    return 0;
}
