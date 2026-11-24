static Word text_path_components(Word cls,const char *selector,struct Sequence *parts) {
    Word result=copy_string("");
    for (size_t i=0;parts && i<parts->count;++i) result=i ? append_path(result,selector,parts->items[i]) : text_copy(parts->items[i],"copy");
    return result;
}
static Word text_padding(Word object,const char *selector,Word length,Word padding,Word start) {
    struct TextView text=text_view(object,selector),pad=text_view(padding,selector);
    if (length>SIZE_MAX/2 || (length>text.length && (!pad.length || start>=pad.length))) stop("Invalid string padding",selector);
    uint16_t *units=length ? malloc(length*2) : NULL;
    if (length && !units) stop("Allocation failed",selector);
    size_t prefix=text.length<length ? text.length : length;
    if (prefix) memcpy(units,text.units,prefix*2);
    for (size_t i=prefix;i<length;++i) units[i]=pad.units[(start+i-prefix)%pad.length];
    Word result=text_from_units(external_class("_OBJC_CLASS_$_NSString"),units,length);
    free(units); free(text.owned); free(pad.owned); return result;
}
static Word text_init_nocopy(Word object,const char *selector,void *bytes,size_t length,Word encoding,unsigned char free_when_done) {
    Word result=text_decode(read_word(object),bytes,length,encoding);
    if (free_when_done) free(bytes); free((void *)object); return result;
}
static Word text_encoding_lossy(Word object,const char *selector,Word encoding,unsigned char lossy) { return encoded_string(object,selector,encoding); }
static Word text_write_file(Word object,const char *selector,Word path,unsigned char atomic,Word encoding,Word *error) {
    Word data=encoded_string(object,selector,encoding);
    if (!data) return file_failure(error,"String encoding",EINVAL);
    return data_write_file((void *)data,selector,path,atomic ? 1 : 0,error);
}
static Word text_write_url(Word object,const char *selector,Word url,unsigned char atomic,Word encoding,Word *error) {
    if (!url || !url_is_file((void *)url)) return file_failure(error,"Non-file string URL",EINVAL);
    return text_write_file(object,selector,url_path((void *)url),atomic,encoding,error);
}
static double text_double(Word object,const char *selector) { return strtod(string_bytes(object,selector),NULL); }
static float text_float(Word object,const char *selector) { return strtof(string_bytes(object,selector),NULL); }
static Word text_integer(Word object,const char *selector) {
    int64_t value=strtoll(string_bytes(object,selector),NULL,10);
    if (!strcmp(selector,"intValue")) return (Word)(int32_t)(value>INT32_MAX ? INT32_MAX : value<INT32_MIN ? INT32_MIN : value);
    return (Word)value;
}
static Word text_boolean(Word object,const char *selector) {
    const char *value=string_bytes(object,selector);
    while (*value==' ' || *value=='\t' || *value=='\n' || *value=='\r' || *value=='\f' || *value=='\v') ++value;
    if (*value=='+' || *value=='-') ++value;
    if (*value=='Y' || *value=='y' || *value=='T' || *value=='t') return 1;
    while (*value=='0') ++value;
    return *value>='1' && *value<='9';
}
static Word text_get_bytes(Word object,const char *selector,void *buffer,Word capacity,Word *used,Word encoding,Word options,struct GuestRange range,struct GuestRange *remaining) {
    if (options&~3UL) stop("Unsupported string conversion options",selector);
    struct TextView text=text_view(object,selector); check_range(range,text.length,selector);
    size_t position=range.location,end=position+range.length,written=0;
    unsigned char *output=buffer;
    if (!buffer) capacity=SIZE_MAX;
    int utf16=encoding==10 || encoding==0x90000100 || encoding==0x94000100,big=encoding==0x90000100;
    int utf32=encoding==0x8c000100 || encoding==0x98000100 || encoding==0x9c000100;
    if (encoding!=1 && encoding!=4 && encoding!=5 && !utf16 && !utf32) { free(text.owned); stop("Unsupported string byte encoding",selector); }
    if (utf16 && encoding==10 && (options&2) && capacity>=2) { if (output) { output[0]=0xff; output[1]=0xfe; } written=2; }
    if (utf32 && encoding==0x8c000100 && (options&2) && capacity>=4) { if (output) { output[0]=0xff; output[1]=0xfe; output[2]=output[3]=0; } written=4; }
    while (position<end) {
        unsigned scalar=text.units[position]; size_t units=1;
        if (scalar>=0xd800 && scalar<=0xdbff && position+1<end && text.units[position+1]>=0xdc00 && text.units[position+1]<=0xdfff) {
            scalar=0x10000+((scalar-0xd800)<<10)+(text.units[position+1]-0xdc00); units=2;
        } else if (scalar>=0xd800 && scalar<=0xdfff && !utf16) {
            if (!(options&1)) break; scalar=0xfffd;
        }
        unsigned char bytes[4]; size_t count=0;
        if (utf16) {
            for (size_t i=0;i<units;++i) { uint16_t unit=text.units[position+i]; bytes[count++]=(unsigned char)(big ? unit>>8 : unit); bytes[count++]=(unsigned char)(big ? unit : unit>>8); }
        } else if (utf32) {
            for (unsigned i=0;i<4;++i) bytes[count++]=(unsigned char)(scalar>>((encoding==0x98000100 ? 3-i : i)*8));
        } else if (encoding==1 || encoding==5) {
            if (scalar>(encoding==1 ? 127U : 255U)) { if (!(options&1)) break; scalar='?'; }
            bytes[count++]=(unsigned char)scalar;
        } else if (scalar<0x80) bytes[count++]=(unsigned char)scalar;
        else if (scalar<0x800) { bytes[count++]=0xc0|(scalar>>6); bytes[count++]=0x80|(scalar&63); }
        else if (scalar<0x10000) { bytes[count++]=0xe0|(scalar>>12); bytes[count++]=0x80|((scalar>>6)&63); bytes[count++]=0x80|(scalar&63); }
        else { bytes[count++]=0xf0|(scalar>>18); bytes[count++]=0x80|((scalar>>12)&63); bytes[count++]=0x80|((scalar>>6)&63); bytes[count++]=0x80|(scalar&63); }
        if (count>capacity-written) break;
        if (output) memcpy(output+written,bytes,count);
        written+=count; position+=units;
    }
    if (used) *used=written;
    if (remaining) *remaining=(struct GuestRange){position,end-position};
    free(text.owned); return position>range.location || !range.length;
}
static Word text_replace_occurrences(Word object,const char *selector,Word needle,Word replacement,Word options,struct GuestRange range) {
    check_range(range,string_length(object,selector),selector);
    size_t limit=range.location+range.length,position=range.location,count=0,replacement_length=string_length(replacement,selector);
    if (!string_length(needle,selector)) return 0;
    if (options&4) stop("Backwards mutable replacement is not implemented",selector);
    while (position<limit) {
        struct GuestRange found=text_find_range(object,selector,needle,options,(struct GuestRange){position,limit-position});
        if (found.location==INT64_MAX) break;
        text_mutate_range(object,selector,found,replacement);
        limit=limit-found.length+replacement_length; position=found.location+replacement_length; ++count;
    }
    return count;
}
static Word data_from_data(Word cls,const char *selector,struct HostObject *data) { return data ? data_with_bytes(cls,selector,(void *)data_buffer(data),data->content[0]) : data_empty(cls); }
static Word data_init_data(Word object,const char *selector,struct HostObject *data) { Word result=data_from_data(read_word(object),selector,data); free((void *)object); return result; }
static Word data_nocopy(Word cls,const char *selector,void *bytes,size_t length,unsigned char free_when_done) {
    if (length && !bytes) stop("Nil data buffer",selector);
    if (cls==external_class("_OBJC_CLASS_$_NSMutableData")) {
        Word result=data_with_bytes(cls,selector,bytes,length); if (free_when_done) free(bytes); return result;
    }
    struct HostObject *data=(void *)host_allocate(cls);
    data->content[0]=length; data->content[1]=(Word)bytes; data->content[2]=free_when_done; data->content[3]=0x4e444154;
    return (Word)data;
}
static Word data_nocopy_basic(Word cls,const char *selector,void *bytes,size_t length) { return data_nocopy(cls,selector,bytes,length,1); }
static Word data_init_nocopy(Word object,const char *selector,void *bytes,size_t length,unsigned char free_when_done) {
    Word result=data_nocopy(read_word(object),selector,bytes,length,free_when_done); free((void *)object); return result;
}
static Word data_base64(struct HostObject *data,const char *selector,Word options) {
    if (options&~0x33UL || data->content[0]>(SIZE_MAX-8)/4*3) stop("Unsupported base64 encoding options",selector);
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *input=(void *)data_buffer(data); size_t size=data->content[0],line=(options&1) ? 64 : (options&2) ? 76 : 0;
    size_t count=(size+2)/3*4; char *bytes=malloc(count+(line ? count/line*2+2 : 0)+1);
    if (!bytes) stop("Allocation failed",selector);
    size_t at=0,column=0;
    for (size_t i=0;i<size;i+=3) {
        if (line && column==line) { if (!(options&0x30) || (options&0x10)) bytes[at++]='\r'; if (!(options&0x30) || (options&0x20)) bytes[at++]='\n'; column=0; }
        unsigned value=input[i]<<16; if (i+1<size) value|=input[i+1]<<8; if (i+2<size) value|=input[i+2];
        bytes[at++]=alphabet[value>>18]; bytes[at++]=alphabet[(value>>12)&63];
        bytes[at++]=i+1<size ? alphabet[(value>>6)&63] : '='; bytes[at++]=i+2<size ? alphabet[value&63] : '='; column+=4;
    }
    bytes[at]=0; Word result=copy_string(bytes); free(bytes); return result;
}
static Word date_relative(Word object,const char *selector,double interval,Word other) {
    Word cls=host_symbol(object) ? object : read_word(object);
    double base=strstr(selector,"SinceNow") ? scheduling_now() : strstr(selector,"ReferenceDate") ? 978307200.0 :
        strstr(selector,"sinceDate:") ? date_seconds(other) : !strcmp(selector,"dateByAddingTimeInterval:") ? date_seconds(object) : 0;
    Word result=date_epoch(cls,selector,base+interval);
    if (!strncmp(selector,"init",4)) free((void *)object);
    return result;
}
static Word date_relative_now(Word object,const char *selector,double interval) { return date_relative(object,selector,interval,0); }
static double date_difference(Word object,const char *selector,Word other) { return date_seconds(object)-(other ? date_seconds(other) : scheduling_now()); }
static double date_difference_now(Word object,const char *selector) { return date_difference(object,selector,0); }
static Word process_activity_tag;
static int process_activity_command(Word token,Word options,int begin) {
    JNIEnv *env=attached_environment("Process activity");static jmethodID command;jmethodID method=__atomic_load_n(&command,__ATOMIC_ACQUIRE);
    if(!method){jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"processActivity","(JJZ)Z");(*env)->DeleteLocalRef(env,cls);__atomic_store_n(&command,method,__ATOMIC_RELEASE);}
    int result=(*env)->CallBooleanMethod(env,activity,method,(jlong)token,(jlong)options,(jboolean)begin);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);return 0;}return result;
}
static Word process_begin_activity(Word object,const char *selector,Word options,Word reason) {
    if(!reason)stop("Missing process activity reason",selector);string_bytes(reason,selector);
    struct HostObject *token=(void *)host_allocate(external_class("_OBJC_CLASS_$_NSObject"));token->content[0]=options;token->content[3]=(Word)&process_activity_tag;
    if(!process_activity_command((Word)token,options,1)){free(token);return 0;}return (Word)token;
}
static void process_end_activity(Word object,const char *selector,struct HostObject *token) {
    if(!token || token->isa!=external_class("_OBJC_CLASS_$_NSObject") || token->content[3]!=(Word)&process_activity_tag)stop("Invalid process activity token",selector);
    if(__atomic_exchange_n(&token->content[1],1,__ATOMIC_ACQ_REL))return;
    if(!process_activity_command((Word)token,token->content[0],0))stop("Process activity release failed",selector);
}
static Word number_init_integer(Word object,const char *selector,Word value) { Word result=number_integer(value); free((void *)object); return result; }
static Word number_unsigned_byte(struct HostObject *object) { return numeric_integer(object)&255; }
static void array_subscript_set(struct Sequence *array,const char *selector,Word value,Word index) {
    if (index==array->count) sequence_add(array,selector,value); else collection_replace(array,selector,index,value);
}
static Word collection_init_capacity(Word object,const char *selector,Word capacity) {
    sequence_reserve((void *)object,capacity,selector); return object;
}
static Word mutable_string_capacity(Word object,const char *selector,Word capacity) { return text_init_empty(object); }
static void set_union(struct Sequence *set,const char *selector,struct Sequence *other) { if (set!=other) collection_add_array(set,selector,other); }
static void set_minus(struct Sequence *set,const char *selector,struct Sequence *other) {
    if (set==other) { set->count=0; ++set->mutation; return; }
    for (size_t i=0;i<other->count;++i) sequence_remove(set,selector,other->items[i]);
}
static Word set_subset(struct Sequence *set,const char *selector,struct Sequence *other) {
    for (size_t i=0;i<set->count;++i) if (!sequence_contains(other,selector,set->items[i])) return 0;
    return 1;
}
static void dictionary_remove_keys(struct GuestMap *map,const char *selector,struct Sequence *keys) {
    for (size_t i=0;i<keys->count;++i) dictionary_remove(map,selector,keys->items[i]);
}
static Word dictionary_init_copy_items(Word object,const char *selector,struct GuestMap *source,unsigned char copy_items) {
    Word result=dictionary_init_dictionary(object,selector,source);
    if (copy_items) {
        struct GuestMap *map=(void *)result;
        for (size_t i=0;i<map->count;++i) {
            Method copy=find_implementation(map->entries[i].value,"copy"); if (!copy) stop("Dictionary value cannot be copied",selector);
            map->entries[i].value=copy(map->entries[i].value,"copy",0,0,0,0);
        }
    }
    return result;
}
static Word url_init_file(Word object,const char *selector,Word path) { Word result=file_url(path); free((void *)object); return result; }
static Word checked_bundle_path(Word cls,const char *selector,Word path) {
    unsigned char directory=0;
    return path && manager_exists((Word)&file_manager,selector,path,&directory) && directory ? bundle_at_path(cls,selector,path) : 0;
}
static Word bundle_init_path(Word object,const char *selector,Word path) { Word result=checked_bundle_path(read_word(object),selector,path); free((void *)object); return result; }
static Word bundle_init_url(Word object,const char *selector,Word url) { return bundle_init_path(object,selector,url ? url_path((void *)url) : 0); }
static Word property_list_decode(Word cls,const char *selector,struct HostObject *data,Word options,Word *format,Word *error) {
    if (error) *error=0;
    if (!data || data->content[0]>UINT32_MAX || options>2) return serialization_error(error,"Invalid property list input");
    plist_t root=NULL; plist_format_t type=PLIST_FORMAT_NONE;
    if (plist_from_memory((void *)data_buffer(data),(uint32_t)data->content[0],&root,&type)!=PLIST_ERR_SUCCESS) return serialization_error(error,"Invalid property list");
    if (type!=PLIST_FORMAT_XML && type!=PLIST_FORMAT_BINARY) { plist_free(root); return serialization_error(error,"Unsupported property list format"); }
    if (format) *format=type==PLIST_FORMAT_BINARY ? 200 : 100;
    Word result=plist_object(root,0); plist_free(root);
    if (options) json_mutability(result,options==2 ? 3 : 1);
    return result;
}
static Word property_list_encode(Word cls,const char *selector,Word object,Word format,Word options,Word *error) {
    if (error) *error=0;
    if (options || (format!=100 && format!=200)) return serialization_error(error,"Unsupported property list format/options");
    Word ancestors[64]; plist_t root=object_property_list(object,ancestors,0);
    if (!root) return serialization_error(error,"Object cannot be represented as a property list");
    char *bytes=NULL; uint32_t size=0;
    int valid=(format==200 ? plist_to_bin(root,&bytes,&size) : plist_to_xml(root,&bytes,&size))==PLIST_ERR_SUCCESS;
    plist_free(root);
    Word result=valid ? data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),selector,bytes,size) : serialization_error(error,"Property list serialization failed");
    free(bytes); return result;
}
static Word property_list_valid(Word cls,const char *selector,Word object,Word format) {
    if (format!=100 && format!=200) return 0;
    Word ancestors[64]; plist_t root=object_property_list(object,ancestors,0); int valid=root!=NULL; plist_free(root); return valid;
}
static Word collection_write_file(Word object,const char *selector,Word path,unsigned char atomic) {
    Word data=property_list_encode(0,selector,object,100,0,NULL);
    return data ? data_write_file((void *)data,selector,path,atomic ? 1 : 0,NULL) : 0;
}
static Word collection_write_url(Word object,const char *selector,Word url,unsigned char atomic) {
    return url && url_is_file((void *)url) ? collection_write_file(object,selector,url_path((void *)url),atomic) : 0;
}
static Word collection_file(Word cls,const char *selector,Word path) {
    Word data=data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,path);
    Word result=data ? property_list_decode(0,selector,(void *)data,0,NULL,NULL) : 0;
    if (!result) return 0;
    int array=!strcmp(class_name(cls),"_OBJC_CLASS_$_NSArray") || !strcmp(class_name(cls),"_OBJC_CLASS_$_NSMutableArray");
    if (read_word(result)!=external_class(array ? "_OBJC_CLASS_$_NSArray" : "_OBJC_CLASS_$_NSDictionary")) return 0;
    *(Word *)result=cls; return result;
}
static Word collection_init_file(Word object,const char *selector,Word path) { Word result=collection_file(read_word(object),selector,path); free((void *)object); return result; }
static Word collection_url(Word cls,const char *selector,Word url) { return url && url_is_file((void *)url) ? collection_file(cls,selector,url_path((void *)url)) : 0; }
static Word collection_init_url(Word object,const char *selector,Word url) { Word result=collection_url(read_word(object),selector,url); free((void *)object); return result; }
static Word method_for_selector(Word object,const char *selector,const char *action) { return (Word)find_implementation(object,action); }
static Word instance_method_for_selector(Word cls,const char *selector,const char *action) {
    Word prototype=cls;
    return action ? (Word)find_implementation((Word)&prototype,action) : 0;
}
static Method foundation_value_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if(receiver && !class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSProcessInfo")){
        if(!strcmp(selector,"beginActivityWithOptions:reason:"))return (Method)process_begin_activity;
        if(!strcmp(selector,"endActivity:"))return (Method)process_end_activity;
    }
    if (!strcmp(selector,"isEqual:") || !strcmp(selector,"isEqualToDictionary:") || !strcmp(selector,"isEqualToArray:")) return (Method)equal_objects;
    if (!strcmp(selector,"methodForSelector:")) return (Method)method_for_selector;
    if ((class_receiver || guest_class_object(object)) && !strcmp(selector,"instanceMethodForSelector:")) return (Method)instance_method_for_selector;
    if (!receiver) return NULL;
    int plist_collection=!strcmp(receiver,"_OBJC_CLASS_$_NSDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSArray") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray");
    if (plist_collection) {
        if (class_receiver && (!strcmp(selector,"dictionaryWithContentsOfFile:") || !strcmp(selector,"arrayWithContentsOfFile:"))) return (Method)collection_file;
        if (class_receiver && (!strcmp(selector,"dictionaryWithContentsOfURL:") || !strcmp(selector,"arrayWithContentsOfURL:"))) return (Method)collection_url;
        if (!class_receiver) {
            if (!strcmp(selector,"writeToFile:atomically:")) return (Method)collection_write_file;
            if (!strcmp(selector,"writeToURL:atomically:")) return (Method)collection_write_url;
            if (!strcmp(selector,"initWithContentsOfFile:")) return (Method)collection_init_file;
            if (!strcmp(selector,"initWithContentsOfURL:")) return (Method)collection_init_url;
        }
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString")) {
        if (class_receiver && !strcmp(selector,"pathWithComponents:")) return (Method)text_path_components;
        if (!class_receiver) {
            if (!strcmp(selector,"stringByPaddingToLength:withString:startingAtIndex:")) return (Method)text_padding;
            if (!strcmp(selector,"initWithBytesNoCopy:length:encoding:freeWhenDone:")) return (Method)text_init_nocopy;
            if (!strcmp(selector,"dataUsingEncoding:allowLossyConversion:")) return (Method)text_encoding_lossy;
            if (!strcmp(selector,"getBytes:maxLength:usedLength:encoding:options:range:remainingRange:")) return (Method)text_get_bytes;
            if (!strcmp(selector,"writeToFile:atomically:encoding:error:")) return (Method)text_write_file;
            if (!strcmp(selector,"writeToURL:atomically:encoding:error:")) return (Method)text_write_url;
            if (!strcmp(selector,"doubleValue")) return (Method)text_double;
            if (!strcmp(selector,"floatValue")) return (Method)text_float;
            if (!strcmp(selector,"intValue") || !strcmp(selector,"integerValue") || !strcmp(selector,"longLongValue") || !strcmp(selector,"longValue")) return (Method)text_integer;
            if (!strcmp(selector,"boolValue")) return (Method)text_boolean;
            if (!strcmp(selector,"stringByReplacingOccurrencesOfString:withString:")) return (Method)replace_string;
            if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableString") && !strcmp(selector,"replaceOccurrencesOfString:withString:options:range:")) return (Method)text_replace_occurrences;
            if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableString") && !strcmp(selector,"initWithCapacity:")) return (Method)mutable_string_capacity;
        }
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSData") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableData")) {
        if (class_receiver) {
            if (!strcmp(selector,"dataWithData:")) return (Method)data_from_data;
            if (!strcmp(selector,"dataWithBytesNoCopy:length:")) return (Method)data_nocopy_basic;
            if (!strcmp(selector,"dataWithBytesNoCopy:length:freeWhenDone:")) return (Method)data_nocopy;
        } else {
            if (!strcmp(selector,"initWithData:")) return (Method)data_init_data;
            if (!strcmp(selector,"initWithBytesNoCopy:length:freeWhenDone:")) return (Method)data_init_nocopy;
            if (!strcmp(selector,"base64EncodedStringWithOptions:")) return (Method)data_base64;
        }
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSDate")) {
        if (class_receiver && (!strcmp(selector,"dateWithTimeIntervalSinceNow:") || !strcmp(selector,"dateWithTimeIntervalSinceReferenceDate:"))) return (Method)date_relative_now;
        if (!class_receiver) {
            if (!strcmp(selector,"initWithTimeIntervalSince1970:") || !strcmp(selector,"initWithTimeIntervalSinceReferenceDate:") || !strcmp(selector,"dateByAddingTimeInterval:")) return (Method)date_relative_now;
            if (!strcmp(selector,"initWithTimeInterval:sinceDate:")) return (Method)date_relative;
            if (!strcmp(selector,"timeIntervalSinceDate:")) return (Method)date_difference;
            if (!strcmp(selector,"timeIntervalSinceNow")) return (Method)date_difference_now;
        }
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSNumber")) {
        if (!strcmp(selector,"initWithInteger:")) return (Method)number_init_integer;
        if (!strcmp(selector,"unsignedCharValue")) return (Method)number_unsigned_byte;
        if (!strcmp(selector,"isEqualToNumber:")) return (Method)equal_objects;
        if (!strcmp(selector,"longValue")) return (Method)numeric_integer;
    }
    if (!class_receiver && !strcmp(selector,"initWithCapacity:")) {
        if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableSet")) return (Method)collection_init_capacity;
        if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary")) return (Method)identity;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray") && !strcmp(selector,"setObject:atIndexedSubscript:")) return (Method)array_subscript_set;
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSSet") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableSet"))) {
        if (!strcmp(selector,"isSubsetOfSet:")) return (Method)set_subset;
        if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableSet")) {
            if (!strcmp(selector,"unionSet:")) return (Method)set_union;
            if (!strcmp(selector,"minusSet:")) return (Method)set_minus;
        }
    }
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary"))) {
        if (!strcmp(selector,"initWithDictionary:copyItems:")) return (Method)dictionary_init_copy_items;
        if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary") && !strcmp(selector,"removeObjectsForKeys:")) return (Method)dictionary_remove_keys;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSURL") && !strcmp(selector,"initFileURLWithPath:")) return (Method)url_init_file;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSBundle")) {
        if (class_receiver && !strcmp(selector,"bundleWithPath:")) return (Method)checked_bundle_path;
        if (!class_receiver && !strcmp(selector,"initWithPath:")) return (Method)bundle_init_path;
        if (!class_receiver && !strcmp(selector,"initWithURL:")) return (Method)bundle_init_url;
    }
    if (class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSPropertyListSerialization")) {
        if (!strcmp(selector,"propertyListWithData:options:format:error:")) return (Method)property_list_decode;
        if (!strcmp(selector,"dataWithPropertyList:format:options:error:")) return (Method)property_list_encode;
        if (!strcmp(selector,"propertyList:isValidForFormat:")) return (Method)property_list_valid;
    }
    return NULL;
}

static Word foundation_string_hash(Word object,const char *selector) {
    struct TextView text=text_view(object,selector); Word hash=14695981039346656037ULL;
    for (size_t i=0;i<text.length;++i) { hash^=text.units[i]; hash*=1099511628211ULL; }
    free(text.owned); return hash;
}
static Word index_path_create(Word cls,const char *selector,Word item,Word section) {
    Word indices[]={section,item}; return new_array(cls,selector,indices,2);
}
static Word index_path_indices(Word cls,const char *selector,const Word *indices,Word count) { return new_array(cls,selector,indices,count); }
static Word index_path_component(struct Sequence *self,const char *selector) {
    return array_at(self,selector,!strcmp(selector,"section") ? 0 : 1);
}
static Word index_path_copy(struct Sequence *self,const char *selector) { return new_array(self->isa,selector,self->items,self->count); }
static Word index_path_equal(struct Sequence *self,const char *selector,struct Sequence *other) {
    return other && other->isa==self->isa && self->count==other->count && !memcmp(self->items,other->items,self->count*sizeof(Word));
}
static Word index_path_compare(struct Sequence *self,const char *selector,struct Sequence *other) {
    if (!other) stop("Nil index path",selector);
    for (size_t i=0;i<self->count && i<other->count;++i) if (self->items[i]!=other->items[i]) return (Word)(intptr_t)(self->items[i]<other->items[i] ? -1 : 1);
    return (Word)(intptr_t)(self->count<other->count ? -1 : self->count>other->count);
}
static Word ordered_copy(struct Sequence *self,const char *selector) {
    Word cls=external_class(!strcmp(selector,"mutableCopy") ? "_OBJC_CLASS_$_NSMutableOrderedSet" : "_OBJC_CLASS_$_NSOrderedSet");
    return new_array(cls,selector,self->items,self->count);
}
static void ordered_add(struct Sequence *self,const char *selector,Word value) {
    if (!sequence_contains(self,selector,value)) sequence_add(self,selector,value);
}
static void ordered_insert(struct Sequence *self,const char *selector,Word value,Word index) {
    if (index>self->count) stop("Invalid ordered set index",selector);
    if (!sequence_contains(self,selector,value)) sequence_insert(self,selector,value,index);
}
static Word ordered_array(Word object,const char *selector,struct Sequence *array) {
    int initialize=!strncmp(selector,"init",4); Word cls=initialize ? read_word(object) : object;
    struct Sequence *result=(void *)sequence_empty(cls);
    if (array) for (size_t i=0;i<array->count;++i) ordered_add(result,selector,array->items[i]);
    if (initialize) free((void *)object); return (Word)result;
}
static Word file_handle_init(struct HostObject *self,const char *selector,int descriptor,unsigned char close_on_dealloc) {
    if (descriptor<0 || fcntl(descriptor,F_GETFD)<0) { free(self); return 0; }
    self->content[0]=(Word)descriptor+1; self->content[1]=close_on_dealloc; return (Word)self;
}
static Word file_handle_standard(Word cls,const char *selector) {
    int descriptor=!strcmp(selector,"fileHandleWithStandardInput") ? 0 : !strcmp(selector,"fileHandleWithStandardOutput") ? 1 : 2;
    return file_handle_init((void *)host_allocate(cls),selector,descriptor,0);
}
static Word file_handle_path(Word cls,const char *selector,Word filename) {
    int mode=strstr(selector,"Updating") ? O_RDWR : strstr(selector,"Writing") ? O_WRONLY : O_RDONLY;
    int descriptor=io_open(string_bytes(filename,selector),mode|O_CLOEXEC,0);
    if (descriptor<0) return 0;
    return file_handle_init((void *)host_allocate(cls),selector,descriptor,1);
}
static int file_handle_descriptor(struct HostObject *self,const char *selector) {
    if (!self->content[0]) stop("Closed file handle",selector); return (int)(self->content[0]-1);
}
static void file_handle_write(struct HostObject *self,const char *selector,struct HostObject *data) {
    if (!data) stop("Nil file data",selector);
    int descriptor=file_handle_descriptor(self,selector); const unsigned char *bytes=(void *)data_buffer(data); size_t left=data->content[0];
    while (left) {
        ssize_t count=write(descriptor,bytes,left>SSIZE_MAX ? SSIZE_MAX : left);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) stop("File handle write failed",strerror(errno));
        bytes+=count; left-=count;
    }
}
static void file_handle_close(struct HostObject *self) {
    Word value=__atomic_exchange_n(&self->content[0],0,__ATOMIC_ACQ_REL); if (value) io_close((int)(value-1));
}
static Word file_handle_seek(struct HostObject *self,const char *selector,uint64_t offset) {
    int end=!strcmp(selector,"seekToEndOfFile"),current=!strcmp(selector,"offsetInFile");
    if (!end && !current && offset>INT64_MAX) stop("Invalid file offset",selector);
    off_t result=io_seek(file_handle_descriptor(self,selector),end || current ? 0 : (off_t)offset,end ? SEEK_END : current ? SEEK_CUR : SEEK_SET);
    if (result<0) stop("File handle seek failed",strerror(errno)); return (Word)result;
}
static void file_handle_sync(struct HostObject *self,const char *selector) { if (fsync(file_handle_descriptor(self,selector))) stop("File handle sync failed",strerror(errno)); }
static Word file_handle_url(Word cls,const char *selector,Word url,Word *error) {
    if (error) *error=0;
    if (!url || !url_is_file((void *)url)) return file_failure(error,"File handle URL",EINVAL);
    const char *path_text=string_bytes(url_path((void *)url),selector);
    int mode=strstr(selector,"Updating") ? O_RDWR : strstr(selector,"Writing") ? O_WRONLY : O_RDONLY;
    int descriptor=io_open(path_text,mode|O_CLOEXEC,0),code=errno;
    return descriptor<0 ? file_failure(error,path_text,code) : file_handle_init((void *)host_allocate(cls),selector,descriptor,1);
}
static Word file_handle_read(struct HostObject *self,const char *selector,Word amount) {
    int descriptor=file_handle_descriptor(self,selector),all=!strcmp(selector,"readDataToEndOfFile"),available=!strcmp(selector,"availableData");
    if (available) amount=8192;
    if (!all && amount>SIZE_MAX-sizeof(struct HostObject)) stop("Read size too large",selector);
    size_t capacity=all ? 8192 : amount,used=0; unsigned char *bytes=capacity ? malloc(capacity) : NULL;
    if (capacity && !bytes) stop("Allocation failed",selector);
    for (;;) {
        if (used==capacity) {
            if (!all) break;
            if (capacity>SIZE_MAX/2) { free(bytes); stop("File data too large",selector); }
            size_t next=capacity*2; void *grown=realloc(bytes,next); if (!grown) { free(bytes); stop("Allocation failed",selector); } bytes=grown; capacity=next;
        }
        size_t wanted=capacity-used; if (wanted>SSIZE_MAX) wanted=SSIZE_MAX;
        ssize_t count=io_read(descriptor,bytes+used,wanted);
        if (count<0 && errno==EINTR) continue;
        if (count<0) { int code=errno; free(bytes); stop("File handle read failed",strerror(code)); }
        if (!count) break; used+=(size_t)count; if (available) break;
    }
    Word data=data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),selector,bytes,used); free(bytes); return data;
}
static void file_handle_truncate(struct HostObject *self,const char *selector,uint64_t size) { if (size>INT64_MAX || ftruncate(file_handle_descriptor(self,selector),(off_t)size)) stop("File handle truncate failed",size>INT64_MAX ? "Invalid size" : strerror(errno)); }
static Word file_handle_init_plain(struct HostObject *self,const char *selector,int descriptor) { return file_handle_init(self,selector,descriptor,0); }
static void file_handle_dealloc(struct HostObject *self) { if (self->content[1]) file_handle_close(self); free(self); }
static Word date_equal(Word object,const char *selector,Word other) { return other && class_inherits(read_word(other),external_class("_OBJC_CLASS_$_NSDate")) && date_seconds(object)==date_seconds(other); }
static Word date_hash(Word object) { union { double value; uint64_t bits; } seconds={.value=date_seconds(object)}; if (!seconds.value) seconds.bits=0; return seconds.bits^(seconds.bits>>32); }
static Word object_enumerator(Word,const char *);
static Word dictionary_enumerator(Word object,const char *selector) { Word array=!strcmp(selector,"keyEnumerator") ? map_keys((void *)object,selector) : dictionary_values((void *)object,selector); return object_enumerator(array,"objectEnumerator"); }
static void collection_perform_objects(Word object,const char *selector,const char *action,Word argument) {
    struct Sequence *array=(void *)object; struct Sequence *snapshot=(void *)sequence_copy(array,"copy");
    struct GuestCallBoundary scoped={.previous=active_guest_call_boundary}; active_guest_call_boundary=&scoped;
    int failed=setjmp(scoped.environment);
    if (!failed) for (size_t i=0;i<snapshot->count;++i) { Method method=find_implementation(snapshot->items[i],action); if (!method) stop("Unrecognized collection selector",action); method(snapshot->items[i],action,argument,0,0,0); }
    active_guest_call_boundary=scoped.previous; free(snapshot->items); free(snapshot);
    if (failed) { char failure[sizeof(status)]; memcpy(failure,status,sizeof(failure)); stop("Collection selector failed",failure); }
}
static void collection_perform_plain(Word object,const char *selector,const char *action) { collection_perform_objects(object,selector,action,0); }
static Word url_hash(Word object,const char *selector) { return foundation_string_hash(url_absolute((void *)object,selector),selector); }
static Word thread_callstack(Word cls,const char *selector) {
    void *addresses[128]; int count=darwin_backtrace(addresses,COUNT(addresses));
    int symbols=!strcmp(selector,"callStackSymbols"); char **names=symbols ? darwin_backtrace_symbols(addresses,count) : NULL;
    struct Sequence *result=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    for (int i=0;i<count;++i) sequence_add(result,selector,symbols ? copy_string(names ? names[i] : "?") : number_integer((Word)addresses[i]));
    free(names); return (Word)result;
}
static Word process_identifier(void) { return getpid(); }
static Word process_name(void) { return copy_string(GUEST_APP_NAME); }
static Method foundation_collection_io_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSDate")) {
        if (!strcmp(selector,"isEqualToDate:")) return (Method)date_equal;
        if (!strcmp(selector,"hash")) return (Method)date_hash;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSURL") && !strcmp(selector,"hash")) return (Method)url_hash;
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary")) && (!strcmp(selector,"keyEnumerator") || !strcmp(selector,"objectEnumerator"))) return (Method)dictionary_enumerator;
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSArray") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray"))) {
        if (!strcmp(selector,"makeObjectsPerformSelector:")) return (Method)collection_perform_plain;
        if (!strcmp(selector,"makeObjectsPerformSelector:withObject:")) return (Method)collection_perform_objects;
    }
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString")) && !strcmp(selector,"hash")) return (Method)foundation_string_hash;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSIndexPath")) {
        if (class_receiver) {
            if (!strcmp(selector,"indexPathForRow:inSection:") || !strcmp(selector,"indexPathForItem:inSection:")) return (Method)index_path_create;
            if (!strcmp(selector,"indexPathWithIndexes:length:")) return (Method)index_path_indices;
        } else {
            if (!strcmp(selector,"row") || !strcmp(selector,"item") || !strcmp(selector,"section")) return (Method)index_path_component;
            if (!strcmp(selector,"length")) return (Method)array_count;
            if (!strcmp(selector,"indexAtPosition:")) return (Method)array_at;
            if (!strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:")) return (Method)index_path_copy;
            if (!strcmp(selector,"isEqual:")) return (Method)index_path_equal;
            if (!strcmp(selector,"compare:")) return (Method)index_path_compare;
        }
    }
    int mutable=!strcmp(receiver,"_OBJC_CLASS_$_NSMutableOrderedSet");
    if (mutable || !strcmp(receiver,"_OBJC_CLASS_$_NSOrderedSet")) {
        if (class_receiver) {
            if (!strcmp(selector,"orderedSet") || !strcmp(selector,"orderedSetWithCapacity:")) return (Method)sequence_empty;
            if (!strcmp(selector,"orderedSetWithArray:")) return (Method)ordered_array;
        } else {
            if (!strcmp(selector,"init") || !strcmp(selector,"initWithCapacity:")) return (Method)identity;
            if (!strcmp(selector,"initWithArray:")) return (Method)ordered_array;
            if (!strcmp(selector,"count")) return (Method)array_count;
            if (!strcmp(selector,"objectAtIndex:") || !strcmp(selector,"objectAtIndexedSubscript:")) return (Method)array_at;
            if (!strcmp(selector,"firstObject")) return (Method)array_first;
            if (!strcmp(selector,"lastObject")) return (Method)array_last;
            if (!strcmp(selector,"containsObject:")) return (Method)sequence_contains;
            if (!strcmp(selector,"indexOfObject:")) return (Method)sequence_index;
            if (!strcmp(selector,"array")) return (Method)set_all;
            if (!strcmp(selector,"copy") || !strcmp(selector,"mutableCopy")) return (Method)ordered_copy;
            if (!strcmp(selector,"countByEnumeratingWithState:objects:count:")) return (Method)array_enumerate;
            if (mutable) {
                if (!strcmp(selector,"addObject:")) return (Method)ordered_add;
                if (!strcmp(selector,"insertObject:atIndex:")) return (Method)ordered_insert;
                if (!strcmp(selector,"removeObject:")) return (Method)sequence_remove;
                if (!strcmp(selector,"removeObjectAtIndex:")) return (Method)sequence_remove_at;
                if (!strcmp(selector,"removeAllObjects")) return (Method)sequence_clear;
            }
        }
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSFileHandle")) {
        if (class_receiver) {
            if (!strcmp(selector,"fileHandleForReadingFromURL:error:") || !strcmp(selector,"fileHandleForWritingToURL:error:") || !strcmp(selector,"fileHandleForUpdatingURL:error:")) return (Method)file_handle_url;
            if (!strcmp(selector,"fileHandleWithStandardInput") || !strcmp(selector,"fileHandleWithStandardOutput") || !strcmp(selector,"fileHandleWithStandardError")) return (Method)file_handle_standard;
            if (!strcmp(selector,"fileHandleForReadingAtPath:") || !strcmp(selector,"fileHandleForWritingAtPath:") || !strcmp(selector,"fileHandleForUpdatingAtPath:")) return (Method)file_handle_path;
        } else {
            if (!strcmp(selector,"initWithFileDescriptor:")) return (Method)file_handle_init_plain;
            if (!strcmp(selector,"readDataOfLength:") || !strcmp(selector,"readDataToEndOfFile") || !strcmp(selector,"availableData")) return (Method)file_handle_read;
            if (!strcmp(selector,"truncateFileAtOffset:")) return (Method)file_handle_truncate;
            if (!strcmp(selector,"dealloc")) return (Method)file_handle_dealloc;
            if (!strcmp(selector,"initWithFileDescriptor:closeOnDealloc:")) return (Method)file_handle_init;
            if (!strcmp(selector,"fileDescriptor")) return (Method)file_handle_descriptor;
            if (!strcmp(selector,"writeData:")) return (Method)file_handle_write;
            if (!strcmp(selector,"closeFile")) return (Method)file_handle_close;
            if (!strcmp(selector,"seekToEndOfFile") || !strcmp(selector,"seekToFileOffset:") || !strcmp(selector,"offsetInFile")) return (Method)file_handle_seek;
            if (!strcmp(selector,"synchronizeFile")) return (Method)file_handle_sync;
        }
    }
    if (class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSThread") && (!strcmp(selector,"callStackSymbols") || !strcmp(selector,"callStackReturnAddresses"))) return (Method)thread_callstack;
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSProcessInfo")) {
        if (!strcmp(selector,"processIdentifier")) return (Method)process_identifier;
        if (!strcmp(selector,"processName")) return (Method)process_name;
    }
    return NULL;
}
