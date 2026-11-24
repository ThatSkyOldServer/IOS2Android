 
static struct GuestRect keyboard_frame;
static int keyboard_frame_known,keyboard_visible;
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_updateGuestKeyboard)(JNIEnv *env,jobject instance,jboolean visible,jint x,jint y,jint width,jint height) {
    if (!started || width<=0 || height<0) return NULL;
    may_unwind=1; snprintf(stage,sizeof(stage),"Android UIKit keyboard notifications");
    int failed=setjmp(boundary);
    if (!failed) {
        double scale=screen_scale();
        struct GuestRect next={x/scale,y/scale,width/scale,height/scale};
        struct GuestRect previous=keyboard_frame_known ? keyboard_frame : (struct GuestRect){next.x,next.y+next.height,next.width,0};
        int shown=visible!=0,transition=shown!=keyboard_visible;
        if (!keyboard_frame_known || transition || memcmp(&next,&keyboard_frame,sizeof(next))) {
            keyboard_frame=next; keyboard_frame_known=1;
            if (transition || shown) {
                struct GuestMap *info=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
                Word cls=external_class("_OBJC_CLASS_$_NSValue");
                map_set(info,"UIKit keyboard",value_rect(cls,"valueWithCGRect:",previous),copy_string("UIKeyboardFrameBeginUserInfoKey"));
                map_set(info,"UIKit keyboard",value_rect(cls,"valueWithCGRect:",next),copy_string("UIKeyboardFrameEndUserInfoKey"));
                map_set(info,"UIKit keyboard",real_number_factory(external_class("_OBJC_CLASS_$_NSNumber"),"keyboard duration",0),copy_string("UIKeyboardAnimationDurationUserInfoKey"));
                map_set(info,"UIKit keyboard",number_integer(0),copy_string("UIKeyboardAnimationCurveUserInfoKey"));
                map_set(info,"UIKit keyboard",number_bool(external_class("_OBJC_CLASS_$_NSNumber"),"keyboard local",1),copy_string("UIKeyboardIsLocalUserInfoKey"));
                keyboard_visible=shown;
                platform_notification("UIKeyboardWillChangeFrameNotification",0,(Word)info);
                if (transition) platform_notification(shown ? "UIKeyboardWillShowNotification" : "UIKeyboardWillHideNotification",0,(Word)info);
                platform_notification("UIKeyboardDidChangeFrameNotification",0,(Word)info);
                if (transition) platform_notification(shown ? "UIKeyboardDidShowNotification" : "UIKeyboardDidHideNotification",0,(Word)info);
#if I2A_DEVELOPMENT
                LOG("[DIAG-ime-state] visible=%d transition=%d frame=%g,%g,%g,%g",shown,transition,next.x,next.y,next.width,next.height);
#endif
            }
        }
    }
    may_unwind=0;
    return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static const char *const input_delegate_fields[]={"textFieldShouldBeginEditing:","textFieldDidBeginEditing:","textFieldShouldEndEditing:","textFieldDidEndEditing:","textField:shouldChangeCharactersInRange:replacementString:",NULL,NULL,"textFieldShouldReturn:"};
static const char *const input_delegate_views[]={"textViewShouldBeginEditing:","textViewDidBeginEditing:","textViewShouldEndEditing:","textViewDidEndEditing:","textView:shouldChangeTextInRange:replacementText:","textViewDidChange:","textViewDidChangeSelection:",NULL};
static Method input_callback(struct TextFieldState *field,unsigned index) {
    if(!field->delegate_cached || field->delegate_epoch!=method_cache_epoch){const char *const *names=field->multiline ? input_delegate_views : input_delegate_fields;
        for(unsigned i=0;i<COUNT(field->callbacks);++i)field->callbacks[i]=field->properties[12] && names[i] ? find_implementation(field->properties[12],names[i]) : NULL;
        field->delegate_cached=1;field->delegate_epoch=method_cache_epoch;}
    return field->callbacks[index];
}
static int input_delegate_bool(Word object,unsigned index){struct TextFieldState *field=text_field_state(object);if(field->search_owner && !ui_search_input_should(field->search_owner,index))return 0;Method method=input_callback(field,index);const char *selector=(field->multiline ? input_delegate_views : input_delegate_fields)[index];return !method || (method(field->properties[12],selector,object,0,0,0)&255);}
static void input_delegate_notify(Word object,unsigned index){struct TextFieldState *field=text_field_state(object);Method method=input_callback(field,index);if(method)method(field->properties[12],(field->multiline ? input_delegate_views : input_delegate_fields)[index],object,0,0,0);if(field->search_owner)ui_search_input_notify(field->search_owner,index);}
static void input_selection_changed(Word object){input_delegate_notify(object,6);}
static void input_release_content(Word);
static void input_content_changed(Word);
static void input_report_failure(JNIEnv *env){jclass cls=(*env)->GetObjectClass(env,activity);jmethodID report=(*env)->GetMethodID(env,cls,"guestFailure","(Ljava/lang/String;)V");jstring error=(*env)->NewStringUTF(env,status);(*env)->CallVoidMethod(env,activity,report,error);(*env)->DeleteLocalRef(env,error);(*env)->DeleteLocalRef(env,cls);}
static void text_field_sync_input(Word object,int show_keyboard) {
    struct TextFieldState *field=text_field_state(object);
    if(field->multiline || field->visual){ui_input_sync(object);JNIEnv *env=attached_environment("Text editor focus");static jmethodID method;if(!method){jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"focusGuestEditor","(JZ)V");(*env)->DeleteLocalRef(env,cls);}(*env)->CallVoidMethod(env,activity,method,(jlong)object,(jboolean)show_keyboard);return;}
    JNIEnv *env=attached_environment("UITextField input");
    jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"syncGuestInput","(JLjava/lang/String;IIIZZ)V");
    jstring text=http_java_string(env,field->properties[0]);
    (*env)->CallVoidMethod(env,activity,method,(jlong)object,text,(jint)field->properties[13],(jint)field->properties[6],(jint)field->properties[9],field->properties[15] ? JNI_TRUE : JNI_FALSE,show_keyboard ? JNI_TRUE : JNI_FALSE);
    (*env)->DeleteLocalRef(env,text); (*env)->DeleteLocalRef(env,cls);
}
static void text_field_sync(Word object) { text_field_sync_input(object,0); }
static Word text_resign(Word object) {
    if (text_first_responder!=object) return 1;
    struct TextFieldState *field=text_field_state(object);if((!field->multiline || field->editable) && !input_delegate_bool(object,2))return 0;
    text_first_responder=0;
    JNIEnv *env=attached_environment("UITextField resign"); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"hideGuestInput","(J)V");
    (*env)->CallVoidMethod(env,activity,method,(jlong)object); (*env)->DeleteLocalRef(env,cls);
    if(!field->multiline || field->editable){input_delegate_notify(object,3);platform_notification(field->multiline ? "UITextViewTextDidEndEditingNotification" : "UITextFieldTextDidEndEditingNotification",object,0);}
    return 1;
}
static Word text_become(Word object) {
    if (text_first_responder==object) { text_field_sync_input(object,1); return 1; }
    struct TextFieldState *field=text_field_state(object);if(!field->properties[16] || (field->multiline && !field->editable && !field->selectable) || ((!field->multiline || field->editable) && !input_delegate_bool(object,0)))return 0;
    if (text_first_responder && !text_resign(text_first_responder)) return 0;
    text_first_responder=object; text_field_sync_input(object,1);
    if(!field->multiline || field->editable){input_delegate_notify(object,1);platform_notification(field->multiline ? "UITextViewTextDidBeginEditingNotification" : "UITextFieldTextDidBeginEditingNotification",object,0);}return 1;
}
static Word text_is_first(Word object) { return object==text_first_responder; }
static Word text_can_become(Word object) { struct TextFieldState *field=text_field_state(object);return field->properties[16] && (!field->multiline || field->editable || field->selectable); }
static Word text_has_text(Word object) { return string_length(text_field_state(object)->properties[0],"length")!=0; }
static Word input_yes(void) { return 1; }
static Word input_no(void) { return 0; }
static Word input_end_editing(Word object,const char *selector,unsigned char force) {
    if (!text_first_responder || (object!=(Word)&main_window && !view_descendant(text_first_responder,selector,object))) return 1;
    return text_resign(text_first_responder);
}
static Word input_replace(Word object,Word value) {
    struct TextFieldState *field=text_field_state(object);
    struct TextView old=text_view(field->properties[0],"UITextField edit"),next=text_view(value,"UITextField edit");
    size_t prefix=0,suffix=0;
    while (prefix<old.length && prefix<next.length && old.units[prefix]==next.units[prefix]) ++prefix;
    while (suffix<old.length-prefix && suffix<next.length-prefix && old.units[old.length-1-suffix]==next.units[next.length-1-suffix]) ++suffix;
    Word replacement=text_from_units(external_class("_OBJC_CLASS_$_NSString"),next.units+prefix,next.length-prefix-suffix);
    struct GuestRange range={prefix,old.length-prefix-suffix}; free(old.owned); free(next.owned);
    if(field->search_owner && !ui_search_input_replace(field->search_owner,range,replacement))return field->properties[0];
    Word delegate=field->properties[12];const char *selector=(field->multiline ? input_delegate_views : input_delegate_fields)[4];Method method=input_callback(field,4);
    if (method && !(((Word (*)(Word,const char *,Word,struct GuestRange,Word))method)(delegate,selector,object,range,replacement)&255)) return field->properties[0];
    if(same_objects(field->properties[0],value))return field->properties[0];
    field->properties[0]=text_copy(value,"copy");field->attributed=0;++field->revision;
    field->selection=(struct GuestRange){string_length(value,"Text input")-suffix,0};input_content_changed(object);
    return field->properties[0];
}
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_editGuestInput)(JNIEnv *env,jobject activity,jlong token,jstring value,jboolean submit) {
    if ((Word)token!=text_first_responder) return NULL;
    may_unwind=1; int failed=setjmp(boundary); Word accepted=0;
    if (!failed) {
        if (submit) { input_delegate_bool(token,7); }
        else {
            const jchar *units=(*env)->GetStringChars(env,value,NULL);
            Word input=text_from_units(external_class("_OBJC_CLASS_$_NSString"),units,(*env)->GetStringLength(env,value));
            (*env)->ReleaseStringChars(env,value,units);
            accepted=input_replace(token,input);
        }
    }
    may_unwind=0;
    if (failed) {
        input_report_failure(env);return NULL;
    }
    return accepted ? http_java_string(env,accepted) : NULL;
}
JNIEXPORT jboolean JNICALL HOST_JNI(HostActivity_editorFocus)(JNIEnv *env,jobject instance,jlong token,jboolean focused){struct TextFieldState *field=view_state(token)->input;if(!field || field->closed)return JNI_FALSE;may_unwind=1;int failed=setjmp(boundary);Word result=0;if(!failed)result=focused ? text_become(token) : text_resign(token);may_unwind=0;if(failed)input_report_failure(env);return result ? JNI_TRUE : JNI_FALSE;}
JNIEXPORT void JNICALL HOST_JNI(HostActivity_editorSelection)(JNIEnv *env,jobject instance,jlong token,jint start,jint end){struct TextFieldState *field=view_state(token)->input;if(!field || field->closed || start<0 || end<start)return;may_unwind=1;int failed=setjmp(boundary);if(!failed && (size_t)end<=string_length(field->properties[0],"Text selection")){struct GuestRange next={(size_t)start,(size_t)(end-start)};if(memcmp(&next,&field->selection,sizeof(next))){field->selection=next;input_delegate_notify(token,6);}}may_unwind=0;if(failed)input_report_failure(env);}
static void text_insert(Word object,const char *selector,Word replacement){struct TextFieldState *field=text_field_state(object);if(!replacement)return;if(field->multiline && !field->editable)return;Word original=field->properties[0],next=text_replacing_range(original,"stringByReplacingCharactersInRange:withString:",field->selection,replacement);input_replace(object,next);if(field->properties[0]==original)return;if(field->multiline)ui_input_sync(object);else text_field_sync(object);}
static void text_delete(Word object,const char *selector){struct TextFieldState *field=text_field_state(object);struct GuestRange previous=field->selection;Word original=field->properties[0];if(!field->selection.length){if(!field->selection.location)return;--field->selection.location;field->selection.length=1;struct TextView text=text_view(field->properties[0],selector);if(field->selection.location && text.units[field->selection.location]>=0xdc00 && text.units[field->selection.location]<=0xdfff && text.units[field->selection.location-1]>=0xd800 && text.units[field->selection.location-1]<=0xdbff){--field->selection.location;++field->selection.length;}free(text.owned);}text_insert(object,selector,copy_string(""));if(field->properties[0]==original)field->selection=previous;}
static void text_editor_dealloc(Word object,const char *selector){struct TextFieldState *field=view_state(object)->input;if(!field)return;if(text_first_responder==object){text_first_responder=0;JNIEnv *env=attached_environment("Text input close");jclass cls=(*env)->GetObjectClass(env,activity);jmethodID method=(*env)->GetMethodID(env,cls,"hideGuestInput","(J)V");(*env)->CallVoidMethod(env,activity,method,(jlong)object);(*env)->DeleteLocalRef(env,cls);}field->closed=1;input_release_content(object);JNIEnv *env=attached_environment("Text input close");jclass cls=(*env)->GetObjectClass(env,activity);jmethodID method=(*env)->GetMethodID(env,cls,"closeGuestEditor","(J)V");(*env)->CallVoidMethod(env,activity,method,(jlong)object);(*env)->DeleteLocalRef(env,cls);}
static Method ui_input_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    const char *base=external_base(read_word(object));
    int multiline=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UITextView"));int field=multiline || (base && !strcmp(base,"_OBJC_CLASS_$_UITextField"));
    int responder=field || (base && (!strcmp(base,"_OBJC_CLASS_$_UIView") || !strcmp(base,"_OBJC_CLASS_$_UIWindow") || !strcmp(base,"_OBJC_CLASS_$_UIViewController") || !strcmp(base,"_OBJC_CLASS_$_UILabel") || !strcmp(base,"_OBJC_CLASS_$_UIImageView")));
    if (!responder) return NULL;
    if (!strcmp(selector,"resignFirstResponder")) return (Method)text_resign;
    if (!strcmp(selector,"isFirstResponder")) return (Method)text_is_first;
    if (!strcmp(selector,"canResignFirstResponder")) return (Method)input_yes;
    if (!strcmp(selector,"canBecomeFirstResponder")) return field ? (Method)text_can_become : (Method)input_no;
    if (!strcmp(selector,"becomeFirstResponder")) return field ? (Method)text_become : (Method)input_no;
    if (!strcmp(selector,"endEditing:")) return (Method)input_end_editing;
    if (field && !strcmp(selector,"hasText")) return (Method)text_has_text;
    if(field && !strcmp(selector,"insertText:"))return (Method)text_insert;if(field && !strcmp(selector,"deleteBackward"))return (Method)text_delete;
    if(multiline && !strcmp(selector,"dealloc"))return (Method)text_editor_dealloc;
    return NULL;
}
