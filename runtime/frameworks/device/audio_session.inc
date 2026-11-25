 
static struct HostObject audio_session;
static Word last_audio_route;
static Word shared_audio_session(Word cls) { audio_session.isa=cls; return (Word)&audio_session; }
static Word audio_session_error(const char *selector,Word *error,int code) {
    if (error) *error=code ? unsupported_service_error("NSOSStatusErrorDomain",code,selector) : 0;
    return code==0;
}
static Word audio_session_category(void) { return copy_string("AVAudioSessionCategoryAmbient"); }
static Word audio_session_set_category(Word object,const char *selector,Word category,Word *error) {
    int code=category && !strcmp(string_bytes(category,selector),"AVAudioSessionCategoryAmbient") ? 0 : -50;
    return audio_session_error(selector,error,code);
}
static Word audio_session_set_category_options(Word object,const char *selector,Word category,Word options,Word *error) {
    if (options) return audio_session_error(selector,error,-50);
    return audio_session_set_category(object,selector,category,error);
}
static Word audio_session_configure(int property,double value,const char *selector,Word *error) {
    JNIEnv *env=attached_environment(selector);
    jclass type=(*env)->GetObjectClass(env,activity);
    jmethodID configure=(*env)->GetMethodID(env,type,"audioSessionConfigure","(ID)I");
    int code=(*env)->CallIntMethod(env,activity,configure,(jint)property,(jdouble)value);
    (*env)->DeleteLocalRef(env,type);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); code=-50; }
    return audio_session_error(selector,error,code);
}
static Word audio_session_active(Word object,const char *selector,unsigned char active,Word *error) { return audio_session_configure(0,active!=0,selector,error); }
static Word audio_session_active_options(Word object,const char *selector,unsigned char active,Word options,Word *error) {
    if (options&~1UL) return audio_session_error(selector,error,-50);
    return audio_session_active(object,selector,active,error);
}
static Word audio_session_preferred(Word object,const char *selector,double value,Word *error) {
    return audio_session_configure(!strcmp(selector,"setPreferredSampleRate:error:") ? 1 : 2,value,selector,error);
}
static Word audio_session_channels(Word object,const char *selector,Word value,Word *error) { return audio_session_configure(3,(double)value,selector,error); }
static double audio_session_value(Word object,const char *selector) {
    int property=!strcmp(selector,"sampleRate") ? 0 : (!strcmp(selector,"maximumOutputNumberOfChannels") || !strcmp(selector,"outputNumberOfChannels")) ? 1 :
        !strcmp(selector,"IOBufferDuration") ? 2 : !strcmp(selector,"secondaryAudioShouldBeSilencedHint") ? 8 : !strcmp(selector,"isOtherAudioPlaying") ? 3 :
        !strcmp(selector,"preferredSampleRate") ? 4 : !strcmp(selector,"preferredIOBufferDuration") ? 5 : !strcmp(selector,"preferredOutputNumberOfChannels") ? 6 : 7;
    JNIEnv *env=attached_environment(selector);
    jclass type=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,type,"audioSessionProperty","(I)D");
    double value=(*env)->CallDoubleMethod(env,activity,method,(jint)property);
    (*env)->DeleteLocalRef(env,type);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Android audio property failed",selector); }
    return value;
}
static Word audio_session_integer(Word object,const char *selector) { return (Word)audio_session_value(object,selector); }
static float audio_session_volume(Word object,const char *selector) { return (float)audio_session_value(object,selector); }
static Word audio_session_route(Word object,const char *selector) {
    JNIEnv *env=attached_environment(selector);
    jclass type=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,type,"audioSessionRoute","()[[Ljava/lang/String;");
    jobjectArray devices=(*env)->CallObjectMethod(env,activity,method);
    (*env)->DeleteLocalRef(env,type);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Android audio route failed",selector); }
    Word outputs=sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    for (jsize i=0;devices && i<(*env)->GetArrayLength(env,devices);++i) {
        jobjectArray fields=(*env)->GetObjectArrayElement(env,devices,i);
        struct HostObject *port=(void *)host_allocate(external_class("_OBJC_CLASS_$_AVAudioSessionPortDescription"));
        for (unsigned f=0;f<3;++f) {
            jstring value=(*env)->GetObjectArrayElement(env,fields,f);
            port->content[f]=string_from_java(env,value,selector); (*env)->DeleteLocalRef(env,value);
        }
        sequence_add((void *)outputs,selector,(Word)port); (*env)->DeleteLocalRef(env,fields);
    }
    if (devices) (*env)->DeleteLocalRef(env,devices);
    struct HostObject *route=(void *)host_allocate(external_class("_OBJC_CLASS_$_AVAudioSessionRouteDescription"));
    route->content[0]=outputs; route->content[1]=sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    last_audio_route=(Word)route; return (Word)route;
}
static Word audio_route_outputs(struct HostObject *route) { return route->content[0]; }
static Word audio_route_inputs(struct HostObject *route) { return route->content[1]; }
static Word audio_port_type(struct HostObject *port) { return port->content[0]; }
static Word audio_port_name(struct HostObject *port) { return port->content[1]; }
static Word audio_port_uid(struct HostObject *port) { return port->content[2]; }
JNIEXPORT jstring JNICALL HOST_JNI(AudioSession_changed)(JNIEnv *env,jclass cls,jint event,jint value) {
    if (!started || !audio_session.isa) return NULL;
    may_unwind=1; snprintf(stage,sizeof(stage),"AVAudioSession notification");
    int failed=setjmp(boundary);
    if (!failed) {
        struct GuestMap *info=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
        if (!event) {
            if (last_audio_route) map_set(info,"route change",last_audio_route,copy_string("AVAudioSessionRouteChangePreviousRouteKey"));
            audio_session_route((Word)&audio_session,"currentRoute");
            map_set(info,"route change",number_integer(value),copy_string("AVAudioSessionRouteChangeReasonKey"));
            platform_notification("AVAudioSessionRouteChangeNotification",(Word)&audio_session,(Word)info);
        } else {
            map_set(info,"secondary audio",number_integer(value),copy_string("AVAudioSessionSilenceSecondaryAudioHintTypeKey"));
            platform_notification("AVAudioSessionSilenceSecondaryAudioHintNotification",(Word)&audio_session,(Word)info);
        }
    }
    may_unwind=0; return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static Method audio_session_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_AVAudioSession")) {
        if (class_receiver) return !strcmp(selector,"sharedInstance") ? (Method)shared_audio_session : NULL;
        if (!strcmp(selector,"category")) return (Method)audio_session_category;
        if (!strcmp(selector,"setCategory:error:")) return (Method)audio_session_set_category;
        if (!strcmp(selector,"setCategory:withOptions:error:")) return (Method)audio_session_set_category_options;
        if (!strcmp(selector,"setActive:error:")) return (Method)audio_session_active;
        if (!strcmp(selector,"setActive:withOptions:error:")) return (Method)audio_session_active_options;
        if (!strcmp(selector,"setPreferredSampleRate:error:") || !strcmp(selector,"setPreferredIOBufferDuration:error:")) return (Method)audio_session_preferred;
        if (!strcmp(selector,"setPreferredOutputNumberOfChannels:error:")) return (Method)audio_session_channels;
        if (!strcmp(selector,"sampleRate") || !strcmp(selector,"IOBufferDuration") || !strcmp(selector,"preferredSampleRate") || !strcmp(selector,"preferredIOBufferDuration")) return (Method)audio_session_value;
        if (!strcmp(selector,"maximumOutputNumberOfChannels") || !strcmp(selector,"outputNumberOfChannels") || !strcmp(selector,"preferredOutputNumberOfChannels") ||
            !strcmp(selector,"secondaryAudioShouldBeSilencedHint") || !strcmp(selector,"isOtherAudioPlaying")) return (Method)audio_session_integer;
        if (!strcmp(selector,"outputVolume")) return (Method)audio_session_volume;
        if (!strcmp(selector,"currentRoute")) return (Method)audio_session_route;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_AVAudioSessionRouteDescription")) {
        if (!strcmp(selector,"outputs")) return (Method)audio_route_outputs;
        if (!strcmp(selector,"inputs")) return (Method)audio_route_inputs;
    }
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_AVAudioSessionPortDescription")) {
        if (!strcmp(selector,"portType")) return (Method)audio_port_type;
        if (!strcmp(selector,"portName")) return (Method)audio_port_name;
        if (!strcmp(selector,"UID")) return (Method)audio_port_uid;
    }
    return NULL;
}
