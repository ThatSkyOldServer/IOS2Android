/* Append-only runtime metadata. Stable method records can be swizzled through
 * the same method_get/setImplementation ABI as original Mach-O records. */
struct DynamicClass {
    Word words[5],meta[5];
    char *name;
    size_t size;
    unsigned registered,initialization;
    pthread_t initializer;
    struct DynamicClass *next;
};
struct AddedMethod { Word cls; struct MethodRecord record; struct AddedMethod *next; };
static struct DynamicClass *dynamic_classes;
static struct AddedMethod *added_methods;
struct AddedProtocol { Word cls,protocol; struct AddedProtocol *next; };
static struct AddedProtocol *added_protocols;
static pthread_mutex_t dynamic_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct DynamicClass *dynamic_class(Word object) {
    for (struct DynamicClass *item=__atomic_load_n(&dynamic_classes,__ATOMIC_ACQUIRE);item;item=item->next)
        if ((Word)item->words==object || (Word)item->meta==object) return item;
    return NULL;
}
static Word dynamic_named_class(const char *name,int registered) {
    for (struct DynamicClass *item=__atomic_load_n(&dynamic_classes,__ATOMIC_ACQUIRE);item;item=item->next)
        if ((!registered || __atomic_load_n(&item->registered,__ATOMIC_ACQUIRE)) && !strcmp(item->name,name)) return (Word)item->words;
    return 0;
}
static struct MethodRecord *added_method(Word cls,const char *selector) {
    for (struct AddedMethod *item=__atomic_load_n(&added_methods,__ATOMIC_ACQUIRE);item;item=item->next)
        if (item->cls==cls && !strcmp(item->record.name,selector)) return &item->record;
    return NULL;
}
