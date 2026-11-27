 
__attribute__((used,noinline)) static Word format_message_arguments(Word object,const char *selector,Word format,const unsigned char *arguments) {
    Word cls=host_symbol(object) ? object : read_word(object);
    Word result=foundation_format(cls,selector,format,arguments);
    if (!strncmp(selector,"initWithFormat:",15)) {
        free((void *)object);
        return result;
    }
    if (!strcmp(selector,"appendFormat:")) {
        append_string((void *)object,selector,result);
        free((void *)result);
        return object;
    }
    if (!strcmp(selector,"stringByAppendingFormat:")) {
        Word combined=append_text(object,selector,result);
        free((void *)result);
        return combined;
    }
    return result;
}
__attribute__((naked)) static void darwin_message_format(void) { __asm__ volatile("mov x3,sp\n b format_message_arguments"); }

__attribute__((used,noinline)) static Word collection_variadic_arguments(Word object,const char *selector,Word first,const Word *remaining) {
    int initialize=!strncmp(selector,"init",4);
    Word cls=initialize ? read_word(object) : object;
    int dictionary=strstr(selector,"ObjectsAndKeys:")!=NULL;
    Word result=dictionary ? empty_map(cls) : sequence_empty(cls);
    Word value=first;
    size_t count=0;
    while (value) {
        if (++count>1048576) stop("Unterminated object arguments",selector);
        if (dictionary) {
            Word key=*remaining++;
            if (!key) stop("Nil dictionary key",selector);
            map_set((void *)result,selector,value,key);
        } else sequence_add((void *)result,selector,value);
        value=*remaining++;
    }
    if (initialize) free((void *)object);
    return result;
}
__attribute__((naked)) static void darwin_collection_variadic(void) { __asm__ volatile("mov x3,sp\n b collection_variadic_arguments"); }
__attribute__((used,noinline)) static Word variable_binding_arguments(Word names,Word first_value,const Word *values) {
    if(!names)stop("Null binding names","NSDictionaryOfVariableBindings");
    Word result=empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));const char *cursor=string_bytes(names,"NSDictionaryOfVariableBindings");size_t index=0;
    while(*cursor){const char *end=strchr(cursor,',');if(!end)end=cursor+strlen(cursor);const char *first=cursor,*last=end;
        while(first<last && (*first==' ' || *first=='\t' || *first=='\n'))++first;while(last>first && (last[-1]==' ' || last[-1]=='\t' || last[-1]=='\n'))--last;
        Word value=index ? values[index-1] : first_value;
        if(first==last || !value)stop("Nil variable binding","NSDictionaryOfVariableBindings");
        char *name=strndup(first,(size_t)(last-first));if(!name)stop("Allocation failed","NSDictionaryOfVariableBindings");
        map_set((void *)result,"Variable bindings",value,copy_string(name));++index;free(name);cursor=*end ? end+1 : end;
    }
    return result;
}
__attribute__((naked)) static void darwin_variable_bindings(void) { __asm__ volatile("mov x2,sp\n b variable_binding_arguments"); }

static Method formatting_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString")) {
        if ((class_receiver && (!strcmp(selector,"stringWithFormat:") || !strcmp(selector,"localizedStringWithFormat:"))) || (!class_receiver &&
            (!strcmp(selector,"initWithFormat:") || !strcmp(selector,"appendFormat:") || !strcmp(selector,"stringByAppendingFormat:"))))
            return (Method)darwin_message_format;
        if (!class_receiver && !strcmp(selector,"initWithFormat:arguments:")) return (Method)format_message_arguments;
    }
    int array=!strcmp(receiver,"_OBJC_CLASS_$_NSArray") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableArray");
    int set=!strcmp(receiver,"_OBJC_CLASS_$_NSSet") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableSet");
    int dictionary=!strcmp(receiver,"_OBJC_CLASS_$_NSDictionary") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableDictionary");
    if ((array || set) && !strcmp(selector,class_receiver ? (array ? "arrayWithObjects:" : "setWithObjects:") : "initWithObjects:")) return (Method)darwin_collection_variadic;
    if (dictionary && !strcmp(selector,class_receiver ? "dictionaryWithObjectsAndKeys:" : "initWithObjectsAndKeys:")) return (Method)darwin_collection_variadic;
    return NULL;
}
