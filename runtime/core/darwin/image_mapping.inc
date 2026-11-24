static int compare_image_address(const void *left,const void *right) {
    Word a=*(const Word *)left,b=*(const Word *)right;return (a>b)-(a<b);
}
static void protect_guest_images(int relocating) {
    Word page=(Word)sysconf(_SC_PAGESIZE);
    if(!page || (page&(page-1)) || aslr%page)stop("Unsupported host page alignment","Mach-O mapping");
    Word edges[2*COUNT(guest_segments)],starts[COUNT(guest_segments)],ends[COUNT(guest_segments)];
    for(size_t i=0;i<COUNT(guest_segments);++i){
        starts[i]=(Word)guest_segments[i].address&~(page-1);
        ends[i]=((Word)guest_segments[i].address+guest_segments[i].memory_size+page-1)&~(page-1);
        edges[2*i]=starts[i];edges[2*i+1]=ends[i];
    }
    qsort(edges,COUNT(edges),sizeof(*edges),compare_image_address);
    for(size_t i=1;i<COUNT(edges);++i){
        Word start=edges[i-1],end=edges[i];if(start==end)continue;
        unsigned protection=0,covered=0;
        for(size_t j=0;j<COUNT(guest_segments);++j)if(starts[j]<end && ends[j]>start){
            covered=1;protection|=guest_segments[j].initial;
            if(relocating)protection|=guest_segments[j].maximum&PROT_WRITE;
        }
        if(covered && mprotect((void *)start,end-start,(int)protection))
            stop("Image protection failure",relocating ? "relocation writes" : "initial mapped permissions");
    }
}
