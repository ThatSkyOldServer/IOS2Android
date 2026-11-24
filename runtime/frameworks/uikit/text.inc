struct ParagraphState { Word object,tabs; long integers[3]; double metrics[12]; float hyphen; struct ParagraphState *next; };
static struct ParagraphState *paragraph_states;
static const char *const paragraph_integers[]={"alignment","lineBreakMode","baseWritingDirection"};
static const char *const paragraph_metrics[]={"lineSpacing","paragraphSpacing","firstLineHeadIndent","headIndent","tailIndent","minimumLineHeight","maximumLineHeight","lineHeightMultiple","paragraphSpacingBefore","defaultTabInterval","minimumLineSpacing","maximumLineSpacing"};
static struct ParagraphState *paragraph_state(Word object) {
    for (struct ParagraphState *s=paragraph_states;s;s=s->next) if (s->object==object) return s;
    struct ParagraphState *s=calloc(1,sizeof(*s)); if (!s) stop("Allocation failed","NSParagraphStyle");
    s->object=object; s->integers[0]=4; s->integers[2]=-1; s->tabs=sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    s->next=paragraph_states; paragraph_states=s; return s;
}
static int paragraph_property(const char *selector,const char *const *names,unsigned count,int setter) {
    for (unsigned i=0;i<count;++i) {
        const char *name=names[i]; size_t n=strlen(name);
        if (!setter && !strcmp(selector,name)) return i;
        if (setter && strlen(selector)==n+4 && !strncmp(selector,"set",3) && selector[3]==name[0]-'a'+'A' && !memcmp(selector+4,name+1,n-1) && selector[n+3]==':') return i;
    }
    return -1;
}
static void paragraph_set_integer(Word object,const char *selector,long value) { paragraph_state(object)->integers[paragraph_property(selector,paragraph_integers,COUNT(paragraph_integers),1)]=value; }
static Word paragraph_get_integer(Word object,const char *selector) { return paragraph_state(object)->integers[paragraph_property(selector,paragraph_integers,COUNT(paragraph_integers),0)]; }
static void paragraph_set_metric(Word object,const char *selector,double value) { paragraph_state(object)->metrics[paragraph_property(selector,paragraph_metrics,COUNT(paragraph_metrics),1)]=value; }
static double paragraph_get_metric(Word object,const char *selector) { return paragraph_state(object)->metrics[paragraph_property(selector,paragraph_metrics,COUNT(paragraph_metrics),0)]; }
static void paragraph_set_hyphen(Word object,const char *selector,float value) { paragraph_state(object)->hyphen=value; }
static float paragraph_get_hyphen(Word object) { return paragraph_state(object)->hyphen; }
static void paragraph_set_tabs(Word object,const char *selector,Word tabs) { paragraph_state(object)->tabs=tabs ? sequence_copy((void *)tabs,"copy") : sequence_empty(external_class("_OBJC_CLASS_$_NSArray")); }
static Word paragraph_get_tabs(Word object) { return paragraph_state(object)->tabs; }
static Word paragraph_copy(Word object,const char *selector) {
    Word copy=host_allocate(external_class(!strncmp(selector,"mutable",7) ? "_OBJC_CLASS_$_NSMutableParagraphStyle" : "_OBJC_CLASS_$_NSParagraphStyle"));
    struct ParagraphState *source=paragraph_state(object),*dest=paragraph_state(copy); struct ParagraphState *next=dest->next;
    *dest=*source; dest->object=copy; dest->next=next; return copy;
}
static Word paragraph_default(Word cls) { Word object=host_allocate(cls); paragraph_state(object); return object; }
static Method ui_paragraph_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || (strcmp(receiver,"_OBJC_CLASS_$_NSParagraphStyle") && strcmp(receiver,"_OBJC_CLASS_$_NSMutableParagraphStyle"))) return NULL;
    if (class_receiver) return !strcmp(selector,"defaultParagraphStyle") ? (Method)paragraph_default : NULL;
    if (!strcmp(selector,"copy") || !strcmp(selector,"mutableCopy") || !strcmp(selector,"copyWithZone:") || !strcmp(selector,"mutableCopyWithZone:")) return (Method)paragraph_copy;
    if (paragraph_property(selector,paragraph_integers,COUNT(paragraph_integers),0)>=0) return (Method)paragraph_get_integer;
    if (paragraph_property(selector,paragraph_metrics,COUNT(paragraph_metrics),0)>=0) return (Method)paragraph_get_metric;
    if (!strcmp(selector,"hyphenationFactor")) return (Method)paragraph_get_hyphen;
    if (!strcmp(selector,"tabStops")) return (Method)paragraph_get_tabs;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableParagraphStyle")) {
        if (paragraph_property(selector,paragraph_integers,COUNT(paragraph_integers),1)>=0) return (Method)paragraph_set_integer;
        if (paragraph_property(selector,paragraph_metrics,COUNT(paragraph_metrics),1)>=0) return (Method)paragraph_set_metric;
        if (!strcmp(selector,"setHyphenationFactor:")) return (Method)paragraph_set_hyphen;
        if (!strcmp(selector,"setTabStops:")) return (Method)paragraph_set_tabs;
    }
    return NULL;
}

struct TextFieldState {
    Word object,properties[17],attributed,detection,synced_owner,search_owner;
    struct GuestInsets insets;
    struct GuestRange selection;
    unsigned multiline,editable,selectable,revision,synced_revision,synced,closed,delegate_epoch,delegate_cached,visual;
    Method callbacks[8];
    float metrics[21];int options[23];
    struct TextFieldState *next;
};
static struct TextFieldState *text_fields;
static Word text_first_responder;
static void text_field_sync(Word);
struct NamedImageState { Word image; double scale; Word mode; struct NamedImageState *next; };
static struct NamedImageState *named_images;
static struct NamedImageState *named_image_state(Word image) {
    for (struct NamedImageState *state=named_images;state;state=state->next) if (state->image==image) return state;
    struct NamedImageState *state=calloc(1,sizeof(*state)); if (!state) stop("Allocation failed","UIImage metadata");
    state->image=image; state->scale=1; state->next=named_images; named_images=state; return state;
}
static Word ui_image_named_bundle(Word cls,const char *selector,Word name,Word source,Word traits) {
    if (!name) return 0;
    const char *text=string_bytes(name,selector); const char *extension=strrchr(text,'.');
    if (extension && strchr(extension,'/')) extension=NULL;
    size_t prefix=extension ? (size_t)(extension-text) : strlen(text);
    unsigned preferred=screen_scale()>2 ? 3 : screen_scale()>1 ? 2 : 1;
    long idiom=(long)device_idiom();
    if(traits){Method scale=find_implementation(traits,"displayScale"),interface=find_implementation(traits,"userInterfaceIdiom");
        if(scale){double value=((double (*)(Word,const char *))scale)(traits,"displayScale");if(isfinite(value) && value>0)preferred=value>2 ? 3 : value>1 ? 2 : 1;}
        if(interface){long value=(long)interface(traits,"userInterfaceIdiom",0,0,0,0);if(value>=0)idiom=value;}
    }
    const char *device=idiom==1 ? "~ipad" : idiom==0 ? "~iphone" : "";
    const char *formats[]={extension ? extension : ".png",".jpg",".jpeg",".heic",".webp",".gif"};
    const char *at=strrchr(text,'@');unsigned explicit_scale=0;
    if(at && at<text+prefix){char *end=NULL;unsigned long value=strtoul(at+1,&end,10);if(end && *end=='x' && end<text+prefix && value && value<=UINT_MAX)explicit_scale=(unsigned)value;}
    const char *device_at=strrchr(text,'~');int explicit_device=device_at && device_at<text+prefix && (!strncmp(device_at,"~iphone",7) || !strncmp(device_at,"~ipad",5));
    size_t stem=explicit_device ? (size_t)(device_at-text) : prefix,device_length=explicit_device ? prefix-stem : strlen(device);
    unsigned scales[3]={preferred,preferred==3 ? 2 : 1,preferred==3 ? 1 : 3};
    for(unsigned format=0;format<(extension ? 1 : COUNT(formats));++format)for(unsigned i=0;i<(explicit_scale ? 1 : 3);++i)for(unsigned variant=0;variant<(*device && !explicit_device ? 2 : 1);++variant){
        char filename[PATH_MAX],density[32]={0};unsigned scale=explicit_scale ? explicit_scale : scales[i];
        if(!explicit_scale && scale>1)snprintf(density,sizeof(density),"@%ux",scale);
        int length=snprintf(filename,sizeof(filename),"%.*s%s%.*s%s",(int)stem,text,density,(int)(explicit_device || variant==0 ? device_length : 0),explicit_device ? device_at : device,formats[format]);
        if(length<0 || (size_t)length>=sizeof(filename))return 0;
        Word path=bundle_resource(source ? source : (Word)&bundle,selector,copy_string(filename),0);if(!path)continue;
        Word data=data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,path),image=image_from_data(cls,selector,(void *)data);
        if(image){named_image_state(image)->scale=scale;LOG("Original named image loaded: %s",filename);return image;}
    }
    static unsigned logged;
    if (logged++<24) LOG("Named image resource unavailable: %s",text);
    return 0;
}
static Word ui_image_named(Word cls,const char *selector,Word name) { return ui_image_named_bundle(cls,selector,name,0,0); }
static Word ui_image_rendering(Word object,const char *selector,Word mode) {
    if (mode>2) stop("Invalid UIImage rendering mode",selector);
    struct HostObject *copy=malloc(sizeof(*copy)); if (!copy) stop("Allocation failed",selector);
    *copy=*(struct HostObject *)object;
    struct NamedImageState *source=named_image_state(object),*state=named_image_state((Word)copy);
    state->scale=source->scale; state->mode=mode; return (Word)copy;
}
static Word ui_image_mode(Word object) { return named_image_state(object)->mode; }
static double ui_image_scale(Word object) { return named_image_state(object)->scale; }
static struct ImageSize ui_image_size(Word object) {
    struct ImageSize size=image_size((void *)object); double scale=ui_image_scale(object);
    return (struct ImageSize){size.width/scale,size.height/scale};
}
static void image_context_begin(struct GuestSize,unsigned char,double);
static void image_context_end(void);
static Word image_context_current(void),image_context_image(void);
static void drawing_fill_color(Word,Word),drawing_stroke_color(Word,Word);
static void drawing_translate(Word,double,double);
static void drawing_image_command(Word,struct GuestRect,Word,unsigned,unsigned);
static void ui_color_set(Word object,const char *selector) {
    Word c=image_context_current(); if (!c) return;
    if (strcmp(selector,"setStroke")) drawing_fill_color(c,object);
    if (strcmp(selector,"setFill")) drawing_stroke_color(c,object);
}
static void ui_image_draw(Word object,const char *selector,struct GuestRect rect) { Word c=image_context_current(); if (c) drawing_image_command(c,rect,object,27,0); }
static void ui_image_draw_point(Word object,const char *selector,struct GuestPoint point) { struct ImageSize size=ui_image_size(object); ui_image_draw(object,selector,(struct GuestRect){point.x,point.y,size.width,size.height}); }
static Word ui_image_data_scale(Word cls,const char *selector,Word data,double scale) {
    if (!isfinite(scale) || scale<=0) return 0;
    Word image=image_from_data(cls,selector,(void *)data); if (image) named_image_state(image)->scale=scale; return image;
}
static Word ui_image_init_data_scale(Word object,const char *selector,Word data,double scale) { Word result=ui_image_data_scale(read_word(object),selector,data,scale); free((void *)object); return result; }
static Word ui_image_init_data(Word object,const char *selector,Word data) { return ui_image_init_data_scale(object,selector,data,1); }
static Word ui_image_file(Word cls,const char *selector,Word file) { Word data=data_from_file(external_class("_OBJC_CLASS_$_NSData"),selector,file); return image_from_data(cls,selector,(void *)data); }
static Word ui_image_hash(Word object) { return object>>4; }
struct TextAttachmentState { Word object,data,type,image; struct TextAttachmentState *next; };
static struct TextAttachmentState *text_attachments;
static struct TextAttachmentState *text_attachment(Word object) {
    for (struct TextAttachmentState *state=text_attachments;state;state=state->next) if (state->object==object) return state;
    struct TextAttachmentState *state=calloc(1,sizeof(*state)); if (!state) stop("Allocation failed","NSTextAttachment");
    state->object=object; state->next=text_attachments; text_attachments=state; return state;
}
static Word ui_attachment_init(Word object,const char *selector,Word data,Word type) {
    struct TextAttachmentState *state=text_attachment(object); state->data=data; state->type=type; return object;
}
static void ui_attachment_image(Word object,const char *selector,Word image) { text_attachment(object)->image=image; }
static Word ui_attachment_get_image(Word object) { return text_attachment(object)->image; }
static const char *const text_field_properties[]={"text","placeholder","textColor","font","borderStyle","clearButtonMode","returnKeyType",
    "contentVerticalAlignment","contentHorizontalAlignment","autocapitalizationType","autocorrectionType","textAlignment","delegate",
    "keyboardType","spellCheckingType","secureTextEntry","enabled"};
static Word ui_font_system(Word,const char *,double);
static Word ui_color_black(Word,const char *);
static Word attributed_string(Word);
static Word attributed_init(Word,const char *,Word,Word);
static Word attributed_copy(Word,const char *);
static void input_selection_changed(Word);
static Word text_resign(Word);
static struct TextFieldState *text_field_state(Word object) {
    struct TextFieldState *existing=view_state(object)->input;if(existing)return existing;
    for (struct TextFieldState *field=text_fields;field;field=field->next) if (field->object==object) return field;
    struct TextFieldState *field=calloc(1,sizeof(*field));
    if (!field) stop("Allocation failed","UITextField attributes");
    field->object=object; field->properties[0]=copy_string(""); field->properties[9]=2; field->properties[16]=1;
    field->multiline=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UITextView"));field->revision=1;
    if(field->multiline){field->editable=field->selectable=1;field->insets=(struct GuestInsets){8,0,8,0};field->properties[11]=4;
        field->properties[2]=ui_color_black(external_class("_OBJC_CLASS_$_UIColor"),"blackColor");field->properties[3]=ui_font_system(external_class("_OBJC_CLASS_$_UIFont"),"systemFontOfSize:",12);view_state(object)->input=field;}
    field->next=text_fields; text_fields=field; return field;
}
static int text_field_property(const char *selector,int setter) {
    size_t length=strlen(selector);
    for (unsigned i=0;i<COUNT(text_field_properties);++i) {
        const char *name=text_field_properties[i];
        if (!setter && !strcmp(selector,name)) return (int)i;
        if (setter && length==strlen(name)+4 && !strncmp(selector,"set",3) && selector[length-1]==':' &&
            selector[3]==(char)(name[0]>='a' && name[0]<='z' ? name[0]-'a'+'A' : name[0]) && !memcmp(selector+4,name+1,strlen(name)-1)) return (int)i;
    }
    return -1;
}
static Word text_field_get(Word object,const char *selector) { return text_field_state(object)->properties[text_field_property(selector,0)]; }
static void text_field_set(Word object,const char *selector,Word value) {
    struct TextFieldState *field=text_field_state(object);
    int index=text_field_property(selector,1);
    if (index==0 || index==1) value=value ? text_copy(value,"copy") : index==0 ? copy_string("") : 0;
    field->properties[index]=value;++field->revision;
    if(index==0){field->attributed=0;size_t length=string_length(value,"Text input");if(field->selection.location>length)field->selection=(struct GuestRange){length,0};else if(field->selection.length>length-field->selection.location)field->selection.length=length-field->selection.location;}
    if(index==12){field->delegate_cached=0;if(field->multiline)ui_scroll_input_delegate(object,value);}
    if(field->multiline || field->visual)ui_input_sync(object);else if (text_first_responder==object) text_field_sync(object);if(field->search_owner)ui_search_field_sync(field->search_owner);
}
static Word text_editor_init(Word object,const char *selector,struct GuestRect frame,Word container) {
    if(container)stop("Custom text containers are unavailable",selector);
    view_init_frame(object,selector,frame);text_field_state(object);ui_input_sync(object);return object;
}
static Word text_editor_frame(Word object,const char *selector,struct GuestRect frame){return text_editor_init(object,selector,frame,0);}
static Word text_editor_plain(Word object,const char *selector){return text_editor_init(object,selector,(struct GuestRect){0},0);}
static void text_editor_attributed(Word object,const char *selector,Word value){struct TextFieldState *field=text_field_state(object);field->attributed=value ? attributed_copy(value,"copy") : 0;field->properties[0]=value ? text_copy(attributed_string(value),"copy") : copy_string("");field->selection=(struct GuestRange){0};++field->revision;ui_input_sync(object);}
static Word text_editor_get_attributed(Word object){struct TextFieldState *field=text_field_state(object);return field->attributed ? field->attributed : attributed_init(host_allocate(external_class("_OBJC_CLASS_$_NSAttributedString")),"initWithString:",field->properties[0],0);}
static struct GuestInsets text_editor_insets(Word object){return text_field_state(object)->insets;}
static void text_editor_set_insets(Word object,const char *selector,struct GuestInsets insets){if(!isfinite(insets.top)||!isfinite(insets.left)||!isfinite(insets.bottom)||!isfinite(insets.right))stop("Invalid text container insets",selector);text_field_state(object)->insets=insets;ui_input_sync(object);}
static struct GuestRange text_editor_selection(Word object){return text_field_state(object)->selection;}
static void text_editor_set_selection(Word object,const char *selector,struct GuestRange range){struct TextFieldState *field=text_field_state(object);size_t length=string_length(field->properties[0],selector);if(range.location>length || range.length>length-range.location)stop("Text selection exceeds content",selector);if(!memcmp(&range,&field->selection,sizeof(range)))return;field->selection=range;ui_input_sync(object);input_selection_changed(object);}
static Word text_editor_property(Word object,const char *selector){struct TextFieldState *field=text_field_state(object);return !strcmp(selector,"isEditable") || !strcmp(selector,"editable") ? field->editable : !strcmp(selector,"isSelectable") || !strcmp(selector,"selectable") ? field->selectable : field->detection;}
static void text_editor_set_property(Word object,const char *selector,Word value){struct TextFieldState *field=text_field_state(object);if(!strcmp(selector,"setEditable:")){if(!value && text_first_responder==object && !text_resign(object))return;field->editable=value!=0;}else if(!strcmp(selector,"setSelectable:")){if(!value && !field->editable && text_first_responder==object && !text_resign(object))return;field->selectable=value!=0;}else{if(value!=UINTPTR_MAX && (value&~(Word)7))stop("Unsupported text data detectors",selector);field->detection=value;}++field->revision;ui_input_sync(object);}
struct UIKitColor { Word isa; double red,green,blue,alpha; };
static Word ui_color_rgba(Word cls,const char *selector,double red,double green,double blue,double alpha) {
    struct UIKitColor *color=calloc(1,sizeof(*color));
    if (!color) stop("Allocation failed",selector);
    *color=(struct UIKitColor){cls,red,green,blue,alpha}; return (Word)color;
}
static Word ui_color_white(Word cls,const char *selector,double white,double alpha) { return ui_color_rgba(cls,selector,white,white,white,alpha); }
static unsigned char ui_color_get_rgba(struct UIKitColor *color,const char *selector,double *red,double *green,double *blue,double *alpha) {
    if (red) *red=color->red; if (green) *green=color->green;
    if (blue) *blue=color->blue; if (alpha) *alpha=color->alpha;
    return 1;  
}
static Word ui_color_alpha(struct UIKitColor *color,const char *selector,double alpha) {
    return ui_color_rgba(color->isa,selector,color->red,color->green,color->blue,alpha);
}
static Word ui_color_black(Word cls,const char *selector) { return ui_color_white(cls,selector,0,1); }
static Word ui_color_named(Word cls,const char *selector) {
    const struct { const char *name; double r,g,b,a; } colors[]={
        {"whiteColor",1,1,1,1},{"lightGrayColor",2.0/3,2.0/3,2.0/3,1},{"grayColor",0.5,0.5,0.5,1},
        {"darkGrayColor",1.0/3,1.0/3,1.0/3,1},{"redColor",1,0,0,1},{"greenColor",0,1,0,1},
        {"blueColor",0,0,1,1},{"yellowColor",1,1,0,1},{"cyanColor",0,1,1,1},{"magentaColor",1,0,1,1},
        {"orangeColor",1,0.5,0,1},{"purpleColor",0.5,0,0.5,1},{"brownColor",0.6,0.4,0.2,1}};
    for (unsigned i=0;i<COUNT(colors);++i) if (!strcmp(selector,colors[i].name)) return ui_color_rgba(cls,selector,colors[i].r,colors[i].g,colors[i].b,colors[i].a);
    return 0;
}
static Word ui_color_clear(Word cls,const char *selector) { return ui_color_white(cls,selector,0,0); }
static Word ui_color_from_cg(Word cls,const char *selector,Word object) {
    if (!object || read_word(object)!=external_class("_OBJC_CLASS_$_UIColor")) return 0;
    struct UIKitColor *color=(void *)object; return ui_color_rgba(cls,selector,color->red,color->green,color->blue,color->alpha);
}
struct UIKitFontDescriptor { Word isa,attributes; };
struct UIKitFont { Word isa,descriptor; double size; double metrics[6]; int metrics_ready; };
static pthread_mutex_t ui_font_metrics_mutex=PTHREAD_MUTEX_INITIALIZER;
struct UIKitShadow { Word isa,color; struct GuestSize offset; double blur; };
static void ui_shadow_color(struct UIKitShadow *shadow,const char *selector,Word color) { shadow->color=color; }
static Word ui_shadow_get_color(struct UIKitShadow *shadow) { return shadow->color; }
static void ui_shadow_offset(struct UIKitShadow *shadow,const char *selector,struct GuestSize offset) { shadow->offset=offset; }
static struct GuestSize ui_shadow_get_offset(struct UIKitShadow *shadow) { return shadow->offset; }
static void ui_shadow_blur(struct UIKitShadow *shadow,const char *selector,double blur) { shadow->blur=blur; }
static double ui_shadow_get_blur(struct UIKitShadow *shadow) { return shadow->blur; }
static Word ui_font_descriptor(Word cls,const char *selector,struct GuestMap *attributes) {
    if (!attributes) return 0;
    struct UIKitFontDescriptor *descriptor=calloc(1,sizeof(*descriptor));
    if (!descriptor) stop("Allocation failed",selector);
    *descriptor=(struct UIKitFontDescriptor){cls,map_copy(attributes,"copy")}; return (Word)descriptor;
}
static Word ui_font_attribute(struct UIKitFontDescriptor *descriptor,const char *selector,Word key) { return map_get((void *)descriptor->attributes,selector,key); }
static Word ui_font_attributes(struct UIKitFontDescriptor *descriptor) { return descriptor->attributes; }
static Word ui_font_symbolic_traits(struct UIKitFontDescriptor *descriptor) {
    Word traits=ui_font_attribute(descriptor,"symbolicTraits",copy_string("NSFontTraitsAttribute"));
    Word symbolic=traits ? map_get((void *)traits,"symbolicTraits",copy_string("NSCTFontSymbolicTrait")) : 0;
    return symbolic ? numeric_integer((void *)symbolic) : 0;
}
static Word ui_font_with_attributes(struct UIKitFontDescriptor *descriptor,const char *selector,struct GuestMap *added) {
    if (!added) return 0;
    struct GuestMap *attributes=(void *)map_copy((void *)descriptor->attributes,"copy");
    dictionary_merge(attributes,selector,added);
    Word result=ui_font_descriptor(descriptor->isa,selector,attributes);
    free(attributes->entries); free(attributes); return result;
}
static Word ui_font_with_traits(struct UIKitFontDescriptor *descriptor,const char *selector,Word symbolic) {
    struct GuestMap *attributes=(void *)map_copy((void *)descriptor->attributes,"copy");
    Word key=copy_string("NSFontTraitsAttribute"),old=map_get(attributes,selector,key);
    struct GuestMap *traits=old ? (void *)map_copy((void *)old,"copy") : (void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(traits,selector,number_integer(symbolic),copy_string("NSCTFontSymbolicTrait"));
    map_set(attributes,selector,(Word)traits,key);
    Word result=ui_font_descriptor(descriptor->isa,selector,attributes);
    free(attributes->entries); free(attributes); return result;
}
static Word ui_font_with_size(struct UIKitFontDescriptor *descriptor,const char *selector,double size) {
    struct GuestMap *attributes=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(attributes,selector,real_number_factory(0,selector,size),copy_string("NSFontSizeAttribute"));
    Word result=ui_font_with_attributes(descriptor,selector,attributes);
    free(attributes->entries); free(attributes); return result;
}
static double ui_font_descriptor_size(struct UIKitFontDescriptor *descriptor) {
    Word number=ui_font_attribute(descriptor,"pointSize",copy_string("NSFontSizeAttribute"));
    return number ? numeric_double((void *)number) : 0;
}
static Word ui_font_create(Word cls,const char *selector,struct UIKitFontDescriptor *descriptor,double size) {
    if (!descriptor) return 0;
    if (!size) size=ui_font_descriptor_size(descriptor);
    if (!(size>0) || !isfinite(size)) return 0;
    struct UIKitFont *font=calloc(1,sizeof(*font));
    if (!font) stop("Allocation failed",selector);
    *font=(struct UIKitFont){.isa=cls,.descriptor=(Word)descriptor,.size=size}; return (Word)font;
}
static Word ui_font_get_descriptor(struct UIKitFont *font) { return font->descriptor; }
static double ui_font_size(struct UIKitFont *font) { return font->size; }
static Word ui_font_name(struct UIKitFont *font,const char *selector) {
    struct UIKitFontDescriptor *descriptor=(void *)font->descriptor;
    Word name=ui_font_attribute(descriptor,selector,copy_string(!strcmp(selector,"familyName") ? "NSFontFamilyAttribute" : "NSFontNameAttribute"));
    if (!name) name=ui_font_attribute(descriptor,selector,copy_string("NSFontFamilyAttribute"));
    return name ? name : copy_string("sans-serif");
}
static Word ui_font_resize(struct UIKitFont *font,const char *selector,double size) {
    return ui_font_create(font->isa,selector,(void *)font->descriptor,size);
}
static double ui_font_metric(struct UIKitFont *font,const char *selector) {
    const char *names[]={"lineHeight","ascender","descender","leading","capHeight","xHeight"};
    unsigned index=0; while (index<COUNT(names) && strcmp(selector,names[index])) ++index;
    if (index==COUNT(names)) stop("Invalid UIFont metric",selector);
    if (!__atomic_load_n(&font->metrics_ready,__ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&ui_font_metrics_mutex);
        if (!font->metrics_ready) {
            struct UIKitFontDescriptor *descriptor=(void *)font->descriptor;
            Word name=ui_font_name(font,"fontName");
            JNIEnv *env=attached_environment(selector); jclass cls=(*env)->GetObjectClass(env,activity);
            jmethodID method=(*env)->GetMethodID(env,cls,"guestFontMetrics","(Ljava/lang/String;IF)[F");
            jstring family=http_java_string(env,name);
            jfloatArray values=(*env)->CallObjectMethod(env,activity,method,family,(jint)ui_font_symbolic_traits(descriptor),(jfloat)font->size);
            jfloat metrics[6]={0};
            int valid=!(*env)->ExceptionCheck(env) && values && (*env)->GetArrayLength(env,values)==6;
            if (valid) (*env)->GetFloatArrayRegion(env,values,0,6,metrics);
            if (values) (*env)->DeleteLocalRef(env,values);
            (*env)->DeleteLocalRef(env,family); (*env)->DeleteLocalRef(env,cls);
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); valid=0; }
            for (unsigned i=0;i<6;++i) { if (!isfinite(metrics[i])) valid=0; font->metrics[i]=metrics[i]; }
            if (!valid || metrics[0]<=0) { pthread_mutex_unlock(&ui_font_metrics_mutex); stop("Android UIFont measurement failed",selector); }
            __atomic_store_n(&font->metrics_ready,1,__ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&ui_font_metrics_mutex);
    }
    return font->metrics[index];
}
static Word ui_font_named(Word cls,const char *selector,Word name,double size) {
    struct GuestMap *attributes=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(attributes,selector,name,copy_string("NSFontNameAttribute"));
    Word descriptor=ui_font_descriptor(external_class("_OBJC_CLASS_$_UIFontDescriptor"),selector,attributes);
    return ui_font_create(cls,selector,(void *)descriptor,size>0 ? size : 17);
}
static Word ui_font_system(Word cls,const char *selector,double size) { return ui_font_named(cls,selector,copy_string(!strcmp(selector,"boldSystemFontOfSize:") ? ".SFUI-Bold" : !strcmp(selector,"italicSystemFontOfSize:") ? ".SFUI-Italic" : ".SFUI-Regular"),size); }
static double ui_font_standard_size(Word cls,const char *selector) { return !strcmp(selector,"buttonFontSize") ? 18 : !strcmp(selector,"smallSystemFontSize") ? 12 : 17; }
static Word ui_font_preferred_descriptor(Word cls,const char *selector,Word style) {
    const struct { const char *style; double size; int bold; } styles[]={
        {"UICTFontTextStyleBody",17,0},{"UICTFontTextStyleHeadline",17,1},{"UICTFontTextStyleSubhead",15,0},
        {"UICTFontTextStyleFootnote",13,0},{"UICTFontTextStyleCaption1",12,0},{"UICTFontTextStyleCaption2",11,0},
        {"UICTFontTextStyleTitle1",28,0},{"UICTFontTextStyleTitle2",22,0},{"UICTFontTextStyleTitle3",20,0},
        {"UICTFontTextStyleCallout",16,0},{"UICTFontTextStyleLargeTitle",34,0}};
    if (!style) return 0; const char *name=string_bytes(style,selector); unsigned index=0;
    while (index<COUNT(styles) && strcmp(name,styles[index].style)) ++index;
    if (index==COUNT(styles)) return 0;
    JNIEnv *env=attached_environment(selector); static jmethodID scale_method;
    if (!scale_method) { jclass type=(*env)->GetObjectClass(env,activity); scale_method=(*env)->GetMethodID(env,type,"guestFontScale","()F"); (*env)->DeleteLocalRef(env,type); }
    float scale=(*env)->CallFloatMethod(env,activity,scale_method);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); stop("Android font scale unavailable",selector); }
    struct GuestMap *attributes=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(attributes,selector,copy_string(styles[index].bold ? ".SFUI-Bold" : ".SFUI-Regular"),copy_string("NSFontNameAttribute"));
    map_set(attributes,selector,real_number_factory(0,selector,styles[index].size*scale),copy_string("NSFontSizeAttribute"));
    Word result=ui_font_descriptor(cls,selector,attributes); free(attributes->entries); free(attributes); return result;
}
static Word ui_font_preferred(Word cls,const char *selector,Word style) {
    Word descriptor=ui_font_preferred_descriptor(external_class("_OBJC_CLASS_$_UIFontDescriptor"),selector,style);
    return descriptor ? ui_font_create(cls,selector,(void *)descriptor,0) : 0;
}
static Word ui_preferred_content_category(Word object,const char *selector) {
    static const double scales[]={.82,.88,.94,1,1.12,1.23,1.35,1.65,1.94,2.35,2.76,3.12};static const char *names[]={"UICTContentSizeCategoryXS","UICTContentSizeCategoryS","UICTContentSizeCategoryM","UICTContentSizeCategoryL","UICTContentSizeCategoryXL","UICTContentSizeCategoryXXL","UICTContentSizeCategoryXXXL","UICTContentSizeCategoryAccessibilityM","UICTContentSizeCategoryAccessibilityL","UICTContentSizeCategoryAccessibilityXL","UICTContentSizeCategoryAccessibilityXXL","UICTContentSizeCategoryAccessibilityXXXL"};
    JNIEnv *env=attached_environment(selector);static jmethodID method;static Word categories[COUNT(scales)];if(!method){jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"guestFontScale","()F");(*env)->DeleteLocalRef(env,cls);}double scale=(*env)->CallFloatMethod(env,activity,method);if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);scale=1;}unsigned index=0;if(!isfinite(scale) || scale<=0)scale=1;while(index+1<COUNT(scales) && scale>(scales[index]+scales[index+1])*.5)++index;if(!categories[index])categories[index]=copy_string(names[index]);return categories[index];
}
static Word ui_interface_color(unsigned index) { JNIEnv *env=attached_environment("Interface palette");static jmethodID method;if(!method){jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"guestInterfaceColors","()[I");(*env)->DeleteLocalRef(env,cls);}jintArray colors=(*env)->CallObjectMethod(env,activity,method);jint palette[]={0xfff8f5fc,0xff211e28,0xff57515e,0xff6750a4,0xffe8e2ec};if(colors && (*env)->GetArrayLength(env,colors)==5)(*env)->GetIntArrayRegion(env,colors,0,5,palette);if(colors)(*env)->DeleteLocalRef(env,colors);if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);unsigned value=palette[index<5 ? index : 0];return ui_color_rgba(external_class("_OBJC_CLASS_$_UIColor"),"Interface palette",((value>>16)&255)/255.0,((value>>8)&255)/255.0,(value&255)/255.0,1); }
struct SearchBarState {
    Word object,field,controller,delegate,prompt,prompt_view,tint,bar_tint,cancel,scope_titles,scope_buttons,surface;
    unsigned shows_cancel,shows_scope,selected_scope,bar_style,style,translucent,closed,layout_running,cached,epoch;
    Method callbacks[10];
};
struct SearchControllerState {
    Word object,bar,results,delegate,updater,cached_updater,original_bar_parent,hidden_navigation,borrowed_view,borrowed_parent;struct GuestRect original_bar_frame,borrowed_frame;unsigned navigation_hidden,updater_epoch;Method updater_method;
    unsigned active,dims,hides_navigation,transition,layout_running,cached,epoch,focus_requested;
    Method callbacks[5];
};
static struct SearchBarState *search_bar_state(Word object) {
    struct ViewState *view=view_state(object);if(view->search)return view->search;struct SearchBarState *bar=calloc(1,sizeof(*bar));if(!bar)stop("Allocation failed","UISearchBar");bar->object=object;bar->translucent=1;view->search=bar;return bar;
}
static struct SearchControllerState *search_controller_state(Word object) {
    struct ViewState *view=view_state(object);if(view->search)return view->search;struct SearchControllerState *state=calloc(1,sizeof(*state));if(!state)stop("Allocation failed","UISearchController");state->object=object;state->dims=state->hides_navigation=1;view->search=state;return state;
}
static Word control_factory(Word,const char *,Word);
static void control_set_content(Word,const char *,Word,Word);
static void control_add_target(Word,const char *,Word,const char *,Word);
static void ui_control_sync(Word);
static Word text_become(Word);
static Word input_replace(Word,Word);
static void search_controller_active(Word,const char *,Word);
static void search_bar_cache(struct SearchBarState *bar) {
    if(bar->cached && bar->epoch==method_cache_epoch)return;const char *names[]={"searchBarShouldBeginEditing:","searchBarTextDidBeginEditing:","searchBarShouldEndEditing:","searchBarTextDidEndEditing:","searchBar:textDidChange:","searchBar:shouldChangeTextInRange:replacementText:","searchBarSearchButtonClicked:","searchBarCancelButtonClicked:","searchBar:selectedScopeButtonIndexDidChange:",NULL};Word delegate=load_weak_retained(&bar->delegate);for(unsigned i=0;i<COUNT(names);++i)bar->callbacks[i]=delegate && names[i] ? find_implementation(delegate,names[i]) : NULL;metal_object_release(delegate);bar->cached=1;bar->epoch=method_cache_epoch;
}
static void search_bar_notify(struct SearchBarState *bar,unsigned index,Word argument) {
    search_bar_cache(bar);Word delegate=load_weak_retained(&bar->delegate);Method method=delegate ? bar->callbacks[index] : NULL;const char *names[]={"searchBarShouldBeginEditing:","searchBarTextDidBeginEditing:","searchBarShouldEndEditing:","searchBarTextDidEndEditing:","searchBar:textDidChange:","searchBar:shouldChangeTextInRange:replacementText:","searchBarSearchButtonClicked:","searchBarCancelButtonClicked:","searchBar:selectedScopeButtonIndexDidChange:"};if(method)method(delegate,names[index],bar->object,argument,0,0);metal_object_release(delegate);
}
static void search_results_update(struct SearchControllerState *state) {
    Word updater=load_weak_retained(&state->updater);if(updater){if(state->cached_updater!=updater || state->updater_epoch!=method_cache_epoch){state->cached_updater=updater;state->updater_epoch=method_cache_epoch;state->updater_method=find_implementation(updater,"updateSearchResultsForSearchController:");}if(state->updater_method)state->updater_method(updater,"updateSearchResultsForSearchController:",state->object,0,0,0);}metal_object_release(updater);
}
static void search_controller_notify(struct SearchControllerState *state,unsigned event) {
    if(!state->cached || state->epoch!=method_cache_epoch){const char *names[]={"willPresentSearchController:","didPresentSearchController:","willDismissSearchController:","didDismissSearchController:","presentSearchController:"};Word delegate=load_weak_retained(&state->delegate);for(unsigned i=0;i<COUNT(names);++i)state->callbacks[i]=delegate ? find_implementation(delegate,names[i]) : NULL;metal_object_release(delegate);state->cached=1;state->epoch=method_cache_epoch;}
    Word delegate=load_weak_retained(&state->delegate);Method method=delegate ? state->callbacks[event] : NULL;const char *names[]={"willPresentSearchController:","didPresentSearchController:","willDismissSearchController:","didDismissSearchController:","presentSearchController:"};if(method)method(delegate,names[event],state->object,0,0,0);metal_object_release(delegate);
}
static Word search_bar_field(struct SearchBarState *bar) {
    if(bar->field)return bar->field;Word field=host_allocate(external_class("_OBJC_CLASS_$_UITextField"));struct TextFieldState *input=text_field_state(field);input->visual=input->editable=input->selectable=1;input->properties[6]=6;input->properties[9]=0;input->properties[10]=1;input->properties[14]=1;input->search_owner=bar->object;view_state(field)->input=input;
    JNIEnv *env=attached_environment("Search palette");jclass cls=(*env)->GetObjectClass(env,activity);jmethodID colors=(*env)->GetMethodID(env,cls,"guestInterfaceColors","()[I");jintArray palette=(*env)->CallObjectMethod(env,activity,colors);jint values[]={0xfff8f5fc,0xff211e28,0xff57515e,0xff6750a4,0xffe8e2ec};if(palette && (*env)->GetArrayLength(env,palette)==5)(*env)->GetIntArrayRegion(env,palette,0,5,values);if(palette)(*env)->DeleteLocalRef(env,palette);(*env)->DeleteLocalRef(env,cls);if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    Word color_class=external_class("_OBJC_CLASS_$_UIColor");input->properties[2]=ui_color_rgba(color_class,"Search text",((values[1]>>16)&255)/255.0,((values[1]>>8)&255)/255.0,(values[1]&255)/255.0,1);input->properties[3]=ui_font_system(external_class("_OBJC_CLASS_$_UIFont"),"systemFontOfSize:",17);
    view_state(field)->background=ui_color_rgba(color_class,"Search field",((values[4]>>16)&255)/255.0,((values[4]>>8)&255)/255.0,(values[4]&255)/255.0,1);bar->tint=ui_color_rgba(color_class,"Search tint",((values[3]>>16)&255)/255.0,((values[3]>>8)&255)/255.0,(values[3]&255)/255.0,1);
    bar->surface=ui_color_rgba(color_class,"Search surface",((values[0]>>16)&255)/255.0,((values[0]>>8)&255)/255.0,(values[0]&255)/255.0,1);bar->field=field;view_add(bar->object,"Search input",field);return field;
}
static Word search_bar_initialize(Word object,const char *selector,struct GuestRect frame) { view_init_frame(object,selector,frame);search_bar_field(search_bar_state(object));ui_search_sync(object);return object; }
static Word search_bar_initialize_plain(Word object,const char *selector) { return search_bar_initialize(object,selector,(struct GuestRect){0,0,0,56}); }
static int ui_search_input_should(Word object,unsigned index) {
    struct SearchBarState *bar=search_bar_state(object);search_bar_cache(bar);unsigned event=index==0 ? 0 : index==2 ? 2 : 6;Word delegate=load_weak_retained(&bar->delegate);Method method=delegate ? bar->callbacks[event] : NULL;int allowed=1;
    if(index==7){metal_object_release(delegate);search_bar_notify(bar,6,0);return 1;}if(method)allowed=method(delegate,event==0 ? "searchBarShouldBeginEditing:" : "searchBarShouldEndEditing:",object,0,0,0)&255;metal_object_release(delegate);return allowed;
}
static int ui_search_input_replace(Word object,struct GuestRange range,Word replacement) {
    struct SearchBarState *bar=search_bar_state(object);search_bar_cache(bar);Word delegate=load_weak_retained(&bar->delegate);Method method=delegate ? bar->callbacks[5] : NULL;int allowed=!method || (((Word (*)(Word,const char *,Word,struct GuestRange,Word))method)(delegate,"searchBar:shouldChangeTextInRange:replacementText:",object,range,replacement)&255);metal_object_release(delegate);return allowed;
}
static void ui_search_input_notify(Word object,unsigned event) {
    if(event!=1 && event!=3)return;struct SearchBarState *bar=search_bar_state(object);search_bar_notify(bar,event,0);if(event==1 && bar->controller && !search_controller_state(bar->controller)->active){search_controller_state(bar->controller)->focus_requested=1;search_controller_active(bar->controller,"setActive:",1);}
}
static void ui_search_input_changed(Word object) { struct SearchBarState *bar=search_bar_state(object);search_bar_notify(bar,4,text_field_state(bar->field)->properties[0]);if(bar->controller)search_results_update(search_controller_state(bar->controller)); }
static void ui_search_field_sync(Word object) { ui_search_sync(object); }
static void search_bar_cancel(Word object,const char *selector,Word sender) { struct SearchBarState *bar=search_bar_state(object);search_bar_notify(bar,7,0);if(bar->controller)search_controller_active(bar->controller,"setActive:",0);else text_resign(bar->field); }
static void search_scope_action(Word object,const char *selector,Word sender) { struct SearchBarState *bar=search_bar_state(object);struct Sequence *buttons=(void *)bar->scope_buttons;for(size_t i=0;buttons && i<buttons->count;++i)if(buttons->items[i]==sender){bar->selected_scope=(unsigned)i;ui_search_sync(object);search_bar_notify(bar,8,i);if(bar->controller)search_results_update(search_controller_state(bar->controller));return;} }
static Word search_bar_get(Word object,const char *selector) {
    struct SearchBarState *bar=search_bar_state(object);if(!strcmp(selector,"searchTextField"))return search_bar_field(bar);if(!strcmp(selector,"text"))return text_field_state(search_bar_field(bar))->properties[0];if(!strcmp(selector,"placeholder"))return text_field_state(search_bar_field(bar))->properties[1];if(!strcmp(selector,"delegate"))return load_weak(&bar->delegate);if(!strcmp(selector,"prompt"))return bar->prompt;if(!strcmp(selector,"tintColor"))return bar->tint;if(!strcmp(selector,"barTintColor"))return bar->bar_tint;if(!strcmp(selector,"showsCancelButton"))return bar->shows_cancel;if(!strcmp(selector,"showsScopeBar"))return bar->shows_scope;if(!strcmp(selector,"scopeButtonTitles"))return bar->scope_titles;if(!strcmp(selector,"selectedScopeButtonIndex"))return bar->selected_scope;if(!strcmp(selector,"barStyle"))return bar->bar_style;if(!strcmp(selector,"searchBarStyle"))return bar->style;return bar->translucent;
}
static void search_bar_set(Word object,const char *selector,Word value) {
    struct SearchBarState *bar=search_bar_state(object);if(!strcmp(selector,"setDelegate:")){store_weak(&bar->delegate,value);bar->cached=0;return;}if(!strcmp(selector,"setText:") || !strcmp(selector,"setPlaceholder:")){text_field_set(search_bar_field(bar),selector,value);return;}
    if(!strcmp(selector,"setPrompt:"))bar->prompt=value ? text_copy(value,"copy") : 0;else if(!strcmp(selector,"setTintColor:"))bar->tint=value;else if(!strcmp(selector,"setBarTintColor:")){bar->bar_tint=value;view_state(object)->background=value;}else if(!strncmp(selector,"setShowsCancelButton:",21))bar->shows_cancel=value!=0;
    else if(!strcmp(selector,"setShowsScopeBar:"))bar->shows_scope=value!=0;else if(!strcmp(selector,"setSelectedScopeButtonIndex:")){struct Sequence *titles=(void *)bar->scope_titles;if(!titles || value>=titles->count)stop("Invalid search scope index",selector);bar->selected_scope=value;}
    else if(!strcmp(selector,"setScopeButtonTitles:")){if(value && !class_inherits(read_word(value),external_class("_OBJC_CLASS_$_NSArray")))stop("Invalid search scopes",selector);struct Sequence *titles=(void *)value;if(titles)for(size_t i=0;i<titles->count;++i)if(!class_inherits(read_word(titles->items[i]),external_class("_OBJC_CLASS_$_NSString")))stop("Invalid search scope title",selector);Word copy=value ? sequence_copy(titles,"copy") : 0;if(bar->scope_titles){struct Sequence *old=(void *)bar->scope_titles;free(old->items);free(old);}bar->scope_titles=copy;bar->selected_scope=0;if(bar->scope_buttons){struct Sequence *buttons=(void *)bar->scope_buttons;for(size_t i=0;i<buttons->count;++i)view_remove(buttons->items[i]);free(buttons->items);free(buttons);bar->scope_buttons=0;}}
    else if(!strcmp(selector,"setBarStyle:")){if(value>1)stop("Unsupported search bar style",selector);bar->bar_style=value;Word cls=external_class("_OBJC_CLASS_$_UIColor");struct TextFieldState *field=text_field_state(search_bar_field(bar));field->properties[2]=ui_color_white(cls,"Search text",value ? 1 : 0,1);view_state(bar->field)->background=ui_color_white(cls,"Search field",value ? .2 : .9,1);bar->surface=ui_color_white(cls,"Search surface",value ? .1 : 1,1);++field->revision;}else if(!strcmp(selector,"setSearchBarStyle:")){if(value>2)stop("Invalid search bar style",selector);bar->style=value;}else bar->translucent=value!=0;ui_search_sync(object);
}
static Word search_bar_input_get(Word object,const char *selector) { return text_field_get(search_bar_field(search_bar_state(object)),selector); }
static void search_bar_input_set(Word object,const char *selector,Word value) { text_field_set(search_bar_field(search_bar_state(object)),selector,value); }
static Word search_bar_first(Word object,const char *selector) { Word field=search_bar_field(search_bar_state(object));return !strcmp(selector,"becomeFirstResponder") ? text_become(field) : !strcmp(selector,"resignFirstResponder") ? text_resign(field) : !strcmp(selector,"isFirstResponder") ? text_first_responder==field : 1; }
static struct GuestSize search_bar_size(Word object,const char *selector,struct GuestSize limit) { struct SearchBarState *bar=search_bar_state(object);return (struct GuestSize){limit.width,56+(bar->prompt ? 24 : 0)+(bar->shows_scope ? 44 : 0)}; }
static Word search_controller_init(Word object,const char *selector,Word results) {
    if(results && !class_inherits(read_word(results),external_class("_OBJC_CLASS_$_UIViewController")))stop("Invalid search results controller",selector);struct SearchControllerState *state=search_controller_state(object);state->results=metal_object_retain(results);
    if(!state->bar){state->bar=search_bar_initialize_plain(host_allocate(external_class("_OBJC_CLASS_$_UISearchBar")),"init");search_bar_state(state->bar)->controller=object;}view_state(object)->modal_style=6;return object;
}
static Word search_controller_plain(Word object,const char *selector) { return search_controller_init(object,selector,0); }
static Word ui_search_controller_load(Word object) {
    if(!class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UISearchController")))return 0;struct SearchControllerState *state=search_controller_state(object);if(!state->bar)search_controller_init(object,"initWithSearchResultsController:",0);Word root=view_state(object)->view;if(!root)return 0;view_add(root,"Search bar presentation",state->bar);
    if(state->results){controller_add_child(object,"Search results containment",state->results);Word results=controller_view(state->results);view_add(root,"Search results presentation",results);Method did=find_implementation(state->results,"didMoveToParentViewController:");if(did)did(state->results,"didMoveToParentViewController:",object,0,0,0);}ui_search_geometry(object);return root;
}
static void ui_search_geometry(Word object) {
    if(!class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UISearchController")))return;struct SearchControllerState *state=search_controller_state(object);Word root=controller_loaded_view(object);if(!root || !state->bar || state->layout_running)return;state->layout_running=1;struct GuestRect bounds=view_state(root)->bounds;struct GuestSize bar_size=search_bar_size(state->bar,"sizeThatFits:",(struct GuestSize){bounds.width,bounds.height});
    if(view_state(state->bar)->parent==root)view_set_frame(state->bar,"Search bar layout",(struct GuestRect){0,0,bounds.width,bar_size.height});Word results=state->results ? controller_loaded_view(state->results) : state->borrowed_view;if(results)view_set_frame(results,"Search results layout",(struct GuestRect){0,bar_size.height,bounds.width,fmax(0,bounds.height-bar_size.height)});state->layout_running=0;
}
static void ui_search_modal_event(Word object,unsigned event) {
    if(!class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UISearchController")))return;struct SearchControllerState *state=search_controller_state(object);if(event==2 && state->bar)text_resign(search_bar_state(state->bar)->field);if(event==1){state->active=1;state->transition=0;}if(event>=3)state->active=state->transition=0;
    if(event==1){state->active=1;state->transition=0;search_results_update(state);if(state->focus_requested && state->bar){state->focus_requested=0;text_become(search_bar_state(state->bar)->field);}}if(event>=3){state->active=state->transition=state->focus_requested=0;if(state->borrowed_view){Word view=state->borrowed_view;view_remove(view);view_set_frame(view,"Search content restoration",state->borrowed_frame);if(state->borrowed_parent)view_add(state->borrowed_parent,"Search content restoration",view);state->borrowed_view=state->borrowed_parent=0;}if(state->hidden_navigation){view_hidden(state->hidden_navigation,"Search navigation restoration",state->navigation_hidden);state->hidden_navigation=0;}if(state->bar){struct SearchBarState *bar=search_bar_state(state->bar);bar->shows_cancel=0;ui_search_sync(state->bar);if(state->original_bar_parent){view_remove(state->bar);view_set_frame(state->bar,"Search bar restoration",state->original_bar_frame);view_add(state->original_bar_parent,"Search bar restoration",state->bar);state->original_bar_parent=0;}}}
    if(event<4){if(state->results){const char *names[]={"viewWillAppear:","viewDidAppear:","viewWillDisappear:","viewDidDisappear:"};Method method=find_implementation(state->results,names[event]);if(method)method(state->results,names[event],1,0,0,0);}search_controller_notify(state,event);}
}
static Word search_controller_get(Word object,const char *selector) { struct SearchControllerState *state=search_controller_state(object);if(!state->bar)search_controller_init(object,"init",0);if(!strcmp(selector,"searchBar"))return state->bar;if(!strcmp(selector,"searchResultsController"))return state->results;if(!strcmp(selector,"searchResultsUpdater"))return load_weak(&state->updater);if(!strcmp(selector,"delegate"))return load_weak(&state->delegate);if(!strcmp(selector,"isActive"))return state->active;if(!strcmp(selector,"hidesNavigationBarDuringPresentation"))return state->hides_navigation;return state->dims; }
static void search_controller_active(Word object,const char *selector,Word active) {
    struct SearchControllerState *state=search_controller_state(object);if(!state->bar)search_controller_init(object,"init",0);if(state->active==(active!=0) || state->transition)return;
    if(!active){state->active=0;state->transition=1;text_resign(search_bar_state(state->bar)->field);if(view_state(object)->presentation_host)ui_controller_dismiss(object,"dismissViewControllerAnimated:completion:",1,0);else{ui_search_modal_event(object,2);ui_search_modal_event(object,3);}return;}
    Word presenter=0;for(Word current=state->bar;current;current=view_state(current)->parent){Word controller=view_state(current)->controller;if(controller && controller!=object){presenter=controller;break;}}if(!presenter)presenter=root_controller;if(!presenter)stop("Search activation requires a presenting controller",selector);
    state->original_bar_parent=view_state(state->bar)->parent;state->original_bar_frame=view_state(state->bar)->frame;state->active=state->transition=1;search_bar_state(state->bar)->shows_cancel=1;
    if(state->hides_navigation){Word navigation=controller_navigation(presenter);if(navigation){state->hidden_navigation=controller_navigation_bar(navigation);state->navigation_hidden=view_state(state->hidden_navigation)->hidden;view_hidden(state->hidden_navigation,"Search navigation hidden",1);}}
    Word root=controller_view(object);if(view_state(state->bar)->parent!=root)view_add(root,"Search bar activation",state->bar);if(!state->results){Word contents=controller_loaded_view(presenter);if(contents && contents!=root){state->borrowed_view=contents;state->borrowed_parent=view_state(contents)->parent;state->borrowed_frame=view_state(contents)->frame;view_add(root,"Search existing results",contents);view_reorder(root,"bringSubviewToFront:",state->bar);}}ui_search_geometry(object);Word delegate=load_weak_retained(&state->delegate);Method custom=delegate ? find_implementation(delegate,"presentSearchController:") : NULL;if(custom)custom(delegate,"presentSearchController:",object,0,0,0);else ui_controller_present(presenter,"presentViewController:animated:completion:",object,1,0);metal_object_release(delegate);
}
static void search_controller_set(Word object,const char *selector,Word value) {
    struct SearchControllerState *state=search_controller_state(object);if(!strcmp(selector,"setActive:")){search_controller_active(object,selector,value);return;}if(!strcmp(selector,"setDelegate:")){store_weak(&state->delegate,value);state->cached=0;}else if(!strcmp(selector,"setSearchResultsUpdater:"))store_weak(&state->updater,value);else if(!strcmp(selector,"setHidesNavigationBarDuringPresentation:"))state->hides_navigation=value!=0;else {state->dims=value!=0;controller_presentation_resize(object);}
}
static double ui_search_dimming(Word object,double fallback) { return class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UISearchController")) ? search_controller_state(object)->dims ? .4 : 0 : fallback; }
struct AttributedRun { struct GuestRange range; Word attributes; unsigned reset; };
struct AttributedState { Word object,string; struct AttributedRun *runs; size_t count,capacity; struct AttributedState *next; };
static struct AttributedState *attributed_states;
static struct AttributedState *attributed_state(Word object) {
    for (struct AttributedState *state=attributed_states;state;state=state->next) if (state->object==object) return state;
    struct AttributedState *state=calloc(1,sizeof(*state)); if (!state) stop("Allocation failed","NSAttributedString");
    state->object=object; state->string=copy_string(""); state->next=attributed_states; attributed_states=state; return state;
}
static void attributed_run_add(struct AttributedState *state,Word attributes,struct GuestRange range,unsigned reset,const char *selector) {
    check_range(range,string_length(state->string,selector),selector);
    if (!range.length) return;
    if (state->count==state->capacity) {
        size_t capacity=state->capacity ? state->capacity*2 : 8;
        if (capacity<state->capacity || capacity>SIZE_MAX/sizeof(*state->runs)) stop("Attributed string too large",selector);
        void *runs=realloc(state->runs,capacity*sizeof(*state->runs)); if (!runs) stop("Allocation failed",selector);
        state->runs=runs; state->capacity=capacity;
    }
    Word copy=attributes ? map_copy((void *)attributes,"copy") : empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    state->runs[state->count++]=(struct AttributedRun){range,copy,reset};
}
static Word attributed_init(Word object,const char *selector,Word string,Word attributes) {
    if (!string) stop("Nil attributed string",selector);
    struct AttributedState *state=attributed_state(object); state->string=text_copy(string,"copy"); state->count=0;
    attributed_run_add(state,attributes,(struct GuestRange){0,string_length(state->string,selector)},1,selector); return object;
}
static Word attributed_init_string(Word object,const char *selector,Word string) { return attributed_init(object,selector,string,0); }
static Word attributed_string(Word object) { return attributed_state(object)->string; }
static Word attributed_length(Word object,const char *selector) { return string_length(attributed_state(object)->string,selector); }
static void attributed_add_attributes(Word object,const char *selector,Word attributes,struct GuestRange range) {
    attributed_run_add(attributed_state(object),attributes,range,!strcmp(selector,"setAttributes:range:"),selector);
}
static void attributed_add_attribute(Word object,const char *selector,Word key,Word value,struct GuestRange range) {
    if (!key || !value) stop("Nil attributed string attribute",selector);
    struct GuestMap *attributes=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary")); map_set(attributes,selector,value,key);
    attributed_run_add(attributed_state(object),(Word)attributes,range,0,selector); free(attributes->entries); free(attributes);
}
static Word attributed_attributes(Word object,const char *selector,size_t index,struct GuestRange *effective) {
    struct AttributedState *state=attributed_state(object); size_t length=string_length(state->string,selector);
    if (index>=length) stop("Attributed string index out of bounds",selector);
    struct GuestMap *result=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary")); size_t begin=0,end=length;
    for (size_t i=0;i<state->count;++i) {
        struct AttributedRun *run=&state->runs[i]; size_t stop_at=run->range.location+run->range.length;
        if (run->range.location<=index && index<stop_at) {
            if (run->reset) dictionary_clear(result);
            dictionary_merge(result,selector,(void *)run->attributes);
        }
        size_t edges[2]={run->range.location,stop_at};
        for (unsigned j=0;j<2;++j) { if (edges[j]<=index && edges[j]>begin) begin=edges[j]; if (edges[j]>index && edges[j]<end) end=edges[j]; }
    }
    if (effective) *effective=(struct GuestRange){begin,end-begin}; return (Word)result;
}
static Word attributed_attribute(Word object,const char *selector,Word key,size_t index,struct GuestRange *effective) {
    struct GuestMap *attributes=(void *)attributed_attributes(object,selector,index,effective); Word result=map_get(attributes,selector,key);
    free(attributes->entries); free(attributes); return result;
}
static Word attributed_init_copy(Word object,const char *selector,Word source) {
    struct AttributedState *old=attributed_state(source),*state=attributed_state(object);
    state->string=text_copy(old->string,"copy"); state->count=0;
    for (size_t i=0;i<old->count;++i) attributed_run_add(state,old->runs[i].attributes,old->runs[i].range,old->runs[i].reset,selector);
    return object;
}
static Word attributed_copy(Word object,const char *selector) {
    struct HostObject *copy=calloc(1,sizeof(*copy)); if (!copy) stop("Allocation failed",selector);
    copy->isa=external_class(!strcmp(selector,"mutableCopy") ? "_OBJC_CLASS_$_NSMutableAttributedString" : "_OBJC_CLASS_$_NSAttributedString");
    return attributed_init_copy((Word)copy,selector,object);
}
static void attributed_append(Word object,const char *selector,Word source) {
    struct AttributedState *state=attributed_state(object),*old=attributed_state(source);
    size_t offset=string_length(state->string,selector),count=old->count;
    Word text=text_copy(state->string,"mutableCopy"); append_string((void *)text,selector,old->string); state->string=text;
    for (size_t i=0;i<count;++i) { struct AttributedRun run=old->runs[i]; run.range.location+=offset; attributed_run_add(state,run.attributes,run.range,run.reset,selector); }
}
static struct GuestSize ui_string_size(Word,const char *,Word);
static Method ui_text_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if(!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_UIApplication") && !strcmp(selector,"preferredContentSizeCategory"))return (Method)ui_preferred_content_category;
    Word cls=class_receiver ? object : read_word(object);
    if(!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UISearchBar"))){
        if(!strcmp(selector,"init"))return (Method)search_bar_initialize_plain;if(!strcmp(selector,"initWithFrame:"))return (Method)search_bar_initialize;
        const char *getters[]={"text","placeholder","prompt","delegate","tintColor","barTintColor","showsCancelButton","showsScopeBar","scopeButtonTitles","selectedScopeButtonIndex","barStyle","searchBarStyle","isTranslucent","searchTextField"};const char *setters[]={"setText:","setPlaceholder:","setPrompt:","setDelegate:","setTintColor:","setBarTintColor:","setShowsCancelButton:","setShowsCancelButton:animated:","setShowsScopeBar:","setScopeButtonTitles:","setSelectedScopeButtonIndex:","setBarStyle:","setSearchBarStyle:","setTranslucent:"};for(unsigned i=0;i<COUNT(getters);++i)if(!strcmp(selector,getters[i]))return (Method)search_bar_get;for(unsigned i=0;i<COUNT(setters);++i)if(!strcmp(selector,setters[i]))return (Method)search_bar_set;
        const char *input[]={"keyboardType","autocapitalizationType","autocorrectionType","spellCheckingType","returnKeyType","secureTextEntry"};for(unsigned i=0;i<COUNT(input);++i){if(!strcmp(selector,input[i]))return (Method)search_bar_input_get;char setter[80];snprintf(setter,sizeof(setter),"set%c%s:",(char)(input[i][0]-'a'+'A'),input[i]+1);if(!strcmp(selector,setter))return (Method)search_bar_input_set;}
        if(!strcmp(selector,"becomeFirstResponder") || !strcmp(selector,"resignFirstResponder") || !strcmp(selector,"isFirstResponder") || !strcmp(selector,"canBecomeFirstResponder") || !strcmp(selector,"canResignFirstResponder"))return (Method)search_bar_first;
        if(!strcmp(selector,"sizeThatFits:"))return (Method)search_bar_size;if(!strcmp(selector,"cancelSearch:"))return (Method)search_bar_cancel;if(!strcmp(selector,"selectSearchScope:"))return (Method)search_scope_action;
    }
    if(!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UISearchController"))){if(!strcmp(selector,"initWithSearchResultsController:"))return (Method)search_controller_init;if(!strcmp(selector,"init"))return (Method)search_controller_plain;
        const char *getters[]={"searchBar","searchResultsController","searchResultsUpdater","delegate","isActive","dimsBackgroundDuringPresentation","obscuresBackgroundDuringPresentation","hidesNavigationBarDuringPresentation"},*setters[]={"setSearchResultsUpdater:","setDelegate:","setActive:","setDimsBackgroundDuringPresentation:","setObscuresBackgroundDuringPresentation:","setHidesNavigationBarDuringPresentation:"};for(unsigned i=0;i<COUNT(getters);++i)if(!strcmp(selector,getters[i]))return (Method)search_controller_get;for(unsigned i=0;i<COUNT(setters);++i)if(!strcmp(selector,setters[i]))return (Method)search_controller_set;
    }
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableString")) && !strcmp(selector,"sizeWithFont:")) return (Method)ui_string_size;
    if (!class_receiver && (!strcmp(receiver,"_OBJC_CLASS_$_NSAttributedString") || !strcmp(receiver,"_OBJC_CLASS_$_NSMutableAttributedString"))) {
        if (!strcmp(selector,"initWithString:attributes:")) return (Method)attributed_init;
        if (!strcmp(selector,"initWithString:")) return (Method)attributed_init_string;
        if (!strcmp(selector,"initWithAttributedString:")) return (Method)attributed_init_copy;
        if (!strcmp(selector,"string")) return (Method)attributed_string;
        if (!strcmp(selector,"length")) return (Method)attributed_length;
        if (!strcmp(selector,"attributesAtIndex:effectiveRange:")) return (Method)attributed_attributes;
        if (!strcmp(selector,"attribute:atIndex:effectiveRange:")) return (Method)attributed_attribute;
        if (!strcmp(selector,"copy") || !strcmp(selector,"mutableCopy")) return (Method)attributed_copy;
        if (!strcmp(receiver,"_OBJC_CLASS_$_NSMutableAttributedString")) {
            if (!strcmp(selector,"addAttributes:range:") || !strcmp(selector,"setAttributes:range:")) return (Method)attributed_add_attributes;
            if (!strcmp(selector,"addAttribute:value:range:")) return (Method)attributed_add_attribute;
            if (!strcmp(selector,"appendAttributedString:")) return (Method)attributed_append;
        }
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIImage")) {
        if (class_receiver && !strcmp(selector,"imageWithData:scale:")) return (Method)ui_image_data_scale;
        if (class_receiver && !strcmp(selector,"imageWithContentsOfFile:")) return (Method)ui_image_file;
        if (!class_receiver && !strcmp(selector,"initWithData:")) return (Method)ui_image_init_data;
        if (!class_receiver && !strcmp(selector,"initWithData:scale:")) return (Method)ui_image_init_data_scale;
        if (!class_receiver && (!strcmp(selector,"CGImage") || !strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:"))) return (Method)identity;
        if (!class_receiver && !strcmp(selector,"hash")) return (Method)ui_image_hash;
        if (!class_receiver && !strcmp(selector,"drawInRect:")) return (Method)ui_image_draw;
        if (!class_receiver && !strcmp(selector,"drawAtPoint:")) return (Method)ui_image_draw_point;
        if (class_receiver && !strcmp(selector,"imageNamed:")) return (Method)ui_image_named;
        if (class_receiver && !strcmp(selector,"imageNamed:inBundle:compatibleWithTraitCollection:")) return (Method)ui_image_named_bundle;
        if (!class_receiver && !strcmp(selector,"imageWithRenderingMode:")) return (Method)ui_image_rendering;
        if (!class_receiver && !strcmp(selector,"renderingMode")) return (Method)ui_image_mode;
        if (!class_receiver && !strcmp(selector,"scale")) return (Method)ui_image_scale;
        if (!class_receiver && !strcmp(selector,"size")) return (Method)ui_image_size;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_NSShadow")) {
        if (!strcmp(selector,"setShadowColor:")) return (Method)ui_shadow_color;
        if (!strcmp(selector,"shadowColor")) return (Method)ui_shadow_get_color;
        if (!strcmp(selector,"setShadowOffset:")) return (Method)ui_shadow_offset;
        if (!strcmp(selector,"shadowOffset")) return (Method)ui_shadow_get_offset;
        if (!strcmp(selector,"setShadowBlurRadius:")) return (Method)ui_shadow_blur;
        if (!strcmp(selector,"shadowBlurRadius")) return (Method)ui_shadow_get_blur;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIColor") && !class_receiver) {
        if (!strcmp(selector,"set") || !strcmp(selector,"setFill") || !strcmp(selector,"setStroke")) return (Method)ui_color_set;
        if (!strcmp(selector,"CGColor")) return (Method)identity;
        if (!strcmp(selector,"getRed:green:blue:alpha:")) return (Method)ui_color_get_rgba;
        if (!strcmp(selector,"colorWithAlphaComponent:")) return (Method)ui_color_alpha;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIColor") && class_receiver) {
        if (!strcmp(selector,"colorWithWhite:alpha:")) return (Method)ui_color_white;
        if (!strcmp(selector,"colorWithRed:green:blue:alpha:")) return (Method)ui_color_rgba;
        if (!strcmp(selector,"colorWithCGColor:")) return (Method)ui_color_from_cg;
        if (!strcmp(selector,"blackColor")) return (Method)ui_color_black;
        if (!strcmp(selector,"clearColor")) return (Method)ui_color_clear;
        const char *names[]={"whiteColor","lightGrayColor","grayColor","darkGrayColor","redColor","greenColor","blueColor","yellowColor","cyanColor","magentaColor","orangeColor","purpleColor","brownColor"};
        for (unsigned i=0;i<COUNT(names);++i) if (!strcmp(selector,names[i])) return (Method)ui_color_named;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIFontDescriptor")) {
        if (class_receiver && !strcmp(selector,"fontDescriptorWithFontAttributes:")) return (Method)ui_font_descriptor;
        if (class_receiver && !strcmp(selector,"preferredFontDescriptorWithTextStyle:")) return (Method)ui_font_preferred_descriptor;
        if (!class_receiver && !strcmp(selector,"fontAttributes")) return (Method)ui_font_attributes;
        if (!class_receiver && !strcmp(selector,"objectForKey:")) return (Method)ui_font_attribute;
        if (!class_receiver && !strcmp(selector,"pointSize")) return (Method)ui_font_descriptor_size;
        if (!class_receiver && !strcmp(selector,"symbolicTraits")) return (Method)ui_font_symbolic_traits;
        if (!class_receiver && !strcmp(selector,"fontDescriptorWithSymbolicTraits:")) return (Method)ui_font_with_traits;
        if (!class_receiver && !strcmp(selector,"fontDescriptorByAddingAttributes:")) return (Method)ui_font_with_attributes;
        if (!class_receiver && !strcmp(selector,"fontDescriptorWithSize:")) return (Method)ui_font_with_size;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIFont")) {
        if (class_receiver && (!strcmp(selector,"systemFontOfSize:") || !strcmp(selector,"boldSystemFontOfSize:") || !strcmp(selector,"italicSystemFontOfSize:"))) return (Method)ui_font_system;
        if (class_receiver && !strcmp(selector,"fontWithName:size:")) return (Method)ui_font_named;
        if (class_receiver && !strcmp(selector,"fontWithDescriptor:size:")) return (Method)ui_font_create;
        if (class_receiver && !strcmp(selector,"preferredFontForTextStyle:")) return (Method)ui_font_preferred;
        if (class_receiver && (!strcmp(selector,"buttonFontSize") || !strcmp(selector,"labelFontSize") || !strcmp(selector,"systemFontSize") || !strcmp(selector,"smallSystemFontSize"))) return (Method)ui_font_standard_size;
        if (!class_receiver && !strcmp(selector,"fontDescriptor")) return (Method)ui_font_get_descriptor;
        if (!class_receiver && !strcmp(selector,"pointSize")) return (Method)ui_font_size;
        if (!class_receiver && (!strcmp(selector,"fontName") || !strcmp(selector,"familyName"))) return (Method)ui_font_name;
        if (!class_receiver && !strcmp(selector,"fontWithSize:")) return (Method)ui_font_resize;
        const char *metrics[]={"lineHeight","ascender","descender","leading","capHeight","xHeight"};
        if (!class_receiver) for (unsigned i=0;i<COUNT(metrics);++i) if (!strcmp(selector,metrics[i])) return (Method)ui_font_metric;
    }
    const char *base=class_receiver ? receiver : external_base(read_word(object));
    if(!class_receiver && class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UITextView"))){
        if(!strcmp(selector,"init"))return (Method)text_editor_plain;if(!strcmp(selector,"initWithFrame:"))return (Method)text_editor_frame;if(!strcmp(selector,"initWithFrame:textContainer:"))return (Method)text_editor_init;
        const char *common[]={"text","textColor","font","returnKeyType","autocapitalizationType","autocorrectionType","textAlignment","delegate","keyboardType","spellCheckingType"};
        if(paragraph_property(selector,common,COUNT(common),0)>=0)return (Method)text_field_get;
        if(paragraph_property(selector,common,COUNT(common),1)>=0)return (Method)text_field_set;
        if(!strcmp(selector,"attributedText"))return (Method)text_editor_get_attributed;if(!strcmp(selector,"setAttributedText:"))return (Method)text_editor_attributed;
        if(!strcmp(selector,"textContainerInset"))return (Method)text_editor_insets;if(!strcmp(selector,"setTextContainerInset:"))return (Method)text_editor_set_insets;
        if(!strcmp(selector,"selectedRange"))return (Method)text_editor_selection;if(!strcmp(selector,"setSelectedRange:"))return (Method)text_editor_set_selection;
        if(!strcmp(selector,"isEditable") || !strcmp(selector,"editable") || !strcmp(selector,"isSelectable") || !strcmp(selector,"selectable") || !strcmp(selector,"dataDetectorTypes"))return (Method)text_editor_property;
        if(!strcmp(selector,"setEditable:") || !strcmp(selector,"setSelectable:") || !strcmp(selector,"setDataDetectorTypes:"))return (Method)text_editor_set_property;
    }
    if (!class_receiver && base && !strcmp(base,"_OBJC_CLASS_$_NSTextAttachment")) {
        if (!strcmp(selector,"initWithData:ofType:")) return (Method)ui_attachment_init;
        if (!strcmp(selector,"setImage:")) return (Method)ui_attachment_image;
        if (!strcmp(selector,"image")) return (Method)ui_attachment_get_image;
    }
    if (!class_receiver && base && !strcmp(base,"_OBJC_CLASS_$_UITextField")) {
        if (text_field_property(selector,0)>=0) return (Method)text_field_get;
        if (text_field_property(selector,1)>=0) return (Method)text_field_set;
    }
    return NULL;
}
