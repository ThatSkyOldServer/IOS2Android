/* Foundation scheduling uses Android's main Handler and condition-backed
 * native runloops. Guest callbacks remain on their requested thread. */
struct ScheduledWork;
struct NativeLoop { Word object; unsigned ports; int failed; struct NativeLoop *next; };
struct ScheduledWork {
    struct HostObject object;
    Word target,argument,block,info;
    const char *selector;
    struct NativeLoop *loop;
    double deadline,interval,tolerance;
    unsigned generation;
    int timer,valid,scheduled,running,done,cancelled,external_source;
    void (*function)(void *),(*cleanup)(void *);
    void *context;
    unsigned replay;
    intptr_t order;
    struct ScheduledWork *next;
};
struct NativeThread {
    Word object,target,argument,block,name,loop;
    const char *selector;
    pthread_t handle;
    size_t stack;
    int started,executing,finished,cancelled,failed;
    struct NativeThread *next;
};
static pthread_mutex_t scheduling_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scheduling_condition=PTHREAD_COND_INITIALIZER;
static struct NativeLoop *native_loops;
static struct ScheduledWork *scheduled_work;
static struct NativeThread *native_threads;
static _Thread_local struct NativeThread *active_native_thread;
static double scheduling_now(void) { struct timespec value; clock_gettime(CLOCK_REALTIME,&value); return value.tv_sec+value.tv_nsec*1e-9; }
static struct NativeLoop *native_loop(Word object) {
    pthread_mutex_lock(&scheduling_mutex);
    struct NativeLoop *loop=native_loops;
    while (loop && loop->object!=object) loop=loop->next;
    if (!loop) {
        loop=calloc(1,sizeof(*loop));
        if (!loop) { pthread_mutex_unlock(&scheduling_mutex); stop("Allocation failed","NSRunLoop"); }
        loop->object=object; loop->next=native_loops; native_loops=loop;
    }
    pthread_mutex_unlock(&scheduling_mutex); return loop;
}
static int scheduling_mode(Word mode) {
    if (!mode) return 0;
    const char *name=string_bytes(mode,"Runloop mode");
    return !strcmp(name,"kCFRunLoopDefaultMode") || !strcmp(name,"kCFRunLoopCommonModes") || !strcmp(name,"NSDefaultRunLoopMode") || !strcmp(name,"NSRunLoopCommonModes");
}
static void scheduling_modes(Word modes,const char *selector) {
    if (!modes) return;
    struct Sequence *array=(void *)modes;
    if (!array->count) stop("Empty runloop mode list",selector);
    for (size_t i=0;i<array->count;++i) if (!scheduling_mode(array->items[i])) stop("Unsupported runloop mode",selector);
}
static void schedule_main_work(struct ScheduledWork *work) {
    pthread_mutex_lock(&scheduling_mutex);
    int valid=work->valid && work->scheduled && work->loop && work->loop->object==(Word)&main_run_loop;
    unsigned generation=work->generation;
    double delay=fmax(0,work->deadline-scheduling_now());
    pthread_mutex_unlock(&scheduling_mutex);
    if (!valid) return;
    JNIEnv *env=attached_environment("Main delayed work");
    jclass type=(*env)->GetObjectClass(env,activity);
    jmethodID schedule=(*env)->GetMethodID(env,type,"scheduleGuestWork","(JIJ)V");
    (*env)->CallVoidMethod(env,activity,schedule,(jlong)(Word)work,(jint)generation,(jlong)ceil(fmin(delay,1e12)*1000));
    (*env)->DeleteLocalRef(env,type);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); stop("Android scheduling failed","scheduleGuestWork"); }
}
static struct ScheduledWork *work_create(void) {
    struct ScheduledWork *work=calloc(1,sizeof(*work));
    if (!work) stop("Allocation failed","Delayed work");
    work->valid=1;
    return work;
}
static void work_publish(struct ScheduledWork *work) {
    pthread_mutex_lock(&scheduling_mutex);
    work->next=scheduled_work; scheduled_work=work;
    pthread_cond_broadcast(&scheduling_condition);
    pthread_mutex_unlock(&scheduling_mutex);
    schedule_main_work(work);
}
static Word loop_function_create(Word loop,void (*function)(void *),void *context,void (*cleanup)(void *)) {
    struct ScheduledWork *work=work_create();work->external_source=1;work->loop=native_loop(loop);
    work->function=function;work->context=context;work->cleanup=cleanup;
    pthread_mutex_lock(&scheduling_mutex);++work->loop->ports;pthread_mutex_unlock(&scheduling_mutex);
    work_publish(work);return (Word)work;
}
static void loop_function_signal(Word object) {
    struct ScheduledWork *work=(void *)object;if(!work)return;
    pthread_mutex_lock(&scheduling_mutex);
    int submit=work->valid && !work->scheduled;
    if(work->valid){if(work->running)work->replay=1;else if(!work->scheduled){work->scheduled=1;work->deadline=scheduling_now();++work->generation;}}
    pthread_cond_broadcast(&scheduling_condition);pthread_mutex_unlock(&scheduling_mutex);
    if(submit)schedule_main_work(work);
}
static void loop_function_close(Word object) {
    struct ScheduledWork *work=(void *)object;if(!work)return;
    void (*cleanup)(void *)=NULL;void *context=NULL;
    pthread_mutex_lock(&scheduling_mutex);
    if(work->valid){work->valid=work->scheduled=work->replay=0;++work->generation;if(work->loop->ports)--work->loop->ports;}
    if(!work->running){cleanup=work->cleanup;context=work->context;work->cleanup=NULL;work->context=NULL;}
    pthread_cond_broadcast(&scheduling_condition);pthread_mutex_unlock(&scheduling_mutex);
    if(cleanup)cleanup(context);
}
static int runtime_block(Word object) {
    if (!object) return 0;
    Word cls=read_word(object);
    return cls==(Word)_NSConcreteStackBlock || cls==(Word)_NSConcreteMallocBlock || cls==(Word)_NSConcreteGlobalBlock;
}
static Word block_copy_message(Word object) { return (Word)_Block_copy((void *)object); }
static Word block_invoke_message(Word object) { ((void (*)(Word))read_word(object+16))(object); return 0; }
static void work_invoke(struct ScheduledWork *work) {
    uint64_t diagnostic_start=render_diagnostic_clock();
    uint64_t diagnostic_work[14],diagnostic_calls[14];
    struct timespec diagnostic_cpu={0};
    if (diagnostic_start) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&diagnostic_cpu);
    if (diagnostic_start) { memcpy(diagnostic_work,render_cpu_work,sizeof(diagnostic_work)); memcpy(diagnostic_calls,render_cpu_calls,sizeof(diagnostic_calls)); }
    /* A runloop turn gets its own selective Metal pool, even when the guest
     * has a long-lived outer thread pool. Tokens/entry arrays are reused. */
    Word host_pool=pool_push();
    if(work->function)work->function(work->context);
    else if (work->block) ((void (*)(Word,Word))read_word(work->block+16))(work->block,(Word)work);
    else {
        Method method=find_implementation(work->target,work->selector);
        if (!method) stop("Unimplemented scheduled selector",work->selector);
        method(work->target,work->selector,work->timer ? (Word)work : work->argument,0,0,0);
    }
    pool_pop(host_pool);
    if (diagnostic_start) {
        uint64_t elapsed=render_diagnostic_clock()-diagnostic_start;
        struct timespec diagnostic_cpu_end; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&diagnostic_cpu_end);
        double diagnostic_cpu_ms=(diagnostic_cpu_end.tv_sec-diagnostic_cpu.tv_sec)*1000.0+(diagnostic_cpu_end.tv_nsec-diagnostic_cpu.tv_nsec)*1e-6;
        static unsigned logged;
        unsigned sample=elapsed>=10000000ULL ? __atomic_fetch_add(&logged,1,__ATOMIC_RELAXED) : UINT_MAX;
        if (sample<24 || (sample!=UINT_MAX && sample%60==0))
            LOG("[DIAG-render] Runloop callback: tid=%d selector=%s elapsed=%.3fms mutex=%.3fms cond=%.3fms commandWait=%.3fms lookup=%.3fms messages=%llu submit=%.3fms present=%.3fms acquire=%.3fms transfer=%.3fms queueLock=%.3fms uploadPrepare=%.3fms passSetup=%.3fms bufferAllocate=%.3fms commandBegin=%.3fms commandCommit=%.3fms threadCPU=%.3fms",gettid(),work->selector ? work->selector : "block",elapsed*1e-6,
                (render_cpu_work[0]-diagnostic_work[0])*1e-6,(render_cpu_work[1]-diagnostic_work[1])*1e-6,
                (render_cpu_work[2]-diagnostic_work[2])*1e-6,(render_cpu_work[3]-diagnostic_work[3])*1e-6,
                (unsigned long long)(render_cpu_calls[3]-diagnostic_calls[3]),
                (render_cpu_work[4]-diagnostic_work[4])*1e-6,(render_cpu_work[5]-diagnostic_work[5])*1e-6,
                (render_cpu_work[6]-diagnostic_work[6])*1e-6,(render_cpu_work[7]-diagnostic_work[7])*1e-6,
                (render_cpu_work[8]-diagnostic_work[8])*1e-6,(render_cpu_work[9]-diagnostic_work[9])*1e-6,(render_cpu_work[10]-diagnostic_work[10])*1e-6,
                (render_cpu_work[11]-diagnostic_work[11])*1e-6,(render_cpu_work[12]-diagnostic_work[12])*1e-6,
                (render_cpu_work[13]-diagnostic_work[13])*1e-6,diagnostic_cpu_ms);
    }
}
static void work_complete(struct ScheduledWork *work,int failed) {
    pthread_mutex_lock(&scheduling_mutex);
    work->running=0; work->done=1;
    if (work->external_source) {
        work->scheduled=work->valid && work->replay;work->replay=0;
        if(work->scheduled){work->deadline=scheduling_now();++work->generation;}
        if (failed) { if(work->valid && work->function && work->loop->ports)--work->loop->ports;work->valid=work->scheduled=0; if (work->loop) work->loop->failed=1; }
        void (*cleanup)(void *)=!work->valid ? work->cleanup : NULL;void *context=work->context;
        if(cleanup){work->cleanup=NULL;work->context=NULL;}
        pthread_cond_broadcast(&scheduling_condition);
        pthread_mutex_unlock(&scheduling_mutex);
        if(cleanup)cleanup(context);
        schedule_main_work(work);
        return;
    }
    if (failed || !work->timer || !work->interval) work->valid=0;
    if (failed && work->loop) work->loop->failed=1;
    if (work->valid && work->scheduled) {
        double now=scheduling_now();
        if (work->deadline<=now) work->deadline+=(floor((now-work->deadline)/work->interval)+1)*work->interval;
        ++work->generation;
    }
    pthread_cond_broadcast(&scheduling_condition);
    pthread_mutex_unlock(&scheduling_mutex);
    schedule_main_work(work);
}
static int work_claim(struct ScheduledWork *work,unsigned generation) {
    pthread_mutex_lock(&scheduling_mutex);
    int valid=work->valid && work->scheduled && !work->running && work->generation==generation && !work->loop->failed;
    if (valid) work->running=1;
    pthread_mutex_unlock(&scheduling_mutex); return valid;
}
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_runGuestWork)(JNIEnv *env,jobject instance,jlong token,jint generation) {
    struct ScheduledWork *work=(void *)token;
    if (!work_claim(work,(unsigned)generation)) return NULL;
    may_unwind=1; snprintf(stage,sizeof(stage),"Scheduled %s",work->selector ? work->selector : "timer block");
    Word parent_pool=active_pool;
    int failed=setjmp(boundary);
    if (!failed) work_invoke(work);
    while (active_pool!=parent_pool) pool_pop(active_pool);
    work_complete(work,failed); may_unwind=0;
    return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static Word loop_run_mode(Word object,const char *selector,Word mode,Word date) {
    if (object!=core_current_runloop()) stop("Runloop entered from a different thread",selector);
    if (!scheduling_mode(mode)) stop("Unsupported runloop mode",selector);
    if (object==(Word)&main_run_loop) stop("Nested Android main runloop is not implemented",selector);
    struct NativeLoop *loop=native_loop(object);
    double until=date ? date_seconds(date) : scheduling_now();
    for (;;) {
        pthread_mutex_lock(&scheduling_mutex);
        struct ScheduledWork *candidate=NULL;
        int has_source=loop->ports!=0;
        double now=scheduling_now();
        for (struct ScheduledWork *work=scheduled_work;work;work=work->next) if (work->loop==loop && work->valid && work->scheduled && !work->running) {
            has_source=1; if (!candidate || (work->external_source && candidate->external_source && work->deadline<=now && candidate->deadline<=now ? work->order<candidate->order : work->deadline<candidate->deadline)) candidate=work;
        }
        if (loop->failed) { pthread_mutex_unlock(&scheduling_mutex); stop("Runloop callback failed",selector); }
        if (!has_source || now>=until) { pthread_mutex_unlock(&scheduling_mutex); return has_source; }
        if (candidate && candidate->deadline<=now) {
            candidate->running=1; pthread_mutex_unlock(&scheduling_mutex);
            work_invoke(candidate); work_complete(candidate,0); return 1;
        }
        double deadline=candidate ? fmin(until,candidate->deadline) : until;
        struct timespec timeout={(time_t)floor(deadline),(long)((deadline-floor(deadline))*1e9)};
        pthread_cond_timedwait(&scheduling_condition,&scheduling_mutex,&timeout);
        pthread_mutex_unlock(&scheduling_mutex);
    }
}
static void loop_run_until(Word object,const char *selector,Word date) {
    Word mode=copy_string("kCFRunLoopDefaultMode");
    while (date_seconds(date)>scheduling_now() && loop_run_mode(object,selector,mode,date)) {}
}
static void loop_run(Word object,const char *selector) { loop_run_until(object,selector,date_epoch(external_class("_OBJC_CLASS_$_NSDate"),selector,64092211200.0)); }
static Word loop_port(Word cls) {
    struct HostObject *port=(void *)host_allocate(cls); port->content[0]=1;
    return (Word)port;
}
static void loop_add_port(Word object,const char *selector,struct HostObject *port,Word mode) {
    if (!port || !port->content[0] || !scheduling_mode(mode)) stop("Unsupported runloop port",selector);
    struct NativeLoop *loop=native_loop(object);
    pthread_mutex_lock(&scheduling_mutex);
    if (!port->content[1]) { port->content[1]=(Word)loop; ++loop->ports; }
    else if (port->content[1]!=(Word)loop) { pthread_mutex_unlock(&scheduling_mutex); stop("Port is already scheduled on another runloop",selector); }
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex);
}
static void loop_remove_port(Word object,const char *selector,struct HostObject *port,Word mode) {
    pthread_mutex_lock(&scheduling_mutex);
    struct NativeLoop *loop=(void *)port->content[1];
    if (loop && loop->object==object) { if (loop->ports) --loop->ports; port->content[1]=0; }
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex);
}
static void loop_port_invalidate(struct HostObject *port,const char *selector) {
    struct NativeLoop *loop=(void *)port->content[1];
    if (loop) loop_remove_port(loop->object,selector,port,0);
    port->content[0]=0;
}
static Word loop_port_valid(struct HostObject *port) { return port->content[0]; }
static struct NativeThread *native_thread(Word object) {
    pthread_mutex_lock(&scheduling_mutex);
    struct NativeThread *thread=native_threads;
    while (thread && thread->object!=object) thread=thread->next;
    if (!thread) {
        thread=calloc(1,sizeof(*thread));
        if (!thread) { pthread_mutex_unlock(&scheduling_mutex); stop("Allocation failed","NSThread"); }
        thread->object=object; thread->stack=1024*1024;
        if (object==(Word)&main_thread) { thread->started=thread->executing=1; thread->loop=core_main_runloop(); }
        thread->next=native_threads; native_threads=thread;
    }
    pthread_mutex_unlock(&scheduling_mutex); return thread;
}
static Word native_current_thread(Word cls) {
    if (gettid()==getpid()) {
        Word object=get_main_thread(cls); native_thread(object)->handle=pthread_self(); return object;
    }
    if (!active_native_thread) {
        active_native_thread=native_thread(host_allocate(cls));
        active_native_thread->handle=pthread_self(); active_native_thread->started=active_native_thread->executing=1;
        active_native_thread->loop=core_current_runloop();
    }
    return active_native_thread->object;
}
static Word native_thread_init(Word object,const char *selector,Word target,const char *action,Word argument) {
    struct NativeThread *thread=native_thread(object); thread->target=target; thread->selector=action; thread->argument=argument; return object;
}
static void native_thread_start(Word,const char *);
static void native_thread_detach(Word cls,const char *selector,const char *action,Word target,Word argument) {
    if (!target || !action) stop("Invalid detached thread target",selector);
    Word object=host_allocate(cls); native_thread_init(object,"initWithTarget:selector:object:",target,action,argument); native_thread_start(object,"start");
}
static void perform_background(Word object,const char *selector,const char *action,Word argument) { native_thread_detach(external_class("_OBJC_CLASS_$_NSThread"),selector,action,object,argument); }
static void *native_thread_entry(void *context) {
    struct NativeThread *thread=context; active_native_thread=thread;
    current_thread_record();
    attached_environment("NSThread entry");
#if I2A_DEVELOPMENT
    LOG("[DIAG-render] NSThread identity: tid=%d selector=%s",gettid(),thread->selector ? thread->selector : "main");
#endif
    pthread_mutex_lock(&scheduling_mutex);
    thread->loop=core_current_runloop(); thread->handle=pthread_self(); thread->executing=1;
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex);
    struct NativeLoop *loop=native_loop(thread->loop);
    if (thread->name) guest_thread_name(string_bytes(thread->name,"NSThread name"));
    may_unwind=1;fatal_guest_thread=1;snprintf(stage,sizeof(stage),"NSThread %s",thread->selector ? thread->selector : "main");
    int failed=setjmp(boundary);
    if (!failed) {
        if (thread->block) ((void (*)(Word))read_word(thread->block+16))(thread->block);
        else {
            Word target=thread->target ? thread->target : thread->object;
            const char *selector=thread->selector ? thread->selector : "main";
            Method method=find_implementation(target,selector);
            if (!method) stop("Unimplemented thread entry",selector);
            method(target,selector,thread->argument,0,0,0);
        }
    }
    if(failed==1)abort();
    may_unwind=0;fatal_guest_thread=0;
    pthread_mutex_lock(&scheduling_mutex);
    thread->failed=0; thread->executing=0; thread->finished=1; loop->failed=1;
    for (struct ScheduledWork *work=scheduled_work;work;work=work->next) if (work->loop==loop) { work->valid=0; work->done=1; work->running=0; }
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex);
    return NULL;
}
static void native_thread_start(Word object,const char *selector) {
    struct NativeThread *thread=native_thread(object);
    pthread_mutex_lock(&scheduling_mutex);
    if (thread->started) { pthread_mutex_unlock(&scheduling_mutex); stop("Thread already started",selector); }
    thread->started=1; pthread_mutex_unlock(&scheduling_mutex);
    pthread_attr_t attributes; pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes,PTHREAD_CREATE_DETACHED);
    /* Android ART/JNI and liblog share this native stack with guest frames.
     * On Android 16 the original 1 MiB programThreadEntry stack overflows in
     * liblog even after the Vulkan compiler has been moved off-thread. Keep
     * NSThread's requested value while reserving room for the host frames. */
    size_t host_stack=thread->stack<4U*1024U*1024U ? 4U*1024U*1024U : thread->stack;
    int result=pthread_attr_setstacksize(&attributes,host_stack);
    if (!result) result=pthread_attr_setguardsize(&attributes,64U*1024U);
    if (render_diagnostics_requested) LOG("[DIAG-thread] NSThread stack requested=%zu host=%zu selector=%s",thread->stack,host_stack,thread->selector ? thread->selector : "main");
    if (!result) result=pthread_create(&thread->handle,&attributes,native_thread_entry,thread);
    pthread_attr_destroy(&attributes);
    if (result) stop("Native thread creation failed",strerror(result));
}
static void native_thread_stack(Word object,const char *selector,Word size) {
    struct NativeThread *thread=native_thread(object);
    if (thread->started || size<PTHREAD_STACK_MIN || size%4096) stop("Invalid thread stack size",selector);
    thread->stack=size;
}
static Word native_thread_property(Word object,const char *selector) {
    struct NativeThread *thread=native_thread(object);
    if (!strcmp(selector,"isMainThread")) return object==(Word)&main_thread;
    if (!strcmp(selector,"stackSize")) return thread->stack;
    if (!strcmp(selector,"name")) return thread->name;
    if (!strcmp(selector,"isExecuting")) return __atomic_load_n(&thread->executing,__ATOMIC_ACQUIRE);
    if (!strcmp(selector,"isFinished")) return __atomic_load_n(&thread->finished,__ATOMIC_ACQUIRE);
    return __atomic_load_n(&thread->cancelled,__ATOMIC_ACQUIRE);
}
static void native_thread_cancel(Word object) { __atomic_store_n(&native_thread(object)->cancelled,1,__ATOMIC_RELEASE); }
static void native_thread_name(Word object,const char *selector,Word name) {
    struct NativeThread *thread=native_thread(object); thread->name=name;
    const char *text=name ? string_bytes(name,selector) : "";
    if (object==native_current_thread(external_class("_OBJC_CLASS_$_NSThread"))) guest_thread_name(text);
    else if (thread->executing) { char shortened[16]; snprintf(shortened,sizeof(shortened),"%.15s",text); pthread_setname_np(thread->handle,shortened); }
}
static Word thread_is_main(void) { return gettid()==getpid(); }
static Word thread_is_multiple(void) { return 1; /* Android and libdispatch already own worker threads. */ }
static void thread_sleep(Word object,const char *selector,double seconds) {
    if (!isfinite(seconds) || seconds<=0) return;
    struct timespec delay={(time_t)seconds,(long)((seconds-floor(seconds))*1e9)};
    while (nanosleep(&delay,&delay) && errno==EINTR) {}
}
static void thread_sleep_until(Word object,const char *selector,Word date) { thread_sleep(object,selector,date_seconds(date)-scheduling_now()); }
static struct ScheduledWork *perform_enqueue(Word target,const char *action,Word argument,Word loop,double delay) {
    if (!action || !target || !isfinite(delay)) stop("Invalid perform request","performSelector");
    struct ScheduledWork *work=work_create();
    work->target=runtime_block(target) ? (Word)_Block_copy((void *)target) : target;
    work->selector=action; work->argument=runtime_block(argument) ? (Word)_Block_copy((void *)argument) : argument;
    work->loop=native_loop(loop); work->deadline=scheduling_now()+fmax(0,delay); work->scheduled=1;
    work_publish(work); return work;
}
static void perform_delayed(Word object,const char *selector,const char *action,Word argument,double delay) {
    perform_enqueue(object,action,argument,core_current_runloop(),delay);
}
static void perform_delayed_modes(Word object,const char *selector,const char *action,Word argument,double delay,Word modes) {
    scheduling_modes(modes,selector); perform_delayed(object,selector,action,argument,delay);
}
static void perform_cancel(Word object,const char *selector,Word target,const char *action,Word argument) {
    Word current=core_current_runloop();
    int filtered=strstr(selector,":selector:")!=NULL;
    pthread_mutex_lock(&scheduling_mutex);
    for (struct ScheduledWork *work=scheduled_work;work;work=work->next) {
        if (work->timer || work->target!=target || !work->loop || work->loop->object!=current || work->running) continue;
        if (filtered && (strcmp(work->selector,action) || !same_objects(work->argument,argument))) continue;
        work->valid=0; work->cancelled=1; work->done=1; ++work->generation;
    }
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex);
}
static void perform_cancel_all(Word object,const char *selector,Word target) { perform_cancel(object,selector,target,NULL,0); }
static void perform_thread(Word object,const char *selector,const char *action,Word thread_object,Word argument,unsigned char wait) {
    struct NativeThread *thread=native_thread(thread_object);
    if (thread_object==native_current_thread(external_class("_OBJC_CLASS_$_NSThread")) && wait) {
        Method method=find_implementation(object,action);
        if (!method) stop("Unimplemented perform selector",action);
        method(object,action,argument,0,0,0); return;
    }
    pthread_mutex_lock(&scheduling_mutex);
    if (!thread->started) { pthread_mutex_unlock(&scheduling_mutex); stop("Target thread has not started",selector); }
    while (!thread->loop && !thread->finished) pthread_cond_wait(&scheduling_condition,&scheduling_mutex);
    int unavailable=thread->finished || thread->failed; Word loop=thread->loop;
    pthread_mutex_unlock(&scheduling_mutex);
    if (unavailable) stop("Target thread has finished",selector);
    struct ScheduledWork *work=perform_enqueue(object,action,argument,loop,0);
    if (wait) {
        pthread_mutex_lock(&scheduling_mutex);
        while (!work->done && !thread->finished && !work->loop->failed) pthread_cond_wait(&scheduling_condition,&scheduling_mutex);
        int failed=thread->failed || work->loop->failed;
        pthread_mutex_unlock(&scheduling_mutex);
        if (failed) stop("Target thread callback failed",action);
    }
}
static void perform_thread_modes(Word object,const char *selector,const char *action,Word thread,Word argument,unsigned char wait,Word modes) {
    scheduling_modes(modes,selector); perform_thread(object,selector,action,thread,argument,wait);
}
static void perform_main(Word object,const char *selector,const char *action,Word argument,unsigned char wait) {
    perform_thread(object,selector,action,get_main_thread(external_class("_OBJC_CLASS_$_NSThread")),argument,wait);
}
static void perform_main_modes(Word object,const char *selector,const char *action,Word argument,unsigned char wait,Word modes) {
    scheduling_modes(modes,selector); perform_main(object,selector,action,argument,wait);
}
static Word perform_direct(Word object,const char *selector,const char *action,Word first,Word second) {
    Method method=find_implementation(object,action);
    if (!method) stop("Unimplemented perform selector",action);
    return method(object,action,first,second,0,0);
}
static Word perform_one(Word object,const char *selector,const char *action,Word first) { return perform_direct(object,selector,action,first,0); }
static Word perform_zero(Word object,const char *selector,const char *action) { return perform_direct(object,selector,action,0,0); }
static Word foundation_timer_create(Word cls,const char *selector,double interval,Word target,const char *action,Word info,unsigned char repeats) {
    if (!isfinite(interval) || !target || !action) stop("Invalid timer arguments",selector);
    struct ScheduledWork *timer=work_create(); timer->object.isa=cls; timer->timer=1;
    timer->target=target; timer->selector=action; timer->info=info;
    timer->deadline=scheduling_now()+fmax(interval,0.0001); timer->interval=repeats ? fmax(interval,0.0001) : 0;
    timer->object.content[0]=0; memcpy(timer->object.content,&interval,sizeof(interval));
    if (!strncmp(selector,"scheduled",9)) { timer->loop=native_loop(core_current_runloop()); timer->scheduled=1; }
    work_publish(timer); return (Word)timer;
}
static Word timer_init_date(Word object,const char *selector,Word date,double interval,Word target,const char *action,Word info,unsigned char repeats) {
    Word result=foundation_timer_create(read_word(object),selector,interval,target,action,info,repeats);
    ((struct ScheduledWork *)result)->deadline=date_seconds(date); free((void *)object); return result;
}
static void timer_add(Word object,const char *selector,struct ScheduledWork *timer,Word mode) {
    if (!timer || !timer->timer || !scheduling_mode(mode)) stop("Invalid timer scheduling",selector);
    struct NativeLoop *loop=native_loop(object);
    pthread_mutex_lock(&scheduling_mutex);
    if (!timer->valid) { pthread_mutex_unlock(&scheduling_mutex); return; }
    if (timer->loop && timer->loop!=loop) { pthread_mutex_unlock(&scheduling_mutex); stop("Timer already belongs to another runloop",selector); }
    timer->loop=loop; timer->scheduled=1; ++timer->generation;
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex); schedule_main_work(timer);
}
static void timer_invalidate(struct ScheduledWork *timer) {
    pthread_mutex_lock(&scheduling_mutex); timer->valid=0; timer->scheduled=0; ++timer->generation;
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex);
}
static Word timer_valid(struct ScheduledWork *timer) { return __atomic_load_n(&timer->valid,__ATOMIC_ACQUIRE); }
static Word timer_info(struct ScheduledWork *timer) { return timer->info; }
static double timer_interval(struct ScheduledWork *timer) { double value; memcpy(&value,timer->object.content,sizeof(value)); return value; }
static double timer_tolerance(struct ScheduledWork *timer) { return timer->tolerance; }
static void timer_set_tolerance(struct ScheduledWork *timer,const char *selector,double value) { if (isfinite(value) && value>=0) timer->tolerance=value; }
static Word timer_date(struct ScheduledWork *timer,const char *selector) { return date_epoch(external_class("_OBJC_CLASS_$_NSDate"),selector,timer->deadline); }
static void timer_set_date(struct ScheduledWork *timer,const char *selector,Word date) {
    pthread_mutex_lock(&scheduling_mutex); timer->deadline=date_seconds(date); ++timer->generation;
    pthread_cond_broadcast(&scheduling_condition); pthread_mutex_unlock(&scheduling_mutex); schedule_main_work(timer);
}
static void timer_fire(struct ScheduledWork *timer) {
    if (!timer->valid || timer->running) return;
    timer->running=1; work_invoke(timer); timer->running=0;
    if (!timer->interval) timer_invalidate(timer);
}
static Word distant_date(Word cls,const char *selector) { return date_epoch(cls,selector,!strcmp(selector,"distantFuture") ? 64092211200.0 : -62135769600.0); }
static Method scheduling_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!class_receiver && !strcmp(selector,"performSelectorInBackground:withObject:")) return (Method)perform_background;
    if (!strcmp(selector,"cancelPreviousPerformRequestsWithTarget:selector:object:")) return (Method)perform_cancel;
    if (!strcmp(selector,"cancelPreviousPerformRequestsWithTarget:")) return (Method)perform_cancel_all;
    if (!strcmp(selector,"performSelector:withObject:afterDelay:")) return (Method)perform_delayed;
    if (!strcmp(selector,"performSelector:withObject:afterDelay:inModes:")) return (Method)perform_delayed_modes;
    if (!strcmp(selector,"performSelector:onThread:withObject:waitUntilDone:")) return (Method)perform_thread;
    if (!strcmp(selector,"performSelector:onThread:withObject:waitUntilDone:modes:")) return (Method)perform_thread_modes;
    if (!strcmp(selector,"performSelectorOnMainThread:withObject:waitUntilDone:")) return (Method)perform_main;
    if (!strcmp(selector,"performSelectorOnMainThread:withObject:waitUntilDone:modes:")) return (Method)perform_main_modes;
    if (!strcmp(selector,"performSelector:withObject:withObject:")) return (Method)perform_direct;
    if (!strcmp(selector,"performSelector:withObject:")) return (Method)perform_one;
    if (!strcmp(selector,"performSelector:")) return (Method)perform_zero;
    if (runtime_block(object)) {
        if (!strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:")) return (Method)block_copy_message;
        if (!strcmp(selector,"invoke")) return (Method)block_invoke_message;
    }
    if (!receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSThread")) {
        if (class_receiver) {
            if (!strcmp(selector,"mainThread")) return (Method)get_main_thread;
            if (!strcmp(selector,"currentThread")) return (Method)native_current_thread;
            if (!strcmp(selector,"isMainThread")) return (Method)thread_is_main;
            if (!strcmp(selector,"isMultiThreaded")) return (Method)thread_is_multiple;
            if (!strcmp(selector,"sleepForTimeInterval:")) return (Method)thread_sleep;
            if (!strcmp(selector,"sleepUntilDate:")) return (Method)thread_sleep_until;
            if (!strcmp(selector,"detachNewThreadSelector:toTarget:withObject:")) return (Method)native_thread_detach;
        } else {
            if (!strcmp(selector,"initWithTarget:selector:object:")) return (Method)native_thread_init;
            if (!strcmp(selector,"start")) return (Method)native_thread_start;
            if (!strcmp(selector,"setStackSize:")) return (Method)native_thread_stack;
            if (!strcmp(selector,"setName:")) return (Method)native_thread_name;
            if (!strcmp(selector,"cancel")) return (Method)native_thread_cancel;
            if (!strcmp(selector,"name") || !strcmp(selector,"stackSize") || !strcmp(selector,"isMainThread") || !strcmp(selector,"isCancelled") || !strcmp(selector,"isExecuting") || !strcmp(selector,"isFinished")) return (Method)native_thread_property;
        }
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSRunLoop")) {
        if (!strcmp(selector,"runMode:beforeDate:")) return (Method)loop_run_mode;
        if (!strcmp(selector,"runUntilDate:")) return (Method)loop_run_until;
        if (!strcmp(selector,"run")) return (Method)loop_run;
        if (!strcmp(selector,"addPort:forMode:")) return (Method)loop_add_port;
        if (!strcmp(selector,"removePort:forMode:")) return (Method)loop_remove_port;
        if (!strcmp(selector,"addTimer:forMode:")) return (Method)timer_add;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSMachPort") || !strcmp(receiver,"_OBJC_CLASS_$_NSPort")) {
        if (class_receiver && !strcmp(selector,"port")) return (Method)loop_port;
        if (!class_receiver && !strcmp(selector,"invalidate")) return (Method)loop_port_invalidate;
        if (!class_receiver && !strcmp(selector,"isValid")) return (Method)loop_port_valid;
    }
    if (class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSDate") && (!strcmp(selector,"distantFuture") || !strcmp(selector,"distantPast"))) return (Method)distant_date;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSTimer")) {
        if (class_receiver && (!strcmp(selector,"timerWithTimeInterval:target:selector:userInfo:repeats:") || !strcmp(selector,"scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"))) return (Method)foundation_timer_create;
        if (!class_receiver) {
            if (!strcmp(selector,"initWithFireDate:interval:target:selector:userInfo:repeats:")) return (Method)timer_init_date;
            if (!strcmp(selector,"invalidate")) return (Method)timer_invalidate;
            if (!strcmp(selector,"isValid")) return (Method)timer_valid;
            if (!strcmp(selector,"fire")) return (Method)timer_fire;
            if (!strcmp(selector,"userInfo")) return (Method)timer_info;
            if (!strcmp(selector,"timeInterval")) return (Method)timer_interval;
            if (!strcmp(selector,"fireDate")) return (Method)timer_date;
            if (!strcmp(selector,"setFireDate:")) return (Method)timer_set_date;
            if (!strcmp(selector,"tolerance")) return (Method)timer_tolerance;
            if (!strcmp(selector,"setTolerance:")) return (Method)timer_set_tolerance;
        }
    }
    return NULL;
}
