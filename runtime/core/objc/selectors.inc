struct SelectorEntry { struct SelectorEntry *next_name,*next_address; char name[]; };
static struct SelectorEntry *selector_names[4096],*selector_addresses[4096];
static pthread_mutex_t selector_mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned selector_hash(const char *name) { unsigned hash=2166136261U;for(;*name;++name)hash=(hash^(unsigned char)*name)*16777619U;return hash&4095; }
static unsigned char selector_is_mapped(const char *name) {
    if(!name)return 0;
    unsigned bucket=((Word)name>>3)&4095;pthread_mutex_lock(&selector_mutex);
    struct SelectorEntry *entry=selector_addresses[bucket];while(entry && entry->name!=name)entry=entry->next_address;
    pthread_mutex_unlock(&selector_mutex);return entry!=NULL;
}
__attribute__((used,noinline)) static const char *register_selector(const char *name,int lock,int copy) {
    if(!name)return NULL;
    unsigned bucket=selector_hash(name);pthread_mutex_lock(&selector_mutex);
    for(struct SelectorEntry *entry=selector_names[bucket];entry;entry=entry->next_name)
        if(!strcmp(entry->name,name)){pthread_mutex_unlock(&selector_mutex);return entry->name;}
    size_t length=strlen(name);struct SelectorEntry *entry=malloc(sizeof(*entry)+length+1);
    if(!entry){pthread_mutex_unlock(&selector_mutex);stop("Allocation failed","sel_registerName");}
    memcpy(entry->name,name,length+1);entry->next_name=selector_names[bucket];selector_names[bucket]=entry;
    unsigned address_bucket=((Word)entry->name>>3)&4095;entry->next_address=selector_addresses[address_bucket];selector_addresses[address_bucket]=entry;
    pthread_mutex_unlock(&selector_mutex);return entry->name;
}
/* Preserve the Darwin arm64 public-to-internal registrar call shape. */
__attribute__((naked)) static const char *selector_register(const char *name) {
    __asm__ volatile("mov w1,#1\n mov w2,#1\n b register_selector");
}
static unsigned char selector_equal(const char *first,const char *second) { return first==second || (first && second && !strcmp(first,second)); }
static const char *selector_get_name(const char *name) { return name ? name : "<null selector>"; }
static Word resolve_selector_import(const char *name) {
    if(!strcmp(name,"_sel_registerName") || !strcmp(name,"_sel_getUid"))return (Word)selector_register;
    if(!strcmp(name,"_sel_getName"))return (Word)selector_get_name;
    if(!strcmp(name,"_sel_isEqual"))return (Word)selector_equal;
    if(!strcmp(name,"_sel_isMapped"))return (Word)selector_is_mapped;
    return 0;
}
