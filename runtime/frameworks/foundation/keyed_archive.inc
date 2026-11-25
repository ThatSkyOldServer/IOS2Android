 
struct ArchiveReference { Word object; uint32_t index; struct ArchiveReference *next; };
struct ArchiveClass { Word cls; uint32_t index; struct ArchiveClass *next; };
struct ConditionalReference { plist_t container; char *key; Word object; struct ConditionalReference *next; };
struct ArchiveGraph {
    plist_t root,objects,top;
    struct ArchiveReference *references;
    struct ArchiveClass *classes;
    struct ConditionalReference *conditional;
    Word *decoded,output;
    unsigned char *visited;
    uint32_t count;
    unsigned depth;
    int decoding,finished;
};
struct KeyedCoder { Word isa; struct ArchiveGraph *graph; plist_t container; unsigned generic; };
static plist_t archive_encode(struct ArchiveGraph *,Word);
static Word archive_decode(struct ArchiveGraph *,plist_t);

static struct KeyedCoder *keyed_coder(struct ArchiveGraph *graph,plist_t container) {
    struct KeyedCoder *coder=calloc(1,sizeof(*coder));
    if (!coder) stop("Allocation failed","NSCoder");
    coder->isa=external_class(graph->decoding ? "_OBJC_CLASS_$_NSKeyedUnarchiver" : "_OBJC_CLASS_$_NSKeyedArchiver");
    coder->graph=graph; coder->container=container;
    return coder;
}
static char *coder_key(struct KeyedCoder *coder,Word key) {
    if (!key) { char text[32]; snprintf(text,sizeof(text),"$%u",coder->generic++); return strdup(text); }
    const char *name=string_bytes(key,"NSCoder key");
    if (*name!='$') return strdup(name);
    size_t length=strlen(name);
    char *escaped=malloc(length+2);
    if (!escaped) stop("Allocation failed","NSCoder key");
    escaped[0]='$'; memcpy(escaped+1,name,length+1); return escaped;
}
static void coder_set(struct KeyedCoder *coder,Word key,plist_t value) {
    if (!coder->graph || coder->graph->decoding || coder->graph->finished) stop("Encoder is not writable","NSKeyedArchiver");
    char *name=coder_key(coder,key);
    plist_dict_set_item(coder->container,name,value); free(name);
}
static plist_t coder_get(struct KeyedCoder *coder,Word key) {
    if (!coder->graph || !coder->graph->decoding || coder->graph->finished) stop("Decoder is not readable","NSKeyedUnarchiver");
    char *name=coder_key(coder,key);
    plist_t value=plist_dict_get_item(coder->container,name); free(name); return value;
}
static uint32_t archive_index(struct ArchiveGraph *graph,Word object) {
    for (struct ArchiveReference *entry=graph->references;entry;entry=entry->next) if (entry->object==object) return entry->index;
    return 0;
}
static const char *archive_class_name(Word cls) {
    const char *name=class_name(cls);
    if (!name) stop("Invalid archive class","NSKeyedArchiver");
    return !strncmp(name,"_OBJC_CLASS_$_",14) ? name+14 : name;
}
static plist_t archive_class(struct ArchiveGraph *graph,Word cls) {
    for (struct ArchiveClass *entry=graph->classes;entry;entry=entry->next) if (entry->cls==cls) return plist_new_uid(entry->index);
    struct ArchiveClass *entry=calloc(1,sizeof(*entry));
    if (!entry) stop("Allocation failed","Archive class");
    entry->cls=cls; entry->index=plist_array_get_size(graph->objects); entry->next=graph->classes; graph->classes=entry;
    plist_t record=plist_new_dict(),classes=plist_new_array();
    plist_dict_set_item(record,"$classname",plist_new_string(archive_class_name(cls)));
    Word current=cls;
    for (unsigned depth=0;current && depth<128;++depth) {
        const char *name=archive_class_name(current);
        plist_array_append_item(classes,plist_new_string(name));
        if (!strcmp(name,"NSObject")) break;
        if (in_image(current)) current=read_word(current+8);
        else if (!strncmp(name,"NSMutable",9)) {
            char base[100]; snprintf(base,sizeof(base),"_OBJC_CLASS_$_NS%s",name+9); current=external_class(base);
        } else current=external_class("_OBJC_CLASS_$_NSObject");
    }
    plist_dict_set_item(record,"$classes",classes);
    plist_array_append_item(graph->objects,record);
    return plist_new_uid(entry->index);
}
static int archive_is_class(Word cls,const char *name) { return cls && cls==external_class(name); }
static plist_t archive_encode(struct ArchiveGraph *graph,Word object) {
    if (!object) return plist_new_uid(0);
    uint32_t index=archive_index(graph,object);
    if (index) return plist_new_uid(index);
    if (++graph->depth>128) stop("Archive nesting exceeds limit","NSKeyedArchiver");
    struct ArchiveReference *entry=calloc(1,sizeof(*entry));
    if (!entry) stop("Allocation failed","Archive reference");
    index=plist_array_get_size(graph->objects);
    entry->object=object; entry->index=index; entry->next=graph->references; graph->references=entry;
    plist_t record=plist_new_dict(); plist_array_append_item(graph->objects,record);
    Word cls=read_word(object);
    const char *name=archive_class_name(cls);
    if (archive_is_class(cls,"_OBJC_CLASS_$_NSString") || archive_is_class(cls,"_OBJC_CLASS_$_NSNumber") || archive_is_class(cls,"_OBJC_CLASS_$_NSData")) {
        Word ancestors[64]; plist_t primitive=object_property_list(object,ancestors,0);
        if (!primitive) stop("Invalid archive scalar",name);
        plist_array_set_item(graph->objects,primitive,index);
    } else {
        int array=archive_is_class(cls,"_OBJC_CLASS_$_NSArray") || archive_is_class(cls,"_OBJC_CLASS_$_NSMutableArray") || archive_is_class(cls,"_OBJC_CLASS_$_NSSet") || archive_is_class(cls,"_OBJC_CLASS_$_NSMutableSet");
        int dictionary=archive_is_class(cls,"_OBJC_CLASS_$_NSDictionary") || archive_is_class(cls,"_OBJC_CLASS_$_NSMutableDictionary");
        if (array || dictionary) {
            plist_t items=plist_new_array(),keys=dictionary ? plist_new_array() : NULL;
            size_t count=read_word(object+8);
            for (size_t i=0;i<count;++i) {
                Word value=dictionary ? ((struct GuestMap *)object)->entries[i].value : ((struct Sequence *)object)->items[i];
                plist_array_append_item(items,archive_encode(graph,value));
                if (dictionary) plist_array_append_item(keys,archive_encode(graph,((struct GuestMap *)object)->entries[i].key));
            }
            plist_dict_set_item(record,"NS.objects",items);
            if (keys) plist_dict_set_item(record,"NS.keys",keys);
        } else if (archive_is_class(cls,"_OBJC_CLASS_$_NSMutableString")) plist_dict_set_item(record,"NS.string",plist_new_string(string_bytes(object,"Archive string")));
        else if (archive_is_class(cls,"_OBJC_CLASS_$_NSMutableData")) plist_dict_set_item(record,"NS.data",plist_new_data((void *)data_buffer((void *)object),read_word(object+8)));
        else if (archive_is_class(cls,"_OBJC_CLASS_$_NSDate")) plist_dict_set_item(record,"NS.time",plist_new_real(date_seconds(object)-978307200));
        else if (archive_is_class(cls,"_OBJC_CLASS_$_NSURL")) {
            plist_dict_set_item(record,"NS.base",plist_new_uid(0));
            plist_dict_set_item(record,"NS.relative",archive_encode(graph,url_absolute((void *)object,"Archive URL")));
        } else if (archive_is_class(cls,"_OBJC_CLASS_$_NSError")) {
            struct HostObject *error=(void *)object;
            plist_dict_set_item(record,"NSDomain",archive_encode(graph,error->content[0]));
            plist_dict_set_item(record,"NSCode",plist_new_int((int64_t)error->content[1]));
            plist_dict_set_item(record,"NSUserInfo",archive_encode(graph,error->content[2]));
        } else if (archive_is_class(cls,"_OBJC_CLASS_$_NSUUID")) plist_dict_set_item(record,"NS.uuidbytes",plist_new_data((void *)((struct HostObject *)object)->content,16));
        else if (!archive_is_class(cls,"_OBJC_CLASS_$_NSNull")) {
            Method encode=find_implementation(object,"encodeWithCoder:");
            if (!encode) stop("Class has no NSCoding implementation",name);
            struct KeyedCoder *coder=keyed_coder(graph,record);
            encode(object,"encodeWithCoder:",(Word)coder,0,0,0);
        }
        const char *class_selector="classForKeyedArchiver";
        Method class_for_coder=find_implementation(object,class_selector);
        if (!class_for_coder) { class_selector="classForCoder"; class_for_coder=find_implementation(object,class_selector); }
        if (class_for_coder) { Word substitute=class_for_coder(object,class_selector,0,0,0,0); if (substitute) cls=substitute; }
        plist_dict_set_item(record,"$class",archive_class(graph,cls));
    }
    --graph->depth;
    return plist_new_uid(index);
}
static struct ArchiveGraph *archive_graph(void) {
    struct ArchiveGraph *graph=calloc(1,sizeof(*graph));
    if (!graph) stop("Allocation failed","NSKeyedArchiver");
    graph->root=plist_new_dict(); graph->objects=plist_new_array(); graph->top=plist_new_dict();
    plist_dict_set_item(graph->root,"$archiver",plist_new_string("NSKeyedArchiver"));
    plist_dict_set_item(graph->root,"$version",plist_new_uint(100000));
    plist_dict_set_item(graph->root,"$objects",graph->objects); plist_dict_set_item(graph->root,"$top",graph->top);
    plist_array_append_item(graph->objects,plist_new_string("$null"));
    return graph;
}
static Word archive_finish(struct ArchiveGraph *graph) {
    for (struct ConditionalReference *ref=graph->conditional;ref;ref=ref->next)
        plist_dict_set_item(ref->container,ref->key,plist_new_uid(archive_index(graph,ref->object)));
    char *bytes=NULL; uint32_t length=0;
    if (plist_to_bin(graph->root,&bytes,&length)!=PLIST_ERR_SUCCESS) stop("Archive serialization failed","NSKeyedArchiver");
    Word data;
    if (graph->output) {
        data=graph->output; data_set_length((void *)data,"finishEncoding",0); data_append_bytes((void *)data,"finishEncoding",bytes,length);
    } else data=data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),"NSKeyedArchiver",bytes,length);
    plist_mem_free(bytes); graph->finished=1;
    return data;
}
static Word archived_data(Word cls,const char *selector,Word object) {
    struct ArchiveGraph *graph=archive_graph();
    plist_dict_set_item(graph->top,"root",archive_encode(graph,object));
    Word data=archive_finish(graph);
    LOG_LIMITED(8,"Keyed archive %s: %u objects, %lu bytes",object ? class_name(read_word(object)) : "nil",plist_array_get_size(graph->objects),read_word(data+8));
    return data;
}
static Word archived_file(Word cls,const char *selector,Word root,Word file) {
    Word data=archived_data(cls,selector,root);
    return data_write_file((void *)data,selector,file,1,NULL);
}
static Word archiver_init(struct KeyedCoder *coder,const char *selector,Word data) {
    if (!data || read_word(data)!=external_class("_OBJC_CLASS_$_NSMutableData")) stop("Expected mutable archive buffer",selector);
    coder->graph=archive_graph(); coder->container=coder->graph->top; coder->graph->output=data; return (Word)coder;
}
static void coder_encode_object(struct KeyedCoder *coder,const char *selector,Word object,Word key) { coder_set(coder,key,archive_encode(coder->graph,object)); }
static void coder_encode_unkeyed(struct KeyedCoder *coder,const char *selector,Word object) { coder_encode_object(coder,selector,object,0); }
static void coder_encode_conditional(struct KeyedCoder *coder,const char *selector,Word object,Word key) {
    struct ConditionalReference *ref=calloc(1,sizeof(*ref));
    if (!ref) stop("Allocation failed",selector);
    ref->container=coder->container; ref->object=object; ref->key=coder_key(coder,key);
    ref->next=coder->graph->conditional; coder->graph->conditional=ref;
    plist_dict_set_item(coder->container,ref->key,plist_new_uid(archive_index(coder->graph,object)));
}
static void coder_encode_bool(struct KeyedCoder *coder,const char *selector,unsigned char value,Word key) { coder_set(coder,key,plist_new_bool(value!=0)); }
static void coder_encode_integer(struct KeyedCoder *coder,const char *selector,int64_t value,Word key) { coder_set(coder,key,plist_new_int(value)); }
static void coder_encode_int(struct KeyedCoder *coder,const char *selector,int32_t value,Word key) { coder_encode_integer(coder,selector,value,key); }
static void coder_encode_double(struct KeyedCoder *coder,const char *selector,double value,Word key) { coder_set(coder,key,plist_new_real(value)); }
static void coder_encode_float(struct KeyedCoder *coder,const char *selector,float value,Word key) { coder_encode_double(coder,selector,value,key); }
static void coder_encode_bytes(struct KeyedCoder *coder,const char *selector,const void *bytes,size_t length,Word key) { coder_set(coder,key,plist_new_data(bytes,length)); }
static void coder_finish_encoding(struct KeyedCoder *coder) { if (!coder->graph->finished) archive_finish(coder->graph); }
static Word coder_yes(void) { return 1; }
static Word coder_no(void) { return 0; }
static void coder_secure(struct KeyedCoder *coder,const char *selector,unsigned char required) { if (required) stop("Secure-coding validation is not implemented",selector); }

static uint32_t archive_uid(struct ArchiveGraph *graph,plist_t ref) {
    uint64_t index=0;
    if (!ref) return 0;
    if (plist_get_node_type(ref)!=PLIST_UID) stop("Expected keyed archive UID","NSKeyedUnarchiver");
    plist_get_uid_val(ref,&index);
    if (index>=graph->count) stop("Invalid keyed archive reference","NSKeyedUnarchiver");
    return (uint32_t)index;
}
static double archive_real(plist_t value) {
    if (!value) return 0;
    if (plist_get_node_type(value)==PLIST_REAL) { double number=0; plist_get_real_val(value,&number); return number; }
    if (plist_get_node_type(value)==PLIST_BOOLEAN) { uint8_t flag=0; plist_get_bool_val(value,&flag); return flag; }
    int64_t integer=0; plist_get_int_val(value,&integer); return (double)integer;
}
static Word archive_decode(struct ArchiveGraph *graph,plist_t reference) {
    if (!reference) return 0;
    if (plist_get_node_type(reference)!=PLIST_UID) return plist_object(reference,0);
    uint32_t index=archive_uid(graph,reference);
    if (!index) return 0;
    if (graph->visited[index]) return graph->decoded[index];
    if (++graph->depth>128) stop("Archive nesting exceeds limit","NSKeyedUnarchiver");
    graph->visited[index]=1;
    plist_t record=plist_array_get_item(graph->objects,index);
    if (plist_get_node_type(record)!=PLIST_DICT) graph->decoded[index]=plist_object(record,0);
    else {
        uint32_t class_index=archive_uid(graph,plist_dict_get_item(record,"$class"));
        plist_t description=plist_array_get_item(graph->objects,class_index);
        const char *name=plist_get_string_ptr(plist_dict_get_item(description,"$classname"),NULL);
        if (!name) stop("Missing archive class name","NSKeyedUnarchiver");
        Word cls=class_from_name(name);
        if (!cls) stop("Unknown archived class",name);
        int array=!strcmp(name,"NSArray") || !strcmp(name,"NSMutableArray") || !strcmp(name,"NSSet") || !strcmp(name,"NSMutableSet");
        int dictionary=!strcmp(name,"NSDictionary") || !strcmp(name,"NSMutableDictionary");
        if (array || dictionary) {
            plist_t values=plist_dict_get_item(record,"NS.objects"),keys=plist_dict_get_item(record,"NS.keys");
            if (plist_get_node_type(values)!=PLIST_ARRAY || (dictionary && plist_array_get_size(values)!=plist_array_get_size(keys))) stop("Invalid archived collection",name);
            Word collection=array ? sequence_empty(cls) : empty_map(cls); graph->decoded[index]=collection;
            for (uint32_t i=0;i<plist_array_get_size(values);++i) {
                Word value=archive_decode(graph,plist_array_get_item(values,i));
                if (dictionary) map_set((void *)collection,"NSKeyedUnarchiver",value,archive_decode(graph,plist_array_get_item(keys,i)));
                else sequence_add((void *)collection,"NSKeyedUnarchiver",value);
            }
        } else if (!strcmp(name,"NSString") || !strcmp(name,"NSMutableString")) {
            Word string=plist_object(plist_dict_get_item(record,"NS.string"),0); *(Word *)string=cls; graph->decoded[index]=string;
        } else if (!strcmp(name,"NSData") || !strcmp(name,"NSMutableData")) {
            uint64_t length=0; const char *bytes=plist_get_data_ptr(plist_dict_get_item(record,"NS.data"),&length);
            graph->decoded[index]=data_with_bytes(cls,"NSKeyedUnarchiver",bytes,(size_t)length);
        } else if (!strcmp(name,"NSDate")) graph->decoded[index]=date_epoch(cls,"NSKeyedUnarchiver",978307200+archive_real(plist_dict_get_item(record,"NS.time")));
        else if (!strcmp(name,"NSURL")) {
            Word base=archive_decode(graph,plist_dict_get_item(record,"NS.base"));
            Word relative=archive_decode(graph,plist_dict_get_item(record,"NS.relative"));
            if (base) stop("Relative archived URL needs base resolution","NSKeyedUnarchiver");
            graph->decoded[index]=url_from_string(cls,"NSKeyedUnarchiver",relative);
        } else if (!strcmp(name,"NSError")) {
            struct HostObject *error=(void *)host_allocate(cls); graph->decoded[index]=(Word)error;
            error->content[0]=archive_decode(graph,plist_dict_get_item(record,"NSDomain"));
            int64_t code=0; plist_get_int_val(plist_dict_get_item(record,"NSCode"),&code); error->content[1]=(Word)code;
            error->content[2]=archive_decode(graph,plist_dict_get_item(record,"NSUserInfo"));
        } else if (!strcmp(name,"NSUUID")) {
            uint64_t length=0; const char *bytes=plist_get_data_ptr(plist_dict_get_item(record,"NS.uuidbytes"),&length);
            if (length!=16) stop("Invalid archived UUID","NSKeyedUnarchiver");
            graph->decoded[index]=uuid_init_bytes((void *)host_allocate(cls),"NSKeyedUnarchiver",bytes);
        } else if (!strcmp(name,"NSNull")) { null_value.isa=cls; graph->decoded[index]=(Word)&null_value; }
        else {
            Word object=host_allocate(cls); graph->decoded[index]=object;
            Method initialize=find_implementation(object,"initWithCoder:");
            if (!initialize) stop("Class has no NSCoding initializer",name);
            graph->decoded[index]=initialize(object,"initWithCoder:",(Word)keyed_coder(graph,record),0,0,0);
        }
    }
    --graph->depth;
    return graph->decoded[index];
}
static struct ArchiveGraph *unarchive_graph(struct HostObject *data) {
    if (!data || data->content[0]>UINT32_MAX) return NULL;
    struct ArchiveGraph *graph=calloc(1,sizeof(*graph));
    if (!graph) stop("Allocation failed","NSKeyedUnarchiver");
    if (plist_from_memory((void *)data_buffer(data),(uint32_t)data->content[0],&graph->root,NULL)!=PLIST_ERR_SUCCESS || plist_get_node_type(graph->root)!=PLIST_DICT) stop("Invalid keyed archive data","NSKeyedUnarchiver");
    const char *format=plist_get_string_ptr(plist_dict_get_item(graph->root,"$archiver"),NULL);
    if (!format || strcmp(format,"NSKeyedArchiver")) stop("Unexpected archive format","NSKeyedUnarchiver");
    graph->objects=plist_dict_get_item(graph->root,"$objects"); graph->top=plist_dict_get_item(graph->root,"$top");
    if (plist_get_node_type(graph->objects)!=PLIST_ARRAY || plist_get_node_type(graph->top)!=PLIST_DICT) stop("Invalid keyed archive table","NSKeyedUnarchiver");
    graph->count=plist_array_get_size(graph->objects);
    if (!graph->count || graph->count>1000000) stop("Invalid keyed archive object count","NSKeyedUnarchiver");
    graph->decoded=calloc(graph->count,sizeof(Word)); graph->visited=calloc(graph->count,1); graph->decoding=1;
    if (!graph->decoded || !graph->visited) stop("Allocation failed","NSKeyedUnarchiver");
    return graph;
}
static Word unarchived_data(Word cls,const char *selector,struct HostObject *data) {
    struct ArchiveGraph *graph=unarchive_graph(data);
    return graph ? archive_decode(graph,plist_dict_get_item(graph->top,"root")) : 0;
}
static Word unarchived_file(Word cls,const char *selector,Word filename) { return unarchived_data(cls,selector,(void *)data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,filename)); }
static Word unarchiver_init(struct KeyedCoder *coder,const char *selector,struct HostObject *data) {
    coder->graph=unarchive_graph(data); if (!coder->graph) return 0;
    coder->container=coder->graph->top; return (Word)coder;
}
static Word coder_decode_object(struct KeyedCoder *coder,const char *selector,Word key) { return archive_decode(coder->graph,coder_get(coder,key)); }
static Word coder_decode_unkeyed(struct KeyedCoder *coder,const char *selector) { return coder_decode_object(coder,selector,0); }
static Word coder_decode_typed(struct KeyedCoder *coder,const char *selector,Word cls,Word key) {
    Word value=coder_decode_object(coder,selector,key);
    if (value && !is_kind_of(value,selector,cls)) stop("Unexpected decoded object class",selector);
    return value;
}
static Word coder_decode_classes(struct KeyedCoder *coder,const char *selector,struct Sequence *classes,Word key) {
    Word value=coder_decode_object(coder,selector,key);
    if (!value) return 0;
    for (size_t i=0;classes && i<classes->count;++i) if (is_kind_of(value,selector,classes->items[i])) return value;
    stop("Unexpected decoded object class",selector);
}
static Word coder_contains(struct KeyedCoder *coder,const char *selector,Word key) { return coder_get(coder,key)!=NULL; }
static Word coder_decode_integer(struct KeyedCoder *coder,const char *selector,Word key) {
    plist_t value=coder_get(coder,key);
    if (!value) return 0;
    if (plist_get_node_type(value)==PLIST_REAL || plist_get_node_type(value)==PLIST_BOOLEAN) return (Word)(int64_t)archive_real(value);
    int64_t number=0; plist_get_int_val(value,&number); return (Word)number;
}
static Word coder_decode_bool(struct KeyedCoder *coder,const char *selector,Word key) { return coder_decode_integer(coder,selector,key)!=0; }
static double coder_decode_double(struct KeyedCoder *coder,const char *selector,Word key) { return archive_real(coder_get(coder,key)); }
static float coder_decode_float(struct KeyedCoder *coder,const char *selector,Word key) { return (float)coder_decode_double(coder,selector,key); }
static Word coder_decode_bytes(struct KeyedCoder *coder,const char *selector,Word key,size_t *length) {
    uint64_t size=0; const char *bytes=plist_get_data_ptr(coder_get(coder,key),&size);
    if (length) *length=(size_t)size; return (Word)bytes;
}
static void coder_finish_decoding(struct KeyedCoder *coder) { coder->graph->finished=1; }
static Method keyed_archive_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    int encoding=!strcmp(receiver,"_OBJC_CLASS_$_NSKeyedArchiver"),decoding=!strcmp(receiver,"_OBJC_CLASS_$_NSKeyedUnarchiver");
    if (!encoding && !decoding) return NULL;
    if (class_receiver) {
        if (encoding && !strcmp(selector,"archivedDataWithRootObject:")) return (Method)archived_data;
        if (encoding && !strcmp(selector,"archiveRootObject:toFile:")) return (Method)archived_file;
        if (decoding && !strcmp(selector,"unarchiveObjectWithData:")) return (Method)unarchived_data;
        if (decoding && !strcmp(selector,"unarchiveObjectWithFile:")) return (Method)unarchived_file;
        return NULL;
    }
    if (!strcmp(selector,"allowsKeyedCoding")) return (Method)coder_yes;
    if (!strcmp(selector,"requiresSecureCoding")) return (Method)coder_no;
    if (!strcmp(selector,"setRequiresSecureCoding:")) return (Method)coder_secure;
    if (encoding) {
        if (!strcmp(selector,"initForWritingWithMutableData:")) return (Method)archiver_init;
        if (!strcmp(selector,"encodeObject:forKey:")) return (Method)coder_encode_object;
        if (!strcmp(selector,"encodeConditionalObject:forKey:")) return (Method)coder_encode_conditional;
        if (!strcmp(selector,"encodeObject:")) return (Method)coder_encode_unkeyed;
        if (!strcmp(selector,"encodeBool:forKey:")) return (Method)coder_encode_bool;
        if (!strcmp(selector,"encodeInt:forKey:") || !strcmp(selector,"encodeInt32:forKey:")) return (Method)coder_encode_int;
        if (!strcmp(selector,"encodeInt64:forKey:") || !strcmp(selector,"encodeInteger:forKey:")) return (Method)coder_encode_integer;
        if (!strcmp(selector,"encodeDouble:forKey:")) return (Method)coder_encode_double;
        if (!strcmp(selector,"encodeFloat:forKey:")) return (Method)coder_encode_float;
        if (!strcmp(selector,"encodeBytes:length:forKey:")) return (Method)coder_encode_bytes;
        if (!strcmp(selector,"finishEncoding")) return (Method)coder_finish_encoding;
    } else {
        if (!strcmp(selector,"initForReadingWithData:")) return (Method)unarchiver_init;
        if (!strcmp(selector,"decodeObjectForKey:")) return (Method)coder_decode_object;
        if (!strcmp(selector,"decodeObjectOfClass:forKey:")) return (Method)coder_decode_typed;
        if (!strcmp(selector,"decodeObjectOfClasses:forKey:")) return (Method)coder_decode_classes;
        if (!strcmp(selector,"decodeObject")) return (Method)coder_decode_unkeyed;
        if (!strcmp(selector,"containsValueForKey:")) return (Method)coder_contains;
        if (!strcmp(selector,"decodeBoolForKey:")) return (Method)coder_decode_bool;
        if (!strcmp(selector,"decodeIntForKey:") || !strcmp(selector,"decodeInt32ForKey:") || !strcmp(selector,"decodeInt64ForKey:") || !strcmp(selector,"decodeIntegerForKey:")) return (Method)coder_decode_integer;
        if (!strcmp(selector,"decodeDoubleForKey:")) return (Method)coder_decode_double;
        if (!strcmp(selector,"decodeFloatForKey:")) return (Method)coder_decode_float;
        if (!strcmp(selector,"decodeBytesForKey:returnedLength:")) return (Method)coder_decode_bytes;
        if (!strcmp(selector,"finishDecoding")) return (Method)coder_finish_decoding;
    }
    return NULL;
}
