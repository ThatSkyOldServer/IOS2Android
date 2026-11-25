/* BSD/Darwin socket ABI -> Android sockets. Addresses, addrinfo/msghdr, ancillary
 * alignment, flags, timeouts and error numbers follow socket-reference sources. */
#include <netdb.h>
#include <poll.h>
#include <sys/time.h>
#include <netinet/tcp.h>
 
_Static_assert(sizeof(struct hostent)==32 && offsetof(struct hostent,h_addr_list)==24 && sizeof(in_addr_t)==4,"Darwin IPv4 hostent ABI");
struct DarwinAddrInfo { int flags,family,type,protocol; uint32_t length; char *canonical; void *address; struct DarwinAddrInfo *next; };
struct DarwinSocketTimeval { int64_t seconds; int32_t microseconds,padding; };
struct DarwinMessage { void *name; uint32_t name_length; struct iovec *iov; int iov_count; void *control; uint32_t control_length; int flags; };
struct DarwinControl { uint32_t length; int level,type; };
_Static_assert(sizeof(struct DarwinAddrInfo)==48 && offsetof(struct DarwinAddrInfo,address)==32,"Darwin addrinfo ABI");
_Static_assert(sizeof(struct DarwinMessage)==48 && offsetof(struct DarwinMessage,flags)==44,"Darwin msghdr ABI");
static int socket_errno(int value) {
    switch (value) {
    case EAGAIN:return 35; case EINPROGRESS:return 36; case EALREADY:return 37; case ENOTSOCK:return 38;
    case EDESTADDRREQ:return 39; case EMSGSIZE:return 40; case EPROTOTYPE:return 41; case ENOPROTOOPT:return 42;
    case EPROTONOSUPPORT:return 43; case ESOCKTNOSUPPORT:return 44; case EOPNOTSUPP:return 45; case EPFNOSUPPORT:return 46;
    case EAFNOSUPPORT:return 47; case EADDRINUSE:return 48; case EADDRNOTAVAIL:return 49; case ENETDOWN:return 50;
    case ENETUNREACH:return 51; case ENETRESET:return 52; case ECONNABORTED:return 53; case ECONNRESET:return 54;
    case ENOBUFS:return 55; case EISCONN:return 56; case ENOTCONN:return 57; case ESHUTDOWN:return 58;
    case ETOOMANYREFS:return 59; case ETIMEDOUT:return 60; case ECONNREFUSED:return 61; case ELOOP:return 62;
    case ENAMETOOLONG:return 63; case EHOSTDOWN:return 64; case EHOSTUNREACH:return 65; case ENOTEMPTY:return 66;
    case EDEADLK:return 11; case ENOLCK:return 77; case ENOSYS:return 78; case EOVERFLOW:return 84; case ECANCELED:return 89;
    default:return value;
    }
}
static ssize_t socket_result(ssize_t result) { if (result<0) errno=socket_errno(errno); return result; }
/* _GNU_SOURCE selects Android's separate __gnu_strerror_r alias. Darwin uses
 * the POSIX int-returning symbol, so name that export explicitly. */
extern int android_posix_strerror_r(int,char *,size_t) __asm__("strerror_r");
static int socket_strerror_r(int error,char *buffer,size_t size) {
    int native=error;
    for (int candidate=1;candidate<256;++candidate) if (socket_errno(candidate)==error) { native=candidate; break; }
    int result=android_posix_strerror_r(native,buffer,size); return result ? socket_errno(result) : 0;
}
static int socket_family(int value) { return value==30 ? AF_INET6 : value; }
static int socket_native_address(const void *input,socklen_t length,struct sockaddr_storage *output) {
    if (!input || length<2 || length>sizeof(*output)) { errno=EFAULT; return -1; }
    const unsigned char *bytes=input; int family=socket_family(bytes[1]);
    if ((family==AF_INET && length<sizeof(struct sockaddr_in)) || (family==AF_INET6 && length<sizeof(struct sockaddr_in6))) { errno=EINVAL; return -1; }
    memset(output,0,sizeof(*output)); memcpy(output,input,length); ((struct sockaddr *)output)->sa_family=(sa_family_t)family; return 0;
}
static int socket_guest_address(const struct sockaddr *input,socklen_t length,void *output,socklen_t *capacity) {
    if (!capacity) return output ? -1 : 0;
    socklen_t copied=*capacity<length ? *capacity : length;
    if (output && copied) {
        memcpy(output,input,copied);
        ((unsigned char *)output)[0]=(unsigned char)length;
        if (copied>=2) ((unsigned char *)output)[1]=(unsigned char)(input->sa_family==AF_INET6 ? 30 : input->sa_family);
    }
    *capacity=length; return 0;
}
static int socket_eai(int result) {
    switch (result) {
    case 0:return 0; case EAI_AGAIN:return 2; case EAI_BADFLAGS:return 3; case EAI_FAIL:return 4; case EAI_FAMILY:return 5;
    case EAI_MEMORY:return 6; case EAI_NONAME:return 8; case EAI_SERVICE:return 9; case EAI_SOCKTYPE:return 10;
    case EAI_SYSTEM:errno=socket_errno(errno);return 11; case EAI_OVERFLOW:return 14; default:return 4;
    }
}
static void socket_free_addrinfo(struct DarwinAddrInfo *list) {
    while (list) { struct DarwinAddrInfo *next=list->next; free(list->canonical); free(list->address); free(list); list=next; }
}
static int socket_getaddrinfo(const char *host,const char *service,const struct DarwinAddrInfo *hints,struct DarwinAddrInfo **output) {
    if (!output) return 12; *output=NULL;
    struct addrinfo native={0},*result=NULL;
    unsigned flags=hints ? (unsigned)hints->flags : 0;
    if (flags&~0x10001f07U) return 3;
    if (!hints) flags=0x600; /* Only absent hints imply Darwin AI_DEFAULT. */
    if (flags&1) native.ai_flags|=AI_PASSIVE; if (flags&2) native.ai_flags|=AI_CANONNAME; if (flags&4) native.ai_flags|=AI_NUMERICHOST;
    if (flags&0x1000) native.ai_flags|=AI_NUMERICSERV; if (flags&0x100) native.ai_flags|=AI_ALL;
    if (flags&0xa00) native.ai_flags|=AI_V4MAPPED; if ((flags&0x400) && !(flags&0x10000000)) native.ai_flags|=AI_ADDRCONFIG;
    if (hints) { native.ai_family=socket_family(hints->family); native.ai_socktype=hints->type; native.ai_protocol=hints->protocol; }
    if (native.ai_family!=AF_INET6) native.ai_flags&=~(AI_V4MAPPED|AI_ALL);
    int error=getaddrinfo(host,service,&native,&result);
    if (error) { int translated=socket_eai(error); LOG("Darwin DNS resolution: %s result=%d flags=0x%x native=0x%x family=%d",host ? host : "(null)",translated,flags,native.ai_flags,native.ai_family); return translated; }
    struct DarwinAddrInfo **tail=output; unsigned count=0;
    for (struct addrinfo *entry=result;entry;entry=entry->ai_next) {
        if (entry->ai_family!=AF_INET && entry->ai_family!=AF_INET6) continue;
        struct DarwinAddrInfo *copy=calloc(1,sizeof(*copy));
        if (!copy) { error=6; break; }
        copy->flags=hints ? hints->flags : 0; copy->family=entry->ai_family==AF_INET6 ? 30 : entry->ai_family;
        copy->type=entry->ai_socktype; copy->protocol=entry->ai_protocol; copy->length=entry->ai_addrlen;
        copy->canonical=entry->ai_canonname ? strdup(entry->ai_canonname) : NULL;
        copy->address=darwin_interface_address(entry->ai_addr,NULL);
        if (!copy->address || (entry->ai_canonname && !copy->canonical)) { free(copy->canonical); free(copy->address); free(copy); error=6; break; }
        *tail=copy; tail=&copy->next; ++count;
    }
    freeaddrinfo(result);
    if (error || !*output) { socket_free_addrinfo(*output); *output=NULL; return error ? error : 8; }
    LOG_LIMITED(24,"Darwin DNS resolved: %s addresses=%u",host ? host : "(null)",count);
    return 0;
}
static const char *socket_gai_error(int code) {
    static const char *messages[]={"Success","Address family unsupported","Temporary DNS failure","Invalid resolver flags","DNS resolution failed",
        "Address family unsupported","Out of memory","No address available","Host or service unknown","Unsupported service","Unsupported socket type",
        "System resolver error","Invalid resolver hints","Unknown protocol","Resolver output buffer too small"};
    return code>=0 && (unsigned)code<COUNT(messages) ? messages[code] : "Unknown resolver error";
}
struct DarwinSocketState { int fd,nosigpipe; struct DarwinSocketState *next; };
static struct DarwinSocketState *darwin_socket_states;
static pthread_mutex_t darwin_socket_mutex=PTHREAD_MUTEX_INITIALIZER;
static int socket_signal_option(int fd,int value) {
    pthread_mutex_lock(&darwin_socket_mutex);
    struct DarwinSocketState *state=darwin_socket_states;
    while (state && state->fd!=fd) state=state->next;
    if (!state && value>=0) { state=calloc(1,sizeof(*state)); if (state) { state->fd=fd; state->next=darwin_socket_states; darwin_socket_states=state; } }
    if (state && value>=0) state->nosigpipe=value!=0;
    int result=state ? state->nosigpipe : 0; pthread_mutex_unlock(&darwin_socket_mutex); return result;
}
static int socket_close(int fd) {
    pthread_mutex_lock(&darwin_socket_mutex); struct DarwinSocketState **slot=&darwin_socket_states;
    while (*slot && (*slot)->fd!=fd) slot=&(*slot)->next;
    if (*slot) { struct DarwinSocketState *state=*slot; *slot=state->next; free(state); }
    pthread_mutex_unlock(&darwin_socket_mutex); return io_close(fd);
}
static int socket_create(int family,int type,int protocol) { return (int)socket_result(socket(socket_family(family),type,protocol)); }
static int socket_connect(int fd,const void *address,socklen_t length) {
    struct sockaddr_storage native; if (socket_native_address(address,length,&native)) return (int)socket_result(-1);
    return (int)socket_result(connect(fd,(void *)&native,length));
}
static int socket_bind(int fd,const void *address,socklen_t length) {
    struct sockaddr_storage native; if (socket_native_address(address,length,&native)) return (int)socket_result(-1);
    return (int)socket_result(bind(fd,(void *)&native,length));
}
static int socket_accept(int fd,void *address,socklen_t *length) {
    struct sockaddr_storage native; socklen_t size=sizeof(native);
    if (address && !length) { errno=EFAULT; return -1; }
    int accepted=accept(fd,address ? (void *)&native : NULL,address ? &size : NULL);
    if (accepted>=0 && address) socket_guest_address((void *)&native,size,address,length);
    return (int)socket_result(accepted);
}
static int socket_listen(int fd,int count) { return (int)socket_result(listen(fd,count)); }
static int socket_option(int level,int option,int *native_level) {
    *native_level=level==0xffff ? SOL_SOCKET : level;
    if (level==0xffff) {
        switch (option) {
        case 1:return SO_DEBUG; case 2:return SO_ACCEPTCONN; case 4:return SO_REUSEADDR; case 8:return SO_KEEPALIVE;
        case 0x10:return SO_DONTROUTE; case 0x20:return SO_BROADCAST; case 0x100:return SO_OOBINLINE; case 0x200:return SO_REUSEPORT;
        case 0x400:return SO_TIMESTAMP; case 0x1001:return SO_SNDBUF; case 0x1002:return SO_RCVBUF; case 0x1003:return SO_SNDLOWAT;
        case 0x1004:return SO_RCVLOWAT; case 0x1005:return SO_SNDTIMEO; case 0x1006:return SO_RCVTIMEO; case 0x1007:return SO_ERROR;
        case 0x1008:return SO_TYPE; case 0x1080:return SO_LINGER; default:return -1;
        }
    }
    if (level==IPPROTO_TCP) { if (option==1) return TCP_NODELAY; if (option==2) return TCP_MAXSEG; if (option==0x10) return TCP_KEEPIDLE; return -1; }
    if (level==IPPROTO_IP) {
        switch (option) { case 2:return IP_HDRINCL; case 3:return IP_TOS; case 4:return IP_TTL; case 9:return IP_MULTICAST_IF;
        case 10:return IP_MULTICAST_TTL; case 11:return IP_MULTICAST_LOOP; case 12:return IP_ADD_MEMBERSHIP; case 13:return IP_DROP_MEMBERSHIP; default:return -1; }
    }
    if (level==IPPROTO_IPV6 && option==27) return IPV6_V6ONLY;
    return -1;
}
static int socket_set_option(int fd,int level,int option,const void *value,socklen_t length) {
    if (level==0xffff && option==0x1022) {
        int type; socklen_t size=sizeof(type);
        if (getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)) return (int)socket_result(-1);
        if (!value || length<sizeof(int)) { errno=EINVAL; return -1; }
        socket_signal_option(fd,*(const int *)value); return 0;
    }
    int native_level,native=socket_option(level,option,&native_level);
    if (native<0) { errno=42; LOG("Unsupported Darwin socket option: level=%d option=0x%x",level,option); return -1; }
    struct timeval timeout;
    if (level==0xffff && (option==0x1005 || option==0x1006)) {
        if (!value || length<sizeof(struct DarwinSocketTimeval)) { errno=EINVAL; return -1; }
        const struct DarwinSocketTimeval *input=value; timeout=(struct timeval){input->seconds,input->microseconds}; value=&timeout; length=sizeof(timeout);
    }
    return (int)socket_result(setsockopt(fd,native_level,native,value,length));
}
static int socket_get_option(int fd,int level,int option,void *value,socklen_t *length) {
    if (!length) { errno=EFAULT; return -1; }
    if (level==0xffff && option==0x1022) {
        if (!value || *length<sizeof(int)) { errno=EINVAL; return -1; }
        *(int *)value=socket_signal_option(fd,-1); *length=sizeof(int); return 0;
    }
    int native_level,native=socket_option(level,option,&native_level);
    if (native<0) { errno=42; return -1; }
    if (level==0xffff && (option==0x1005 || option==0x1006)) {
        if (!value || *length<sizeof(struct DarwinSocketTimeval)) { errno=EINVAL; return -1; }
        struct timeval timeout; socklen_t size=sizeof(timeout); int result=getsockopt(fd,native_level,native,&timeout,&size);
        if (!result) { *(struct DarwinSocketTimeval *)value=(struct DarwinSocketTimeval){timeout.tv_sec,(int32_t)timeout.tv_usec,0}; *length=sizeof(struct DarwinSocketTimeval); }
        return (int)socket_result(result);
    }
    int result=getsockopt(fd,native_level,native,value,length);
    if (!result && level==0xffff && option==0x1007 && value && *length>=sizeof(int)) *(int *)value=socket_errno(*(int *)value);
    return (int)socket_result(result);
}
static int socket_flags(int flags) {
    if (flags&~0xff) { errno=EINVAL; return -1; }
    int native=flags&7; if (flags&8) native|=MSG_EOR; if (flags&0x10) native|=MSG_TRUNC; if (flags&0x20) native|=MSG_CTRUNC;
    if (flags&0x40) native|=MSG_WAITALL; if (flags&0x80) native|=MSG_DONTWAIT; return native;
}
static int socket_guest_flags(int flags) {
    return (flags&7)|((flags&MSG_EOR) ? 8 : 0)|((flags&MSG_TRUNC) ? 0x10 : 0)|((flags&MSG_CTRUNC) ? 0x20 : 0)|
        ((flags&MSG_WAITALL) ? 0x40 : 0)|((flags&MSG_DONTWAIT) ? 0x80 : 0);
}
static ssize_t socket_send(int fd,const void *data,size_t size,int flags) {
    int native=socket_flags(flags); if (native<0) return -1; if (socket_signal_option(fd,-1)) native|=MSG_NOSIGNAL;
    return socket_result(send(fd,data,size,native));
}
static ssize_t socket_receive(int fd,void *data,size_t size,int flags) { int native=socket_flags(flags); return native<0 ? -1 : socket_result(recv(fd,data,size,native)); }
static ssize_t socket_write(int fd,const void *data,size_t size) { return socket_signal_option(fd,-1) ? socket_result(send(fd,data,size,MSG_NOSIGNAL)) : write(fd,data,size); }
static ssize_t socket_send_message(int fd,const struct DarwinMessage *message,int flags) {
    if (!message || message->iov_count<0 || (message->iov_count && !message->iov)) { errno=EINVAL; return -1; }
    int native_flags=socket_flags(flags); if (native_flags<0) return -1; if (socket_signal_option(fd,-1)) native_flags|=MSG_NOSIGNAL;
    struct msghdr native={.msg_iov=message->iov,.msg_iovlen=(size_t)message->iov_count}; struct sockaddr_storage address;
    if (message->name) { if (socket_native_address(message->name,message->name_length,&address)) return socket_result(-1); native.msg_name=&address; native.msg_namelen=message->name_length; }
    if (message->control_length>1024*1024 || (message->control_length && !message->control)) { errno=EINVAL; return -1; }
    unsigned char *control=message->control_length ? calloc(1,(size_t)message->control_length*2+32) : NULL; size_t used=0;
    if (message->control_length && !control) { errno=ENOMEM; return -1; }
    for (size_t offset=0;offset+sizeof(struct DarwinControl)<=message->control_length;) {
        const struct DarwinControl *header=(const void *)((const unsigned char *)message->control+offset);
        if (header->length<12 || header->length>message->control_length-offset || header->level!=0xffff || header->type!=1) { free(control); errno=EINVAL; return -1; }
        size_t bytes=header->length-12; struct cmsghdr *dest=(void *)(control+used);
        dest->cmsg_len=CMSG_LEN(bytes); dest->cmsg_level=SOL_SOCKET; dest->cmsg_type=SCM_RIGHTS;
        memcpy(CMSG_DATA(dest),(const unsigned char *)header+12,bytes); used+=CMSG_SPACE(bytes); offset+=(header->length+3U)&~3U;
    }
    native.msg_control=control; native.msg_controllen=used;
    ssize_t result=sendmsg(fd,&native,native_flags); int error=errno; free(control); errno=error; return socket_result(result);
}
static ssize_t socket_receive_message(int fd,struct DarwinMessage *message,int flags) {
    if (!message || message->iov_count<0 || (message->iov_count && !message->iov) || message->control_length>1024*1024 || (message->control_length && !message->control)) { errno=EINVAL; return -1; }
    int native_flags=socket_flags(flags); if (native_flags<0) return -1;
    struct sockaddr_storage address; unsigned char *control=message->control_length ? malloc((size_t)message->control_length*2+32) : NULL;
    if (message->control_length && !control) { errno=ENOMEM; return -1; }
    struct msghdr native={.msg_name=message->name ? &address : NULL,.msg_namelen=message->name ? sizeof(address) : 0,
        .msg_iov=message->iov,.msg_iovlen=(size_t)message->iov_count,.msg_control=control,.msg_controllen=control ? (size_t)message->control_length*2+32 : 0};
    ssize_t result=recvmsg(fd,&native,native_flags); int error=errno;
    if (result>=0) {
        if (message->name) socket_guest_address((void *)&address,native.msg_namelen,message->name,&message->name_length);
        message->flags=socket_guest_flags(native.msg_flags); size_t used=0;
        for (struct cmsghdr *header=CMSG_FIRSTHDR(&native);header;header=CMSG_NXTHDR(&native,header)) {
            if (header->cmsg_len<CMSG_LEN(0)) { message->flags|=0x20; break; }
            unsigned char payload[sizeof(struct DarwinSocketTimeval)]; const void *data=CMSG_DATA(header); size_t bytes=header->cmsg_len-CMSG_LEN(0); int type;
            if (header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS) type=1;
            else if (header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_TIMESTAMP && bytes>=sizeof(struct timeval)) {
                const struct timeval *time=data; struct DarwinSocketTimeval converted={time->tv_sec,(int32_t)time->tv_usec,0};
                memcpy(payload,&converted,sizeof(converted)); data=payload; bytes=sizeof(converted); type=2;
            } else { message->flags|=0x20; continue; }
            size_t space=12+((bytes+3)&~(size_t)3);
            if (space>message->control_length-used) { message->flags|=0x20; break; }
            struct DarwinControl *dest=(void *)((unsigned char *)message->control+used); *dest=(struct DarwinControl){(uint32_t)(12+bytes),0xffff,type};
            memcpy((unsigned char *)dest+12,data,bytes); if (space>12+bytes) memset((unsigned char *)dest+12+bytes,0,space-12-bytes); used+=space;
        }
        message->control_length=(uint32_t)used;
    }
    free(control); errno=error; return socket_result(result);
}
static int socket_poll(struct pollfd *descriptors,nfds_t count,int timeout) {
    if (count>INT_MAX || (count && !descriptors)) { errno=EINVAL; return -1; }
    struct pollfd *native=calloc(count ? count : 1,sizeof(*native)); if (!native) { errno=ENOMEM; return -1; }
    for (nfds_t i=0;i<count;++i) { native[i]=descriptors[i]; native[i].events=(short)((descriptors[i].events&0xff)|((descriptors[i].events&0x100) ? POLLWRBAND : 0)); }
    int result=poll(native,count,timeout),error=errno;
    if (result>=0) for (nfds_t i=0;i<count;++i) descriptors[i].revents=(short)((native[i].revents&0xff)|((native[i].revents&POLLWRBAND) ? 0x100 : 0));
    free(native); errno=error; return (int)socket_result(result);
}
static int socket_select(int count,fd_set *readable,fd_set *writable,fd_set *excepted,struct DarwinSocketTimeval *timeout) {
    struct timeval native,*value=NULL;
    if (timeout) { native=(struct timeval){timeout->seconds,timeout->microseconds}; value=&native; }
    int result=select(count,readable,writable,excepted,value);
    if (timeout && result>=0) *timeout=(struct DarwinSocketTimeval){native.tv_sec,(int32_t)native.tv_usec,0};
    return (int)socket_result(result);
}
static Word resolve_socket_import(const char *name) {
    if (!strcmp(name,"_getaddrinfo")) return (Word)socket_getaddrinfo; if (!strcmp(name,"_freeaddrinfo")) return (Word)socket_free_addrinfo;
    if (!strcmp(name,"_gai_strerror")) return (Word)socket_gai_error; if (!strcmp(name,"_socket")) return (Word)socket_create;
    if (!strcmp(name,"_connect")) return (Word)socket_connect; if (!strcmp(name,"_bind")) return (Word)socket_bind;
    if (!strcmp(name,"_accept")) return (Word)socket_accept; if (!strcmp(name,"_listen")) return (Word)socket_listen;
    if (!strcmp(name,"_setsockopt")) return (Word)socket_set_option; if (!strcmp(name,"_getsockopt")) return (Word)socket_get_option;
    if (!strcmp(name,"_send")) return (Word)socket_send; if (!strcmp(name,"_recv")) return (Word)socket_receive;
    if (!strcmp(name,"_strerror_r")) return (Word)socket_strerror_r;
    if (!strcmp(name,"_sendmsg")) return (Word)socket_send_message; if (!strcmp(name,"_recvmsg")) return (Word)socket_receive_message;
    if (!strcmp(name,"_poll")) return (Word)socket_poll; if (!strcmp(name,"_select")) return (Word)socket_select;
    if (!strcmp(name,"_close")) return (Word)socket_close; if (!strcmp(name,"_write")) return (Word)socket_write;
    return 0;
}
