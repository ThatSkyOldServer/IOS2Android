 
static int backtrace_read_frame(Word frame,Word words[2]) {
    if (!frame || (frame&7) || frame>UINTPTR_MAX-16) return 0;
    struct iovec local={words,16},remote={(void *)frame,16};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==16;
}
__attribute__((noinline)) static int darwin_backtrace(void **addresses,int capacity) {
    if (!addresses || capacity<=0) return 0;
    if (capacity>4096) capacity=4096;
    int saved_errno=errno,count=0;
    addresses[count++]=__builtin_return_address(0);
    Word frame=(Word)__builtin_frame_address(0),words[2];
    if (backtrace_read_frame(frame,words)) {
        frame=words[0];
        while (count<capacity && backtrace_read_frame(frame,words)) {
            if (!words[1]) break;
            addresses[count++]=(void *)words[1];
            if (words[0]<=frame || words[0]-frame>64*1024*1024) break;
            frame=words[0];
        }
    }
#if I2A_DEVELOPMENT
    static unsigned logged;
    if (__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<6) {
        LOG("Original diagnostic backtrace: %d frames",count);
        for (int i=0;i<count && i<24;++i) if (in_image((Word)addresses[i]))
            LOG("  guest frame %d: 0x%lx",i,guest_native_address((Word)addresses[i]));
    }
#endif
    errno=saved_errno;
    return count;
}
static char **darwin_backtrace_symbols(void *const *addresses,int count) {
    if (!addresses || count<0 || count>4096) { errno=EINVAL; return NULL; }
    size_t slot=768,bytes=(size_t)count*(sizeof(char *)+slot);
    char **result=malloc(bytes ? bytes : sizeof(char *));
    if (!result) return NULL;
    char *text=(char *)(result+count);
    for (int i=0;i<count;++i) {
        result[i]=text+(size_t)i*slot;
        Word address=(Word)addresses[i];
        if (in_image(address)) {
            snprintf(result[i],slot,"%d I2A 0x%016lx 0x%016lx + %lu",i,address,aslr,address-aslr);
        } else {
            Dl_info info={0}; dladdr(addresses[i],&info);
            const char *file=info.dli_fname ? strrchr(info.dli_fname,'/') : NULL;
            file=file ? file+1 : info.dli_fname ? info.dli_fname : "?";
            Word base=(Word)(info.dli_saddr ? info.dli_saddr : info.dli_fbase);
            snprintf(result[i],slot,"%d %.200s 0x%016lx %.400s + %lu",i,file,address,info.dli_sname ? info.dli_sname : "?",base ? address-base : 0);
        }
    }
    return result;
}
static Word resolve_backtrace_import(const char *name) {
    if (!strcmp(name,"_backtrace")) return (Word)darwin_backtrace;
    if (!strcmp(name,"_backtrace_symbols")) return (Word)darwin_backtrace_symbols;
    return 0;
}
