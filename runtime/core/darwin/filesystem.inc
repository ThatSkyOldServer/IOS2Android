/* APK file descriptors need entry-relative positions and EOF boundaries. */
struct BundleFile { off_t start,length,position; ino_t inode; unsigned references,region; pthread_mutex_t mutex; };
struct BundleDescriptor { int fd; struct BundleFile *file; struct BundleDescriptor *next; };
static struct BundleDescriptor *bundle_descriptors[64];
static pthread_mutex_t bundle_descriptors_mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned bundle_descriptor_count;
static char *bundle_normal_path(const char *relative){
    size_t length=strlen(relative);char *result=malloc(length+1);if(!result){errno=ENOMEM;return NULL;}size_t used=0;const char *cursor=relative;
    while(*cursor){while(*cursor=='/')++cursor;const char *first=cursor;while(*cursor && *cursor!='/')++cursor;size_t count=(size_t)(cursor-first);if(!count || (count==1 && *first=='.'))continue;
        if(count==2 && first[0]=='.' && first[1]=='.'){if(!used){free(result);errno=EACCES;return NULL;}while(used && result[used-1]!='/')--used;if(used)--used;continue;}
        if(used)result[used++]='/';memcpy(result+used,first,count);used+=count;
    }
    result[used]=0;return result;
}
static ino_t bundle_inode(const char *name){uint64_t value=UINT64_C(14695981039346656037);for(const unsigned char *p=(const void *)name;*p;++p)value=(value^*p)*UINT64_C(1099511628211);return (ino_t)(value ? value : 1);}
static void bundle_stat_info(struct stat *info,off_t length,ino_t inode,mode_t kind){*info=bundle_archive_info;info->st_size=length;info->st_ino=inode;info->st_mode=kind|(kind==S_IFDIR ? 0555 : 0444);info->st_uid=getuid();info->st_gid=getgid();info->st_nlink=1;info->st_blocks=(length+511)/512;}
static void bundle_file_release(struct BundleFile *file){if(__atomic_sub_fetch(&file->references,1,__ATOMIC_ACQ_REL)==0){pthread_mutex_destroy(&file->mutex);free(file);}}
static struct BundleFile *bundle_fd_acquire(int fd){
    if(fd<0 || !__atomic_load_n(&bundle_descriptor_count,__ATOMIC_ACQUIRE))return NULL;
    pthread_mutex_lock(&bundle_descriptors_mutex);struct BundleDescriptor *entry=bundle_descriptors[(unsigned)fd%COUNT(bundle_descriptors)];
    while(entry && entry->fd!=fd)entry=entry->next;struct BundleFile *file=entry ? entry->file : NULL;if(file)__atomic_add_fetch(&file->references,1,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&bundle_descriptors_mutex);return file;
}
static int bundle_fd_register(int fd,struct BundleFile *file){
    struct BundleDescriptor *entry=malloc(sizeof(*entry));if(!entry){errno=ENOMEM;return -1;}
    unsigned bucket=(unsigned)fd%COUNT(bundle_descriptors);pthread_mutex_lock(&bundle_descriptors_mutex);
    *entry=(struct BundleDescriptor){fd,file,bundle_descriptors[bucket]};bundle_descriptors[bucket]=entry;__atomic_add_fetch(&bundle_descriptor_count,1,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&bundle_descriptors_mutex);return fd;
}
static void bundle_fd_remove(int fd){
    if(fd<0 || !__atomic_load_n(&bundle_descriptor_count,__ATOMIC_ACQUIRE))return;
    unsigned bucket=(unsigned)fd%COUNT(bundle_descriptors);pthread_mutex_lock(&bundle_descriptors_mutex);struct BundleDescriptor **slot=&bundle_descriptors[bucket];
    while(*slot && (*slot)->fd!=fd)slot=&(*slot)->next;struct BundleDescriptor *entry=*slot;
    if(entry){*slot=entry->next;__atomic_sub_fetch(&bundle_descriptor_count,1,__ATOMIC_RELEASE);}pthread_mutex_unlock(&bundle_descriptors_mutex);
    if(entry){bundle_file_release(entry->file);free(entry);}
}
static int io_close(int fd){bundle_fd_remove(fd);return close(fd);}
static int bundle_fd_duplicate(int from,int to){
    if(to<0)return to;struct BundleFile *file=bundle_fd_acquire(from);if(!file)return to;
    if(bundle_fd_register(to,file)<0){int code=errno;close(to);bundle_file_release(file);errno=code;return -1;}return to;
}
static int io_dup(int fd){return bundle_fd_duplicate(fd,dup(fd));}
static int io_dup2(int from,int to){
    if(from==to)return dup2(from,to);struct BundleFile *file=bundle_fd_acquire(from);int result=dup2(from,to);
    if(result>=0){bundle_fd_remove(to);if(file && bundle_fd_register(to,file)<0){int code=errno;close(to);bundle_file_release(file);errno=code;return -1;}if(file)return result;}
    if(file)bundle_file_release(file);return result;
}
static int bundle_open(const char *relative){
    if(!bundle_assets || !relative || !*relative){errno=ENOENT;return -1;}
    char *normal=bundle_normal_path(relative);if(!normal)return -1;ino_t inode=bundle_inode(normal);
    char *name=NULL;if(asprintf(&name,"%s/%s",GUEST_BUNDLE_NAME,normal)<0){free(normal);errno=ENOMEM;return -1;}free(normal);
    AAsset *asset=AAssetManager_open(bundle_assets,name,AASSET_MODE_RANDOM);free(name);if(!asset){errno=ENOENT;return -1;}
    off64_t start=0,length=0;int fd=AAsset_openFileDescriptor64(asset,&start,&length);unsigned region=fd>=0;
    if(fd<0){
        length=AAsset_getLength64(asset);start=0;fd=(int)syscall(__NR_memfd_create,"bundle-resource",MFD_CLOEXEC);
        if(fd>=0 && length>0){size_t capacity=(uint64_t)length<65536 ? (size_t)length : 65536;unsigned char *buffer=malloc(capacity);if(!buffer){close(fd);fd=-1;errno=ENOMEM;}else{off64_t done=0;while(done<length){int count=AAsset_read(asset,buffer,capacity);if(count<=0){close(fd);fd=-1;errno=EIO;break;}int offset=0;while(offset<count){ssize_t written=write(fd,buffer+offset,(size_t)(count-offset));if(written<0 && errno==EINTR)continue;if(written<=0){int code=errno;close(fd);fd=-1;errno=code;break;}offset+=(int)written;}if(fd<0)break;done+=count;}free(buffer);}}
    }
    int code=errno;AAsset_close(asset);if(fd<0){errno=code;return -1;}
    if(!region){char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);int readonly=open(path,O_RDONLY|O_CLOEXEC);code=errno;close(fd);fd=readonly;if(fd<0){errno=code;return -1;}}
    if(start<0 || length<0 || start>INT64_MAX-length){close(fd);errno=EOVERFLOW;return -1;}
    fcntl(fd,F_SETFD,FD_CLOEXEC);struct BundleFile *file=calloc(1,sizeof(*file));if(!file){close(fd);errno=ENOMEM;return -1;}
    file->start=start;file->length=length;file->inode=inode;file->references=1;file->region=region;pthread_mutex_init(&file->mutex,NULL);
    if(bundle_fd_register(fd,file)<0){code=errno;close(fd);bundle_file_release(file);errno=code;return -1;}return fd;
}
static int io_open(const char *path,int flags,mode_t mode){
    if(!path){errno=EFAULT;return -1;}const char *relative=bundle_relative(path);if(!relative)return open(path,flags,mode);
    if((flags&O_ACCMODE)!=O_RDONLY || (flags&(O_CREAT|O_TRUNC|O_APPEND))){errno=EACCES;return -1;}
    int fd=bundle_open(relative);if(fd<0 && (errno==ENOSYS || errno==EPERM || errno==ENOTSUP)){char *native=native_path(path);if(native){fd=open(native,flags,mode);free(native);}}return fd;
}
static ssize_t bundle_pread(int fd,struct BundleFile *file,void *bytes,size_t count,off_t offset){
    if(offset<0 || count>SSIZE_MAX){errno=EINVAL;return -1;}if(offset>=file->length)return 0;
    if(count>(uint64_t)(file->length-offset))count=(size_t)(file->length-offset);return pread(fd,bytes,count,file->start+offset);
}
static ssize_t io_pread(int fd,void *bytes,size_t count,off_t offset){struct BundleFile *file=bundle_fd_acquire(fd);if(!file)return pread(fd,bytes,count,offset);ssize_t result=bundle_pread(fd,file,bytes,count,offset);bundle_file_release(file);return result;}
static ssize_t io_read(int fd,void *bytes,size_t count){
    struct BundleFile *file=bundle_fd_acquire(fd);if(!file)return read(fd,bytes,count);pthread_mutex_lock(&file->mutex);
    ssize_t result=bundle_pread(fd,file,bytes,count,file->position);if(result>0)file->position+=result;pthread_mutex_unlock(&file->mutex);bundle_file_release(file);return result;
}
static ssize_t io_readv(int fd,const struct iovec *vectors,int count){
    struct BundleFile *file=bundle_fd_acquire(fd);if(!file)return readv(fd,vectors,count);if(count<0 || count>IOV_MAX){bundle_file_release(file);errno=EINVAL;return -1;}
    struct iovec *selected=count ? malloc((size_t)count*sizeof(*selected)) : NULL;if(count && !selected){bundle_file_release(file);errno=ENOMEM;return -1;}pthread_mutex_lock(&file->mutex);off_t left=file->position<file->length ? file->length-file->position : 0;int used=0;size_t total=0;
    for(int i=0;i<count;++i){if(vectors[i].iov_len>SSIZE_MAX-total){pthread_mutex_unlock(&file->mutex);free(selected);bundle_file_release(file);errno=EINVAL;return -1;}total+=vectors[i].iov_len;if(left<=0)continue;selected[used]=vectors[i];if(selected[used].iov_len>(uint64_t)left)selected[used].iov_len=(size_t)left;left-=selected[used++].iov_len;}
    ssize_t result=used ? preadv(fd,selected,used,file->start+file->position) : 0;if(result>0)file->position+=result;pthread_mutex_unlock(&file->mutex);free(selected);bundle_file_release(file);return result;
}
static off_t io_seek(int fd,off_t offset,int origin){
    struct BundleFile *file=bundle_fd_acquire(fd);if(!file)return lseek(fd,offset,origin);pthread_mutex_lock(&file->mutex);off_t base=origin==SEEK_SET ? 0 : origin==SEEK_CUR ? file->position : file->length,result;
    if((origin!=SEEK_SET && origin!=SEEK_CUR && origin!=SEEK_END) || __builtin_add_overflow(base,offset,&result) || result<0){errno=EINVAL;result=-1;}else file->position=result;
    pthread_mutex_unlock(&file->mutex);bundle_file_release(file);return result;
}
static int io_fstat(int fd,struct stat *info){
    int result=fstat(fd,info);if(result)return result;struct BundleFile *file=bundle_fd_acquire(fd);if(file){bundle_stat_info(info,file->length,file->inode,S_IFREG);bundle_file_release(file);}return 0;
}
static int io_stat(const char *path,struct stat *info){
    const char *relative=bundle_relative(path);if(!relative)return stat(path,info);char *normal=bundle_normal_path(relative);if(!normal)return -1;ino_t inode=bundle_inode(normal);char *name=NULL;if(asprintf(&name,"%s%s%s",GUEST_BUNDLE_NAME,*normal ? "/" : "",normal)<0){free(normal);errno=ENOMEM;return -1;}
    AAsset *asset=AAssetManager_open(bundle_assets,name,AASSET_MODE_RANDOM);free(name);
    if(asset){bundle_stat_info(info,AAsset_getLength64(asset),inode,S_IFREG);AAsset_close(asset);free(normal);return 0;}
    JNIEnv *env=attached_environment("Bundle metadata");static jmethodID method;if(!method){jclass type=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,type,"bundleEntryType","(Ljava/lang/String;)I");(*env)->DeleteLocalRef(env,type);}jstring key=(*env)->NewStringUTF(env,normal);free(normal);int kind=(*env)->CallIntMethod(env,activity,method,key);(*env)->DeleteLocalRef(env,key);
    if(kind==2){bundle_stat_info(info,0,inode,S_IFDIR);return 0;}errno=ENOENT;return -1;
}
static void *io_mmap(void *address,size_t length,int protection,int flags,int fd,off_t offset){
    struct BundleFile *file=(flags&MAP_ANONYMOUS) ? NULL : bundle_fd_acquire(fd);if(!file)return mmap(address,length,protection,flags,fd,offset);
    if((flags&MAP_SHARED) && (protection&PROT_WRITE)){bundle_file_release(file);errno=EACCES;return MAP_FAILED;}
    if(!file->region){bundle_file_release(file);return mmap(address,length,protection,flags,fd,offset);}
    if(offset<0){bundle_file_release(file);errno=EINVAL;return MAP_FAILED;}
    long page=sysconf(_SC_PAGESIZE);uint64_t remaining=offset<file->length ? (uint64_t)(file->length-offset) : 0;
    uint64_t rounded=(remaining+(uint64_t)page-1)/(uint64_t)page*(uint64_t)page;
    if(file->start%page || offset>=file->length || length>rounded){
        int copy=(int)syscall(__NR_memfd_create,"bundle-mapping",MFD_CLOEXEC);if(copy<0){bundle_file_release(file);return MAP_FAILED;}
        size_t capacity=file->length>0 && file->length<65536 ? (size_t)file->length : 65536;unsigned char *bytes=malloc(capacity);off_t done=0;int code=bytes ? 0 : ENOMEM;
        while(!code && done<file->length){size_t wanted=(uint64_t)(file->length-done)<capacity ? (size_t)(file->length-done) : capacity;ssize_t count=pread(fd,bytes,wanted,file->start+done);if(count<0 && errno==EINTR)continue;if(count<=0){code=count<0 ? errno : EIO;break;}size_t written=0;while(written<(size_t)count){ssize_t n=write(copy,bytes+written,(size_t)count-written);if(n<0 && errno==EINTR)continue;if(n<=0){code=n<0 ? errno : EIO;break;}written+=(size_t)n;}if(code)break;done+=count;}
        free(bytes);if(!code)fchmod(copy,0444);void *result=code ? MAP_FAILED : mmap(address,length,protection,flags,copy,offset);if(code)errno=code;int saved=errno;close(copy);bundle_file_release(file);errno=saved;return result;
    }
    void *result=mmap(address,length,protection,flags,fd,file->start+offset);
    uint64_t tail=remaining%(uint64_t)page,tail_offset=remaining-tail;
    if(result!=MAP_FAILED && tail && length>tail_offset){
        void *last=mmap((char *)result+tail_offset,(size_t)page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);int code=0;size_t done=0;
        if(last==MAP_FAILED)code=errno;
        while(!code && done<tail){ssize_t count=pread(fd,(char *)last+done,(size_t)tail-done,file->start+offset+(off_t)tail_offset+(off_t)done);if(count<0 && errno==EINTR)continue;if(count<=0)code=count<0 ? errno : EIO;else done+=(size_t)count;}
        if(!code && mprotect(last,(size_t)page,protection))code=errno;
        if(code){munmap(result,length);result=MAP_FAILED;errno=code;}
    }
    bundle_file_release(file);return result;
}
static const char *directory_suffix(Word directory) {
    switch (directory) {
        case 5: return "Library";
        case 9: return "Documents";
        case 13: return "Caches";
        case 14: return "";
        case 21: return "Library/Application Support";
        default: return NULL;
    }
}
static Word manager_create_path(Word,const char *,Word,Word,Word,Word *);
static Word directory_path(Word directory) {
    const char *suffix=directory_suffix(directory); if (!suffix) stop("Unsupported sandbox directory","NSSearchPathForDirectoriesInDomains");
    if (!*suffix) return copy_string(files_directory);
    Word path=append_path(copy_string(files_directory),"sandbox directory",copy_string(suffix));
    const char *bytes=string_bytes(path,"sandbox directory");
    if (strchr(suffix,'/')) { if (!manager_create_path((Word)&file_manager,"Sandbox directory",path,1,0,NULL)) stop("Sandbox directory creation failed",bytes); }
    else if (mkdir(bytes,0700) && errno!=EEXIST) stop("Sandbox directory creation failed",bytes);
    return path;
}

static Word search_directories(Word directory,Word domains,unsigned char expand) {
    if (domains!=1) stop("Unsupported search domain","NSSearchPathForDirectoriesInDomains");
    Word path=directory_path(directory);
    return new_array(external_class("_OBJC_CLASS_$_NSArray"),"search path",&path,1);
}

static Word manager_directory_urls(Word self,const char *selector,Word directory,Word domains) {
    if (domains!=1) stop("Unsupported search domain",selector);
    Word url=file_url(directory_path(directory));
    return new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,&url,1);
}

static Word home_directory(void) { return copy_string(files_directory); }
static Word temporary_directory(void) {
    Word root=directory_path(13);
    Word path=append_path(root,"temporary directory",copy_string("tmp"));
    const char *bytes=string_bytes(path,"temporary directory");
    if (mkdir(bytes,0700) && errno!=EEXIST) stop("Temporary directory creation failed",bytes);
    return append_text(path,"temporary directory",copy_string("/"));
}

static Word error_with_domain(Word cls,const char *selector,Word domain,long code,Word info) {
    struct HostObject *error=calloc(1,sizeof(*error));
    if (!error) stop("Allocation failed",selector);
    error->isa=cls; error->content[0]=domain; error->content[1]=(Word)code;
    error->content[2]=info ? map_copy((void *)info,"copy") : empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    return (Word)error;
}
static Word error_domain(struct HostObject *error) { return error->content[0]; }
static Word error_code(struct HostObject *error) { return error->content[1]; }
static Word error_info(struct HostObject *error) { return error->content[2]; }
static Word error_description(struct HostObject *error) {
    struct GuestMap *info=(void *)error->content[2];
    Word value=info ? map_get(info,"localizedDescription",copy_string("NSLocalizedDescription")) : 0;
    return value ? value : copy_string(strerror((int)error->content[1]));
}

static Word file_failure(Word *error,const char *path,int code) {
    if (error) {
        struct GuestMap *info=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
        map_set(info,"file error",copy_string(path),copy_string("NSFilePath"));
        map_set(info,"file error",copy_string(strerror(code)),copy_string("NSLocalizedDescription"));
        *error=error_with_domain(external_class("_OBJC_CLASS_$_NSError"),"file error",copy_string("NSPOSIXErrorDomain"),code,(Word)info);
    }
    return 0;
}

static int writable_sandbox_path(const char *path) {
    size_t root=strlen(files_directory);
    if (strncmp(path,files_directory,root) || path[root]!='/' || strstr(path,"/../") || !strcmp(strrchr(path,'/'),"/..")) {
        errno=EACCES;
        return 0;
    }
    return 1;
}

static Word manager_exists(Word self,const char *selector,Word filename,unsigned char *is_directory) {
    if (!filename) { if (is_directory) *is_directory=0; return 0; }
    const char *logical=string_bytes(filename,selector);
    const char *relative=bundle_relative(logical);
    if (relative) {
        JNIEnv *env=attached_environment(selector);
        jclass cls=(*env)->GetObjectClass(env,activity);
        jmethodID query=(*env)->GetMethodID(env,cls,"bundleEntryType","(Ljava/lang/String;)I");
        jstring key=(*env)->NewStringUTF(env,relative);
        jint type=(*env)->CallIntMethod(env,activity,query,key);
        (*env)->DeleteLocalRef(env,key); (*env)->DeleteLocalRef(env,cls);
        if (is_directory) *is_directory=type==2;
        return type!=0;
    }
    struct stat info;
    int exists=io_stat(string_bytes(filename,selector),&info)==0;
    if (is_directory) *is_directory=exists && S_ISDIR(info.st_mode);
    return exists;
}

static Word manager_exists_simple(Word self,const char *selector,Word path) { return manager_exists(self,selector,path,NULL); }

static Word manager_file_attributes(Word self,const char *selector,Word filename,Word *error) {
    const char *name=string_bytes(filename,selector);
    struct stat info;
    if (io_stat(name,&info)) return file_failure(error,name,errno);
    struct GuestMap *attributes=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(attributes,selector,number_integer(info.st_size),copy_string("NSFileSize"));
    map_set(attributes,selector,number_integer(info.st_mode&07777),copy_string("NSFilePosixPermissions"));
    Word modified=date_epoch(external_class("_OBJC_CLASS_$_NSDate"),selector,info.st_mtim.tv_sec+info.st_mtim.tv_nsec*1e-9);
    map_set(attributes,selector,modified,copy_string("NSFileModificationDate"));
     
    map_set(attributes,selector,copy_string(S_ISDIR(info.st_mode) ? "NSFileTypeDirectory" : "NSFileTypeRegular"),copy_string("NSFileType"));
    return (Word)attributes;
}

static Word manager_filesystem_attributes(Word self,const char *selector,Word filename,Word *error) {
    const char *path=string_bytes(filename,selector);
    struct statvfs info;
    if (statvfs(path,&info)) return file_failure(error,path,errno);
    struct GuestMap *attributes=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(attributes,selector,number_integer((Word)info.f_blocks*info.f_frsize),copy_string("NSFileSystemSize"));
    map_set(attributes,selector,number_integer((Word)info.f_bavail*info.f_frsize),copy_string("NSFileSystemFreeSize"));
    return (Word)attributes;
}

static Word manager_list(Word self,const char *selector,Word filename,Word *error) {
    const char *path=string_bytes(filename,selector);
    const char *relative=bundle_relative(path);
    if (relative) {
        JNIEnv *env=attached_environment(selector);
        jclass cls=(*env)->GetObjectClass(env,activity);
        jmethodID list=(*env)->GetMethodID(env,cls,"bundleList","(Ljava/lang/String;)[Ljava/lang/String;");
        jstring key=(*env)->NewStringUTF(env,relative);
        jobjectArray names=(*env)->CallObjectMethod(env,activity,list,key);
        (*env)->DeleteLocalRef(env,key); (*env)->DeleteLocalRef(env,cls);
        if (!names) return file_failure(error,path,ENOENT);
        struct Sequence *items=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
        jsize count=(*env)->GetArrayLength(env,names);
        for (jsize index=0;index<count;++index) {
            jstring name=(*env)->GetObjectArrayElement(env,names,index);
            sequence_add(items,selector,string_from_java(env,name,selector));
            (*env)->DeleteLocalRef(env,name);
        }
        (*env)->DeleteLocalRef(env,names);
        return (Word)items;
    }
    DIR *directory=opendir(path);
    if (!directory) return file_failure(error,path,errno);
    struct Sequence *items=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    struct dirent *entry;
    while ((entry=readdir(directory))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        sequence_add(items,selector,copy_string(entry->d_name));
    }
    closedir(directory);
    return (Word)items;
}

static Word manager_set_attributes(Word self,const char *selector,struct GuestMap *attributes,Word filename,Word *error) {
    const char *path=string_bytes(filename,selector);
    if (!writable_sandbox_path(path)) return file_failure(error,path,errno);
    for (size_t index=0;index<attributes->count;++index) {
        const struct MapEntry *entry=attributes->entries+index;
        const char *key=string_bytes(entry->key,selector);
        if (!strcmp(key,"NSFilePosixPermissions")) {
            mode_t mode=(mode_t)numeric_integer((void *)entry->value)&07777;
            if (chmod(path,mode)) return file_failure(error,path,errno);
        } else if (!strcmp(key,"NSFileImmutable")) {
            if (numeric_bool((void *)entry->value)) return file_failure(error,path,ENOTSUP);
        } else if (!strcmp(key,"NSFileProtectionKey")) {
            if (strcmp(string_bytes(entry->value,selector),"NSFileProtectionNone")) return file_failure(error,path,ENOTSUP);
        } else return file_failure(error,path,ENOTSUP);
    }
    return 1;
}

static Word manager_create_path(Word self,const char *selector,Word path,Word intermediates,Word attributes,Word *error) {
    const char *name=string_bytes(path,selector);
    if (!writable_sandbox_path(name)) return file_failure(error,name,errno);
    if (!make_directory(self,selector,(void *)file_url(path),intermediates,0,NULL)) return file_failure(error,name,errno);
    return attributes ? manager_set_attributes(self,selector,(void *)attributes,path,error) : 1;
}

static int remove_tree(const char *path) {
    struct stat info;
    if (lstat(path,&info)) return -1;
    if (!S_ISDIR(info.st_mode)) return unlink(path);
    DIR *directory=opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    int result=0;
    while (!result && (entry=readdir(directory))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char *child=NULL;
        if (asprintf(&child,"%s/%s",path,entry->d_name)<0) { errno=ENOMEM; result=-1; break; }
        result=remove_tree(child);
        free(child);
    }
    int code=errno;
    closedir(directory);
    if (!result) result=rmdir(path);
    else errno=code;
    return result;
}

static Word manager_remove(Word self,const char *selector,Word filename,Word *error) {
    const char *path=string_bytes(filename,selector);
    if (!writable_sandbox_path(path) || remove_tree(path)) return file_failure(error,path,errno);
    return 1;
}

static Word manager_move(Word self,const char *selector,Word source,Word destination,Word *error) {
    const char *from=string_bytes(source,selector), *to=string_bytes(destination,selector);
    if (!writable_sandbox_path(from) || !writable_sandbox_path(to) || rename(from,to)) return file_failure(error,from,errno);
    return 1;
}

static Word manager_writable(Word self,const char *selector,Word filename) {
    const char *path=string_bytes(filename,selector);
    return writable_sandbox_path(path) && access(path,W_OK)==0;
}
static Word manager_readable(Word self,const char *selector,Word filename) {
    const char *name=string_bytes(filename,selector);
    if (bundle_relative(name)!=NULL) return manager_exists_simple(self,selector,filename);
    return access(name,R_OK)==0;
}
static Word manager_contents(Word self,const char *selector,Word filename) { return data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,filename); }
static Word manager_create_file(Word self,const char *selector,Word filename,struct HostObject *data,struct GuestMap *attributes) {
    const char *path=string_bytes(filename,selector);
    if (!writable_sandbox_path(path)) return 0;
    int descriptor=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0666);
    if (descriptor<0) return 0;
    size_t length=data ? data->content[0] : 0,position=0;
    const unsigned char *bytes=data ? (void *)data_buffer(data) : NULL;
    int success=1;
    while (position<length) {
        ssize_t count=write(descriptor,bytes+position,length-position);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { success=0; break; }
        position+=(size_t)count;
    }
    if (close(descriptor)) success=0;
    return success && (!attributes || manager_set_attributes(self,selector,attributes,filename,NULL));
}
static Word error_init(Word self,const char *selector,Word domain,long code,Word info) {
    Word result=error_with_domain(read_word(self),selector,domain,code,info);
    free((void *)self);
    return result;
}

static Word manager_single_directory(Word object,const char *selector,Word directory,Word domain,Word appropriate,unsigned char create,Word *error) {
    if (error) *error=0;
    const char *suffix=directory_suffix(directory);
    if (domain!=1 || !suffix) return file_failure(error,"Sandbox directory",ENOTSUP);
    Word path=*suffix ? append_path(copy_string(files_directory),selector,copy_string(suffix)) : copy_string(files_directory);
    if (create && !manager_create_path(object,selector,path,1,0,error)) return 0;
    struct stat metadata; const char *bytes=string_bytes(path,selector);
    if (stat(bytes,&metadata)) return file_failure(error,bytes,errno);
    if (!S_ISDIR(metadata.st_mode)) return file_failure(error,bytes,ENOTDIR);
    return file_url(path);
}
static Method filesystem_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSFileManager")) {
        if (!strcmp(selector,"URLForDirectory:inDomain:appropriateForURL:create:error:")) return (Method)manager_single_directory;
        if (!strcmp(selector,"URLsForDirectory:inDomains:")) return (Method)manager_directory_urls;
        if (!strcmp(selector,"fileExistsAtPath:")) return (Method)manager_exists_simple;
        if (!strcmp(selector,"fileExistsAtPath:isDirectory:")) return (Method)manager_exists;
        if (!strcmp(selector,"attributesOfItemAtPath:error:")) return (Method)manager_file_attributes;
        if (!strcmp(selector,"attributesOfFileSystemForPath:error:")) return (Method)manager_filesystem_attributes;
        if (!strcmp(selector,"contentsOfDirectoryAtPath:error:")) return (Method)manager_list;
        if (!strcmp(selector,"createDirectoryAtPath:withIntermediateDirectories:attributes:error:")) return (Method)manager_create_path;
        if (!strcmp(selector,"removeItemAtPath:error:")) return (Method)manager_remove;
        if (!strcmp(selector,"moveItemAtPath:toPath:error:")) return (Method)manager_move;
        if (!strcmp(selector,"isWritableFileAtPath:")) return (Method)manager_writable;
        if (!strcmp(selector,"isReadableFileAtPath:")) return (Method)manager_readable;
        if (!strcmp(selector,"contentsAtPath:")) return (Method)manager_contents;
        if (!strcmp(selector,"createFileAtPath:contents:attributes:")) return (Method)manager_create_file;
        if (!strcmp(selector,"setAttributes:ofItemAtPath:error:")) return (Method)manager_set_attributes;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSError")) {
        if (class_receiver && !strcmp(selector,"errorWithDomain:code:userInfo:")) return (Method)error_with_domain;
        if (!class_receiver && !strcmp(selector,"initWithDomain:code:userInfo:")) return (Method)error_init;
        if (!class_receiver && !strcmp(selector,"domain")) return (Method)error_domain;
        if (!class_receiver && !strcmp(selector,"code")) return (Method)error_code;
        if (!class_receiver && !strcmp(selector,"userInfo")) return (Method)error_info;
        if (!class_receiver && !strcmp(selector,"localizedDescription")) return (Method)error_description;
        if (!class_receiver && (!strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:"))) return (Method)identity;
    }
    return NULL;
}
