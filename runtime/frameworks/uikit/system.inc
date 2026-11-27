 
struct GuestRect { double x,y,width,height; };
static Word value_rect(Word cls,const char *selector,struct GuestRect rect) {
    struct BoxedValue *value=calloc(1,sizeof(*value));
    if (!value) stop("Allocation failed",selector);
    value->isa=cls; value->type="{CGRect={CGPoint=dd}{CGSize=dd}}"; value->size=sizeof(rect);
    memcpy(value->bytes,&rect,sizeof(rect)); return (Word)value;
}
static struct GuestRect value_rect_get(struct BoxedValue *value,const char *selector) {
    if (value->size!=sizeof(struct GuestRect) || strcmp(value->type,"{CGRect={CGPoint=dd}{CGSize=dd}}"))
        stop("NSValue does not contain CGRect",selector);
    struct GuestRect rect; memcpy(&rect,value->bytes,sizeof(rect)); return rect;
}
struct DisplayState { int width,height,rotation,tablet,battery_state,low_power; double scale; float battery_level; };
static struct DisplayState display_state;
static struct DisplayState display_snapshot(void);
static pthread_mutex_t display_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct HostObject screen_object;
static int battery_monitoring,orientation_monitoring;
static Word current_user_notification_settings;
static int current_application_state;
static int application_lifecycle_ready;
static long application_badge;
 
static long application_status_style;
static int application_status_hidden=1;
static double application_status_height;
JNIEXPORT void JNICALL HOST_JNI(HostActivity_updateStatusBarSnapshot)(JNIEnv *env,jobject instance,jlong style,jboolean hidden,jdouble height) {
    __atomic_store_n(&application_status_style,(long)style,__ATOMIC_RELAXED);
    __atomic_store_n(&application_status_hidden,hidden!=0,__ATOMIC_RELAXED);
    __atomic_store(&application_status_height,&height,__ATOMIC_RELAXED);
}
static Word application_status_bar_style(void) { return (Word)__atomic_load_n(&application_status_style,__ATOMIC_RELAXED); }
static Word application_status_bar_hidden(void) { return (Word)__atomic_load_n(&application_status_hidden,__ATOMIC_RELAXED); }
static struct GuestRect application_status_bar_frame(void) {
    struct DisplayState display=display_snapshot(); double height;
    __atomic_load(&application_status_height,&height,__ATOMIC_RELAXED);
    return (struct GuestRect){0,0,display.width/display.scale,height};
}
static void application_set_status_style(Word object,const char *selector,long style,Word animated) {
    if (style<0 || style>3) stop("Invalid status bar style",selector);
    if (__atomic_exchange_n(&application_status_style,style,__ATOMIC_RELAXED)==style) return;
    JNIEnv *env=attached_environment(selector); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"guestStatusBarStyle","(J)V");
    (*env)->CallVoidMethod(env,activity,method,(jlong)style); (*env)->DeleteLocalRef(env,cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); stop("Android status bar style failed",selector); }
}
static void application_set_status_style_plain(Word object,const char *selector,long style) { application_set_status_style(object,selector,style,0); }
static void application_set_status_hidden(Word object,const char *selector,unsigned char hidden,Word animation) {
    if (__atomic_exchange_n(&application_status_hidden,hidden!=0,__ATOMIC_RELAXED)==(hidden!=0)) return;
    JNIEnv *env=attached_environment(selector); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"guestStatusBarHidden","(Z)V");
    (*env)->CallVoidMethod(env,activity,method,(jboolean)(hidden!=0)); (*env)->DeleteLocalRef(env,cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); stop("Android status bar visibility failed",selector); }
}
static void application_set_status_hidden_plain(Word object,const char *selector,unsigned char hidden) { application_set_status_hidden(object,selector,hidden,0); }
static Word application_get_badge(void) { return (Word)application_badge; }
static void application_set_badge(Word object,const char *selector,long badge) {
    if (badge<0) badge=0;
    if (badge>INT32_MAX) badge=INT32_MAX;
    JNIEnv *env=attached_environment(selector); jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,cls,"updateBadge","(I)V");
    (*env)->CallVoidMethod(env,activity,method,(jint)badge); (*env)->DeleteLocalRef(env,cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Android badge update failed",selector); }
    application_badge=badge;
}
struct BackgroundTask { Word identifier,block; struct BackgroundTask *next; };
static struct BackgroundTask *background_tasks;
static Word next_background_task=1;
static pthread_mutex_t background_task_mutex=PTHREAD_MUTEX_INITIALIZER;
static void deliver_display_notifications(void *context) {
    unsigned changes=(unsigned)(Word)context;
    if (changes&1) platform_notification("UIDeviceBatteryLevelDidChangeNotification",(Word)&device,0);
    if (changes&2) platform_notification("UIDeviceBatteryStateDidChangeNotification",(Word)&device,0);
    if (changes&4) platform_notification("UIDeviceOrientationDidChangeNotification",(Word)&device,0);
    if (changes&8) platform_notification("NSProcessInfoPowerStateDidChangeNotification",(Word)&process_info,0);
    if (changes&16) platform_notification("NSProcessInfoThermalStateDidChangeNotification",(Word)&process_info,0);
}
JNIEXPORT void JNICALL HOST_JNI(HostActivity_updateThermalSnapshot)(JNIEnv *env,jobject instance,jint status) {
    /* Android NONE/LIGHT/MODERATE/SEVERE/CRITICAL map to Apple's four levels;
     * shutdown/emergency remain critical. No per-frame platform query. */
    unsigned condition=status<=0 ? 0 : status<=2 ? 1 : status==3 ? 2 : 3;
    unsigned previous=__atomic_exchange_n(&process_thermal_condition,condition,__ATOMIC_RELAXED);
    if(previous!=condition && java_vm && started)schedule_main_function(deliver_display_notifications,(void *)(Word)16);
}

JNIEXPORT void JNICALL HOST_JNI(HostActivity_updateSystemSnapshot)(JNIEnv *env,jobject activity,jint width,jint height,jfloat scale,jint rotation,jboolean tablet,jfloat battery,jint battery_state,jboolean low_power) {
    pthread_mutex_lock(&display_mutex);
    struct DisplayState previous=display_state;
    display_state=(struct DisplayState){width,height,rotation,tablet,battery_state,low_power,scale,battery};
    pthread_mutex_unlock(&display_mutex);
#if I2A_DEVELOPMENT && I2A_SHADER_DIAGNOSTIC_COUNT
    if(previous.width!=width || previous.height!=height || previous.scale!=scale || previous.rotation!=rotation)
        LOG("[DIAG-shader-watch] display=%dx%d scale=%g rotation=%d",width,height,scale,rotation);
#endif
    if (java_vm && started) {
        unsigned changes=(battery_monitoring && previous.battery_level!=battery ? 1U : 0U) |
            (battery_monitoring && previous.battery_state!=battery_state ? 2U : 0U) |
            (orientation_monitoring && previous.rotation!=rotation ? 4U : 0U) | (previous.low_power!=low_power ? 8U : 0U);
        if (changes) schedule_main_function(deliver_display_notifications,(void *)(Word)changes);
    }
}
static struct DisplayState display_snapshot(void) {
    pthread_mutex_lock(&display_mutex);
    struct DisplayState result=display_state;
    pthread_mutex_unlock(&display_mutex);
    if (result.width<1 || result.height<1 || result.scale<=0) stop("Display snapshot unavailable","UIKit");
    return result;
}
static Word device_orientation(void) {
    struct DisplayState display=display_snapshot();
    if (display.width>display.height) return display.rotation==3 ? 4 : 3;
    return display.rotation==2 ? 2 : 1;
}
static Word interface_orientation(void) {
    Word orientation=device_orientation();
    return orientation==3 ? 4 : orientation==4 ? 3 : orientation;
}
static Word device_idiom(void) { return display_snapshot().tablet ? 1 : 0; }
static Word device_multitasking(void) { return 1; }
static void device_battery_monitor(Word self,const char *selector,unsigned char enabled) { battery_monitoring=enabled!=0; }
static Word device_battery_monitored(void) { return battery_monitoring; }
static float device_battery_level(void) { return battery_monitoring ? display_snapshot().battery_level : -1; }
static Word device_battery_state(void) { return battery_monitoring ? (Word)display_snapshot().battery_state : 0; }
static Word low_power_mode(void) { return display_snapshot().low_power; }
static void orientation_begin(void) {
    if (__atomic_fetch_add(&orientation_monitoring,1,__ATOMIC_RELAXED)==0)
        schedule_main_function(deliver_display_notifications,(void *)(Word)4);
}
static void orientation_end(void) {
    int value=__atomic_load_n(&orientation_monitoring,__ATOMIC_RELAXED);
    while (value>0 && !__atomic_compare_exchange_n(&orientation_monitoring,&value,value-1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
static Word orientation_generating(void) { return __atomic_load_n(&orientation_monitoring,__ATOMIC_RELAXED)>0; }
static Word main_screen(Word cls) { screen_object.isa=cls; return (Word)&screen_object; }
static struct GuestRect screen_bounds(void) { struct DisplayState s=display_snapshot(); return (struct GuestRect){0,0,s.width/s.scale,s.height/s.scale}; }
static struct GuestRect screen_native_bounds(void) { struct DisplayState s=display_snapshot(); return (struct GuestRect){0,0,s.width,s.height}; }
static double screen_scale(void) { return display_snapshot().scale; }
static Word notification_settings(Word cls,const char *selector,Word types,Word categories) {
    struct HostObject *settings=(void *)host_allocate(cls);
    settings->content[0]=types; settings->content[1]=categories;
    return (Word)settings;
}
static Word notification_types(struct HostObject *settings) { return settings->content[0]; }
static Word notification_categories(struct HostObject *settings) { return settings->content[1]; }
static void did_register_user_notifications(void *context) {
    Method method=find_implementation(app_delegate,"application:didRegisterUserNotificationSettings:");
    if (method) method(app_delegate,"application:didRegisterUserNotificationSettings:",application_object,(Word)context,0,0);
}
static void notification_legacy_authorize(Word,const char *,struct HostObject *);
static void register_user_notifications(Word self,const char *selector,struct HostObject *requested) { notification_legacy_authorize(self,selector,requested); }
static Word get_user_notification_settings(void) {
    if (!current_user_notification_settings) current_user_notification_settings=notification_settings(external_class("_OBJC_CLASS_$_UIUserNotificationSettings"),"currentUserNotificationSettings",0,0);
    return current_user_notification_settings;
}
static void remote_registration_failed(void *context) {
    Method method=find_implementation(app_delegate,"application:didFailToRegisterForRemoteNotificationsWithError:");
    if (method) method(app_delegate,"application:didFailToRegisterForRemoteNotificationsWithError:",application_object,(Word)context,0,0);
}
static void register_remote_notifications(void) {
    struct GuestMap *info=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
    map_set(info,"APNs",copy_string("APNs registration is unavailable on Android"),copy_string("NSLocalizedDescription"));
    Word error=error_with_domain(external_class("_OBJC_CLASS_$_NSError"),"APNs",copy_string("I2ACompatibilityErrorDomain"),1,(Word)info);
    schedule_main_function(remote_registration_failed,(void *)error);
}
static Word remote_notifications_registered(void) { return 0; }
static void cancel_local_notifications(void) {
    JNIEnv *env=attached_environment("cancelAllLocalNotifications");
    jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID cancel=(*env)->GetMethodID(env,cls,"cancelLocalNotifications","()V");
    (*env)->CallVoidMethod(env,activity,cancel);
    (*env)->DeleteLocalRef(env,cls);
}
static Word application_state(void) { return (Word)__atomic_load_n(&current_application_state,__ATOMIC_ACQUIRE); }
static void application_set_delegate(Word object,const char *selector,Word delegate) {
    Word key=copy_string("delegate"); key_will_change(object,selector,key); app_delegate=delegate; key_did_change(object,selector,key);
}
static struct HostObject user_notification_center;
static pthread_once_t user_notification_once=PTHREAD_ONCE_INIT;
static void user_notification_init(void) { user_notification_center.isa=external_class("_OBJC_CLASS_$_UNUserNotificationCenter"); }
static Word current_user_notification_center(void) {
    pthread_once(&user_notification_once,user_notification_init); return (Word)&user_notification_center;
}
static void user_notification_set_delegate(Word object,const char *selector,Word delegate) {
    __atomic_store_n(&((struct HostObject *)object)->content[0],delegate,__ATOMIC_RELEASE);
}
static Word user_notification_get_delegate(Word object,const char *selector) {
    return __atomic_load_n(&((struct HostObject *)object)->content[0],__ATOMIC_ACQUIRE);
}
static Word application_protected_data(void) {
    JNIEnv *env=attached_environment("isProtectedDataAvailable"); jclass type=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,type,"keychainUnlocked","()Z");
    Word result=(*env)->CallBooleanMethod(env,activity,method); (*env)->DeleteLocalRef(env,type); return result;
}
static void application_lifecycle_event(const char *selector,const char *notification) {
    Method callback=find_implementation(app_delegate,selector);
    if (callback) callback(app_delegate,selector,application_object,0,0,0);
    platform_notification(notification,application_object,0);
#if I2A_DEVELOPMENT
    LOG("[DIAG-lifecycle] %s state=%d tid=%d",selector,__atomic_load_n(&current_application_state,__ATOMIC_ACQUIRE),gettid());
#endif
}
static void application_lifecycle_transition(int value) {
    int previous=__atomic_load_n(&current_application_state,__ATOMIC_ACQUIRE);
    if (!application_lifecycle_ready || previous==value) {
        __atomic_store_n(&current_application_state,value,__ATOMIC_RELEASE); return;
    }
    if (previous==0) {
        __atomic_store_n(&current_application_state,1,__ATOMIC_RELEASE);
        application_lifecycle_event("applicationWillResignActive:","UIApplicationWillResignActiveNotification");
    } else if (previous==2) {
        __atomic_store_n(&current_application_state,1,__ATOMIC_RELEASE);
        application_lifecycle_event("applicationWillEnterForeground:","UIApplicationWillEnterForegroundNotification");
    }
    __atomic_store_n(&current_application_state,value,__ATOMIC_RELEASE);
    if (value==0) application_lifecycle_event("applicationDidBecomeActive:","UIApplicationDidBecomeActiveNotification");
    else if (value==2) application_lifecycle_event("applicationDidEnterBackground:","UIApplicationDidEnterBackgroundNotification");
}
static void application_lifecycle_start(void) {
    if (application_lifecycle_ready) return;
    int desired=__atomic_exchange_n(&current_application_state,1,__ATOMIC_ACQ_REL);
    application_lifecycle_ready=1;
    application_lifecycle_transition(desired);
}
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_updateApplicationState)(JNIEnv *env,jobject instance,jint value) {
    if (value<0 || value>2) return (*env)->NewStringUTF(env,"Invalid application state");
    may_unwind=1;
    snprintf(stage,sizeof(stage),"Android application lifecycle state=%d",value);
    int failed=setjmp(boundary);
    if (!failed) application_lifecycle_transition(value);
    may_unwind=0;
    return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static void end_background_task(Word self,const char *selector,Word identifier) {
    pthread_mutex_lock(&background_task_mutex);
    struct BackgroundTask **slot=&background_tasks;
    while (*slot && (*slot)->identifier!=identifier) slot=&(*slot)->next;
    struct BackgroundTask *task=*slot;
    if (task) *slot=task->next;
    pthread_mutex_unlock(&background_task_mutex);
    if (!task) return;
    JNIEnv *env=attached_environment(selector);
    jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID finish=(*env)->GetMethodID(env,cls,"endBackgroundTask","(J)V");
    (*env)->CallVoidMethod(env,activity,finish,(jlong)identifier);
    (*env)->DeleteLocalRef(env,cls);
    if (task->block) _Block_release((void *)task->block);
    free(task);
}
static Word begin_background_task(Word self,const char *selector,Word block) {
    struct BackgroundTask *task=calloc(1,sizeof(*task));
    if (!task) return UINT64_MAX;
    task->block=block ? (Word)_Block_copy((void *)block) : 0;
    pthread_mutex_lock(&background_task_mutex);
    task->identifier=next_background_task++; task->next=background_tasks; background_tasks=task;
    Word identifier=task->identifier;
    pthread_mutex_unlock(&background_task_mutex);
    JNIEnv *env=attached_environment(selector);
    jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID begin=(*env)->GetMethodID(env,cls,"beginBackgroundTask","(J)Z");
    jboolean accepted=(*env)->CallBooleanMethod(env,activity,begin,(jlong)identifier);
    (*env)->DeleteLocalRef(env,cls);
    if (!accepted) { end_background_task(self,selector,identifier); return UINT64_MAX; }
    return identifier;
}
static Word begin_named_background_task(Word self,const char *selector,Word name,Word block) { return begin_background_task(self,selector,block); }
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_expireBackgroundTask)(JNIEnv *env,jobject instance,jlong identifier) {
    pthread_mutex_lock(&background_task_mutex);
    struct BackgroundTask *task=background_tasks;
    while (task && task->identifier!=(Word)identifier) task=task->next;
    Word block=task && task->block ? (Word)_Block_copy((void *)task->block) : 0;
    pthread_mutex_unlock(&background_task_mutex);
    int failed=0;
    if (block) {
        may_unwind=1;
        snprintf(stage,sizeof(stage),"Background task expiration");
        failed=setjmp(boundary);
        if (!failed) ((void (*)(Word))read_word(block+16))(block);
        may_unwind=0;
        _Block_release((void *)block);
    }
    end_background_task(application_object,"endBackgroundTask:",(Word)identifier);
    return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static unsigned accessibility_state;
JNIEXPORT void JNICALL HOST_JNI(HostActivity_updateGuestAccessibility)(JNIEnv *env,jobject instance,jboolean spoken) { __atomic_store_n(&accessibility_state,spoken ? 1U : 0U,__ATOMIC_RELEASE); }
static unsigned char accessibility_voice_running(void) { return (__atomic_load_n(&accessibility_state,__ATOMIC_ACQUIRE)&1)!=0; }
static unsigned char accessibility_reduce_transparency(void) { return 0; }
static void accessibility_post(unsigned notification,Word argument) {
    Word text=0,token=0;
    if(argument) { Word cls=read_word(argument);if(class_inherits(cls,external_class("_OBJC_CLASS_$_NSString")))text=argument;else if(class_inherits(cls,external_class("_OBJC_CLASS_$_UIView")))token=argument; }
    JNIEnv *env=attached_environment("Accessibility notification");static jmethodID method;
    if(!method){jclass type=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,type,"postGuestAccessibility","(IJLjava/lang/String;)V");(*env)->DeleteLocalRef(env,type);}
    jstring value=http_java_string(env,text);(*env)->CallVoidMethod(env,activity,method,(jint)notification,(jlong)token,value);if(value)(*env)->DeleteLocalRef(env,value);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Accessibility notification failed","Android");}
}
static Word resolve_accessibility_import(const char *name) {
    if(!strcmp(name,"_UIAccessibilityIsVoiceOverRunning"))return (Word)accessibility_voice_running;
    if(!strcmp(name,"_UIAccessibilityIsReduceTransparencyEnabled"))return (Word)accessibility_reduce_transparency;
    if(!strcmp(name,"_UIAccessibilityPostNotification"))return (Word)accessibility_post;
    return 0;
}
enum { NOTICE_TITLE,NOTICE_SUBTITLE,NOTICE_BODY,NOTICE_CATEGORY,NOTICE_THREAD,NOTICE_IMAGE,NOTICE_INFO,NOTICE_BADGE,NOTICE_SOUND,
    NOTICE_DATE,NOTICE_ZONE,NOTICE_REPEAT,NOTICE_ACTION,NOTICE_HAS_ACTION,NOTICE_REPEAT_CALENDAR,NOTICE_PROPERTY_COUNT };
struct NotificationValues { Word properties[NOTICE_PROPERTY_COUNT]; };
struct NotificationProperty { const char *getter,*setter;unsigned index,kind; };
static const struct NotificationProperty notification_content_properties[]={
    {"title","setTitle:",NOTICE_TITLE,1},{"subtitle","setSubtitle:",NOTICE_SUBTITLE,1},{"body","setBody:",NOTICE_BODY,1},
    {"categoryIdentifier","setCategoryIdentifier:",NOTICE_CATEGORY,1},{"threadIdentifier","setThreadIdentifier:",NOTICE_THREAD,1},
    {"launchImageName","setLaunchImageName:",NOTICE_IMAGE,1},{"userInfo","setUserInfo:",NOTICE_INFO,2},
    {"badge","setBadge:",NOTICE_BADGE,2},{"sound","setSound:",NOTICE_SOUND,2}
};
static const struct NotificationProperty local_notification_properties[]={
    {"alertTitle","setAlertTitle:",NOTICE_TITLE,1},{"alertBody","setAlertBody:",NOTICE_BODY,1},
    {"category","setCategory:",NOTICE_CATEGORY,1},{"alertLaunchImage","setAlertLaunchImage:",NOTICE_IMAGE,1},
    {"userInfo","setUserInfo:",NOTICE_INFO,2},{"applicationIconBadgeNumber","setApplicationIconBadgeNumber:",NOTICE_BADGE,0},
    {"soundName","setSoundName:",NOTICE_SOUND,1},{"fireDate","setFireDate:",NOTICE_DATE,2},{"timeZone","setTimeZone:",NOTICE_ZONE,2},
    {"repeatInterval","setRepeatInterval:",NOTICE_REPEAT,0},{"alertAction","setAlertAction:",NOTICE_ACTION,1},
    {"hasAction","setHasAction:",NOTICE_HAS_ACTION,0},{"repeatCalendar","setRepeatCalendar:",NOTICE_REPEAT_CALENDAR,2}
};
static struct NotificationValues *notification_values(Word object) {
    struct HostObject *host=(void *)object;
    if(!host->content[0]){struct NotificationValues *values=calloc(1,sizeof(*values));if(!values)stop("Allocation failed","Notification content");values->properties[NOTICE_INFO]=empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));values->properties[NOTICE_HAS_ACTION]=1;host->content[0]=(Word)values;}
    return (void *)host->content[0];
}
static int notification_local(Word object) { return class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UILocalNotification")); }
static const struct NotificationProperty *notification_property(Word object,const char *selector,int setter) {
    int local=notification_local(object);const struct NotificationProperty *properties=local ? local_notification_properties : notification_content_properties;
    size_t count=local ? COUNT(local_notification_properties) : COUNT(notification_content_properties);
    for(size_t i=0;i<count;++i)if(!strcmp(selector,setter ? properties[i].setter : properties[i].getter))return properties+i;return NULL;
}
static Word notification_get_property(Word object,const char *selector) {
    const struct NotificationProperty *property=notification_property(object,selector,0);if(!property)return 0;
    Word value=notification_values(object)->properties[property->index];return !value && property->kind==1 && !notification_local(object) ? copy_string("") : value;
}
static void notification_set_property(Word object,const char *selector,Word value) {
    const struct NotificationProperty *property=notification_property(object,selector,1);if(!property)return;
    notification_values(object)->properties[property->index]=property->kind ? copied_object(value) : value;
}
static Word notification_content_init(Word object) { notification_values(object);return object; }
static Word notification_content_copy(Word object,const char *selector,Word zone) {
    Word cls=notification_local(object) ? read_word(object) : external_class(!strncmp(selector,"mutableCopy",11) ? "_OBJC_CLASS_$_UNMutableNotificationContent" : "_OBJC_CLASS_$_UNNotificationContent");
    Word result=host_allocate(cls);struct NotificationValues *copy=notification_values(result);*copy=*notification_values(object);
    if(copy->properties[NOTICE_INFO])copy->properties[NOTICE_INFO]=copied_object(copy->properties[NOTICE_INFO]);return result;
}
static Word notification_sound_default(Word cls) { struct HostObject *sound=(void *)host_allocate(cls);sound->content[0]=copy_string("default");return (Word)sound; }
static Word notification_sound_named(Word cls,const char *selector,Word name) { if(!name)return 0;struct HostObject *sound=(void *)host_allocate(cls);sound->content[0]=text_copy(name,"copy");return (Word)sound; }
static Word notification_request_create(Word cls,const char *selector,Word identifier,Word content,Word trigger) {
    if(!identifier || !*string_bytes(identifier,selector) || !content)stop("Invalid notification request",selector);
    struct HostObject *request=(void *)host_allocate(cls);request->content[0]=text_copy(identifier,"copy");request->content[1]=notification_content_copy(content,"copy",0);request->content[2]=trigger;return (Word)request;
}
static Word notification_record_property(struct HostObject *object,const char *selector) {
    if(!strcmp(selector,"identifier") || !strcmp(selector,"request") || !strcmp(selector,"notification"))return object->content[0];
    if(!strcmp(selector,"content") || !strcmp(selector,"date") || !strcmp(selector,"actionIdentifier"))return object->content[1];
    return object->content[2];
}
static Word notification_trigger_create(Word cls,const char *selector,double interval,unsigned char repeats) {
    if(!isfinite(interval) || interval<=0 || interval>(double)INT64_MAX/1000 || (repeats && interval<60))stop("Invalid notification time interval",selector);
    struct HostObject *trigger=(void *)host_allocate(cls);memcpy(trigger->content,&interval,sizeof(interval));trigger->content[1]=repeats!=0;return (Word)trigger;
}
static double notification_trigger_interval(struct HostObject *trigger) { double interval;memcpy(&interval,trigger->content,sizeof(interval));return interval; }
static Word notification_trigger_repeats(struct HostObject *trigger) { return trigger->content[1]; }
static Word notification_trigger_next(struct HostObject *trigger) { return date_epoch(external_class("_OBJC_CLASS_$_NSDate"),"nextTriggerDate",scheduling_now()+notification_trigger_interval(trigger)); }
static Word notification_json(Word object,Word *error) {
    return json_encode(external_class("_OBJC_CLASS_$_NSJSONSerialization"),"Notification data",object,0,error);
}
static jstring ui_java_json(JNIEnv *env,Word object,Word *error) {
    Word data=notification_json(object,error);if(!data)return NULL;
    Word string=text_decode(external_class("_OBJC_CLASS_$_NSString"),(void *)data_buffer((void *)data),read_word(data+8),4);
    jstring result=http_java_string(env,string);cf_release(data);cf_release(string);return result;
}
static Word notification_host_operation(const char *operation,Word payload,Word *error) {
    JNIEnv *env=attached_environment(operation);static jmethodID method;
    if(!method){jclass type=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,type,"notificationOperation","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");(*env)->DeleteLocalRef(env,type);}
    jstring name=(*env)->NewStringUTF(env,operation),input=payload ? ui_java_json(env,payload,error) : NULL;
    if(payload && !input){(*env)->DeleteLocalRef(env,name);return 0;}
    jstring reply=(*env)->CallObjectMethod(env,activity,method,name,input);Word result=reply ? string_from_java(env,reply,operation) : 0;
    (*env)->DeleteLocalRef(env,name);if(input)(*env)->DeleteLocalRef(env,input);if(reply)(*env)->DeleteLocalRef(env,reply);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Android notification operation failed",operation);}return result;
}
static int notification_authorization_status(void) {
    JNIEnv *env=attached_environment("Notification authorization");static jmethodID method;
    if(!method){jclass type=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,type,"notificationAuthorization","()I");(*env)->DeleteLocalRef(env,type);}
    return (*env)->CallIntMethod(env,activity,method);
}
static Word notification_error(const char *description,int code) {
    struct GuestMap *info=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));map_set(info,"Notification error",copy_string(description),copy_string("NSLocalizedDescription"));
    return error_with_domain(external_class("_OBJC_CLASS_$_NSError"),"Notification error",copy_string("UNErrorDomain"),code,(Word)info);
}
struct UICompletion { Word block,value,error;int two_arguments; };
static void ui_completion_run(void *context) {
    struct UICompletion *completion=context;struct GuestCallBoundary call={.previous=active_guest_call_boundary};active_guest_call_boundary=&call;
    int failed=setjmp(call.environment);
    if(!failed){Word invoke=read_word(completion->block+16);if(completion->two_arguments)((void (*)(Word,Word,Word))invoke)(completion->block,completion->value,completion->error);else((void (*)(Word,Word))invoke)(completion->block,completion->value);}
    active_guest_call_boundary=call.previous;_Block_release((void *)completion->block);free(completion);
    if(failed){char failure[sizeof(status)];memcpy(failure,status,sizeof(failure));stop("Notification callback failed",failure);}
}
static void ui_complete_callback(Word block,Word value,Word error,int two) {
    if(!block)return;struct UICompletion *completion=malloc(sizeof(*completion));if(!completion)stop("Allocation failed","Notification callback");
    *completion=(struct UICompletion){(Word)_Block_copy((void *)block),value,error,two};schedule_main_function(ui_completion_run,completion);
}
static void notification_authorize(Word object,const char *selector,Word options,Word block) {
    if(!block)return;
    if(options&~(Word)7){ui_complete_callback(block,0,notification_error("These notification authorization options are unavailable.",1),1);return;}
    JNIEnv *env=attached_environment(selector);jclass type=(*env)->GetObjectClass(env,activity);jmethodID method=(*env)->GetMethodID(env,type,"requestNotificationAuthorization","(J)V");
    Word copy=(Word)_Block_copy((void *)block);(*env)->CallVoidMethod(env,activity,method,(jlong)copy);(*env)->DeleteLocalRef(env,type);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);_Block_release((void *)copy);ui_complete_callback(block,0,notification_error("Unable to request notification authorization.",1),1);}
}
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_completeNotificationAuthorization)(JNIEnv *env,jobject instance,jlong block,jboolean allowed) {
    may_unwind=1;int failed=setjmp(boundary);
    if(!failed)((void (*)(Word,unsigned char,Word))read_word((Word)block+16))((Word)block,allowed!=0,0);
    may_unwind=0;_Block_release((void *)block);return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
struct NotificationLegacyBlock { struct Block_layout layout;Word settings; };
static void notification_legacy_reply(struct NotificationLegacyBlock *block,unsigned char allowed,Word error) {
    struct HostObject *requested=(void *)block->settings;current_user_notification_settings=notification_settings(requested->isa,"Notification permission",allowed ? requested->content[0] : 0,requested->content[1]);did_register_user_notifications((void *)current_user_notification_settings);
}
static void notification_legacy_authorize(Word object,const char *selector,struct HostObject *requested) {
    if(!requested)stop("Notification settings are required",selector);
    static struct Block_descriptor_1 descriptor={0,sizeof(struct NotificationLegacyBlock)};
    struct NotificationLegacyBlock callback={.layout={.isa=_NSConcreteStackBlock,.invoke=(void (*)(void *,...))notification_legacy_reply,.descriptor=&descriptor},.settings=(Word)requested};
    notification_authorize(current_user_notification_center(),selector,7,(Word)&callback);
}
static Word notification_settings_create(void) { struct HostObject *settings=(void *)host_allocate(external_class("_OBJC_CLASS_$_UNNotificationSettings"));settings->content[0]=(Word)notification_authorization_status();return (Word)settings; }
static Word notification_settings_value(struct HostObject *settings,const char *selector) {
    if(!strcmp(selector,"authorizationStatus"))return settings->content[0];
    if(!strcmp(selector,"lockScreenSetting") || !strcmp(selector,"notificationCenterSetting") || !strcmp(selector,"alertSetting") || !strcmp(selector,"soundSetting") || !strcmp(selector,"badgeSetting"))return settings->content[0]==2 ? 2 : 1;
    if(!strcmp(selector,"alertStyle"))return settings->content[0]==2 ? 1 : 0;return 0;
}
static void notification_get_settings(Word object,const char *selector,Word block) { ui_complete_callback(block,notification_settings_create(),0,0); }
static void notification_payload_string(struct GuestMap *map,const char *key,Word value) { if(value)map_set(map,"Notification payload",value,copy_string(key)); }
static Word notification_payload(Word request,Word legacy,Word *error) {
    struct HostObject *record=(void *)request;struct NotificationValues *values=notification_values(legacy ? legacy : record->content[1]);Word *properties=values->properties;
    double fire=scheduling_now(),interval=0;Word repeat=legacy ? properties[NOTICE_REPEAT] : 0;
    if(legacy && properties[NOTICE_DATE])fire=date_seconds(properties[NOTICE_DATE]);
    if(!legacy && record->content[2]){struct HostObject *trigger=(void *)record->content[2];if(!class_inherits(trigger->isa,external_class("_OBJC_CLASS_$_UNTimeIntervalNotificationTrigger"))){*error=notification_error("This notification trigger is unavailable.",1);return 0;}interval=notification_trigger_interval(trigger);fire+=interval;if(!trigger->content[1])interval=0;}
    if(!isfinite(fire) || fire<0 || fire>(double)INT64_MAX/1000){*error=notification_error("Invalid notification fire date.",1);return 0;}
    if(repeat && repeat!=4 && repeat!=8 && repeat!=16 && repeat!=32 && repeat!=64 && repeat!=128 && repeat!=256 && repeat!=8192){*error=notification_error("This calendar repeat unit is unavailable.",1);return 0;}
    struct GuestMap *map=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));char identifier[96];
    if(legacy)snprintf(identifier,sizeof(identifier),"local-%llu-%llu",(unsigned long long)(scheduling_now()*1000000),(unsigned long long)legacy);
    notification_payload_string(map,"id",legacy ? copy_string(identifier) : record->content[0]);
    const unsigned indices[]={NOTICE_TITLE,NOTICE_SUBTITLE,NOTICE_BODY,NOTICE_CATEGORY,NOTICE_THREAD,NOTICE_IMAGE,NOTICE_INFO};
    const char *keys[]={"title","subtitle","body","category","thread","image","userInfo"};
    for(unsigned i=0;i<COUNT(indices);++i)notification_payload_string(map,keys[i],properties[indices[i]]);
    Word sound=properties[NOTICE_SOUND];if(sound && !legacy)sound=((struct HostObject *)sound)->content[0];
    if(legacy && sound && !strcmp(string_bytes(sound,"Local notification sound"),"UILocalNotificationDefaultSoundName"))sound=copy_string("default");
    notification_payload_string(map,"sound",sound);notification_payload_string(map,"badge",legacy ? number_integer(properties[NOTICE_BADGE]) : properties[NOTICE_BADGE]);
    map_set(map,"Notification payload",number_integer((Word)(fire*1000)),copy_string("fire"));map_set(map,"Notification payload",number_integer((Word)(interval*1000)),copy_string("repeat"));
    map_set(map,"Notification payload",number_integer(repeat),copy_string("calendarUnit"));map_set(map,"Notification payload",number_bool(external_class("_OBJC_CLASS_$_NSNumber"),"Legacy notification",legacy!=0),copy_string("legacy"));
    if(legacy){notification_payload_string(map,"action",properties[NOTICE_ACTION]);map_set(map,"Notification payload",number_bool(external_class("_OBJC_CLASS_$_NSNumber"),"Notification action",properties[NOTICE_HAS_ACTION]),copy_string("hasAction"));
        if(properties[NOTICE_ZONE])notification_payload_string(map,"timeZone",timezone_name((void *)properties[NOTICE_ZONE]));}
    return (Word)map;
}
static Word notification_submit(Word request,Word legacy) {
    Word error=0,payload=notification_payload(request,legacy,&error);if(!payload)return error;
    Word failure=notification_host_operation("schedule",payload,&error);return error ? error : failure ? notification_error(string_bytes(failure,"Notification scheduling"),1) : 0;
}
static void notification_add_request(Word object,const char *selector,Word request,Word block) { Word error=request ? notification_submit(request,0) : notification_error("A notification request is required.",1);ui_complete_callback(block,error,0,0); }
static Word notification_from_payload(Word payload,int local,int delivered) {
    struct GuestMap *map=(void *)payload;Word cls=external_class(local ? "_OBJC_CLASS_$_UILocalNotification" : "_OBJC_CLASS_$_UNMutableNotificationContent");Word content=host_allocate(cls);Word *properties=notification_values(content)->properties;
    const unsigned indices[]={NOTICE_TITLE,NOTICE_SUBTITLE,NOTICE_BODY,NOTICE_CATEGORY,NOTICE_THREAD,NOTICE_IMAGE,NOTICE_INFO};const char *keys[]={"title","subtitle","body","category","thread","image","userInfo"};
    for(unsigned i=0;i<COUNT(indices);++i){Word value=map_get(map,"Notification payload",copy_string(keys[i]));if(value)properties[indices[i]]=value;}
    Word badge=map_get(map,"Notification payload",copy_string("badge")),sound=map_get(map,"Notification payload",copy_string("sound")),fire=map_get(map,"Notification payload",copy_string("fire"));
    properties[NOTICE_BADGE]=local && badge ? numeric_integer((void *)badge) : badge;
    properties[NOTICE_SOUND]=local ? sound : sound ? notification_sound_named(external_class("_OBJC_CLASS_$_UNNotificationSound"),"soundNamed:",sound) : 0;
    properties[NOTICE_DATE]=date_epoch(external_class("_OBJC_CLASS_$_NSDate"),"Notification date",fire ? numeric_double((void *)fire)/1000 : scheduling_now());
    Word repeat=map_get(map,"Notification payload",copy_string("calendarUnit")),zone=map_get(map,"Notification payload",copy_string("timeZone"));properties[NOTICE_REPEAT]=repeat ? numeric_integer((void *)repeat) : 0;
    properties[NOTICE_ACTION]=map_get(map,"Notification payload",copy_string("action"));Word action=map_get(map,"Notification payload",copy_string("hasAction"));if(action)properties[NOTICE_HAS_ACTION]=numeric_bool((void *)action);
    if(zone)properties[NOTICE_ZONE]=timezone_abbreviation(external_class("_OBJC_CLASS_$_NSTimeZone"),"Notification timezone",zone);
    if(local)return content;
    Word interval=map_get(map,"Notification payload",copy_string("repeat")),trigger=0;double seconds=interval ? numeric_double((void *)interval)/1000 : 0;
    if(seconds>0)trigger=notification_trigger_create(external_class("_OBJC_CLASS_$_UNTimeIntervalNotificationTrigger"),"Notification trigger",seconds,1);
    else if(fire){seconds=fmax(0.001,numeric_double((void *)fire)/1000-scheduling_now());trigger=notification_trigger_create(external_class("_OBJC_CLASS_$_UNTimeIntervalNotificationTrigger"),"Notification trigger",seconds,0);}
    Word request=notification_request_create(external_class("_OBJC_CLASS_$_UNNotificationRequest"),"Notification request",map_get(map,"Notification payload",copy_string("id")),content,trigger);
    if(!delivered)return request;struct HostObject *notice=(void *)host_allocate(external_class("_OBJC_CLASS_$_UNNotification"));notice->content[0]=request;notice->content[1]=properties[NOTICE_DATE];return (Word)notice;
}
static Word notification_query(int delivered,int local) {
    Word text=notification_host_operation(delivered ? "delivered" : "pending",0,NULL),result=sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));if(!text)return result;
    Word data=encoded_string(text,"Notification query",4),error=0;struct Sequence *payloads=(void *)json_parse(0,"Notification query",(void *)data,0,&error);
    if(payloads)for(size_t i=0;i<payloads->count;++i){struct GuestMap *payload=(void *)payloads->items[i];Word legacy=map_get(payload,"Notification query",copy_string("legacy"));if(local && (!legacy || !numeric_bool((void *)legacy)))continue;sequence_add((void *)result,"Notification query",notification_from_payload((Word)payload,local,delivered));}
    return result;
}
static void notification_get_requests(Word object,const char *selector,Word block) { ui_complete_callback(block,notification_query(!strcmp(selector,"getDeliveredNotificationsWithCompletionHandler:"),0),0,0); }
static void notification_remove_requests(Word object,const char *selector,Word identifiers) { notification_host_operation(strstr(selector,"Delivered") ? "removeDelivered" : "removePending",identifiers,NULL); }
static void notification_remove_all(Word object,const char *selector) { notification_remove_requests(object,selector,0); }
static Word notification_scheduled_local(void) { return notification_query(0,1); }
static Word notification_enabled_remote_types(void) { return 0; }
static void notification_schedule_local(Word object,const char *selector,Word local) { Word error=local ? notification_submit(0,local) : notification_error("A local notification is required.",1);if(error)LOG("Local notification failed: %s",string_bytes(error_description((void *)error),selector)); }
static void notification_cancel_local(Word object,const char *selector,Word local) {
    if(!local)return;Word pending=notification_host_operation("pending",0,NULL);if(!pending)return;
    Word data=encoded_string(pending,selector,4),error=0;struct Sequence *payloads=(void *)json_parse(0,selector,(void *)data,0,&error);Word ids=sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    if(payloads)for(size_t i=0;i<payloads->count;++i){Word restored=notification_from_payload(payloads->items[i],1,0);struct NotificationValues *a=notification_values(local),*b=notification_values(restored);
        int equal=1;const unsigned compare[]={NOTICE_TITLE,NOTICE_BODY,NOTICE_INFO,NOTICE_CATEGORY};for(unsigned j=0;j<COUNT(compare);++j){Word first=a->properties[compare[j]],second=b->properties[compare[j]];if(first!=second && (!first || !second || !same_objects(first,second)))equal=0;}
        if(equal && a->properties[NOTICE_DATE] && fabs(date_seconds(a->properties[NOTICE_DATE])-date_seconds(b->properties[NOTICE_DATE]))>0.001)equal=0;
        if(equal)sequence_add((void *)ids,selector,map_get((void *)payloads->items[i],selector,copy_string("id")));}
    notification_host_operation("removePending",ids,NULL);
}
static void notification_show(Word payload,Word options) {
    JNIEnv *env=attached_environment("Notification presentation");Word error=0;jstring text=ui_java_json(env,payload,&error);if(!text)return;
    jclass type=(*env)->GetObjectClass(env,activity);jmethodID method=(*env)->GetMethodID(env,type,"showLocalNotification","(Ljava/lang/String;J)V");(*env)->CallVoidMethod(env,activity,method,text,(jlong)options);(*env)->DeleteLocalRef(env,type);(*env)->DeleteLocalRef(env,text);
    if(options&1){Word badge=map_get((void *)payload,"Notification badge",copy_string("badge"));if(badge)application_set_badge(application_object,"Notification badge",(long)numeric_integer((void *)badge));}
}
struct NotificationReply { unsigned references,answered;Word payload; };
struct NotificationReplyBlock { struct Block_layout layout;struct NotificationReply *reply; };
static void notification_reply_release(struct NotificationReply *reply) { if(__atomic_sub_fetch(&reply->references,1,__ATOMIC_ACQ_REL)==0){cf_release(reply->payload);free(reply);} }
static void notification_reply_copy(void *destination,const void *source) { struct NotificationReply *reply=((const struct NotificationReplyBlock *)source)->reply;((struct NotificationReplyBlock *)destination)->reply=reply;__atomic_add_fetch(&reply->references,1,__ATOMIC_RELAXED); }
static void notification_reply_dispose(const void *block) { notification_reply_release(((const struct NotificationReplyBlock *)block)->reply); }
static void notification_presentation_reply(struct NotificationReplyBlock *block,Word options) { if(!__atomic_exchange_n(&block->reply->answered,1,__ATOMIC_ACQ_REL))notification_show(block->reply->payload,options); }
static void notification_response_reply(struct NotificationReplyBlock *block) { __atomic_store_n(&block->reply->answered,1,__ATOMIC_RELEASE); }
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_deliverGuestNotification)(JNIEnv *env,jobject instance,jstring text,jboolean response) {
    if(!application_ready)return NULL;may_unwind=1;int failed=setjmp(boundary);struct NotificationReply *volatile reply=NULL;
    if(!failed){Word string=string_from_java(env,text,"Local notification"),data=encoded_string(string,"Local notification",4),error=0,payload=json_parse(0,"Local notification",(void *)data,0,&error);
        if(!payload)stop("Invalid notification payload","Android notification");Word legacy=map_get((void *)payload,"Local notification",copy_string("legacy"));
        if(legacy && numeric_bool((void *)legacy)){Word local=notification_from_payload(payload,1,0);Method callback=find_implementation(app_delegate,"application:didReceiveLocalNotification:");if(callback)callback(app_delegate,"application:didReceiveLocalNotification:",application_object,local,0,0);}
        else{Word notice=notification_from_payload(payload,0,1),delegate=user_notification_get_delegate(current_user_notification_center(),"delegate");const char *selector=response ? "userNotificationCenter:didReceiveNotificationResponse:withCompletionHandler:" : "userNotificationCenter:willPresentNotification:withCompletionHandler:";Method callback=delegate ? find_implementation(delegate,selector) : NULL;
            if(callback){reply=calloc(1,sizeof(*reply));if(!reply)stop("Allocation failed",selector);reply->references=1;reply->payload=cf_retain(payload);
                static struct { uintptr_t reserved,size;void (*copy)(void *,const void *);void (*dispose)(const void *); } descriptor={0,sizeof(struct NotificationReplyBlock),notification_reply_copy,notification_reply_dispose};
                struct NotificationReplyBlock completion={.layout={.isa=_NSConcreteStackBlock,.flags=BLOCK_HAS_COPY_DISPOSE,.invoke=(void (*)(void *,...))(response ? (void *)notification_response_reply : (void *)notification_presentation_reply),.descriptor=(void *)&descriptor},.reply=(void *)reply};Word argument=notice;
                if(response){struct HostObject *result=(void *)host_allocate(external_class("_OBJC_CLASS_$_UNNotificationResponse"));result->content[0]=notice;result->content[1]=copy_string("com.apple.UNNotificationDefaultActionIdentifier");argument=(Word)result;}
                callback(delegate,selector,current_user_notification_center(),argument,(Word)&completion,0);
            }
        }
    }
    if(reply)notification_reply_release((void *)reply);may_unwind=0;return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static Method ui_notification_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if(!receiver)return NULL;
    if(!strcmp(receiver,"_OBJC_CLASS_$_UILocalNotification") || !strcmp(receiver,"_OBJC_CLASS_$_UNNotificationContent") || !strcmp(receiver,"_OBJC_CLASS_$_UNMutableNotificationContent")){
        if(class_receiver)return NULL;
        if(!strcmp(selector,"init"))return (Method)notification_content_init;
        if(!strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:") || !strcmp(selector,"mutableCopy") || !strcmp(selector,"mutableCopyWithZone:"))return (Method)notification_content_copy;
        if(notification_property(object,selector,0))return (Method)notification_get_property;
        if(strcmp(receiver,"_OBJC_CLASS_$_UNNotificationContent") && notification_property(object,selector,1))return (Method)notification_set_property;
    }
    if(!strcmp(receiver,"_OBJC_CLASS_$_UNNotificationSound")){
        if(class_receiver && !strcmp(selector,"defaultSound"))return (Method)notification_sound_default;
        if(class_receiver && !strcmp(selector,"soundNamed:"))return (Method)notification_sound_named;
        if(!class_receiver && (!strcmp(selector,"copy") || !strcmp(selector,"copyWithZone:")))return (Method)identity;
    }
    if(!strcmp(receiver,"_OBJC_CLASS_$_UNNotificationRequest")){
        if(class_receiver && !strcmp(selector,"requestWithIdentifier:content:trigger:"))return (Method)notification_request_create;
        if(!class_receiver && (!strcmp(selector,"identifier") || !strcmp(selector,"content") || !strcmp(selector,"trigger")))return (Method)notification_record_property;
    }
    if(!strcmp(receiver,"_OBJC_CLASS_$_UNTimeIntervalNotificationTrigger")){
        if(class_receiver && !strcmp(selector,"triggerWithTimeInterval:repeats:"))return (Method)notification_trigger_create;
        if(!class_receiver && !strcmp(selector,"timeInterval"))return (Method)notification_trigger_interval;
        if(!class_receiver && !strcmp(selector,"repeats"))return (Method)notification_trigger_repeats;
        if(!class_receiver && !strcmp(selector,"nextTriggerDate"))return (Method)notification_trigger_next;
    }
    if(!class_receiver && ((!strcmp(receiver,"_OBJC_CLASS_$_UNNotification") && (!strcmp(selector,"request") || !strcmp(selector,"date"))) || (!strcmp(receiver,"_OBJC_CLASS_$_UNNotificationResponse") && (!strcmp(selector,"notification") || !strcmp(selector,"actionIdentifier")))))return (Method)notification_record_property;
    if(!strcmp(receiver,"_OBJC_CLASS_$_UNNotificationSettings") && !class_receiver){const char *names[]={"authorizationStatus","soundSetting","badgeSetting","alertSetting","notificationCenterSetting","lockScreenSetting","alertStyle"};for(unsigned i=0;i<COUNT(names);++i)if(!strcmp(selector,names[i]))return (Method)notification_settings_value;}
    if(!strcmp(receiver,"_OBJC_CLASS_$_UNUserNotificationCenter") && !class_receiver){
        if(!strcmp(selector,"requestAuthorizationWithOptions:completionHandler:"))return (Method)notification_authorize;
        if(!strcmp(selector,"getNotificationSettingsWithCompletionHandler:"))return (Method)notification_get_settings;
        if(!strcmp(selector,"addNotificationRequest:withCompletionHandler:"))return (Method)notification_add_request;
        if(!strcmp(selector,"getPendingNotificationRequestsWithCompletionHandler:") || !strcmp(selector,"getDeliveredNotificationsWithCompletionHandler:"))return (Method)notification_get_requests;
        if(!strcmp(selector,"removePendingNotificationRequestsWithIdentifiers:") || !strcmp(selector,"removeDeliveredNotificationsWithIdentifiers:"))return (Method)notification_remove_requests;
        if(!strcmp(selector,"removeAllPendingNotificationRequests") || !strcmp(selector,"removeAllDeliveredNotifications"))return (Method)notification_remove_all;
    }
    if(object==application_object){
        if(!strcmp(selector,"scheduleLocalNotification:"))return (Method)notification_schedule_local;
        if(!strcmp(selector,"cancelLocalNotification:"))return (Method)notification_cancel_local;
        if(!strcmp(selector,"scheduledLocalNotifications"))return (Method)notification_scheduled_local;
        if(!strcmp(selector,"enabledRemoteNotificationTypes"))return (Method)notification_enabled_remote_types;
    }
    return NULL;
}
static Method ui_system_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    Method notification=ui_notification_method(object,selector,receiver,class_receiver);if(notification)return notification;
    if (!strcmp(receiver,"_OBJC_CLASS_$_NSValue")) {
        if (class_receiver && !strcmp(selector,"valueWithCGRect:")) return (Method)value_rect;
        if (!class_receiver && !strcmp(selector,"CGRectValue")) return (Method)value_rect_get;
    }
    if (class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_UNUserNotificationCenter") && !strcmp(selector,"currentNotificationCenter"))
        return (Method)current_user_notification_center;
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIDevice") && !class_receiver) {
        if (!strcmp(selector,"orientation")) return (Method)device_orientation;
        if (!strcmp(selector,"userInterfaceIdiom")) return (Method)device_idiom;
        if (!strcmp(selector,"isMultitaskingSupported")) return (Method)device_multitasking;
        if (!strcmp(selector,"setBatteryMonitoringEnabled:")) return (Method)device_battery_monitor;
        if (!strcmp(selector,"isBatteryMonitoringEnabled")) return (Method)device_battery_monitored;
        if (!strcmp(selector,"batteryLevel")) return (Method)device_battery_level;
        if (!strcmp(selector,"batteryState")) return (Method)device_battery_state;
        if (!strcmp(selector,"beginGeneratingDeviceOrientationNotifications")) return (Method)orientation_begin;
        if (!strcmp(selector,"endGeneratingDeviceOrientationNotifications")) return (Method)orientation_end;
        if (!strcmp(selector,"isGeneratingDeviceOrientationNotifications")) return (Method)orientation_generating;
    }
    if (object==(Word)&process_info && !strcmp(selector,"isLowPowerModeEnabled")) return (Method)low_power_mode;
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIScreen")) {
        if (class_receiver && !strcmp(selector,"mainScreen")) return (Method)main_screen;
        if (!class_receiver) {
            if (!strcmp(selector,"bounds")) return (Method)screen_bounds;
            if (!strcmp(selector,"nativeBounds")) return (Method)screen_native_bounds;
            if (!strcmp(selector,"scale") || !strcmp(selector,"nativeScale")) return (Method)screen_scale;
        }
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIUserNotificationSettings")) {
        if (class_receiver && !strcmp(selector,"settingsForTypes:categories:")) return (Method)notification_settings;
        if (!class_receiver && !strcmp(selector,"types")) return (Method)notification_types;
        if (!class_receiver && !strcmp(selector,"categories")) return (Method)notification_categories;
    }
    if (object==application_object) {
        if (!strcmp(selector,"setDelegate:")) return (Method)application_set_delegate;
        if (!strcmp(selector,"isProtectedDataAvailable")) return (Method)application_protected_data;
        if (!strcmp(selector,"statusBarOrientation")) return (Method)interface_orientation;
        if (!strcmp(selector,"statusBarStyle")) return (Method)application_status_bar_style;
        if (!strcmp(selector,"setStatusBarStyle:")) return (Method)application_set_status_style_plain;
        if (!strcmp(selector,"setStatusBarStyle:animated:")) return (Method)application_set_status_style;
        if (!strcmp(selector,"isStatusBarHidden")) return (Method)application_status_bar_hidden;
        if (!strcmp(selector,"statusBarFrame")) return (Method)application_status_bar_frame;
        if (!strcmp(selector,"setStatusBarHidden:")) return (Method)application_set_status_hidden_plain;
        if (!strcmp(selector,"setStatusBarHidden:withAnimation:")) return (Method)application_set_status_hidden;
        if (!strcmp(selector,"setApplicationIconBadgeNumber:")) return (Method)application_set_badge;
        if (!strcmp(selector,"applicationIconBadgeNumber")) return (Method)application_get_badge;
        if (!strcmp(selector,"applicationState")) return (Method)application_state;
        if (!strcmp(selector,"registerForRemoteNotifications")) return (Method)register_remote_notifications;
        if (!strcmp(selector,"isRegisteredForRemoteNotifications")) return (Method)remote_notifications_registered;
        if (!strcmp(selector,"registerUserNotificationSettings:")) return (Method)register_user_notifications;
        if (!strcmp(selector,"currentUserNotificationSettings")) return (Method)get_user_notification_settings;
        if (!strcmp(selector,"cancelAllLocalNotifications")) return (Method)cancel_local_notifications;
        if (!strcmp(selector,"beginBackgroundTaskWithExpirationHandler:")) return (Method)begin_background_task;
        if (!strcmp(selector,"beginBackgroundTaskWithName:expirationHandler:")) return (Method)begin_named_background_task;
        if (!strcmp(selector,"endBackgroundTask:")) return (Method)end_background_task;
    }
    return NULL;
}
