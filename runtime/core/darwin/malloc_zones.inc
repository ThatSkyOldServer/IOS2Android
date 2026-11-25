 
struct GuestMallocZone;
struct ZoneAllocation {
    void *pointer;
    size_t size;
    struct GuestMallocZone *zone;
    struct ZoneAllocation *hash_next,*zone_next,*zone_previous;
};
struct GuestMallocZone {
    size_t hint;
    struct ZoneAllocation *allocations;
    struct GuestMallocZone *next;
};
static pthread_mutex_t malloc_zones_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct GuestMallocZone *malloc_zones;
static struct ZoneAllocation *zone_allocations[1024];
static size_t zone_live_allocations;
static size_t zone_pointer_bucket(const void *pointer) { return (((Word)pointer>>4)^((Word)pointer>>20))%COUNT(zone_allocations); }
static struct ZoneAllocation **zone_allocation_link(const void *pointer) {
    struct ZoneAllocation **link=&zone_allocations[zone_pointer_bucket(pointer)];
    while (*link && (*link)->pointer!=pointer) link=&(*link)->hash_next;
    return link;
}
static struct GuestMallocZone **malloc_zone_link(struct GuestMallocZone *zone) {
    struct GuestMallocZone **link=&malloc_zones;
    while (*link && *link!=zone) link=&(*link)->next;
    return link;
}
static void zone_unlink_allocation(struct ZoneAllocation *allocation) {
    *zone_allocation_link(allocation->pointer)=allocation->hash_next;
    if (allocation->zone_previous) allocation->zone_previous->zone_next=allocation->zone_next;
    else allocation->zone->allocations=allocation->zone_next;
    if (allocation->zone_next) allocation->zone_next->zone_previous=allocation->zone_previous;
    __atomic_fetch_sub(&zone_live_allocations,1,__ATOMIC_RELEASE);
}
static struct GuestMallocZone *guest_malloc_create_zone(size_t hint,unsigned flags) {
    if (flags) { errno=ENOTSUP; LOG("Unsupported malloc zone flags 0x%x",flags); return NULL; }
    struct GuestMallocZone *zone=calloc(1,sizeof(*zone));
    if (!zone) return NULL;
    zone->hint=hint;
    pthread_mutex_lock(&malloc_zones_mutex);
    zone->next=malloc_zones; malloc_zones=zone;
    pthread_mutex_unlock(&malloc_zones_mutex);
    return zone;
}
static void *guest_malloc_zone_malloc(struct GuestMallocZone *zone,size_t size) {
    pthread_mutex_lock(&malloc_zones_mutex);
    if (!zone || !*malloc_zone_link(zone)) { pthread_mutex_unlock(&malloc_zones_mutex); errno=EINVAL; return NULL; }
    struct ZoneAllocation *allocation=calloc(1,sizeof(*allocation));
    void *pointer=allocation ? malloc(size ? size : 1) : NULL;
    if (!pointer) { free(allocation); pthread_mutex_unlock(&malloc_zones_mutex); errno=ENOMEM; return NULL; }
    allocation->pointer=pointer; allocation->size=size; allocation->zone=zone;
    allocation->zone_next=zone->allocations;
    if (zone->allocations) zone->allocations->zone_previous=allocation;
    zone->allocations=allocation;
    size_t bucket=zone_pointer_bucket(pointer);
    allocation->hash_next=zone_allocations[bucket]; zone_allocations[bucket]=allocation;
    __atomic_fetch_add(&zone_live_allocations,1,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&malloc_zones_mutex);
    LOG_LIMITED(48,"Darwin malloc zone backing allocated: %zu bytes",size);
    return pointer;
}
static struct GuestMallocZone *guest_malloc_zone_from_ptr(const void *pointer) {
    pthread_mutex_lock(&malloc_zones_mutex);
    struct ZoneAllocation *allocation=*zone_allocation_link(pointer);
    struct GuestMallocZone *zone=allocation ? allocation->zone : NULL;
    pthread_mutex_unlock(&malloc_zones_mutex);
    return zone;
}
static void guest_malloc_destroy_zone(struct GuestMallocZone *zone) {
    pthread_mutex_lock(&malloc_zones_mutex);
    struct GuestMallocZone **link=malloc_zone_link(zone);
    if (!zone || !*link) { pthread_mutex_unlock(&malloc_zones_mutex); stop("Invalid malloc zone","malloc_destroy_zone"); }
    *link=zone->next;
    size_t released=0;
    while (zone->allocations) {
        struct ZoneAllocation *allocation=zone->allocations;
        zone_unlink_allocation(allocation); released+=allocation->size;
        free(allocation->pointer); free(allocation);
    }
    pthread_mutex_unlock(&malloc_zones_mutex);
    free(zone);
    LOG("Darwin malloc zone destroyed: %zu bytes released",released);
}
static void guest_zone_aware_free(void *pointer) {
    if (!pointer) return;
    if (__atomic_load_n(&zone_live_allocations,__ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&malloc_zones_mutex);
        struct ZoneAllocation *allocation=*zone_allocation_link(pointer);
        if (allocation) { zone_unlink_allocation(allocation); free(allocation); }
        pthread_mutex_unlock(&malloc_zones_mutex);
    }
    free(pointer);
}
static void *guest_zone_aware_realloc(void *pointer,size_t size) {
    if (!pointer || !__atomic_load_n(&zone_live_allocations,__ATOMIC_ACQUIRE)) return realloc(pointer,size);
    pthread_mutex_lock(&malloc_zones_mutex);
    struct ZoneAllocation **link=zone_allocation_link(pointer),*allocation=*link;
    if (!allocation) { pthread_mutex_unlock(&malloc_zones_mutex); return realloc(pointer,size); }
    void *result=realloc(pointer,size ? size : 1);
    if (result) {
        *link=allocation->hash_next; allocation->pointer=result; allocation->size=size;
        size_t bucket=zone_pointer_bucket(result);
        allocation->hash_next=zone_allocations[bucket]; zone_allocations[bucket]=allocation;
    }
    pthread_mutex_unlock(&malloc_zones_mutex);
    return result;
}
static Word resolve_malloc_zone_import(const char *name) {
    if (!strcmp(name,"_malloc_create_zone")) return (Word)guest_malloc_create_zone;
    if (!strcmp(name,"_malloc_destroy_zone")) return (Word)guest_malloc_destroy_zone;
    if (!strcmp(name,"_malloc_zone_malloc")) return (Word)guest_malloc_zone_malloc;
    if (!strcmp(name,"_malloc_zone_from_ptr")) return (Word)guest_malloc_zone_from_ptr;
    if (!strcmp(name,"_free")) return (Word)guest_zone_aware_free;
    if (!strcmp(name,"_realloc")) return (Word)guest_zone_aware_realloc;
    return 0;
}
