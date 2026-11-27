struct CoreSourceContext { intptr_t version; Word info,retain,release,description,equal,hash,schedule,cancel,perform; };
struct CoreSourceLink { struct NativeLoop *loop; unsigned mode; Word task,mode_object; struct CoreSourceLink *next; };
struct CoreRunLoopSource { Word tag; unsigned references,valid,signaled; intptr_t order; struct CoreSourceContext context;struct CoreSourceLink *links; };
_Static_assert(sizeof(struct CoreSourceContext)==80,"Darwin CFRunLoopSourceContext ABI");
static Word core_source_new(Word allocator,intptr_t order,const struct CoreSourceContext *context) {
    if (allocator || !context || context->version) return 0;
    struct CoreRunLoopSource *source=calloc(1,sizeof(*source)); if (!source) return 0;
    source->tag=(Word)&runloop_source_tag; source->references=source->valid=1; source->order=order;source->context=*context;
    if(context->retain)source->context.info=((Word (*)(Word))context->retain)(context->info);
    return (Word)source;
}
static struct CoreRunLoopSource *core_source_check(Word object) {
    if (!object || read_word(object)!=(Word)&runloop_source_tag) stop("Invalid runloop source","CFRunLoopSource");
    return (void *)object;
}
static int runloop_source_retain(Word object) {
    if (read_word(object)!=(Word)&runloop_source_tag) return 0;
    struct CoreRunLoopSource *source=(void *)object;
    unsigned old=__atomic_fetch_add(&source->references,1,__ATOMIC_RELAXED);
    if (!old || old==UINT_MAX) stop("Invalid source retain","CFRetain"); return 1;
}
static int runloop_source_release(Word object) {
    if (read_word(object)!=(Word)&runloop_source_tag) return 0;
    struct CoreRunLoopSource *source=(void *)object;
    unsigned old=__atomic_fetch_sub(&source->references,1,__ATOMIC_ACQ_REL);
    if (!old) stop("Unbalanced source release","CFRelease");
    if (old==1) { if (source->links) stop("Registered source lost its owner","CFRelease");if(source->context.release)((void (*)(Word))source->context.release)(source->context.info);free(source); }
    return 1;
}
static unsigned core_source_mode(Word mode) {
    if (!scheduling_mode(mode)) stop("Unsupported source runloop mode","CFRunLoopAddSource");
    const char *name=string_bytes(mode,"Runloop source mode");return !strcmp(name,"kCFRunLoopCommonModes") || !strcmp(name,"NSRunLoopCommonModes") ? 2 : 1;
}
static void core_source_cleanup(void *context) { runloop_source_release((Word)context); }
static void core_source_perform(void *context) {
    struct CoreRunLoopSource *source=context;Word function=0,info=0;
    pthread_mutex_lock(&scheduling_mutex);
    if(source->valid && source->signaled){source->signaled=0;function=source->context.perform;info=source->context.info;}
    pthread_mutex_unlock(&scheduling_mutex);if(function)((void (*)(Word))function)(info);
}
static void core_source_add(Word loop_object,Word source_object,Word mode) {
    struct CoreRunLoopSource *source=core_source_check(source_object);
    unsigned kind=core_source_mode(mode); struct NativeLoop *loop=native_loop(loop_object);
    runloop_source_retain(source_object);Word task=loop_function_create(loop_object,core_source_perform,source,core_source_cleanup);
    ((struct ScheduledWork *)task)->order=source->order;
    pthread_mutex_lock(&scheduling_mutex);
    if (!source->valid) { pthread_mutex_unlock(&scheduling_mutex);loop_function_close(task);return; }
    for (struct CoreSourceLink *p=source->links;p;p=p->next) if (p->loop==loop && p->mode==kind) { pthread_mutex_unlock(&scheduling_mutex);loop_function_close(task);return; }
    struct CoreSourceLink *link=malloc(sizeof(*link));
    if (!link) { pthread_mutex_unlock(&scheduling_mutex);loop_function_close(task);stop("Runloop source allocation failed","CFRunLoopAddSource"); }
    *link=(struct CoreSourceLink){loop,kind,task,mode,source->links}; source->links=link;
    unsigned pending=source->signaled;Word schedule=source->context.schedule,info=source->context.info;
    loop_notify_locked(loop);pthread_mutex_unlock(&scheduling_mutex);
    if(schedule)((void (*)(Word,Word,Word))schedule)(info,loop_object,mode);
    if(pending)loop_function_signal(task);
}
static void core_source_remove(Word loop_object,Word source_object,Word mode) {
    struct CoreRunLoopSource *source=core_source_check(source_object); unsigned kind=core_source_mode(mode);
    pthread_mutex_lock(&scheduling_mutex); struct CoreSourceLink **slot=&source->links;
    while (*slot && ((*slot)->loop->object!=loop_object || (*slot)->mode!=kind)) slot=&(*slot)->next;
    struct CoreSourceLink *removed=*slot;
    if (removed) *slot=removed->next;
    pthread_mutex_unlock(&scheduling_mutex);
    if (removed) { if(source->context.cancel)((void (*)(Word,Word,Word))source->context.cancel)(source->context.info,loop_object,mode);Word task=removed->task;free(removed);loop_function_close(task); }
}
static void core_source_invalidate(Word object) {
    struct CoreRunLoopSource *source=core_source_check(object); runloop_source_retain(object);
    pthread_mutex_lock(&scheduling_mutex); source->valid=0;
    struct CoreSourceLink *links=source->links; source->links=NULL;
    pthread_mutex_unlock(&scheduling_mutex);
    while (links) { struct CoreSourceLink *next=links->next;if(source->context.cancel)((void (*)(Word,Word,Word))source->context.cancel)(source->context.info,links->loop->object,links->mode_object);Word task=links->task;free(links);loop_function_close(task);links=next; }
    runloop_source_release(object);
}
static unsigned char core_source_valid(Word object) {
    struct CoreRunLoopSource *source=core_source_check(object); pthread_mutex_lock(&scheduling_mutex);
    unsigned char valid=source->valid!=0; pthread_mutex_unlock(&scheduling_mutex); return valid;
}
static intptr_t core_source_order(Word object) { return core_source_check(object)->order; }
static unsigned char core_source_equal(Word first,Word second) {
    struct CoreRunLoopSource *a=core_source_check(first),*b=core_source_check(second);
    if(a->order!=b->order || a->context.equal!=b->context.equal || a->context.perform!=b->context.perform)return 0;
    return a->context.equal ? ((unsigned char (*)(Word,Word))a->context.equal)(a->context.info,b->context.info) : a->context.info==b->context.info;
}
static Word core_source_hash(Word object) { struct CoreRunLoopSource *source=core_source_check(object);return source->context.hash ? ((Word (*)(Word))source->context.hash)(source->context.info) : source->context.info; }
static Word core_source_description(Word object) { struct CoreRunLoopSource *source=core_source_check(object);if(source->context.description)return ((Word (*)(Word))source->context.description)(source->context.info);return copy_string("CFRunLoopSource"); }
static void core_source_context(Word object,struct CoreSourceContext *context) {
    struct CoreRunLoopSource *source=core_source_check(object); if (!context || context->version) stop("Unsupported source context version","CFRunLoopSourceGetContext");
    *context=source->context;
}
static void core_source_signal(Word object) {
    struct CoreRunLoopSource *source=core_source_check(object);Word local[16],*tasks=local;size_t count=0;
    pthread_mutex_lock(&scheduling_mutex);if(!source->valid){pthread_mutex_unlock(&scheduling_mutex);return;}
    source->signaled=1;for(struct CoreSourceLink *p=source->links;p;p=p->next)++count;
    if(count>COUNT(local))tasks=malloc(count*sizeof(*tasks));
    if(!tasks){pthread_mutex_unlock(&scheduling_mutex);stop("Allocation failed","CFRunLoopSourceSignal");}
    size_t index=0;for(struct CoreSourceLink *p=source->links;p;p=p->next)tasks[index++]=p->task;
    pthread_mutex_unlock(&scheduling_mutex);for(size_t i=0;i<count;++i)loop_function_signal(tasks[i]);if(tasks!=local)free(tasks);
}
static void core_loop_wakeup(Word object) { if(object==(Word)&main_run_loop && platform_main_looper)ALooper_wake(platform_main_looper);struct NativeLoop *loop=native_loop(object);pthread_mutex_lock(&scheduling_mutex);loop_notify_locked(loop);pthread_mutex_unlock(&scheduling_mutex); }
static unsigned char core_source_contains(Word loop,Word object,Word mode) {
    struct CoreRunLoopSource *source=core_source_check(object); unsigned kind=core_source_mode(mode); unsigned char found=0;
    pthread_mutex_lock(&scheduling_mutex);
    for (struct CoreSourceLink *p=source->links;p;p=p->next) if (p->loop->object==loop && p->mode==kind) { found=1; break; }
    pthread_mutex_unlock(&scheduling_mutex); return found;
}
static Word core_source_type(void) { return (Word)&runloop_source_tag; }
static Word resolve_runloop_source_import(const char *name) {
#define RUNLOOP_SOURCE(api,fn) if (!strcmp(name,"_" #api)) return (Word)fn
    RUNLOOP_SOURCE(CFRunLoopSourceCreate,core_source_new); RUNLOOP_SOURCE(CFRunLoopAddSource,core_source_add);
    RUNLOOP_SOURCE(CFRunLoopRemoveSource,core_source_remove); RUNLOOP_SOURCE(CFRunLoopContainsSource,core_source_contains);
    RUNLOOP_SOURCE(CFRunLoopSourceInvalidate,core_source_invalidate); RUNLOOP_SOURCE(CFRunLoopSourceIsValid,core_source_valid);
    RUNLOOP_SOURCE(CFRunLoopSourceGetOrder,core_source_order); RUNLOOP_SOURCE(CFRunLoopSourceGetContext,core_source_context);
    RUNLOOP_SOURCE(CFRunLoopSourceGetTypeID,core_source_type);
    RUNLOOP_SOURCE(CFRunLoopSourceSignal,core_source_signal);RUNLOOP_SOURCE(CFRunLoopWakeUp,core_loop_wakeup);
#undef RUNLOOP_SOURCE
    return 0;
}
