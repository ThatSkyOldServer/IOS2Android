 
struct GuestPoint { double x,y; };
struct GuestSize { double width,height; };
struct GuestInsets { double top,left,bottom,right; };
#define VIEW_PRESENTATION_FIELDS 25
struct GuestAffine { double a,b,c,d,tx,ty; };
struct GuestTransform3D { double m[16]; };
static const struct GuestTransform3D transform3d_identity={{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
static void ui_view_composition_sync(Word);
static void ui_layer_sync_tree(Word);
static int ui_layer_has_content(Word);
static void layer_backing_remove(Word);
static void layer_backing_add(Word,Word);
static void layer_collect_order(Word,struct Sequence *);
static void view_presentation(Word,float [VIEW_PRESENTATION_FIELDS]);
static struct GuestPoint view_convert_point(Word,const char *,struct GuestPoint,Word);
static struct GuestRect view_transformed_frame(Word);
static struct GuestPoint view_anchor(Word);
static unsigned label_argb(Word,unsigned);
static void ui_label_sync(Word);
static void ui_web_sync(Word);
static void ui_input_sync(Word);
static void ui_table_sync(Word);
static void ui_table_scroll(Word,unsigned);
static void ui_table_controller_load(Word,Word);
static void ui_table_controller_appear(Word);
static uint32_t ui_cell_background(Word,uint32_t);
static void ui_scroll_input_options(Word,float *,int *);
static void ui_scroll_input_delegate(Word,Word);
static Word ui_alert_initialize(Word,const char *);
static void ui_view_order_sync(Word);
static void view_request_layout(Word,const char *);
static void view_layout_if_needed(Word,const char *);
static void controller_geometry(Word);
static struct GuestSize ui_view_intrinsic_size(Word);
static void view_set_frame(Word,const char *,struct GuestRect);
static int ui_view_rendered(Word);
static void ui_view_load_content(Word,const char *,const double *);
static void ui_view_request_display(Word,const char *);
static void ui_view_load_label(Word,plist_t);
static Word storyboard_startup(Word);
static void load_storyboard_view(Word,Word);
static void window_schedule_presentation(Word);
static void window_set_hidden(Word,const char *,unsigned char);
static int window_lifecycle_ready;
struct ViewOpacity { Word object; float before,presentation[VIEW_PRESENTATION_FIELDS]; struct ViewOpacity *next; };
struct ViewAnimation { struct ViewOpacity *items,*sources; struct ViewAnimation *previous; double duration,delay,damping,velocity,repeat; Word options,identifier,delegate,context; const char *started,*stopped; unsigned legacy; };
static _Thread_local struct ViewAnimation *view_animation;
static _Atomic unsigned view_animations_enabled=1;
static void view_animation_capture(Word);
static void view_animation_change(Word);
struct ViewState {
    Word object,parent,children,layer,controller,view,owner_window,image_name,background,presented,presenting,modal_alert,popover,navigation_bar,storyboard,nib_name,nib_bundle;
    Word navigation_item,title,prompt,left_item,right_item,back_item,archived_graph;
    void *control,*web,*scroll,*input,*table,*cell;
    void *composition;
    Word host_container;
    Word accessibility_label;
    Word constraints,gestures;
    struct GuestRect margins;
    struct GuestRect frame,bounds;
    double alpha,scale;
    double corner,z,window_level;
    long tag,content_mode,autoresizing,modal_style,transition_style,bar_style;
    int hidden,interaction,multitouch,clips,presenting_transition,dismissing_transition,modal_in_popover,deferred_storyboard,loading_view,view_did_load,autoresizes,exclusive_touch,accessibility_element;
    unsigned translates,layout_managed,layout_pending,layout_running,layout_notify,needs_layout,needs_constraints;
    unsigned edges,automatic_insets,defines_context,opaque_bars,clears_selection,moving_to,moving_from,opaque;
    unsigned needs_display,display_pending,display_running,display_checked;
    Method display_method;
    Method intrinsic_method;unsigned intrinsic_epoch,intrinsic_checked;
    unsigned display_epoch;
    unsigned appearance_pending,appeared;
    unsigned composition_synced;
    Word composition_owner;
    float presentation[VIEW_PRESENTATION_FIELDS];
    float *composition_clips;Word *composition_clip_owners;unsigned composition_clip_count;
    struct ViewState *next;
};
static struct ViewState *view_states;
static struct ViewState *existing_view_state(Word object){for(struct ViewState *state=view_states;state;state=state->next)if(state->object==object)return state;return NULL;}
static struct ViewState *view_state(Word object) {
    struct ViewState *known=existing_view_state(object);if(known)return known;
    struct ViewState *state=calloc(1,sizeof(*state));
    if (!state) stop("Allocation failed","UIView state");
    state->object=object; state->alpha=1; state->scale=screen_scale(); state->interaction=1; state->autoresizes=1; state->translates=1;
    state->hidden=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIWindow"));
    state->margins=(struct GuestRect){8,8,8,8};
    state->edges=15; state->automatic_insets=1; state->clears_selection=1; state->opaque=!class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UILabel"));
    state->children=sequence_empty(external_class("_OBJC_CLASS_$_NSMutableArray"));
    state->next=view_states; view_states=state;
    return state;
}
static Word application_windows(void) {
    struct Sequence *windows=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    for(struct ViewState *state=view_states;state;state=state->next)if(class_inherits(read_word(state->object),external_class("_OBJC_CLASS_$_UIWindow")))sequence_add(windows,"windows",state->object);
    return (Word)windows;
}
static void view_sync_subtree(Word object) {
    struct ViewState *state=view_state(object);
    ui_label_sync(object);
    ui_web_sync(object);
    ui_input_sync(object);
    ui_table_sync(object);
    ui_view_composition_sync(object);
    if(state->layer)ui_layer_sync_tree(state->layer);
    struct Sequence *children=(void *)state->children;
    for (size_t i=0;i<children->count;++i) view_sync_subtree(children->items[i]);
}
static Word view_init_frame(Word object,const char *selector,struct GuestRect frame) {
    struct ViewState *state=view_state(object);
    state->frame=frame; state->bounds=(struct GuestRect){0,0,frame.width,frame.height}; state->needs_display=1;
    return object;
}
static Word view_initialize(Word object,const char *selector) {
    Method initialize=find_implementation(object,"initWithFrame:");
    if (!initialize) stop("Missing UIView initializer","initWithFrame:");
    return ((Word (*)(Word,const char *,struct GuestRect))initialize)(object,"initWithFrame:",(struct GuestRect){0});
}
static struct GuestRect view_frame(Word object) { return view_transformed_frame(object); }
static struct GuestRect view_bounds(Word object) { return view_state(object)->bounds; }
static struct GuestSize view_size_that_fits(Word object,const char *selector,struct GuestSize size) { struct GuestRect bounds=view_state(object)->bounds;return (struct GuestSize){bounds.width,bounds.height}; }
static void view_size_to_fit(Word object,const char *selector) {
    struct ViewState *state=view_state(object);Method measure=find_implementation(object,"sizeThatFits:");
    struct GuestSize size=measure ? ((struct GuestSize (*)(Word,const char *,struct GuestSize))measure)(object,"sizeThatFits:",(struct GuestSize){state->bounds.width,state->bounds.height}) : (struct GuestSize){state->bounds.width,state->bounds.height};
    struct GuestRect frame=state->frame;frame.width=size.width;frame.height=size.height;view_set_frame(object,selector,frame);
}
static void view_invalidate_intrinsic(Word object,const char *selector) { struct ViewState *state=view_state(object);view_request_layout(state->parent ? state->parent : object,selector); }
static struct GuestSize view_intrinsic_for_layout(Word object) {
    struct ViewState *state=view_state(object);unsigned epoch=__atomic_load_n(&method_cache_epoch,__ATOMIC_ACQUIRE);
    if(!state->intrinsic_checked || state->intrinsic_epoch!=epoch){state->intrinsic_method=find_implementation(object,"intrinsicContentSize");state->intrinsic_checked=1;state->intrinsic_epoch=epoch;}
    if(state->intrinsic_method && state->intrinsic_method!=(Method)ui_view_intrinsic_size)return ((struct GuestSize (*)(Word,const char *))state->intrinsic_method)(object,"intrinsicContentSize");
    return ui_view_intrinsic_size(object);
}
static void view_draw_rect(Word object,const char *selector,struct GuestRect rect) {}
static void view_background(Word object,const char *selector,Word color) { view_state(object)->background=color; view_sync_subtree(object); }
static Word view_get_background(Word object) { return view_state(object)->background; }
static double view_window_level(Word object) { for (Word current=object;current;current=view_state(current)->parent) if (class_inherits(read_word(current),external_class("_OBJC_CLASS_$_UIWindow"))) return view_state(current)->window_level; return 0; }
static void window_set_level(Word object,const char *selector,double value) { if (!isfinite(value)) stop("Invalid window level",selector); view_state(object)->window_level=value; view_sync_subtree(object); }
static double window_get_level(Word object) { return view_state(object)->window_level; }
static void view_set_opaque(Word object,const char *selector,Word value) { view_state(object)->opaque=value!=0; }
static Word view_get_opaque(Word object) { return view_state(object)->opaque; }
static void view_resize_axis(double *origin,double *length,double parent,double delta,unsigned mask) {
    if (!mask || !delta) return;
    double segments[]={*origin,*length,parent-*origin-*length},total=0; unsigned flexible=0;
    for (unsigned i=0;i<3;++i) if (mask&(1U<<i)) { total+=fmax(0,segments[i]); ++flexible; }
    double leading=(mask&1) ? delta*(total>0 ? fmax(0,segments[0])/total : 1.0/flexible) : 0;
    double extent=(mask&2) ? delta*(total>0 ? fmax(0,segments[1])/total : 1.0/flexible) : 0;
    *origin+=leading; *length=fmax(0,*length+extent);
}
static void view_resize_children(struct ViewState *state,double width,double height) {
    double dx=state->bounds.width-width,dy=state->bounds.height-height;
    if (!state->autoresizes || (!dx && !dy)) return;
    struct Sequence *children=(void *)state->children;
    for (size_t i=0;i<children->count;++i) {
        struct ViewState *child=view_state(children->items[i]);
        if (!child->autoresizing || !child->translates) continue;
        double old_width=child->bounds.width,old_height=child->bounds.height;
        view_resize_axis(&child->frame.x,&child->frame.width,width,dx,(unsigned)child->autoresizing&7);
        view_resize_axis(&child->frame.y,&child->frame.height,height,dy,((unsigned)child->autoresizing>>3)&7);
        child->bounds.width=child->frame.width; child->bounds.height=child->frame.height;
        view_resize_children(child,old_width,old_height);
    }
}
static void view_set_frame(Word object,const char *selector,struct GuestRect frame) {
    struct ViewState *state=view_state(object); double width=state->bounds.width,height=state->bounds.height;
    if (!memcmp(&state->frame,&frame,sizeof(frame))) return;
    if(view_animation)view_animation_change(object);
    state->frame=frame; state->bounds.width=frame.width; state->bounds.height=frame.height;
    state->needs_display|=state->content_mode==3;
    view_resize_children(state,width,height); view_sync_subtree(object);
    view_request_layout(object,selector);
}
static void view_set_bounds(Word object,const char *selector,struct GuestRect bounds) {
    struct ViewState *state=view_state(object);
    if (!memcmp(&state->bounds,&bounds,sizeof(bounds))) return;
    if(view_animation)view_animation_change(object);
    double width=state->bounds.width,height=state->bounds.height;
    struct GuestPoint anchor=view_anchor(object);state->frame.x+=(state->bounds.width-bounds.width)*anchor.x;
    state->frame.y+=(state->bounds.height-bounds.height)*anchor.y;
    state->frame.width=bounds.width; state->frame.height=bounds.height; state->bounds=bounds;
    view_resize_children(state,width,height); view_sync_subtree(object);
    view_request_layout(object,selector);
}
static struct GuestPoint view_center(Word object) {
    struct ViewState *view=view_state(object);struct GuestPoint anchor=view_anchor(object);
    return (struct GuestPoint){view->frame.x+view->bounds.width*anchor.x,view->frame.y+view->bounds.height*anchor.y};
}
static void view_set_center(Word object,const char *selector,struct GuestPoint center) {
    struct ViewState *state=view_state(object);
    if(view_animation)view_animation_change(object);
    struct GuestPoint anchor=view_anchor(object);state->frame.x=center.x-state->bounds.width*anchor.x; state->frame.y=center.y-state->bounds.height*anchor.y;
    view_sync_subtree(object);
}
static void view_hidden(Word object,const char *selector,unsigned char hidden) {if(class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UIWindow"))){window_set_hidden(object,selector,hidden);return;}view_state(object)->hidden=hidden!=0; view_sync_subtree(object);}
static Word view_is_hidden(Word object) { return view_state(object)->hidden; }
static void view_alpha(Word object,const char *selector,double alpha) { if (view_animation) view_animation_change(object); view_state(object)->alpha=fmin(1,fmax(0,alpha)); view_sync_subtree(object); }
static double view_get_alpha(Word object) { return view_state(object)->alpha; }
static void view_scale(Word object,const char *selector,double scale) { if (scale>0) view_state(object)->scale=scale; }
static double view_get_scale(Word object) { return view_state(object)->scale; }
static void view_interaction(Word object,const char *selector,unsigned char enabled) { view_state(object)->interaction=enabled!=0; view_sync_subtree(object); }
static Word view_get_interaction(Word object) { return view_state(object)->interaction; }
static void view_multitouch(Word object,const char *selector,unsigned char enabled) { view_state(object)->multitouch=enabled!=0; }
static Word view_get_multitouch(Word object) { return view_state(object)->multitouch; }
static void view_exclusive_touch(Word object,const char *selector,Word enabled) { view_state(object)->exclusive_touch=enabled!=0; }
static Word view_get_exclusive_touch(Word object) { return view_state(object)->exclusive_touch; }
static void view_accessibility(Word object,const char *selector,Word value) {
    struct ViewState *state=view_state(object);
    if (!strcmp(selector,"setIsAccessibilityElement:")) state->accessibility_element=value!=0;
    else state->accessibility_label=value ? text_copy(value,"copy") : 0;
    view_sync_subtree(object);
}
static Word view_get_accessibility(Word object,const char *selector) {
    return !strcmp(selector,"isAccessibilityElement") ? view_state(object)->accessibility_element : view_state(object)->accessibility_label;
}
static void view_tag(Word object,const char *selector,long tag) { view_state(object)->tag=tag; }
static Word view_get_tag(Word object) { return view_state(object)->tag; }
static void view_content_mode(Word object,const char *selector,long mode) { view_state(object)->content_mode=mode; }
static Word view_get_content_mode(Word object) { return view_state(object)->content_mode; }
static void view_set_autoresizing(Word object,const char *selector,Word mask) { view_state(object)->autoresizing=(long)mask; }
static Word view_autoresizing(Word object) { return (Word)view_state(object)->autoresizing; }
static void view_set_autoresizes(Word object,const char *selector,unsigned char enabled) { view_state(object)->autoresizes=enabled!=0; }
static Word view_autoresizes(Word object) { return view_state(object)->autoresizes; }
static Word view_find_tag(Word object,const char *selector,long tag) {
    struct ViewState *state=view_state(object);
    if (state->tag==tag) return object;
    struct Sequence *children=(void *)state->children;
    for (size_t i=0;i<children->count;++i) { Word found=view_find_tag(children->items[i],selector,tag); if (found) return found; }
    return 0;
}
static void view_remove(Word object) {
    struct ViewState *state=view_state(object);
    if (state->parent) sequence_remove((void *)view_state(state->parent)->children,"removeFromSuperview",object);
    layer_backing_remove(object);
    state->parent=0;
    view_sync_subtree(object);
}
static void view_add(Word object,const char *selector,Word child) {
    if (!child) stop("Nil subview",selector);
    for (Word ancestor=object;ancestor;ancestor=view_state(ancestor)->parent) if (ancestor==child) stop("Cyclic UIView tree",selector);
    view_remove(child);
    view_state(child)->parent=object;
    sequence_add((void *)view_state(object)->children,selector,child);
    layer_backing_add(object,child);
    view_sync_subtree(child);
    ui_view_order_sync(object);
}
static Word view_superview(Word object) { return view_state(object)->parent; }
static Word view_subviews(Word object) { return sequence_copy((void *)view_state(object)->children,"copy"); }
static void view_move_child(Word object,const char *selector,size_t from,size_t to) {
    struct Sequence *children=(void *)view_state(object)->children;
    if (from>=children->count || to>=children->count) stop("Subview index out of range",selector);
    if (from==to) return;
    Word child=children->items[from];
    if (from<to) memmove(children->items+from,children->items+from+1,(to-from)*sizeof(Word));
    else memmove(children->items+to+1,children->items+to,(from-to)*sizeof(Word));
    children->items[to]=child; ++children->mutation; ui_view_order_sync(object);
}
static void view_insert_index(Word object,const char *selector,Word child,Word index) {
    struct Sequence *children=(void *)view_state(object)->children;
    if (index>children->count) stop("Subview index out of range",selector);
    if (child && view_state(child)->parent==object && index<children->count && children->items[index]==child) return;
    view_add(object,selector,child);
    if (index>=children->count) index=children->count-1;
    view_move_child(object,selector,children->count-1,index);
}
static void view_insert_relative(Word object,const char *selector,Word child,Word sibling) {
    if (!sibling || view_state(sibling)->parent!=object) stop("Relative view is not a subview",selector);
    if (child==sibling) return;
    view_add(object,selector,child); struct Sequence *children=(void *)view_state(object)->children;
    for (size_t i=0;i<children->count;++i) if (children->items[i]==sibling) {
        view_move_child(object,selector,children->count-1,i+(strstr(selector,"aboveSubview:")!=NULL)); return;
    }
}
static void view_reorder(Word object,const char *selector,Word child) {
    struct Sequence *children=(void *)view_state(object)->children;
    for (size_t i=0;i<children->count;++i) if (children->items[i]==child) {
        view_move_child(object,selector,i,!strcmp(selector,"bringSubviewToFront:") ? children->count-1 : 0); return;
    }
}
static void view_exchange(Word object,const char *selector,Word first,Word second) {
    struct Sequence *children=(void *)view_state(object)->children;
    if (first>=children->count || second>=children->count) stop("Subview index out of range",selector);
    if (first==second) return;
    Word temporary=children->items[first]; children->items[first]=children->items[second]; children->items[second]=temporary;
    ++children->mutation; ui_view_order_sync(object);
}
static Word default_layer_class(void) { return external_class("_OBJC_CLASS_$_CALayer"); }
static Word view_layer(Word object) {
    struct ViewState *state=view_state(object);
    if (!state->layer) {
        Word cls=read_word(object);
        Method layer_class=find_implementation(cls,"layerClass");
        Word layer=layer_class ? layer_class(cls,"layerClass",0,0,0,0) : default_layer_class();
        state->layer=host_allocate(layer);
        struct ViewState *layer_state=view_state(state->layer);
        layer_state->view=object;
        layer_state->clips=state->clips;
        layer_state->frame=state->bounds; layer_state->bounds=state->bounds; layer_state->scale=state->scale;
    }
    return state->layer;
}
static void controller_load_view(Word object,const char *selector) {
    struct ViewState *state=view_state(object);
    if (state->archived_graph) { load_storyboard_view(object,state->archived_graph); return; }
    if (state->deferred_storyboard || state->nib_name) stop("Archived controller view decoding unavailable",selector);
    Word cls=read_word(object),view_cls=external_class(class_inherits(cls,external_class("_OBJC_CLASS_$_UITableViewController")) ? "_OBJC_CLASS_$_UITableView" : "_OBJC_CLASS_$_UIView");
    state->view=view_init_frame(host_allocate(view_cls),selector,screen_bounds());
    ui_table_controller_load(object,state->view);
}
static Word controller_view(Word object) {
    struct ViewState *state=view_state(object);
    if (!state->view && !state->loading_view) {
        state->loading_view=1;
        Method load=find_implementation(object,"loadView");
        if (load) load(object,"loadView",0,0,0,0);
        else controller_load_view(object,"loadView");
        state->loading_view=0;
        if (!state->view) stop("Controller loadView did not set a view",class_name(read_word(object)));
        view_state(state->view)->controller=object;
    }
    if (state->view) {
        view_state(state->view)->controller=object;
        if(!state->view_did_load && !state->loading_view){state->view_did_load=1;Method loaded=find_implementation(object,"viewDidLoad");if(loaded)loaded(object,"viewDidLoad",0,0,0,0);}
    }
    return state->view;
}
static Word controller_loaded_view(Word object) { return view_state(object)->view; }
static void controller_load_if_needed(Word object,const char *selector) { controller_view(object); }
static Word controller_init_nib(Word object,const char *selector,Word name,Word bundle) {
    struct ViewState *state=view_state(object);
    state->nib_name=name ? text_copy(name,"copy") : 0; state->nib_bundle=bundle;
    return object;
}
static Word controller_initialize(Word object,const char *selector) {
    const char *initializer=class_inherits(read_word(object),external_class("_OBJC_CLASS_$_UITableViewController")) ? "initWithStyle:" : "initWithNibName:bundle:";
    Method initialize=find_implementation(object,initializer);
    if (!initialize) stop("Missing controller initializer",initializer);
    return initialize(object,initializer,0,0,0,0);
}
static Word controller_init_style(Word object,const char *selector,long style) { view_state(object)->bar_style=style; return object; }
static Word controller_table_view(Word object) { return controller_view(object); }
static void view_strong_property(Word *slot,Word value) {
    Word previous=*slot; *slot=metal_object_retain(value); metal_object_release(previous);
}
static Word navigation_item_title(Word object) { return view_state(object)->title; }
static void navigation_item_set_title(Word object,const char *selector,Word title) {
    struct ViewState *state=view_state(object); view_strong_property(&state->title,title ? text_copy(title,"copy") : 0);
}
static Word controller_navigation_item(Word object) {
    struct ViewState *state=view_state(object);
    if (!state->navigation_item) {
        state->navigation_item=host_allocate(external_class("_OBJC_CLASS_$_UINavigationItem"));
        navigation_item_set_title(state->navigation_item,"setTitle:",state->title);
    }
    return state->navigation_item;
}
static void controller_set_title(Word object,const char *selector,Word title) {
    navigation_item_set_title(object,selector,title);
    struct ViewState *state=view_state(object);
    if (state->navigation_item) navigation_item_set_title(state->navigation_item,selector,title);
}
static Word navigation_item_init(Word object,const char *selector,Word title) { navigation_item_set_title(object,selector,title); return object; }
static Word navigation_item_value(Word object,const char *selector) {
    struct ViewState *state=view_state(object);
    if (!strcmp(selector,"leftBarButtonItem")) return state->left_item;
    if (!strcmp(selector,"rightBarButtonItem")) return state->right_item;
    if (!strcmp(selector,"backBarButtonItem")) return state->back_item;
    if (!strcmp(selector,"titleView")) return state->view;
    return state->prompt;
}
static void navigation_item_set_value(Word object,const char *selector,Word value) {
    struct ViewState *state=view_state(object);
    Word *slot=!strncmp(selector,"setLeftBarButtonItem:",21) ? &state->left_item :
        !strncmp(selector,"setRightBarButtonItem:",22) ? &state->right_item :
        !strcmp(selector,"setBackBarButtonItem:") ? &state->back_item : !strcmp(selector,"setTitleView:") ? &state->view : &state->prompt;
    if (slot==&state->prompt) value=value ? text_copy(value,"copy") : 0;
    view_strong_property(slot,value);
}
static void controller_set_view(Word object,const char *selector,Word view) {
    struct ViewState *state=view_state(object);
    if (state->view==view) return;
    Word parent=state->view ? view_state(state->view)->parent : 0;
     
    if (!parent) parent=state->owner_window;
    if (state->view) { view_state(state->view)->controller=0; view_remove(state->view); }
    state->view=view;
    if (view) view_state(view)->controller=object;
    if (!view) state->view_did_load=0;
    if (parent && view) view_add(parent,selector,view);
}
static Word controller_loaded(Word object) { return view_state(object)->view!=0; }
static void controller_geometry(Word object) {
    struct ViewState *controller=view_state(object); if (!controller->view || !controller->owner_window) return;
    struct ViewState *window=view_state(controller->owner_window); struct GuestRect frame={0,0,window->bounds.width,window->bounds.height};
    JNIEnv *env=attached_environment("UIViewController extended layout"); static jmethodID insets_method;
    if (!insets_method) { jclass cls=(*env)->GetObjectClass(env,activity); insets_method=(*env)->GetMethodID(env,cls,"guestSafeInsets","()[I"); (*env)->DeleteLocalRef(env,cls); }
    jintArray insets=(*env)->CallObjectMethod(env,activity,insets_method); jint values[4]={0};
    if (insets && (*env)->GetArrayLength(env,insets)==4) (*env)->GetIntArrayRegion(env,insets,0,4,values);
    if (insets) (*env)->DeleteLocalRef(env,insets);
    double scale=screen_scale();
    double top=(controller->edges&1) ? 0 : values[0]/scale,left=(controller->edges&2) ? 0 : values[1]/scale;
    double bottom=(controller->edges&4) ? 0 : values[2]/scale,right=(controller->edges&8) ? 0 : values[3]/scale;
    frame.x=left; frame.y=top; frame.width=fmax(0,frame.width-left-right); frame.height=fmax(0,frame.height-top-bottom);
    view_set_frame(controller->view,"UIViewController extended layout",frame);
}
static Word controller_layout_property(Word object,const char *selector) {
    struct ViewState *state=view_state(object);
    if (!strcmp(selector,"edgesForExtendedLayout")) return state->edges;
    if (!strcmp(selector,"automaticallyAdjustsScrollViewInsets")) return state->automatic_insets;
    if (!strcmp(selector,"definesPresentationContext")) return state->defines_context;
    if (!strcmp(selector,"extendedLayoutIncludesOpaqueBars")) return state->opaque_bars;
    if (!strcmp(selector,"clearsSelectionOnViewWillAppear")) return state->clears_selection;
    return !strcmp(selector,"isMovingToParentViewController") ? state->moving_to : state->moving_from;
}
static void controller_set_layout_property(Word object,const char *selector,Word value) {
    struct ViewState *state=view_state(object);
    if (!strcmp(selector,"setEdgesForExtendedLayout:")) { if (value&~15UL) stop("Invalid extended layout edges",selector); state->edges=value; }
    else if (!strcmp(selector,"setAutomaticallyAdjustsScrollViewInsets:")) state->automatic_insets=value!=0;
    else if (!strcmp(selector,"setDefinesPresentationContext:")) state->defines_context=value!=0;
    else if (!strcmp(selector,"setExtendedLayoutIncludesOpaqueBars:")) state->opaque_bars=value!=0;
    else state->clears_selection=value!=0;
    if (!strcmp(selector,"setEdgesForExtendedLayout:") || !strcmp(selector,"setExtendedLayoutIncludesOpaqueBars:")) controller_geometry(object);
}
static Word controller_split_view(Word object) {
    for (Word parent=object;parent;parent=view_state(parent)->parent) if (class_inherits(read_word(parent),external_class("_OBJC_CLASS_$_UISplitViewController"))) return parent;
    return 0;
}
static void controller_move(Word object,const char *selector,Word parent) {
    struct ViewState *state=view_state(object);
    int moving=!strcmp(selector,"willMoveToParentViewController:"); state->moving_to=moving && parent; state->moving_from=moving && !parent;
}
static void controller_add_child(Word object,const char *selector,Word child) {
    if (!child || !class_inherits(read_word(child),external_class("_OBJC_CLASS_$_UIViewController"))) stop("Invalid child controller",selector);
    struct ViewState *state=view_state(child); state->moving_to=1;
    Method will=find_implementation(child,"willMoveToParentViewController:"); if (will) will(child,"willMoveToParentViewController:",object,0,0,0);
    view_add(object,selector,child);
}
static void controller_remove_parent(Word object,const char *selector) {
    if (!view_state(object)->parent) return;
    struct ViewState *state=view_state(object); state->moving_from=1;
    Method will=find_implementation(object,"willMoveToParentViewController:"); if (will) will(object,"willMoveToParentViewController:",0,0,0,0);
    view_remove(object);
    Method did=find_implementation(object,"didMoveToParentViewController:"); if (did) did(object,"didMoveToParentViewController:",0,0,0,0);
    state->moving_to=state->moving_from=0;
}
 
static Word controller_navigation(Word object) {
    object=view_state(object)->parent;
    for (unsigned depth=0;object && depth<1024;++depth) {
        const char *base=external_base(read_word(object));
        if (base && (!strcmp(base,"_OBJC_CLASS_$_UINavigationController") || !strcmp(base,"_OBJC_CLASS_$_UIImagePickerController"))) return object;
        object=view_state(object)->parent;
    }
    return 0;
}
static Word controller_navigation_bar(Word object) {
    struct ViewState *state=view_state(object);
    if (!state->navigation_bar) state->navigation_bar=host_allocate(external_class("_OBJC_CLASS_$_UINavigationBar"));
    return state->navigation_bar;
}
static Word navigation_bar_style(Word object) { return view_state(object)->bar_style; }
static void navigation_bar_set_style(Word object,const char *selector,long style) { view_state(object)->bar_style=style; }
static Word controller_nib_name(Word object) { return view_state(object)->nib_name; }
static Word controller_nib_bundle(Word object) { return view_state(object)->nib_bundle; }
static Word navigation_top(Word object) {
    struct Sequence *children=(void *)view_state(object)->children;
    return children->count ? children->items[children->count-1] : 0;
}
static Word navigation_visible(Word object) {
    Word presented=view_state(object)->presented;
    if (presented) return presented;
    Word top=navigation_top(object);
    return top && view_state(top)->presented ? view_state(top)->presented : top;
}
static void navigation_push(Word object,const char *selector,Word controller,Word animated) {
    if (!controller || controller==object || view_state(controller)->parent) stop("Invalid navigation push",selector);
    view_add(object,selector,controller);
}
static Word navigation_pop(Word object,const char *selector,Word animated) {
    struct Sequence *children=(void *)view_state(object)->children;
    if (children->count<=1) return 0;
    Word controller=children->items[children->count-1]; view_remove(controller); return controller;
}
static void navigation_set_controllers(Word object,const char *selector,struct Sequence *controllers,Word animated) {
    if (!controllers || !controllers->count) stop("Empty navigation stack",selector);
    for (size_t i=0;i<controllers->count;++i) {
        Word child=controllers->items[i];
        if (!child || child==object || (view_state(child)->parent && view_state(child)->parent!=object)) stop("Invalid navigation child",selector);
        for (size_t j=0;j<i;++j) if (controllers->items[j]==child) stop("Duplicate navigation child",selector);
    }
    struct Sequence *children=(void *)view_state(object)->children;
    if (controllers==children) return;
    while (children->count) view_remove(children->items[children->count-1]);
    for (size_t i=0;i<controllers->count;++i) view_add(object,selector,controllers->items[i]);
}
static void navigation_set_controllers_plain(Word object,const char *selector,struct Sequence *controllers) { navigation_set_controllers(object,selector,controllers,0); }
static Word navigation_init_root(Word object,const char *selector,Word controller) { navigation_push(object,selector,controller,0); return object; }
static void controller_set_modal_popover(Word object,const char *selector,Word enabled) { view_state(object)->modal_in_popover=enabled!=0; }
static Word controller_modal_popover(Word object) { return view_state(object)->modal_in_popover; }
static Word window_controller(Word object) { return view_state(object)->controller; }
static Word controller_presented(Word object) { return view_state(object)->presented; }
static Word controller_presenting(Word object) { return view_state(object)->presenting; }
static Word controller_is_presenting(Word object) { return view_state(object)->presenting_transition; }
static Word controller_is_dismissing(Word object) { return view_state(object)->dismissing_transition; }
static void controller_modal_style(Word object,const char *selector,long style) { view_state(object)->modal_style=style; }
static Word controller_get_modal_style(Word object) { return view_state(object)->modal_style; }
static void controller_transition_style(Word object,const char *selector,long style) { view_state(object)->transition_style=style; }
static Word controller_get_transition_style(Word object) { return view_state(object)->transition_style; }
static Word controller_popover(Word object) {
    struct ViewState *state=view_state(object);
    if (state->modal_style!=6) return 0;
    if (!state->popover) state->popover=host_allocate(external_class("_OBJC_CLASS_$_UIPopoverPresentationController"));
    return state->popover;
}
static void popover_set_source(Word object,const char *selector,Word source) { view_state(object)->view=source; }
static Word popover_source(Word object) { return view_state(object)->view; }
static void popover_set_delegate(Word object,const char *selector,Word delegate) { view_state(object)->controller=delegate; }
static Word popover_delegate(Word object) { return view_state(object)->controller; }
static void popover_set_arrows(Word object,const char *selector,Word directions) { view_state(object)->tag=directions; }
static Word popover_arrows(Word object) { return view_state(object)->tag; }
static Word view_descendant(Word object,const char *selector,Word ancestor) {
    if (!ancestor) return 0;
    for (unsigned depth=0;object && depth<1024;++depth) {
        if (object==ancestor) return 1;
        object=view_state(object)->parent;
    }
    if (object) stop("Cycle in UIView parent hierarchy",selector);
    return 0;
}
static Word view_window(Word object) {
    for (unsigned depth=0;object && depth<1024;++depth) {
        const char *base=external_base(read_word(object));
        if (object==(Word)&main_window || (base && !strcmp(base,"_OBJC_CLASS_$_UIWindow"))) return object;
        object=view_state(object)->parent;
    }
    return 0;
}
static void window_set_controller(Word object,const char *selector,Word controller) {
    struct ViewState *window=view_state(object);
    if (window->controller && window->controller!=controller) {
        struct ViewState *previous=view_state(window->controller);
        previous->appeared=0;
        if (previous->view) view_remove(previous->view);
        previous->owner_window=0;
    }
    window->controller=controller;
    if(object==key_window)root_controller=controller;
    if (controller) {
        view_state(controller)->owner_window=object;
        if(!window->hidden)window_schedule_presentation(object);
    }
}
static void window_present(void *value) {
    Word object=(Word)value;struct ViewState *window=view_state(object);window->appearance_pending=0;
    Word controller=window->controller;if(window->hidden || !controller || !window_lifecycle_ready)return;
    struct ViewState *state=view_state(controller);Word view=controller_view(controller);if(!view)return;
    if(!state->appeared){ui_table_controller_appear(controller);Method callback=find_implementation(controller,"viewWillAppear:");if(callback)callback(controller,"viewWillAppear:",0,0,0,0);}
    if(window->hidden || window->controller!=controller)return;
    if(view_state(view)->parent!=object)view_add(object,"Window presentation",view);
    controller_geometry(controller);view_request_layout(view,"Window presentation");view_layout_if_needed(view,"Window presentation");view_sync_subtree(view);
    if(!state->appeared){state->appeared=1;Method callback=find_implementation(controller,"viewDidAppear:");if(callback)callback(controller,"viewDidAppear:",0,0,0,0);}
    JNIEnv *env=attached_environment("Window presentation");static jmethodID ready;if(!ready){jclass cls=(*env)->GetObjectClass(env,activity);ready=(*env)->GetMethodID(env,cls,"guestWindowReady","()V");(*env)->DeleteLocalRef(env,cls);}(*env)->CallVoidMethod(env,activity,ready);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Android window presentation failed","UIWindow");}
}
static void window_schedule_presentation(Word object){struct ViewState *state=view_state(object);if(window_lifecycle_ready && !state->hidden && !state->appearance_pending){state->appearance_pending=1;schedule_main_function(window_present,(void *)object);}}
static Word window_is_key(Word object){return key_window==object;}
static void window_become_key(Word object,const char *selector){key_window=object;root_controller=view_state(object)->controller;}
static void window_resign_key(Word object,const char *selector){if(key_window==object){key_window=0;root_controller=0;}}
static void window_make_key(Word object,const char *selector){
    if(key_window==object)return;Word previous=key_window;
    if(previous){Method callback=find_implementation(previous,"resignKeyWindow");if(callback)callback(previous,"resignKeyWindow",0,0,0,0);platform_notification("UIWindowDidResignKeyNotification",previous,0);}
    window_become_key(object,selector);Method callback=find_implementation(object,"becomeKeyWindow");if(callback && callback!=(Method)window_become_key)callback(object,"becomeKeyWindow",0,0,0,0);platform_notification("UIWindowDidBecomeKeyNotification",object,0);
}
static void window_set_hidden(Word object,const char *selector,unsigned char hidden){
    struct ViewState *state=view_state(object);int changed=state->hidden!=(hidden!=0);state->hidden=hidden!=0;
    if(state->hidden && state->controller){struct ViewState *controller=view_state(state->controller);if(controller->appeared){controller->appeared=0;Method before=find_implementation(state->controller,"viewWillDisappear:"),after=find_implementation(state->controller,"viewDidDisappear:");if(before)before(state->controller,"viewWillDisappear:",0,0,0,0);view_sync_subtree(object);if(after)after(state->controller,"viewDidDisappear:",0,0,0,0);}}
    if(changed && window_lifecycle_ready)platform_notification(hidden ? "UIWindowDidBecomeHiddenNotification" : "UIWindowDidBecomeVisibleNotification",object,0);
    if(!hidden)window_schedule_presentation(object);view_sync_subtree(object);
}
static void window_make_visible(Word object,const char *selector){window_make_key(object,selector);window_set_hidden(object,selector,0);}
static void view_set_clips(Word object,const char *selector,Word clips) { struct ViewState *view=view_state(object);if(view->clips==(clips!=0))return;view->clips=clips!=0;if(view->layer)view_state(view->layer)->clips=view->clips;view_sync_subtree(object); }
static Word view_clips(Word object) { return view_state(object)->clips; }
struct LayerState {
    struct GuestTransform3D transform,sublayer_transform;
    struct GuestPoint anchor,start,end;
    struct GuestRect contents_rect;
    Word background,border,colors,locations,contents,gravity,type,delegate,synced_background,synced_contents;
    double border_width;
    unsigned dirty,children_ready,layout_pending,layout_running,background_set,published;
};
static struct LayerState *layer_state(Word object) {
    struct ViewState *view=view_state(object);
    if(!view->composition){struct LayerState *layer=calloc(1,sizeof(*layer));if(!layer)stop("Allocation failed","CALayer");
        layer->transform=layer->sublayer_transform=transform3d_identity;layer->anchor=(struct GuestPoint){.5,.5};layer->end=(struct GuestPoint){.5,1};layer->start=(struct GuestPoint){.5,0};
        layer->contents_rect=(struct GuestRect){0,0,1,1};layer->dirty=1;view->composition=layer;if(!view->view)view->scale=1;}
    return view->composition;
}
static struct LayerState *view_backing_state(Word object) { struct ViewState *view=view_state(object);return view->layer ? view_state(view->layer)->composition : NULL; }
static struct GuestPoint view_anchor(Word object) { struct LayerState *layer=view_backing_state(object);return layer ? layer->anchor : (struct GuestPoint){.5,.5}; }
static int layer_object(Word object) { return object && class_inherits(read_word(object),external_class("_OBJC_CLASS_$_CALayer")); }
static struct GuestTransform3D ui_matrix_multiply(struct GuestTransform3D left,struct GuestTransform3D right) {
    struct GuestTransform3D result={0};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)result.m[c*4+r]+=left.m[k*4+r]*right.m[c*4+k];return result;
}
static struct GuestTransform3D ui_matrix_translation(double x,double y) { struct GuestTransform3D result=transform3d_identity;result.m[12]=x;result.m[13]=y;return result; }
static struct GuestTransform3D ui_local_matrix(Word object) {
    struct ViewState *view=view_state(object);struct LayerState *layer=layer_object(object) ? view->composition : view_backing_state(object);
    struct GuestPoint anchor=layer ? layer->anchor : (struct GuestPoint){.5,.5};
    struct GuestTransform3D result=ui_matrix_translation(view->frame.x+view->bounds.width*anchor.x,view->frame.y+view->bounds.height*anchor.y);
    if(layer)result=ui_matrix_multiply(result,layer->transform);
    return ui_matrix_multiply(result,ui_matrix_translation(-view->bounds.width*anchor.x,-view->bounds.height*anchor.y));
}
static struct GuestTransform3D ui_children_matrix(Word object) {
    struct ViewState *view=view_state(object);struct LayerState *layer=layer_object(object) ? view->composition : view_backing_state(object);
    if(layer_object(object) && view->view)view=view_state(view->view);
    if(!layer)return ui_matrix_translation(-view->bounds.x,-view->bounds.y);
    double x=layer->anchor.x*view->bounds.width,y=layer->anchor.y*view->bounds.height;
    return ui_matrix_multiply(ui_matrix_multiply(ui_matrix_translation(x,y),layer->sublayer_transform),ui_matrix_translation(-x-view->bounds.x,-y-view->bounds.y));
}
static Word layer_owner(Word object) {
    for(Word current=object;current;current=view_state(current)->parent){struct ViewState *view=view_state(current);if(view->view)return view->view;}return 0;
}
static Word view_container(Word object) { for(Word current=object;current;current=view_state(current)->parent)if(view_state(current)->host_container)return current;return 0; }
static struct GuestTransform3D ui_world_matrix(Word object,Word container) {
    if(!object || object==container)return transform3d_identity;
    struct GuestTransform3D result=ui_local_matrix(object);Word parent=view_state(object)->parent;
    for(;parent && parent!=container;parent=view_state(parent)->parent)result=ui_matrix_multiply(ui_matrix_multiply(ui_local_matrix(parent),ui_children_matrix(parent)),result);
    if(parent)result=ui_matrix_multiply(ui_children_matrix(parent),result);return result;
}
static struct GuestTransform3D layer_world_matrix(Word object,Word container) {
    struct ViewState *view=view_state(object);if(view->view)return ui_world_matrix(view->view,container);
    struct GuestTransform3D result=ui_local_matrix(object);
    for(Word parent=view->parent;parent;parent=view_state(parent)->parent){struct ViewState *ancestor=view_state(parent);
        if(ancestor->view)return ui_matrix_multiply(ui_matrix_multiply(ui_world_matrix(ancestor->view,container),ui_children_matrix(parent)),result);
        result=ui_matrix_multiply(ui_matrix_multiply(ui_local_matrix(parent),ui_children_matrix(parent)),result);
    }return result;
}
static struct GuestPoint ui_project_point(struct GuestTransform3D matrix,struct GuestPoint point) {
    double w=point.x*matrix.m[3]+point.y*matrix.m[7]+matrix.m[15];if(!isfinite(w) || fabs(w)<1e-12)return (struct GuestPoint){NAN,NAN};
    return (struct GuestPoint){(point.x*matrix.m[0]+point.y*matrix.m[4]+matrix.m[12])/w,(point.x*matrix.m[1]+point.y*matrix.m[5]+matrix.m[13])/w};
}
static struct GuestPoint ui_unproject_point(struct GuestTransform3D m,struct GuestPoint p) {
    double a=m.m[0]-p.x*m.m[3],b=m.m[4]-p.x*m.m[7],c=p.x*m.m[15]-m.m[12],d=m.m[1]-p.y*m.m[3],e=m.m[5]-p.y*m.m[7],f=p.y*m.m[15]-m.m[13],det=a*e-b*d;
    if(!isfinite(det) || fabs(det)<1e-12)return (struct GuestPoint){NAN,NAN};return (struct GuestPoint){(c*e-b*f)/det,(a*f-c*d)/det};
}
static struct GuestRect ui_project_rect(struct GuestTransform3D matrix,struct GuestRect rect) {
    double left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;
    for(unsigned i=0;i<4;++i){struct GuestPoint p=ui_project_point(matrix,(struct GuestPoint){rect.x+((i&1) ? rect.width : 0),rect.y+((i&2) ? rect.height : 0)});if(!isfinite(p.x) || !isfinite(p.y))return (struct GuestRect){NAN,NAN,0,0};left=fmin(left,p.x);top=fmin(top,p.y);right=fmax(right,p.x);bottom=fmax(bottom,p.y);}
    return (struct GuestRect){left,top,right-left,bottom-top};
}
static struct GuestRect view_transformed_frame(Word object) {
    struct ViewState *view=view_state(object);struct LayerState *layer=view_backing_state(object);
    if(!layer || !memcmp(&layer->transform,&transform3d_identity,sizeof(transform3d_identity)))return view->frame;
    return ui_project_rect(ui_local_matrix(object),(struct GuestRect){0,0,view->bounds.width,view->bounds.height});
}
static struct GuestPoint view_convert_point(Word object,const char *selector,struct GuestPoint point,Word other) {
    if(!other)other=view_window(object);
    Word from=strstr(selector,"fromView:") ? other : object,to=strstr(selector,"fromView:") ? object : other;
    if(from){struct ViewState *view=view_state(from);point.x-=view->bounds.x;point.y-=view->bounds.y;point=ui_project_point(ui_world_matrix(from,0),point);}
    if(to){point=ui_unproject_point(ui_world_matrix(to,0),point);point.x+=view_state(to)->bounds.x;point.y+=view_state(to)->bounds.y;}return point;
}
static struct GuestRect view_convert_rect(Word object,const char *selector,struct GuestRect rect,Word other) {
    double left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;const char *point_selector=strstr(selector,"fromView:") ? "convertPoint:fromView:" : "convertPoint:toView:";
    for(unsigned i=0;i<4;++i){struct GuestPoint p=view_convert_point(object,point_selector,(struct GuestPoint){rect.x+((i&1) ? rect.width : 0),rect.y+((i&2) ? rect.height : 0)},other);left=fmin(left,p.x);top=fmin(top,p.y);right=fmax(right,p.x);bottom=fmax(bottom,p.y);}return (struct GuestRect){left,top,right-left,bottom-top};
}
static struct GuestTransform3D layer_transform(Word object) { return layer_state(object)->transform; }
static void layer_set_transform(Word object,const char *selector,struct GuestTransform3D transform);
static struct GuestAffine view_get_transform(Word object) {
    struct LayerState *layer=view_backing_state(object);struct GuestTransform3D matrix=layer ? layer->transform : transform3d_identity;return (struct GuestAffine){matrix.m[0],matrix.m[1],matrix.m[4],matrix.m[5],matrix.m[12],matrix.m[13]};
}
static void view_set_transform(Word object,const char *selector,struct GuestAffine value) {
    struct GuestTransform3D matrix=transform3d_identity;matrix.m[0]=value.a;matrix.m[1]=value.b;matrix.m[4]=value.c;matrix.m[5]=value.d;matrix.m[12]=value.tx;matrix.m[13]=value.ty;layer_set_transform(view_layer(object),selector,matrix);
}
static Word layer_initialize(Word object) { layer_state(object);return object; }
static Word layer_create(Word cls) { return layer_initialize(host_allocate(cls)); }
static void layer_backing_remove(Word object) {
    Word layer=view_state(object)->layer;if(!layer)return;struct ViewState *view=view_state(layer);if(view->parent)sequence_remove((void *)view_state(view->parent)->children,"Backing layer removal",layer);view->parent=0;
}
static void layer_backing_add(Word parent,Word child) {
    Word layer=view_state(parent)->layer;if(!layer || !layer_state(layer)->children_ready)return;
    Word sublayer=view_layer(child);layer_backing_remove(child);view_state(sublayer)->parent=layer;sequence_add((void *)view_state(layer)->children,"Backing layer insertion",sublayer);
}
static void layer_materialize_children(Word object) {
    struct LayerState *layer=layer_state(object);if(layer->children_ready)return;layer->children_ready=1;
    Word owner=view_state(object)->view;if(!owner)return;struct Sequence *children=(void *)view_state(owner)->children;
    for(size_t i=0;i<children->count;++i)layer_backing_add(owner,children->items[i]);
}
static Word layer_superlayer(Word object) {
    struct ViewState *view=view_state(object);if(view->parent)return view->parent;
    if(view->view && view_state(view->view)->parent){Word parent=view_layer(view_state(view->view)->parent);layer_materialize_children(parent);return view->parent;}return 0;
}
static void layer_changed(Word object) {
    layer_state(object)->dirty=1;Word owner=layer_owner(object);if(owner){view_sync_subtree(owner);ui_view_order_sync(owner);}else ui_layer_sync_tree(object);
}
static Word layer_sublayers(Word object) { layer_materialize_children(object);return sequence_copy((void *)view_state(object)->children,"CALayer sublayers"); }
static void layer_remove(Word object,const char *selector) {
    struct ViewState *view=view_state(object);if(view->view)stop("Direct removal of a UIView backing layer is unavailable",selector);
    Word parent=view->parent;if(parent)sequence_remove((void *)view_state(parent)->children,selector,object);view->parent=0;ui_layer_sync_tree(object);if(parent)layer_changed(parent);
}
static void layer_insert(Word object,const char *selector,Word child,Word index) {
    if(!layer_object(child))stop("Invalid sublayer",selector);if(view_state(child)->view)stop("Reparenting a UIView backing layer is unavailable",selector);
    for(Word parent=object;parent;parent=layer_superlayer(parent))if(parent==child)stop("Cyclic layer tree",selector);
    layer_materialize_children(object);struct Sequence *children=(void *)view_state(object)->children;if(index>children->count)stop("Sublayer index out of range",selector);
    if(view_state(child)->parent==object){size_t old=0;while(old<children->count && children->items[old]!=child)++old;if(old<children->count && old<index)--index;}
    if(view_state(child)->parent)layer_remove(child,selector);view_state(child)->parent=object;sequence_add(children,selector,child);
    if(index+1<children->count){memmove(children->items+index+1,children->items+index,(children->count-index-1)*sizeof(Word));children->items[index]=child;++children->mutation;}layer_changed(object);
}
static void layer_add(Word object,const char *selector,Word child) { layer_materialize_children(object);layer_insert(object,selector,child,((struct Sequence *)view_state(object)->children)->count); }
static void layer_insert_relative(Word object,const char *selector,Word child,Word sibling) {
    layer_materialize_children(object);struct Sequence *children=(void *)view_state(object)->children;size_t index=0;
    if(!sibling)index=strstr(selector,"above:") ? children->count : 0;
    else {while(index<children->count && children->items[index]!=sibling)++index;if(index==children->count)stop("Relative layer is not a sublayer",selector);if(child==sibling)return;if(strstr(selector,"above:"))++index;}layer_insert(object,selector,child,index);
}
static void layer_replace(Word object,const char *selector,Word old,Word replacement) {
    layer_materialize_children(object);struct Sequence *children=(void *)view_state(object)->children;size_t index=0;while(index<children->count && children->items[index]!=old)++index;
    if(index==children->count)stop("Layer to replace is not a sublayer",selector);if(old==replacement)return;layer_insert(object,selector,replacement,index);layer_remove(old,selector);
}
static void layer_set_children(Word object,const char *selector,Word value) {
    layer_materialize_children(object);struct Sequence *children=(void *)view_state(object)->children,*input=value ? (void *)value : NULL;
    if(input)for(size_t i=0;i<input->count;++i){if(!layer_object(input->items[i]) || view_state(input->items[i])->view)stop("Unsupported backing layer mutation",selector);for(Word p=object;p;p=layer_superlayer(p))if(p==input->items[i])stop("Cyclic layer tree",selector);for(size_t j=0;j<i;++j)if(input->items[j]==input->items[i])stop("Duplicate sublayer",selector);}
    for(size_t i=0;i<children->count;++i)if(view_state(children->items[i])->view)stop("Replacing UIView backing sublayers is unavailable",selector);
    Word copy=input ? sequence_copy(input,"copy") : sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));while(children->count)layer_remove(children->items[children->count-1],selector);
    struct Sequence *items=(void *)copy;for(size_t i=0;i<items->count;++i)layer_add(object,selector,items->items[i]);free(items->items);free(items);
}
static struct ViewState *layer_geometry(Word object) { struct ViewState *view=view_state(object);return view->view ? view_state(view->view) : view; }
static struct GuestRect layer_frame(Word object) { struct ViewState *view=layer_geometry(object);return ui_project_rect(ui_local_matrix(view->object),(struct GuestRect){0,0,view->bounds.width,view->bounds.height}); }
static struct GuestRect layer_bounds(Word object) { return layer_geometry(object)->bounds; }
static void layer_set_frame(Word object,const char *selector,struct GuestRect frame) { struct ViewState *view=layer_geometry(object);view_set_frame(view->object,selector,frame);layer_changed(object); }
static void layer_set_bounds(Word object,const char *selector,struct GuestRect bounds) { struct ViewState *view=layer_geometry(object);view_set_bounds(view->object,selector,bounds);layer_changed(object); }
static struct GuestPoint layer_position(Word object) { struct ViewState *view=layer_geometry(object);struct GuestPoint anchor=layer_state(object)->anchor;return (struct GuestPoint){view->frame.x+view->bounds.width*anchor.x,view->frame.y+view->bounds.height*anchor.y}; }
static void layer_set_position(Word object,const char *selector,struct GuestPoint point) {
    if(!isfinite(point.x) || !isfinite(point.y))stop("Invalid layer position",selector);struct ViewState *view=layer_geometry(object);if(view_animation)view_animation_change(view->object);
    struct GuestPoint anchor=layer_state(object)->anchor;view->frame.x=point.x-view->bounds.width*anchor.x;view->frame.y=point.y-view->bounds.height*anchor.y;layer_changed(object);
}
static struct GuestPoint layer_anchor(Word object) { return layer_state(object)->anchor; }
static void layer_set_anchor(Word object,const char *selector,struct GuestPoint anchor) {
    if(!isfinite(anchor.x) || !isfinite(anchor.y))stop("Invalid layer anchor",selector);struct LayerState *layer=layer_state(object);struct ViewState *view=layer_geometry(object);if(view_animation)view_animation_change(view->object);
    view->frame.x+=(layer->anchor.x-anchor.x)*view->bounds.width;view->frame.y+=(layer->anchor.y-anchor.y)*view->bounds.height;layer->anchor=anchor;layer_changed(object);
}
static void layer_set_transform(Word object,const char *selector,struct GuestTransform3D transform) {
    for(unsigned i=0;i<16;++i)if(!isfinite(transform.m[i]))stop("Invalid layer transform",selector);struct ViewState *view=layer_geometry(object);if(view_animation)view_animation_change(view->object);
    layer_state(object)->transform=transform;layer_changed(object);
}
static struct GuestAffine layer_affine(Word object) { struct GuestTransform3D t=layer_transform(object);return (struct GuestAffine){t.m[0],t.m[1],t.m[4],t.m[5],t.m[12],t.m[13]}; }
static void layer_set_affine(Word object,const char *selector,struct GuestAffine value) { struct GuestTransform3D t=transform3d_identity;t.m[0]=value.a;t.m[1]=value.b;t.m[4]=value.c;t.m[5]=value.d;t.m[12]=value.tx;t.m[13]=value.ty;layer_set_transform(object,selector,t); }
static struct GuestTransform3D layer_sublayer_transform(Word object) { return layer_state(object)->sublayer_transform; }
static void layer_set_sublayer_transform(Word object,const char *selector,struct GuestTransform3D value) {
    for(unsigned i=0;i<16;++i)if(!isfinite(value.m[i]))stop("Invalid sublayer transform",selector);if(view_animation)view_animation_change(layer_geometry(object)->object);layer_state(object)->sublayer_transform=value;layer_changed(object);
}
static double layer_metric(Word object,const char *selector) {
    struct ViewState *view=view_state(object);struct LayerState *layer=layer_state(object);
    if(!strcmp(selector,"borderWidth"))return layer->border_width;if(!strcmp(selector,"cornerRadius"))return view->corner;if(!strcmp(selector,"zPosition"))return view->z;return view->view ? view_state(view->view)->scale : view->scale;
}
static void layer_set_metric(Word object,const char *selector,double value) {
    if(!isfinite(value) || (strcmp(selector,"setZPosition:") && value<0))stop("Invalid layer metric",selector);struct ViewState *view=view_state(object);struct LayerState *layer=layer_state(object);
    if(!strcmp(selector,"setBorderWidth:"))layer->border_width=value;else if(!strcmp(selector,"setCornerRadius:"))view->corner=value;else if(!strcmp(selector,"setZPosition:"))view->z=value;else {view->scale=value;if(view->view)view_state(view->view)->scale=value;}layer_changed(object);
}
static float layer_opacity(Word object) { return (float)layer_geometry(object)->alpha; }
static void layer_set_opacity(Word object,const char *selector,float value) { if(!isfinite(value))stop("Invalid layer opacity",selector);struct ViewState *view=layer_geometry(object);if(view_animation)view_animation_change(view->object);view->alpha=fmin(1,fmax(0,value));layer_changed(object); }
static Word layer_flag(Word object,const char *selector) { struct ViewState *view=view_state(object);return !strcmp(selector,"isHidden") ? layer_geometry(object)->hidden : view->view ? view_state(view->view)->clips : view->clips; }
static void layer_set_flag(Word object,const char *selector,Word value) { if(!strcmp(selector,"setHidden:"))layer_geometry(object)->hidden=value!=0;else {struct ViewState *view=view_state(object);view->clips=value!=0;if(view->view)view_state(view->view)->clips=view->clips;}layer_changed(object); }
static Word layer_value(Word object,const char *selector) {
    struct ViewState *view=view_state(object);struct LayerState *layer=layer_state(object);
    if(!strcmp(selector,"backgroundColor"))return view->view ? view_state(view->view)->background : layer->background;if(!strcmp(selector,"borderColor"))return layer->border;if(!strcmp(selector,"colors"))return layer->colors;if(!strcmp(selector,"locations"))return layer->locations;if(!strcmp(selector,"contents"))return layer->contents;
    if(!strcmp(selector,"contentsGravity"))return layer->gravity ? layer->gravity : copy_string("resize");if(!strcmp(selector,"type"))return layer->type ? layer->type : copy_string("axial");return load_weak(&layer->delegate);
}
static void layer_set_value(Word object,const char *selector,Word value) {
    struct ViewState *view=view_state(object);struct LayerState *layer=layer_state(object);
    if(!strcmp(selector,"setDelegate:")){store_weak(&layer->delegate,value);return;}
    if(!strcmp(selector,"setBackgroundColor:") || !strcmp(selector,"setBorderColor:")){if(value && !class_inherits(read_word(value),external_class("_OBJC_CLASS_$_UIColor")))stop("Unsupported layer color",selector);
        if(!strcmp(selector,"setBorderColor:"))layer->border=value;else {layer->background_set=1;if(view->view)view_state(view->view)->background=value;else layer->background=value;}}
    else if(!strcmp(selector,"setContents:")){if(value && !class_inherits(read_word(value),external_class("_OBJC_CLASS_$_UIImage")))stop("Unsupported layer image contents",selector);layer->contents=value;}
    else if(!strcmp(selector,"setContentsGravity:")){const char *name=value ? string_bytes(value,selector) : "resize";const char *valid[]={"resize","resizeAspect","resizeAspectFill","center","top","bottom","left","right","topLeft","topRight","bottomLeft","bottomRight"};unsigned found=0;for(unsigned i=0;i<COUNT(valid);++i)found|=!strcmp(name,valid[i]);if(!found)stop("Unsupported layer contents gravity",selector);layer->gravity=copy_string(name);}
    else if(!strcmp(selector,"setType:")){if(value && strcmp(string_bytes(value,selector),"axial"))stop("Unsupported gradient type",selector);layer->type=value ? text_copy(value,"copy") : 0;}
    else {if(value && !class_inherits(read_word(value),external_class("_OBJC_CLASS_$_NSArray")))stop("Invalid gradient array",selector);struct Sequence *values=(void *)value;
        if(values)for(size_t i=0;i<values->count;++i){if(!strcmp(selector,"setColors:")){if(!class_inherits(read_word(values->items[i]),external_class("_OBJC_CLASS_$_UIColor")))stop("Unsupported gradient color",selector);}else {if(!class_inherits(read_word(values->items[i]),external_class("_OBJC_CLASS_$_NSNumber")))stop("Invalid gradient location",selector);double p=numeric_double((void *)values->items[i]);if(!isfinite(p) || p<0 || p>1 || (i && p<numeric_double((void *)values->items[i-1])))stop("Invalid gradient stops",selector);}}
        Word copy=value ? sequence_copy((void *)value,"copy") : 0;Word *slot=!strcmp(selector,"setColors:") ? &layer->colors : &layer->locations;Word old=*slot;*slot=copy;if(old){struct Sequence *sequence=(void *)old;free(sequence->items);free(sequence);}}
    layer_changed(object);
}
static struct GuestPoint layer_gradient_point(Word object,const char *selector) { return !strcmp(selector,"startPoint") ? layer_state(object)->start : layer_state(object)->end; }
static void layer_set_gradient_point(Word object,const char *selector,struct GuestPoint point) { if(!isfinite(point.x) || !isfinite(point.y))stop("Invalid gradient point",selector);if(!strcmp(selector,"setStartPoint:"))layer_state(object)->start=point;else layer_state(object)->end=point;layer_changed(object); }
static struct GuestRect layer_contents_rect(Word object) { return layer_state(object)->contents_rect; }
static void layer_set_contents_rect(Word object,const char *selector,struct GuestRect rect) { if(!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.width) || !isfinite(rect.height) || rect.width<0 || rect.height<0)stop("Invalid layer contents rectangle",selector);layer_state(object)->contents_rect=rect;layer_changed(object); }
static Word layer_contains_point(Word object,const char *selector,struct GuestPoint point) { struct GuestRect bounds=layer_bounds(object);return point.x>=bounds.x && point.y>=bounds.y && point.x<bounds.x+bounds.width && point.y<bounds.y+bounds.height; }
static int ui_layer_has_content(Word object) { struct ViewState *view=view_state(object);struct LayerState *layer=view_backing_state(object);return layer && (layer->border_width>0 || layer->contents || layer->colors || view_state(view->layer)->corner>0); }
static void layer_layout_content(Word object,const char *selector) { struct LayerState *layer=layer_state(object);Word delegate=load_weak_retained(&layer->delegate);Method callback=delegate ? find_implementation(delegate,"layoutSublayersOfLayer:") : NULL;if(callback)callback(delegate,"layoutSublayersOfLayer:",object,0,0,0);metal_object_release(delegate); }
static void layer_layout_if_needed(Word object,const char *selector) {
    struct LayerState *layer=layer_state(object);if(layer->layout_running)return;
    if(layer->layout_pending){layer->layout_pending=0;layer->layout_running=1;Method layout=find_implementation(object,"layoutSublayers");if(layout)layout(object,"layoutSublayers",0,0,0,0);else layer_layout_content(object,"layoutSublayers");layer->layout_running=0;}
    struct Sequence *children=(void *)view_state(object)->children;for(size_t i=0;i<children->count;++i)layer_layout_if_needed(children->items[i],selector);
}
static void layer_layout_deliver(void *object) { layer_layout_if_needed((Word)object,"layoutIfNeeded"); }
static void layer_needs_layout(Word object,const char *selector) { struct LayerState *layer=layer_state(object);if(layer->layout_pending)return;layer->layout_pending=1;schedule_main_function(layer_layout_deliver,(void *)object); }
static Word layer_get_needs_layout(Word object) { return layer_state(object)->layout_pending; }

static void view_animation_capture(Word object) {
    if (ui_view_rendered(object) || view_state(object)->clips || (view_state(object)->layer && view_state(view_state(object)->layer)->clips)) {
        struct ViewOpacity *item=view_animation->items;
        while (item && item->object!=object) item=item->next;
        if (!item) { item=calloc(1,sizeof(*item)); if (!item) stop("Allocation failed","UIView animation"); item->object=object;view_presentation(object,item->presentation); item->next=view_animation->items; view_animation->items=item; }
    }
    struct Sequence *children=(void *)view_state(object)->children;
    for (size_t i=0;i<children->count;++i) view_animation_capture(children->items[i]);
    if(view_state(object)->layer){struct Sequence *layers=(void *)view_state(view_state(object)->layer)->children;for(size_t i=0;i<layers->count;++i)if(!view_state(layers->items[i])->view)view_animation_capture(layers->items[i]);}
}
static void view_animation_change(Word object) {
    struct ViewOpacity *source=view_animation->sources;
    while (source && source->object!=object) source=source->next;
    if (!source) { source=calloc(1,sizeof(*source)); if (!source) stop("Allocation failed","UIView animation"); source->object=object; source->before=(float)view_state(object)->alpha; source->next=view_animation->sources; view_animation->sources=source; }
    view_animation_capture(object);
}
static void view_animation_free(struct ViewAnimation *context) {
    while (context->items) { struct ViewOpacity *item=context->items; context->items=item->next; free(item); }
    while (context->sources) { struct ViewOpacity *item=context->sources; context->sources=item->next; free(item); }
    destroy_weak(&context->delegate);metal_object_release(context->identifier);free(context);
}
static Word view_opacity_parent(Word object) {
    struct ViewState *view=view_state(object);Word parent=view->parent;
    if(layer_object(object) && parent && view_state(parent)->view)parent=view_state(parent)->view;
    return view->host_container ? 0 : parent;
}
struct ViewAnimationCompletion { Word block,delegate,identifier,context;const char *started,*stopped; };
static void view_animation_completion_free(struct ViewAnimationCompletion *value) { if(value->block)_Block_release((void *)value->block);destroy_weak(&value->delegate);metal_object_release(value->identifier);free(value); }
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_completeViewAnimation)(JNIEnv *env,jobject activity,jlong token,jint event,jboolean completed) {
    struct ViewAnimationCompletion *value=(void *)token;if(!value)return NULL;may_unwind=1;snprintf(stage,sizeof(stage),"UIView animation callback");int failed=setjmp(boundary);
    if(!failed){Word delegate=load_weak_retained(&value->delegate);const char *selector=event==0 ? value->started : value->stopped;Method method=delegate && selector ? find_implementation(delegate,selector) : NULL;
        if(method){if(event==0)method(delegate,selector,value->identifier,value->context,0,0);else method(delegate,selector,value->identifier,completed,value->context,0);}metal_object_release(delegate);
        if(event!=0 && value->block){struct Block_layout *block=(void *)value->block;((void (*)(Word,unsigned char))block->invoke)(value->block,completed);}}
    if(event!=0)view_animation_completion_free(value);may_unwind=0;return failed ? (*env)->NewStringUTF(env,status) : NULL;
}
static void view_animation_submit(struct ViewAnimation *context,const char *selector,Word completion) {
    double duration=context->duration,delay=context->delay;Word options=context->options;
    if(duration+delay>(double)INT64_MAX/1000.0)stop("Invalid animation time",selector);
    if(!view_animations_enabled){duration=delay=0;options&=~(Word)24;context->repeat=0;}
    unsigned count=0,sources=0,ancestors=0;
    for (struct ViewOpacity *item=context->items;item;item=item->next) { ++count; for (Word object=item->object;object;object=view_opacity_parent(object)) ++ancestors; }
    for (struct ViewOpacity *item=context->sources;item;item=item->next) ++sources;
    if (!count && !completion && !context->legacy) { view_animation_free(context); return; }
    JNIEnv *env=attached_environment(selector); static jmethodID animate;
    if (!animate) { jclass type=(*env)->GetObjectClass(env,activity); animate=(*env)->GetMethodID(env,type,"animateGuestViews","([J[F[F[J[I[J[F[F[FJJIFFFJ)V"); (*env)->DeleteLocalRef(env,type); }
    jlongArray changed=(*env)->NewLongArray(env,sources),tokens=(*env)->NewLongArray(env,count),chain=(*env)->NewLongArray(env,ancestors);
    jintArray offsets=(*env)->NewIntArray(env,count+1);
    jfloatArray before=(*env)->NewFloatArray(env,sources),after=(*env)->NewFloatArray(env,sources),model=(*env)->NewFloatArray(env,ancestors);
    jfloatArray geometry_before=(*env)->NewFloatArray(env,count*VIEW_PRESENTATION_FIELDS),geometry_after=(*env)->NewFloatArray(env,count*VIEW_PRESENTATION_FIELDS);
    if (!animate || !changed || !tokens || !chain || !offsets || !before || !after || !model || !geometry_before || !geometry_after) { view_animation_free(context); if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); stop("Android animation allocation failed",selector); }
    unsigned i=0;
    for (struct ViewOpacity *item=context->sources;item;item=item->next,++i) {
        jlong token=item->object; jfloat target=(float)view_state(item->object)->alpha;
        (*env)->SetLongArrayRegion(env,changed,i,1,&token); (*env)->SetFloatArrayRegion(env,before,i,1,&item->before); (*env)->SetFloatArrayRegion(env,after,i,1,&target);
    }
    i=0; jint cursor=0;
    for (struct ViewOpacity *item=context->items;item;item=item->next,++i) {
        jlong token=item->object; (*env)->SetLongArrayRegion(env,tokens,i,1,&token); (*env)->SetIntArrayRegion(env,offsets,i,1,&cursor);
        float presentation[VIEW_PRESENTATION_FIELDS];view_presentation(item->object,presentation);(*env)->SetFloatArrayRegion(env,geometry_before,i*VIEW_PRESENTATION_FIELDS,VIEW_PRESENTATION_FIELDS,item->presentation);(*env)->SetFloatArrayRegion(env,geometry_after,i*VIEW_PRESENTATION_FIELDS,VIEW_PRESENTATION_FIELDS,presentation);
        for (Word object=item->object;object;object=view_opacity_parent(object),++cursor) {
            token=object; jfloat alpha=(float)view_state(object)->alpha;
            (*env)->SetLongArrayRegion(env,chain,cursor,1,&token); (*env)->SetFloatArrayRegion(env,model,cursor,1,&alpha);
        }
    }
    (*env)->SetIntArrayRegion(env,offsets,count,1,&cursor);
    struct ViewAnimationCompletion *record=NULL;if(completion || context->legacy){record=calloc(1,sizeof(*record));if(!record){view_animation_free(context);stop("Allocation failed",selector);}record->block=completion ? (Word)_Block_copy((void *)completion) : 0;record->identifier=context->identifier ? metal_object_retain(context->identifier) : 0;record->context=context->context;record->started=context->started;record->stopped=context->stopped;Word delegate=load_weak_retained(&context->delegate);store_weak(&record->delegate,delegate);metal_object_release(delegate);}
    (*env)->CallVoidMethod(env,activity,animate,changed,before,after,tokens,offsets,chain,model,geometry_before,geometry_after,(jlong)(duration*1000),(jlong)(delay*1000),(jint)options,(jfloat)context->damping,(jfloat)context->velocity,(jfloat)context->repeat,(jlong)record);
    (*env)->DeleteLocalRef(env,changed); (*env)->DeleteLocalRef(env,tokens); (*env)->DeleteLocalRef(env,chain); (*env)->DeleteLocalRef(env,offsets); (*env)->DeleteLocalRef(env,before); (*env)->DeleteLocalRef(env,after); (*env)->DeleteLocalRef(env,model);(*env)->DeleteLocalRef(env,geometry_before);(*env)->DeleteLocalRef(env,geometry_after);view_animation_free(context);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env);if(record)view_animation_completion_free(record);stop("Android animation failed",selector); }
}
static void view_animate_mode(Word cls,const char *selector,double duration,double delay,Word options,Word animation,Word completion,double damping,double velocity) {
    if (!isfinite(duration) || !isfinite(delay) || duration<0 || delay<0 || duration+delay>(double)INT64_MAX/1000.0) stop("Invalid animation time",selector);
    struct ViewAnimation *context=calloc(1,sizeof(*context)); if (!context) stop("Allocation failed",selector);context->previous=view_animation;context->duration=duration;context->delay=delay;context->options=options;context->damping=damping;context->velocity=velocity;
    if(context->previous){if(!(options&32))context->duration=context->previous->duration;if(!(options&64))context->options=(options&~(Word)(3<<16))|(context->previous->options&(3<<16));}
    view_animation=context;struct GuestCallBoundary call={.previous=active_guest_call_boundary};active_guest_call_boundary=&call;int failed=setjmp(call.environment);
    if(!failed && animation){struct Block_layout *block=(void *)animation;((void (*)(Word))block->invoke)(animation);}active_guest_call_boundary=call.previous;view_animation=context->previous;
    if(failed){char failure[sizeof(status)];memcpy(failure,status,sizeof(failure));view_animation_free(context);stop("UIView animation failed",failure);}view_animation_submit(context,selector,completion);
}
static void view_animate(Word cls,const char *selector,double duration,double delay,Word options,Word animation,Word completion) { view_animate_mode(cls,selector,duration,delay,options,animation,completion,-1,0); }
static void view_animate_spring(Word cls,const char *selector,double duration,double delay,double damping,double velocity,Word options,Word animation,Word completion) { if(!isfinite(damping) || damping<0 || damping>1 || !isfinite(velocity))stop("Invalid spring parameters",selector);view_animate_mode(cls,selector,duration,delay,options,animation,completion,damping,velocity); }
static void view_animate_plain(Word cls,const char *selector,double duration,Word animation,Word completion) { view_animate(cls,selector,duration,0,0,animation,completion); }
static void view_animate_simple(Word cls,const char *selector,double duration,Word animation) { view_animate(cls,selector,duration,0,0,animation,0); }
static void view_begin_animations(Word cls,const char *selector,Word identifier,Word context) {
    struct ViewAnimation *value=calloc(1,sizeof(*value));if(!value)stop("Allocation failed",selector);value->previous=view_animation;value->duration=view_animation ? view_animation->duration : .2;value->damping=-1;value->identifier=identifier ? text_copy(identifier,"copy") : 0;value->context=context;value->legacy=1;view_animation=value;
}
static void view_commit_animations(Word cls,const char *selector) { struct ViewAnimation *value=view_animation;if(!value || !value->legacy)return;view_animation=value->previous;view_animation_submit(value,selector,0); }
static void view_animation_time(Word cls,const char *selector,double value) { if(!isfinite(value) || value<0 || value>(double)INT64_MAX/1000.0)stop("Invalid animation time",selector);if(!view_animation || !view_animation->legacy)return;if(!strcmp(selector,"setAnimationDuration:"))view_animation->duration=value;else view_animation->delay=value; }
static void view_animation_repeat(Word cls,const char *selector,float value) { if(!isfinite(value) || value<0 || (double)value>INT_MAX)stop("Invalid animation repeat count",selector);if(view_animation && view_animation->legacy)view_animation->repeat=value; }
static void view_animation_setting(Word cls,const char *selector,Word value) {
    if(!strcmp(selector,"setAnimationsEnabled:")){view_animations_enabled=value!=0;return;}if(!view_animation || !view_animation->legacy)return;
    if(!strcmp(selector,"setAnimationDelegate:"))store_weak(&view_animation->delegate,value);
    else if(!strcmp(selector,"setAnimationCurve:")){if(value>3)stop("Invalid animation curve",selector);view_animation->options=(view_animation->options&~(Word)(3<<16))|(value<<16);}
    else {Word bit=!strcmp(selector,"setAnimationRepeatAutoreverses:") ? 16 : 4;if(value)view_animation->options|=bit;else view_animation->options&=~bit;}
}
static void view_animation_selector(Word cls,const char *selector,const char *value) { if(!view_animation || !view_animation->legacy)return;if(!strcmp(selector,"setAnimationWillStartSelector:"))view_animation->started=value;else view_animation->stopped=value; }
static Word view_animations_state(void) { return view_animations_enabled; }
static void view_without_animation(Word cls,const char *selector,Word value) {
    unsigned previous=view_animations_enabled;struct ViewAnimation *previous_animation=view_animation;view_animations_enabled=0;view_animation=NULL;struct GuestCallBoundary call={.previous=active_guest_call_boundary};active_guest_call_boundary=&call;int failed=setjmp(call.environment);if(!failed && value){struct Block_layout *block=(void *)value;((void (*)(Word))block->invoke)(value);}active_guest_call_boundary=call.previous;view_animations_enabled=previous;view_animation=previous_animation;if(failed)stop("UIView animation block failed",selector);
}
JNIEXPORT jstring JNICALL HOST_JNI(HostActivity_completeAnimation)(JNIEnv *env,jobject activity,jlong value,jboolean completed) {
    may_unwind=1;
    snprintf(stage,sizeof(stage),"UIView animation completion");
    int failed=setjmp(boundary);
    if (!failed) {
        struct Block_layout *block=(void *)value;
        ((void (*)(void *,unsigned char))block->invoke)(block,completed);
    }
    _Block_release((void *)value);
    may_unwind=0;
    return failed ? (*env)->NewStringUTF(env,status) : NULL;
}

static double scene_number(plist_t dict,const char *key,double fallback) {
    plist_t value=plist_dict_get_item(dict,key);
    if (!value) return fallback;
    if (plist_get_node_type(value)==PLIST_BOOLEAN) { uint8_t flag=0; plist_get_bool_val(value,&flag); return flag; }
    if (plist_get_node_type(value)==PLIST_REAL) { double real=0; plist_get_real_val(value,&real); return real; }
    int64_t integer=0; plist_get_int_val(value,&integer); return (double)integer;
}
static void scene_attribute(double *equation,int node,int attribute,double scale) {
    if (node<0) return;
    double *terms=equation+node*4;
    switch (attribute) {
        case 1: case 5: terms[0]+=scale; break;
        case 2: case 6: terms[0]+=scale; terms[2]+=scale; break;
        case 3: terms[1]+=scale; break;
        case 4: terms[1]+=scale; terms[3]+=scale; break;
        case 7: terms[2]+=scale; break;
        case 8: terms[3]+=scale; break;
        case 9: terms[0]+=scale; terms[2]+=scale*0.5; break;
        case 10: terms[1]+=scale; terms[3]+=scale*0.5; break;
        default: stop("Unsupported startup layout attribute","Storyboard");
    }
}
static int layout_equation(double *basis,unsigned variables,double *row,int required) {
    for (unsigned i=0;i<variables;++i) {
        double factor=row[i];
        if (fabs(factor)<1e-9) continue;
        double *pivot=basis+i*(variables+1);
        if (pivot[i]) { for (unsigned j=i;j<=variables;++j) row[j]-=factor*pivot[j]; }
        else { for (unsigned j=i;j<=variables;++j) pivot[j]=row[j]/factor; return 1; }
    }
    return !required || fabs(row[variables])<=1e-5;
}
static void scene_equation(double *basis,unsigned variables,double *row,int required) { if (!layout_equation(basis,variables,row,required)) stop("Inconsistent required startup constraint","Storyboard"); }
struct LayoutConstraint { Word isa,first,second,owner,identifier; intptr_t attribute,other_attribute,relation; double multiplier,constant; float priority; unsigned active; };
static Word constraint_create(Word cls,const char *selector,Word first,intptr_t attribute,intptr_t relation,Word second,intptr_t other,double multiplier,double constant) {
    if (!first || attribute<1 || attribute>20 || (second && (other<1 || other>20)) || relation < -1 || relation>1 || !isfinite(multiplier) || !isfinite(constant) ||
        (!second && attribute!=7 && attribute!=8)) stop("Invalid layout constraint",selector);
    if (attribute==11 || attribute==12 || (second && (other==11 || other==12))) stop("Baseline layout is not yet supported",selector);
    if (relation && (second || (attribute!=7 && attribute!=8))) stop("Unsupported inequality between layout items",selector);
    struct LayoutConstraint *constraint=(void *)host_allocate(cls);
    *constraint=(struct LayoutConstraint){.isa=cls,.first=first,.second=second,.attribute=attribute,.other_attribute=second ? other : 0,.relation=relation,.multiplier=multiplier,.constant=constant,.priority=1000};
    return (Word)constraint;
}
static Word layout_root(Word object) { while (view_state(object)->parent) object=view_state(object)->parent; return object; }
static void view_layout_deliver(void *context) {
    Word object=(Word)context; view_state(object)->layout_pending=0; view_layout_if_needed(object,"layoutIfNeeded");
}
static void view_request_layout(Word object,const char *selector) {
    Word root=layout_root(object); struct ViewState *state=view_state(root);
    if (!state->layout_managed && strcmp(selector,"setNeedsLayout") && strcmp(selector,"setNeedsUpdateConstraints")) return;
    state->needs_layout=1;
    view_state(object)->needs_layout=1;
    if (!state->layout_pending && !state->layout_running) { state->layout_pending=1; schedule_main_function(view_layout_deliver,(void *)root); }
}
static Word constraint_common_owner(struct LayoutConstraint *constraint) {
    if (!constraint->second) return constraint->first;
    for (Word a=constraint->first;a;a=view_state(a)->parent)
        for (Word b=constraint->second;b;b=view_state(b)->parent) if (a==b) return a;
    stop("Layout items have no common ancestor","NSLayoutConstraint"); return 0;
}
static void view_add_constraint(Word object,const char *selector,Word value) {
    if (!value || !class_inherits(read_word(value),external_class("_OBJC_CLASS_$_NSLayoutConstraint"))) stop("Invalid layout constraint",selector);
    struct LayoutConstraint *constraint=(void *)value;
    if (!view_descendant(constraint->first,selector,object) || (constraint->second && !view_descendant(constraint->second,selector,object))) stop("Layout owner is not an ancestor",selector);
    if (constraint->active && constraint->owner==object) return;
    if (constraint->active) sequence_remove((void *)view_state(constraint->owner)->constraints,selector,value);
    struct ViewState *owner=view_state(object);
    if (!owner->constraints) owner->constraints=sequence_empty(external_class("_OBJC_CLASS_$_NSMutableArray"));
    sequence_add((void *)owner->constraints,selector,value); constraint->owner=object; constraint->active=1;
    view_state(layout_root(object))->layout_managed=1; view_request_layout(object,selector);
}
static void view_remove_constraint(Word object,const char *selector,Word value) {
    struct ViewState *owner=view_state(object);
    if (!owner->constraints || !value || !sequence_contains((void *)owner->constraints,selector,value)) return;
    sequence_remove((void *)owner->constraints,selector,value); ((struct LayoutConstraint *)value)->active=0; ((struct LayoutConstraint *)value)->owner=0;
    view_request_layout(object,selector);
}
static void view_constraints_batch(Word object,const char *selector,struct Sequence *constraints) {
    if (!constraints) return;
    int remove=!strcmp(selector,"removeConstraints:");
    for (size_t i=0;i<constraints->count;++i) if (remove) view_remove_constraint(object,selector,constraints->items[i]); else view_add_constraint(object,selector,constraints->items[i]);
}
static void constraint_set_active(struct LayoutConstraint *constraint,const char *selector,Word active) {
    if (!constraint || !class_inherits(constraint->isa,external_class("_OBJC_CLASS_$_NSLayoutConstraint"))) stop("Invalid layout constraint",selector);
    if (!!active==constraint->active) return;
    if (active) view_add_constraint(constraint_common_owner(constraint),selector,(Word)constraint);
    else view_remove_constraint(constraint->owner,selector,(Word)constraint);
}
static void constraints_set_active(Word cls,const char *selector,struct Sequence *constraints) {
    if (constraints) for (size_t i=0;i<constraints->count;++i) constraint_set_active((void *)constraints->items[i],selector,!strcmp(selector,"activateConstraints:"));
}
static double constraint_constant(struct LayoutConstraint *constraint) { return constraint->constant; }
static void constraint_set_constant(struct LayoutConstraint *constraint,const char *selector,double value) {
    if (!isfinite(value)) stop("Invalid layout constant",selector);
    if (constraint->constant==value) return;
    constraint->constant=value; if (constraint->active) view_request_layout(constraint->owner,selector);
}
static float constraint_priority(struct LayoutConstraint *constraint) { return constraint->priority; }
static void constraint_set_priority(struct LayoutConstraint *constraint,const char *selector,float value) {
    if (!isfinite(value) || value<1 || value>1000) stop("Invalid layout priority",selector);
    if (constraint->priority==value) return;
    constraint->priority=value; if (constraint->active) view_request_layout(constraint->owner,selector);
}
static Word constraint_property(struct LayoutConstraint *constraint,const char *selector) {
    if (!strcmp(selector,"firstItem")) return constraint->first;
    if (!strcmp(selector,"secondItem")) return constraint->second;
    if (!strcmp(selector,"firstAttribute")) return constraint->attribute;
    if (!strcmp(selector,"secondAttribute")) return constraint->other_attribute;
    if (!strcmp(selector,"relation")) return constraint->relation;
    if (!strcmp(selector,"isActive")) return constraint->active;
    return constraint->identifier;
}
static double constraint_multiplier(struct LayoutConstraint *constraint) { return constraint->multiplier; }
static void constraint_set_identifier(struct LayoutConstraint *constraint,const char *selector,Word value) { constraint->identifier=value ? text_copy(value,"copy") : 0; }
static void layout_collect(Word object,struct Sequence *constraints,struct Sequence *objects) {
    struct ViewState *state=view_state(object); struct Sequence *installed=(void *)state->constraints,*children=(void *)state->children;
    for (size_t i=0;installed && i<installed->count;++i) {
        struct LayoutConstraint *constraint=(void *)installed->items[i]; if (!constraint->active) continue;
        sequence_add(constraints,"UILayout",(Word)constraint);
        Word items[]={constraint->first,constraint->second};
        for (unsigned j=0;j<2;++j) {
            Word chain[128]; unsigned count=0;
            for (Word item=items[j];item;item=view_state(item)->parent) { if (count==COUNT(chain)) stop("Layout ancestry exceeds supported size","UILayout"); chain[count++]=item; }
            while (count) { Word item=chain[--count]; if (!sequence_contains(objects,"UILayout",item)) sequence_add(objects,"UILayout",item); }
        }
    }
    for (size_t i=0;i<children->count;++i) layout_collect(children->items[i],constraints,objects);
}
static void layout_attribute(double *row,struct Sequence *objects,Word object,int attribute,double factor,int rtl,unsigned variables) {
    if (!object) return;
    size_t index=sequence_index(objects,"indexOfObjectIdenticalTo:",object);
    if (index==INT64_MAX) stop("Unlisted layout item","UILayout");
    struct GuestRect margins=view_state(object)->margins;
    double offset=0; int horizontal=attribute==1 || attribute==2 || attribute==5 || attribute==6 || attribute==9 || attribute==13 || attribute==14 || attribute==17 || attribute==18 || attribute==19;
    if (attribute==5) attribute=rtl ? 2 : 1; if (attribute==6) attribute=rtl ? 1 : 2;
    if (attribute==17) attribute=rtl ? 14 : 13; if (attribute==18) attribute=rtl ? 13 : 14;
    if (attribute==13) { attribute=1; offset=margins.y; } else if (attribute==14) { attribute=2; offset=-margins.height; }
    else if (attribute==15) { attribute=3; offset=margins.x; } else if (attribute==16) { attribute=4; offset=-margins.width; }
    else if (attribute==19) { attribute=9; offset=(margins.y-margins.height)*0.5; } else if (attribute==20) { attribute=10; offset=(margins.x-margins.width)*0.5; }
    if (attribute==11 || attribute==12) { attribute=4; }
    scene_attribute(row,index,attribute,factor); row[variables]-=offset*factor;
    if (attribute!=7 && attribute!=8) for (Word parent=view_state(object)->parent;parent;parent=view_state(parent)->parent) {
        size_t p=sequence_index(objects,"indexOfObjectIdenticalTo:",parent); if (p==INT64_MAX) break;
        row[p*4+(horizontal ? 0 : 1)]+=factor;
        row[variables]+=factor*(horizontal ? view_state(parent)->bounds.x : view_state(parent)->bounds.y);
    }
}
static int layout_priority_order(const void *a,const void *b) {
    const struct LayoutConstraint *x=*(const struct LayoutConstraint *const *)a,*y=*(const struct LayoutConstraint *const *)b;
    return x->priority>y->priority ? -1 : x->priority<y->priority ? 1 : 0;
}
static void layout_values(double *basis,double *values,unsigned variables) {
    for (unsigned i=variables;i>0;--i) { double *pivot=basis+(i-1)*(variables+1); values[i-1]=pivot[variables]; for (unsigned j=i;j<variables;++j) values[i-1]-=pivot[j]*values[j]; }
}
static void layout_prepare_callbacks(Word object) {
    struct ViewState *state=view_state(object); state->layout_notify|=state->needs_layout || state->needs_constraints; state->needs_layout=0;
    if (state->needs_constraints) { state->needs_constraints=0; Method update=find_implementation(object,"updateConstraints"); if (update) update(object,"updateConstraints",0,0,0,0); }
    struct Sequence *children=(void *)state->children;
    for (size_t i=0;i<children->count;++i) layout_prepare_callbacks(children->items[i]);
}
static void layout_finish_callbacks(Word object) {
    struct ViewState *state=view_state(object);
    if (state->layout_notify) { state->layout_notify=0; Method layout=find_implementation(object,"layoutSubviews"); if (layout) layout(object,"layoutSubviews",0,0,0,0); }
    struct Sequence *children=(void *)state->children;
    for (size_t i=0;i<children->count;++i) layout_finish_callbacks(children->items[i]);
}
static void view_layout_if_needed(Word object,const char *selector) {
    Word root=layout_root(object); struct ViewState *state=view_state(root);
    if (state->layout_running || !state->needs_layout) return;
    state->layout_running=1; state->layout_notify=1;
    layout_prepare_callbacks(root);
    Word controller=state->controller; Method lifecycle=controller ? find_implementation(controller,"viewWillLayoutSubviews") : NULL;
    if (lifecycle) lifecycle(controller,"viewWillLayoutSubviews",0,0,0,0);
    state->needs_layout=0;
    struct Sequence *constraints=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray")),*objects=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"));
    sequence_add(objects,selector,root); layout_collect(root,constraints,objects);
    if (objects->count>128) stop("Layout component exceeds supported size",selector);
    unsigned variables=objects->count*4; size_t cells=(size_t)variables*(variables+1);
    double *basis=calloc(cells,sizeof(double)),*required=calloc(cells,sizeof(double)),*row=calloc(variables+1,sizeof(double)),*values=calloc(variables,sizeof(double));
    double *lower=malloc(variables*sizeof(double)),*upper=malloc(variables*sizeof(double));
    struct GuestSize *intrinsic=malloc(objects->count*sizeof(*intrinsic)); const char *failure=NULL;
    if (!basis || !required || !row || !values || !lower || !upper || !intrinsic) { failure="Layout allocation failed"; goto cleanup; }
    for (size_t i=0;i<objects->count;++i) intrinsic[i]=view_intrinsic_for_layout(objects->items[i]);
#define INSERT_LAYOUT_ROW(storage,mandatory) do { if (!layout_equation(storage,variables,row,mandatory)) { failure="Inconsistent required layout equation"; goto cleanup; } } while (0)
    for (unsigned i=0;i<variables;++i) { lower[i]=-INFINITY; upper[i]=INFINITY; }
    JNIEnv *env=attached_environment(selector); static jmethodID direction;
    if (!direction) { jclass cls=(*env)->GetObjectClass(env,activity); direction=(*env)->GetMethodID(env,cls,"guestLayoutDirection","()I"); (*env)->DeleteLocalRef(env,cls); }
    int rtl=(*env)->CallIntMethod(env,activity,direction)!=0;
    double fixed[]={0,0,state->bounds.width,state->bounds.height};
    int self_sizing=0;
    for (size_t i=0;i<constraints->count;++i) { struct LayoutConstraint *constraint=(void *)constraints->items[i]; self_sizing|=constraint->first==root && !constraint->second; }
    for (unsigned i=0;i<(self_sizing ? 2U : 4U);++i) { memset(row,0,(variables+1)*sizeof(double)); row[i]=1; row[variables]=fixed[i]; INSERT_LAYOUT_ROW(required,1); }
    qsort(constraints->items,constraints->count,sizeof(Word),layout_priority_order);
    for (size_t i=0;i<constraints->count;++i) {
        struct LayoutConstraint *constraint=(void *)constraints->items[i];
        if (constraint->relation) {
            if (constraint->priority!=1000) { failure="Optional layout inequalities are not yet supported"; goto cleanup; }
            size_t index=sequence_index(objects,"indexOfObjectIdenticalTo:",constraint->first)*4+(constraint->attribute==7 ? 2 : 3);
            if (constraint->relation>0) lower[index]=fmax(lower[index],constraint->constant); else upper[index]=fmin(upper[index],constraint->constant);
        } else if (constraint->priority==1000) {
            memset(row,0,(variables+1)*sizeof(double)); row[variables]=constraint->constant;
            layout_attribute(row,objects,constraint->first,constraint->attribute,1,rtl,variables);
            layout_attribute(row,objects,constraint->second,constraint->other_attribute,-constraint->multiplier,rtl,variables);
            INSERT_LAYOUT_ROW(required,1);
        }
    }
    for (unsigned pass=0;pass<=variables;++pass) {
        memcpy(basis,required,cells*sizeof(double));
        for (size_t i=0;i<constraints->count;++i) {
            struct LayoutConstraint *constraint=(void *)constraints->items[i]; if (constraint->relation || constraint->priority==1000) continue;
            memset(row,0,(variables+1)*sizeof(double)); row[variables]=constraint->constant;
            layout_attribute(row,objects,constraint->first,constraint->attribute,1,rtl,variables);
            layout_attribute(row,objects,constraint->second,constraint->other_attribute,-constraint->multiplier,rtl,variables);
            INSERT_LAYOUT_ROW(basis,0);
        }
        for (size_t i=0;i<objects->count;++i) {
            struct ViewState *v=view_state(objects->items[i]);
            double fallback[]={i ? v->frame.x : 0,i ? v->frame.y : 0,intrinsic[i].width>=0 && !v->translates ? intrinsic[i].width : v->frame.width,intrinsic[i].height>=0 && !v->translates ? intrinsic[i].height : v->frame.height};
            for (unsigned j=0;j<4;++j) { memset(row,0,(variables+1)*sizeof(double)); row[i*4+j]=1; row[variables]=fallback[j]; INSERT_LAYOUT_ROW(basis,0); }
        }
        layout_values(basis,values,variables); int violated=0;
        for (unsigned i=0;i<variables;++i) {
            if (lower[i]>upper[i]) { failure="Inconsistent required layout bounds"; goto cleanup; }
            double bounded=fmin(upper[i],fmax(lower[i],values[i]));
            if (fabs(bounded-values[i])>1e-7) { memset(row,0,(variables+1)*sizeof(double)); row[i]=1; row[variables]=bounded; INSERT_LAYOUT_ROW(required,1); violated=1; }
        }
        if (!violated) break; if (pass==variables) { failure="Required layout bounds cannot be satisfied"; goto cleanup; }
    }
    if (self_sizing) {
        double width=state->bounds.width,height=state->bounds.height;
        state->frame.width=state->bounds.width=fmax(0,values[2]); state->frame.height=state->bounds.height=fmax(0,values[3]); view_resize_children(state,width,height);
    }
    for (size_t i=1;i<objects->count;++i) {
        struct ViewState *v=view_state(objects->items[i]); double width=v->bounds.width,height=v->bounds.height;
        struct GuestRect frame={values[i*4],values[i*4+1],fmax(0,values[i*4+2]),fmax(0,values[i*4+3])}; v->layout_notify|=memcmp(&v->frame,&frame,sizeof(frame))!=0;
        v->frame=frame; v->bounds.width=v->frame.width; v->bounds.height=v->frame.height;
        view_resize_children(v,width,height);
    }
cleanup:
#undef INSERT_LAYOUT_ROW
    free(basis); free(required); free(row); free(values); free(lower); free(upper); free(intrinsic);
    free(constraints->items); free(constraints); free(objects->items); free(objects);
    if (failure) { state->layout_running=0; stop(failure,selector); }
    layout_finish_callbacks(root);
    lifecycle=controller ? find_implementation(controller,"viewDidLayoutSubviews") : NULL; if (lifecycle) lifecycle(controller,"viewDidLayoutSubviews",0,0,0,0);
    view_sync_subtree(root); state->layout_running=0;
    if (state->needs_layout) view_request_layout(root,selector);
}
static void view_set_translates(Word object,const char *selector,Word value) { struct ViewState *state=view_state(object); if (state->translates==!!value) return; state->translates=value!=0; view_request_layout(object,selector); }
static Word view_get_translates(Word object) { return view_state(object)->translates; }
static void view_set_margins(Word object,const char *selector,struct GuestRect value) { view_state(object)->margins=value; view_request_layout(object,selector); }
static struct GuestRect view_get_margins(Word object) { return view_state(object)->margins; }
static void view_needs_constraints(Word object,const char *selector) { view_state(object)->needs_constraints=1; view_request_layout(object,selector); }
static Word view_get_constraints(Word object) { Word value=view_state(object)->constraints; return value ? sequence_copy((void *)value,"copy") : sequence_empty(external_class("_OBJC_CLASS_$_NSArray")); }
struct VisualLayout { const char *cursor,*selector; struct GuestMap *metrics,*views; struct Sequence *constraints,*items; Word cls,owner; int vertical,rtl; };
static void visual_skip(struct VisualLayout *layout) { while (*layout->cursor==' ' || *layout->cursor=='\t') ++layout->cursor; }
static Word visual_lookup(struct GuestMap *map,const char *name,const char *selector) { return map ? map_get(map,selector,copy_string(name)) : 0; }
static void visual_name(struct VisualLayout *layout,char *output,size_t capacity) {
    visual_skip(layout); size_t count=0;
    while ((*layout->cursor>='a' && *layout->cursor<='z') || (*layout->cursor>='A' && *layout->cursor<='Z') || (*layout->cursor>='0' && *layout->cursor<='9') || *layout->cursor=='_') {
        if (count+1>=capacity) stop("Visual format identifier is too long",layout->selector);
        output[count++]=*layout->cursor++;
    }
    output[count]=0; if (!count) stop("Expected visual format identifier",layout->selector);
}
static double visual_number(struct VisualLayout *layout) {
    visual_skip(layout); char *end=NULL; double value=strtod(layout->cursor,&end);
    if (end!=layout->cursor) { layout->cursor=end; if (!isfinite(value)) stop("Invalid visual format number",layout->selector); return value; }
    char name[256]; visual_name(layout,name,sizeof(name)); Word metric=visual_lookup(layout->metrics,name,layout->selector);
    if (!metric) stop("Missing visual format metric",name); return numeric_double((void *)metric);
}
static void visual_constraint(struct VisualLayout *layout,Word first,int attribute,int relation,Word second,int other,double constant,float priority) {
    Word value=constraint_create(layout->cls,layout->selector,first,attribute,relation,second,other,1,constant);
    ((struct LayoutConstraint *)value)->priority=priority; sequence_add(layout->constraints,layout->selector,value);
}
static void visual_predicate(struct VisualLayout *layout,Word item) {
    visual_skip(layout); int relation=0;
    if (!strncmp(layout->cursor,">=",2)) { relation=1; layout->cursor+=2; }
    else if (!strncmp(layout->cursor,"<=",2)) { relation=-1; layout->cursor+=2; }
    else if (!strncmp(layout->cursor,"==",2)) layout->cursor+=2;
    visual_skip(layout); const char *start=layout->cursor; Word second=0; double constant=0; float priority=1000;
    if ((*start>='a' && *start<='z') || (*start>='A' && *start<='Z') || *start=='_') {
        char name[256]; visual_name(layout,name,sizeof(name)); second=visual_lookup(layout->views,name,layout->selector);
        if (!second) { layout->cursor=start; constant=visual_number(layout); }
    } else constant=visual_number(layout);
    visual_skip(layout); if (*layout->cursor=='@') { ++layout->cursor; priority=visual_number(layout); }
    if (priority<1 || priority>1000) stop("Invalid visual format priority",layout->selector);
    visual_constraint(layout,item,layout->vertical ? 8 : 7,relation,second,layout->vertical ? 8 : 7,constant,priority);
}
static Word visual_item(struct VisualLayout *layout) {
    visual_skip(layout);
    if (*layout->cursor=='|') { ++layout->cursor; if (!layout->owner) stop("Visual format requires a common superview",layout->selector); return layout->owner; }
    if (*layout->cursor++!='[') stop("Expected visual format view",layout->selector);
    char name[256]; visual_name(layout,name,sizeof(name)); Word item=visual_lookup(layout->views,name,layout->selector);
    if (!item) stop("Missing visual format view",name);
    sequence_add(layout->items,layout->selector,item); visual_skip(layout);
    if (*layout->cursor=='(') {
        ++layout->cursor;
        for (;;) { visual_predicate(layout,item); visual_skip(layout); if (*layout->cursor!=',') break; ++layout->cursor; }
        if (*layout->cursor++!=')') stop("Unclosed visual format predicate",layout->selector);
    }
    visual_skip(layout); if (*layout->cursor++!=']') stop("Unclosed visual format view",layout->selector); return item;
}
static Word constraint_visual_format(Word cls,const char *selector,Word format,Word options,struct GuestMap *metrics,struct GuestMap *views) {
    if (!format || !views) stop("Missing visual format arguments",selector);
    unsigned align=(unsigned)options&0x7fe,mode=(unsigned)options&0x30000;
    if (options&~(Word)0x307fe || mode==0x30000) stop("Unsupported visual format options",selector);
    struct VisualLayout layout={.cursor=string_bytes(format,selector),.selector=selector,.metrics=metrics,.views=views,.cls=cls,
        .constraints=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray")),.items=(void *)sequence_empty(external_class("_OBJC_CLASS_$_NSArray"))};
    int first=1;
    for (const char *scan=layout.cursor;*scan;++scan) if (*scan=='[') {
        const char *saved=layout.cursor; layout.cursor=scan+1; char name[256]; visual_name(&layout,name,sizeof(name)); layout.cursor=saved;
        Word item=visual_lookup(views,name,selector); if (!item) stop("Missing visual format view",name);
        if (first) { layout.owner=view_state(item)->parent; first=0; }
        else if (layout.owner) {
            Word common=0;
            for (Word a=layout.owner;a && !common;a=view_state(a)->parent) for (Word b=item;b;b=view_state(b)->parent) if (a==b) { common=a; break; }
            layout.owner=common;
        }
    }
    visual_skip(&layout);
    if (layout.cursor[0]=='H' && layout.cursor[1]==':') layout.cursor+=2;
    else if (layout.cursor[0]=='V' && layout.cursor[1]==':') { layout.vertical=1; layout.cursor+=2; }
    JNIEnv *env=attached_environment(selector); jclass type=(*env)->GetObjectClass(env,activity); jmethodID direction=(*env)->GetMethodID(env,type,"guestLayoutDirection","()I");
    layout.rtl=!layout.vertical && (mode==0x20000 || (!mode && (*env)->CallIntMethod(env,activity,direction)!=0)); (*env)->DeleteLocalRef(env,type);
    Word previous=visual_item(&layout); int leading=layout.vertical ? 3 : layout.rtl ? 2 : 1,trailing=layout.vertical ? 4 : layout.rtl ? 1 : 2;
    visual_skip(&layout);
    while (*layout.cursor) {
        double spacing=0; visual_skip(&layout);
        if (*layout.cursor=='-') {
            ++layout.cursor; visual_skip(&layout); spacing=8;
            if (*layout.cursor!='[' && *layout.cursor!='|') {
                int parenthesized=*layout.cursor=='('; if (parenthesized) ++layout.cursor;
                spacing=visual_number(&layout); visual_skip(&layout);
                if (parenthesized && *layout.cursor++!=')') stop("Unclosed visual format spacing",selector);
                if (*layout.cursor++!='-') stop("Expected visual format spacing separator",selector);
            }
        }
        Word next=visual_item(&layout);
        double constant=spacing*(layout.rtl ? -1 : 1);
        if (next==layout.owner) visual_constraint(&layout,previous,trailing,0,next,trailing,-constant,1000);
        else visual_constraint(&layout,next,leading,0,previous,previous==layout.owner ? leading : trailing,constant,1000);
        previous=next; visual_skip(&layout);
    }
    if (layout.items->count>1) for (unsigned attribute=1;attribute<=10;++attribute) if (align&(1U<<attribute))
        for (size_t i=1;i<layout.items->count;++i) visual_constraint(&layout,layout.items->items[i],attribute,0,layout.items->items[0],attribute,0,1000);
    free(layout.items->items); free(layout.items); return (Word)layout.constraints;
}
static void scene_graph_attribute(double *row,plist_t views,int node,int attribute,double factor){
    if(node<0)return;unsigned count=plist_array_get_size(views);if((unsigned)node>=count)stop("Invalid archived layout item","Storyboard");scene_attribute(row,node,attribute,factor);
    int axis=(attribute==1 || attribute==2 || attribute==5 || attribute==6 || attribute==9) ? 0 : (attribute==3 || attribute==4 || attribute==10) ? 1 : -1;if(axis<0)return;
    for(unsigned depth=0;depth<count;++depth){int parent=(int)scene_number(plist_array_get_item(views,node),"parent",-1);if(parent<0)return;if((unsigned)parent>=(unsigned)node)stop("Invalid archived view hierarchy","Storyboard");row[parent*4+axis]+=factor;node=parent;}
}
static void load_storyboard_view(Word controller,Word graph) {
    plist_t scene=NULL;
    const char *encoded=string_bytes(graph,"Storyboard view");if (plist_from_json(encoded,strlen(encoded),&scene)!=PLIST_ERR_SUCCESS) stop("Invalid generated storyboard","UIKit");
    plist_t views=plist_dict_get_item(scene,"views"),constraints=plist_dict_get_item(scene,"constraints");
    unsigned count=plist_array_get_size(views),variables=count*4;
    if (!count || count>128) stop("Archived layout exceeds supported size","UIKit");
    Word *objects=calloc(count,sizeof(*objects));if(!objects)stop("Allocation failed","Storyboard views");
    double *basis=calloc(variables*(variables+1),sizeof(double)),*row=calloc(variables+1,sizeof(double)),*values=calloc(variables,sizeof(double));
    if (!basis || !row || !values) stop("Allocation failed","Storyboard constraints");
    struct GuestRect screen=screen_bounds();
    double root_values[]={0,0,screen.width,screen.height};
    for (unsigned i=0;i<4;++i) { memset(row,0,(variables+1)*sizeof(double)); row[i]=1; row[variables]=root_values[i]; scene_equation(basis,variables,row,1); }
    for (int priority=1000;priority>=1;--priority) for (unsigned i=0;i<plist_array_get_size(constraints);++i) {
        plist_t constraint=plist_array_get_item(constraints,i);
        if ((int)scene_number(constraint,"priority",1000)!=priority) continue;
        if (scene_number(constraint,"relation",0)!=0) stop("Unsupported startup constraint relation","Storyboard");
        memset(row,0,(variables+1)*sizeof(double));
        scene_graph_attribute(row,views,(int)scene_number(constraint,"first",-1),(int)scene_number(constraint,"attribute",0),1);
        scene_graph_attribute(row,views,(int)scene_number(constraint,"second",-1),(int)scene_number(constraint,"otherAttribute",0),-scene_number(constraint,"multiplier",1));
        row[variables]=scene_number(constraint,"constant",0);
        scene_equation(basis,variables,row,priority==1000);
    }
    for (unsigned i=0;i<count;++i) {
        plist_t node=plist_array_get_item(views,i),frame=plist_dict_get_item(node,"frame");
        for (unsigned j=0;j<4;++j) {
            double fallback=0; plist_t item=plist_array_get_item(frame,j);
            if (plist_get_node_type(item)==PLIST_REAL) plist_get_real_val(item,&fallback);
            else { int64_t integer=0; plist_get_int_val(item,&integer); fallback=(double)integer; }
            memset(row,0,(variables+1)*sizeof(double)); row[i*4+j]=1; row[variables]=fallback;
            scene_equation(basis,variables,row,0);
        }
    }
    for (unsigned i=variables;i>0;--i) {
        double *pivot=basis+(i-1)*(variables+1);
        values[i-1]=pivot[variables];
        for (unsigned j=i;j<variables;++j) values[i-1]-=pivot[j]*values[j];
    }
    for (unsigned i=0;i<count;++i) {
        plist_t node=plist_array_get_item(views,i);
        const char *name=plist_get_string_ptr(plist_dict_get_item(node,"class"),NULL);
        Word cls=class_from_name(name);
        if (!cls) stop("Unknown storyboard view class",name);
        objects[i]=host_allocate(cls);
        struct GuestRect frame={values[i*4],values[i*4+1],values[i*4+2],values[i*4+3]};
        view_init_frame(objects[i],"Storyboard",frame);
        struct ViewState *state=view_state(objects[i]);
        state->tag=(long)scene_number(node,"tag",0); state->hidden=(int)scene_number(node,"hidden",0); state->alpha=scene_number(node,"alpha",1);
        state->interaction=(int)scene_number(node,"interaction",1); state->content_mode=(long)scene_number(node,"contentMode",0); state->autoresizing=(long)scene_number(node,"autoresizing",0);
        state->image_name=copy_string(plist_get_string_ptr(plist_dict_get_item(node,"image"),NULL));
        plist_t background=plist_dict_get_item(node,"background"); double rgba[4];
        if (plist_get_node_type(background)!=PLIST_ARRAY) background=NULL;
        for (unsigned j=0;j<4;++j) { rgba[j]=0; if (background) plist_get_real_val(plist_array_get_item(background,j),rgba+j); }
        ui_view_load_content(objects[i],string_bytes(state->image_name,"Storyboard image"),background ? rgba : NULL);
        if(class_inherits(cls,external_class("_OBJC_CLASS_$_UILabel")))ui_view_load_label(objects[i],node);
        int parent=(int)scene_number(node,"parent",-1);
        if (parent>=0) {
            if ((unsigned)parent>=i) stop("Invalid archived view hierarchy","Storyboard");
            view_add(objects[parent],"Storyboard",objects[i]);
        }
    }
    view_state(controller)->view=objects[0];
    LOG("Original storyboard restored: %u views, %u constraints, original images loaded",count,plist_array_get_size(constraints));
    free(objects);free(basis); free(row); free(values); plist_free(scene);
}
static void application_activate(void *unused) { application_lifecycle_start(); }
static void ui_application_start(Word principal,Word delegate_name){
    application.isa=external_class("_OBJC_CLASS_$_UIApplication");application_object=(Word)&application;
    if(principal){
        Word cls=class_from_string(principal);if(!cls || !class_inherits(cls,application.isa))stop("Invalid application principal","UIApplicationMain");
        application_object=host_allocate(cls);Method initializer=find_implementation(application_object,"init");
        if(!initializer)stop("Missing application initializer","UIApplicationMain");
        application_object=initializer(application_object,"init",0,0,0,0);
        if(!application_object)stop("Application initialization failed","UIApplicationMain");
    }
    if(delegate_name){Word cls=class_from_string(delegate_name);if(!cls)stop("Unknown application delegate","UIApplicationMain");app_delegate=new_object(cls);}
    Word managed=0;
    if(*GUEST_MAIN_STORYBOARD){
        Word controller=storyboard_startup(copy_string(GUEST_MAIN_STORYBOARD));
        if(controller){
            main_window.isa=external_class("_OBJC_CLASS_$_UIWindow");managed=(Word)&main_window;
            view_init_frame(managed,"Main storyboard window",screen_bounds());window_set_controller(managed,"setRootViewController:",controller);window_make_key(managed,"Main storyboard window");
            Method setter=find_implementation(app_delegate,"setWindow:");if(setter)setter(app_delegate,"setWindow:",managed,0,0,0);
        }
    }
    const char *callbacks[]={"application:willFinishLaunchingWithOptions:","application:didFinishLaunchingWithOptions:"};
    for(unsigned i=0;i<COUNT(callbacks);++i){Method callback=find_implementation(app_delegate,callbacks[i]);if(callback){Word result=callback(app_delegate,callbacks[i],application_object,0,0,0);LOG("Application launch callback %s returned %u",callbacks[i],(unsigned)(result&255));}}
    window_lifecycle_ready=1;platform_notification("UIApplicationDidFinishLaunchingNotification",application_object,0);
    if(managed && key_window==managed)window_make_visible(managed,"Main storyboard presentation");
    for(struct ViewState *state=view_states;state;state=state->next)if(!state->hidden && class_inherits(read_word(state->object),external_class("_OBJC_CLASS_$_UIWindow")))window_schedule_presentation(state->object);
    schedule_main_function(application_activate,NULL);
}

static Word view_host_geometry(Word object,float metrics[11],int options[3]) {
    struct ViewState *view=view_state(object);double scale=screen_scale(),x=view->frame.x,y=view->frame.y,alpha=view->alpha;int visible=!view->hidden,enabled=view->interaction;
    Word container=view->host_container ? object : 0;
    if(container)x=y=0;
    for(Word parent=container ? 0 : view->parent;parent;parent=view_state(parent)->parent){struct ViewState *ancestor=view_state(parent);alpha*=ancestor->alpha;visible&=!ancestor->hidden;enabled&=ancestor->interaction;if(ancestor->host_container){x-=ancestor->bounds.x;y-=ancestor->bounds.y;container=parent;break;}x+=ancestor->frame.x-ancestor->bounds.x;y+=ancestor->frame.y-ancestor->bounds.y;}
    struct GuestRect clip=container ? (struct GuestRect){0,0,view_state(container)->bounds.width,view_state(container)->bounds.height} : screen_bounds();double px=x-view->frame.x,py=y-view->frame.y;
    for(Word parent=container==object ? 0 : view->parent;parent;parent=view_state(parent)->parent){struct ViewState *ancestor=view_state(parent);px+=ancestor->bounds.x;py+=ancestor->bounds.y;
        if(parent==container)break;
        if(ancestor->clips){double right=fmin(clip.x+clip.width,px+ancestor->frame.width),bottom=fmin(clip.y+clip.height,py+ancestor->frame.height);clip.x=fmax(clip.x,px);clip.y=fmax(clip.y,py);clip.width=fmax(0,right-clip.x);clip.height=fmax(0,bottom-clip.y);}
        px-=ancestor->frame.x;py-=ancestor->frame.y;
    }
    double left=fmax(x,clip.x),top=fmax(y,clip.y),right=fmin(x+view->frame.width,clip.x+clip.width),bottom=fmin(y+view->frame.height,clip.y+clip.height);
    float values[]={(float)(x*scale),(float)(y*scale),(float)(view->frame.width*scale),(float)(view->frame.height*scale),(float)alpha,(float)(view->layer ? view_state(view->layer)->z : 0),(float)view_window_level(object),(float)((left-x)*scale),(float)((top-y)*scale),(float)(fmax(0,right-left)*scale),(float)(fmax(0,bottom-top)*scale)};
    memcpy(metrics,values,sizeof(values));options[0]=visible && alpha>0 && right>left && bottom>top;options[1]=view_window(object)!=0;options[2]=enabled;
    return container ? view_state(container)->host_container : 0;
}
static void ui_plane_values(struct GuestTransform3D matrix,double scale,float values[9]) {
    float plane[]={(float)matrix.m[0],(float)matrix.m[4],(float)(matrix.m[12]*scale),(float)matrix.m[1],(float)matrix.m[5],(float)(matrix.m[13]*scale),(float)(matrix.m[3]/scale),(float)(matrix.m[7]/scale),(float)matrix.m[15]};memcpy(values,plane,sizeof(plane));
}
static void view_presentation(Word object,float output[VIEW_PRESENTATION_FIELDS]) {
    struct ViewState *view=view_state(object);double scale=screen_scale();int raw=layer_object(object) && !view->view;Word owner=raw ? layer_owner(object) : object,container=owner ? view_container(owner) : 0;int options[3]={0};
    memset(output,0,VIEW_PRESENTATION_FIELDS*sizeof(float));struct GuestTransform3D matrix;
    if(!raw){view_host_geometry(object,output,options);matrix=ui_world_matrix(object,container);output[2]=view->bounds.width*scale;output[3]=view->bounds.height*scale;output[20]=options[1];output[22]=options[2];
        int visible=1;for(Word p=object;p;p=view_state(p)->parent){visible&=!view_state(p)->hidden;if(p==container)break;}output[21]=visible;}
    else {matrix=layer_world_matrix(object,container);float parent[VIEW_PRESENTATION_FIELDS]={0};if(owner)view_presentation(owner,parent);output[2]=view->bounds.width*scale;output[3]=view->bounds.height*scale;output[4]=owner ? parent[4] : 1;output[5]=owner ? parent[5] : 0;output[6]=owner ? parent[6] : 0;output[20]=owner ? parent[20] : 0;output[21]=owner ? parent[21] : 0;
        for(Word p=object;p && !view_state(p)->view;p=view_state(p)->parent){output[4]*=view_state(p)->alpha;output[5]+=view_state(p)->z;output[21]*=!view_state(p)->hidden;}output[9]=output[2];output[10]=output[3];struct LayerState *layer=view->composition;output[22]=layer && (layer->background || layer->border_width>0 || layer->colors || layer->contents);}
    ui_plane_values(matrix,scale,output+11);
    struct GuestPoint anchor=raw ? layer_state(object)->anchor : view_anchor(object);output[23]=anchor.x;output[24]=anchor.y;
    struct GuestRect visible=ui_project_rect(matrix,(struct GuestRect){0,0,view->bounds.width,view->bounds.height}),limit=container ? (struct GuestRect){0,0,view_state(container)->bounds.width,view_state(container)->bounds.height} : screen_bounds();
    output[21]*=isfinite(visible.x) && isfinite(visible.y) && visible.x+visible.width>limit.x && visible.y+visible.height>limit.y && visible.x<limit.x+limit.width && visible.y<limit.y+limit.height;
}
static unsigned view_clip_planes(Word object,float clips[64*12],Word owners[64]) {
    int raw=layer_object(object) && !view_state(object)->view;Word owner=raw ? layer_owner(object) : object,container=owner ? view_container(owner) : 0;double scale=screen_scale();unsigned count=0;
    for(Word p=raw ? object : 0;p && !view_state(p)->view && count<64;p=view_state(p)->parent)if(view_state(p)->clips){struct ViewState *view=view_state(p);owners[count]=p;float *clip=clips+count++*12;clip[0]=view->bounds.width*scale;clip[1]=view->bounds.height*scale;clip[2]=view->corner*scale;ui_plane_values(layer_world_matrix(p,container),scale,clip+3);}
    for(Word p=owner;p && count<64;p=view_state(p)->parent){struct ViewState *view=view_state(p),*layer=view->layer ? view_state(view->layer) : NULL;
        if(view->clips || (layer && layer->clips)){owners[count]=p;float *clip=clips+count++*12;clip[0]=view->bounds.width*scale;clip[1]=view->bounds.height*scale;clip[2]=layer ? layer->corner*scale : 0;ui_plane_values(ui_world_matrix(p,container),scale,clip+3);}if(p==container)break;}
    return count;
}
static void ui_view_composition_sync(Word object) {
    struct ViewState *view=view_state(object);int raw=layer_object(object) && !view->view;if(!raw && !ui_view_rendered(object) && !view->clips && !(view->layer && view_state(view->layer)->clips))return;float values[VIEW_PRESENTATION_FIELDS],clips[64*12];Word clip_owners[64];view_presentation(object,values);unsigned count=view_clip_planes(object,clips,clip_owners);Word owner=raw ? layer_owner(object) : object,container=owner ? view_container(owner) : 0,host=container ? view_state(container)->host_container : 0;
    if(view->composition_synced && view->composition_owner==host && !memcmp(values,view->presentation,sizeof(values)) && view->composition_clip_count==count && (!count || (!memcmp(clips,view->composition_clips,count*12*sizeof(float)) && !memcmp(clip_owners,view->composition_clip_owners,count*sizeof(Word)))))return;
    if(!values[20] && (!view->composition_synced || !view->presentation[20]))return;
    if(layer_object(object) && view->composition && values[20]!=view->presentation[20])layer_state(object)->dirty=1;
    if(view->layer && view_state(view->layer)->composition && values[20]!=view->presentation[20]){struct LayerState *layer=layer_state(view->layer);layer->dirty=1;layer->published=0;}
    float *copy=count ? realloc(view->composition_clips,count*12*sizeof(float)) : NULL;if(count && !copy)stop("Allocation failed","View clipping");if(!count)free(view->composition_clips);else memcpy(copy,clips,count*12*sizeof(float));view->composition_clips=copy;view->composition_clip_count=count;
    Word *owner_copy=count ? realloc(view->composition_clip_owners,count*sizeof(Word)) : NULL;if(count && !owner_copy)stop("Allocation failed","View clip owners");if(!count)free(view->composition_clip_owners);else memcpy(owner_copy,clip_owners,count*sizeof(Word));view->composition_clip_owners=owner_copy;
    view->composition_synced=1;view->composition_owner=host;memcpy(view->presentation,values,sizeof(values));
    JNIEnv *env=attached_environment("View composition");static jmethodID method;if(!method){jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"syncGuestComposition","(JJZ[F[F[J)V");(*env)->DeleteLocalRef(env,cls);}
    jfloatArray presentation=(*env)->NewFloatArray(env,VIEW_PRESENTATION_FIELDS),bounds=count ? (*env)->NewFloatArray(env,count*12) : NULL;(*env)->SetFloatArrayRegion(env,presentation,0,VIEW_PRESENTATION_FIELDS,values);if(bounds)(*env)->SetFloatArrayRegion(env,bounds,0,count*12,clips);
    jlongArray owners=count ? (*env)->NewLongArray(env,count) : NULL;if(owners)(*env)->SetLongArrayRegion(env,owners,0,count,(const jlong *)clip_owners);
    (*env)->CallVoidMethod(env,activity,method,(jlong)object,(jlong)host,(jboolean)raw,presentation,bounds,owners);(*env)->DeleteLocalRef(env,presentation);if(bounds)(*env)->DeleteLocalRef(env,bounds);if(owners)(*env)->DeleteLocalRef(env,owners);if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Android view composition failed","UIView");}
}
static void ui_layer_publish(Word object) {
    struct ViewState *view=view_state(object);struct LayerState *layer=view->composition;if(!layer)return;Word owner=view->view,background=owner ? view_state(owner)->background : layer->background;
    if(!layer->dirty && layer->synced_background==background)return;Word token=owner ? owner : object;float presentation[VIEW_PRESENTATION_FIELDS];view_presentation(token,presentation);if(!presentation[20]){layer->dirty=1;layer->published=0;return;}
    struct Sequence *colors=(void *)layer->colors,*locations=(void *)layer->locations;double scale=screen_scale();float metrics[]={(float)(layer->border_width*scale),(float)(view->corner*scale),(float)layer->start.x,(float)layer->start.y,(float)layer->end.x,(float)layer->end.y,(float)layer->contents_rect.x,(float)layer->contents_rect.y,(float)layer->contents_rect.width,(float)layer->contents_rect.height,(float)(scale/fmax(1e-6,layer_metric(object,"contentsScale")))};
    const char *gravities[]={"resize","resizeAspect","resizeAspectFill","center","top","bottom","left","right","topLeft","topRight","bottomLeft","bottomRight"};unsigned gravity=0;if(layer->gravity)for(unsigned i=0;i<COUNT(gravities);++i)if(!strcmp(gravities[i],string_bytes(layer->gravity,"contentsGravity")))gravity=i;
    jint options[]={(jint)label_argb(background,0),(jint)label_argb(layer->border,0xff000000),(jint)gravity,(jint)(background || layer->background_set)};JNIEnv *env=attached_environment("CALayer composition");static jmethodID method;
    if(!method){jclass cls=(*env)->GetObjectClass(env,activity);method=(*env)->GetMethodID(env,cls,"syncGuestLayer","(JJZZ[F[I[I[F[B)V");(*env)->DeleteLocalRef(env,cls);}
    jfloatArray m=(*env)->NewFloatArray(env,COUNT(metrics)),stops=locations && locations->count ? (*env)->NewFloatArray(env,locations->count) : NULL;jintArray o=(*env)->NewIntArray(env,COUNT(options)),palette=colors && colors->count ? (*env)->NewIntArray(env,colors->count) : NULL;
    (*env)->SetFloatArrayRegion(env,m,0,COUNT(metrics),metrics);(*env)->SetIntArrayRegion(env,o,0,COUNT(options),options);
    if(palette)for(size_t i=0;i<colors->count;++i){jint color=label_argb(colors->items[i],0);(*env)->SetIntArrayRegion(env,palette,i,1,&color);}if(stops)for(size_t i=0;i<locations->count;++i){jfloat value=numeric_double((void *)locations->items[i]);(*env)->SetFloatArrayRegion(env,stops,i,1,&value);}
    unsigned replace=!layer->published || layer->contents!=layer->synced_contents;jbyteArray bytes=NULL;if(replace && layer->contents){struct HostObject *data=(void *)image_png((void *)layer->contents);if(data){bytes=(*env)->NewByteArray(env,data->content[0]);(*env)->SetByteArrayRegion(env,bytes,0,data->content[0],(void *)data_buffer(data));}}
    Word container=view_container(owner ? owner : layer_owner(object)),host=container ? view_state(container)->host_container : 0;(*env)->CallVoidMethod(env,activity,method,(jlong)token,(jlong)host,(jboolean)!owner,(jboolean)replace,m,o,palette,stops,bytes);
    (*env)->DeleteLocalRef(env,m);(*env)->DeleteLocalRef(env,o);if(palette)(*env)->DeleteLocalRef(env,palette);if(stops)(*env)->DeleteLocalRef(env,stops);if(bytes)(*env)->DeleteLocalRef(env,bytes);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);stop("Android layer drawing failed","CALayer");}layer->dirty=0;layer->synced_background=background;layer->synced_contents=layer->contents;layer->published=1;
}
static void ui_layer_sync_tree(Word object) {
    struct ViewState *view=view_state(object);if(!view->view)ui_view_composition_sync(object);ui_layer_publish(object);
    struct Sequence *children=(void *)view->children;for(size_t i=0;i<children->count;++i)if(!view_state(children->items[i])->view)ui_layer_sync_tree(children->items[i]);
}
static void layer_collect_order(Word object,struct Sequence *order) {
    struct Sequence *children=(void *)view_state(object)->children;for(size_t i=0;i<children->count;++i){Word child=children->items[i];if(!view_state(child)->view){sequence_add(order,"Layer drawing order",child);layer_collect_order(child,order);}}
}
static Method layer_method(Word object,const char *selector,int class_receiver) {
    Word cls=class_receiver ? object : read_word(object);if(!class_inherits(cls,external_class("_OBJC_CLASS_$_CALayer")))return NULL;
    if(class_receiver)return !strcmp(selector,"layer") ? (Method)layer_create : NULL;
    if(!strcmp(selector,"init"))return (Method)layer_initialize;
    if(!strcmp(selector,"frame"))return (Method)layer_frame;if(!strcmp(selector,"setFrame:"))return (Method)layer_set_frame;if(!strcmp(selector,"bounds"))return (Method)layer_bounds;if(!strcmp(selector,"setBounds:"))return (Method)layer_set_bounds;
    if(!strcmp(selector,"position"))return (Method)layer_position;if(!strcmp(selector,"setPosition:"))return (Method)layer_set_position;if(!strcmp(selector,"anchorPoint"))return (Method)layer_anchor;if(!strcmp(selector,"setAnchorPoint:"))return (Method)layer_set_anchor;
    if(!strcmp(selector,"transform"))return (Method)layer_transform;if(!strcmp(selector,"setTransform:"))return (Method)layer_set_transform;if(!strcmp(selector,"affineTransform"))return (Method)layer_affine;if(!strcmp(selector,"setAffineTransform:"))return (Method)layer_set_affine;
    if(!strcmp(selector,"sublayerTransform"))return (Method)layer_sublayer_transform;if(!strcmp(selector,"setSublayerTransform:"))return (Method)layer_set_sublayer_transform;
    const char *metrics[]={"borderWidth","cornerRadius","zPosition","contentsScale"},*set_metrics[]={"setBorderWidth:","setCornerRadius:","setZPosition:","setContentsScale:"};for(unsigned i=0;i<COUNT(metrics);++i){if(!strcmp(selector,metrics[i]))return (Method)layer_metric;if(!strcmp(selector,set_metrics[i]))return (Method)layer_set_metric;}
    if(!strcmp(selector,"opacity"))return (Method)layer_opacity;if(!strcmp(selector,"setOpacity:"))return (Method)layer_set_opacity;if(!strcmp(selector,"isHidden") || !strcmp(selector,"masksToBounds"))return (Method)layer_flag;if(!strcmp(selector,"setHidden:") || !strcmp(selector,"setMasksToBounds:"))return (Method)layer_set_flag;
    const char *values[]={"backgroundColor","borderColor","contents","contentsGravity","delegate"},*set_values[]={"setBackgroundColor:","setBorderColor:","setContents:","setContentsGravity:","setDelegate:"};for(unsigned i=0;i<COUNT(values);++i){if(!strcmp(selector,values[i]))return (Method)layer_value;if(!strcmp(selector,set_values[i]))return (Method)layer_set_value;}
    if(!strcmp(selector,"contentsRect"))return (Method)layer_contents_rect;if(!strcmp(selector,"setContentsRect:"))return (Method)layer_set_contents_rect;
    if(!strcmp(selector,"superlayer"))return (Method)layer_superlayer;if(!strcmp(selector,"sublayers"))return (Method)layer_sublayers;if(!strcmp(selector,"setSublayers:"))return (Method)layer_set_children;if(!strcmp(selector,"addSublayer:"))return (Method)layer_add;if(!strcmp(selector,"removeFromSuperlayer"))return (Method)layer_remove;
    if(!strcmp(selector,"insertSublayer:atIndex:"))return (Method)layer_insert;if(!strcmp(selector,"insertSublayer:above:") || !strcmp(selector,"insertSublayer:below:"))return (Method)layer_insert_relative;if(!strcmp(selector,"replaceSublayer:with:"))return (Method)layer_replace;
    if(!strcmp(selector,"setNeedsLayout"))return (Method)layer_needs_layout;if(!strcmp(selector,"needsLayout"))return (Method)layer_get_needs_layout;if(!strcmp(selector,"layoutIfNeeded"))return (Method)layer_layout_if_needed;if(!strcmp(selector,"layoutSublayers"))return (Method)layer_layout_content;if(!strcmp(selector,"containsPoint:"))return (Method)layer_contains_point;
    if(class_inherits(cls,external_class("_OBJC_CLASS_$_CAGradientLayer"))){const char *gradient[]={"colors","locations","type"},*set_gradient[]={"setColors:","setLocations:","setType:"};for(unsigned i=0;i<COUNT(gradient);++i){if(!strcmp(selector,gradient[i]))return (Method)layer_value;if(!strcmp(selector,set_gradient[i]))return (Method)layer_set_value;}if(!strcmp(selector,"startPoint") || !strcmp(selector,"endPoint"))return (Method)layer_gradient_point;if(!strcmp(selector,"setStartPoint:") || !strcmp(selector,"setEndPoint:"))return (Method)layer_set_gradient_point;}
    return NULL;
}
static Method views_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if(!class_receiver && object==application_object && !strcmp(selector,"windows"))return (Method)application_windows;
    Word cls=class_receiver ? object : read_word(object);
    Method layer=layer_method(object,selector,class_receiver);if(layer)return layer;
    const char *base=external_base(cls);
    int view=class_inherits(cls,external_class("_OBJC_CLASS_$_UIView")) || (base && !strcmp(base,"_OBJC_CLASS_$__UILayoutGuide"));
    int navigation=class_inherits(cls,external_class("_OBJC_CLASS_$_UINavigationController"));
    int controller=class_inherits(cls,external_class("_OBJC_CLASS_$_UIViewController"));
    if (class_inherits(cls,external_class("_OBJC_CLASS_$_NSLayoutConstraint"))) {
        if (class_receiver) {
            if (!strcmp(selector,"constraintWithItem:attribute:relatedBy:toItem:attribute:multiplier:constant:")) return (Method)constraint_create;
            if (!strcmp(selector,"constraintsWithVisualFormat:options:metrics:views:")) return (Method)constraint_visual_format;
            if (!strcmp(selector,"activateConstraints:") || !strcmp(selector,"deactivateConstraints:")) return (Method)constraints_set_active;
        } else {
            if (!strcmp(selector,"constant")) return (Method)constraint_constant;
            if (!strcmp(selector,"setConstant:")) return (Method)constraint_set_constant;
            if (!strcmp(selector,"priority")) return (Method)constraint_priority;
            if (!strcmp(selector,"setPriority:")) return (Method)constraint_set_priority;
            if (!strcmp(selector,"multiplier")) return (Method)constraint_multiplier;
            if (!strcmp(selector,"setActive:")) return (Method)constraint_set_active;
            if (!strcmp(selector,"setIdentifier:")) return (Method)constraint_set_identifier;
            if (!strcmp(selector,"firstItem") || !strcmp(selector,"secondItem") || !strcmp(selector,"firstAttribute") || !strcmp(selector,"secondAttribute") || !strcmp(selector,"relation") || !strcmp(selector,"isActive") || !strcmp(selector,"identifier")) return (Method)constraint_property;
        }
    }
    if (class_receiver) {
        if (view && !strcmp(selector,"layerClass")) return (Method)default_layer_class;
        if (view && !strcmp(selector,"animateWithDuration:delay:options:animations:completion:")) return (Method)view_animate;
        if (view && !strcmp(selector,"animateWithDuration:animations:completion:")) return (Method)view_animate_plain;
        if (view && !strcmp(selector,"animateWithDuration:animations:")) return (Method)view_animate_simple;
        if(view && !strcmp(selector,"animateWithDuration:delay:usingSpringWithDamping:initialSpringVelocity:options:animations:completion:"))return (Method)view_animate_spring;
        if(view && !strcmp(selector,"beginAnimations:context:"))return (Method)view_begin_animations;
        if(view && !strcmp(selector,"commitAnimations"))return (Method)view_commit_animations;
        if(view && (!strcmp(selector,"setAnimationDuration:") || !strcmp(selector,"setAnimationDelay:")))return (Method)view_animation_time;
        if(view && !strcmp(selector,"setAnimationRepeatCount:"))return (Method)view_animation_repeat;
        if(view && (!strcmp(selector,"setAnimationDelegate:") || !strcmp(selector,"setAnimationCurve:") || !strcmp(selector,"setAnimationRepeatAutoreverses:") || !strcmp(selector,"setAnimationBeginsFromCurrentState:") || !strcmp(selector,"setAnimationsEnabled:")))return (Method)view_animation_setting;
        if(view && (!strcmp(selector,"setAnimationWillStartSelector:") || !strcmp(selector,"setAnimationDidStopSelector:")))return (Method)view_animation_selector;
        if(view && !strcmp(selector,"areAnimationsEnabled"))return (Method)view_animations_state;
        if(view && !strcmp(selector,"performWithoutAnimation:"))return (Method)view_without_animation;
        return NULL;
    }
    if (navigation) {
        if (!strcmp(selector,"navigationBar")) return (Method)controller_navigation_bar;
        if (!strcmp(selector,"pushViewController:animated:")) return (Method)navigation_push;
        if (!strcmp(selector,"popViewControllerAnimated:")) return (Method)navigation_pop;
        if (!strcmp(selector,"viewControllers")) return (Method)view_subviews;
        if (!strcmp(selector,"setViewControllers:animated:")) return (Method)navigation_set_controllers;
        if (!strcmp(selector,"setViewControllers:")) return (Method)navigation_set_controllers_plain;
        if (!strcmp(selector,"topViewController")) return (Method)navigation_top;
        if (!strcmp(selector,"visibleViewController")) return (Method)navigation_visible;
        if (!strcmp(selector,"initWithRootViewController:")) return (Method)navigation_init_root;
    }
    if (base && !strcmp(base,"_OBJC_CLASS_$_UINavigationItem")) {
        if (!strcmp(selector,"initWithTitle:")) return (Method)navigation_item_init;
        if (!strcmp(selector,"title")) return (Method)navigation_item_title;
        if (!strcmp(selector,"setTitle:")) return (Method)navigation_item_set_title;
        const char *getters[]={"leftBarButtonItem","rightBarButtonItem","backBarButtonItem","titleView","prompt"};
        const char *setters[]={"setLeftBarButtonItem:","setLeftBarButtonItem:animated:","setRightBarButtonItem:","setRightBarButtonItem:animated:","setBackBarButtonItem:","setTitleView:","setPrompt:"};
        for (unsigned i=0;i<COUNT(getters);++i) if (!strcmp(selector,getters[i])) return (Method)navigation_item_value;
        for (unsigned i=0;i<COUNT(setters);++i) if (!strcmp(selector,setters[i])) return (Method)navigation_item_set_value;
    }
    if (base && !strcmp(base,"_OBJC_CLASS_$_UINavigationBar")) {
        if (!strcmp(selector,"barStyle")) return (Method)navigation_bar_style;
        if (!strcmp(selector,"setBarStyle:")) return (Method)navigation_bar_set_style;
    }
    if (controller) {
        if (!strcmp(selector,"edgesForExtendedLayout") || !strcmp(selector,"automaticallyAdjustsScrollViewInsets") || !strcmp(selector,"definesPresentationContext") || !strcmp(selector,"extendedLayoutIncludesOpaqueBars") ||
            !strcmp(selector,"clearsSelectionOnViewWillAppear") || !strcmp(selector,"isMovingToParentViewController") || !strcmp(selector,"isMovingFromParentViewController")) return (Method)controller_layout_property;
        if (!strcmp(selector,"setEdgesForExtendedLayout:") || !strcmp(selector,"setAutomaticallyAdjustsScrollViewInsets:") || !strcmp(selector,"setDefinesPresentationContext:") || !strcmp(selector,"setExtendedLayoutIncludesOpaqueBars:") || !strcmp(selector,"setClearsSelectionOnViewWillAppear:")) return (Method)controller_set_layout_property;
        if (!strcmp(selector,"splitViewController")) return (Method)controller_split_view;
        if (!strcmp(selector,"willMoveToParentViewController:") || !strcmp(selector,"didMoveToParentViewController:")) return (Method)controller_move;
        if (!strcmp(selector,"init")) return class_inherits(cls,external_class("_OBJC_CLASS_$_UIAlertController")) ? (Method)ui_alert_initialize : (Method)controller_initialize;
        if (!strcmp(selector,"initWithNibName:bundle:")) return (Method)controller_init_nib;
        if (class_inherits(cls,external_class("_OBJC_CLASS_$_UITableViewController"))) {
            if (!strcmp(selector,"initWithStyle:")) return (Method)controller_init_style;
            if (!strcmp(selector,"tableView")) return (Method)controller_table_view;
            if (!strcmp(selector,"setTableView:")) return (Method)controller_set_view;
        }
        if (!strcmp(selector,"title")) return (Method)navigation_item_title;
        if (!strcmp(selector,"setTitle:")) return (Method)controller_set_title;
        if (!strcmp(selector,"navigationItem")) return (Method)controller_navigation_item;
        if (!strcmp(selector,"nibName")) return (Method)controller_nib_name;
        if (!strcmp(selector,"nibBundle")) return (Method)controller_nib_bundle;
        if (!strcmp(selector,"setModalInPopover:")) return (Method)controller_set_modal_popover;
        if (!strcmp(selector,"isModalInPopover")) return (Method)controller_modal_popover;
        if (!strcmp(selector,"setModalPresentationStyle:")) return (Method)controller_modal_style;
        if (!strcmp(selector,"parentViewController")) return (Method)view_superview;
        if (!strcmp(selector,"childViewControllers")) return (Method)view_subviews;
        if (!strcmp(selector,"addChildViewController:")) return (Method)controller_add_child;
        if (!strcmp(selector,"removeFromParentViewController")) return (Method)controller_remove_parent;
        if (!strcmp(selector,"isBeingPresented")) return (Method)controller_is_presenting;
        if (!strcmp(selector,"isBeingDismissed")) return (Method)controller_is_dismissing;
        if (!strcmp(selector,"modalPresentationStyle")) return (Method)controller_get_modal_style;
        if (!strcmp(selector,"setModalTransitionStyle:")) return (Method)controller_transition_style;
        if (!strcmp(selector,"modalTransitionStyle")) return (Method)controller_get_transition_style;
        if (!strcmp(selector,"popoverPresentationController")) return (Method)controller_popover;
        if (!strcmp(selector,"presentedViewController")) return (Method)controller_presented;
        if (!strcmp(selector,"presentingViewController")) return (Method)controller_presenting;
        if (!strcmp(selector,"view")) return (Method)controller_view;
        if (!strcmp(selector,"loadView")) return (Method)controller_load_view;
        if (!strcmp(selector,"loadViewIfNeeded")) return (Method)controller_load_if_needed;
        if (!strcmp(selector,"viewIfLoaded")) return (Method)controller_loaded_view;
        if (!strcmp(selector,"setView:")) return (Method)controller_set_view;
        if (!strcmp(selector,"isViewLoaded")) return (Method)controller_loaded;
        for (unsigned i=0;i<COUNT(view_methods);++i) if (!strcmp(selector,view_methods[i].name)) return view_methods[i].implementation;
    }
    if (!strcmp(receiver,"_OBJC_CLASS_$_UIPopoverPresentationController")) {
        if (!strcmp(selector,"setSourceView:")) return (Method)popover_set_source;
        if (!strcmp(selector,"sourceView")) return (Method)popover_source;
        if (!strcmp(selector,"setSourceRect:")) return (Method)view_set_frame;
        if (!strcmp(selector,"sourceRect")) return (Method)view_frame;
        if (!strcmp(selector,"setDelegate:")) return (Method)popover_set_delegate;
        if (!strcmp(selector,"delegate")) return (Method)popover_delegate;
        if (!strcmp(selector,"setPermittedArrowDirections:")) return (Method)popover_set_arrows;
        if (!strcmp(selector,"permittedArrowDirections")) return (Method)popover_arrows;
    }
    if (!view) return NULL;
    if(!strcmp(selector,"intrinsicContentSize"))return (Method)ui_view_intrinsic_size;
    if(!strcmp(selector,"invalidateIntrinsicContentSize"))return (Method)view_invalidate_intrinsic;
    if(!strcmp(selector,"sizeThatFits:"))return (Method)view_size_that_fits;
    if(!strcmp(selector,"sizeToFit"))return (Method)view_size_to_fit;
    if (!strcmp(selector,"drawRect:") && !class_inherits(cls,external_class("_OBJC_CLASS_$_UILabel"))) return (Method)view_draw_rect;
    if (!strcmp(selector,"setNeedsDisplay")) return (Method)ui_view_request_display;
    if (!strcmp(selector,"setOpaque:")) return (Method)view_set_opaque;
    if (!strcmp(selector,"isOpaque")) return (Method)view_get_opaque;
    if (class_inherits(cls,external_class("_OBJC_CLASS_$_UIWindow"))) {
        if (!strcmp(selector,"makeKeyAndVisible")) return (Method)window_make_visible;
        if (!strcmp(selector,"makeKeyWindow")) return (Method)window_make_key;
        if (!strcmp(selector,"becomeKeyWindow")) return (Method)window_become_key;
        if (!strcmp(selector,"resignKeyWindow")) return (Method)window_resign_key;
        if (!strcmp(selector,"isKeyWindow")) return (Method)window_is_key;
        if (!strcmp(selector,"setWindowLevel:")) return (Method)window_set_level;
        if (!strcmp(selector,"windowLevel")) return (Method)window_get_level;
    }
    if (!strcmp(selector,"addConstraint:")) return (Method)view_add_constraint;
    if (!strcmp(selector,"removeConstraint:")) return (Method)view_remove_constraint;
    if (!strcmp(selector,"addConstraints:") || !strcmp(selector,"removeConstraints:")) return (Method)view_constraints_batch;
    if (!strcmp(selector,"constraints")) return (Method)view_get_constraints;
    if (!strcmp(selector,"layoutIfNeeded")) return (Method)view_layout_if_needed;
    if (!strcmp(selector,"setNeedsLayout")) return (Method)view_request_layout;
    if (!strcmp(selector,"setNeedsUpdateConstraints")) return (Method)view_needs_constraints;
    if (!strcmp(selector,"setTranslatesAutoresizingMaskIntoConstraints:")) return (Method)view_set_translates;
    if (!strcmp(selector,"translatesAutoresizingMaskIntoConstraints")) return (Method)view_get_translates;
    if (!strcmp(selector,"setLayoutMargins:")) return (Method)view_set_margins;
    if (!strcmp(selector,"layoutMargins")) return (Method)view_get_margins;
    if (!strcmp(selector,"init")) return (Method)view_initialize;
    if (!strcmp(selector,"setClipsToBounds:")) return (Method)view_set_clips;
    if (!strcmp(selector,"clipsToBounds")) return (Method)view_clips;
    if (!strcmp(selector,"initWithFrame:")) return (Method)view_init_frame;
    if (!strcmp(selector,"isDescendantOfView:")) return (Method)view_descendant;
    if (!strcmp(selector,"window")) return (Method)view_window;
    if (!strcmp(selector,"setBackgroundColor:")) return (Method)view_background;
    if (!strcmp(selector,"backgroundColor")) return (Method)view_get_background;
    if (!strcmp(selector,"frame")) return (Method)view_frame;
    if (!strcmp(selector,"bounds")) return (Method)view_bounds;
    if (!strcmp(selector,"center")) return (Method)view_center;
    if (!strcmp(selector,"setFrame:")) return (Method)view_set_frame;
    if (!strcmp(selector,"setBounds:")) return (Method)view_set_bounds;
    if (!strcmp(selector,"setCenter:")) return (Method)view_set_center;
    if (!strcmp(selector,"transform")) return (Method)view_get_transform;
    if (!strcmp(selector,"setTransform:")) return (Method)view_set_transform;
    if (!strcmp(selector,"convertPoint:toView:") || !strcmp(selector,"convertPoint:fromView:")) return (Method)view_convert_point;
    if (!strcmp(selector,"convertRect:toView:") || !strcmp(selector,"convertRect:fromView:")) return (Method)view_convert_rect;
    if (!strcmp(selector,"setHidden:")) return (Method)view_hidden;
    if (!strcmp(selector,"isHidden")) return (Method)view_is_hidden;
    if (!strcmp(selector,"setAlpha:")) return (Method)view_alpha;
    if (!strcmp(selector,"alpha")) return (Method)view_get_alpha;
    if (!strcmp(selector,"setContentScaleFactor:")) return (Method)view_scale;
    if (!strcmp(selector,"contentScaleFactor")) return (Method)view_get_scale;
    if (!strcmp(selector,"setUserInteractionEnabled:")) return (Method)view_interaction;
    if (!strcmp(selector,"isUserInteractionEnabled")) return (Method)view_get_interaction;
    if (!strcmp(selector,"setMultipleTouchEnabled:")) return (Method)view_multitouch;
    if (!strcmp(selector,"isMultipleTouchEnabled")) return (Method)view_get_multitouch;
    if (!strcmp(selector,"setExclusiveTouch:")) return (Method)view_exclusive_touch;
    if (!strcmp(selector,"isExclusiveTouch")) return (Method)view_get_exclusive_touch;
    if (!strcmp(selector,"setIsAccessibilityElement:") || !strcmp(selector,"setAccessibilityLabel:")) return (Method)view_accessibility;
    if (!strcmp(selector,"isAccessibilityElement") || !strcmp(selector,"accessibilityLabel")) return (Method)view_get_accessibility;
    if (!strcmp(selector,"setTag:")) return (Method)view_tag;
    if (!strcmp(selector,"tag")) return (Method)view_get_tag;
    if (!strcmp(selector,"viewWithTag:")) return (Method)view_find_tag;
    if (!strcmp(selector,"setContentMode:")) return (Method)view_content_mode;
    if (!strcmp(selector,"contentMode")) return (Method)view_get_content_mode;
    if (!strcmp(selector,"setAutoresizingMask:")) return (Method)view_set_autoresizing;
    if (!strcmp(selector,"autoresizingMask")) return (Method)view_autoresizing;
    if (!strcmp(selector,"setAutoresizesSubviews:")) return (Method)view_set_autoresizes;
    if (!strcmp(selector,"autoresizesSubviews")) return (Method)view_autoresizes;
    if (!strcmp(selector,"removeFromSuperview")) return (Method)view_remove;
    if (!strcmp(selector,"addSubview:")) return (Method)view_add;
    if (!strcmp(selector,"insertSubview:atIndex:")) return (Method)view_insert_index;
    if (!strcmp(selector,"insertSubview:aboveSubview:") || !strcmp(selector,"insertSubview:belowSubview:")) return (Method)view_insert_relative;
    if (!strcmp(selector,"bringSubviewToFront:") || !strcmp(selector,"sendSubviewToBack:")) return (Method)view_reorder;
    if (!strcmp(selector,"exchangeSubviewAtIndex:withSubviewAtIndex:")) return (Method)view_exchange;
    if (!strcmp(selector,"superview")) return (Method)view_superview;
    if (!strcmp(selector,"subviews")) return (Method)view_subviews;
    if (!strcmp(selector,"layer")) return (Method)view_layer;
    if (!strcmp(selector,"rootViewController")) return (Method)window_controller;
    if (!strcmp(selector,"setRootViewController:")) return (Method)window_set_controller;
    return NULL;
}
