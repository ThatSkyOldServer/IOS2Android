 
static int32_t atomic_add32(int32_t delta,volatile int32_t *value) { return __atomic_add_fetch(value,delta,__ATOMIC_RELAXED); }
static int32_t atomic_add32_barrier(int32_t delta,volatile int32_t *value) { return __atomic_add_fetch(value,delta,__ATOMIC_SEQ_CST); }
static int32_t atomic_increment32(volatile int32_t *value) { return atomic_add32(1,value); }
static int32_t atomic_increment32_barrier(volatile int32_t *value) { return atomic_add32_barrier(1,value); }
static int32_t atomic_decrement32(volatile int32_t *value) { return atomic_add32(-1,value); }
static int32_t atomic_decrement32_barrier(volatile int32_t *value) { return atomic_add32_barrier(-1,value); }
static unsigned char atomic_swap32(int32_t expected,int32_t desired,volatile int32_t *value) {
    return __atomic_compare_exchange_n(value,&expected,desired,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
}
static unsigned char atomic_swap32_barrier(int32_t expected,int32_t desired,volatile int32_t *value) {
    return __atomic_compare_exchange_n(value,&expected,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
static int64_t atomic_add64(int64_t delta,volatile int64_t *value) { return __atomic_add_fetch(value,delta,__ATOMIC_RELAXED); }
static int64_t atomic_add64_barrier(int64_t delta,volatile int64_t *value) { return __atomic_add_fetch(value,delta,__ATOMIC_SEQ_CST); }
static int64_t atomic_increment64(volatile int64_t *value) { return atomic_add64_barrier(1,value); }
static int64_t atomic_decrement64(volatile int64_t *value) { return atomic_add64_barrier(-1,value); }
static unsigned char atomic_swap64(int64_t expected,int64_t desired,volatile int64_t *value) {
    return __atomic_compare_exchange_n(value,&expected,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
static unsigned char atomic_swap_pointer(void *expected,void *desired,void *volatile *value) {
    return __atomic_compare_exchange_n(value,&expected,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
static void spin_lock(volatile int32_t *value) {
    for (;;) {
        if (!__atomic_exchange_n(value,1,__ATOMIC_ACQUIRE)) return;
        while (__atomic_load_n(value,__ATOMIC_RELAXED)) sched_yield();
    }
}
static void spin_unlock(volatile int32_t *value) { __atomic_store_n(value,0,__ATOMIC_RELEASE); }
#include <linux/futex.h>
/* Four-byte, zero-initialized lock storage; use the same owner identity as
 * mach_thread_self. Android futexes provide blocking acquire/release semantics.
 * Darwin workqueue/adaptive-spin options are scheduling hints only here. */
static uint32_t unfair_lock_owner(void) { return 0x80000000U|(uint32_t)syscall(SYS_gettid); }
static unsigned char unfair_trylock(uint32_t *lock) {
    uint32_t unlocked=0;
    return __atomic_compare_exchange_n(lock,&unlocked,unfair_lock_owner(),0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}
static void unfair_lock_options(uint32_t *lock,uint32_t options) {
    if(options&~0x50000U)stop("Unsupported unfair lock options","os_unfair_lock_lock_with_options");
    uint32_t owner=unfair_lock_owner();
    for(;;){
        uint32_t expected=0;
        if(__atomic_compare_exchange_n(lock,&expected,owner,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
        if(expected==owner)stop("Recursive unfair lock","os_unfair_lock_lock");
        if(syscall(SYS_futex,lock,FUTEX_WAIT_PRIVATE,expected,NULL,NULL,0)<0 && errno!=EAGAIN && errno!=EINTR)
            stop("Unfair lock wait failed","os_unfair_lock_lock");
    }
}
static void unfair_lock(uint32_t *lock) { unfair_lock_options(lock,0); }
static void unfair_unlock(uint32_t *lock) {
    uint32_t expected=unfair_lock_owner();
    if(!__atomic_compare_exchange_n(lock,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))stop("Unfair lock owner mismatch","os_unfair_lock_unlock");
    syscall(SYS_futex,lock,FUTEX_WAKE_PRIVATE,1,NULL,NULL,0);
}
static Word resolve_atomic_import(const char *name) {
    if (!strcmp(name,"_os_unfair_lock_lock")) return (Word)unfair_lock;
    if (!strcmp(name,"_os_unfair_lock_lock_with_options")) return (Word)unfair_lock_options;
    if (!strcmp(name,"_os_unfair_lock_trylock")) return (Word)unfair_trylock;
    if (!strcmp(name,"_os_unfair_lock_unlock")) return (Word)unfair_unlock;
    if (!strcmp(name,"_OSAtomicAdd32")) return (Word)atomic_add32;
    if (!strcmp(name,"_OSAtomicAdd32Barrier")) return (Word)atomic_add32_barrier;
    if (!strcmp(name,"_OSAtomicIncrement32")) return (Word)atomic_increment32;
    if (!strcmp(name,"_OSAtomicIncrement32Barrier")) return (Word)atomic_increment32_barrier;
    if (!strcmp(name,"_OSAtomicDecrement32")) return (Word)atomic_decrement32;
    if (!strcmp(name,"_OSAtomicDecrement32Barrier")) return (Word)atomic_decrement32_barrier;
    if (!strcmp(name,"_OSAtomicCompareAndSwap32")) return (Word)atomic_swap32;
    if (!strcmp(name,"_OSAtomicCompareAndSwap32Barrier")) return (Word)atomic_swap32_barrier;
    if (!strcmp(name,"_OSAtomicAdd64")) return (Word)atomic_add64;
    if (!strcmp(name,"_OSAtomicAdd64Barrier")) return (Word)atomic_add64_barrier;
    if (!strcmp(name,"_OSAtomicIncrement64Barrier")) return (Word)atomic_increment64;
    if (!strcmp(name,"_OSAtomicDecrement64Barrier")) return (Word)atomic_decrement64;
    if (!strcmp(name,"_OSAtomicCompareAndSwap64Barrier")) return (Word)atomic_swap64;
    if (!strcmp(name,"_OSAtomicCompareAndSwapPtrBarrier")) return (Word)atomic_swap_pointer;
    if (!strcmp(name,"_OSSpinLockLock")) return (Word)spin_lock;
    if (!strcmp(name,"_OSSpinLockUnlock")) return (Word)spin_unlock;
    return 0;
}
