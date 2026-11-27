/* Clang's non-fragile ObjC ABI emits each class against the superclass size
 * visible in that translation unit. libobjc normally slides subclass ivars
 * when the loaded superclass is larger. This is runtime realization, not a
 * patch to the original executable's instructions or on-disk metadata. */
static unsigned moved_classes,moved_ivars;
static pthread_mutex_t initialize_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t initialize_condition=PTHREAD_COND_INITIALIZER;
static int class_ro_valid(Word cls,Word ro) {
    if(image_segment_for(ro,72))return 1;
    struct ClassLayout *layout=class_layout(cls);
    return layout && __atomic_load_n(&layout->ro_copy,__ATOMIC_ACQUIRE)==ro;
}

static uint32_t external_instance_size(Word cls) {
    const char *name=host_symbol(cls);
    if (!name) return 8;
    if (!strcmp(name,"_OBJC_CLASS_$_NSOperation") || !strcmp(name,"_OBJC_CLASS_$_NSBlockOperation") || !strcmp(name,"_OBJC_CLASS_$_NSInvocationOperation")) return 24;
    /* Abstract collection bases contribute only isa to guest subclasses.
     * Host container payloads belong to the host allocation, not this ABI. */
    if (!strcmp(name,"_OBJC_CLASS_$_NSArray") || !strcmp(name,"_OBJC_CLASS_$_NSMutableArray") ||
        !strcmp(name,"_OBJC_CLASS_$_NSSet") || !strcmp(name,"_OBJC_CLASS_$_NSMutableSet") ||
        !strcmp(name,"_OBJC_CLASS_$_NSDictionary") || !strcmp(name,"_OBJC_CLASS_$_NSMutableDictionary")) return sizeof(Word);
    if (!strcmp(name,"_OBJC_CLASS_$_NSString") || !strcmp(name,"_OBJC_CLASS_$_NSMutableString") ||
        !strcmp(name,"_OBJC_CLASS_$_NSData") || !strcmp(name,"_OBJC_CLASS_$_NSMutableData")) return sizeof(Word);
    if (!strcmp(name,"_OBJC_CLASS_$_NSRecursiveLock")) return 24;
    if (!strcmp(name,"_OBJC_CLASS_$_NSSortDescriptor")) return sizeof(struct HostObject);
    if (!strcmp(name,"_OBJC_CLASS_$_UIAlertController")) return sizeof(struct UIKitAlert);
    if (!strcmp(name,"_OBJC_CLASS_$_UIAlertAction")) return sizeof(struct UIKitAlertAction);
    if (!strcmp(name,"_OBJC_CLASS_$_UIAlertView") || !strcmp(name,"_OBJC_CLASS_$_UIActionSheet")) return sizeof(struct UIKitLegacyAlert);
    if (!strcmp(name,"_OBJC_CLASS_$_SKPayment") || !strcmp(name,"_OBJC_CLASS_$_SKMutablePayment")) return sizeof(struct StorePayment);
    if (!strcmp(name,"_OBJC_CLASS_$_SKPaymentTransaction")) return sizeof(struct StoreTransaction);
    if (!strcmp(name,"_OBJC_CLASS_$_NSLayoutConstraint")) return sizeof(struct LayoutConstraint);
    if (!strcmp(name,"_OBJC_CLASS_$_UIColor")) return sizeof(struct UIKitColor);
    if (!strcmp(name,"_OBJC_CLASS_$_CLLocation")) return sizeof(struct LocationValue);
    return 8;  
}

static Word swift_metadata_initializer(Word cls) {
    if (!in_image(cls) || !(read_word(cls+32)&3)) return 0;
    Word ro=read_word(cls+32)&~(Word)7;
    const struct I2AImageSegment *region=image_segment_for(ro,72);
    struct ClassLayout *layout=class_layout(cls);
    int copied=layout && __atomic_load_n(&layout->ro_copy,__ATOMIC_ACQUIRE)==ro;
    if (!region && !copied) stop("Invalid Swift class metadata",class_name(cls));
    uint32_t flags; memcpy(&flags,(void *)ro,4);
    if (!(flags&(1U<<6))) return 0;
    if (!copied && !image_segment_for(ro,80)) stop("Truncated Swift metadata initializer",class_name(cls));
    Word initializer=read_word(ro+72);
    region=image_segment_for(initializer,4);
    if (!region || !(region->initial&PROT_EXEC)) stop("Invalid Swift metadata initializer",class_name(cls));
    return initializer;
}
static uint32_t realize_class_layout_body(Word cls,unsigned depth) {
    if (!cls) return 0;
    if (!in_image(cls)) return external_instance_size(cls);
    if (depth>=128) stop("Invalid class inheritance depth","Objective-C realization");
    struct ClassLayout *layout=class_layout(cls);
    if (!layout) stop("Unlisted superclass","Objective-C realization");
    if (layout->done) return layout->size;
    Word swift_initializer=swift_metadata_initializer(cls);
    if (swift_initializer && layout->swift_initialization!=2) {
        if (layout->swift_initialization==1) stop("Recursive Swift metadata initialization",class_name(cls));
        layout->swift_initialization=1;
        LOG("Swift metadata initializer: %s at 0x%lx",class_name(cls),swift_initializer-aslr+GUEST_IMAGE_BASE);
        Word result=((Word (*)(Word,Word))swift_initializer)(cls,0);
        if (result!=cls) stop("Unsupported relocated Swift class",class_name(cls));
        if (!layout->done) stop("Swift initializer did not realize class",class_name(cls));
        return layout->size;
    }
    if (layout->visiting) stop("Cyclic class inheritance",class_name(cls));
    layout->visiting=1;
    uint32_t super_size=realize_class_layout(read_word(cls+8),depth+1);
    Word ro=read_word(cls+32)&~(Word)7;
    if (!class_ro_valid(cls,ro)) stop("Invalid class metadata",class_name(cls));
    uint32_t start,size;
    memcpy(&start,(void *)(ro+4),4); memcpy(&size,(void *)(ro+8),4);
    if (size<start) stop("Invalid instance bounds",class_name(cls));
    Word ivars=read_word(ro+48);
    uint32_t count=0,alignment=1;
    if (ivars) {
        uint32_t stride;
        memcpy(&stride,(void *)ivars,4); memcpy(&count,(void *)(ivars+4),4);
        if (stride!=32 || count>4096) stop("Invalid ivar list",class_name(cls));
        for (uint32_t i=0;i<count;++i) {
            uint32_t exponent;
            memcpy(&exponent,(void *)(ivars+8+32*i+24),4);
            if (exponent==UINT32_MAX) exponent=3;
            if (exponent>15) stop("Invalid ivar alignment",class_name(cls));
            if ((1U<<exponent)>alignment) alignment=1U<<exponent;
        }
    }
    if (super_size>start) {
        uint64_t delta=((uint64_t)super_size-start+alignment-1)&~((uint64_t)alignment-1);
        if (delta>UINT32_MAX-size) stop("Realized instance too large",class_name(cls));
        for (uint32_t i=0;i<count;++i) {
            Word pointer=read_word(ivars+8+32*i);
            if (!pointer) continue;
            const struct I2AImageSegment *region=image_segment_for(pointer,4);
            if (!region || !(region->initial&PROT_WRITE)) stop("Ivar offset cannot be relocated",class_name(cls));
            uint32_t offset;
            memcpy(&offset,(void *)pointer,4);
            if (offset<start || delta>INT32_MAX-offset) stop("Invalid realized ivar",class_name(cls));
            offset+=(uint32_t)delta;
            memcpy((void *)pointer,&offset,4);
            __atomic_fetch_add(&moved_ivars,1,__ATOMIC_RELAXED);
        }
        /* libobjc copies class_ro_t before changing instance bounds. Its
         * original may live in an executable/read-only segment. */
        uint32_t flags;memcpy(&flags,(void *)ro,4);
        size_t ro_bytes=(flags&(1U<<6)) ? 80 : 72;
        void *copy=malloc(ro_bytes);
        if(!copy)stop("Allocation failed","realized class metadata");
        memcpy(copy,(void *)ro,ro_bytes);
        const struct I2AImageSegment *class_region=image_segment_for(cls+32,8);
        if(!class_region || !(class_region->initial&PROT_WRITE)){free(copy);stop("Class metadata pointer is read-only",class_name(cls));}
        ro=(Word)copy;
        start+=(uint32_t)delta; size+=(uint32_t)delta;
        memcpy((void *)(ro+4),&start,4); memcpy((void *)(ro+8),&size,4);
        __atomic_store_n(&layout->ro_copy,ro,__ATOMIC_RELEASE);
        __atomic_store_n((Word *)(cls+32),ro|(read_word(cls+32)&7),__ATOMIC_RELEASE);
        __atomic_fetch_add(&moved_classes,1,__ATOMIC_RELAXED);
    }
    layout->size=size; layout->done=1; layout->visiting=0;
    return size;
}

static uint32_t realize_class_layout(Word cls,unsigned depth) {
    if(!cls)return 0;
    if(!in_image(cls))return external_instance_size(cls);
    struct ClassLayout *layout=class_layout(cls);
    if(!layout)stop("Unlisted superclass","Objective-C realization");
    if(__atomic_load_n(&layout->realization,__ATOMIC_ACQUIRE)==2)return layout->size;
    pthread_mutex_lock(&realization_mutex);
    while(layout->realization==1 && !pthread_equal(layout->realizer,pthread_self()))
        pthread_cond_wait(&realization_condition,&realization_mutex);
    unsigned state=layout->realization;
    if(!state){layout->realizer=pthread_self();__atomic_store_n(&layout->realization,1,__ATOMIC_RELEASE);}
    pthread_mutex_unlock(&realization_mutex);
    if(state==2)return layout->size;
    if(state==3)stop("Class realization previously failed",class_name(cls));
    if(state==1){
        if(layout->swift_initialization==2 && !layout->visiting)return realize_class_layout_body(cls,depth);
        stop("Recursive class realization",class_name(cls));
    }
    struct GuestCallBoundary call={.previous=active_guest_call_boundary};
    active_guest_call_boundary=&call;
    int failed=setjmp(call.environment);
    if(!failed)realize_class_layout_body(cls,depth);
    active_guest_call_boundary=call.previous;
    pthread_mutex_lock(&realization_mutex);
    __atomic_store_n(&layout->realization,failed ? 3U : 2U,__ATOMIC_RELEASE);
    pthread_cond_broadcast(&realization_condition);
    pthread_mutex_unlock(&realization_mutex);
    if(failed)propagate_guest_failure();
    return layout->size;
}

static Word realize_class_from_swift(Word cls,Word previous) {
    if (!cls || (previous && previous!=cls)) stop("Unsupported Swift class relocation","_objc_realizeClassFromSwift");
    struct ClassLayout *layout=class_layout(cls);
    if(layout){
        pthread_mutex_lock(&realization_mutex);
        while(layout->realization==1 && !pthread_equal(layout->realizer,pthread_self()))
            pthread_cond_wait(&realization_condition,&realization_mutex);
        layout->swift_initialization=2;
        pthread_mutex_unlock(&realization_mutex);
        realize_class_layout(cls,0);
        return cls;
    }
    stop("Unregistered Swift class","_objc_realizeClassFromSwift");
}
static void realize_class_layouts(void) {
    const Word *classes=(const Word *)(aslr+CLASSES_OFFSET);
    for(unsigned i=0;i<CLASSES_COUNT;++i){
        class_layouts[i].cls=classes[i];
        size_t slot=class_pointer_bucket(classes[i],COUNT(class_layout_index));
        while(class_layout_index[slot])slot=(slot+1)%COUNT(class_layout_index);
        class_layout_index[slot]=i+1;
    }
    const Word *categories=(const Word *)(aslr+CATEGORIES_OFFSET);
    unsigned count=0;
    for(unsigned i=0;i<CATEGORIES_COUNT;++i){
        Word category=categories[i],cls=read_word(category+8);
        if(!cls)continue;
        for(unsigned meta=0;meta<2;++meta){
            Word target=meta ? read_word(cls) : cls;
            if(!target)continue;
            size_t bucket=class_pointer_bucket(target,COUNT(category_buckets));
            category_methods[++count]=(struct CategoryMethods){target,category+16+8*meta,category_buckets[bucket]};
            category_buckets[bucket]=count;
        }
    }
    class_layouts_ready=1;
    LOG("Objective-C classes registered for lazy realization: %u",CLASSES_COUNT);
}

static void initialize_guest_class(Word cls) {
    struct DynamicClass *dynamic=dynamic_class(cls);
    unsigned *state;pthread_t *owner;Word superclass,metaclass;
    if(dynamic){
        if(cls!=(Word)dynamic->words)return;
        state=&dynamic->initialization;owner=&dynamic->initializer;
        superclass=dynamic->words[1];metaclass=dynamic->words[0];
    }else{
        if(!in_image(cls))return;
        realize_class_layout(cls,0);
        struct ClassLayout *layout=class_layout(cls);
        if(!layout)return;
        state=&layout->initialization;owner=&layout->initializer;
        superclass=read_word(cls+8);metaclass=read_word(cls);
    }
    if(__atomic_load_n(state,__ATOMIC_ACQUIRE)==2)return;
    initialize_guest_class(superclass);
    pthread_mutex_lock(&initialize_mutex);
    while(*state==1 && !pthread_equal(*owner,pthread_self()))pthread_cond_wait(&initialize_condition,&initialize_mutex);
    unsigned previous=*state;
    if(!previous){*owner=pthread_self();__atomic_store_n(state,1,__ATOMIC_RELEASE);}
    pthread_mutex_unlock(&initialize_mutex);
    if(previous==3)stop("Class initialization previously failed",class_name(cls));
    if(previous)return;
    struct GuestCallBoundary call={.previous=active_guest_call_boundary};
    active_guest_call_boundary=&call;
    int failed=setjmp(call.environment);
    if(!failed){
        Method method=lookup(metaclass,"initialize");
        if(method){LOG("+[%s initialize]",class_name(cls));method(cls,"initialize",0,0,0,0);}
    }
    active_guest_call_boundary=call.previous;
    pthread_mutex_lock(&initialize_mutex);
    __atomic_store_n(state,failed ? 3U : 2U,__ATOMIC_RELEASE);
    pthread_cond_broadcast(&initialize_condition);
    pthread_mutex_unlock(&initialize_mutex);
    if(failed)propagate_guest_failure();
}

static void construct_guest_ivars(Word object,Word cls) {
    if (dynamic_class(cls)) { construct_guest_ivars(object,read_word(cls+8)); return; }
    if (!in_image(cls)) return;
    construct_guest_ivars(object,read_word(cls+8));
    Word ro=read_word(cls+32)&~(Word)7;
    Method construct=method_list(read_word(ro+32),".cxx_construct");
    if (construct && !construct(object,".cxx_construct",0,0,0,0)) stop("C++ ivar construction failed",class_name(cls));
}
