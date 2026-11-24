/* Materialize immutable native method entries into writable runtime records.
 * Relative entries use field-relative signed offsets; selector references are
 * indirect unless the direct-selector flag is set. Swizzling always updates
 * these stable records, including for methods originally stored in __TEXT. */
struct GuestMethodList { Word source; uint32_t count; struct GuestMethodList *next; struct MethodRecord records[]; };
static struct GuestMethodList *guest_method_lists[4096];
static pthread_mutex_t guest_method_lists_mutex=PTHREAD_MUTEX_INITIALIZER;
static Word relative_method_pointer(Word field) {
    int32_t offset;memcpy(&offset,(void *)field,4);return field+(intptr_t)offset;
}
static Word find_method_record(Word list,const char *selector) {
    if (!list || !selector) return 0;
    unsigned bucket=(list>>3)&(COUNT(guest_method_lists)-1);
    pthread_mutex_lock(&guest_method_lists_mutex);
    struct GuestMethodList *cached=guest_method_lists[bucket];
    while(cached && cached->source!=list)cached=cached->next;
    if (!cached) {
        uint32_t flags=0,count=0;
        if (!image_segment_for(list,8)) { pthread_mutex_unlock(&guest_method_lists_mutex);stop("Invalid method list",selector); }
        memcpy(&flags,(void *)list,4);memcpy(&count,(void *)(list+4),4);
        unsigned relative=(flags>>31),stride=flags&0xfffc;
        if (stride!=(relative ? 12U : 24U) || count>1000000 || !image_segment_for(list,8+(size_t)count*stride)) {
            pthread_mutex_unlock(&guest_method_lists_mutex);stop("Unsupported method list",selector);
        }
        cached=calloc(1,sizeof(*cached)+(size_t)count*sizeof(struct MethodRecord));
        if(!cached){pthread_mutex_unlock(&guest_method_lists_mutex);stop("Allocation failed","Objective-C method list");}
        cached->source=list;cached->count=count;
        for(uint32_t i=0;i<count;++i){
            Word entry=list+8+(size_t)i*stride,name,types,implementation;
            if(relative){
                name=relative_method_pointer(entry);
                if(!(flags&0x40000000U)) {
                    if(!image_segment_for(name,8)){free(cached);pthread_mutex_unlock(&guest_method_lists_mutex);stop("Invalid method selector reference",selector);}
                    name=read_word(name);
                }
                types=relative_method_pointer(entry+4);implementation=relative_method_pointer(entry+8);
            }else{name=read_word(entry);types=read_word(entry+8);implementation=read_word(entry+16);}
            if((!in_image(name) && !selector_is_mapped((void *)name)) || !in_image(types) || !in_image(implementation)){
                free(cached);pthread_mutex_unlock(&guest_method_lists_mutex);stop("Invalid method entry",selector);
            }
            cached->records[i]=(struct MethodRecord){register_selector((void *)name,1,1),(void *)types,(Method)implementation};
        }
        cached->next=guest_method_lists[bucket];guest_method_lists[bucket]=cached;
    }
    pthread_mutex_unlock(&guest_method_lists_mutex);
    for(uint32_t i=0;i<cached->count;++i)if(!strcmp(cached->records[i].name,selector))return (Word)&cached->records[i];
    return 0;
}
