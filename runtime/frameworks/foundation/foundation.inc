 

static char *native_path(const char *path) {
    const char *relative=bundle_relative(path);
    if (!relative || !*relative) return strdup(path);
    JNIEnv *env=attached_environment(path);
    jstring key=(*env)->NewStringUTF(env,relative);
    jstring filename=(*env)->CallObjectMethod(env,activity,bundle_file_method,key);
    (*env)->DeleteLocalRef(env,key);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        stop("Bundle extraction failed",path);
    }
    if (!filename) { errno=ENOENT; return NULL; }
    const char *name=(*env)->GetStringUTFChars(env,filename,NULL);
    if (!name) stop("Allocation failed",path);
    char *result=strdup(name);
    (*env)->ReleaseStringUTFChars(env,filename,name);
    (*env)->DeleteLocalRef(env,filename);
    if (!result) stop("Allocation failed",path);
    return result;
}

struct EqualPair { Word first,second; };
static int objects_equal_path(Word first,Word second,struct EqualPair *path,unsigned depth) {
    if (first==second) return 1;
    if (!first || !second) return 0;
    if (depth>=128) stop("Object comparison nesting limit","isEqual:");
    for (unsigned i=0;i<depth;++i) if (path[i].first==first && path[i].second==second) return 1;
    path[depth]=(struct EqualPair){first,second};
    Word cls=read_word(first);
    Word other=read_word(second);
    int string=cls==external_class("_OBJC_CLASS_$_NSString") || cls==external_class("_OBJC_CLASS_$_NSMutableString");
    int other_string=other==external_class("_OBJC_CLASS_$_NSString") || other==external_class("_OBJC_CLASS_$_NSMutableString");
    if (string && other_string) {
        struct TextView a=text_view(first,"isEqual:"),b=text_view(second,"isEqual:");
        int equal=a.length==b.length && !memcmp(a.units,b.units,a.length*2);
        free(a.owned); free(b.owned);
        return equal;
    }
    if (cls==decimal_number_class || other==decimal_number_class) return decimal_objects_equal(first,second);
    if (cls==external_class("_OBJC_CLASS_$_NSNumber") && other==cls) {
        struct HostObject *a=(void *)first,*b=(void *)second;
        if (!a->content[1] && !b->content[1]) return a->content[0]==b->content[0];
        if (a->content[1] && b->content[1]) return numeric_double(a)==numeric_double(b);
        struct HostObject *real=a->content[1] ? a : b,*integer=a->content[1] ? b : a;
        double value=numeric_double(real);
        return isfinite(value) && value>=-9223372036854775808.0 && value<9223372036854775808.0 && floor(value)==value && (int64_t)value==(int64_t)integer->content[0];
    }
    int array=cls==external_class("_OBJC_CLASS_$_NSArray") || cls==external_class("_OBJC_CLASS_$_NSMutableArray");
    int other_array=other==external_class("_OBJC_CLASS_$_NSArray") || other==external_class("_OBJC_CLASS_$_NSMutableArray");
    if (array && other_array) {
        struct Sequence *a=(void *)first,*b=(void *)second;
        if (a->count!=b->count) return 0;
        for (size_t i=0;i<a->count;++i) if (!objects_equal_path(a->items[i],b->items[i],path,depth+1)) return 0;
        return 1;
    }
    int dictionary=cls==external_class("_OBJC_CLASS_$_NSDictionary") || cls==external_class("_OBJC_CLASS_$_NSMutableDictionary");
    int other_dictionary=other==external_class("_OBJC_CLASS_$_NSDictionary") || other==external_class("_OBJC_CLASS_$_NSMutableDictionary");
    if (dictionary && other_dictionary) {
        struct GuestMap *a=(void *)first,*b=(void *)second;
        if (a->count!=b->count) return 0;
        for (size_t i=0;i<a->count;++i) {
            size_t j=0;
            for (;j<b->count;++j) if (objects_equal_path(a->entries[i].key,b->entries[j].key,path,depth+1)) break;
            if (j==b->count || !objects_equal_path(a->entries[i].value,b->entries[j].value,path,depth+1)) return 0;
        }
        return 1;
    }
    int data=cls==external_class("_OBJC_CLASS_$_NSData") || cls==external_class("_OBJC_CLASS_$_NSMutableData");
    int other_data=other==external_class("_OBJC_CLASS_$_NSData") || other==external_class("_OBJC_CLASS_$_NSMutableData");
    if (data && other_data) return read_word(first+8)==read_word(second+8) && !memcmp((void *)data_buffer((void *)first),(void *)data_buffer((void *)second),read_word(first+8));
    if (cls==other && cls==external_class("_OBJC_CLASS_$_NSDate")) return date_seconds(first)==date_seconds(second);
    Method native=lookup(cls,"isEqual:");
    if (native) return native(first,"isEqual:",second,0,0,0)&255;
    return 0;
}
static int same_objects(Word first,Word second) { struct EqualPair path[128]; return objects_equal_path(first,second,path,0); }

static Word equal_objects(Word self,const char *selector,Word other) { return same_objects(self,other); }

static Word array_first(struct Sequence *array) { return array->count ? array->items[0] : 0; }

static Word sequence_index(struct Sequence *array,const char *selector,Word object) {
    int identical=!strcmp(selector,"indexOfObjectIdenticalTo:");
    for (size_t index=0;index<array->count;++index)
        if (identical ? array->items[index]==object : same_objects(array->items[index],object)) return index;
    return INT64_MAX;  
}

static Word sequence_contains(struct Sequence *array,const char *selector,Word object) {
    return sequence_index(array,selector,object)!=INT64_MAX;
}

static void sequence_reserve(struct Sequence *array,size_t count,const char *selector) {
    if (count<=array->capacity) return;
    if (count>SIZE_MAX/sizeof(Word)) stop("Collection too large",selector);
    size_t capacity=array->capacity<=SIZE_MAX/2 ? array->capacity*2 : count;
    if (capacity<count) capacity=count;
    if (capacity<8) capacity=8;
    Word *items=realloc(array->items,capacity*sizeof(*items));
    if (!items) stop("Allocation failed",selector);
    array->items=items;
    array->capacity=capacity;
}

static int sequence_set(Word cls) {
    return cls==external_class("_OBJC_CLASS_$_NSSet") || cls==external_class("_OBJC_CLASS_$_NSMutableSet");
}

static void sequence_add(struct Sequence *array,const char *selector,Word object) {
    if (!object) stop("Nil collection object",selector);
    if (sequence_set(array->isa) && sequence_contains(array,selector,object)) return;
    sequence_reserve(array,array->count+1,selector);
    array->items[array->count++]=object;
    ++array->mutation;
}

static void sequence_insert(struct Sequence *array,const char *selector,Word object,size_t index) {
    if (!object || index>array->count) stop("Invalid collection insertion",selector);
    sequence_reserve(array,array->count+1,selector);
    memmove(array->items+index+1,array->items+index,(array->count-index)*sizeof(Word));
    array->items[index]=object;
    ++array->count;
    ++array->mutation;
}

static void sequence_remove_at(struct Sequence *array,const char *selector,size_t index) {
    if (index>=array->count) stop("Collection index out of bounds",selector);
    memmove(array->items+index,array->items+index+1,(array->count-index-1)*sizeof(Word));
    --array->count;
    ++array->mutation;
}

static void sequence_remove(struct Sequence *array,const char *selector,Word object) {
    for (size_t index=array->count;index>0;--index)
        if (same_objects(array->items[index-1],object)) sequence_remove_at(array,selector,index-1);
}

static void sequence_clear(struct Sequence *array) { array->count=0; ++array->mutation; }

static Word sequence_empty(Word cls) { return new_array(cls,"collection",NULL,0); }

static Word sequence_copy(struct Sequence *array,const char *selector) {
    int mutable=!strcmp(selector,"mutableCopy");
    const char *name=sequence_set(array->isa) ? (mutable ? "_OBJC_CLASS_$_NSMutableSet" : "_OBJC_CLASS_$_NSSet") :
        (mutable ? "_OBJC_CLASS_$_NSMutableArray" : "_OBJC_CLASS_$_NSArray");
    return new_array(external_class(name),selector,array->items,array->count);
}

static Word sequence_from_array(Word cls,const char *selector,struct Sequence *source) {
    struct Sequence *result=(void *)sequence_empty(cls);
    for (size_t index=0;index<source->count;++index) sequence_add(result,selector,source->items[index]);
    return (Word)result;
}

static Word array_adding(struct Sequence *array,const char *selector,Word object) {
    struct Sequence *result=(void *)sequence_copy(array,"copy");
    sequence_add(result,selector,object);
    return (Word)result;
}

static Word set_all(struct Sequence *set,const char *selector) {
    return new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,set->items,set->count);
}

static Word append_text(Word self,const char *selector,Word suffix) {
    struct TextView first=text_view(self,selector),second=text_view(suffix,selector);
    if (second.length>=SIZE_MAX/2-first.length) stop("String too large",selector);
    size_t length=first.length+second.length;
    uint16_t *units=malloc((length+1)*sizeof(*units));
    if (!units) stop("Allocation failed",selector);
    memcpy(units,first.units,first.length*2); memcpy(units+first.length,second.units,second.length*2);
    Word result=text_from_units(external_class("_OBJC_CLASS_$_NSString"),units,length);
    free(units); free(first.owned); free(second.owned);
    return result;
}

static Word append_path(Word self,const char *selector,Word suffix) {
    const char *first=string_bytes(self,selector), *second=string_bytes(suffix,selector);
    size_t length=strlen(first);
    while (length>1 && first[length-1]=='/') --length;
    while (*second=='/') ++second;
    char *out=NULL;
    if (asprintf(&out,"%.*s%s%s",(int)length,first,length && *second && first[length-1]!='/' ? "/" : "",second)<0)
        stop("Allocation failed",selector);
    Word result=copy_string(out);
    free(out);
    return result;
}

static Word append_path_extension(Word self,const char *selector,Word suffix) {
    const char *first=string_bytes(self,selector), *second=string_bytes(suffix,selector);
    if (strchr(second,'/')) return 0;
    char *out=NULL;
    if (asprintf(&out,"%s.%s",first,second)<0) stop("Allocation failed",selector);
    Word result=copy_string(out);
    free(out);
    return result;
}

static Word path_extension(Word self,const char *selector) {
    const char *path=string_bytes(self,selector), *slash=strrchr(path,'/');
    const char *name=slash ? slash+1 : path, *dot=strrchr(name,'.');
    return copy_string(dot && dot!=name ? dot+1 : "");
}

static Word path_basename(Word self,const char *selector) {
    const char *path=string_bytes(self,selector), *slash=strrchr(path,'/');
    return copy_string(slash ? slash+1 : path);
}

static Word string_compare(Word self,const char *selector,Word other) {
    int order=compare_ascii_strings(&self,&other);
    return (Word)(intptr_t)(order<0 ? -1 : order>0);
}

static Word string_length(Word self,const char *selector) {
    struct TextView text=text_view(self,selector);
    size_t length=text.length;
    free(text.owned);
    return length;
}

static Word substring_prefix(Word self,const char *selector,size_t count) {
    struct TextView text=text_view(self,selector);
    if (count>text.length) { free(text.owned); stop("Substring index out of bounds",selector); }
    Word result=text_from_units(external_class("_OBJC_CLASS_$_NSString"),text.units,count);
    free(text.owned);
    return result;
}

static Word split_string(Word self,const char *selector,Word delimiter) {
    const char *text=string_bytes(self,selector), *separator=string_bytes(delimiter,selector);
    size_t length=strlen(separator);
    struct Sequence *array=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    if (!length) { sequence_add(array,selector,self); return (Word)array; }
    for (;;) {
        const char *match=strstr(text,separator);
        size_t size=match ? (size_t)(match-text) : strlen(text);
        Word part=new_string(size);
        memcpy((void *)read_word(part+16),text,size);
        sequence_add(array,selector,part);
        if (!match) break;
        text=match+length;
    }
    return (Word)array;
}

static Word data_from_file(Word cls,const char *selector,Word filename) {
    const char *path=string_bytes(filename,selector),*relative=bundle_relative(path);
    if (relative) return bundle_data(cls,relative);
    int fd=io_open(path,O_RDONLY|O_CLOEXEC,0);
    if (fd<0) return 0;
    struct stat info;
    if (io_fstat(fd,&info) || info.st_size<0 || (uint64_t)info.st_size>SIZE_MAX-sizeof(struct HostObject)) { io_close(fd); return 0; }
    size_t length=(size_t)info.st_size;
    struct HostObject *data=calloc(1,sizeof(*data)+length);
    if (!data) stop("Allocation failed",selector);
    data->isa=cls;
    data->content[0]=length;
    if (cls==external_class("_OBJC_CLASS_$_NSMutableData")) {
        data->content[1]=(Word)(data+1); data->content[2]=length; data->content[3]=0x4d444154;
    }
    size_t done=0;
    while (done<length) {
        ssize_t count=io_read(fd,(char *)(data+1)+done,length-done);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { io_close(fd); free(data); return 0; }
        done+=(size_t)count;
    }
    io_close(fd);
    return (Word)data;
}

static Word data_init_file(Word self,const char *selector,Word filename) {
    Word result=data_from_file(read_word(self),selector,filename);
    free((void *)self);
    return result;
}

static Word data_init_bytes(Word self,const char *selector,const void *bytes,size_t length) {
    Word result=data_with_bytes(read_word(self),selector,bytes,length);
    free((void *)self);
    return result;
}

static Word dictionary_from_file(Word cls,const char *selector,Word filename) {
    struct HostObject *data=(void *)data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,filename);
    if(!data)return 0;if(data->content[0]>UINT32_MAX){free(data);return 0;}
    plist_t node=NULL;
    int valid=plist_from_memory((const char *)data_buffer(data),(uint32_t)data->content[0],&node,NULL)==PLIST_ERR_SUCCESS;
    free(data);
    if (!valid) return 0;
    if (plist_get_node_type(node)!=PLIST_DICT) { plist_free(node); return 0; }
    Word result=plist_object(node,0);
    ((struct GuestMap *)result)->isa=cls;
    plist_free(node);
    return result;
}

static Word utf8_pointer(Word self,const char *selector) { return (Word)string_bytes(self,selector); }

static Word integer_from_string(Word self,const char *selector) {
    return (Word)strtoll(string_bytes(self,selector),NULL,10);
}

static double double_from_string(Word self,const char *selector) {
    return strtod(string_bytes(self,selector),NULL);
}

static Word integer_number_factory(Word cls,const char *selector,Word value) {
    if (!strcmp(selector,"numberWithInt:")) value=(Word)(intptr_t)(int32_t)value;
    else if (!strcmp(selector,"numberWithShort:")) value=(Word)(intptr_t)(int16_t)value;
    else if (!strcmp(selector,"numberWithChar:")) value=(Word)(intptr_t)(int8_t)value;
    else if (!strcmp(selector,"numberWithUnsignedInt:")) value=(uint32_t)value;
    else if (!strcmp(selector,"numberWithUnsignedShort:")) value=(uint16_t)value;
    else if (!strcmp(selector,"numberWithUnsignedChar:")) value=(uint8_t)value;
    return number_integer(value);
}

static Word real_number_factory(Word cls,const char *selector,double value) {
    struct HostObject *number=(void *)number_integer(0);
    memcpy(&number->content[0],&value,sizeof(value));
    number->content[1]=1;
    return (Word)number;
}

static Word float_number_factory(Word cls,const char *selector,float value) { return real_number_factory(cls,selector,value); }
static float numeric_float(struct HostObject *number) { return (float)numeric_double(number); }
static Word numeric_short(struct HostObject *number) { return (Word)(intptr_t)(int16_t)numeric_integer(number); }
static Word numeric_char(struct HostObject *number) { return (Word)(intptr_t)(int8_t)numeric_integer(number); }
static const char *numeric_type(struct HostObject *number) { return number->content[1] ? "d" : "q"; }

static Word object_description(Word self,const char *selector) {
    char *text=NULL;
    size_t length=0;
    FILE *stream=open_memstream(&text,&length);
    if (!stream) stop("Allocation failed",selector);
    print_object(stream,self);
    if (fclose(stream)) stop("Description failed",selector);
    Word result=copy_string(text);
    free(text);
    return result;
}

static Word synchronized_defaults(void) { return 1; }

static Word bundle_path(Word self) {
    return self==(Word)&bundle ? copy_string(GUEST_BUNDLE_NAME) : read_word(self+8);
}

static Word bundle_at_path(Word cls,const char *selector,Word path) {
    if (!path) return 0;
    struct HostObject *result=calloc(1,sizeof(*result));
    if (!result) stop("Allocation failed",selector);
    result->isa=cls;
    result->content[0]=path;
    return (Word)result;
}

static Word bundle_metadata(Word self,const char *selector) {
    if (self==(Word)&bundle) return bundle_info();
    struct HostObject *object=(void *)self;
    if (!object->content[2]) {
        Word filename=append_path(bundle_path(self),selector,copy_string("Info.plist"));
        object->content[1]=dictionary_from_file(external_class("_OBJC_CLASS_$_NSDictionary"),selector,filename);
        object->content[2]=1;
    }
    return object->content[1];
}
static Word bundle_info_key(Word self,const char *selector,Word key) {
    Word info=bundle_metadata(self,selector);
    return info ? map_get((void *)info,selector,key) : 0;
}
static Word bundle_class(Word cls,const char *selector,Word target) {
    return in_image(target) ? main_bundle(cls) : 0;
}
static Word bundle_at_url(Word cls,const char *selector,struct HostObject *url) { return bundle_at_path(cls,selector,url_path(url)); }
static Word bundle_url(Word self,const char *selector) { return file_url(bundle_path(self)); }
static Word bundle_executable(Word self,const char *selector) {
    Word name=bundle_info_key(self,selector,copy_string("CFBundleExecutable"));
    return name ? append_path(bundle_path(self),selector,name) : 0;
}
static Word bundle_receipt(Word self,const char *selector) {
    return file_url(append_path(bundle_path(self),selector,copy_string("_MASReceipt/receipt")));
}

static Word bundle_resource(Word self,const char *selector,Word name,Word type) {
    if (!name) return 0;
    Word filename=type && *string_bytes(type,selector) ? append_path_extension(name,selector,type) : name;
    Word root=bundle_path(self),logical=append_path(root,selector,filename);
    const char *logical_path=string_bytes(logical,selector);
    struct stat info;if (!io_stat(logical_path,&info)) return logical;
    {
        JNIEnv *env=attached_environment(selector);
        static jmethodID method;if (!method) {jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"localizedBundleResource","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");(*env)->DeleteLocalRef(env,cls);}
        Word development=bundle_info_key(self,selector,copy_string("CFBundleDevelopmentRegion"));
        jstring base=(*env)->NewStringUTF(env,string_bytes(root,selector)),file=(*env)->NewStringUTF(env,string_bytes(filename,selector));
        jstring language=development ? (*env)->NewStringUTF(env,string_bytes(development,selector)) : NULL;
        jstring result=(*env)->CallObjectMethod(env,activity,method,base,file,language);Word path=0;
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);else if (result) path=string_from_java(env,result,selector);
        if (result) (*env)->DeleteLocalRef(env,result);if (language) (*env)->DeleteLocalRef(env,language);(*env)->DeleteLocalRef(env,base);(*env)->DeleteLocalRef(env,file);
        return path;
    }
}
static Word bundle_resource_directory(Word self,const char *selector,Word name,Word type,Word directory) {return bundle_resource(self,selector,directory && name ? append_path(directory,selector,name) : name,type);}
static Word bundle_resource_localization(Word self,const char *selector,Word name,Word type,Word directory,Word localization) {
    if (!localization) return bundle_resource_directory(self,selector,name,type,directory);if (!name) return 0;
    Word filename=type && *string_bytes(type,selector) ? append_path_extension(name,selector,type) : name;
    if (directory) filename=append_path(directory,selector,filename);
    Word folder=append_path_extension(localization,selector,copy_string("lproj"));Word path=append_path(append_path(bundle_path(self),selector,folder),selector,filename);struct stat info;
    return !io_stat(string_bytes(path,selector),&info) ? path : 0;
}
static Word bundle_resource_url(Word self,const char *selector,Word name,Word type) {Word path=bundle_resource(self,selector,name,type);return path ? file_url(path) : 0;}
static Word bundle_resource_directory_url(Word self,const char *selector,Word name,Word type,Word directory) {Word path=bundle_resource_directory(self,selector,name,type,directory);return path ? file_url(path) : 0;}
static Word bundle_resource_localization_url(Word self,const char *selector,Word name,Word type,Word directory,Word localization) {Word path=bundle_resource_localization(self,selector,name,type,directory,localization);return path ? file_url(path) : 0;}

static Word bundle_localized(Word self,const char *selector,Word key,Word fallback,Word table) {
    Word filename=append_path_extension(table ? table : copy_string("Localizable"),selector,copy_string("strings"));
    Word path=bundle_resource(self,selector,filename,0);
    Word strings=path ? dictionary_from_file(external_class("_OBJC_CLASS_$_NSDictionary"),selector,path) : 0;
    Word value=strings ? map_get((void *)strings,selector,key) : 0;
    return value ? value : fallback && *string_bytes(fallback,selector) ? fallback : key;
}

static Word string_from_data(Word cls,const char *selector,struct HostObject *data,Word encoding) {
    if (!data) return 0;
    return text_decode(cls,(void *)data_buffer(data),data->content[0],encoding);
}

static Word string_init_data(Word self,const char *selector,struct HostObject *data,Word encoding) {
    Word result=string_from_data(read_word(self),selector,data,encoding);
    free((void *)self);
    return result;
}

static Word string_file(Word cls,const char *selector,Word path,Word encoding,Word *error) {
    Word data=data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,path);
    if (!data) return 0;
    return string_from_data(cls,selector,(void *)data,encoding);
}

static Word copy_via_zone(Word self,const char *selector) {
    const char *operation=!strcmp(selector,"mutableCopy") ? "mutableCopyWithZone:" : "copyWithZone:";
    Method copy=find_implementation(self,operation);
    if (!copy) stop("Missing copy implementation",operation);
    return copy(self,operation,0,0,0,0);
}

static Word responds_selector(Word self,const char *selector,const char *wanted) {
    return wanted && find_implementation(self,wanted)!=NULL;
}
static Word instances_respond_selector(Word cls,const char *selector,const char *wanted) {
    Word prototype=cls;
    return wanted && find_implementation((Word)&prototype,wanted)!=NULL;
}

static Word host_allocate(Word cls) {
    const char *name=host_symbol(cls);
    if (!name) return guest_object(cls);
    if (!strcmp(name,"_OBJC_CLASS_$_NSArray") || !strcmp(name,"_OBJC_CLASS_$_NSMutableArray") ||
        !strcmp(name,"_OBJC_CLASS_$_NSOrderedSet") || !strcmp(name,"_OBJC_CLASS_$_NSMutableOrderedSet") ||
        !strcmp(name,"_OBJC_CLASS_$_NSIndexSet") || !strcmp(name,"_OBJC_CLASS_$_NSMutableIndexSet") || sequence_set(cls)) return sequence_empty(cls);
    if (!strcmp(name,"_OBJC_CLASS_$_NSDictionary") || !strcmp(name,"_OBJC_CLASS_$_NSMutableDictionary")) return empty_map(cls);
    struct HostObject *object=calloc(1,host_allocation_size(cls));
    if (!object) stop("Allocation failed",name);
    object->isa=cls;
    return (Word)object;
}

static Word new_object(Word cls) {
    Word object=host_allocate(cls);
    Method init=find_implementation(object,"init");
    if (!init) stop("Unimplemented initializer",class_name(cls));
    return init(object,"init",0,0,0,0);
}

static int protocol_list_contains(Word list,Word target,unsigned depth) {
    if (!list || depth>32) return 0;
    size_t count=read_word(list);
    if (count>4096) stop("Invalid protocol list","conformsToProtocol:");
    for (size_t index=0;index<count;++index) {
        Word protocol=read_word(list+8+8*index);
        if (protocol==target) return 1;
        if (in_image(protocol) && protocol_list_contains(read_word(protocol+16),target,depth+1)) return 1;
    }
    return 0;
}

static Word conforms_protocol(Word self,const char *selector,Word protocol) {
    if(!self || !protocol)return 0;
    Word cls=guest_class_object(self) || host_symbol(self) ? self : read_word(self);
    for (unsigned depth=0;depth<64 && cls;++depth) {
        for(struct AddedProtocol *p=__atomic_load_n(&added_protocols,__ATOMIC_ACQUIRE);p;p=p->next)
            if(p->cls==cls && (p->protocol==protocol || (in_image(p->protocol) && protocol_list_contains(read_word(p->protocol+16),protocol,0))))return 1;
        if(in_image(cls)){
            Word ro=read_word(cls+32)&~(Word)7;
            if(class_ro_valid(cls,ro) && protocol_list_contains(read_word(ro+40),protocol,0))return 1;
            const Word *categories=(const Word *)(aslr+CATEGORIES_OFFSET);
            for(unsigned index=0;index<CATEGORIES_COUNT;++index)
                if(read_word(categories[index]+8)==cls && protocol_list_contains(read_word(categories[index]+32),protocol,0))return 1;
            cls=read_word(cls+8);
        } else if(dynamic_class(cls))cls=read_word(cls+8);
        else cls=host_superclass(cls);
    }
    return 0;
}

struct ImageSize { double width, height; };

static uint32_t big_endian32(const unsigned char *bytes) {
    return ((uint32_t)bytes[0]<<24)|((uint32_t)bytes[1]<<16)|((uint32_t)bytes[2]<<8)|bytes[3];
}

static Word image_from_data(Word cls,const char *selector,struct HostObject *data) {
    if (!data || data->content[0]<8) return 0;
    const unsigned char *bytes=(const void *)data_buffer(data);
    static const unsigned char signature[]={137,80,78,71,13,10,26,10};
    if (memcmp(bytes,signature,sizeof(signature))) {
        data=(void *)image_data_normalize((Word)data);
        if (!data || data->content[0]<24) return 0;
        bytes=(const void *)data_buffer(data);
    }
    size_t length=data->content[0], offset=8;
    uint32_t width=0,height=0;
    while (length-offset>=12) {
        uint32_t size=big_endian32(bytes+offset);
        if (size>length-offset-12) return 0;
        if (!memcmp(bytes+offset+4,"IHDR",4)) {
            if (size!=13) return 0;
            width=big_endian32(bytes+offset+8);
            height=big_endian32(bytes+offset+12);
            break;
        }
        offset+=size+12;
    }
    if (!width || !height) return 0;
    struct HostObject *image=calloc(1,sizeof(*image));
    if (!image) stop("Allocation failed",selector);
    image->isa=cls;
    image->content[0]=(Word)data;
    image->content[1]=width;
    image->content[2]=height;
    LOG("PNG metadata loaded %ux%u",width,height);
    return (Word)image;
}

static struct ImageSize image_size(struct HostObject *image) {
    return (struct ImageSize){image->content[1],image->content[2]};
}

static double image_scale(void) { return 1; }

static Word image_init_file(Word self,const char *selector,Word filename) {
    Word data=data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,filename);
    Word result=image_from_data(read_word(self),selector,(void *)data);
    free((void *)self);
    return result;
}

static void enumeration_mutation(Word collection) {
    stop("Collection mutated during enumeration",class_name(read_word(collection)));
}

#include "image_png.inc"
#include "strings.inc"
#include "collections.inc"
#include "formatting.inc"

static Method foundation_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    Method text_method=strings_method(object,selector,receiver,class_receiver);
    if (text_method) return text_method;
    Method collection_method=collections_method(object,selector,receiver,class_receiver);
    if (collection_method) return collection_method;
    Method format_method=formatting_method(object,selector,receiver,class_receiver);
    if (format_method) return format_method;
    if (!class_receiver && ((!strcmp(selector,"copy") && lookup(read_word(object),"copyWithZone:")) ||
        (!strcmp(selector,"mutableCopy") && lookup(read_word(object),"mutableCopyWithZone:")))) return (Method)copy_via_zone;
    if (!strcmp(selector,"respondsToSelector:")) return (Method)responds_selector;
    if (!strcmp(selector,"self")) return (Method)identity;
    if ((class_receiver || guest_class_object(object)) && !strcmp(selector,"instancesRespondToSelector:")) return (Method)instances_respond_selector;
    if ((class_receiver || guest_class_object(object)) && !strcmp(selector,"isSubclassOfClass:")) return (Method)is_subclass_of;
    if (!strcmp(selector,"retain")) return (Method)metal_object_retain;
    if (!strcmp(selector,"autorelease")) return (Method)metal_object_autorelease;
    if (!strcmp(selector,"release")) return (Method)metal_object_release;
    if (!strcmp(selector,"conformsToProtocol:")) return (Method)conforms_protocol;
    if (!strcmp(selector,"description") || !strcmp(selector,"debugDescription")) return (Method)object_description;
    if (object==(Word)&defaults && !strcmp(selector,"synchronize")) return (Method)synchronized_defaults;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSNumber")) {
        const char *const constructors[]={"numberWithInteger:","numberWithUnsignedInteger:","numberWithInt:","numberWithUnsignedInt:",
            "numberWithShort:","numberWithUnsignedShort:","numberWithChar:","numberWithUnsignedChar:","numberWithLong:",
            "numberWithUnsignedLong:","numberWithLongLong:","numberWithUnsignedLongLong:"};
        if (class_receiver) for (unsigned index=0;index<COUNT(constructors);++index)
            if (!strcmp(selector,constructors[index])) return (Method)integer_number_factory;
        if (class_receiver && !strcmp(selector,"numberWithDouble:")) return (Method)real_number_factory;
        if (class_receiver && !strcmp(selector,"numberWithFloat:")) return (Method)float_number_factory;
        if (!class_receiver && !strcmp(selector,"doubleValue")) return (Method)numeric_double;
        if (!class_receiver && !strcmp(selector,"floatValue")) return (Method)numeric_float;
        if (!class_receiver && !strcmp(selector,"shortValue")) return (Method)numeric_short;
        if (!class_receiver && !strcmp(selector,"charValue")) return (Method)numeric_char;
        if (!class_receiver && (!strcmp(selector,"longLongValue") || !strcmp(selector,"unsignedLongLongValue") ||
            !strcmp(selector,"unsignedIntegerValue") || !strcmp(selector,"unsignedIntValue"))) return (Method)numeric_integer;
        if (!class_receiver && !strcmp(selector,"objCType")) return (Method)numeric_type;
        if (!class_receiver && !strcmp(selector,"copy")) return (Method)identity;
    }
    if (class_receiver || guest_class_object(object)) {
        if (!strcmp(selector,"alloc") || !strcmp(selector,"allocWithZone:")) return (Method)host_allocate;
        if (!strcmp(selector,"new")) return (Method)new_object;
    }
    if (!class_receiver && !strcmp(selector,"init") && class_inherits(read_word(object),external_class("_OBJC_CLASS_$_NSObject"))) return (Method)identity;
    int set=!strcmp(receiver,"_OBJC_CLASS_$_NSSet") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableSet");
    int array=!strcmp(receiver,"_OBJC_CLASS_$_NSArray") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray");
    int mutable=!strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableSet");
    if ((array || set) && object!=(Word)&arguments_array) {
        if (class_receiver) {
            if (!strcmp(selector,"array") || !strcmp(selector,"set") || !strcmp(selector,"arrayWithCapacity:") || !strcmp(selector,"setWithCapacity:")) return (Method)sequence_empty;
            if (!strcmp(selector,"setWithArray:") || !strcmp(selector,"arrayWithArray:")) return (Method)sequence_from_array;
            if (!strcmp(selector,"arrayWithObjects:count:")) return (Method)new_array;
        } else {
            if (!strcmp(selector,"copy") || !strcmp(selector,"mutableCopy")) return (Method)sequence_copy;
            if (!strcmp(selector,"count")) return (Method)array_count;
            if (!strcmp(selector,"containsObject:")) return (Method)sequence_contains;
            if (!strcmp(selector,"countByEnumeratingWithState:objects:count:")) return (Method)array_enumerate;
            if (!strcmp(selector,"allObjects")) return (Method)set_all;
            if (!strcmp(selector,"anyObject") || !strcmp(selector,"firstObject")) return (Method)array_first;
            if (!strcmp(selector,"lastObject")) return (Method)array_last;
            if (array && (!strcmp(selector,"objectAtIndex:") || !strcmp(selector,"objectAtIndexedSubscript:"))) return (Method)array_at;
            if (array && (!strcmp(selector,"indexOfObject:") || !strcmp(selector,"indexOfObjectIdenticalTo:"))) return (Method)sequence_index;
            if (array && !strcmp(selector,"arrayByAddingObject:")) return (Method)array_adding;
            if (array && !strcmp(selector,"sortedArrayUsingSelector:")) return (Method)sorted_array;
            if (mutable) {
                if (!strcmp(selector,"addObject:")) return (Method)sequence_add;
                if (!strcmp(selector,"removeObject:")) return (Method)sequence_remove;
                if (!strcmp(selector,"removeAllObjects")) return (Method)sequence_clear;
                if (array && !strcmp(selector,"insertObject:atIndex:")) return (Method)sequence_insert;
                if (array && !strcmp(selector,"removeObjectAtIndex:")) return (Method)sequence_remove_at;
            }
        }
    }
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString"))) {
        if (!strcmp(selector,"initWithData:encoding:")) return (Method)string_init_data;
        if (!strcmp(selector,"isEqualToString:") || !strcmp(selector,"isEqual:")) return (Method)equal_objects;
        if (!strcmp(selector,"length")) return (Method)string_length;
        if (!strcmp(selector,"UTF8String") || !strcmp(selector,"fileSystemRepresentation")) return (Method)utf8_pointer;
        if (!strcmp(selector,"copy")) return (Method)identity;
        if (!strcmp(selector,"compare:")) return (Method)string_compare;
        if (!strcmp(selector,"integerValue") || !strcmp(selector,"intValue") || !strcmp(selector,"longLongValue")) return (Method)integer_from_string;
        if (!strcmp(selector,"doubleValue")) return (Method)double_from_string;
        if (!strcmp(selector,"stringByAppendingString:")) return (Method)append_text;
        if (!strcmp(selector,"stringByAppendingPathComponent:")) return (Method)append_path;
        if (!strcmp(selector,"stringByAppendingPathExtension:")) return (Method)append_path_extension;
        if (!strcmp(selector,"pathExtension")) return (Method)path_extension;
        if (!strcmp(selector,"lastPathComponent")) return (Method)path_basename;
        if (!strcmp(selector,"substringToIndex:")) return (Method)substring_prefix;
        if (!strcmp(selector,"componentsSeparatedByString:")) return (Method)split_string;
    }
    if (class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSString") && !strcmp(selector,"stringWithContentsOfFile:encoding:error:")) return (Method)string_file;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSData")) {
        if (class_receiver && !strcmp(selector,"dataWithContentsOfFile:")) return (Method)data_from_file;
        if (!class_receiver && !strcmp(selector,"initWithContentsOfFile:")) return (Method)data_init_file;
        if (!class_receiver && !strcmp(selector,"initWithBytes:length:")) return (Method)data_init_bytes;
        if (!class_receiver && !strcmp(selector,"copy")) return (Method)identity;
    }
    if (class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary")) && !strcmp(selector,"dictionaryWithContentsOfFile:")) return (Method)dictionary_from_file;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSBundle")) {
        if (class_receiver && !strcmp(selector,"bundleWithPath:")) return (Method)bundle_at_path;
        if (class_receiver && !strcmp(selector,"bundleWithURL:")) return (Method)bundle_at_url;
        if (class_receiver && !strcmp(selector,"bundleForClass:")) return (Method)bundle_class;
        if (!class_receiver && !strcmp(selector,"infoDictionary")) return (Method)bundle_metadata;
        if (!class_receiver && !strcmp(selector,"objectForInfoDictionaryKey:")) return (Method)bundle_info_key;
        if (!class_receiver && (!strcmp(selector,"bundleURL") || !strcmp(selector,"resourceURL"))) return (Method)bundle_url;
        if (!class_receiver && !strcmp(selector,"executablePath")) return (Method)bundle_executable;
        if (!class_receiver && !strcmp(selector,"appStoreReceiptURL")) return (Method)bundle_receipt;
        if (!class_receiver && (!strcmp(selector,"bundlePath") || !strcmp(selector,"resourcePath"))) return (Method)bundle_path;
        if (!class_receiver && !strcmp(selector,"pathForResource:ofType:")) return (Method)bundle_resource;
        if (!class_receiver && !strcmp(selector,"pathForResource:ofType:inDirectory:")) return (Method)bundle_resource_directory;
        if (!class_receiver && !strcmp(selector,"pathForResource:ofType:inDirectory:forLocalization:")) return (Method)bundle_resource_localization;
        if (!class_receiver && !strcmp(selector,"URLForResource:withExtension:")) return (Method)bundle_resource_url;
        if (!class_receiver && !strcmp(selector,"URLForResource:withExtension:subdirectory:")) return (Method)bundle_resource_directory_url;
        if (!class_receiver && !strcmp(selector,"URLForResource:withExtension:subdirectory:localization:")) return (Method)bundle_resource_localization_url;
        if (!class_receiver && !strcmp(selector,"localizedStringForKey:value:table:")) return (Method)bundle_localized;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIImage")) {
        if (class_receiver && !strcmp(selector,"imageWithData:")) return (Method)image_from_data;
        if (!class_receiver && !strcmp(selector,"initWithContentsOfFile:")) return (Method)image_init_file;
        if (!class_receiver && !strcmp(selector,"size")) return (Method)image_size;
        if (!class_receiver && !strcmp(selector,"scale")) return (Method)image_scale;
    }
    return NULL;
}
