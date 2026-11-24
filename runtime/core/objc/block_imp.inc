/* ARM64 IMP(self, _cmd, ...) -> Block.invoke(block, self, ...).
 * Both signatures consume two integer argument slots. x2-x8, vector
 * arguments, stack arguments and the caller's return address stay intact. */
struct BlockImplementation { void *code; struct Block_layout *block; size_t bytes; struct BlockImplementation *next; };
static struct BlockImplementation *block_implementations;
static pthread_mutex_t block_implementation_mutex=PTHREAD_MUTEX_INITIALIZER;
static Word implementation_with_block(Word value) {
    if (!value) return 0;
    struct Block_layout *block=_Block_copy((void *)value);
    if (!block || !block->invoke) stop("Invalid method block","imp_implementationWithBlock");
    size_t bytes=(size_t)sysconf(_SC_PAGESIZE);
    void *code=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    struct BlockImplementation *entry=calloc(1,sizeof(*entry));
    if (code==MAP_FAILED || !entry) {
        if (code!=MAP_FAILED) munmap(code,bytes);
        free(entry); _Block_release(block); return 0;
    }
    uint32_t instructions[]={0xaa0003e1,0x58000060,0x58000090,0xd61f0200};
    memcpy(code,instructions,sizeof(instructions));
    Word pointers[]={(Word)block,(Word)block->invoke};
    memcpy((char *)code+16,pointers,sizeof(pointers));
    __builtin___clear_cache(code,(char *)code+32);
    if (mprotect(code,bytes,PROT_READ|PROT_EXEC)) {
        munmap(code,bytes);free(entry);_Block_release(block);return 0;
    }
    *entry=(struct BlockImplementation){code,block,bytes,NULL};
    pthread_mutex_lock(&block_implementation_mutex);
    entry->next=block_implementations;block_implementations=entry;
    pthread_mutex_unlock(&block_implementation_mutex);
    return (Word)code;
}
static Word block_for_implementation(Word implementation) {
    pthread_mutex_lock(&block_implementation_mutex);
    struct BlockImplementation *entry=block_implementations;
    while(entry && (Word)entry->code!=implementation)entry=entry->next;
    Word block=entry ? (Word)entry->block : 0;
    pthread_mutex_unlock(&block_implementation_mutex);
    return block;
}
static unsigned char remove_block_implementation(Word implementation) {
    pthread_mutex_lock(&block_implementation_mutex);
    struct BlockImplementation **link=&block_implementations;
    while(*link && (Word)(*link)->code!=implementation)link=&(*link)->next;
    struct BlockImplementation *entry=*link;
    if(entry)*link=entry->next;
    pthread_mutex_unlock(&block_implementation_mutex);
    if(!entry)return 0;
    _Block_release(entry->block);munmap(entry->code,entry->bytes);free(entry);return 1;
}
