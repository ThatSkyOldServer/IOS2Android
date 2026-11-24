extern size_t lzfse_decode_buffer(uint8_t *,size_t,const uint8_t *,size_t,void *);
extern size_t lzfse_decode_scratch_size(void);
static size_t guest_compression_decode(void *destination,size_t capacity,const void *source,size_t bytes,void *scratch,int algorithm) {
    if((!destination && capacity) || (!source && bytes))return 0;
    if(algorithm==0x801){
        size_t result=lzfse_decode_buffer(destination,capacity,source,bytes,scratch);
        LOG("LZFSE decoded: %zu -> %zu bytes (capacity=%zu)",bytes,result,capacity);
        return result;
    }
    char detail[80];snprintf(detail,sizeof(detail),"compression_decode_buffer algorithm=0x%x",algorithm);
    stop("Unsupported compression algorithm",detail);
}
static size_t guest_compression_scratch(int algorithm) { return algorithm==0x801 ? lzfse_decode_scratch_size() : 0; }
static Word resolve_compression_import(const char *name) {
    if(!strcmp(name,"_compression_decode_buffer"))return (Word)guest_compression_decode;
    if(!strcmp(name,"_compression_decode_scratch_buffer_size"))return (Word)guest_compression_scratch;
    return 0;
}
