/* Darwin pthread structures have signatures, different sizes and different
 * enum values. Only their public opaque storage crosses into this bridge. */
#define MUTEX_TAG UINT64_C(0x534b594d55545831)
#define COND_TAG UINT64_C(0x534b59434f4e4431)
#define RWLOCK_TAG UINT64_C(0x534b5952574c4b31)
#define ATTR_TAG UINT64_C(0x534b594154545231)
#define MUTEX_ATTR_TAG UINT64_C(0x534b594d41545431)
struct DarwinMutex { uint64_t tag; pthread_mutex_t native; };
struct DarwinCond { uint64_t tag; pthread_cond_t *native; };
struct DarwinRwlock { uint64_t tag; pthread_rwlock_t native; };
struct DarwinThreadAttr { uint64_t tag; pthread_attr_t *native; };
struct DarwinMutexAttr { uint64_t tag; pthread_mutexattr_t native; };
_Static_assert(sizeof(struct DarwinMutex)<=64,"Darwin mutex storage");
_Static_assert(sizeof(struct DarwinCond)<=48,"Darwin condition storage");
_Static_assert(sizeof(struct DarwinRwlock)<=200,"Darwin rwlock storage");
_Static_assert(sizeof(struct DarwinMutexAttr)<=16,"Darwin mutex attribute storage");
static pthread_mutex_t guest_pthread_init_lock=PTHREAD_MUTEX_INITIALIZER;

static int darwin_pthread_error(int error) {
    switch (error) {
        case EAGAIN: return 35;
        case EDEADLK: return 11;
        case ETIMEDOUT: return 60;
        case ENOTSUP: return 45;
        default: return error;
    }
}

static int guest_mutex_init(void *storage,const void *attributes) {
    struct DarwinMutex *mutex=storage;
    const struct DarwinMutexAttr *attr=attributes;
    if (attr && attr->tag!=MUTEX_ATTR_TAG) return EINVAL;
    int error=pthread_mutex_init(&mutex->native,attr ? &attr->native : NULL);
    if (!error) __atomic_store_n(&mutex->tag,MUTEX_TAG,__ATOMIC_RELEASE);
    return darwin_pthread_error(error);
}

static pthread_mutex_t *native_mutex(struct DarwinMutex *mutex) {
    if (!mutex) return NULL;
    if (__atomic_load_n(&mutex->tag,__ATOMIC_ACQUIRE)==MUTEX_TAG) return &mutex->native;
    pthread_mutex_lock(&guest_pthread_init_lock);
    uint64_t tag=mutex->tag;
    if (tag==0 || tag==0x32aaaba7 || tag==0x32aaaba2 || tag==0x32aaaba1) {
        struct DarwinMutexAttr attr={.tag=MUTEX_ATTR_TAG};
        pthread_mutexattr_init(&attr.native);
        pthread_mutexattr_settype(&attr.native,tag==0x32aaaba2 ? PTHREAD_MUTEX_RECURSIVE : tag==0x32aaaba1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
        guest_mutex_init(mutex,&attr);
        pthread_mutexattr_destroy(&attr.native);
    }
    pthread_mutex_unlock(&guest_pthread_init_lock);
    return mutex->tag==MUTEX_TAG ? &mutex->native : NULL;
}
static int guest_mutex_lock(struct DarwinMutex *mutex) {
    uint64_t diagnostic_start=render_diagnostic_clock();
    pthread_mutex_t *native=native_mutex(mutex);
    int result=native ? darwin_pthread_error(pthread_mutex_lock(native)) : EINVAL;
    render_diagnostic_cost(0,diagnostic_start); return result;
}
static int guest_mutex_trylock(struct DarwinMutex *mutex) {
    pthread_mutex_t *native=native_mutex(mutex);
    return native ? darwin_pthread_error(pthread_mutex_trylock(native)) : EINVAL;
}
static int guest_mutex_unlock(struct DarwinMutex *mutex) {
    pthread_mutex_t *native=native_mutex(mutex);
    return native ? darwin_pthread_error(pthread_mutex_unlock(native)) : EINVAL;
}
static int guest_mutex_destroy(struct DarwinMutex *mutex) {
    pthread_mutex_t *native=native_mutex(mutex);
    if (!native) return EINVAL;
    int result=pthread_mutex_destroy(native);
    if (!result) mutex->tag=UINT64_MAX;
    return darwin_pthread_error(result);
}
static int guest_mutexattr_init(struct DarwinMutexAttr *attr) {
    int error=pthread_mutexattr_init(&attr->native);
    if (!error) attr->tag=MUTEX_ATTR_TAG;
    return darwin_pthread_error(error);
}
static int guest_mutexattr_type(struct DarwinMutexAttr *attr,int type) {
    if (!attr || attr->tag!=MUTEX_ATTR_TAG || type<0 || type>2) return EINVAL;
    return darwin_pthread_error(pthread_mutexattr_settype(&attr->native,type==2 ? PTHREAD_MUTEX_RECURSIVE : type==1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL));
}
static int guest_mutexattr_protocol(struct DarwinMutexAttr *attr,int protocol) {
    if(!attr || attr->tag!=MUTEX_ATTR_TAG || protocol<0 || protocol>2)return EINVAL;
    if(protocol==2)return darwin_pthread_error(ENOTSUP); /* Android has no priority-ceiling mutexes. */
    int native=protocol==0 ? PTHREAD_PRIO_NONE : PTHREAD_PRIO_INHERIT;
    return darwin_pthread_error(pthread_mutexattr_setprotocol(&attr->native,native));
}
static int guest_mutexattr_destroy(struct DarwinMutexAttr *attr) {
    if (!attr || attr->tag!=MUTEX_ATTR_TAG) return EINVAL;
    int error=pthread_mutexattr_destroy(&attr->native);
    attr->tag=0;
    return darwin_pthread_error(error);
}

static int guest_cond_init(struct DarwinCond *cond,const void *attributes) {
    if (attributes) return EINVAL;
    pthread_cond_t *native=malloc(sizeof(*native));
    if (!native) return ENOMEM;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    int error=pthread_cond_init(native,&attr);
    pthread_condattr_destroy(&attr);
    if (error) { free(native); return darwin_pthread_error(error); }
    cond->native=native;
    __atomic_store_n(&cond->tag,COND_TAG,__ATOMIC_RELEASE);
    return 0;
}
static pthread_cond_t *native_cond(struct DarwinCond *cond) {
    if (!cond) return NULL;
    if (__atomic_load_n(&cond->tag,__ATOMIC_ACQUIRE)==COND_TAG) return cond->native;
    pthread_mutex_lock(&guest_pthread_init_lock);
    if (!cond->tag || cond->tag==0x3cb0b1bb) guest_cond_init(cond,NULL);
    pthread_mutex_unlock(&guest_pthread_init_lock);
    return cond->tag==COND_TAG ? cond->native : NULL;
}
static int guest_cond_wait(struct DarwinCond *cond,struct DarwinMutex *mutex) {
    uint64_t diagnostic_start=render_diagnostic_clock();
    pthread_cond_t *c=native_cond(cond); pthread_mutex_t *m=native_mutex(mutex);
    int result=c && m ? darwin_pthread_error(pthread_cond_wait(c,m)) : EINVAL;
    render_diagnostic_cost(1,diagnostic_start); return result;
}
static int guest_cond_relative(struct DarwinCond *cond,struct DarwinMutex *mutex,const struct timespec *duration) {
    pthread_cond_t *c=native_cond(cond); pthread_mutex_t *m=native_mutex(mutex);
    if (!c || !m || !duration || duration->tv_sec<0 || duration->tv_nsec<0 || duration->tv_nsec>=1000000000L) return EINVAL;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC,&deadline);
    if (duration->tv_sec>INT64_MAX-deadline.tv_sec-1) return EINVAL;
    deadline.tv_sec+=duration->tv_sec;
    deadline.tv_nsec+=duration->tv_nsec;
    if (deadline.tv_nsec>=1000000000L) { ++deadline.tv_sec; deadline.tv_nsec-=1000000000L; }
    return darwin_pthread_error(pthread_cond_timedwait(c,m,&deadline));
}
static int guest_cond_signal(struct DarwinCond *cond) { pthread_cond_t *c=native_cond(cond); return c ? darwin_pthread_error(pthread_cond_signal(c)) : EINVAL; }
static int guest_cond_broadcast(struct DarwinCond *cond) { pthread_cond_t *c=native_cond(cond); return c ? darwin_pthread_error(pthread_cond_broadcast(c)) : EINVAL; }
static int guest_cond_destroy(struct DarwinCond *cond) {
    pthread_cond_t *c=native_cond(cond);
    if (!c) return EINVAL;
    int error=pthread_cond_destroy(c);
    if (!error) { free(c); cond->native=NULL; cond->tag=UINT64_MAX; }
    return darwin_pthread_error(error);
}

static int guest_rwlock_init(void *storage,const void *attributes) {
    if (attributes) return EINVAL;
    struct DarwinRwlock *lock=storage;
    int error=pthread_rwlock_init(&lock->native,NULL);
    if (!error) __atomic_store_n(&lock->tag,RWLOCK_TAG,__ATOMIC_RELEASE);
    return darwin_pthread_error(error);
}
static pthread_rwlock_t *native_rwlock(struct DarwinRwlock *lock) {
    if (!lock) return NULL;
    if (__atomic_load_n(&lock->tag,__ATOMIC_ACQUIRE)==RWLOCK_TAG) return &lock->native;
    pthread_mutex_lock(&guest_pthread_init_lock);
    if (!lock->tag || lock->tag==0x2da8b3b4) guest_rwlock_init(lock,NULL);
    pthread_mutex_unlock(&guest_pthread_init_lock);
    return lock->tag==RWLOCK_TAG ? &lock->native : NULL;
}
static int guest_rwlock_read(struct DarwinRwlock *lock) { pthread_rwlock_t *p=native_rwlock(lock); return p ? darwin_pthread_error(pthread_rwlock_rdlock(p)) : EINVAL; }
static int guest_rwlock_write(struct DarwinRwlock *lock) { pthread_rwlock_t *p=native_rwlock(lock); return p ? darwin_pthread_error(pthread_rwlock_wrlock(p)) : EINVAL; }
static int guest_rwlock_unlock(struct DarwinRwlock *lock) { pthread_rwlock_t *p=native_rwlock(lock); return p ? darwin_pthread_error(pthread_rwlock_unlock(p)) : EINVAL; }
static int guest_rwlock_destroy(struct DarwinRwlock *lock) {
    pthread_rwlock_t *p=native_rwlock(lock);
    if (!p) return EINVAL;
    int error=pthread_rwlock_destroy(p);
    if (!error) lock->tag=UINT64_MAX;
    return darwin_pthread_error(error);
}

struct ThreadRecord { pthread_t handle; pid_t tid; char name[64]; struct ThreadRecord *next; };
static struct ThreadRecord *thread_records;
static pthread_mutex_t thread_records_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t thread_record_key;
static pthread_once_t thread_record_once=PTHREAD_ONCE_INIT;
static unsigned thread_record_key_ready;
static void forget_guest_thread(void *value) {
    struct ThreadRecord *record=value;
    pthread_mutex_lock(&thread_records_lock);
    struct ThreadRecord **slot=&thread_records;
    while (*slot && *slot!=record) slot=&(*slot)->next;
    if (*slot) *slot=record->next;
    pthread_mutex_unlock(&thread_records_lock);
    free(record);
}
static void thread_record_key_init(void) {
    int error=pthread_key_create(&thread_record_key,forget_guest_thread);
    __atomic_store_n(&thread_record_key_ready,error ? 2 : 1,__ATOMIC_RELEASE);
}
static struct ThreadRecord *registered_thread(pthread_t handle) {
    for (struct ThreadRecord *record=thread_records;record;record=record->next) if (pthread_equal(record->handle,handle)) return record;
    return NULL;
}
static struct ThreadRecord *current_thread_record(void) {
    if (!__atomic_load_n(&thread_record_key_ready,__ATOMIC_ACQUIRE)) pthread_once(&thread_record_once,thread_record_key_init);
    if (__atomic_load_n(&thread_record_key_ready,__ATOMIC_ACQUIRE)!=1) stop("Thread registration key unavailable","pthread key");
    struct ThreadRecord *record=pthread_getspecific(thread_record_key);
    if (record) return record;
    pthread_t handle=pthread_self();
    pthread_mutex_lock(&thread_records_lock);
    record=registered_thread(handle);
    if (!record) {
        record=calloc(1,sizeof(*record));
        if (!record) { pthread_mutex_unlock(&thread_records_lock); stop("Allocation failed","Thread registration"); }
        record->handle=handle; record->next=thread_records; thread_records=record;
    }
    record->tid=gettid(); pthread_getname_np(handle,record->name,sizeof(record->name));
    pthread_mutex_unlock(&thread_records_lock);
    if (pthread_setspecific(thread_record_key,record)) { forget_guest_thread(record); stop("Thread registration failed","pthread_setspecific"); }
    return record;
}
static pthread_t guest_pthread_self(void) { return current_thread_record()->handle; }
static uint32_t guest_pthread_mach_thread(pthread_t thread) {
    if(pthread_equal(thread,pthread_self()))current_thread_record();
    pthread_mutex_lock(&thread_records_lock);
    struct ThreadRecord *record=registered_thread(thread);
    uint32_t port=record && record->tid>0 ? 0x80000000U|(uint32_t)record->tid : 0;
    pthread_mutex_unlock(&thread_records_lock);return port;
}
static int guest_thread_stack(pthread_t thread,void **base,size_t *size) {
    pthread_mutex_lock(&thread_records_lock);
    if (!registered_thread(thread)) { pthread_mutex_unlock(&thread_records_lock); return ESRCH; }
    pthread_attr_t attr;
    int error=pthread_getattr_np(thread,&attr);
    if (!error) { error=pthread_attr_getstack(&attr,base,size); pthread_attr_destroy(&attr); }
    pthread_mutex_unlock(&thread_records_lock);
    return error;
}
static void *guest_thread_stack_address(pthread_t thread) {
    void *base=NULL; size_t size=0;
    return guest_thread_stack(thread,&base,&size) ? NULL : (char *)base+size;
}
static size_t guest_thread_stack_size(pthread_t thread) {
    void *base=NULL; size_t size=0;
    return guest_thread_stack(thread,&base,&size) ? 0 : size;
}
static int guest_thread_name(const char *name) {
    if (!name || strlen(name)>=64) return ERANGE;
    struct ThreadRecord *record=current_thread_record();
    pthread_mutex_lock(&thread_records_lock);
    snprintf(record->name,sizeof(record->name),"%s",name);
    pthread_mutex_unlock(&thread_records_lock);
    char native[16]; snprintf(native,sizeof(native),"%.15s",name);
    return darwin_pthread_error(pthread_setname_np(pthread_self(),native));
}
static int guest_thread_getname(pthread_t thread,char *name,size_t size) {
    if (!name || !size) return ERANGE;
    pthread_mutex_lock(&thread_records_lock);
    for (struct ThreadRecord *entry=thread_records;entry;entry=entry->next) {
        if (!pthread_equal(thread,entry->handle)) continue;
        if (strlen(entry->name)>=size) { pthread_mutex_unlock(&thread_records_lock); return ERANGE; }
        strcpy(name,entry->name); pthread_mutex_unlock(&thread_records_lock); return 0;
    }
    pthread_mutex_unlock(&thread_records_lock);
    return darwin_pthread_error(pthread_getname_np(thread,name,size));
}
static pthread_t guest_thread_from_mach(uint32_t port) {
    pthread_t result=0;
    pthread_mutex_lock(&thread_records_lock);
    for (struct ThreadRecord *entry=thread_records;entry;entry=entry->next)
        if ((uint32_t)entry->tid==(port&0x7fffffff)) { result=entry->handle; break; }
    pthread_mutex_unlock(&thread_records_lock);
    return result;
}

static int guest_attr_init(struct DarwinThreadAttr *attr) {
    pthread_attr_t *native=malloc(sizeof(*native));
    if (!native) return ENOMEM;
    int error=pthread_attr_init(native);
    if (error) { free(native); return darwin_pthread_error(error); }
    attr->tag=ATTR_TAG; attr->native=native;
    return 0;
}
static int guest_attr_destroy(struct DarwinThreadAttr *attr) {
    if (!attr || attr->tag!=ATTR_TAG) return EINVAL;
    int error=pthread_attr_destroy(attr->native);
    free(attr->native); attr->native=NULL; attr->tag=0;
    return darwin_pthread_error(error);
}
static int guest_attr_detach(struct DarwinThreadAttr *attr,int state) {
    if (!attr || attr->tag!=ATTR_TAG || (state!=1 && state!=2)) return EINVAL;
    return darwin_pthread_error(pthread_attr_setdetachstate(attr->native,state==2 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE));
}
static int guest_attr_stacksize(struct DarwinThreadAttr *attr,size_t size) {
    return attr && attr->tag==ATTR_TAG ? darwin_pthread_error(pthread_attr_setstacksize(attr->native,size)) : EINVAL;
}
static int guest_attr_stack(struct DarwinThreadAttr *attr,void *stack,size_t size) {
    return attr && attr->tag==ATTR_TAG ? darwin_pthread_error(pthread_attr_setstack(attr->native,stack,size)) : EINVAL;
}
static int guest_attr_policy(struct DarwinThreadAttr *attr,int policy) {
    /* Darwin: OTHER=1, FIFO=4, RR=2 (pthread_impl.h). Linux uses 0,1,2. */
    if (!attr || attr->tag!=ATTR_TAG || (policy!=1 && policy!=2 && policy!=4)) return EINVAL;
    return darwin_pthread_error(pthread_attr_setschedpolicy(attr->native,policy==4 ? SCHED_FIFO : policy==2 ? SCHED_RR : SCHED_OTHER));
}
static int guest_attr_sched(struct DarwinThreadAttr *attr,const struct sched_param *parameters) {
    return attr && attr->tag==ATTR_TAG ? darwin_pthread_error(pthread_attr_setschedparam(attr->native,parameters)) : EINVAL;
}
struct GuestThreadStart { void *(*entry)(void *); void *argument; int translate_priority,nice_priority; };
static void *guest_thread_start(void *context) {
    struct GuestThreadStart start=*(struct GuestThreadStart *)context;
    free(context);
    current_thread_record();
    if (start.translate_priority) {
        int error=setpriority(PRIO_PROCESS,gettid(),start.nice_priority) ? errno : 0;
        errno=0; int actual=getpriority(PRIO_PROCESS,gettid());
        LOG("Android scheduling translation: requested nice=%d actual=%d setError=%d (real-time FIFO unavailable)",start.nice_priority,actual,error);
    }
    void *result=NULL;
    may_unwind=1;
    snprintf(stage,sizeof(stage),"pthread entry 0x%lx",(Word)start.entry-aslr+GUEST_IMAGE_BASE);
    if (setjmp(boundary)==0) result=start.entry(start.argument);
    may_unwind=0;
    return result;
}
static int guest_timeshare_attr(pthread_attr_t *translated,const pthread_attr_t *original) {
    int error=pthread_attr_init(translated),detach;
    if (error) return error;
    size_t size,guard; void *stack;
    if (!(error=pthread_attr_getdetachstate(original,&detach))) error=pthread_attr_setdetachstate(translated,detach);
    if (!error && !(error=pthread_attr_getguardsize(original,&guard))) error=pthread_attr_setguardsize(translated,guard);
    if (!error && !(error=pthread_attr_getstack(original,&stack,&size)))
        error=stack ? pthread_attr_setstack(translated,stack,size) : pthread_attr_setstacksize(translated,size);
    if (!error) error=pthread_attr_setschedpolicy(translated,SCHED_OTHER);
    struct sched_param parameters={0};
    if (!error) error=pthread_attr_setschedparam(translated,&parameters);
    if (!error) error=pthread_attr_setinheritsched(translated,PTHREAD_EXPLICIT_SCHED);
    if (error) pthread_attr_destroy(translated);
    return error;
}
static int guest_thread_create(pthread_t *thread,const struct DarwinThreadAttr *attr,void *(*entry)(void *),void *argument) {
    if (!thread || !entry || (attr && attr->tag!=ATTR_TAG)) return EINVAL;
    struct GuestThreadStart *start=malloc(sizeof(*start));
    if (!start) return ENOMEM;
    struct ThreadRecord *record=calloc(1,sizeof(*record));
    if (!record) { free(start); return ENOMEM; }
    *start=(struct GuestThreadStart){.entry=entry,.argument=argument};
    pthread_mutex_lock(&thread_records_lock);
    int error=pthread_create(thread,attr ? attr->native : NULL,guest_thread_start,start);
    if (error==EPERM && attr) {
        int policy=0; struct sched_param requested={0};
        pthread_attr_getschedpolicy(attr->native,&policy);
        pthread_attr_getschedparam(attr->native,&requested);
        if (policy==SCHED_FIFO || policy==SCHED_RR) {
            pthread_attr_t translated;
            int translated_error=guest_timeshare_attr(&translated,attr->native);
            if (!translated_error) {
                int priority=requested.sched_priority;
                start->translate_priority=1;
                start->nice_priority=priority>=80 ? -19+(99-priority)/6 : -priority*15/79;
                if (start->nice_priority<-19) start->nice_priority=-19;
                if (start->nice_priority>0) start->nice_priority=0;
                LOG("Android rejected real-time policy=%d priority=%d; creating the original entry with timeshare policy and nice target=%d",policy,priority,start->nice_priority);
                error=pthread_create(thread,&translated,guest_thread_start,start);
                pthread_attr_destroy(&translated);
            } else error=translated_error;
        }
    }
    if (error) {
        free(start); free(record);
        LOG("Guest pthread_create failed: native error=%d (%s), entry=0x%lx",error,strerror(error),in_image((Word)entry) ? (Word)entry-aslr+GUEST_IMAGE_BASE : 0UL);
    } else if (attr) {
        int requested=0,actual=0,inherit=0; struct sched_param desired={0},current={0};
        pthread_attr_getschedpolicy(attr->native,&requested); pthread_attr_getschedparam(attr->native,&desired);
        pthread_attr_getinheritsched(attr->native,&inherit);
        if (requested!=SCHED_OTHER && !pthread_getschedparam(*thread,&actual,&current))
            LOG("Guest thread scheduling: requested Linux policy=%d priority=%d inherit=%d; actual policy=%d priority=%d",requested,desired.sched_priority,inherit,actual,current.sched_priority);
    }
    if (!error) { record->handle=*thread; record->next=thread_records; thread_records=record; }
    pthread_mutex_unlock(&thread_records_lock);
    return darwin_pthread_error(error);
}
static int guest_thread_join(pthread_t thread,void **value) { return darwin_pthread_error(pthread_join(thread,value)); }

struct GuestOnceRecord { void *control; pthread_once_t native; struct GuestOnceRecord *next; };
static struct GuestOnceRecord *guest_once_records;
static pthread_mutex_t guest_once_mutex=PTHREAD_MUTEX_INITIALIZER;
static int guest_pthread_once(void *control,void (*initialize)(void)) {
    if(!control || !initialize)return EINVAL;
    pthread_mutex_lock(&guest_once_mutex);
    struct GuestOnceRecord *entry=guest_once_records;
    while(entry && entry->control!=control)entry=entry->next;
    if(!entry){entry=calloc(1,sizeof(*entry));if(entry){entry->control=control;entry->native=PTHREAD_ONCE_INIT;entry->next=guest_once_records;guest_once_records=entry;}}
    pthread_mutex_unlock(&guest_once_mutex);
    return entry ? darwin_pthread_error(pthread_once(&entry->native,initialize)) : ENOMEM;
}
static Word resolve_thread_import(const char *name) {
    if(!strcmp(name,"_pthread_once"))return (Word)guest_pthread_once;
    if(!strcmp(name,"_pthread_mach_thread_np"))return (Word)guest_pthread_mach_thread;
    if(!strcmp(name,"_pthread_mutexattr_setprotocol"))return (Word)guest_mutexattr_protocol;
    const struct { const char *name; Word pointer; } entries[]={
        {"_pthread_mutex_init",(Word)guest_mutex_init},{"_pthread_mutex_destroy",(Word)guest_mutex_destroy},
        {"_pthread_mutex_lock",(Word)guest_mutex_lock},{"_pthread_mutex_unlock",(Word)guest_mutex_unlock},{"_pthread_mutex_trylock",(Word)guest_mutex_trylock},
        {"_pthread_mutexattr_init",(Word)guest_mutexattr_init},{"_pthread_mutexattr_destroy",(Word)guest_mutexattr_destroy},{"_pthread_mutexattr_settype",(Word)guest_mutexattr_type},
        {"_pthread_cond_init",(Word)guest_cond_init},{"_pthread_cond_destroy",(Word)guest_cond_destroy},
        {"_pthread_cond_signal",(Word)guest_cond_signal},{"_pthread_cond_broadcast",(Word)guest_cond_broadcast},
        {"_pthread_cond_wait",(Word)guest_cond_wait},{"_pthread_cond_timedwait_relative_np",(Word)guest_cond_relative},
        {"_pthread_rwlock_init",(Word)guest_rwlock_init},{"_pthread_rwlock_destroy",(Word)guest_rwlock_destroy},
        {"_pthread_rwlock_rdlock",(Word)guest_rwlock_read},{"_pthread_rwlock_wrlock",(Word)guest_rwlock_write},{"_pthread_rwlock_unlock",(Word)guest_rwlock_unlock},
        {"_pthread_attr_init",(Word)guest_attr_init},{"_pthread_attr_destroy",(Word)guest_attr_destroy},
        {"_pthread_attr_setdetachstate",(Word)guest_attr_detach},{"_pthread_attr_setstack",(Word)guest_attr_stack},
        {"_pthread_attr_setstacksize",(Word)guest_attr_stacksize},{"_pthread_attr_setschedpolicy",(Word)guest_attr_policy},{"_pthread_attr_setschedparam",(Word)guest_attr_sched},
        {"_pthread_create",(Word)guest_thread_create},{"_pthread_join",(Word)guest_thread_join},{"_pthread_self",(Word)guest_pthread_self},
        {"_pthread_equal",(Word)pthread_equal},{"_pthread_setname_np",(Word)guest_thread_name},{"_pthread_getname_np",(Word)guest_thread_getname},
        {"_pthread_from_mach_thread_np",(Word)guest_thread_from_mach},
        {"_pthread_get_stackaddr_np",(Word)guest_thread_stack_address},{"_pthread_get_stacksize_np",(Word)guest_thread_stack_size}
    };
    for (unsigned i=0;i<COUNT(entries);++i) if (!strcmp(name,entries[i].name)) return entries[i].pointer;
    return 0;
}
