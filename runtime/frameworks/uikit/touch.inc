 
#define I2A_TOUCH_LIMIT 32
struct I2ATouch {
    Word isa,view,window;
    struct GuestPoint location,previous;
    double timestamp,pressure;
    long phase,taps,type;
    int pointer,active,delivered,cancelled;
};
struct I2ATouchEvent { Word isa; double timestamp; struct Sequence all; };
static struct I2ATouch i2a_touches[I2A_TOUCH_LIMIT];
static Word i2a_changed_items[I2A_TOUCH_LIMIT],i2a_all_items[I2A_TOUCH_LIMIT];
static struct Sequence i2a_changed;
static Word i2a_cancelled_items[I2A_TOUCH_LIMIT],i2a_delivery_items[I2A_TOUCH_LIMIT];
static struct Sequence i2a_cancelled,i2a_delivery;
static struct I2ATouchEvent i2a_touch_event;
static uint64_t i2a_touch_deliveries[5];
static struct GuestPoint touch_in_view(struct GuestPoint point,Word view) {
    if(!view)return point;point=ui_unproject_point(ui_world_matrix(view,0),point);point.x+=view_state(view)->bounds.x;point.y+=view_state(view)->bounds.y;return point;
}
static struct GuestPoint touch_location(struct I2ATouch *touch,const char *selector,Word view) { return touch_in_view(touch->location,view ? view : touch->window); }
static struct GuestPoint touch_previous(struct I2ATouch *touch,const char *selector,Word view) { return touch_in_view(touch->previous,view ? view : touch->window); }
static unsigned char touch_point_inside(Word object,const char *selector,struct GuestPoint point,Word event) {
    struct GuestRect bounds=view_state(object)->bounds;return point.x>=bounds.x && point.y>=bounds.y && point.x<bounds.x+bounds.width && point.y<bounds.y+bounds.height;
}
static Word touch_hit_test(Word object,const char *selector,struct GuestPoint point,Word event) {
    struct ViewState *view=view_state(object);if(view->hidden || !view->interaction || view->alpha<0.01)return 0;
    Method inside=find_implementation(object,"pointInside:withEvent:");if(!inside || !((unsigned char (*)(Word,const char *,struct GuestPoint,Word))inside)(object,"pointInside:withEvent:",point,event))return 0;
    struct Sequence *children=(void *)view->children;for(size_t i=children->count;i>0;--i){Word child=children->items[i-1];struct GuestPoint local=view_convert_point(child,"convertPoint:fromView:",point,object);Method hit=find_implementation(child,"hitTest:withEvent:");if(hit){Word found=((Word (*)(Word,const char *,struct GuestPoint,Word))hit)(child,"hitTest:withEvent:",local,event);if(found)return found;}}
    return object;
}
struct GestureTarget { Word target;const char *action;struct GestureTarget *next; };
struct GestureState {
    Word object,view,delegate;struct GestureTarget *targets;
    unsigned mask,peak,state,enabled,cancels,minimum,maximum,taps,completed_taps,pan,tap,active;
    struct GuestPoint origin,location,translation,velocity;double start,last,tap_end;
    Method should_begin,should_receive,simultaneous;unsigned epoch,cached,generation;
    struct GestureState *next,*active_next;
};
static struct GestureState *gesture_states,*active_gestures;
static double gesture_slop=10,gesture_tap_interval=0.3,gesture_tap_duration=0.5;
static int gesture_configuration_ready;
static void gesture_configure(void) {
    if(gesture_configuration_ready)return;JNIEnv *env=attached_environment("Gesture configuration");jclass type=(*env)->GetObjectClass(env,activity);jmethodID method=(*env)->GetMethodID(env,type,"gestureConfiguration","()[D");jdoubleArray values=(*env)->CallObjectMethod(env,activity,method);
    if(values && (*env)->GetArrayLength(env,values)==3){jdouble settings[3];(*env)->GetDoubleArrayRegion(env,values,0,3,settings);gesture_slop=settings[0];gesture_tap_interval=settings[1];gesture_tap_duration=settings[2];}
    if(values)(*env)->DeleteLocalRef(env,values);(*env)->DeleteLocalRef(env,type);gesture_configuration_ready=1;
}
static struct GestureState *gesture_state(Word object) {
    for(struct GestureState *state=gesture_states;state;state=state->next)if(state->object==object)return state;
    struct GestureState *state=calloc(1,sizeof(*state));if(!state)stop("Allocation failed","UIGestureRecognizer");state->object=object;state->enabled=state->cancels=state->minimum=state->taps=1;
    state->pan=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIPanGestureRecognizer"));state->tap=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UITapGestureRecognizer"));state->maximum=state->pan ? I2A_TOUCH_LIMIT : 1;state->next=gesture_states;gesture_states=state;gesture_configure();return state;
}
static void gesture_add_target(Word object,const char *selector,Word target,const char *action) {
    if(!action)return;struct GestureState *state=gesture_state(object);for(struct GestureTarget *p=state->targets;p;p=p->next)if(p->target==target && !strcmp(p->action,action))return;
    struct GestureTarget *item=malloc(sizeof(*item));char *name=strdup(action);if(!item || !name){free(item);free(name);stop("Allocation failed",selector);}*item=(struct GestureTarget){target,name,state->targets};state->targets=item;
}
static void gesture_remove_target(Word object,const char *selector,Word target,const char *action) {
    struct GestureTarget **slot=&gesture_state(object)->targets;while(*slot){struct GestureTarget *item=*slot;if((!target || target==item->target) && (!action || !strcmp(action,item->action))){*slot=item->next;free((void *)item->action);free(item);}else slot=&item->next;}
}
static Word gesture_init(Word object,const char *selector,Word target,const char *action) { gesture_state(object);gesture_add_target(object,selector,target,action);return object; }
static Word gesture_init_plain(Word object) { gesture_state(object);return object; }
static void gesture_delegate_cache(struct GestureState *state) {
    if(state->cached && state->epoch==method_cache_epoch)return;state->should_begin=state->delegate ? find_implementation(state->delegate,"gestureRecognizerShouldBegin:") : NULL;
    state->should_receive=state->delegate ? find_implementation(state->delegate,"gestureRecognizer:shouldReceiveTouch:") : NULL;state->simultaneous=state->delegate ? find_implementation(state->delegate,"gestureRecognizer:shouldRecognizeSimultaneouslyWithGestureRecognizer:") : NULL;state->cached=1;state->epoch=method_cache_epoch;
}
static void gesture_notify(struct GestureState *state,unsigned value) {
    state->state=value;if(value==0 || value==5)return;
    if(state->cancels && value!=4)for(unsigned i=0;i<I2A_TOUCH_LIMIT;++i)if(state->mask&(1U<<i))i2a_touches[i].cancelled=1;
    size_t count=0;for(struct GestureTarget *item=state->targets;item;item=item->next)++count;struct GestureTarget *targets=count ? calloc(count,sizeof(*targets)) : NULL;if(count && !targets)stop("Allocation failed","Gesture callbacks");
    size_t index=0;for(struct GestureTarget *item=state->targets;item;item=item->next){targets[index].target=item->target;targets[index].action=strdup(item->action);if(!targets[index].action)stop("Allocation failed","Gesture callback selector");++index;}
    for(size_t i=0;i<count;++i){Word target=targets[i].target;const char *action=targets[i].action;
        if(!target)for(Word view=state->view;view;view=view_state(view)->parent){Word controller=view_state(view)->controller;if(controller && find_implementation(controller,action)){target=controller;break;}}
        Method method=target ? find_implementation(target,action) : NULL;if(target && !method)stop("Unimplemented gesture target",action);if(method)method(target,action,state->object,0,0,0);free((void *)action);}
    free(targets);
}
static int gesture_simultaneous(struct GestureState *first,struct GestureState *second) { gesture_delegate_cache(first);return first->simultaneous && (first->simultaneous(first->delegate,"gestureRecognizer:shouldRecognizeSimultaneouslyWithGestureRecognizer:",first->object,second->object,0,0)&255); }
static int gesture_may_begin(struct GestureState *state) {
    gesture_delegate_cache(state);if(state->should_begin && !(state->should_begin(state->delegate,"gestureRecognizerShouldBegin:",state->object,0,0,0)&255))return 0;
    for(struct GestureState *other=active_gestures;other;other=other->active_next)if(other!=state && (state->mask&other->mask) && other->state>=1 && other->state<=3 && !gesture_simultaneous(state,other) && !gesture_simultaneous(other,state))return 0;return 1;
}
static unsigned gesture_touch_count(struct GestureState *state) { return (unsigned)__builtin_popcount(state->mask); }
static struct GuestPoint gesture_centroid(struct GestureState *state) {
    struct GuestPoint point={0};unsigned count=gesture_touch_count(state);if(!count)return state->location;for(unsigned i=0;i<I2A_TOUCH_LIMIT;++i)if(state->mask&(1U<<i)){point.x+=i2a_touches[i].location.x;point.y+=i2a_touches[i].location.y;}point.x/=count;point.y/=count;return point;
}
static void gesture_track_touch(struct GestureState *state,unsigned index,double timestamp) {
    if(!state->enabled || (!state->pan && !state->tap) || (state->mask&(1U<<index)))return;gesture_delegate_cache(state);
    if(state->should_receive && !(state->should_receive(state->delegate,"gestureRecognizer:shouldReceiveTouch:",state->object,(Word)&i2a_touches[index],0,0)&255))return;
    if(!state->mask){if(state->state || timestamp-state->tap_end>gesture_tap_interval)state->completed_taps=0;++state->generation;state->state=state->peak=0;state->translation=state->velocity=(struct GuestPoint){0};state->start=state->last=timestamp;state->origin=state->location=i2a_touches[index].location;}
    struct GuestPoint before=gesture_centroid(state);state->mask|=1U<<index;struct GuestPoint after=gesture_centroid(state);state->origin.x+=after.x-before.x;state->origin.y+=after.y-before.y;state->location=after;
    unsigned count=gesture_touch_count(state);if(state->peak<count)state->peak=count;
    if(!state->active){state->active=1;state->active_next=active_gestures;active_gestures=state;}
}
struct GestureReset { struct GestureState *state;unsigned generation; };
static void gesture_reset(void *context) { struct GestureReset reset=*(struct GestureReset *)context;free(context);if(reset.state->generation==reset.generation && !reset.state->mask){reset.state->state=reset.state->completed_taps=0;reset.state->velocity=(struct GuestPoint){0};} }
static void gesture_process(long phase,double timestamp) {
    if(phase==0)for(size_t j=0;j<i2a_changed.count;++j){struct I2ATouch *touch=(void *)i2a_changed.items[j];unsigned index=(unsigned)(touch-i2a_touches);for(Word view=touch->view;view;view=view_state(view)->parent){struct Sequence *gestures=(void *)view_state(view)->gestures;if(gestures)for(size_t i=0;i<gestures->count;++i)gesture_track_touch(gesture_state(gestures->items[i]),index,timestamp);}}
    for(struct GestureState *state=active_gestures;state;state=state->active_next){if(!state->mask)continue;unsigned changed=0,ended=0;
        for(size_t i=0;i<i2a_changed.count;++i){unsigned bit=1U<<(unsigned)((struct I2ATouch *)i2a_changed.items[i]-i2a_touches);if(state->mask&bit){changed|=bit;if(phase==3 || phase==4)ended|=bit;}}
        if(!changed)continue;struct GuestPoint point=gesture_centroid(state);double elapsed=timestamp-state->last;if(elapsed>0){state->velocity.x=(point.x-state->location.x)/elapsed;state->velocity.y=(point.y-state->location.y)/elapsed;}state->last=timestamp;state->location=point;state->translation=(struct GuestPoint){point.x-state->origin.x,point.y-state->origin.y};
        double distance=hypot(state->translation.x,state->translation.y);unsigned count=gesture_touch_count(state);
        if(phase==4 || !state->enabled || count>state->maximum){gesture_notify(state,state->state==1 || state->state==2 ? 4 : 5);}
        else if(state->pan){if(state->state==0 && phase!=3 && count>=state->minimum && distance>=gesture_slop)gesture_notify(state,gesture_may_begin(state) ? 1 : 5);
            else if(state->state==1 || state->state==2){if(ended && count-(unsigned)__builtin_popcount(ended)<state->minimum)gesture_notify(state,3);else if(phase==1)gesture_notify(state,2);}}
        else if(state->state==0){if(distance>gesture_slop || timestamp-state->start>gesture_tap_duration)state->state=5;
            else if(ended==state->mask && state->peak==state->minimum){++state->completed_taps;state->tap_end=timestamp;if(state->completed_taps==state->taps)gesture_notify(state,gesture_may_begin(state) ? 3 : 5);}}
        if(ended){struct GuestPoint before=state->location;state->mask&=~ended;if(state->mask){struct GuestPoint after=gesture_centroid(state);state->origin.x+=after.x-before.x;state->origin.y+=after.y-before.y;state->location=after;}}
    }
    struct GestureState **slot=&active_gestures;while(*slot){struct GestureState *state=*slot;if(!state->mask){*slot=state->active_next;state->active=0;state->active_next=NULL;if(state->state){struct GestureReset *reset=malloc(sizeof(*reset));if(!reset)stop("Allocation failed","Gesture reset");*reset=(struct GestureReset){state,state->generation};schedule_main_function(gesture_reset,reset);}}else slot=&state->active_next;}
}
static void gesture_set_value(Word object,const char *selector,Word value) {
    struct GestureState *state=gesture_state(object);if(!strcmp(selector,"setDelegate:")){state->delegate=value;state->cached=0;}
    else if(!strcmp(selector,"setEnabled:")){state->enabled=value!=0;if(!state->enabled)gesture_notify(state,state->state==1 || state->state==2 ? 4 : 5);}
    else if(!strcmp(selector,"setCancelsTouchesInView:"))state->cancels=value!=0;
    else {if(!value || value>I2A_TOUCH_LIMIT)stop("Invalid gesture touch count",selector);
        if(!strcmp(selector,"setNumberOfTapsRequired:"))state->taps=(unsigned)value;else if(!strcmp(selector,"setNumberOfTouchesRequired:"))state->minimum=state->maximum=(unsigned)value;
        else if(!strcmp(selector,"setMinimumNumberOfTouches:"))state->minimum=(unsigned)value;else state->maximum=(unsigned)value;}
}
static Word gesture_get_value(Word object,const char *selector) {
    struct GestureState *state=gesture_state(object);if(!strcmp(selector,"delegate"))return state->delegate;if(!strcmp(selector,"view"))return state->view;if(!strcmp(selector,"state"))return state->state;
    if(!strcmp(selector,"isEnabled"))return state->enabled;if(!strcmp(selector,"cancelsTouchesInView"))return state->cancels;if(!strcmp(selector,"numberOfTouches"))return gesture_touch_count(state);
    if(!strcmp(selector,"numberOfTapsRequired"))return state->taps;if(!strcmp(selector,"numberOfTouchesRequired") || !strcmp(selector,"minimumNumberOfTouches"))return state->minimum;return state->maximum;
}
static struct GuestPoint gesture_location(Word object,const char *selector,Word view) { return touch_in_view(gesture_state(object)->location,view); }
static struct GuestPoint gesture_touch_location(Word object,const char *selector,Word index,Word view) { struct GestureState *state=gesture_state(object);for(unsigned i=0;i<I2A_TOUCH_LIMIT;++i)if(state->mask&(1U<<i)){if(!index--)return touch_in_view(i2a_touches[i].location,view);}stop("Gesture touch index out of range",selector);return (struct GuestPoint){0}; }
static struct GuestPoint gesture_vector(Word object,const char *selector,Word view) { struct GestureState *state=gesture_state(object);struct GuestPoint vector=!strcmp(selector,"velocityInView:") ? state->velocity : state->translation;if(!view)view=view_window(state->view);struct GuestPoint end=touch_in_view(state->location,view),start=touch_in_view((struct GuestPoint){state->location.x-vector.x,state->location.y-vector.y},view);return (struct GuestPoint){end.x-start.x,end.y-start.y}; }
static void gesture_set_translation(Word object,const char *selector,struct GuestPoint point,Word view) { struct GestureState *state=gesture_state(object);if(!view)view=view_window(state->view);if(view){struct GuestPoint end=touch_in_view(state->location,view),start={end.x-point.x,end.y-point.y};struct ViewState *target=view_state(view);start.x-=target->bounds.x;start.y-=target->bounds.y;start=ui_project_point(ui_world_matrix(view,0),start);point=(struct GuestPoint){state->location.x-start.x,state->location.y-start.y};}state->translation=point;state->origin=(struct GuestPoint){state->location.x-point.x,state->location.y-point.y}; }
static void view_remove_gesture(Word object,const char *selector,Word gesture) {
    struct GestureState *state=gesture_state(gesture);if(state->view!=object)return;struct Sequence *array=(void *)view_state(object)->gestures;if(array)for(size_t i=0;i<array->count;++i)if(array->items[i]==gesture){sequence_remove_at(array,selector,i);break;}state->view=0;if(state->mask)gesture_notify(state,state->state==1 || state->state==2 ? 4 : 5);state->mask=0;
}
static void view_add_gesture(Word object,const char *selector,Word gesture) {
    if(!gesture)return;struct GestureState *state=gesture_state(gesture);if(state->view==object)return;if(state->view)view_remove_gesture(state->view,selector,gesture);struct ViewState *view=view_state(object);if(!view->gestures)view->gestures=sequence_empty(external_class("_OBJC_CLASS_$_NSMutableArray"));sequence_add((void *)view->gestures,selector,gesture);state->view=object;
}
static Word view_gestures(Word object,const char *selector) { Word array=view_state(object)->gestures;return array ? new_array(external_class("_OBJC_CLASS_$_NSArray"),selector,((struct Sequence *)array)->items,((struct Sequence *)array)->count) : sequence_empty(external_class("_OBJC_CLASS_$_NSArray")); }
static Method gesture_method(Word object,const char *selector,int class_receiver) {
    if(class_receiver)return NULL;Word cls=read_word(object);if(class_inherits(cls,external_class("_OBJC_CLASS_$_UIView"))){if(!strcmp(selector,"addGestureRecognizer:"))return (Method)view_add_gesture;if(!strcmp(selector,"removeGestureRecognizer:"))return (Method)view_remove_gesture;if(!strcmp(selector,"gestureRecognizers"))return (Method)view_gestures;if(!strcmp(selector,"hitTest:withEvent:"))return (Method)touch_hit_test;if(!strcmp(selector,"pointInside:withEvent:"))return (Method)touch_point_inside;}
    if(!class_inherits(cls,external_class("_OBJC_CLASS_$_UIGestureRecognizer")))return NULL;
    if(!strcmp(selector,"initWithTarget:action:"))return (Method)gesture_init;if(!strcmp(selector,"addTarget:action:"))return (Method)gesture_add_target;if(!strcmp(selector,"removeTarget:action:"))return (Method)gesture_remove_target;
    if(!strcmp(selector,"init"))return (Method)gesture_init_plain;
    const char *setters[]={"setDelegate:","setEnabled:","setCancelsTouchesInView:"};for(unsigned i=0;i<COUNT(setters);++i)if(!strcmp(selector,setters[i]))return (Method)gesture_set_value;
    const char *getters[]={"delegate","view","state","isEnabled","cancelsTouchesInView","numberOfTouches"};for(unsigned i=0;i<COUNT(getters);++i)if(!strcmp(selector,getters[i]))return (Method)gesture_get_value;
    if(class_inherits(cls,external_class("_OBJC_CLASS_$_UITapGestureRecognizer"))){if(!strcmp(selector,"setNumberOfTapsRequired:") || !strcmp(selector,"setNumberOfTouchesRequired:"))return (Method)gesture_set_value;if(!strcmp(selector,"numberOfTapsRequired") || !strcmp(selector,"numberOfTouchesRequired"))return (Method)gesture_get_value;}
    if(!strcmp(selector,"locationInView:"))return (Method)gesture_location;if(!strcmp(selector,"locationOfTouch:inView:"))return (Method)gesture_touch_location;
    if(class_inherits(cls,external_class("_OBJC_CLASS_$_UIPanGestureRecognizer"))){if(!strcmp(selector,"translationInView:") || !strcmp(selector,"velocityInView:"))return (Method)gesture_vector;if(!strcmp(selector,"setTranslation:inView:"))return (Method)gesture_set_translation;if(!strcmp(selector,"setMinimumNumberOfTouches:") || !strcmp(selector,"setMaximumNumberOfTouches:"))return (Method)gesture_set_value;if(!strcmp(selector,"minimumNumberOfTouches") || !strcmp(selector,"maximumNumberOfTouches"))return (Method)gesture_get_value;}
    return NULL;
}
static double touch_timestamp(struct I2ATouch *touch) { return touch->timestamp; }
static double touch_force(struct I2ATouch *touch) { return touch->pressure; }
static double touch_max_force(void) { return 1; }  
static Word touch_view(struct I2ATouch *touch) { return touch->view; }
static Word touch_window(struct I2ATouch *touch) { return touch->window; }
static Word touch_phase(struct I2ATouch *touch) { return touch->phase; }
static Word touch_taps(struct I2ATouch *touch) { return touch->taps; }
static Word touch_type(struct I2ATouch *touch) { return touch->type; }
static double touch_event_time(struct I2ATouchEvent *event) { return event->timestamp; }
static Word touch_event_kind(void) { return 0; }
static Word touch_event_all(struct I2ATouchEvent *event) { return (Word)&event->all; }
static Word touch_event_filtered(struct I2ATouchEvent *event,const char *selector,Word object) {
    Word matches[I2A_TOUCH_LIMIT]; size_t count=0;
    for (size_t i=0;i<event->all.count;++i) {
        struct I2ATouch *touch=(void *)event->all.items[i];
        if ((!strcmp(selector,"touchesForView:") ? touch->view : touch->window)==object) matches[count++]=(Word)touch;
    }
    return new_array(external_class("_OBJC_CLASS_$_NSSet"),selector,matches,count);
}
static Method ui_touch_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    Method gesture=gesture_method(object,selector,class_receiver);if(gesture)return gesture;
    if (!strcmp(receiver,"_OBJC_CLASS_$_UITouch")) {
        if (!strcmp(selector,"locationInView:") || !strcmp(selector,"preciseLocationInView:")) return (Method)touch_location;
        if (!strcmp(selector,"previousLocationInView:") || !strcmp(selector,"precisePreviousLocationInView:")) return (Method)touch_previous;
        if (!strcmp(selector,"timestamp")) return (Method)touch_timestamp;
        if (!strcmp(selector,"force")) return (Method)touch_force;
        if (!strcmp(selector,"maximumPossibleForce")) return (Method)touch_max_force;
        if (!strcmp(selector,"view")) return (Method)touch_view;
        if (!strcmp(selector,"window")) return (Method)touch_window;
        if (!strcmp(selector,"phase")) return (Method)touch_phase;
        if (!strcmp(selector,"tapCount")) return (Method)touch_taps;
        if (!strcmp(selector,"type")) return (Method)touch_type;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIEvent")) {
        if (!strcmp(selector,"timestamp")) return (Method)touch_event_time;
        if (!strcmp(selector,"allTouches")) return (Method)touch_event_all;
        if (!strcmp(selector,"touchesForView:") || !strcmp(selector,"touchesForWindow:")) return (Method)touch_event_filtered;
        if (!strcmp(selector,"type") || !strcmp(selector,"subtype")) return (Method)touch_event_kind;
    }
    return NULL;
}
static void touch_deliver(long phase,double timestamp) {
    if (!i2a_changed.count) return;
    static const char *selectors[]={"touchesBegan:withEvent:","touchesMoved:withEvent:",NULL,"touchesEnded:withEvent:","touchesCancelled:withEvent:"};
    i2a_touch_event.isa=external_class("_OBJC_CLASS_$_UIEvent"); i2a_touch_event.timestamp=timestamp;
    i2a_touch_event.all.isa=i2a_changed.isa=external_class("_OBJC_CLASS_$_NSSet");
    i2a_touch_event.all.items=i2a_all_items; i2a_touch_event.all.capacity=I2A_TOUCH_LIMIT; i2a_touch_event.all.count=0;
    ++i2a_touch_event.all.mutation; ++i2a_changed.mutation;
    for (unsigned i=0;i<I2A_TOUCH_LIMIT;++i) if (i2a_touches[i].active) i2a_all_items[i2a_touch_event.all.count++]=(Word)&i2a_touches[i];
    gesture_process(phase,timestamp);
    size_t cancel_count=0,delivery_count=0;
    for(size_t i=0;i<i2a_changed.count;++i){struct I2ATouch *touch=(void *)i2a_changed.items[i];if(touch->cancelled){if(touch->delivered){touch->phase=4;touch->delivered=0;i2a_cancelled_items[cancel_count++]=(Word)touch;}}else{touch->delivered=phase==0 || (phase==1 && touch->delivered);i2a_delivery_items[delivery_count++]=(Word)touch;}}
    Method callback=find_implementation(root_controller,selectors[phase]);
    if (!callback) stop("Missing original touch callback",selectors[phase]);
    i2a_cancelled=i2a_delivery=i2a_changed;i2a_cancelled.items=i2a_cancelled_items;i2a_cancelled.count=cancel_count;i2a_delivery.items=i2a_delivery_items;i2a_delivery.count=delivery_count;
    if(cancel_count){Method cancel=find_implementation(root_controller,"touchesCancelled:withEvent:");if(cancel)cancel(root_controller,"touchesCancelled:withEvent:",(Word)&i2a_cancelled,(Word)&i2a_touch_event,0,0);}
    if(delivery_count)callback(root_controller,selectors[phase],(Word)&i2a_delivery,(Word)&i2a_touch_event,0,0);
    ++i2a_touch_deliveries[phase];
    if (render_diagnostics_requested && (phase!=1 || i2a_touch_deliveries[phase]%60==1))
        LOG("[DIAG-input] original %s changed=%lu active=%lu total=%llu",selectors[phase],i2a_changed.count,i2a_touch_event.all.count,(unsigned long long)i2a_touch_deliveries[phase]);
}
static void touch_cancel_all(double timestamp) {
    if (runtime_extension.cancel_touches) runtime_extension.cancel_touches();
    i2a_changed.items=i2a_changed_items; i2a_changed.capacity=I2A_TOUCH_LIMIT; i2a_changed.count=0;
    for (unsigned i=0;i<I2A_TOUCH_LIMIT;++i) if (i2a_touches[i].active) {
        i2a_touches[i].phase=4; i2a_touches[i].timestamp=timestamp;
        i2a_changed_items[i2a_changed.count++]=(Word)&i2a_touches[i];
    }
    touch_deliver(4,timestamp);
    for (unsigned i=0;i<I2A_TOUCH_LIMIT;++i) i2a_touches[i].active=0;
}
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_deliverTouches)(JNIEnv *env,jobject activity,jint action,jint changed,jlong millis,jintArray pointers,jfloatArray coordinates) {
    if (!application_ready) return NULL;
    may_unwind=1;
    int failed=setjmp(boundary);
    if (!failed) {
        double timestamp=millis*0.001;
        if (action==3) touch_cancel_all(timestamp);
        else {
            jsize count=(*env)->GetArrayLength(env,pointers);
            if (count<1 || count>I2A_TOUCH_LIMIT || (*env)->GetArrayLength(env,coordinates)!=count*4) stop("Invalid touch packet","Android MotionEvent");
            jint ids[I2A_TOUCH_LIMIT]; jfloat positions[I2A_TOUCH_LIMIT*4];
            (*env)->GetIntArrayRegion(env,pointers,0,count,ids); (*env)->GetFloatArrayRegion(env,coordinates,0,count*4,positions);
            if (action==0) touch_cancel_all(timestamp);
            Word view=controller_view(root_controller);
            if (!view || !view_state(view)->interaction || view_state(view)->hidden || view_state(root_controller)->presented) {
                if (runtime_extension.cancel_touches) runtime_extension.cancel_touches();
                goto touch_done;
            }
            double scale=screen_scale(); if (scale<=0) scale=1;
            i2a_changed.items=i2a_changed_items; i2a_changed.capacity=I2A_TOUCH_LIMIT; i2a_changed.count=0;
            long phase=action==0 || action==5 ? 0 : action==1 || action==6 ? 3 : 1;
            for (jsize i=0;i<count;++i) {
                int selected=phase==1 || ids[i]==changed;
                if (runtime_extension.filter_touch && runtime_extension.filter_touch(selected ? phase : 2,ids[i],timestamp,positions[i*4],positions[i*4+1])) continue;
                struct I2ATouch *touch=NULL;
                for (unsigned j=0;j<I2A_TOUCH_LIMIT;++j) if (i2a_touches[j].active && i2a_touches[j].pointer==ids[i]) { touch=&i2a_touches[j]; break; }
                if (!touch && selected && phase==0) {
                    unsigned active=0; for (unsigned j=0;j<I2A_TOUCH_LIMIT;++j) active+=i2a_touches[j].active;
                    if (active && !view_state(view)->multitouch) continue;
                    for (unsigned j=0;j<I2A_TOUCH_LIMIT;++j) if (!i2a_touches[j].active) { touch=&i2a_touches[j]; break; }
                    if (!touch) stop("Touch capacity exceeded","Android MotionEvent");
                    memset(touch,0,sizeof(*touch)); touch->isa=external_class("_OBJC_CLASS_$_UITouch");
                    touch->pointer=ids[i]; touch->active=1; touch->view=view; touch->window=view_window(view); touch->taps=1;
                    touch->location=(struct GuestPoint){positions[i*4]/scale,positions[i*4+1]/scale};
                    struct GuestPoint local=touch_in_view(touch->location,view);Method tester=find_implementation(view,"hitTest:withEvent:");Word hit=tester ? ((Word (*)(Word,const char *,struct GuestPoint,Word))tester)(view,"hitTest:withEvent:",local,(Word)&i2a_touch_event) : view;if(hit)touch->view=hit;
                }
                if (!touch) continue;
                touch->previous=touch->location;
                touch->location=(struct GuestPoint){positions[i*4]/scale,positions[i*4+1]/scale};
                touch->pressure=fmax(0,positions[i*4+2]); touch->timestamp=timestamp;
                touch->type=positions[i*4+3]==2 ? 2 : 0;
                touch->phase=selected ? phase : 2;
                if (selected) i2a_changed_items[i2a_changed.count++]=(Word)touch;
            }
            touch_deliver(phase,timestamp);
            if (phase==3) for (size_t i=0;i<i2a_changed.count;++i) ((struct I2ATouch *)i2a_changed_items[i])->active=0;
        }
    }
touch_done:
    may_unwind=0;
    return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
