/* UIKit keeps ownership of content and positions. Android lays out the same
 * attributed runs and native attachment bounds for both sizeToFit and drawing. */
struct LabelState { Word object,text,attributed,font,color,shadow,synced_text,synced_owner; jobject rich_text; long alignment,lines,line_break,baseline; double preferred_width,minimum_scale;int adjusts,synced; float metrics[17]; jint options[10]; struct LabelState *next;
#if I2A_DEVELOPMENT
    unsigned diagnostic_traces;
#endif
};
struct ControlAppearance { Word state,title,image,color; struct ControlAppearance *next; };
struct ControlAction { Word target,events; const char *selector; struct ControlAction *next; };
struct ControlState { Word object,label,image,image_view,empty_title,default_color,synced_image,synced_accessibility,indicator_color,synced_owner; unsigned button,type,enabled,selected,highlighted,synced,indicator,animating,hides; struct GuestRect content_insets,title_insets,image_insets; struct ControlAppearance *appearances; struct ControlAction *actions; float metrics[14]; jint options[11]; };
static Word image_view_init(Word,const char *,Word);
static void ui_control_sync(Word);
static struct LabelState *label_states;

static struct LabelState *label_state(Word object) {
    for (struct LabelState *s=label_states;s;s=s->next) if (s->object==object) return s;
    struct LabelState *s=calloc(1,sizeof(*s)); if (!s) stop("Allocation failed","UILabel");
    s->object=object; s->alignment=4; s->lines=1; s->line_break=4; s->text=copy_string("");
    s->font=ui_font_system(external_class("_OBJC_CLASS_$_UIFont"),"systemFontOfSize:",17); s->color=ui_color_black(external_class("_OBJC_CLASS_$_UIColor"),"blackColor");
    s->next=label_states; label_states=s; return s;
}
static int label_object(Word object) { const char *base=object ? external_base(read_word(object)) : NULL; return base && !strcmp(base,"_OBJC_CLASS_$_UILabel"); }
static void label_collect_order(Word object,struct Sequence *order) {
    if (label_object(object) || view_state(object)->control || view_state(object)->web || view_state(object)->input || view_state(object)->table) sequence_add(order,"UIView drawing order",object);
    if(view_state(object)->layer)layer_collect_order(view_state(object)->layer,order);
    struct Sequence *children=(void *)view_state(object)->children;
    for (size_t i=0;i<children->count;++i) label_collect_order(children->items[i],order);
}
static void ui_view_order_sync(Word object) {
    while (view_state(object)->parent && !view_state(object)->host_container) object=view_state(object)->parent;
    struct Sequence *order=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray")); label_collect_order(object,order);
    if (order->count) {
        JNIEnv *env=attached_environment("UIView drawing order"); static jmethodID method;
        if (!method) { jclass cls=(*env)->GetObjectClass(env,activity); method=(*env)->GetMethodID(env,cls,"reorderGuestLabels","([J)V"); (*env)->DeleteLocalRef(env,cls); }
        jlongArray values=(*env)->NewLongArray(env,order->count); (*env)->SetLongArrayRegion(env,values,0,order->count,(const jlong *)order->items);
        (*env)->CallVoidMethod(env,activity,method,values); (*env)->DeleteLocalRef(env,values);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); free(order->items); free(order); stop("Android view reordering failed","UIView"); }
    }
    free(order->items); free(order);
}
static unsigned label_argb(Word value,unsigned fallback) {
    if (!value || read_word(value)!=external_class("_OBJC_CLASS_$_UIColor")) return fallback;
    struct UIKitColor *c=(void *)value;
    return ((unsigned)lrint(fmin(1,fmax(0,c->alpha))*255)<<24)|((unsigned)lrint(fmin(1,fmax(0,c->red))*255)<<16)|((unsigned)lrint(fmin(1,fmax(0,c->green))*255)<<8)|(unsigned)lrint(fmin(1,fmax(0,c->blue))*255);
}
static void label_content_changed(struct LabelState *state) {
    if (state->rich_text) { JNIEnv *env=attached_environment("UILabel content"); (*env)->DeleteGlobalRef(env,state->rich_text); state->rich_text=NULL; }
    state->synced=0;
    if (state->object) view_request_layout(state->object,"intrinsicContentSize");
}
static Word label_attribute(struct GuestMap *attributes,const char *name) {
     
    static const char *names[]={"NSFont","NSColor","NSAttachment","NSParagraphStyle","NSKern","NSBaselineOffset","NSLink"};
    static Word keys[COUNT(names)];
    for (unsigned i=0;i<COUNT(names);++i) if (!strcmp(name,names[i])) {
        if (!keys[i]) keys[i]=copy_string(name);
        return attributes ? map_get(attributes,"UILabel attribute",keys[i]) : 0;
    }
    return 0;
}
static jobject label_rich_text(JNIEnv *env,jclass cls,struct LabelState *state) {
    if (state->rich_text) return state->rich_text;
    jmethodID create=(*env)->GetMethodID(env,cls,"createGuestText","(Ljava/lang/String;)Landroid/text/SpannableStringBuilder;");
    jmethodID add=(*env)->GetMethodID(env,cls,"addGuestTextRun","(Landroid/text/SpannableStringBuilder;IILjava/lang/String;IFIIFFF)F");
    jmethodID image=(*env)->GetMethodID(env,cls,"addGuestTextAttachment","(Landroid/text/SpannableStringBuilder;I[B[FZ)V");
    jstring plain=http_java_string(env,state->text);
    jobject result=(*env)->CallObjectMethod(env,activity,create,plain); if (plain) (*env)->DeleteLocalRef(env,plain);
    struct TextView text=text_view(state->text,"UILabel rich text");
    double scale=screen_scale();
    for (size_t index=0;index<text.length;) {
        struct GuestRange range={index,text.length-index};
        struct GuestMap *attributes=state->attributed ? (void *)attributed_attributes(state->attributed,"UILabel rich text",index,&range) : NULL;
        struct UIKitFont *font=(void *)label_attribute(attributes,"NSFont"); if (!font) font=(void *)state->font;
        Word color=label_attribute(attributes,"NSColor"),paragraph=label_attribute(attributes,"NSParagraphStyle"),attachment=label_attribute(attributes,"NSAttachment");
        Word kern=label_attribute(attributes,"NSKern"),baseline=label_attribute(attributes,"NSBaselineOffset");
        struct UIKitFontDescriptor *descriptor=font ? (void *)font->descriptor : NULL;
        Word name=descriptor ? ui_font_attribute(descriptor,"font family",copy_string("NSFontNameAttribute")) : 0;
        if (!name && descriptor) name=ui_font_attribute(descriptor,"font family",copy_string("NSFontFamilyAttribute"));
        jstring family=name ? http_java_string(env,name) : (*env)->NewStringUTF(env,"sans-serif");
        struct ParagraphState *p=paragraph ? paragraph_state(paragraph) : NULL;
        float height=(*env)->CallFloatMethod(env,activity,add,result,(jint)index,(jint)(range.location+range.length),family,
            (jint)(descriptor ? ui_font_symbolic_traits(descriptor) : 0),(jfloat)((font ? font->size : 17)*scale),
            (jint)label_argb(color ? color : state->color,0xff000000),(jint)(p ? p->integers[0] : state->alignment),
            (jfloat)(p ? p->metrics[0]*scale : 0),(jfloat)(kern ? numeric_double((void *)kern)*scale : 0),(jfloat)(baseline ? numeric_double((void *)baseline)*scale : 0));
        (*env)->DeleteLocalRef(env,family);
        Word link=label_attribute(attributes,"NSLink");
        if (link) {
            Method absolute=find_implementation(link,"absoluteString");
            Word address=absolute ? absolute(link,"absoluteString",0,0,0,0) : link;
            jstring url=http_java_string(env,address);
            static jmethodID add_link;
            if(!add_link)add_link=(*env)->GetMethodID(env,cls,"addGuestTextLink","(Landroid/text/SpannableStringBuilder;IILjava/lang/String;)V");
            (*env)->CallVoidMethod(env,activity,add_link,result,(jint)index,(jint)(range.location+range.length),url);
            if (url) (*env)->DeleteLocalRef(env,url);
        }
        if (attachment) {
            Word original=text_attachment(attachment)->image;
            struct HostObject *png=(void *)image_png((void *)original);
            if (original && png) {
                const char *selector="attachmentBoundsForTextContainer:proposedLineFragment:glyphPosition:characterIndex:";
                Method bounds_method=find_implementation(attachment,selector);
                struct ImageSize natural=ui_image_size(original);
                for (size_t i=index;i<range.location+range.length;++i) if (text.units[i]==0xfffc) {
                    struct GuestRect bounds=view_state(attachment)->bounds;
                    if (!bounds.width || !bounds.height) bounds=(struct GuestRect){0,0,natural.width,natural.height};
                    if (bounds_method) bounds=((struct GuestRect (*)(Word,const char *,Word,struct GuestRect,struct GuestPoint,Word))bounds_method)(attachment,selector,0,(struct GuestRect){0,0,32768,height/scale},(struct GuestPoint){0,0},i);
                    jfloat b[]={(jfloat)(bounds.x*scale),(jfloat)(bounds.y*scale),(jfloat)(bounds.width*scale),(jfloat)(bounds.height*scale)};
                    jfloatArray box=(*env)->NewFloatArray(env,4); (*env)->SetFloatArrayRegion(env,box,0,4,b);
                    jbyteArray bytes=(*env)->NewByteArray(env,png->content[0]); (*env)->SetByteArrayRegion(env,bytes,0,png->content[0],(void *)data_buffer(png));
                    (*env)->CallVoidMethod(env,activity,image,result,(jint)i,bytes,box,(jboolean)(ui_image_mode(original)==2));
                    (*env)->DeleteLocalRef(env,bytes); (*env)->DeleteLocalRef(env,box);
                }
            }
        }
        index=range.location+range.length;
        if (attributes) { free(attributes->entries); free(attributes); }
    }
    free(text.owned);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Android attributed text failed","UILabel"); }
    state->rich_text=(*env)->NewGlobalRef(env,result); (*env)->DeleteLocalRef(env,result); return state->rich_text;
}
static struct LabelState *text_editor_label(struct TextFieldState *field) {
    struct LabelState *label=label_state(field->object);
    if(label->text!=field->properties[0] || label->font!=field->properties[3] || label->color!=field->properties[2] || label->attributed!=field->attributed || label->alignment!=(long)field->properties[11]){
        label->text=field->properties[0];label->font=field->properties[3];label->color=field->properties[2];label->attributed=field->attributed;label->alignment=field->properties[11];label->lines=0;label->line_break=0;label_content_changed(label);
    }
    return label;
}
static void ui_input_sync(Word object) {
    struct ViewState *view=view_state(object);struct TextFieldState *field=view->input;if(!field || field->closed)return;
    float metrics[21]={0};int options[23]={0};Word owner=view_host_geometry(object,metrics,options);double scale=screen_scale();struct UIKitFont *font=(void *)field->properties[3];
    metrics[11]=(font ? font->size : 12)*scale;metrics[12]=(field->insets.left+5)*scale;metrics[13]=field->insets.top*scale;metrics[14]=(field->insets.right+5)*scale;metrics[15]=field->insets.bottom*scale;metrics[18]=scale;
    options[3]=field->editable;options[4]=field->selectable;options[5]=field->properties[10];options[6]=field->properties[14];options[7]=field->properties[13];options[8]=field->properties[6];options[9]=field->properties[9];options[10]=field->properties[15];options[11]=(int)(field->detection&7);options[12]=field->properties[11];options[13]=label_argb(field->properties[2],0xff000000);options[14]=label_argb(view->background,0xffffffff);options[15]=field->selection.location;options[16]=field->selection.location+field->selection.length;
    ui_scroll_input_options(object,metrics,options);
    options[22]=!field->multiline;
    if(!options[1] && (!field->synced || !field->options[1]))return;
    if(field->synced && field->synced_owner==owner && field->synced_revision==field->revision && !memcmp(field->metrics,metrics,sizeof(metrics)) && !memcmp(field->options,options,sizeof(options)))return;
    JNIEnv *env=attached_environment("Text input sync");static jmethodID method;if(!method){jclass type=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,type,"syncGuestEditor","(JJLjava/lang/CharSequence;Ljava/lang/String;[F[I)V");(*env)->DeleteLocalRef(env,type);}
    jclass cls=(*env)->GetObjectClass(env,activity);jobject text=field->synced_revision!=field->revision ? label_rich_text(env,cls,text_editor_label(field)) : NULL;(*env)->DeleteLocalRef(env,cls);
    jfloatArray m=(*env)->NewFloatArray(env,COUNT(metrics));jintArray o=(*env)->NewIntArray(env,COUNT(options));(*env)->SetFloatArrayRegion(env,m,0,COUNT(metrics),metrics);(*env)->SetIntArrayRegion(env,o,0,COUNT(options),options);
    jstring hint=http_java_string(env,field->properties[1]);(*env)->CallVoidMethod(env,activity,method,(jlong)object,(jlong)owner,text,hint,m,o);if(hint)(*env)->DeleteLocalRef(env,hint);(*env)->DeleteLocalRef(env,m);(*env)->DeleteLocalRef(env,o);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Android text input failed","UITextView");}
    field->synced=1;field->synced_owner=owner;field->synced_revision=field->revision;memcpy(field->metrics,metrics,sizeof(metrics));memcpy(field->options,options,sizeof(options));
}
static void ui_label_sync(Word object) {
    if (!label_object(object)) {
        struct ViewState *state=view_state(object);
        if (state->needs_display && !state->display_pending && !state->display_running && !state->hidden && view_window(object)) ui_view_request_display(object,"Initial display");
        ui_control_sync(object); return;
    }
    struct LabelState *s=label_state(object); struct ViewState *v=view_state(object),*layer=v->layer ? view_state(v->layer) : NULL;
    float geometry[11];int visibility[3];Word owner=view_host_geometry(object,geometry,visibility);
    double scale=screen_scale(),x=geometry[0]/scale,y=geometry[1]/scale,alpha=geometry[4];int visible=visibility[0],attached=visibility[1];
    struct UIKitFont *font=s->font ? (void *)s->font : NULL; struct UIKitShadow *shadow=s->shadow ? (void *)s->shadow : NULL;
    float metrics[]={ (float)((font ? font->size : 17)*scale),(float)(x*scale),(float)(y*scale),(float)(v->frame.width*scale),(float)(v->frame.height*scale),(float)alpha,
        shadow ? (float)(shadow->blur*scale) : 0,shadow ? (float)(shadow->offset.width*scale) : 0,shadow ? (float)(shadow->offset.height*scale) : 0,
        layer ? (float)(layer->corner*scale) : 0,layer ? (float)layer->z : 0,(float)view_window_level(object),geometry[7],geometry[8],geometry[9],geometry[10],(float)s->minimum_scale };
    jint options[]={ (jint)label_argb(s->color,0xff000000),(jint)s->alignment,(jint)s->lines,(jint)label_argb(v->background,0),visible && attached,s->adjusts,
        shadow ? (jint)label_argb(shadow->color,0x55000000) : 0,attached,(jint)s->line_break,(jint)s->baseline };
     
    if (s->synced && s->synced_owner==owner && s->synced_text==s->text && !memcmp(s->metrics,metrics,sizeof(metrics)) && !memcmp(s->options,options,sizeof(options))) return;
    if (!attached && (!s->synced || !s->options[7])) return;
    s->synced=1; s->synced_owner=owner; s->synced_text=s->text;
    memcpy(s->metrics,metrics,sizeof(metrics)); memcpy(s->options,options,sizeof(options));
#if I2A_DEVELOPMENT
    static unsigned label_traces;
    if (business_labels_requested && visible && alpha>0.5 && s->diagnostic_traces<2 && label_traces<48) {
        ++s->diagnostic_traces; ++label_traces;
        LOG("[DIAG-label] object=%p parent=%p attached=%d visible=%d alpha=%g rect=%g,%g,%g,%g chars=%lu color=%08x",(void *)object,(void *)v->parent,attached,visible,alpha,x,y,v->frame.width,v->frame.height,string_length(s->text,"length"),options[0]);
        LOG("[DIAG-label] font=%g scale=%g lines=%ld adjusts=%d local=%g,%g,%g,%g",font ? font->size : 17,scale,s->lines,s->adjusts,v->frame.x,v->frame.y,v->frame.width,v->frame.height);
        if (runtime_extension.label_diagnostic) runtime_extension.label_diagnostic(object);
        for (Word parent=v->parent;parent;parent=view_state(parent)->parent)
            LOG("[DIAG-label] ancestor=%p class=%s frame=%g,%g,%g,%g root=%d",(void *)parent,class_name(read_word(parent)),view_state(parent)->frame.x,view_state(parent)->frame.y,view_state(parent)->frame.width,view_state(parent)->frame.height,root_controller && parent==controller_loaded_view(root_controller));
    }
#endif
    JNIEnv *env=attached_environment("UILabel sync"); jclass cls=(*env)->GetObjectClass(env,activity);
    static jmethodID sync;if(!sync)sync=(*env)->GetMethodID(env,cls,"syncGuestLabel","(JJLjava/lang/CharSequence;[F[I)V");
    jfloatArray m=(*env)->NewFloatArray(env,COUNT(metrics)); jintArray o=(*env)->NewIntArray(env,COUNT(options));
    (*env)->SetFloatArrayRegion(env,m,0,COUNT(metrics),metrics); (*env)->SetIntArrayRegion(env,o,0,COUNT(options),options);
    jobject text=label_rich_text(env,cls,s); (*env)->CallVoidMethod(env,activity,sync,(jlong)object,(jlong)owner,text,m,o);
    (*env)->DeleteLocalRef(env,m); (*env)->DeleteLocalRef(env,o); (*env)->DeleteLocalRef(env,cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Android label update failed","UILabel"); }
}
static void label_text(Word object,const char *selector,Word value) { struct LabelState *s=label_state(object); s->text=value ? text_copy(value,"copy") : copy_string(""); s->attributed=0; label_content_changed(s); ui_label_sync(object); }
static Word label_get_text(Word object) { return label_state(object)->text; }
static void label_attributed(Word object,const char *selector,Word value) {
    struct LabelState *s=label_state(object); label_content_changed(s); s->attributed=value; s->text=value ? attributed_string(value) : copy_string("");
    if (value && attributed_length(value,selector)) {
        struct GuestMap *attrs=(void *)attributed_attributes(value,selector,0,NULL);
        s->font=map_get(attrs,selector,copy_string("NSFont")); s->color=map_get(attrs,selector,copy_string("NSColor")); s->shadow=map_get(attrs,selector,copy_string("NSShadow"));
        Word paragraph=map_get(attrs,selector,copy_string("NSParagraphStyle")); if (paragraph) s->alignment=paragraph_state(paragraph)->integers[0];
        free(attrs->entries); free(attrs);
    }
    ui_label_sync(object);
}
static Word label_get_attributed(Word object) { return label_state(object)->attributed; }
static void label_font(Word object,const char *selector,Word value) { struct LabelState *s=label_state(object); s->font=value; label_content_changed(s); ui_label_sync(object); }
static Word label_get_font(Word object) { return label_state(object)->font; }
static void label_color(Word object,const char *selector,Word value) { struct LabelState *s=label_state(object); s->color=value; label_content_changed(s); ui_label_sync(object); }
static Word label_get_color(Word object) { return label_state(object)->color; }
static void label_alignment(Word object,const char *selector,long value) { label_state(object)->alignment=value; ui_label_sync(object); }
static Word label_get_alignment(Word object) { return label_state(object)->alignment; }
static void label_lines(Word object,const char *selector,long value) { struct LabelState *state=label_state(object); if (value<0 || (value!=1 && (state->line_break==3 || state->line_break==5))) stop("Unsupported label line count",selector); state->lines=value; view_request_layout(object,selector); ui_label_sync(object); }
static Word label_get_lines(Word object) { return label_state(object)->lines; }
static void label_adjusts(Word object,const char *selector,Word value) { label_state(object)->adjusts=value!=0; ui_label_sync(object); }
static Word label_get_adjusts(Word object) { return label_state(object)->adjusts; }
static void label_set_baseline(Word object,const char *selector,Word value) { if(value>2)stop("Invalid baseline adjustment",selector);label_state(object)->baseline=(long)value;ui_label_sync(object); }
static Word label_baseline(Word object) { return (Word)label_state(object)->baseline; }
static void label_set_minimum_scale(Word object,const char *selector,double value) { if(!isfinite(value) || value<0 || value>1)stop("Invalid minimum label scale",selector);label_state(object)->minimum_scale=value;ui_label_sync(object); }
static double label_minimum_scale(Word object) { return label_state(object)->minimum_scale; }
static void label_line_break(Word object,const char *selector,Word value) { struct LabelState *state=label_state(object); if (value>5 || value==1 || (state->lines!=1 && (value==3 || value==5))) stop("Unsupported label wrapping mode",selector); state->line_break=value; view_request_layout(object,selector); ui_label_sync(object); }
static Word label_get_line_break(Word object) { return label_state(object)->line_break; }
static void label_set_preferred_width(Word object,const char *selector,double width) { if(!isfinite(width) || width<0)stop("Invalid preferred label width",selector);struct LabelState *state=label_state(object);if(state->preferred_width==width)return;state->preferred_width=width;view_request_layout(object,selector); }
static double label_preferred_width(Word object) { return label_state(object)->preferred_width; }
#if I2A_DEVELOPMENT
JNIEXPORT void JNICALL HOST_JNI(HostActivity_dumpLabelDiagnostics)(JNIEnv *env,jobject activity) {
    unsigned count=0,total=0,attached=0;
    for (struct LabelState *s=label_states;s;s=s->next) {
        ++total;
        if (!view_window(s->object)) continue;
        ++attached;
        if (count>=128) continue;
        ++count;
        LOG("[DIAG-label-current] object=%p synced=%d visible=%d alpha=%g z=%g chars=%lu pixelRect=%g,%g,%g,%g",(void *)s->object,s->synced,s->options[4],s->metrics[5],s->metrics[10],string_length(s->text,"length"),s->metrics[1],s->metrics[2],s->metrics[3],s->metrics[4]);
        if (runtime_extension.label_diagnostic) runtime_extension.label_diagnostic(s->object);
        for (Word object=s->object;object;object=view_state(object)->parent) {
            struct ViewState *v=view_state(object);
            LOG("[DIAG-label-current] view=%p hidden=%d alpha=%g frame=%g,%g,%g,%g bounds=%g,%g,%g,%g",(void *)object,v->hidden,v->alpha,v->frame.x,v->frame.y,v->frame.width,v->frame.height,v->bounds.x,v->bounds.y,v->bounds.width,v->bounds.height);
        }
    }
    LOG("[DIAG-label-current] reported=%u attached=%u total=%u",count,attached,total);
    if (runtime_extension.scene_diagnostic) runtime_extension.scene_diagnostic();
}
#endif
static struct GuestSize label_measure(struct LabelState *s,const char *selector,struct GuestSize constraint) {
    struct UIKitFont *font=s->font ? (void *)s->font : NULL;
    double scale=screen_scale(); JNIEnv *env=attached_environment(selector); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID measure=(*env)->GetMethodID(env,cls,"measureGuestText","(Ljava/lang/CharSequence;FFII)[F"); jobject text=label_rich_text(env,cls,s);
    jfloatArray measured=(*env)->CallObjectMethod(env,activity,measure,text,(jfloat)((font ? font->size : 17)*scale),(jfloat)(constraint.width*scale),(jint)s->lines,(jint)s->line_break);
    jfloat values[2]={0}; if (measured && (*env)->GetArrayLength(env,measured)==2) (*env)->GetFloatArrayRegion(env,measured,0,2,values);
    if (measured) (*env)->DeleteLocalRef(env,measured); (*env)->DeleteLocalRef(env,cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); stop("Android label measurement failed",selector); }
    return (struct GuestSize){values[0]/scale,values[1]/scale};
}
static struct GuestSize label_size(Word object,const char *selector,struct GuestSize constraint) { struct LabelState *state=label_state(object);if(!strcmp(selector,"intrinsicContentSize") && state->lines!=1 && state->preferred_width>0)constraint.width=fmin(constraint.width,state->preferred_width);return label_measure(state,selector,constraint); }
static struct GuestSize text_editor_size(Word object,const char *selector,struct GuestSize constraint){struct TextFieldState *field=text_field_state(object);struct GuestInsets insets=field->insets;double padding=insets.left+insets.right+10;struct GuestSize size=label_measure(text_editor_label(field),selector,(struct GuestSize){fmax(0,constraint.width-padding),INFINITY});return (struct GuestSize){size.width+padding,size.height+insets.top+insets.bottom};}
static struct GuestSize ui_string_size(Word object,const char *selector,Word font) {
    if (!font) return (struct GuestSize){0};
    struct LabelState state={.text=object,.font=font,.alignment=4,.lines=1};
    struct GuestSize result=label_measure(&state,selector,(struct GuestSize){INFINITY,INFINITY});
    if (state.rich_text) { JNIEnv *env=attached_environment(selector); (*env)->DeleteGlobalRef(env,state.rich_text); }
    return result;
}
static struct GuestRect attributed_bounding_rect(Word object,const char *selector,struct GuestSize size,Word options,Word context) {
    if(context || (options&~(Word)35))stop("Unsupported text drawing context or options",selector);if(isnan(size.width) || isnan(size.height) || size.width<0 || size.height<0)stop("Invalid text drawing size",selector);
    if(!size.width || !size.height || !attributed_length(object,selector))return (struct GuestRect){0};static Word font;if(!font)font=ui_font_system(external_class("_OBJC_CLASS_$_UIFont"),"systemFontOfSize:",12);
    struct LabelState state={.text=attributed_string(object),.attributed=object,.font=font,.alignment=4};JNIEnv *env=attached_environment(selector);jclass cls=(*env)->GetObjectClass(env,activity);jobject text=label_rich_text(env,cls,&state);static jmethodID measure;
    if(!measure)measure=(*env)->GetMethodID(env,cls,"measureGuestAttributedText","(Ljava/lang/CharSequence;FFFI)[F");double scale=screen_scale();jfloatArray result=(*env)->CallObjectMethod(env,activity,measure,text,(jfloat)(12*scale),(jfloat)(size.width*scale),(jfloat)(size.height*scale),(jint)options);jfloat measured[2]={0};if(result && (*env)->GetArrayLength(env,result)==2)(*env)->GetFloatArrayRegion(env,result,0,2,measured);
    if(result)(*env)->DeleteLocalRef(env,result);if(state.rich_text)(*env)->DeleteGlobalRef(env,state.rich_text);(*env)->DeleteLocalRef(env,cls);if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Android attributed text measurement failed",selector);}return (struct GuestRect){0,0,measured[0]/scale,measured[1]/scale};
}
static struct GuestSize attributed_size(Word object,const char *selector) { struct GuestRect rect=attributed_bounding_rect(object,selector,(struct GuestSize){INFINITY,INFINITY},0,0);return (struct GuestSize){rect.width,rect.height}; }
static void label_fit(Word object,const char *selector) { struct ViewState *v=view_state(object); struct GuestSize size=label_size(object,selector,(struct GuestSize){v->frame.width,INFINITY}); v->frame.width=v->bounds.width=size.width; v->frame.height=v->bounds.height=size.height; ui_label_sync(object); }
static struct ControlState *control_state(Word object) {
    struct ViewState *view=view_state(object);
    if (!view->control) {
        struct ControlState *state=calloc(1,sizeof(*state)); if (!state) stop("Allocation failed","UIKit control");
        state->object=object; state->enabled=1; state->empty_title=copy_string(""); state->button=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIButton"));
        view->control=state;
    }
    return view->control;
}
static Word control_flags(struct ControlState *state) { return (state->highlighted ? 1 : 0)|(!state->enabled ? 2 : 0)|(state->selected ? 4 : 0); }
static struct ControlAppearance *control_appearance(struct ControlState *state,Word flags,int create) {
    for (struct ControlAppearance *item=state->appearances;item;item=item->next) if (item->state==flags) return item;
    if (!create) return NULL;
    struct ControlAppearance *item=calloc(1,sizeof(*item)); if (!item) stop("Allocation failed","UIButton appearance");
    item->state=flags; item->next=state->appearances; state->appearances=item; return item;
}
static Word control_content(struct ControlState *state,Word flags,unsigned property) {
    struct ControlAppearance *exact=control_appearance(state,flags,0),*normal=control_appearance(state,0,0);
    Word value=exact ? property==0 ? exact->title : property==1 ? exact->image : exact->color : 0;
    return value ? value : normal ? property==0 ? normal->title : property==1 ? normal->image : normal->color : 0;
}
static Word control_title_label(Word object,const char *selector) {
    struct ControlState *state=control_state(object);
    if (!state->label) {
        state->label=host_allocate(external_class("_OBJC_CLASS_$_UILabel")); label_state(state->label)->alignment=1;
        state->default_color=label_state(state->label)->color;
        view_add(object,selector,state->label); view_state(state->label)->interaction=0;
    }
    return state->label;
}
static Word control_image_view(Word object,const char *selector) {
    struct ControlState *state=control_state(object);
    if(!state->image_view){state->image_view=host_allocate(external_class("_OBJC_CLASS_$_UIImageView"));image_view_init(state->image_view,selector,control_content(state,control_flags(state),1));view_state(state->image_view)->content_mode=1;view_add(object,selector,state->image_view);}
    ui_control_sync(object);return state->image_view;
}
static void ui_control_sync(Word object) {
    struct ViewState *view=view_state(object); struct ControlState *state=view->control;
    if(view->table)return;
    uint32_t background=ui_cell_background(object,label_argb(view->background,0));
    if (!state && (background || ui_layer_has_content(object)) && class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIView"))) state=control_state(object);
    if (!state) return;
    float geometry[11];int visibility[3];Word owner=view_host_geometry(object,geometry,visibility);
    double scale=screen_scale(),x=geometry[0]/scale,y=geometry[1]/scale,alpha=geometry[4];int visible=visibility[0],enabled=visibility[2] && state->enabled;
    Word flags=control_flags(state),image=state->button ? control_content(state,flags,1) : state->image;
    if(state->button && state->image_view){struct GuestRect c=state->content_insets,i=state->image_insets;struct ViewState *child=view_state(state->image_view);control_state(state->image_view)->image=image;
        child->frame=(struct GuestRect){c.y+i.y,c.x+i.x,fmax(0,view->bounds.width-c.y-c.height-i.y-i.height),fmax(0,view->bounds.height-c.x-c.width-i.x-i.width)};child->bounds=(struct GuestRect){0,0,child->frame.width,child->frame.height};ui_control_sync(state->image_view);image=0;}
    int attached=view_window(object)!=0;
    struct ImageSize natural=image ? ui_image_size(image) : (struct ImageSize){0};
    float metrics[]={(float)(x*scale),(float)(y*scale),(float)(view->frame.width*scale),(float)(view->frame.height*scale),(float)alpha,
        (float)((view->layer ? view_state(view->layer)->corner : 0)*scale),(float)(view->layer ? view_state(view->layer)->z : 0),(float)(natural.width*scale),(float)(natural.height*scale),(float)view_window_level(object),geometry[7],geometry[8],geometry[9],geometry[10]};
    visible&=!state->indicator || state->animating || !state->hides;
    jint options[]={visible && attached,attached,state->button,enabled,(jint)background,(jint)view->content_mode,view->exclusive_touch,view->accessibility_element,state->indicator,state->animating,(jint)label_argb(state->indicator_color,0xff808080)};
    if (state->button && state->label) {
        struct LabelState *label=label_state(state->label); Word title=control_content(state,flags,0),color=control_content(state,flags,2);
        if (!title) title=state->empty_title; if (!color) color=state->default_color;
        if (label->text!=title) { label->text=title; label->attributed=0; label_content_changed(label); }
        if (label->color!=color) { label->color=color; label_content_changed(label); }
        struct GuestRect c=state->content_insets,t=state->title_insets;
        struct ViewState *child=view_state(state->label); child->frame=(struct GuestRect){c.y+t.y,c.x+t.x,fmax(0,view->bounds.width-c.y-c.height-t.y-t.height),fmax(0,view->bounds.height-c.x-c.width-t.x-t.width)};
        child->bounds=(struct GuestRect){0,0,child->frame.width,child->frame.height}; ui_label_sync(state->label);
    }
    if (state->synced && state->synced_owner==owner && state->synced_image==image && state->synced_accessibility==view->accessibility_label &&
        !memcmp(state->metrics,metrics,sizeof(metrics)) && !memcmp(state->options,options,sizeof(options))) return;
    int content_changed=!state->synced || state->synced_image!=image || (attached && !state->options[1]) || (options[0] && !state->options[0]);
    if (!attached && (!state->synced || !state->options[1])) return;
    JNIEnv *env=attached_environment("UIKit control update"); static jmethodID sync;
    if (!sync) { jclass cls=(*env)->GetObjectClass(env,activity); sync=(*env)->GetMethodID(env,cls,"syncGuestControl","(JJ[BZ[F[ILjava/lang/String;)V"); (*env)->DeleteLocalRef(env,cls); }
    jbyteArray bytes=NULL;
    if (content_changed && image && attached) {
        struct HostObject *png=(void *)image_png((void *)image);
        if (png) { bytes=(*env)->NewByteArray(env,png->content[0]); (*env)->SetByteArrayRegion(env,bytes,0,png->content[0],(void *)data_buffer(png)); }
    }
    jfloatArray m=(*env)->NewFloatArray(env,COUNT(metrics)); jintArray o=(*env)->NewIntArray(env,COUNT(options));
    (*env)->SetFloatArrayRegion(env,m,0,COUNT(metrics),metrics); (*env)->SetIntArrayRegion(env,o,0,COUNT(options),options);
    jstring description=http_java_string(env,view->accessibility_label);
    (*env)->CallVoidMethod(env,activity,sync,(jlong)object,(jlong)owner,bytes,(jboolean)content_changed,m,o,description);
    if (bytes) (*env)->DeleteLocalRef(env,bytes); if (description) (*env)->DeleteLocalRef(env,description);
    (*env)->DeleteLocalRef(env,m); (*env)->DeleteLocalRef(env,o);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); stop("Android control update failed","UIKit"); }
    state->synced=1; state->synced_owner=owner; state->synced_image=image; state->synced_accessibility=view->accessibility_label;
    memcpy(state->metrics,metrics,sizeof(metrics)); memcpy(state->options,options,sizeof(options));
}
static Word control_factory(Word cls,const char *selector,Word type) {
    if (type>6) stop("Invalid UIButton type",selector);
    Word object=host_allocate(cls); Method init=find_implementation(object,"initWithFrame:");
    if (!init) stop("Missing UIButton initializer",selector);
    object=((Word (*)(Word,const char *,struct GuestRect))init)(object,"initWithFrame:",(struct GuestRect){0});
    struct ControlState *state=control_state(object); state->type=type; view_state(object)->content_mode=1; control_title_label(object,selector); return object;
}
static void control_set_content(Word object,const char *selector,Word value,Word flags) {
    struct ControlState *state=control_state(object); struct ControlAppearance *item=control_appearance(state,flags,1);
    if (!strcmp(selector,"setTitle:forState:")) { item->title=value ? text_copy(value,"copy") : 0; control_title_label(object,selector); }
    else if (!strcmp(selector,"setImage:forState:")) item->image=value;
    else item->color=value;
    ui_control_sync(object);
}
static Word control_get_content(Word object,const char *selector,Word flags) {
    struct ControlState *state=control_state(object);
    unsigned property=strstr(selector,"Image") || strstr(selector,"image") ? 1 : strstr(selector,"Color") ? 2 : 0;
    return control_content(state,!strncmp(selector,"current",7) ? control_flags(state) : flags,property);
}
static Word control_get_property(Word object,const char *selector) {
    struct ControlState *state=control_state(object);
    if (!strcmp(selector,"buttonType")) return state->type;
    if (!strcmp(selector,"state")) return control_flags(state);
    if (!strcmp(selector,"isEnabled")) return state->enabled;
    if (!strcmp(selector,"isSelected")) return state->selected;
    return state->highlighted;
}
static void control_set_property(Word object,const char *selector,Word value) {
    struct ControlState *state=control_state(object);
    if (!strcmp(selector,"setEnabled:")) { state->enabled=value!=0; if (!state->enabled) state->highlighted=0; }
    else if (!strcmp(selector,"setSelected:")) state->selected=value!=0; else state->highlighted=value!=0;
    ui_control_sync(object);
}
static void control_set_insets(Word object,const char *selector,struct GuestRect insets) {
    struct ControlState *state=control_state(object);
    if (!strcmp(selector,"setContentEdgeInsets:")) state->content_insets=insets;else if(!strcmp(selector,"setImageEdgeInsets:"))state->image_insets=insets;else state->title_insets=insets;
    ui_control_sync(object);
}
static struct GuestRect control_get_insets(Word object,const char *selector) { struct ControlState *state=control_state(object); return !strcmp(selector,"contentEdgeInsets") ? state->content_insets : !strcmp(selector,"imageEdgeInsets") ? state->image_insets : state->title_insets; }
static void control_add_target(Word object,const char *selector,Word target,const char *action,Word events) {
    if (!action || !events) return;
    struct ControlState *state=control_state(object);
    for (struct ControlAction *item=state->actions;item;item=item->next) if (item->target==target && !strcmp(item->selector,action)) { item->events|=events; return; }
    struct ControlAction *item=calloc(1,sizeof(*item)); if (!item) stop("Allocation failed",selector);
    *item=(struct ControlAction){target,events,action,state->actions}; state->actions=item;
}
static void control_remove_target(Word object,const char *selector,Word target,const char *action,Word events) {
    struct ControlState *state=control_state(object); struct ControlAction **cursor=&state->actions;
    while (*cursor) {
        struct ControlAction *item=*cursor;
        if ((!target || target==item->target) && (!action || !strcmp(action,item->selector))) item->events&=~events;
        if (!item->events) { *cursor=item->next; free(item); } else cursor=&item->next;
    }
}
static void control_send_action(Word object,const char *selector,const char *action,Word target,Word event) {
    if (!action) return;
    if (!target) for (Word next=object;next;next=view_state(next)->parent) {
        Word controller=view_state(next)->controller;
        if (controller && find_implementation(controller,action)) { target=controller; break; }
        if (next!=object && find_implementation(next,action)) { target=next; break; }
    }
    Method callback=target ? find_implementation(target,action) : NULL;
    if (target && !callback) stop("Unrecognized control action",action);
    if (callback) callback(target,action,object,event,0,0);
}
static void control_send_events(Word object,const char *selector,Word events) {
    struct ControlState *state=control_state(object); size_t count=0;
    for (struct ControlAction *item=state->actions;item;item=item->next) if (item->events&events) ++count;
    struct ControlAction *snapshot=count ? malloc(count*sizeof(*snapshot)) : NULL;
    if (count && !snapshot) stop("Allocation failed",selector);
    size_t index=0; for (struct ControlAction *item=state->actions;item;item=item->next) if (item->events&events) snapshot[index++]=*item;
    for (size_t i=0;i<count;++i) control_send_action(object,selector,snapshot[i].selector,snapshot[i].target,0);
    free(snapshot);
}
static void input_content_changed(Word object){struct TextFieldState *field=text_field_state(object);if(field->multiline){input_delegate_notify(object,5);platform_notification("UITextViewTextDidChangeNotification",object,0);}else{control_send_events(object,"sendActionsForControlEvents:",1U<<17);platform_notification("UITextFieldTextDidChangeNotification",object,0);}if(field->search_owner)ui_search_input_changed(field->search_owner);}
static void input_release_content(Word object){for(struct LabelState *label=label_states;label;label=label->next)if(label->object==object && label->rich_text){JNIEnv *env=attached_environment("Text input release");(*env)->DeleteGlobalRef(env,label->rich_text);label->rich_text=NULL;return;}}
static Word control_all_targets(Word object,const char *selector) { struct Sequence *targets=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSMutableSet"));for(struct ControlAction *action=control_state(object)->actions;action;action=action->next)if(action->target)sequence_add(targets,selector,action->target);targets->isa=external_class("_OBJC_CLASS_$_NSSet");return (Word)targets; }
static Word control_all_events(Word object) { Word events=0;for(struct ControlAction *action=control_state(object)->actions;action;action=action->next)events|=action->events;return events; }
static Word control_actions_for_target(Word object,const char *selector,Word target,Word event) { struct Sequence *actions=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));for(struct ControlAction *action=control_state(object)->actions;action;action=action->next)if(action->target==target && (action->events&event))sequence_add(actions,selector,copy_string(action->selector));if(actions->count)return (Word)actions;free(actions);return 0; }
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_completeControlEvent)(JNIEnv *env,jobject instance,jlong token,jint event,jboolean highlighted) {
    may_unwind=1; snprintf(stage,sizeof(stage),"UIKit control action"); int failed=setjmp(boundary);
    if (!failed) { control_set_property(token,"setHighlighted:",highlighted); if (control_state(token)->enabled) control_send_events(token,"sendActionsForControlEvents:",event); }
    may_unwind=0; return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static Word image_view_init(Word object,const char *selector,Word image) {
    struct ImageSize size=image ? ui_image_size(image) : (struct ImageSize){0};
    view_init_frame(object,selector,(struct GuestRect){0,0,size.width,size.height});
    control_state(object)->image=image; view_state(object)->interaction=0; return object;
}
static void image_view_set_image(Word object,const char *selector,Word image) { control_state(object)->image=image; ui_control_sync(object); }
static Word image_view_get_image(Word object) { return control_state(object)->image; }
static void ui_view_display(void *context) {
    Word object=(Word)context; struct ViewState *state=view_state(object); state->display_pending=0;
    if (!state->needs_display || state->display_running || state->hidden || !view_window(object) || state->bounds.width<=0 || state->bounds.height<=0) return;
    Method draw=state->display_method; state->needs_display=0;
    if (!draw) return;
    state->display_running=1;
    Word previous_context=image_context_current();
    image_context_begin((struct GuestSize){state->bounds.width,state->bounds.height},0,state->scale);
    struct GuestCallBoundary scoped; scoped.previous=active_guest_call_boundary; active_guest_call_boundary=&scoped;
    int failed=setjmp(scoped.environment);
    if (!failed) {
        drawing_translate(image_context_current(),-state->bounds.x,-state->bounds.y);
        ((void (*)(Word,const char *,struct GuestRect))draw)(object,"drawRect:",state->bounds);
        control_state(object)->image=image_context_image();
    }
    active_guest_call_boundary=scoped.previous;
    while (image_context_current()!=previous_context) image_context_end();
    state->display_running=0;
    if (failed) { char failure[sizeof(status)]; memcpy(failure,status,sizeof(failure)); stop("UIView drawing failed",failure); }
    ui_control_sync(object);
    if (state->needs_display) ui_view_request_display(object,"setNeedsDisplay");
}
static void ui_view_request_display(Word object,const char *selector) {
    struct ViewState *state=view_state(object);
    if (!state->display_checked || state->display_epoch!=method_cache_epoch) {
        state->display_checked=1; state->display_epoch=method_cache_epoch; state->display_method=find_implementation(object,"drawRect:");
        if (state->display_method==(Method)view_draw_rect) state->display_method=NULL;
    }
    if (!state->display_method) { state->needs_display=0; return; }
    state->needs_display=1;
    if (!state->display_pending && !state->display_running && !state->hidden && view_window(object)) {
        state->display_pending=1; schedule_main_function(ui_view_display,(void *)object);
    }
}
static struct GuestSize image_view_size(Word object,const char *selector,struct GuestSize limit) { Word image=control_state(object)->image; struct ImageSize size=image ? ui_image_size(image) : (struct ImageSize){0}; return (struct GuestSize){size.width,size.height}; }
static void image_view_fit(Word object,const char *selector) { struct ViewState *view=view_state(object); struct GuestSize size=image_view_size(object,selector,(struct GuestSize){0}); struct GuestRect frame=view->frame; frame.width=size.width; frame.height=size.height; view_set_frame(object,selector,frame); }
static int ui_view_rendered(Word object) { return (layer_object(object) && !view_state(object)->view) || label_object(object) || view_state(object)->control!=NULL || view_state(object)->web!=NULL || view_state(object)->input!=NULL || view_state(object)->table!=NULL; }
static void ui_view_load_label(Word object,plist_t node){
    const char *text=plist_get_string_ptr(plist_dict_get_item(node,"text"),NULL);if(text)label_text(object,"Archived label",copy_string(text));
    plist_t color=plist_dict_get_item(node,"textColor");if(plist_get_node_type(color)==PLIST_ARRAY){double rgba[4]={0};for(unsigned i=0;i<4;++i)plist_get_real_val(plist_array_get_item(color,i),rgba+i);label_color(object,"Archived label",ui_color_rgba(external_class("_OBJC_CLASS_$_UIColor"),"colorWithRed:green:blue:alpha:",rgba[0],rgba[1],rgba[2],rgba[3]));}
    label_alignment(object,"Archived label",(long)scene_number(node,"alignment",0));label_lines(object,"Archived label",(long)scene_number(node,"lines",1));
}
static void ui_view_load_content(Word object,const char *image,const double *background) {
    if (background) view_state(object)->background=ui_color_rgba(external_class("_OBJC_CLASS_$_UIColor"),"colorWithRed:green:blue:alpha:",background[0],background[1],background[2],background[3]);
    if (image && *image) {
        Word content=ui_image_named(external_class("_OBJC_CLASS_$_UIImage"),"imageNamed:",copy_string(image));
        if (!content) LOG("Storyboard image unavailable: %s",image);
        image_view_set_image(object,"setImage:",content);
    } else ui_label_sync(object);
}
static double indicator_extent(unsigned style) { return style==0 || style==101 ? 37 : 20; }
static void indicator_style(Word object,const char *selector,Word style) {
    if (style>2 && style!=100 && style!=101) stop("Invalid activity indicator style",selector);
    struct ControlState *state=control_state(object); double previous=indicator_extent(state->type),extent=indicator_extent(style);
    state->indicator=1; state->type=style;
    state->indicator_color=style==2 ? ui_color_named(external_class("_OBJC_CLASS_$_UIColor"),"grayColor") : style<2 ? ui_color_named(external_class("_OBJC_CLASS_$_UIColor"),"whiteColor") : ui_color_named(external_class("_OBJC_CLASS_$_UIColor"),"grayColor");
    struct GuestRect frame=view_state(object)->frame;
    if ((!frame.width && !frame.height) || (frame.width==previous && frame.height==previous)) { frame.width=frame.height=extent; view_set_frame(object,selector,frame); }
    ui_control_sync(object); view_request_layout(object,selector);
}
static Word indicator_init(Word object,const char *selector,Word style) { struct ControlState *state=control_state(object); state->indicator=1; state->hides=1; indicator_style(object,selector,style); return object; }
static Word indicator_init_frame(Word object,const char *selector,struct GuestRect frame) { indicator_init(object,selector,0); return view_init_frame(object,selector,frame); }
static void indicator_set(Word object,const char *selector,Word value) {
    struct ControlState *state=control_state(object);
    if (!strcmp(selector,"setActivityIndicatorViewStyle:")) { indicator_style(object,selector,value); return; }
    if (!strcmp(selector,"setColor:")) state->indicator_color=value;
    else if (!strcmp(selector,"setHidesWhenStopped:")) state->hides=value!=0;
    else { state->animating=!strcmp(selector,"startAnimating"); if (state->animating && state->hides) view_state(object)->hidden=0; }
    ui_control_sync(object);
}
static Word indicator_get(Word object,const char *selector) { struct ControlState *state=control_state(object); return !strcmp(selector,"isAnimating") ? state->animating : !strcmp(selector,"color") ? state->indicator_color : !strcmp(selector,"hidesWhenStopped") ? state->hides : state->type; }
static struct GuestSize indicator_size(Word object,const char *selector,struct GuestSize limit) { double extent=indicator_extent(control_state(object)->type); return (struct GuestSize){extent,extent}; }
static struct GuestSize ui_view_intrinsic_size(Word object) {
    if(class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UISearchBar"))){struct GuestSize size=search_bar_size(object,"intrinsicContentSize",(struct GuestSize){-1,0});return size;}
    if (label_object(object)) return label_size(object,"intrinsicContentSize",(struct GuestSize){INFINITY,INFINITY});
    if (class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIImageView"))) return image_view_size(object,"intrinsicContentSize",(struct GuestSize){0});
    struct ControlState *state=view_state(object)->control;
    if (state && state->indicator) return indicator_size(object,"intrinsicContentSize",(struct GuestSize){0});
    if (state && state->button && state->label) {
        struct GuestSize size=label_size(state->label,"intrinsicContentSize",(struct GuestSize){INFINITY,INFINITY});
        struct GuestRect insets=state->content_insets; return (struct GuestSize){size.width+insets.y+insets.height,size.height+insets.x+insets.width};
    }
    return (struct GuestSize){-1,-1};
}
static void ui_search_sync(Word object) {
    if(!class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UISearchBar")))return;struct SearchBarState *bar=search_bar_state(object);if(bar->closed || bar->layout_running)return;bar->layout_running=1;Word field=search_bar_field(bar);struct ViewState *view=view_state(object);double width=view->bounds.width,height=view->bounds.height,top=bar->prompt ? 24 : 0,scope=bar->shows_scope ? 44 : 0;
    Word background=bar->bar_tint ? bar->bar_tint : bar->style==2 || bar->translucent ? 0 : bar->surface;if(view->background!=background)view_background(object,"Search bar background",background);
    if(bar->prompt){if(!bar->prompt_view){bar->prompt_view=host_allocate(external_class("_OBJC_CLASS_$_UILabel"));view_add(object,"Search prompt",bar->prompt_view);label_font(bar->prompt_view,"setFont:",ui_font_system(external_class("_OBJC_CLASS_$_UIFont"),"systemFontOfSize:",13));label_alignment(bar->prompt_view,"setTextAlignment:",1);}if(!same_objects(label_get_text(bar->prompt_view),bar->prompt))label_text(bar->prompt_view,"setText:",bar->prompt);view_hidden(bar->prompt_view,"Search prompt",0);view_set_frame(bar->prompt_view,"Search prompt",(struct GuestRect){8,0,fmax(0,width-16),top});}else if(bar->prompt_view)view_hidden(bar->prompt_view,"Search prompt",1);
    double cancel=bar->shows_cancel ? 80 : 0;view_set_frame(field,"Search field",(struct GuestRect){8,top+8,fmax(0,width-16-cancel),fmax(0,height-top-scope-16)});
    Word layer=view_layer(field);layer_set_metric(layer,"setCornerRadius:",12);layer_set_flag(layer,"setMasksToBounds:",1);
    if(bar->shows_cancel && !bar->cancel){bar->cancel=control_factory(external_class("_OBJC_CLASS_$_UIButton"),"buttonWithType:",1);control_set_content(bar->cancel,"setTitle:forState:",copy_string("Cancel"),0);control_add_target(bar->cancel,"addTarget:action:forControlEvents:",object,"cancelSearch:",64);view_add(object,"Search cancel",bar->cancel);}
    if(bar->cancel){control_set_content(bar->cancel,"setTitleColor:forState:",bar->tint,0);view_hidden(bar->cancel,"Search cancel",!bar->shows_cancel);view_set_frame(bar->cancel,"Search cancel",(struct GuestRect){fmax(0,width-80),top,80,fmax(0,height-top-scope)});}
    struct Sequence *titles=(void *)bar->scope_titles;if(titles && !bar->scope_buttons){bar->scope_buttons=sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));for(size_t i=0;i<titles->count;++i){Word button=control_factory(external_class("_OBJC_CLASS_$_UIButton"),"buttonWithType:",1);control_set_content(button,"setTitle:forState:",titles->items[i],0);control_add_target(button,"addTarget:action:forControlEvents:",object,"selectSearchScope:",64);sequence_add((void *)bar->scope_buttons,"Search scope",button);view_add(object,"Search scope",button);}}
    struct Sequence *buttons=(void *)bar->scope_buttons;for(size_t i=0;buttons && i<buttons->count;++i){Word button=buttons->items[i];view_hidden(button,"Search scope",!bar->shows_scope);control_set_content(button,"setTitleColor:forState:",bar->tint,0);control_set_property(button,"setSelected:",i==bar->selected_scope);view_background(button,"Search scope",i==bar->selected_scope ? view_state(field)->background : 0);view_set_frame(button,"Search scope",(struct GuestRect){width*i/buttons->count,fmax(0,height-44),width/buttons->count,44});layer_set_metric(view_layer(button),"setCornerRadius:",12);}
    ui_input_sync(field);ui_view_composition_sync(field);if(bar->controller)ui_search_geometry(bar->controller);bar->layout_running=0;
}
struct BarItemState { Word object,title,image,custom,button,target,toolbar,accessibility;const char *action;double width;unsigned style,enabled,system,system_item; };
struct ToolbarState { Word object,items,tint,color,default_tint,surface;double width,height;unsigned style,translucent,dirty,layout_running; };
static struct BarItemState *bar_item_state(Word object) { struct ViewState *view=view_state(object);if(!view->bar){struct BarItemState *state=calloc(1,sizeof(*state));if(!state)stop("Allocation failed","UIBarButtonItem");state->object=object;state->enabled=1;view->bar=state;}return view->bar; }
static struct ToolbarState *toolbar_state(Word object) { struct ViewState *view=view_state(object);if(!view->bar){struct ToolbarState *state=calloc(1,sizeof(*state));if(!state)stop("Allocation failed","UIToolbar");state->object=object;state->translucent=1;state->dirty=1;state->default_tint=ui_interface_color(3);state->surface=ui_interface_color(0);view->bar=state;}return view->bar; }
static void bar_item_changed(struct BarItemState *item) { if(item->toolbar){toolbar_state(item->toolbar)->dirty=1;ui_toolbar_sync(item->toolbar);} }
static Word bar_item_init_title(Word object,const char *selector,Word title,Word style,Word target,const char *action) { if(style>2)stop("Invalid bar button style",selector);struct BarItemState *item=bar_item_state(object);item->title=title ? text_copy(title,"copy") : 0;item->style=style;store_weak(&item->target,target);item->action=action;return object; }
static Word bar_item_init_custom(Word object,const char *selector,Word view) { if(view && !class_inherits(read_word(view),external_class("_OBJC_CLASS_$_UIView")))stop("Invalid custom bar button view",selector);struct BarItemState *item=bar_item_state(object);view_strong_property(&item->custom,view);return object; }
static Word bar_item_init_image(Word object,const char *selector,Word image,Word style,Word target,const char *action) { bar_item_init_title(object,selector,0,style,target,action);view_strong_property(&bar_item_state(object)->image,image);return object; }
static Word bar_item_init_system(Word object,const char *selector,Word system,Word target,const char *action) { const char *titles[]={"Done","Cancel","Edit","Save","Add","","","Compose","Reply","Share","Organize","Bookmarks","Search","Refresh","Stop","Camera","Trash","Play","Pause","Rewind","Fast Forward","Undo","Redo","Page Curl","Close"};if(system>=COUNT(titles))stop("Unsupported system bar button",selector);bar_item_init_title(object,selector,copy_string(titles[system]),0,target,action);struct BarItemState *item=bar_item_state(object);item->system=1;item->system_item=system;return object; }
static void bar_item_action(Word object,const char *selector,Word sender) {
    struct BarItemState *item=bar_item_state(object);if(!item->enabled || !item->action)return;Word target=load_weak_retained(&item->target);if(!target)for(Word view=item->toolbar;view;view=view_state(view)->parent){Word controller=view_state(view)->controller;if(controller && find_implementation(controller,item->action)){target=metal_object_retain(controller);break;}}
    Method method=target ? find_implementation(target,item->action) : NULL;if(method)method(target,item->action,object,0,0,0);else if(target)stop("Unimplemented bar button action",item->action);metal_object_release(target);
}
static Word bar_item_view(Word object) { struct BarItemState *item=bar_item_state(object);if(item->custom)return item->custom;if(item->system && (item->system_item==5 || item->system_item==6))return 0;if(!item->button){item->button=control_factory(external_class("_OBJC_CLASS_$_UIButton"),"buttonWithType:",1);control_add_target(item->button,"addTarget:action:forControlEvents:",object,"invokeBarButton:",64);}return item->button; }
static Word bar_item_get(Word object,const char *selector) { struct BarItemState *item=bar_item_state(object);if(!strcmp(selector,"title"))return item->title;if(!strcmp(selector,"image"))return item->image;if(!strcmp(selector,"customView"))return item->custom;if(!strcmp(selector,"target"))return load_weak(&item->target);if(!strcmp(selector,"action"))return (Word)item->action;if(!strcmp(selector,"isEnabled"))return item->enabled;if(!strcmp(selector,"style"))return item->style;return item->accessibility; }
static void bar_item_set(Word object,const char *selector,Word value) { struct BarItemState *item=bar_item_state(object);if(!strcmp(selector,"setTarget:")){store_weak(&item->target,value);return;}if(!strcmp(selector,"setAction:")){item->action=(const char *)value;return;}if(!strcmp(selector,"setTitle:")){item->title=value ? text_copy(value,"copy") : 0;item->system=0;}else if(!strcmp(selector,"setImage:"))view_strong_property(&item->image,value);else if(!strcmp(selector,"setCustomView:")){Word old=bar_item_view(object);if(old && view_state(old)->parent==item->toolbar)view_remove(old);bar_item_init_custom(object,selector,value);}else if(!strcmp(selector,"setEnabled:"))item->enabled=value!=0;else if(!strcmp(selector,"setStyle:")){if(value>2)stop("Invalid bar button style",selector);item->style=value;}else item->accessibility=value ? text_copy(value,"copy") : 0;bar_item_changed(item); }
static double bar_item_width(Word object) { return bar_item_state(object)->width; }
static void bar_item_set_width(Word object,const char *selector,double width) { if(!isfinite(width) || width<0)stop("Invalid bar button width",selector);struct BarItemState *item=bar_item_state(object);item->width=width;bar_item_changed(item); }
static Word toolbar_init(Word object,const char *selector,struct GuestRect frame) { view_init_frame(object,selector,frame);toolbar_state(object);ui_toolbar_sync(object);return object; }
static Word toolbar_plain(Word object,const char *selector) { return toolbar_init(object,selector,(struct GuestRect){0,0,0,44}); }
static Word toolbar_get(Word object,const char *selector) { struct ToolbarState *state=toolbar_state(object);if(!strcmp(selector,"items"))return state->items;if(!strcmp(selector,"tintColor"))return state->tint;if(!strcmp(selector,"barTintColor"))return state->color;if(!strcmp(selector,"barStyle"))return state->style;return state->translucent; }
static void toolbar_set(Word object,const char *selector,Word value) {
    struct ToolbarState *state=toolbar_state(object);if(!strncmp(selector,"setItems:",9)){if(value && !class_inherits(read_word(value),external_class("_OBJC_CLASS_$_NSArray")))stop("Invalid toolbar items",selector);struct Sequence *input=(void *)value;if(input)for(size_t i=0;i<input->count;++i){if(!class_inherits(read_word(input->items[i]),external_class("_OBJC_CLASS_$_UIBarButtonItem")))stop("Invalid toolbar item",selector);for(size_t j=0;j<i;++j)if(input->items[j]==input->items[i])stop("Duplicate toolbar item",selector);}
        Word copied=value ? sequence_copy(input,"copy") : 0;struct Sequence *previous=(void *)state->items;if(previous){for(size_t i=0;i<previous->count;++i){struct BarItemState *item=bar_item_state(previous->items[i]);Word view=bar_item_view(item->object);if(view && view_state(view)->parent==object)view_remove(view);if(item->toolbar==object)item->toolbar=0;}free(previous->items);free(previous);}state->items=copied;
    }else if(!strcmp(selector,"setTintColor:"))state->tint=value;else if(!strcmp(selector,"setBarTintColor:"))state->color=value;else if(!strcmp(selector,"setBarStyle:")){if(value>1)stop("Invalid toolbar style",selector);state->style=value;}else state->translucent=value!=0;state->dirty=1;ui_toolbar_sync(object);
}
static struct GuestSize toolbar_size(Word object,const char *selector,struct GuestSize size) { return (struct GuestSize){size.width,44}; }
static void ui_toolbar_sync(Word object) {
    if(!class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIToolbar")))return;struct ToolbarState *state=toolbar_state(object);struct ViewState *root=view_state(object);if(state->layout_running || (!state->dirty && state->width==root->bounds.width && state->height==root->bounds.height))return;state->layout_running=1;state->width=root->bounds.width;state->height=root->bounds.height;
    Word color_class=external_class("_OBJC_CLASS_$_UIColor");Word background=state->color ? state->color : state->translucent ? 0 : state->style ? ui_color_white(color_class,"Toolbar surface",.1,1) : state->surface;view_background(object,"Toolbar background",background);Word tint=state->tint ? state->tint : state->style ? ui_color_white(color_class,"Toolbar tint",1,1) : state->default_tint;
    struct Sequence *items=(void *)state->items;double available=fmax(0,state->width-16),fixed=0;unsigned flexible=0;double *widths=items && items->count ? calloc(items->count,sizeof(double)) : NULL;if(items && items->count && !widths)stop("Allocation failed","Toolbar layout");
    for(size_t i=0;items && i<items->count;++i){struct BarItemState *item=bar_item_state(items->items[i]);item->toolbar=object;if(item->system && item->system_item==5){++flexible;continue;}Word view=bar_item_view(item->object);double width=item->width;
        if(view){if(view_state(view)->parent!=object)view_add(object,"Toolbar item",view);if(item->custom){if(width<=0)width=view_state(view)->bounds.width;}else {control_set_content(view,"setTitle:forState:",item->title,0);control_set_content(view,"setImage:forState:",item->image,0);control_set_content(view,"setTitleColor:forState:",tint,0);control_set_property(view,"setEnabled:",item->enabled);view_accessibility(view,"setAccessibilityLabel:",item->accessibility);if(width<=0){struct GuestSize size=label_size(control_title_label(view,"titleLabel"),"sizeThatFits:",(struct GuestSize){INFINITY,INFINITY});width=fmax(44,size.width+24);if(item->image)width=fmax(width,ui_image_size(item->image).width+16);}}}widths[i]=width;fixed+=width;}
    double flexible_width=flexible ? fmax(0,available-fixed)/flexible : 0,factor=fixed>available && fixed>0 ? available/fixed : 1,x=8;
    for(size_t i=0;items && i<items->count;++i){struct BarItemState *item=bar_item_state(items->items[i]);if(item->system && item->system_item==5){x+=flexible_width;continue;}double width=widths[i]*factor;Word view=bar_item_view(item->object);if(view)view_set_frame(view,"Toolbar item layout",(struct GuestRect){x,0,width,state->height});x+=width;}
    free(widths);state->dirty=state->layout_running=0;
}
static Method ui_labels_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if(!class_receiver && class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UITextView")) && !strcmp(selector,"sizeThatFits:"))return (Method)text_editor_size;
    if (!class_receiver && !strcmp(selector,"intrinsicContentSize")){struct ViewState *state=existing_view_state(object);if(label_object(object) || (state && state->control))return (Method)ui_view_intrinsic_size;}
    Word cls=class_receiver ? object : read_word(object);
    if(!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UIBarButtonItem"))){if(!strcmp(selector,"initWithTitle:style:target:action:"))return (Method)bar_item_init_title;if(!strcmp(selector,"initWithCustomView:"))return (Method)bar_item_init_custom;if(!strcmp(selector,"initWithImage:style:target:action:"))return (Method)bar_item_init_image;if(!strcmp(selector,"initWithBarButtonSystemItem:target:action:"))return (Method)bar_item_init_system;if(!strcmp(selector,"width"))return (Method)bar_item_width;if(!strcmp(selector,"setWidth:"))return (Method)bar_item_set_width;if(!strcmp(selector,"invokeBarButton:"))return (Method)bar_item_action;const char *getters[]={"title","image","customView","target","action","isEnabled","style","accessibilityLabel"},*setters[]={"setTitle:","setImage:","setCustomView:","setTarget:","setAction:","setEnabled:","setStyle:","setAccessibilityLabel:"};for(unsigned i=0;i<COUNT(getters);++i){if(!strcmp(selector,getters[i]))return (Method)bar_item_get;if(!strcmp(selector,setters[i]))return (Method)bar_item_set;}}
    if(!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UIToolbar"))){if(!strcmp(selector,"init"))return (Method)toolbar_plain;if(!strcmp(selector,"initWithFrame:"))return (Method)toolbar_init;if(!strcmp(selector,"sizeThatFits:"))return (Method)toolbar_size;const char *getters[]={"items","tintColor","barTintColor","barStyle","isTranslucent"},*setters[]={"setItems:","setItems:animated:","setTintColor:","setBarTintColor:","setBarStyle:","setTranslucent:"};for(unsigned i=0;i<COUNT(getters);++i)if(!strcmp(selector,getters[i]))return (Method)toolbar_get;for(unsigned i=0;i<COUNT(setters);++i)if(!strcmp(selector,setters[i]))return (Method)toolbar_set;}
    if(!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_NSAttributedString"))){if(!strcmp(selector,"boundingRectWithSize:options:context:"))return (Method)attributed_bounding_rect;if(!strcmp(selector,"size"))return (Method)attributed_size;}
    if (!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UIActivityIndicatorView"))) {
        if (!strcmp(selector,"initWithActivityIndicatorStyle:")) return (Method)indicator_init;
        if (!strcmp(selector,"initWithFrame:")) return (Method)indicator_init_frame;
        if (!strcmp(selector,"setActivityIndicatorViewStyle:") || !strcmp(selector,"setColor:") || !strcmp(selector,"setHidesWhenStopped:") || !strcmp(selector,"startAnimating") || !strcmp(selector,"stopAnimating")) return (Method)indicator_set;
        if (!strcmp(selector,"activityIndicatorViewStyle") || !strcmp(selector,"color") || !strcmp(selector,"hidesWhenStopped") || !strcmp(selector,"isAnimating")) return (Method)indicator_get;
        if (!strcmp(selector,"sizeThatFits:")) return (Method)indicator_size;
    }
    int button=class_inherits(cls,external_class("_OBJC_CLASS_$_UIButton"));
    if(!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UIControl"))){
        if(!strcmp(selector,"allTargets"))return (Method)control_all_targets;if(!strcmp(selector,"allControlEvents"))return (Method)control_all_events;
        if(!strcmp(selector,"actionsForTarget:forControlEvent:"))return (Method)control_actions_for_target;
        if(!strcmp(selector,"addTarget:action:forControlEvents:"))return (Method)control_add_target;if(!strcmp(selector,"removeTarget:action:forControlEvents:"))return (Method)control_remove_target;
        if(!strcmp(selector,"sendActionsForControlEvents:"))return (Method)control_send_events;if(!strcmp(selector,"sendAction:to:forEvent:"))return (Method)control_send_action;
        if(!strcmp(selector,"state") || !strcmp(selector,"isEnabled") || !strcmp(selector,"isSelected") || !strcmp(selector,"isHighlighted"))return (Method)control_get_property;
        if(!strcmp(selector,"setEnabled:") || !strcmp(selector,"setSelected:") || !strcmp(selector,"setHighlighted:"))return (Method)control_set_property;
    }
    if (button) {
        if (class_receiver && !strcmp(selector,"buttonWithType:")) return (Method)control_factory;
        if (!class_receiver) {
            if (!strcmp(selector,"titleLabel")) return (Method)control_title_label;
            if (!strcmp(selector,"imageView")) return (Method)control_image_view;
            if (!strcmp(selector,"setTitle:forState:") || !strcmp(selector,"setImage:forState:") || !strcmp(selector,"setTitleColor:forState:")) return (Method)control_set_content;
            if (!strcmp(selector,"titleForState:") || !strcmp(selector,"imageForState:") || !strcmp(selector,"titleColorForState:") || !strcmp(selector,"currentTitle") || !strcmp(selector,"currentImage") || !strcmp(selector,"currentTitleColor")) return (Method)control_get_content;
            if (!strcmp(selector,"buttonType") || !strcmp(selector,"state") || !strcmp(selector,"isEnabled") || !strcmp(selector,"isSelected") || !strcmp(selector,"isHighlighted")) return (Method)control_get_property;
            if (!strcmp(selector,"setEnabled:") || !strcmp(selector,"setSelected:") || !strcmp(selector,"setHighlighted:")) return (Method)control_set_property;
            if (!strcmp(selector,"setContentEdgeInsets:") || !strcmp(selector,"setTitleEdgeInsets:") || !strcmp(selector,"setImageEdgeInsets:")) return (Method)control_set_insets;
            if (!strcmp(selector,"contentEdgeInsets") || !strcmp(selector,"titleEdgeInsets") || !strcmp(selector,"imageEdgeInsets")) return (Method)control_get_insets;
            if (!strcmp(selector,"addTarget:action:forControlEvents:")) return (Method)control_add_target;
            if (!strcmp(selector,"removeTarget:action:forControlEvents:")) return (Method)control_remove_target;
            if (!strcmp(selector,"sendActionsForControlEvents:")) return (Method)control_send_events;
            if (!strcmp(selector,"sendAction:to:forEvent:")) return (Method)control_send_action;
        }
    }
    if (!class_receiver && class_inherits(cls,external_class("_OBJC_CLASS_$_UIImageView"))) {
        if (!strcmp(selector,"initWithImage:")) return (Method)image_view_init;
        if (!strcmp(selector,"setImage:")) return (Method)image_view_set_image;
        if (!strcmp(selector,"image")) return (Method)image_view_get_image;
        if (!strcmp(selector,"sizeToFit")) return (Method)image_view_fit;
        if (!strcmp(selector,"sizeThatFits:")) return (Method)image_view_size;
    }
    if (class_receiver || !label_object(object)) return NULL;
    if(!strcmp(selector,"setPreferredMaxLayoutWidth:"))return (Method)label_set_preferred_width;if(!strcmp(selector,"preferredMaxLayoutWidth"))return (Method)label_preferred_width;
    if (!strcmp(selector,"setText:")) return (Method)label_text; if (!strcmp(selector,"text")) return (Method)label_get_text;
    if (!strcmp(selector,"setAttributedText:")) return (Method)label_attributed; if (!strcmp(selector,"attributedText")) return (Method)label_get_attributed;
    if (!strcmp(selector,"setFont:")) return (Method)label_font; if (!strcmp(selector,"font")) return (Method)label_get_font;
    if (!strcmp(selector,"setTextColor:")) return (Method)label_color; if (!strcmp(selector,"textColor")) return (Method)label_get_color;
    if (!strcmp(selector,"setTextAlignment:")) return (Method)label_alignment; if (!strcmp(selector,"textAlignment")) return (Method)label_get_alignment;
    if (!strcmp(selector,"setNumberOfLines:")) return (Method)label_lines; if (!strcmp(selector,"numberOfLines")) return (Method)label_get_lines;
    if (!strcmp(selector,"setAdjustsFontSizeToFitWidth:")) return (Method)label_adjusts; if (!strcmp(selector,"adjustsFontSizeToFitWidth")) return (Method)label_get_adjusts;
    if(!strcmp(selector,"setBaselineAdjustment:"))return (Method)label_set_baseline;if(!strcmp(selector,"baselineAdjustment"))return (Method)label_baseline;
    if(!strcmp(selector,"setMinimumScaleFactor:"))return (Method)label_set_minimum_scale;if(!strcmp(selector,"minimumScaleFactor"))return (Method)label_minimum_scale;
    if (!strcmp(selector,"setLineBreakMode:")) return (Method)label_line_break; if (!strcmp(selector,"lineBreakMode")) return (Method)label_get_line_break;
    if (!strcmp(selector,"sizeToFit")) return (Method)label_fit; if (!strcmp(selector,"sizeThatFits:")) return (Method)label_size;
    return NULL;
}
