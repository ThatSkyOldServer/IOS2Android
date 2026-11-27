/* Darwin arm64 varargs occupy eight-byte stack slots (also long double).
 * NSString %C/%S are UTF-16, unlike Android printf's 32-bit wchar_t. Keep
 * the result in UTF-16 so adjacent %C arguments can form a surrogate pair. */
struct FormatText { uint16_t *units; size_t count,capacity; };
static void format_units(struct FormatText *out,const uint16_t *units,size_t count) {
    if (count>SIZE_MAX/sizeof(uint16_t)-out->count-1) stop("String format too large","NSString");
    size_t needed=out->count+count+1;
    if (needed>out->capacity) {
        size_t capacity=needed>out->capacity*2 ? needed : out->capacity*2;
        uint16_t *grown=realloc(out->units,capacity*sizeof(*grown));
        if (!grown) stop("Allocation failed","NSString format");
        out->units=grown; out->capacity=capacity;
    }
    if (count) memcpy(out->units+out->count,units,count*sizeof(*units));
    out->count+=count; out->units[out->count]=0;
}
static void format_utf8(struct FormatText *out,const char *bytes,size_t count) {
    const unsigned char *p=(const void *)bytes,*end=p+count;
    while (p<end) {
        uint32_t scalar;
        if (decode_utf8(&p,end,&scalar)<0) stop("Invalid formatted UTF-8","NSString");
        uint16_t units[2]; size_t n=1;
        if (scalar>0xffff) { scalar-=0x10000; units[0]=0xd800+(scalar>>10); units[1]=0xdc00+(scalar&1023); n=2; }
        else units[0]=(uint16_t)scalar;
        format_units(out,units,n);
    }
}
static unsigned format_decimal(const char **cursor,const char *text) {
    unsigned value=0;
    while (**cursor>='0' && **cursor<='9') {
        unsigned digit=(unsigned)(*(*cursor)++-'0');
        if (value>100000 || value*10+digit>1000000) stop("String format field too large",text);
        value=value*10+digit;
    }
    return value;
}
static unsigned format_position(const char **cursor,const char *text) {
    const char *start=*cursor;
    unsigned value=format_decimal(cursor,text);
    if (**cursor!='$') { *cursor=start; return 0; }
    if (!value || value>4096) stop("Invalid format argument index",text);
    ++*cursor; return value;
}
static Word format_slot(const unsigned char *arguments,unsigned position,unsigned *next) {
    if (!position) position=(*next)++;
    if (!position || position>4096) stop("Too many format arguments","NSString");
    const unsigned char *slot=(const void *)(((uintptr_t)arguments+7)&~(uintptr_t)7);
    slot+=(position-1)*8;
    return format_argument(&slot,8);
}
static void format_padding(struct FormatText *out,int count) {
    const uint16_t space=' ';
    while (count-->0) format_units(out,&space,1);
}
static void format_unicode_field(struct FormatText *out,const uint16_t *units,size_t count,int width,int precision,int left) {
    if (precision>=0 && count>(size_t)precision) count=(size_t)precision;
    int padding=width>(int)count ? width-(int)count : 0;
    if (!left) format_padding(out,padding);
    format_units(out,units,count);
    if (left) format_padding(out,padding);
}
static pthread_once_t format_locale_once=PTHREAD_ONCE_INIT;
static jmethodID format_locale_method;
static void format_locale_initialize(void) {
    JNIEnv *env=attached_environment("Localized string format");
    jclass type=(*env)->GetObjectClass(env,activity);
    format_locale_method=(*env)->GetMethodID(env,type,"localizedFormatNumber","(JCIII)Ljava/lang/String;");
    (*env)->DeleteLocalRef(env,type);
    if ((*env)->ExceptionCheck(env) || !format_locale_method) {
        (*env)->ExceptionClear(env); stop("Localized formatter unavailable","NSString");
    }
}
static void format_localized_number(struct FormatText *out,Word value,char code,const char *length,const char *flags,int width,int precision) {
    if (code=='d' || code=='i') {
        if (length[0]=='h') value=length[1] ? (Word)(int64_t)(int8_t)value : (Word)(int64_t)(int16_t)value;
        else if (!length[0]) value=(Word)(int64_t)(int32_t)value;
    } else if (code=='u') {
        if (length[0]=='h') value=length[1] ? (uint8_t)value : (uint16_t)value;
        else if (!length[0]) value=(uint32_t)value;
    }
    int options=0; const char *names="-+ 0#";
    for (unsigned i=0;names[i];++i) if (strchr(flags,names[i])) options|=1<<i;
    pthread_once(&format_locale_once,format_locale_initialize);
    JNIEnv *env=attached_environment("Localized string format");
    jstring text=(*env)->CallObjectMethod(env,activity,format_locale_method,(jlong)value,(jchar)code,(jint)options,(jint)width,(jint)precision);
    if ((*env)->ExceptionCheck(env) || !text) {
        (*env)->ExceptionClear(env); stop("Localized number formatting failed","NSString");
    }
    const jchar *units=(*env)->GetStringChars(env,text,NULL);
    if (!units) { (*env)->ExceptionClear(env); stop("Allocation failed","Localized string format"); }
    format_units(out,units,(size_t)(*env)->GetStringLength(env,text));
    (*env)->ReleaseStringChars(env,text,units); (*env)->DeleteLocalRef(env,text);
}
static int format_scalar(char *output,size_t capacity,const char *spec,char code,Word value,int wide) {
    if (code=='s') return snprintf(output,capacity,spec,value ? (const char *)value : "(null)");
    if (code=='p') return snprintf(output,capacity,spec,(void *)value);
    if (code=='c') return snprintf(output,capacity,spec,(int)value);
    if (code=='d' || code=='i') return wide ? snprintf(output,capacity,spec,(long long)value) : snprintf(output,capacity,spec,(int)value);
    if (strchr("ouxX",code)) return wide ? snprintf(output,capacity,spec,(unsigned long long)value) : snprintf(output,capacity,spec,(unsigned int)value);
    double number;memcpy(&number,&value,sizeof(number));return snprintf(output,capacity,spec,number);
}
__attribute__((used,noinline)) static Word foundation_format(Word cls,const char *selector,Word format,const unsigned char *arguments) {
    const char *text=string_bytes(format,selector),*cursor=text;
    int localized=!strcmp(selector,"localizedStringWithFormat:");
    struct FormatText out={0}; unsigned next=1;
    while (*cursor) {
        const char *literal=cursor;
        while (*cursor && *cursor!='%') ++cursor;
        format_utf8(&out,literal,(size_t)(cursor-literal));
        if (!*cursor) break;
        ++cursor;
        if (*cursor=='%') { format_utf8(&out,"%",1); ++cursor; continue; }
        unsigned position=format_position(&cursor,text);
        char flags[8]={0}; size_t flag_count=0; int left=0;
        while (*cursor && strchr("-+ #0'",*cursor)) {
            if (*cursor=='-') left=1;
            if (!strchr(flags,*cursor) && flag_count<sizeof(flags)-1) flags[flag_count++]=*cursor;
            ++cursor;
        }
        int width=0,precision=-1;
        if (*cursor=='*') {
            ++cursor; unsigned p=format_position(&cursor,text);
            width=(int32_t)format_slot(arguments,p,&next);
            if (width==INT_MIN) stop("String format field too large",text);
            if (width<0) { left=1; width=-width; if (!strchr(flags,'-')) flags[flag_count++]='-'; }
        } else width=(int)format_decimal(&cursor,text);
        if (*cursor=='.') {
            ++cursor;
            if (*cursor=='*') { ++cursor; unsigned p=format_position(&cursor,text); precision=(int32_t)format_slot(arguments,p,&next); }
            else precision=(int)format_decimal(&cursor,text);
        }
        if (width>1000000 || precision>1000000) stop("String format field too large",text);
        char length[3]={0};
        if (*cursor && strchr("hlqjztL",*cursor)) {
            length[0]=*cursor++;
            if ((length[0]=='h' || length[0]=='l') && *cursor==length[0]) length[1]=*cursor++;
        }
        char code=*cursor;
        if (!code) stop("Unterminated NSString format",text);
        ++cursor;
        if (!strchr("@CSspdiouxXcDfFeEgGaAUOn",code)) stop("Unsupported NSString format",text);
        if (localized && code=='@' && strchr(flags,'#')) stop("Localized plural format unavailable",text);
        Word value=format_slot(arguments,position,&next);
        if (code=='@' || code=='C' || code=='S' || ((code=='s' || code=='c') && length[0]=='l')) {
            struct TextView view={0}; uint16_t single=(uint16_t)value; char *description=NULL;
            if (code=='C') view=(struct TextView){&single,NULL,1};
            else if (code=='S') {
                const uint16_t *units=(const void *)value; size_t count=0;
                if (units) { while (units[count] && (precision<0 || count<(size_t)precision)) ++count; view=(struct TextView){units,NULL,count}; }
            } else if (length[0]=='l' && code!='@') {
                struct FormatText wide={0}; const uint32_t *units=(const void *)value; uint32_t character=(uint32_t)value;
                if (code=='c') units=&character;
                if (units) for (size_t i=0;(code=='c' ? i<1 : units[i]!=0) && (precision<0 || wide.count<(size_t)precision);++i) {
                    uint32_t scalar=units[i]; uint16_t pair[2]; size_t count=1;
                    if (scalar>0x10ffff) scalar=0xfffd;
                    if (scalar>0xffff) { scalar-=0x10000; pair[0]=0xd800+(scalar>>10); pair[1]=0xdc00+(scalar&1023); count=2; }
                    else pair[0]=(uint16_t)scalar;
                    format_units(&wide,pair,count);
                }
                view=(struct TextView){wide.units,wide.units,wide.count};
            } else if (value && (read_word(value)==external_class("_OBJC_CLASS_$_NSString") || read_word(value)==external_class("_OBJC_CLASS_$_NSMutableString"))) view=text_view(value,selector);
            else if (value) {
                size_t size=0; FILE *stream=open_memstream(&description,&size);
                if (!stream) stop("Allocation failed",selector);
                print_object(stream,value); fclose(stream);
                struct FormatText rendered={0}; format_utf8(&rendered,description,size);
                view=(struct TextView){rendered.units,rendered.units,rendered.count};
            }
            static const uint16_t null_text[]={'(','n','u','l','l',')'};
            if (!view.units && !value) view=(struct TextView){null_text,NULL,6};
            format_unicode_field(&out,view.units,view.length,width,precision,left);
            free(view.owned); free(description); continue;
        }
        int wide=length[0] && length[0]!='h';
        if (code=='D' || code=='U' || code=='O') { wide=1; length[0]='l'; code=code=='D' ? 'd' : code=='U' ? 'u' : 'o'; }
        if (code=='n') {
            if (!value) stop("Nil format count pointer",selector);
            if (length[0]=='h' && length[1]) *(signed char *)value=(signed char)out.count;
            else if (length[0]=='h') *(short *)value=(short)out.count;
            else if (wide) *(int64_t *)value=(int64_t)out.count;
            else *(int *)value=(int)out.count;
            continue;
        }
        if (localized && strchr("diufFeEgG",code)) {
            format_localized_number(&out,value,code,length,flags,width,precision); continue;
        }
        char spec[64],precision_text[20]={0};
        if (precision>=0) snprintf(precision_text,sizeof(precision_text),".%d",precision);
        const char *modifier=strchr("diouxX",code) ? (wide ? "ll" : length[0]=='h' ? length : "") : "";
        snprintf(spec,sizeof(spec),"%%%s%d%s%s%c",flags,width,precision_text,modifier,code);
        /* Most numeric fields fit on the stack. Avoid a FILE and its heap
         * buffer per field; preserve the exact printf format and Darwin slots. */
        char local[256],*rendered=local;
        int size=format_scalar(local,sizeof(local),spec,code,value,wide);
        if (size<0) stop("String formatting failed",selector);
        if ((size_t)size>=sizeof(local)) {
            rendered=malloc((size_t)size+1);if (!rendered) stop("Allocation failed",selector);
            if (format_scalar(rendered,(size_t)size+1,spec,code,value,wide)!=size) {
                free(rendered);stop("String formatting changed during conversion",selector);
            }
        }
        format_utf8(&out,rendered,(size_t)size);if (rendered!=local) free(rendered);
    }
    Word result=text_from_units(cls==external_class("_OBJC_CLASS_$_NSMutableString") ? cls : external_class("_OBJC_CLASS_$_NSString"),out.units,out.count);
    free(out.units); return result;
}
