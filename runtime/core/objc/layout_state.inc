struct ClassLayout {
    Word cls,ro_copy;
    uint32_t size;
    unsigned visiting,done,initialization,swift_initialization,realization;
    pthread_t initializer,realizer;
};
static struct ClassLayout class_layouts[CLASSES_COUNT];
static unsigned class_layout_index[2*CLASSES_COUNT+1];
static pthread_mutex_t realization_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t realization_condition=PTHREAD_COND_INITIALIZER;
static size_t class_pointer_bucket(Word cls,size_t count) { return ((cls>>3)^(cls>>19))%count; }
static struct ClassLayout *class_layout(Word cls) {
    if(!cls || !class_layouts_ready)return NULL;
    size_t slot=class_pointer_bucket(cls,COUNT(class_layout_index));
    while(class_layout_index[slot]){
        struct ClassLayout *entry=&class_layouts[class_layout_index[slot]-1];
        if(entry->cls==cls)return entry;
        slot=(slot+1)%COUNT(class_layout_index);
    }
    return NULL;
}
struct CategoryMethods { Word cls,list; unsigned next; };
static struct CategoryMethods category_methods[2*CATEGORIES_COUNT+1];
static unsigned category_buckets[2*CATEGORIES_COUNT+1];
