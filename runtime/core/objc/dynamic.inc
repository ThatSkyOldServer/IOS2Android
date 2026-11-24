static Word allocate_class_pair(Word superclass,const char *name,size_t extra_bytes) {
    if (!name || !*name || !class_name(superclass) || extra_bytes>1024*1024) return 0;
    if (class_from_name(name) || dynamic_named_class(name,0)) return 0;
    struct DynamicClass *pair=calloc(1,sizeof(*pair)+extra_bytes);
    if (!pair) return 0;
    pair->name=strdup(name); pair->size=instance_size(superclass);
    pair->words[0]=(Word)pair->meta; pair->words[1]=superclass;
    Word parent_meta=read_word(superclass);
    pair->meta[0]=parent_meta ? read_word(parent_meta) : (Word)pair->meta;
    pair->meta[1]=parent_meta;
    pthread_mutex_lock(&dynamic_mutex);
    if (dynamic_named_class(name,0)) { pthread_mutex_unlock(&dynamic_mutex); free(pair->name); free(pair); return 0; }
    pair->next=dynamic_classes; __atomic_store_n(&dynamic_classes,pair,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&dynamic_mutex); return (Word)pair->words;
}
static void register_class_pair(Word cls) {
    struct DynamicClass *pair=dynamic_class(cls);
    if (!pair || cls!=(Word)pair->words) stop("Invalid dynamic class","objc_registerClassPair");
    __atomic_store_n(&pair->registered,1,__ATOMIC_RELEASE);
    LOG("Registered original SDK dynamic class %s (super=%s)",pair->name,class_name(pair->words[1]));
}
static unsigned char add_class_method(Word cls,const char *selector,Method implementation,const char *types) {
    if (!class_name(cls) || !selector || !implementation || !types) return 0;
    if(owned_method_record(cls,selector))return 0;
    pthread_mutex_lock(&dynamic_mutex);
    if (added_method(cls,selector)) { pthread_mutex_unlock(&dynamic_mutex); return 0; }
    struct AddedMethod *entry=calloc(1,sizeof(*entry));
    if (!entry) { pthread_mutex_unlock(&dynamic_mutex); return 0; }
    entry->cls=cls; entry->record=(struct MethodRecord){strdup(selector),strdup(types),implementation};
    entry->next=added_methods; __atomic_store_n(&added_methods,entry,__ATOMIC_RELEASE);
    __atomic_add_fetch(&method_cache_epoch,1,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&dynamic_mutex); return 1;
}
static Word object_superclass(Word object) {
    Word cls=host_symbol(object) || guest_class_object(object) ? object : read_word(object);
    if (in_image(cls) || dynamic_class(cls)) return read_word(cls+8);
    return host_superclass(cls);
}
static Method replace_class_method(Word cls,const char *selector,Method implementation,const char *types) {
    if(!cls || !selector || !implementation || !types)return NULL;
    struct MethodRecord *record=owned_method_record(cls,selector);
    if(record)return set_imp(record,implementation);
    add_class_method(cls,selector,implementation,types);return NULL;
}
static Word resolve_dynamic_import(const char *name) {
    if (!strcmp(name,"_class_replaceMethod")) return (Word)replace_class_method;
    if (!strcmp(name,"_objc_allocateClassPair")) return (Word)allocate_class_pair;
    if (!strcmp(name,"_objc_registerClassPair")) return (Word)register_class_pair;
    if (!strcmp(name,"_class_addMethod")) return (Word)add_class_method;
    return 0;
}
