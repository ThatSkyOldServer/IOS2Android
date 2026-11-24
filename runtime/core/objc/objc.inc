/* Object destruction remains selective for classes managed by the runtime. */

static Word copied_object(Word value) {
    if (!value) return 0;
    Word isa=read_word(value);
    if (isa==(Word)_NSConcreteStackBlock || isa==(Word)_NSConcreteGlobalBlock || isa==(Word)_NSConcreteMallocBlock)
        return (Word)_Block_copy((void *)value);
    Method copy=find_implementation(value,"copy");
    if (!copy) stop("Unsupported copied object",class_name(isa));
    return copy(value,"copy",0,0,0,0);
}

static void copy_property(Word object,const char *selector,Word value,intptr_t offset) {
    value=copied_object(value);
    set_property(object,selector,value,offset);
    metal_object_release(value);
}

static void copy_atomic_property(Word object,const char *selector,Word value,intptr_t offset) {
    value=copied_object(value);
    set_atomic_property(object,selector,value,offset);
    metal_object_release(value);
}

typedef unsigned char (*ClassLookupHook)(const char *,Word *);
struct NamedRuntimeObject { char *name; Word object; struct NamedRuntimeObject *next; };
static struct NamedRuntimeObject *guest_class_names[1024],*guest_protocol_names[1024];
static pthread_mutex_t named_runtime_mutex=PTHREAD_MUTEX_INITIALIZER;
static Word named_runtime_find(struct NamedRuntimeObject **table,const char *name) {
    unsigned bucket=selector_hash(name)&1023;
    pthread_mutex_lock(&named_runtime_mutex);
    struct NamedRuntimeObject *entry=table[bucket];
    while(entry && strcmp(entry->name,name))entry=entry->next;
    Word result=entry ? entry->object : 0;
    pthread_mutex_unlock(&named_runtime_mutex);return result;
}
static Word named_runtime_insert(struct NamedRuntimeObject **table,const char *name,Word object) {
    unsigned bucket=selector_hash(name)&1023;
    pthread_mutex_lock(&named_runtime_mutex);
    for(struct NamedRuntimeObject *entry=table[bucket];entry;entry=entry->next)
        if(!strcmp(entry->name,name)){Word found=entry->object;pthread_mutex_unlock(&named_runtime_mutex);return found;}
    struct NamedRuntimeObject *entry=calloc(1,sizeof(*entry));
    if(entry)entry->name=strdup(name);
    if(!entry || !entry->name){free(entry);pthread_mutex_unlock(&named_runtime_mutex);stop("Allocation failed","runtime name index");}
    entry->object=object;entry->next=table[bucket];table[bucket]=entry;
    pthread_mutex_unlock(&named_runtime_mutex);return object;
}
static unsigned char empty_class_lookup(const char *name,Word *result) { *result=0; return 0; }
static ClassLookupHook class_lookup_hook=empty_class_lookup;
static pthread_mutex_t class_lookup_hook_mutex=PTHREAD_MUTEX_INITIALIZER;
struct ClassLookupFrame { const char *name; struct ClassLookupFrame *previous; };
static _Thread_local struct ClassLookupFrame *class_lookup_frame;
static void set_class_lookup_hook(ClassLookupHook hook,ClassLookupHook *old) {
    if (!hook) stop("Invalid class lookup hook","objc_setHook_getClass");
    pthread_mutex_lock(&class_lookup_hook_mutex);
    if (old) *old=__atomic_load_n(&class_lookup_hook,__ATOMIC_ACQUIRE);
    __atomic_store_n(&class_lookup_hook,hook,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&class_lookup_hook_mutex);
}
static Word class_from_name(const char *wanted) {
    if (!wanted) return 0;
    Word dynamic=dynamic_named_class(wanted,1);
    if (dynamic) return dynamic;
    Word cached=named_runtime_find(guest_class_names,wanted);
    if(cached){if(class_layouts_ready)realize_class_layout(cached,0);return cached;}
    const Word *classes=(void *)(aslr+CLASSES_OFFSET);
    for (unsigned index=0;index<CLASSES_COUNT;++index) {
        if (!strcmp(class_name(classes[index]),wanted)) {
            if (class_layouts_ready) realize_class_layout(classes[index],0);
            return named_runtime_insert(guest_class_names,wanted,classes[index]);
        }
    }
    char symbol[192];
    snprintf(symbol,sizeof(symbol),"_OBJC_CLASS_$_%s",wanted);
    Word result=external_class(symbol);
    if (result) return result;
    for (struct ClassLookupFrame *frame=class_lookup_frame;frame;frame=frame->previous)
        if (!strcmp(frame->name,wanted)) return 0;
    struct ClassLookupFrame frame={wanted,class_lookup_frame};
    class_lookup_frame=&frame;
    struct GuestCallBoundary call={.previous=active_guest_call_boundary};
    active_guest_call_boundary=&call;
    int failed=setjmp(call.environment);
    unsigned char found=0;
    if(!failed)found=__atomic_load_n(&class_lookup_hook,__ATOMIC_ACQUIRE)(wanted,&result);
    active_guest_call_boundary=call.previous;
    class_lookup_frame=frame.previous;
    if(failed)propagate_guest_failure();
    return found ? result : 0;
}

static const char *method_types(struct MethodRecord *method) { return method ? method->types : NULL; }

static void exchange_methods(struct MethodRecord *first,struct MethodRecord *second) {
    if (!first || !second) return;
    Method imp=__atomic_load_n(&first->implementation,__ATOMIC_ACQUIRE);
    Method other=__atomic_exchange_n(&second->implementation,imp,__ATOMIC_ACQ_REL);
    __atomic_store_n(&first->implementation,other,__ATOMIC_RELEASE);
    __atomic_add_fetch(&method_cache_epoch,1,__ATOMIC_RELEASE);
}

static Word instance_size(Word cls) {
    struct DynamicClass *dynamic=dynamic_class(cls);
    if (dynamic) return dynamic->size;
    if (in_image(cls)) {
        if (class_layouts_ready) return realize_class_layout(cls,0);
        Word ro=read_word(cls+32)&~(Word)7;
        uint32_t size;
        memcpy(&size,(void *)(ro+8),sizeof(size));
        return size;
    }
    if (host_symbol(cls)) return host_allocation_size(cls);
    return 0;
}

static Word change_class(Word object,Word cls) {
    if (!object) return 0;
    if (!class_name(cls)) stop("Unknown replacement class","object_setClass");
    return __atomic_exchange_n((Word *)object,cls,__ATOMIC_ACQ_REL);
}

static void copy_weak(Word *destination,const Word *source) { Word value=load_weak_retained((Word *)source);store_weak(destination,value);metal_object_release(value); }

static pthread_mutex_t objc_data_mutex=PTHREAD_MUTEX_INITIALIZER;
struct Association { Word object, key, value; struct Association *next; };
static struct Association *associations;
struct SynchronizedObject { Word object; pthread_mutex_t mutex; struct SynchronizedObject *next; };
static struct SynchronizedObject *synchronized_objects;

static Word associated_object(Word object,Word key) {
    pthread_mutex_lock(&objc_data_mutex);
    struct Association *entry=associations;
    while (entry && (entry->object!=object || entry->key!=key)) entry=entry->next;
    Word value=entry ? entry->value : 0;
    pthread_mutex_unlock(&objc_data_mutex);
    return value;
}

static void associate_object(Word object,Word key,Word value,Word policy) {
    if ((policy&3)==3) value=copied_object(value);
    pthread_mutex_lock(&objc_data_mutex);
    struct Association *entry=associations;
    while (entry && (entry->object!=object || entry->key!=key)) entry=entry->next;
    if (!entry && value) {
        entry=calloc(1,sizeof(*entry));
        if (!entry) { pthread_mutex_unlock(&objc_data_mutex); stop("Allocation failed","objc association"); }
        entry->object=object; entry->key=key; entry->next=associations; associations=entry;
    }
    if (entry) entry->value=value;
    pthread_mutex_unlock(&objc_data_mutex);
}

static pthread_mutex_t *synchronized_mutex(Word object) {
    pthread_mutex_lock(&objc_data_mutex);
    struct SynchronizedObject *entry=synchronized_objects;
    while (entry && entry->object!=object) entry=entry->next;
    if (!entry) {
        entry=calloc(1,sizeof(*entry));
        if (!entry) { pthread_mutex_unlock(&objc_data_mutex); stop("Allocation failed","objc synchronized object"); }
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
        int result=pthread_mutex_init(&entry->mutex,&attr);
        pthread_mutexattr_destroy(&attr);
        if (result) { pthread_mutex_unlock(&objc_data_mutex); stop("Synchronization initialization failed","pthread"); }
        entry->object=object; entry->next=synchronized_objects; synchronized_objects=entry;
    }
    pthread_mutex_unlock(&objc_data_mutex);
    return &entry->mutex;
}

static int synchronized_enter(Word object) { return object ? pthread_mutex_lock(synchronized_mutex(object)) : 0; }
static int synchronized_exit(Word object) { return object ? pthread_mutex_unlock(synchronized_mutex(object)) : 0; }

static void copy_struct(void *destination,const void *source,size_t size,unsigned char atomic,unsigned char strong) {
    if (atomic) pthread_mutex_lock(&objc_data_mutex);
    memmove(destination,source,size);
    if (atomic) pthread_mutex_unlock(&objc_data_mutex);
}

static char **copy_image_names(unsigned *count) {
    char **names=calloc(1,sizeof(*names));
    if (!names) stop("Allocation failed","objc_copyImageNames");
    names[0]=GUEST_IMAGE_PATH;
    if (count) *count=1;
    return names;
}

static Word class_conforms(Word cls,Word protocol) { return conforms_protocol(cls,"conformsToProtocol:",protocol); }
static unsigned char class_add_protocol(Word cls,Word protocol) {
    if(!cls || !protocol || (!guest_class_object(cls) && !host_symbol(cls)))return 0;
    pthread_mutex_lock(&dynamic_mutex);
    if(class_conforms(cls,protocol)){pthread_mutex_unlock(&dynamic_mutex);return 0;}
    struct AddedProtocol *entry=calloc(1,sizeof(*entry));
    if(!entry){pthread_mutex_unlock(&dynamic_mutex);stop("Allocation failed","class_addProtocol");}
    entry->cls=cls;entry->protocol=protocol;entry->next=added_protocols;
    __atomic_store_n(&added_protocols,entry,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&dynamic_mutex);return 1;
}
static const char *protocol_name(Word protocol) {
    if(!protocol)return NULL;
    if(!image_segment_for(protocol,16))stop("Invalid protocol","protocol_getName");
    Word name=read_word(protocol+8);
    if(!in_image(name))stop("Invalid protocol name","protocol_getName");
    return (const char *)name;
}
static Word string_from_protocol(Word protocol) {
    const char *name=protocol_name(protocol);return name ? copy_string(name) : 0;
}
static Word protocol_from_name(const char *name) {
    if(!name)return 0;
    Word cached=named_runtime_find(guest_protocol_names,name);if(cached)return cached;
    for(unsigned image=0;image<guest_image_count();++image){
        const void *header=guest_image_header(image);
        const char *segments[]={"__DATA","__DATA_CONST","__AUTH_CONST"};
        for(unsigned s=0;s<COUNT(segments);++s){
            unsigned long bytes=0;const Word *protocols=guest_section_data(header,segments[s],"__objc_protolist",&bytes);
            if(!protocols)continue;
            for(size_t i=0;i<bytes/sizeof(Word);++i){
                Word protocol=protocols[i];if(protocol && !strcmp(protocol_name(protocol),name))return named_runtime_insert(guest_protocol_names,name,protocol);
            }
        }
    }
    return 0;
}
static Word protocol_from_string(Word name) { return name ? protocol_from_name(string_bytes(name,"NSProtocolFromString")) : 0; }

struct InvocationType { size_t size,alignment; unsigned count; char floating,code; char *encoding; };
struct SignatureState { struct InvocationType result,*arguments; size_t count,frame; };
struct InvocationState { Word signature; unsigned char **arguments,*result; int retained,invoked; };
static Word cf_retain(Word);
static int runtime_block(Word);
static size_t invocation_align(size_t size,size_t alignment) { if (!alignment || size>SIZE_MAX-alignment+1) stop("Invalid encoded type size","NSMethodSignature"); return (size+alignment-1)&~(alignment-1); }
static void invocation_skip_quote(const char **p) { if (**p=='"') { ++*p; while (**p && **p!='"') ++*p; if (**p) ++*p; } }
static struct InvocationType invocation_type(const char **cursor,unsigned depth) {
    const char *begin=*cursor; while (**cursor && strchr("rnNoORV",**cursor)) ++*cursor;
    struct InvocationType t={.alignment=1}; t.code=*(*cursor)++;
    if (depth>32 || !t.code) stop("Invalid type encoding","NSMethodSignature");
    switch (t.code) {
        case 'v':break; case 'B':case 'c':case 'C':t.size=t.alignment=1;break;
        case 's':case 'S':t.size=t.alignment=2;break;
        case 'i':case 'I':t.size=t.alignment=4;break;
        case 'l':case 'L':case 'q':case 'Q':case '@':case '#':case ':':case '*':t.size=t.alignment=8;break;
        case 'f':t.size=t.alignment=4;t.floating='f';t.count=1;break;
        case 'd':t.size=t.alignment=8;t.floating='d';t.count=1;break;
        case '^': { struct InvocationType pointed=invocation_type(cursor,depth+1);free(pointed.encoding);t.size=t.alignment=8;break; }
        case '?':break;
        case '[': {
            char *end; unsigned long count=strtoul(*cursor,&end,10); if(end==*cursor) stop("Invalid array type","NSMethodSignature"); *cursor=end;
            struct InvocationType item=invocation_type(cursor,depth+1);
            if (*(*cursor)++!=']' || (item.size && count>SIZE_MAX/item.size)) stop("Invalid array type","NSMethodSignature");
            t.size=item.size*count;t.alignment=item.alignment;
            if (item.count && count<=4/item.count) { t.count=item.count*count;t.floating=item.floating; }
            free(item.encoding);break;
        }
        case '{':case '(': {
            char close=t.code=='{' ? '}' : ')'; while (**cursor && **cursor!='=' && **cursor!=close) ++*cursor;
            if (**cursor=='=') ++*cursor;
            int homogeneous=1; unsigned members=0; char floating=0;
            while (**cursor && **cursor!=close) {
                invocation_skip_quote(cursor); struct InvocationType field=invocation_type(cursor,depth+1);
                if (field.alignment>t.alignment) t.alignment=field.alignment;
                if(t.code=='{') { size_t offset=invocation_align(t.size,field.alignment); if(field.size>SIZE_MAX-offset)stop("Type too large","NSMethodSignature");t.size=offset+field.size; }
                else if(field.size>t.size)t.size=field.size;
                if (!field.count || (floating && floating!=field.floating)) homogeneous=0;
                if(field.count) { floating=field.floating; if(t.code=='{')members+=field.count;else if(field.count>members)members=field.count; }
                free(field.encoding);
            }
            if (*(*cursor)++!=close) stop("Unterminated aggregate type","NSMethodSignature");
            t.size=invocation_align(t.size,t.alignment);if(homogeneous && members && members<=4){t.floating=floating;t.count=members;}break;
        }
        default:stop("Unsupported invocation type","NSMethodSignature");
    }
    if(t.code=='@') { if(**cursor=='?')++*cursor;else invocation_skip_quote(cursor); }
    t.encoding=strndup(begin,(size_t)(*cursor-begin)); if(!t.encoding)stop("Allocation failed","NSMethodSignature"); return t;
}
static struct SignatureState *signature_state(Word object) { struct HostObject *self=(void *)object; if(!self || !self->content[0])stop("Invalid method signature","NSInvocation");return (void *)self->content[0]; }
static void signature_destroy(struct SignatureState *s) { free(s->result.encoding);for(size_t i=0;i<s->count;++i)free(s->arguments[i].encoding);free(s->arguments);free(s); }
static Word signature_create(Word cls,const char *selector,const char *encoding) {
    if(!encoding)return 0;
    struct SignatureState *s=calloc(1,sizeof(*s));if(!s)stop("Allocation failed",selector);
    const char *p=encoding;s->result=invocation_type(&p,0);if(*p>='0' && *p<='9'){char *end;s->frame=strtoul(p,&end,10);p=end;}
    size_t computed=0;
    while(*p) {
        if(s->count>=1024){signature_destroy(s);stop("Too many method arguments",selector);}
        struct InvocationType arg=invocation_type(&p,0);struct InvocationType *arguments=realloc(s->arguments,(s->count+1)*sizeof(*arguments));if(!arguments)stop("Allocation failed",selector);
        s->arguments=arguments;s->arguments[s->count++]=arg;computed=invocation_align(computed,arg.alignment)+arg.size;
        if(*p=='+' || *p=='-')++p;while(*p>='0' && *p<='9')++p;
    }
    if(!s->frame)s->frame=computed;
    Word object=host_allocate(cls);((struct HostObject *)object)->content[0]=(Word)s;return object;
}
static const char *signature_bridge_encoding(const char *selector) {
    const struct {const char *name,*types;} known[]={
        {"setContentInsetAdjustmentBehavior:","v24@0:8q16"},{"setFrame:","v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"setBounds:","v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},{"drawRect:","v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"frame","{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},{"bounds","{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"setAlpha:","v24@0:8d16"},{"alpha","d16@0:8"},{"setContentOffset:","v32@0:8{CGPoint=dd}16"},
        {"setContentInset:","v48@0:8{UIEdgeInsets=dddd}16"},{"setText:","v24@0:8@16"},{"text","@16@0:8"},
        {"setDelegate:","v24@0:8@16"},{"delegate","@16@0:8"},{"count","Q16@0:8"},{"init","@16@0:8"},
        {"setHidden:","v20@0:8B16"},{"isHidden","B16@0:8"},{"layoutSubviews","v16@0:8"},
        {"setNeedsDisplay","v16@0:8"},{"setNeedsLayout","v16@0:8"},{"layoutIfNeeded","v16@0:8"},
        {"objectForKey:","@24@0:8@16"},{"setObject:forKey:","v32@0:8@16@24"},{"valueForKey:","@24@0:8@16"},
        {"setValue:forKey:","v32@0:8@16@24"},{"objectAtIndex:","@24@0:8Q16"},{"compare:","q24@0:8@16"},
        {"setAutomaticallyAdjustsScrollViewInsets:","v20@0:8B16"},
        {"openURL:","B24@0:8@16"},{"canOpenURL:","B24@0:8@16"}
    };
    for(unsigned i=0;i<COUNT(known);++i)if(!strcmp(known[i].name,selector))return known[i].types;return NULL;
}
static Word signature_for_selector(Word object,const char *selector,const char *action) {
    if(!action)return 0;Word cls=!strcmp(selector,"instanceMethodSignatureForSelector:") ? object : read_word(object);
    struct MethodRecord *record=(void *)instance_method(cls,action);const char *encoding=record ? record->types : NULL;
    if(!encoding) { struct HostObject prototype={.isa=cls}; if(find_implementation((Word)&prototype,action))encoding=signature_bridge_encoding(action); }
    return encoding ? signature_create(external_class("_OBJC_CLASS_$_NSMethodSignature"),selector,encoding) : 0;
}
static Word signature_property(Word object,const char *selector) { struct SignatureState *s=signature_state(object);return !strcmp(selector,"numberOfArguments") ? s->count : !strcmp(selector,"methodReturnLength") ? s->result.size : s->frame; }
static const char *signature_return(Word object) { return signature_state(object)->result.encoding; }
static const char *signature_argument(Word object,const char *selector,size_t index) { struct SignatureState *s=signature_state(object);if(index>=s->count)stop("Argument index out of bounds",selector);return s->arguments[index].encoding; }
static Word signature_oneway(Word object) { const char *p=signature_state(object)->result.encoding;while(*p && strchr("rnNoORV",*p)){if(*p++=='V')return 1;}return 0; }
static void signature_dealloc(struct HostObject *self) { if(self->content[0])signature_destroy((void *)self->content[0]);free(self); }
static struct InvocationState *invocation_state(Word object) { struct HostObject *self=(void *)object;if(!self || !self->content[0])stop("Uninitialized invocation","NSInvocation");return (void *)self->content[0]; }
static Word invocation_create(Word cls,const char *selector,Word signature) {
    struct SignatureState *source=signature_state(signature);if(source->count<2 || source->arguments[0].size!=sizeof(Word) || source->arguments[1].size!=sizeof(Word))stop("Method signature needs self and selector",selector);
    Word object=host_allocate(cls);struct InvocationState *v=calloc(1,sizeof(*v));if(!v)stop("Allocation failed",selector);
    struct SignatureState *s=calloc(1,sizeof(*s));if(!s)stop("Allocation failed",selector);
    s->count=source->count;s->frame=source->frame;s->result=source->result;s->result.encoding=strdup(source->result.encoding);s->arguments=calloc(s->count,sizeof(*s->arguments));
    v->arguments=calloc(s->count,sizeof(*v->arguments));v->result=calloc(1,s->result.size ? s->result.size : 1);v->signature=host_allocate(external_class("_OBJC_CLASS_$_NSMethodSignature"));
    if(!s->result.encoding || !s->arguments || !v->arguments || !v->result)stop("Allocation failed",selector);
    for(size_t i=0;i<s->count;++i){s->arguments[i]=source->arguments[i];s->arguments[i].encoding=strdup(source->arguments[i].encoding);v->arguments[i]=calloc(1,s->arguments[i].size ? s->arguments[i].size : 1);if(!s->arguments[i].encoding || !v->arguments[i])stop("Allocation failed",selector);}
    ((struct HostObject *)v->signature)->content[0]=(Word)s;((struct HostObject *)object)->content[0]=(Word)v;return object;
}
static void invocation_owned_argument(struct InvocationType *t,void *slot,int retain) {
    if(t->code!='@' && t->code!='*')return;Word value=read_word((Word)slot);if(!value)return;
    if(t->code=='*'){if(retain){char *copy=strdup((void *)value);if(!copy)stop("Allocation failed","NSInvocation arguments");memcpy(slot,&copy,sizeof(copy));}else free((void *)value);}
    else if(runtime_block(value)){Word result=retain ? (Word)_Block_copy((void *)value) : 0;if(retain)memcpy(slot,&result,sizeof(result));else _Block_release((void *)value);}
    else if(retain)cf_retain(value);else cf_release(value);
}
static void invocation_set_argument(Word object,const char *selector,const void *value,size_t index) {
    struct InvocationState *v=invocation_state(object);struct SignatureState *s=signature_state(v->signature);if(index>=s->count || !value)stop("Invalid invocation argument",selector);
    size_t size=s->arguments[index].size;
    if(!v->retained){memmove(v->arguments[index],value,size);return;}
    void *copy=malloc(size ? size : 1);if(!copy)stop("Allocation failed",selector);memcpy(copy,value,size);
    invocation_owned_argument(s->arguments+index,copy,1);invocation_owned_argument(s->arguments+index,v->arguments[index],0);memcpy(v->arguments[index],copy,size);free(copy);
}
static void invocation_get_argument(Word object,const char *selector,void *value,size_t index) { struct InvocationState *v=invocation_state(object);struct SignatureState *s=signature_state(v->signature);if(index>=s->count || !value)stop("Invalid invocation argument",selector);memcpy(value,v->arguments[index],s->arguments[index].size); }
static void invocation_target(Word object,const char *selector,Word target) { invocation_set_argument(object,selector,&target,0); }
static void invocation_selector(Word object,const char *selector,const char *action) { invocation_set_argument(object,selector,&action,1); }
static Word invocation_property(Word object,const char *selector) { struct InvocationState *v=invocation_state(object);return !strcmp(selector,"methodSignature") ? v->signature : !strcmp(selector,"argumentsRetained") ? v->retained : read_word((Word)v->arguments[!strcmp(selector,"selector")]); }
static void invocation_retain_arguments(Word object) { struct InvocationState *v=invocation_state(object);if(v->retained)return;struct SignatureState *s=signature_state(v->signature);for(size_t i=0;i<s->count;++i)invocation_owned_argument(s->arguments+i,v->arguments[i],1);v->retained=1; }
struct InvocationRegisters { Word integers[2];uint64_t floating[4]; };
__attribute__((naked)) static void invocation_call(Word implementation,const Word *integers,const uint64_t *floating,const void *stack,size_t stack_size,void *indirect,struct InvocationRegisters *returned) {
    __asm__ volatile(
        "stp x29,x30,[sp,#-48]!\n stp x19,x20,[sp,#16]\n stp x21,x22,[sp,#32]\n mov x29,sp\n"
        "mov x19,x0\n mov x20,x6\n mov x21,x1\n mov x22,x2\n sub sp,sp,x4\n mov x9,sp\n cbz x4,2f\n"
        "1: ldr x10,[x3],#8\n str x10,[x9],#8\n subs x4,x4,#8\n b.ne 1b\n"
        "2: mov x8,x5\n ldp d0,d1,[x22]\n ldp d2,d3,[x22,#16]\n ldp d4,d5,[x22,#32]\n ldp d6,d7,[x22,#48]\n"
        "ldp x0,x1,[x21]\n ldp x2,x3,[x21,#16]\n ldp x4,x5,[x21,#32]\n ldp x6,x7,[x21,#48]\n blr x19\n"
        "stp x0,x1,[x20]\n stp d0,d1,[x20,#16]\n stp d2,d3,[x20,#32]\n mov sp,x29\n"
        "ldp x21,x22,[sp,#32]\n ldp x19,x20,[sp,#16]\n ldp x29,x30,[sp],#48\n ret\n");
}
static Word invocation_integer(struct InvocationType *t,const void *value) {
    Word result=0;memcpy(&result,value,t->size<8 ? t->size : 8);
    if(t->code=='c')return (Word)(intptr_t)(int8_t)result;if(t->code=='s')return (Word)(intptr_t)(int16_t)result;if(t->code=='i')return (Word)(intptr_t)(int32_t)result;return result;
}
static void invocation_invoke(Word object) {
    struct InvocationState *v=invocation_state(object);struct SignatureState *s=signature_state(v->signature);Word target=read_word((Word)v->arguments[0]);const char *action=(void *)read_word((Word)v->arguments[1]);
    memset(v->result,0,s->result.size);v->invoked=1;if(!target)return;if(!action)stop("Invocation has no selector","NSInvocation");
    Method method=find_implementation(target,action);if(!method)stop("Unrecognized invocation selector",action);
    size_t capacity=16;for(size_t i=0;i<s->count;++i){if(s->arguments[i].size>SIZE_MAX-capacity-32)stop("Invocation frame too large",action);capacity+=s->arguments[i].size+32;}
    unsigned char *stack=calloc(1,capacity);if(!stack)stop("Allocation failed",action);
    Word gp[8]={0};uint64_t fp[8]={0};unsigned ng=0,nf=0;size_t ns=0;int darwin=in_image((Word)method);
    for(size_t i=0;i<s->count;++i){struct InvocationType *t=s->arguments+i;const void *value=v->arguments[i];
        if(t->count && nf+t->count<=8){for(unsigned j=0;j<t->count;++j)memcpy(fp+nf+j,(const char *)value+j*(t->floating=='f' ? 4 : 8),t->floating=='f' ? 4 : 8);nf+=t->count;continue;}
        if(t->count){nf=8;size_t align=darwin ? t->alignment : t->alignment<8 ? 8 : t->alignment;ns=invocation_align(ns,align);memcpy(stack+ns,value,t->size);ns+=darwin ? t->size : invocation_align(t->size,8);continue;}
        int indirect=t->size>16;size_t size=indirect ? 8 : t->size,words=(size+7)/8;Word address=(Word)value;
        if(t->alignment>=16 && !darwin)ng=(ng+1)&~1U;
        if(ng+words<=8){if(indirect)gp[ng]=address;else if(words==1)gp[ng]=invocation_integer(t,value);else memcpy(gp+ng,value,size);ng+=words;continue;}
        if(!darwin && !indirect && ng<8 && !ns){size_t prefix=(8-ng)*8;memcpy(gp+ng,value,prefix);memcpy(stack,(const char *)value+prefix,size-prefix);ns=invocation_align(size-prefix,8);ng=8;continue;}
        ng=8;size_t align=darwin ? (indirect ? 8 : t->alignment) : t->alignment<8 ? 8 : t->alignment;ns=invocation_align(ns,align);if(indirect)memcpy(stack+ns,&address,8);else memcpy(stack+ns,value,size);ns+=darwin ? size : invocation_align(size,8);
    }
    struct InvocationRegisters result={0};struct GuestCallBoundary scoped={.previous=active_guest_call_boundary};active_guest_call_boundary=&scoped;int failed=setjmp(scoped.environment);
    if(!failed)invocation_call((Word)method,gp,fp,stack,invocation_align(ns,16),v->result,&result);
    active_guest_call_boundary=scoped.previous;free(stack);if(failed){char failure[sizeof(status)];memcpy(failure,status,sizeof(failure));stop("Invocation failed",failure);}
    if(s->result.count)for(unsigned i=0;i<s->result.count;++i)memcpy(v->result+i*(s->result.floating=='f' ? 4 : 8),result.floating+i,s->result.floating=='f' ? 4 : 8);
    else if(s->result.size<=16)memcpy(v->result,result.integers,s->result.size);
}
static void invocation_invoke_target(Word object,const char *selector,Word target) { invocation_target(object,selector,target);invocation_invoke(object); }
static void invocation_get_return(Word object,const char *selector,void *value) { struct InvocationState *v=invocation_state(object);if(!value)stop("Null return buffer",selector);memcpy(value,v->result,signature_state(v->signature)->result.size); }
static void invocation_set_return(Word object,const char *selector,const void *value) { struct InvocationState *v=invocation_state(object);if(!value)stop("Null return buffer",selector);memcpy(v->result,value,signature_state(v->signature)->result.size);v->invoked=1; }
static void invocation_dealloc(struct HostObject *self) { struct InvocationState *v=invocation_state((Word)self);struct SignatureState *s=signature_state(v->signature);for(size_t i=0;i<s->count;++i){if(v->retained)invocation_owned_argument(s->arguments+i,v->arguments[i],0);free(v->arguments[i]);}signature_dealloc((void *)v->signature);free(v->arguments);free(v->result);free(v);free(self); }
static Word value_encoded(Word cls,const char *selector,const void *bytes,const char *encoding) {
    if(!bytes || !encoding)stop("Invalid boxed value",selector);const char *end=encoding;struct InvocationType type=invocation_type(&end,0);
    size_t size=type.size,length=strlen(type.encoding),storage=size<32 ? 32 : size;
    if(storage>SIZE_MAX-sizeof(struct BoxedValue)-length-1)stop("Boxed value too large",selector);
    struct BoxedValue *value=calloc(1,offsetof(struct BoxedValue,bytes)+storage+length+1);if(!value)stop("Allocation failed",selector);
    value->isa=cls;value->size=size;char *owned=(char *)value->bytes+storage;memcpy(owned,type.encoding,length+1);value->type=owned;
    if(size)memcpy(value->bytes,bytes,size);free(type.encoding);return (Word)value;
}
static Word value_init_encoded(Word object,const char *selector,const void *bytes,const char *encoding) { Word value=value_encoded(read_word(object),selector,bytes,encoding);free((void *)object);return value; }
static void value_get_sized(struct BoxedValue *value,const char *selector,void *output,size_t size) { if(!output || size<value->size)stop("Boxed value output too small",selector);memcpy(output,value->bytes,value->size); }
static Word value_equal(struct BoxedValue *value,const char *selector,Word other) { if(!other || !class_inherits(read_word(other),external_class("_OBJC_CLASS_$_NSValue")) || class_inherits(read_word(other),external_class("_OBJC_CLASS_$_NSNumber")))return 0;struct BoxedValue *b=(void *)other;return value->size==b->size && !strcmp(value->type,b->type) && !memcmp(value->bytes,b->bytes,value->size); }
static Word value_hash(struct BoxedValue *value) { Word hash=14695981039346656037ULL;for(size_t i=0;i<value->size;++i)hash=(hash^value->bytes[i])*1099511628211ULL;for(const char *p=value->type;*p;++p)hash=(hash^(unsigned char)*p)*1099511628211ULL;return hash; }
struct BoxedPair { double x,y; };
static Word value_pair(Word cls,const char *selector,struct BoxedPair pair) { return value_encoded(cls,selector,&pair,!strcmp(selector,"valueWithCGPoint:") ? "{CGPoint=dd}" : "{CGSize=dd}"); }
static struct BoxedPair value_pair_get(struct BoxedValue *value,const char *selector) { const char *type=!strcmp(selector,"CGPointValue") ? "{CGPoint=dd}" : "{CGSize=dd}";if(value->size!=sizeof(struct BoxedPair) || strcmp(value->type,type))stop("Boxed value has a different type",selector);struct BoxedPair pair;memcpy(&pair,value->bytes,sizeof(pair));return pair; }
static const char *encoded_size_alignment(const char *encoding,size_t *size,size_t *alignment) { if(!encoding)return NULL;const char *end=encoding;struct InvocationType t=invocation_type(&end,0);if(size)*size=t.size;if(alignment)*alignment=t.alignment;free(t.encoding);return end; }
static Method invocation_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if(!strcmp(selector,"methodSignatureForSelector:") || (class_receiver && !strcmp(selector,"instanceMethodSignatureForSelector:")))return (Method)signature_for_selector;
    if(!receiver)return NULL;
    if(!strcmp(receiver,"_OBJC_CLASS_$_NSValue") && (class_receiver || !class_inherits(read_word(object),external_class("_OBJC_CLASS_$_NSNumber")))) {
        if(class_receiver && (!strcmp(selector,"valueWithBytes:objCType:") || !strcmp(selector,"value:withObjCType:")))return (Method)value_encoded;
        if(class_receiver && (!strcmp(selector,"valueWithCGPoint:") || !strcmp(selector,"valueWithCGSize:")))return (Method)value_pair;
        if(!class_receiver){if(!strcmp(selector,"initWithBytes:objCType:"))return (Method)value_init_encoded;if(!strcmp(selector,"getValue:size:"))return (Method)value_get_sized;
            if(!strcmp(selector,"CGPointValue") || !strcmp(selector,"CGSizeValue"))return (Method)value_pair_get;if(!strcmp(selector,"isEqualToValue:") || !strcmp(selector,"isEqual:"))return (Method)value_equal;if(!strcmp(selector,"hash"))return (Method)value_hash;}
    }
    if(!strcmp(receiver,"_OBJC_CLASS_$_NSMethodSignature")){
        if(class_receiver && !strcmp(selector,"signatureWithObjCTypes:"))return (Method)signature_create;
        if(!class_receiver){if(!strcmp(selector,"numberOfArguments") || !strcmp(selector,"frameLength") || !strcmp(selector,"methodReturnLength"))return (Method)signature_property;
            if(!strcmp(selector,"methodReturnType"))return (Method)signature_return;if(!strcmp(selector,"getArgumentTypeAtIndex:"))return (Method)signature_argument;if(!strcmp(selector,"isOneway"))return (Method)signature_oneway;if(!strcmp(selector,"dealloc"))return (Method)signature_dealloc;}
    }
    if(!strcmp(receiver,"_OBJC_CLASS_$_NSInvocation")){
        if(class_receiver && !strcmp(selector,"invocationWithMethodSignature:"))return (Method)invocation_create;
        if(!class_receiver){if(!strcmp(selector,"setTarget:"))return (Method)invocation_target;if(!strcmp(selector,"setSelector:"))return (Method)invocation_selector;
            if(!strcmp(selector,"target") || !strcmp(selector,"selector") || !strcmp(selector,"methodSignature") || !strcmp(selector,"argumentsRetained"))return (Method)invocation_property;
            if(!strcmp(selector,"setArgument:atIndex:"))return (Method)invocation_set_argument;if(!strcmp(selector,"getArgument:atIndex:"))return (Method)invocation_get_argument;
            if(!strcmp(selector,"retainArguments"))return (Method)invocation_retain_arguments;if(!strcmp(selector,"invoke"))return (Method)invocation_invoke;if(!strcmp(selector,"invokeWithTarget:"))return (Method)invocation_invoke_target;
            if(!strcmp(selector,"getReturnValue:"))return (Method)invocation_get_return;if(!strcmp(selector,"setReturnValue:"))return (Method)invocation_set_return;if(!strcmp(selector,"dealloc"))return (Method)invocation_dealloc;}
    }
    return NULL;
}

#include "block_imp.inc"
static Word resolve_objc_import(const char *name) {
    if (!strcmp(name,"_NSProtocolFromString")) return (Word)protocol_from_string;
    if (!strcmp(name,"_objc_getProtocol")) return (Word)protocol_from_name;
    if (!strcmp(name,"_NSStringFromProtocol")) return (Word)string_from_protocol;
    if (!strcmp(name,"_protocol_getName")) return (Word)protocol_name;
    if (!strcmp(name,"_imp_implementationWithBlock")) return (Word)implementation_with_block;
    if (!strcmp(name,"_imp_getBlock")) return (Word)block_for_implementation;
    if (!strcmp(name,"_imp_removeBlock")) return (Word)remove_block_implementation;
    if (!strcmp(name,"_objc_setHook_getClass")) return (Word)set_class_lookup_hook;
    if (!strcmp(name,"__objc_realizeClassFromSwift")) return (Word)realize_class_from_swift;
    if (!strcmp(name,"__objc_empty_cache")) { static Word buckets[32] __attribute__((aligned(16)));return (Word)buckets; }
    if (!strcmp(name,"_NSGetSizeAndAlignment")) return (Word)encoded_size_alignment;
    if (!strcmp(name,"_objc_setProperty_nonatomic_copy")) return (Word)copy_property;
    if (!strcmp(name,"_objc_setProperty_atomic_copy")) return (Word)copy_atomic_property;
    if (!strcmp(name,"_objc_getClass") || !strcmp(name,"_objc_lookUpClass")) return (Word)class_from_name;
    if (!strcmp(name,"_objc_autorelease")) return (Word)metal_object_autorelease;
    if (!strcmp(name,"_objc_copyWeak")) return (Word)copy_weak;
    if (!strcmp(name,"_objc_copyStruct")) return (Word)copy_struct;
    if (!strcmp(name,"_objc_getAssociatedObject")) return (Word)associated_object;
    if (!strcmp(name,"_objc_setAssociatedObject")) return (Word)associate_object;
    if (!strcmp(name,"_objc_sync_enter")) return (Word)synchronized_enter;
    if (!strcmp(name,"_objc_sync_exit")) return (Word)synchronized_exit;
    if (!strcmp(name,"_objc_copyImageNames")) return (Word)copy_image_names;
    if (!strcmp(name,"_class_getInstanceSize")) return (Word)instance_size;
    if (!strcmp(name,"_class_conformsToProtocol")) return (Word)class_conforms;
    if (!strcmp(name,"_class_addProtocol")) return (Word)class_add_protocol;
    if (!strcmp(name,"_method_getTypeEncoding")) return (Word)method_types;
    if (!strcmp(name,"_method_exchangeImplementations")) return (Word)exchange_methods;
    if (!strcmp(name,"_object_setClass")) return (Word)change_class;
    return 0;
}
