/* Exception-only output survives release log removal. Signal recording uses
 * fixed buffers and syscalls: no JNI, allocation, stdio, or loader locks. The
 * original Android signal action still receives the original ucontext. */
#include <link.h>
#include <ucontext.h>

static int crash_fd=-1;
static Word crash_module_base,crash_module_end;
static char crash_build_id[129];
static char crash_gpu_context[512];
static unsigned crash_gpu_ready;
static struct sigaction crash_previous[NSIG];
static unsigned crash_signal_active;
static char crash_runtime_bytes[1024*1024],crash_signal_bytes[1024*1024];
static pthread_mutex_t crash_runtime_mutex=PTHREAD_MUTEX_INITIALIZER;
struct CrashOutput { char *bytes; size_t size,capacity; unsigned truncated; };
struct CrashText { char bytes[2048]; size_t size; };
static void crash_text(struct CrashText *text,const char *value) {
    while (*value && text->size<sizeof(text->bytes)) text->bytes[text->size++]=*value++;
}
static void crash_number(struct CrashText *text,uint64_t value,int hexadecimal) {
    char digits[32]; unsigned count=0;
    do { unsigned digit=value%(hexadecimal ? 16 : 10); digits[count++]="0123456789abcdef"[digit]; value/=hexadecimal ? 16 : 10; } while(value);
    if (hexadecimal) crash_text(text,"0x");
    while(count && text->size<sizeof(text->bytes))text->bytes[text->size++]=digits[--count];
}
static void crash_write_bytes(int fd,const char *bytes,size_t size) {
    while(size && fd>=0) {
        ssize_t written=syscall(SYS_write,fd,bytes,size);
        if(written>0){bytes+=written;size-=(size_t)written;}
        else if(written<0 && errno==EINTR)continue;
        else break;
    }
}
static void crash_output_bytes(struct CrashOutput *output,const char *bytes,size_t size) {
    if(size>output->capacity-output->size){size=output->capacity-output->size;output->truncated=1;}
    for(size_t i=0;i<size;++i)output->bytes[output->size+i]=bytes[i];
    output->size+=size;
}
static void crash_output_write(struct CrashOutput *output) {
    if(output->truncated) {
        static const char marker[]="\ncrash_log_limit_reached: output truncated\n";
        output->capacity+=sizeof(marker)-1;crash_output_bytes(output,marker,sizeof(marker)-1);
    }
    crash_write_bytes(crash_fd,output->bytes,output->size);
}
static void crash_flush(struct CrashOutput *output,struct CrashText *text) { crash_output_bytes(output,text->bytes,text->size);text->size=0; }
static void crash_event(struct CrashOutput *output,const char *kind) {
    struct timespec clock={0};clock_gettime(CLOCK_REALTIME,&clock);
    struct CrashText text={0};crash_text(&text,"\n");crash_text(&text,kind);
    crash_text(&text," epoch_ms=");crash_number(&text,(uint64_t)clock.tv_sec*1000+(uint64_t)clock.tv_nsec/1000000,0);
    crash_text(&text," pid=");crash_number(&text,getpid(),0);
    crash_text(&text," tid=");crash_number(&text,syscall(SYS_gettid),0);crash_text(&text,"\n");
    crash_text(&text,"module_base=");crash_number(&text,crash_module_base,1);
    crash_text(&text," guest_base=");crash_number(&text,(Word)guest_text,1);
    crash_text(&text," guest_image_base=");crash_number(&text,GUEST_IMAGE_BASE,1);
    crash_text(&text," build_id=");crash_text(&text,crash_build_id);crash_text(&text,"\n");
    if(__atomic_load_n(&crash_gpu_ready,__ATOMIC_ACQUIRE)){crash_text(&text,crash_gpu_context);crash_text(&text,"\n");}
    crash_flush(output,&text);
}
static void crash_frame(struct CrashOutput *output,unsigned index,Word address) {
    struct CrashText text={0};crash_text(&text,"  #");crash_number(&text,index,0);
    crash_text(&text," pc=");crash_number(&text,address,1);
    if(address>=crash_module_base && address<crash_module_end){crash_text(&text," module=libi2a.so elf_pc=");crash_number(&text,address-crash_module_base,1);}
    if(in_image(address)){crash_text(&text," guest_pc=");crash_number(&text,guest_native_address(address),1);}
    crash_text(&text,"\n");crash_flush(output,&text);
}
static void crash_walk(struct CrashOutput *output,Word pc,Word lr,Word frame,Word stack) {
    unsigned count=0;Word previous=pc;
    if(pc)crash_frame(output,count++,pc);
    if(lr && lr!=pc){crash_frame(output,count++,lr);previous=lr;}
    const char *end="frame_chain_end";
    while(frame && count<4096) {
        Word words[2];
        if(frame<stack || frame-stack>64*1024*1024 || (frame&7) || frame>UINTPTR_MAX-16){end="invalid_frame_pointer";break;}
        struct iovec local={words,sizeof(words)},remote={(void *)frame,sizeof(words)};
        if(syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0)!=sizeof(words)){end="unreadable_frame_pointer";break;}
        if(words[1] && words[1]!=previous){crash_frame(output,count++,words[1]);previous=words[1];}
        if(!words[0])break;
        if(words[0]<=frame || words[0]-frame>64*1024*1024){end="invalid_frame_chain";break;}
        frame=words[0];
    }
    if(count==4096)end="frame_limit_reached";
    struct CrashText text={0};crash_text(&text,"stack_frames=");crash_number(&text,count,0);
    crash_text(&text," stack_end=");crash_text(&text,end);crash_text(&text,"\n");crash_flush(output,&text);
}
static void crash_maps(struct CrashOutput *output) {
    int fd=(int)syscall(SYS_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0);
    if(fd<0)return;
    static const char marker[]="memory_maps:\n";crash_output_bytes(output,marker,sizeof(marker)-1);
    char bytes[2048];ssize_t count;
    while((count=syscall(SYS_read,fd,bytes,sizeof(bytes)))>0) {
        crash_output_bytes(output,bytes,(size_t)count);if(output->truncated)break;
    }
    syscall(SYS_close,fd);
}
static void crash_signal(int number,siginfo_t *info,void *context) {
    int saved_errno=errno;
    int recording=!__atomic_exchange_n(&crash_signal_active,1,__ATOMIC_ACQ_REL);
    if(recording) {
        struct CrashOutput output={.bytes=crash_signal_bytes,.capacity=sizeof(crash_signal_bytes)-64};
        ucontext_t *state=context;struct CrashText text={0};crash_event(&output,"NATIVE_CRASH");
        crash_text(&text,"signal=");crash_number(&text,number,0);
        int64_t code=info ? info->si_code : 0;
        crash_text(&text," code=");if(code<0)crash_text(&text,"-");crash_number(&text,(uint64_t)(code<0 ? -code : code),0);
        crash_text(&text," fault_addr=");crash_number(&text,info && code>0 ? (Word)info->si_addr : 0,1);crash_text(&text,"\n");
        if(state) {
            for(unsigned i=0;i<31;++i){crash_text(&text,"x");crash_number(&text,i,0);crash_text(&text,"=");crash_number(&text,state->uc_mcontext.regs[i],1);crash_text(&text,i%3==2 ? "\n" : " ");}
            crash_text(&text,"\npc=");crash_number(&text,state->uc_mcontext.pc,1);
            crash_text(&text," sp=");crash_number(&text,state->uc_mcontext.sp,1);
            crash_text(&text," pstate=");crash_number(&text,state->uc_mcontext.pstate,1);crash_text(&text,"\n");crash_flush(&output,&text);
            crash_walk(&output,state->uc_mcontext.pc,state->uc_mcontext.regs[30],state->uc_mcontext.regs[29],state->uc_mcontext.sp);
        } else crash_flush(&output,&text);
        crash_maps(&output);crash_output_write(&output);syscall(SYS_fsync,crash_fd);
    }
    errno=saved_errno;
    struct sigaction *previous=&crash_previous[number];
    if((Word)previous->sa_handler>1) {
        if(previous->sa_flags&SA_SIGINFO)previous->sa_sigaction(number,info,context);
        else previous->sa_handler(number);
    } else {
        /* A synchronous fault repeats at its original instruction after return.
         * User-generated signals must be requeued after restoring the action. */
        sigaction(number,previous,NULL);
        if(!info || info->si_code<=0)syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),number);
    }
    if(recording)__atomic_store_n(&crash_signal_active,0,__ATOMIC_RELEASE);
    errno=saved_errno;
}
static int crash_identify_module(struct dl_phdr_info *info,size_t size,void *unused) {
    if((Word)&crash_identify_module<info->dlpi_addr)return 0;
    int contains=0;
    for(unsigned i=0;i<info->dlpi_phnum;++i) {
        const ElfW(Phdr) *header=info->dlpi_phdr+i;
        Word first=info->dlpi_addr+header->p_vaddr,last=first+header->p_memsz;
        if(header->p_type==PT_LOAD && (Word)&crash_identify_module>=first && (Word)&crash_identify_module<last)contains=1;
    }
    if(!contains)return 0;
    crash_module_base=info->dlpi_addr;
    for(unsigned i=0;i<info->dlpi_phnum;++i) {
        const ElfW(Phdr) *header=info->dlpi_phdr+i;
        if(header->p_type==PT_LOAD && info->dlpi_addr+header->p_vaddr+header->p_memsz>crash_module_end)crash_module_end=info->dlpi_addr+header->p_vaddr+header->p_memsz;
        if(header->p_type!=PT_NOTE)continue;
        const unsigned char *cursor=(const void *)(info->dlpi_addr+header->p_vaddr),*end=cursor+header->p_memsz;
        while((size_t)(end-cursor)>=sizeof(ElfW(Nhdr))) {
            ElfW(Nhdr) note;memcpy(&note,cursor,sizeof(note));cursor+=sizeof(note);
            size_t names=((size_t)note.n_namesz+3)&~3UL,bytes=((size_t)note.n_descsz+3)&~3UL;
            if(names>(size_t)(end-cursor) || bytes>(size_t)(end-cursor)-names)break;
            if(note.n_type==NT_GNU_BUILD_ID && note.n_namesz==4 && !memcmp(cursor,"GNU",4) && note.n_descsz<=64) {
                for(unsigned j=0;j<note.n_descsz;++j){unsigned byte=cursor[names+j];crash_build_id[j*2]="0123456789abcdef"[byte>>4];crash_build_id[j*2+1]="0123456789abcdef"[byte&15];}
                crash_build_id[note.n_descsz*2]=0;
            }
            cursor+=names+bytes;
        }
    }
    return 1;
}
JNIEXPORT jint JNICALL HOST_JNI(HostActivity_configureCrashLog)(JNIEnv *env,jclass cls,jstring file) {
    if(crash_fd>=0)return 0;
    const char *path=(*env)->GetStringUTFChars(env,file,NULL);if(!path)return ENOMEM;
    int fd=(int)syscall(SYS_openat,AT_FDCWD,path,O_WRONLY|O_APPEND|O_CLOEXEC,0600),error=fd<0 ? errno : 0;
    (*env)->ReleaseStringUTFChars(env,file,path);if(fd<0)return error;
    dl_iterate_phdr(crash_identify_module,NULL);crash_fd=fd;
    const int signals[]={SIGABRT,SIGBUS,SIGFPE,SIGILL,SIGSEGV,SIGTRAP};
    for(unsigned i=0;i<COUNT(signals);++i) {
        if(sigaction(signals[i],NULL,&crash_previous[signals[i]]))error=errno;
        else {
            struct sigaction action=crash_previous[signals[i]];
            action.sa_sigaction=crash_signal;action.sa_flags|=SA_SIGINFO|SA_ONSTACK;
            if(sigaction(signals[i],&action,NULL))error=errno;
        }
        if(error) {
            while(i){--i;sigaction(signals[i],&crash_previous[signals[i]],NULL);}
            syscall(SYS_close,crash_fd);crash_fd=-1;return error;
        }
    }
    return 0;
}
static __attribute__((noinline)) void record_failure(const char *kind,const char *detail) {
    int saved_errno=errno;
    if(crash_fd>=0) {
        pthread_mutex_lock(&crash_runtime_mutex);
        struct CrashOutput output={.bytes=crash_runtime_bytes,.capacity=sizeof(crash_runtime_bytes)-64};
        crash_event(&output,"RUNTIME_FAILURE");
        crash_output_bytes(&output,kind,strlen(kind));crash_output_bytes(&output,": ",2);
        crash_output_bytes(&output,detail,strlen(detail));crash_output_bytes(&output,"\n",1);
        Word stack;__asm__ volatile("mov %0, sp" : "=r"(stack));
        crash_walk(&output,(Word)__builtin_return_address(0),0,(Word)__builtin_frame_address(0),stack);
        crash_maps(&output);crash_output_write(&output);syscall(SYS_fsync,crash_fd);
        pthread_mutex_unlock(&crash_runtime_mutex);
    }
    char text[1024];snprintf(text,sizeof(text),"%s: %s",kind,detail);
    (__android_log_write)(ANDROID_LOG_ERROR,"I2ACrash",text);
    errno=saved_errno;
}
