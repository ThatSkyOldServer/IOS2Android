static struct GuestTransform3D transform3d_scale(double x,double y,double z) {
    struct GuestTransform3D result=transform3d_identity; result.m[0]=x; result.m[5]=y; result.m[10]=z; return result;
}
static struct GuestTransform3D transform3d_translation(double x,double y,double z) {
    struct GuestTransform3D result=transform3d_identity; result.m[12]=x; result.m[13]=y; result.m[14]=z; return result;
}
static struct GuestTransform3D transform3d_concat(struct GuestTransform3D a,struct GuestTransform3D b) {
    struct GuestTransform3D result={0};
    for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column)
        for (unsigned k=0;k<4;++k) result.m[row*4+column]+=a.m[row*4+k]*b.m[k*4+column];
    return result;
}
static double ca_current_time(void) { return absolute_time()*1e-9; }
static Word color_space_device_rgb(void) { return (Word)&device_rgb_space; }
static Word color_space_device_gray(void) { return (Word)&device_gray_space; }
static void color_space_release(Word space) { if (space && space!=(Word)&device_rgb_space && space!=(Word)&device_gray_space) stop("Unsupported color space","CGColorSpaceRelease"); }
static const double *color_components(Word color) {
    if (!color || read_word(color)!=external_class("_OBJC_CLASS_$_UIColor")) stop("Unsupported color representation","CGColorGetComponents");
    _Static_assert(offsetof(struct UIKitColor,red)==8 && offsetof(struct UIKitColor,alpha)==32,"RGBA CGFloat storage");
    struct UIKitColor *value=(void *)color;return value->pattern ? &value->alpha : &value->red;
}
static size_t color_component_count(Word color) { color_components(color);return ui_color_pattern_image(color) ? 1 : 4; }
static double color_alpha(Word color) { color_components(color);return ui_color_opacity(color); }
static double rect_min_x(struct GuestRect r) { return fmin(r.x,r.x+r.width); }
static double rect_max_x(struct GuestRect r) { return fmax(r.x,r.x+r.width); }
static double rect_min_y(struct GuestRect r) { return fmin(r.y,r.y+r.height); }
static double rect_max_y(struct GuestRect r) { return fmax(r.y,r.y+r.height); }
static double rect_mid_x(struct GuestRect r) { return r.x+r.width*0.5; }
static double rect_mid_y(struct GuestRect r) { return r.y+r.height*0.5; }
static double rect_width(struct GuestRect r) { return fabs(r.width); }
static double rect_height(struct GuestRect r) { return fabs(r.height); }
static int rect_null(struct GuestRect r) { return r.x==INFINITY && r.y==INFINITY; }
static int rect_empty(struct GuestRect r) { return rect_null(r) || !r.width || !r.height; }
static struct GuestRect rect_standard(struct GuestRect r) { return (struct GuestRect){rect_min_x(r),rect_min_y(r),fabs(r.width),fabs(r.height)}; }
static struct GuestRect rect_integral(struct GuestRect r) {
    if (rect_null(r) || !isfinite(r.x) || !isfinite(r.y) || !isfinite(r.width) || !isfinite(r.height)) return r;
    double left=floor(rect_min_x(r)),top=floor(rect_min_y(r));
    return (struct GuestRect){left,top,ceil(rect_max_x(r))-left,ceil(rect_max_y(r))-top};
}
static int rect_equal(struct GuestRect a,struct GuestRect b) { return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height; }
static int point_equal(struct GuestPoint a,struct GuestPoint b) { return a.x==b.x && a.y==b.y; }
static int size_equal(struct GuestSize a,struct GuestSize b) { return a.width==b.width && a.height==b.height; }
 
static struct GuestPoint location_coordinate_make(double latitude,double longitude) { return (struct GuestPoint){latitude,longitude}; }
static struct GuestRect rect_inset(struct GuestRect r,double dx,double dy) {
    if (rect_null(r)) return r;
    r=rect_standard(r);
    if (r.width<2*dx || r.height<2*dy) return (struct GuestRect){INFINITY,INFINITY,0,0};
    return (struct GuestRect){r.x+dx,r.y+dy,r.width-2*dx,r.height-2*dy};
}
static struct GuestRect rect_offset(struct GuestRect r,double dx,double dy) { if (!rect_null(r)) { r.x+=dx; r.y+=dy; } return r; }
static struct GuestRect rect_intersection(struct GuestRect a,struct GuestRect b) {
    if (rect_null(a) || rect_null(b)) return (struct GuestRect){INFINITY,INFINITY,0,0};
    double x=fmax(rect_min_x(a),rect_min_x(b)),y=fmax(rect_min_y(a),rect_min_y(b)),right=fmin(rect_max_x(a),rect_max_x(b)),bottom=fmin(rect_max_y(a),rect_max_y(b));
    if (right<x || bottom<y) return (struct GuestRect){INFINITY,INFINITY,0,0};
    return (struct GuestRect){x,y,right-x,bottom-y};
}
static int rect_intersects(struct GuestRect a,struct GuestRect b) { return !rect_empty(rect_intersection(a,b)); }
static int rect_contains_point(struct GuestRect r,struct GuestPoint p) { return !rect_empty(r) && p.x>=rect_min_x(r) && p.x<rect_max_x(r) && p.y>=rect_min_y(r) && p.y<rect_max_y(r); }
static struct GuestAffine affine_make(double a,double b,double c,double d,double tx,double ty) { return (struct GuestAffine){a,b,c,d,tx,ty}; }
static struct GuestAffine affine_scale(double x,double y) { return affine_make(x,0,0,y,0,0); }
static struct GuestAffine affine_translation(double x,double y) { return affine_make(1,0,0,1,x,y); }
static struct GuestAffine affine_rotation(double angle) { double s=sin(angle),c=cos(angle); return affine_make(c,s,-s,c,0,0); }
static struct GuestAffine affine_concat(struct GuestAffine a,struct GuestAffine b) {
    return affine_make(a.a*b.a+a.b*b.c,a.a*b.b+a.b*b.d,a.c*b.a+a.d*b.c,a.c*b.b+a.d*b.d,a.tx*b.a+a.ty*b.c+b.tx,a.tx*b.b+a.ty*b.d+b.ty);
}
static struct GuestAffine affine_translate(struct GuestAffine t,double x,double y) { return affine_concat(affine_translation(x,y),t); }
static struct GuestAffine affine_scaled(struct GuestAffine t,double x,double y) { return affine_concat(affine_scale(x,y),t); }
static struct GuestAffine affine_rotated(struct GuestAffine t,double a) { return affine_concat(affine_rotation(a),t); }
static struct GuestPoint affine_point(struct GuestPoint p,struct GuestAffine t) { return (struct GuestPoint){p.x*t.a+p.y*t.c+t.tx,p.x*t.b+p.y*t.d+t.ty}; }
static struct GuestSize affine_size(struct GuestSize s,struct GuestAffine t) { return (struct GuestSize){s.width*t.a+s.height*t.c,s.width*t.b+s.height*t.d}; }

enum DrawingCommand {
    DRAW_SAVE=1,DRAW_RESTORE,DRAW_TRANSFORM,DRAW_FILL_COLOR,DRAW_STROKE_COLOR,DRAW_WIDTH,DRAW_JOIN,DRAW_MITER,DRAW_ALPHA,DRAW_BLEND,DRAW_SHADOW,
    DRAW_MOVE,DRAW_LINE,DRAW_CURVE,DRAW_QUAD,DRAW_CLOSE,DRAW_BEGIN,DRAW_ARC,DRAW_ARC_TO,DRAW_PATH,DRAW_PAINT,DRAW_CLIP,DRAW_RECT,DRAW_ELLIPSE,
    DRAW_GRADIENT,DRAW_MASK,DRAW_IMAGE,DRAW_LAYER_BEGIN,DRAW_LAYER_END,DRAW_PATH_RECT,DRAW_DASH,DRAW_FILL_PATTERN,DRAW_STROKE_PATTERN
};
struct DrawingBuffer { double *values; size_t count,capacity; };
struct DrawingPath { Word tag; unsigned references,mutable; struct DrawingBuffer buffer; struct GuestPoint current,start; unsigned has_current; };
struct DrawingGradient { Word tag; unsigned references; size_t count; double *stops; };
struct DrawingFont { Word tag; unsigned references; Word data; };
struct DrawingContext { struct DrawingBuffer buffer; struct GuestSize size; double scale; unsigned opaque; Word *images,cached; size_t image_count,image_capacity,cached_count; struct DrawingContext *previous; };
static Word drawing_path_tag,drawing_gradient_tag,drawing_font_tag;
static _Thread_local struct DrawingContext *drawing_context;
static double *drawing_append(struct DrawingBuffer *buffer,unsigned opcode,size_t count) {
    if (count>INT_MAX-2 || buffer->count>(size_t)INT_MAX-count-2) stop("Drawing command buffer too large","CoreGraphics");
    size_t needed=buffer->count+count+2;
    if (needed>buffer->capacity) {
        size_t capacity=buffer->capacity ? buffer->capacity*2 : 128;
        if (capacity<needed) capacity=needed;
        double *values=realloc(buffer->values,capacity*sizeof(*values));
        if (!values) stop("Allocation failed","CoreGraphics commands");
        buffer->values=values; buffer->capacity=capacity;
    }
    double *result=buffer->values+buffer->count; result[0]=opcode; result[1]=count; buffer->count=needed; return result+2;
}
static void drawing_record(struct DrawingBuffer *buffer,unsigned opcode,const double *values,size_t count) {
    double *result=drawing_append(buffer,opcode,count); if (count) memcpy(result,values,count*sizeof(*values));
}
static struct DrawingContext *drawing_check(Word object) {
    if (!object) stop("No current drawing context","CoreGraphics"); return (void *)object;
}
static struct DrawingPath *drawing_path_check(Word object,int mutable) {
    struct DrawingPath *path=(void *)object;
    if (!path || path->tag!=(Word)&drawing_path_tag || (mutable && !path->mutable)) stop("Invalid path","CoreGraphics");
    return path;
}
static int drawing_resource_retain(Word object) {
    if (!object) return 0; Word tag=read_word(object);
    if (tag!=(Word)&drawing_path_tag && tag!=(Word)&drawing_gradient_tag && tag!=(Word)&drawing_font_tag) return 0;
    unsigned *references=(void *)(object+sizeof(Word)); __atomic_add_fetch(references,1,__ATOMIC_RELAXED); return 1;
}
static int drawing_resource_release(Word object) {
    if (!object) return 0; Word tag=read_word(object);
    if (tag!=(Word)&drawing_path_tag && tag!=(Word)&drawing_gradient_tag && tag!=(Word)&drawing_font_tag) return 0;
    unsigned *references=(void *)(object+sizeof(Word));
    if (__atomic_sub_fetch(references,1,__ATOMIC_ACQ_REL)) return 1;
    if (tag==(Word)&drawing_path_tag) free(((struct DrawingPath *)object)->buffer.values);
    else if (tag==(Word)&drawing_gradient_tag) free(((struct DrawingGradient *)object)->stops);
    else free((void *)((struct DrawingFont *)object)->data);
    free((void *)object); return 1;
}
static Word drawing_resource_keep(Word object) { if (object && !drawing_resource_retain(object)) stop("Invalid drawing resource","CoreGraphics retain"); return object; }
static void drawing_resource_drop(Word object) { if (object && !drawing_resource_release(object)) stop("Invalid drawing resource","CoreGraphics release"); }
static Word drawing_path_create(void) {
    struct DrawingPath *path=calloc(1,sizeof(*path)); if (!path) stop("Allocation failed","CGPathCreateMutable");
    path->tag=(Word)&drawing_path_tag; path->references=1; path->mutable=1; return (Word)path;
}
static struct GuestPoint drawing_point(double x,double y,const struct GuestAffine *transform) {
    struct GuestPoint p={x,y}; return transform ? affine_point(p,*transform) : p;
}
static void drawing_path_point(Word object,const struct GuestAffine *transform,double x,double y,unsigned opcode) {
    struct DrawingPath *path=drawing_path_check(object,1); struct GuestPoint point=drawing_point(x,y,transform);
    if (!path->has_current) opcode=DRAW_MOVE;
    drawing_record(&path->buffer,opcode,(double[]){point.x,point.y},2);
    path->current=point; if (opcode==DRAW_MOVE) path->start=point; path->has_current=1;
}
static void drawing_path_move(Word object,const struct GuestAffine *t,double x,double y) { drawing_path_point(object,t,x,y,DRAW_MOVE); }
static void drawing_path_line(Word object,const struct GuestAffine *t,double x,double y) { drawing_path_point(object,t,x,y,DRAW_LINE); }
static void drawing_path_curve(Word object,const struct GuestAffine *t,double x1,double y1,double x2,double y2,double x,double y) {
    struct DrawingPath *path=drawing_path_check(object,1);
    if (!path->has_current) drawing_path_move(object,t,x1,y1);
    struct GuestPoint a=drawing_point(x1,y1,t),b=drawing_point(x2,y2,t),c=drawing_point(x,y,t);
    drawing_record(&path->buffer,DRAW_CURVE,(double[]){a.x,a.y,b.x,b.y,c.x,c.y},6); path->current=c;
}
static void drawing_path_quad(Word object,const struct GuestAffine *t,double x1,double y1,double x,double y) {
    struct DrawingPath *path=drawing_path_check(object,1); if (!path->has_current) drawing_path_move(object,t,x1,y1);
    struct GuestPoint a=drawing_point(x1,y1,t),b=drawing_point(x,y,t);
    drawing_record(&path->buffer,DRAW_QUAD,(double[]){a.x,a.y,b.x,b.y},4); path->current=b;
}
static void drawing_path_close(Word object) { struct DrawingPath *p=drawing_path_check(object,1); if (p->has_current) { drawing_record(&p->buffer,DRAW_CLOSE,NULL,0); p->current=p->start; } }
static void drawing_path_lines(Word object,const struct GuestAffine *t,const struct GuestPoint *points,size_t count) {
    if (count && !points) stop("Null path points","CGPathAddLines");
    for (size_t i=0;i<count;++i) drawing_path_point(object,t,points[i].x,points[i].y,i ? DRAW_LINE : DRAW_MOVE);
}
static void drawing_path_arc(Word object,const struct GuestAffine *t,double x,double y,double radius,double start,double end,unsigned char clockwise) {
    if (!isfinite(radius) || radius<0 || !isfinite(start) || !isfinite(end)) stop("Invalid arc","CGPathAddArc");
    double delta=end-start;
    if (fabs(delta)>=2*M_PI) delta=clockwise ? -2*M_PI : 2*M_PI;
    else if (clockwise) { if (delta>0) delta-=2*M_PI; } else if (delta<0) delta+=2*M_PI;
    drawing_path_line(object,t,x+radius*cos(start),y+radius*sin(start));
    unsigned parts=(unsigned)ceil(fabs(delta)/(M_PI*0.5));
    for (unsigned i=0;i<parts;++i) {
        double a=start+delta*i/parts,b=start+delta*(i+1)/parts,k=4.0/3*tan((b-a)*0.25);
        drawing_path_curve(object,t,x+radius*(cos(a)-k*sin(a)),y+radius*(sin(a)+k*cos(a)),x+radius*(cos(b)+k*sin(b)),y+radius*(sin(b)-k*cos(b)),x+radius*cos(b),y+radius*sin(b));
    }
}
static void drawing_path_arc_to(Word object,const struct GuestAffine *t,double x1,double y1,double x2,double y2,double radius) {
    struct DrawingPath *p=drawing_path_check(object,1); if (!p->has_current || radius==0) { drawing_path_line(object,t,x1,y1); return; }
    struct GuestPoint current=p->current;
    if (t) {
        double det=t->a*t->d-t->b*t->c; if (!det) { drawing_path_line(object,t,x1,y1); return; }
        double x=current.x-t->tx,y=current.y-t->ty; current=(struct GuestPoint){(t->d*x-t->c*y)/det,(-t->b*x+t->a*y)/det};
    }
    double ux=current.x-x1,uy=current.y-y1,vx=x2-x1,vy=y2-y1,ul=hypot(ux,uy),vl=hypot(vx,vy);
    if (!ul || !vl || radius<0) { drawing_path_line(object,t,x1,y1); return; }
    ux/=ul; uy/=ul; vx/=vl; vy/=vl; double cross=ux*vy-uy*vx,dot=fmax(-1,fmin(1,ux*vx+uy*vy));
    if (fabs(cross)<1e-12) { drawing_path_line(object,t,x1,y1); return; }
    double angle=acos(dot),length=radius/tan(angle*0.5),side=cross>0 ? 1 : -1;
    double ax=x1+ux*length,ay=y1+uy*length,cx=ax-uy*side*radius,cy=ay+ux*side*radius,bx=x1+vx*length,by=y1+vy*length;
    drawing_path_line(object,t,ax,ay); drawing_path_arc(object,t,cx,cy,radius,atan2(ay-cy,ax-cx),atan2(by-cy,bx-cx),cross>0);
}
static void drawing_path_add(Word object,const struct GuestAffine *t,Word source) {
    struct DrawingPath *p=drawing_path_check(source,0);
    size_t length=p->buffer.count; double *values=length ? malloc(length*sizeof(*values)) : NULL;
    if (length && !values) stop("Allocation failed","CGPathAddPath"); if (length) memcpy(values,p->buffer.values,length*sizeof(*values));
    for (size_t i=0;i<length;) {
        unsigned op=values[i]; size_t n=values[i+1]; double *v=values+i+2;
        if (op==DRAW_MOVE || op==DRAW_LINE) drawing_path_point(object,t,v[0],v[1],op);
        else if (op==DRAW_CURVE) drawing_path_curve(object,t,v[0],v[1],v[2],v[3],v[4],v[5]);
        else if (op==DRAW_QUAD) drawing_path_quad(object,t,v[0],v[1],v[2],v[3]);
        else if (op==DRAW_CLOSE) drawing_path_close(object);
        i+=n+2;
    }
    free(values);
}
static Word drawing_path_copy(Word object) { Word result=drawing_path_create(); drawing_path_add(result,NULL,object); drawing_path_check(result,0)->mutable=0; return result; }
static Word drawing_path_mutable_copy(Word object) { Word result=drawing_path_create(); drawing_path_add(result,NULL,object); return result; }
static void drawing_path_rect(Word object,const struct GuestAffine *t,struct GuestRect rect) {
    drawing_path_move(object,t,rect.x,rect.y); drawing_path_line(object,t,rect.x+rect.width,rect.y); drawing_path_line(object,t,rect.x+rect.width,rect.y+rect.height); drawing_path_line(object,t,rect.x,rect.y+rect.height); drawing_path_close(object);
}
static struct GuestPoint drawing_path_current(Word object) { return drawing_path_check(object,0)->current; }
static int drawing_path_empty(Word object) { return !drawing_path_check(object,0)->buffer.count; }
static void drawing_context_command(Word object,unsigned opcode,const double *values,size_t count) { drawing_record(&drawing_check(object)->buffer,opcode,values,count); }
static void drawing_save(Word c) { drawing_context_command(c,DRAW_SAVE,NULL,0); }
static void drawing_restore(Word c) { drawing_context_command(c,DRAW_RESTORE,NULL,0); }
static void drawing_transform(Word c,struct GuestAffine t) { drawing_context_command(c,DRAW_TRANSFORM,(double[]){t.a,t.b,t.c,t.d,t.tx,t.ty},6); }
static void drawing_translate(Word c,double x,double y) { drawing_transform(c,affine_translation(x,y)); }
static void drawing_scale(Word c,double x,double y) { drawing_transform(c,affine_scale(x,y)); }
static void drawing_rotate(Word c,double angle) { drawing_transform(c,affine_rotation(angle)); }
static void drawing_fill(Word c,double r,double g,double b,double a) { drawing_context_command(c,DRAW_FILL_COLOR,(double[]){r,g,b,a},4); }
static void drawing_stroke(Word c,double r,double g,double b,double a) { drawing_context_command(c,DRAW_STROKE_COLOR,(double[]){r,g,b,a},4); }
static void drawing_fill_gray(Word c,double gray,double a) { drawing_fill(c,gray,gray,gray,a); }
static void drawing_stroke_gray(Word c,double gray,double a) { drawing_stroke(c,gray,gray,gray,a); }
static size_t drawing_add_image(struct DrawingContext *,Word);
static void drawing_color(Word c,Word color,int stroke) {
    if(!color)return;const double *v=color_components(color);Word image=ui_color_pattern_image(color);
    if(image){size_t index=drawing_add_image(drawing_check(c),image);drawing_context_command(c,stroke ? DRAW_STROKE_PATTERN : DRAW_FILL_PATTERN,(double[]){index,1/ui_image_scale(image),ui_color_opacity(color)},3);}
    else (stroke ? drawing_stroke : drawing_fill)(c,v[0],v[1],v[2],v[3]);
}
static void drawing_fill_color(Word c,Word color) { drawing_color(c,color,0); }
static void drawing_stroke_color(Word c,Word color) { drawing_color(c,color,1); }
static void drawing_line_width(Word c,double value) { drawing_context_command(c,DRAW_WIDTH,&value,1); }
static void drawing_line_join(Word c,unsigned value) { drawing_context_command(c,DRAW_JOIN,(double[]){value},1); }
static void drawing_miter(Word c,double value) { drawing_context_command(c,DRAW_MITER,&value,1); }
static void drawing_alpha(Word c,double value) { drawing_context_command(c,DRAW_ALPHA,&value,1); }
static void drawing_blend(Word c,unsigned value) { drawing_context_command(c,DRAW_BLEND,(double[]){value},1); }
static void drawing_shadow(Word c,struct GuestSize offset,double blur,Word color) {
    if(ui_color_pattern_image(color))stop("Pattern shadow colors are unavailable","CGContextSetShadowWithColor");
    const double transparent[4]={0}; const double *v=color ? color_components(color) : transparent;
    drawing_context_command(c,DRAW_SHADOW,(double[]){offset.width,offset.height,blur,v[0],v[1],v[2],v[3]},7);
}
static void drawing_dash(Word c,double phase,const double *lengths,size_t count) {
    if (count && !lengths) stop("Null dash lengths","CGContextSetLineDash");
    double *v=drawing_append(&drawing_check(c)->buffer,DRAW_DASH,count+1); v[0]=phase; if (count) memcpy(v+1,lengths,count*sizeof(*lengths));
}
static void drawing_begin(Word c) { drawing_context_command(c,DRAW_BEGIN,NULL,0); }
static void drawing_close(Word c) { drawing_context_command(c,DRAW_CLOSE,NULL,0); }
static void drawing_move(Word c,double x,double y) { drawing_context_command(c,DRAW_MOVE,(double[]){x,y},2); }
static void drawing_line(Word c,double x,double y) { drawing_context_command(c,DRAW_LINE,(double[]){x,y},2); }
static void drawing_curve(Word c,double x1,double y1,double x2,double y2,double x,double y) { drawing_context_command(c,DRAW_CURVE,(double[]){x1,y1,x2,y2,x,y},6); }
static void drawing_quad(Word c,double x1,double y1,double x,double y) { drawing_context_command(c,DRAW_QUAD,(double[]){x1,y1,x,y},4); }
static void drawing_arc(Word c,double x,double y,double r,double start,double end,unsigned char clockwise) { drawing_context_command(c,DRAW_ARC,(double[]){x,y,r,start,end,clockwise},6); }
static void drawing_arc_to(Word c,double x1,double y1,double x2,double y2,double r) { drawing_context_command(c,DRAW_ARC_TO,(double[]){x1,y1,x2,y2,r},5); }
static void drawing_add_path(Word c,Word object) { struct DrawingPath *p=drawing_path_check(object,0); drawing_context_command(c,DRAW_PATH,p->buffer.values,p->buffer.count); }
static void drawing_add_lines(Word c,const struct GuestPoint *points,size_t count) { if (count && !points) stop("Null points","CGContextAddLines"); for (size_t i=0;i<count;++i) (i ? drawing_line : drawing_move)(c,points[i].x,points[i].y); }
static void drawing_add_rect(Word c,struct GuestRect r) { drawing_context_command(c,DRAW_PATH_RECT,(double[]){r.x,r.y,r.width,r.height},4); }
static void drawing_paint(Word c,unsigned mode) { if (mode>4) stop("Invalid path paint mode","CGContextDrawPath"); drawing_context_command(c,DRAW_PAINT,(double[]){mode},1); }
static void drawing_fill_path(Word c) { drawing_paint(c,0); }
static void drawing_even_fill_path(Word c) { drawing_paint(c,1); }
static void drawing_stroke_path(Word c) { drawing_paint(c,2); }
static void drawing_clip(Word c) { drawing_context_command(c,DRAW_CLIP,(double[]){0},1); }
static void drawing_even_clip(Word c) { drawing_context_command(c,DRAW_CLIP,(double[]){1},1); }
static void drawing_shape(Word c,struct GuestRect r,unsigned opcode,unsigned kind) { drawing_context_command(c,opcode,(double[]){kind,r.x,r.y,r.width,r.height},5); }
static void drawing_fill_rect(Word c,struct GuestRect r) { drawing_shape(c,r,DRAW_RECT,0); }
static void drawing_stroke_rect(Word c,struct GuestRect r) { drawing_shape(c,r,DRAW_RECT,1); }
static void drawing_clear_rect(Word c,struct GuestRect r) { drawing_shape(c,r,DRAW_RECT,2); }
static void drawing_fill_ellipse(Word c,struct GuestRect r) { drawing_shape(c,r,DRAW_ELLIPSE,0); }
static void drawing_stroke_ellipse(Word c,struct GuestRect r) { drawing_shape(c,r,DRAW_ELLIPSE,1); }
static void drawing_clip_rect(Word c,struct GuestRect r) { drawing_context_command(c,DRAW_CLIP,(double[]){2,r.x,r.y,r.width,r.height},5); }
static Word drawing_gradient_create(Word space,const double *components,const double *locations,size_t count) {
    if (!components || count<1 || count>INT_MAX/5 || (space && space!=(Word)&device_rgb_space && space!=(Word)&device_gray_space)) return 0;
    struct DrawingGradient *gradient=calloc(1,sizeof(*gradient)); if (!gradient) stop("Allocation failed","CGGradient");
    gradient->tag=(Word)&drawing_gradient_tag; gradient->references=1; gradient->count=count; gradient->stops=malloc(count*5*sizeof(double));
    if (!gradient->stops) { free(gradient); stop("Allocation failed","CGGradient stops"); }
    unsigned channels=space==(Word)&device_gray_space ? 2 : 4;
    for (size_t i=0;i<count;++i) {
        double *v=gradient->stops+i*5; v[0]=locations ? locations[i] : count==1 ? 0 : (double)i/(count-1);
        if (!isfinite(v[0]) || v[0]<0 || v[0]>1 || (i && v[0]<gradient->stops[(i-1)*5])) { drawing_resource_drop((Word)gradient); return 0; }
        const double *p=components+i*channels; v[1]=p[0]; v[2]=channels==2 ? p[0] : p[1]; v[3]=channels==2 ? p[0] : p[2]; v[4]=p[channels-1];
    }
    return (Word)gradient;
}
static Word drawing_gradient_colors(Word space,Word colors,const double *locations) {
    struct Sequence *array=(void *)colors; if (!array || !array->count || array->count>INT_MAX/4) return 0;
    double *rgba=malloc(array->count*4*sizeof(double)); if (!rgba) stop("Allocation failed","CGGradient colors");
    for (size_t i=0;i<array->count;++i) { if(ui_color_pattern_image(array->items[i])){free(rgba);return 0;}memcpy(rgba+i*4,color_components(array->items[i]),4*sizeof(double)); }
    Word result=drawing_gradient_create(space==(Word)&device_gray_space ? (Word)&device_rgb_space : space,rgba,locations,array->count); free(rgba); return result;
}
static void drawing_gradient(Word c,Word object,struct GuestPoint start,double r0,struct GuestPoint end,double r1,unsigned options,int radial) {
    struct DrawingGradient *g=(void *)object; if (!g || g->tag!=(Word)&drawing_gradient_tag) stop("Invalid gradient","CoreGraphics");
    double *v=drawing_append(&drawing_check(c)->buffer,DRAW_GRADIENT,9+g->count*5);
    double header[]={radial,start.x,start.y,r0,end.x,end.y,r1,options,g->count}; memcpy(v,header,sizeof(header)); memcpy(v+9,g->stops,g->count*5*sizeof(double));
}
static void drawing_linear_gradient(Word c,Word g,struct GuestPoint start,struct GuestPoint end,unsigned options) { drawing_gradient(c,g,start,0,end,0,options,0); }
static void drawing_radial_gradient(Word c,Word g,struct GuestPoint start,double r0,struct GuestPoint end,double r1,unsigned options) { drawing_gradient(c,g,start,r0,end,r1,options,1); }
static size_t drawing_add_image(struct DrawingContext *c,Word image) {
    for (size_t i=0;i<c->image_count;++i) if (c->images[i]==image) return i;
    if (c->image_count==c->image_capacity) { size_t capacity=c->image_capacity ? c->image_capacity*2 : 8; Word *images=realloc(c->images,capacity*sizeof(*images)); if (!images) stop("Allocation failed","Drawing images"); c->images=images; c->image_capacity=capacity; }
    c->images[c->image_count]=image; return c->image_count++;
}
static void drawing_image_command(Word c,struct GuestRect r,Word image,unsigned opcode,unsigned flip) {
    if (!image) return; struct DrawingContext *ctx=drawing_check(c); size_t index=drawing_add_image(ctx,image);
    drawing_context_command(c,opcode,(double[]){r.x,r.y,r.width,r.height,index,flip},6);
}
static void drawing_image(Word c,struct GuestRect r,Word image) { drawing_image_command(c,r,image,DRAW_IMAGE,1); }
static void drawing_mask(Word c,struct GuestRect r,Word image) { drawing_image_command(c,r,image,DRAW_MASK,1); }
static void drawing_layer_begin(Word c,Word info) { if (info) stop("Unsupported transparency layer auxiliary info","CGContextBeginTransparencyLayer"); drawing_context_command(c,DRAW_LAYER_BEGIN,NULL,0); }
static void drawing_layer_end(Word c) { drawing_context_command(c,DRAW_LAYER_END,NULL,0); }
static void image_context_begin(struct GuestSize size,unsigned char opaque,double scale) {
    if (!scale) scale=screen_scale();
    if (!isfinite(scale) || scale<=0 || !isfinite(size.width) || !isfinite(size.height) || size.width<0 || size.height<0 || ceil(size.width*scale)*ceil(size.height*scale)>INT_MAX/4) stop("Invalid image context size","UIGraphicsBeginImageContextWithOptions");
    struct DrawingContext *c=calloc(1,sizeof(*c)); if (!c) stop("Allocation failed","Image context");
    c->size=size; c->scale=scale; c->opaque=opaque; c->previous=drawing_context; drawing_context=c;
}
static void image_context_begin_plain(struct GuestSize size) { image_context_begin(size,0,1); }
static Word image_context_current(void) { return (Word)drawing_context; }
static void image_context_end(void) { struct DrawingContext *c=drawing_context; if (!c) return; drawing_context=c->previous; free(c->buffer.values); free(c->images); free(c); }
static Word drawing_data_from_java(JNIEnv *env,jbyteArray bytes,const char *operation) {
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); stop("Android image operation failed",operation); }
    if (!bytes) return 0; jsize size=(*env)->GetArrayLength(env,bytes); jbyte *values=(*env)->GetByteArrayElements(env,bytes,NULL);
    if (!values) { (*env)->DeleteLocalRef(env,bytes); stop("Allocation failed",operation); }
    Word data=data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),operation,values,size);
    (*env)->ReleaseByteArrayElements(env,bytes,values,JNI_ABORT); (*env)->DeleteLocalRef(env,bytes); return data;
}
static Word image_context_image(void) {
    struct DrawingContext *c=drawing_context; if (!c || !c->size.width || !c->size.height) return 0;
    if (c->cached && c->cached_count==c->buffer.count) return c->cached;
    JNIEnv *env=attached_environment("Image context render"); static jmethodID method;
    if (!method) { jclass cls=(*env)->GetObjectClass(env,activity); method=(*env)->GetMethodID(env,cls,"renderGuestImage","(IIDZ[D[[B)[B"); (*env)->DeleteLocalRef(env,cls); }
    jdoubleArray commands=(*env)->NewDoubleArray(env,c->buffer.count); jclass array=(*env)->FindClass(env,"[B"); jobjectArray images=(*env)->NewObjectArray(env,c->image_count,array,NULL); (*env)->DeleteLocalRef(env,array);
    if (!commands || !images) stop("Allocation failed","Image context JNI");
    if (c->buffer.count) (*env)->SetDoubleArrayRegion(env,commands,0,c->buffer.count,c->buffer.values);
    for (size_t i=0;i<c->image_count;++i) {
        struct HostObject *data=(void *)image_png((void *)c->images[i]); if (!data || data->content[0]>INT_MAX) stop("Invalid image source","Image context");
        jbyteArray bytes=(*env)->NewByteArray(env,data->content[0]); if (!bytes) stop("Allocation failed","Image context source");
        (*env)->SetByteArrayRegion(env,bytes,0,data->content[0],(void *)data_buffer(data)); (*env)->SetObjectArrayElement(env,images,i,bytes); (*env)->DeleteLocalRef(env,bytes);
    }
    jbyteArray bytes=(*env)->CallObjectMethod(env,activity,method,(jint)ceil(c->size.width*c->scale),(jint)ceil(c->size.height*c->scale),(jdouble)c->scale,(jboolean)c->opaque,commands,images);
    (*env)->DeleteLocalRef(env,commands); (*env)->DeleteLocalRef(env,images);
    Word data=drawing_data_from_java(env,bytes,"Image context render"); Word image=image_from_data(external_class("_OBJC_CLASS_$_UIImage"),"Image context",(void *)data);
    if (image) { named_image_state(image)->scale=c->scale; c->cached=image; c->cached_count=c->buffer.count; } return image;
}
static Word image_data_convert(Word object,double quality,int jpeg) {
    struct HostObject *data=(void *)object; if (!data || data->content[0]>INT_MAX) return 0;
    JNIEnv *env=attached_environment("Image representation"); static jmethodID method;
    if (!method) { jclass cls=(*env)->GetObjectClass(env,activity); method=(*env)->GetMethodID(env,cls,"convertGuestImage","([BZI)[B"); (*env)->DeleteLocalRef(env,cls); }
    jbyteArray input=(*env)->NewByteArray(env,data->content[0]); if (!input) stop("Allocation failed","Image representation");
    (*env)->SetByteArrayRegion(env,input,0,data->content[0],(void *)data_buffer(data));
    jbyteArray output=(*env)->CallObjectMethod(env,activity,method,input,(jboolean)jpeg,(jint)lround(fmax(0,fmin(1,quality))*100)); (*env)->DeleteLocalRef(env,input);
    return drawing_data_from_java(env,output,"Image representation");
}
static Word image_data_normalize(Word object) { return image_data_convert(object,1,0); }
static Word image_jpeg(Word object,double quality) { return object ? image_data_convert(image_png((void *)object),quality,1) : 0; }
static Word drawing_font_create(Word name) {
    if (!name) return 0; JNIEnv *env=attached_environment("Font tables"); static jmethodID method;
    if (!method) { jclass cls=(*env)->GetObjectClass(env,activity); method=(*env)->GetMethodID(env,cls,"guestFontBytes","(Ljava/lang/String;)[B"); (*env)->DeleteLocalRef(env,cls); }
    jstring text=http_java_string(env,name); jbyteArray bytes=(*env)->CallObjectMethod(env,activity,method,text); (*env)->DeleteLocalRef(env,text);
    Word data=drawing_data_from_java(env,bytes,"Font tables"); if (!data) return 0;
    struct DrawingFont *font=calloc(1,sizeof(*font)); if (!font) stop("Allocation failed","CGFont"); font->tag=(Word)&drawing_font_tag; font->references=1; font->data=data; return (Word)font;
}
static Word drawing_font_table(Word object,uint32_t tag) {
    struct DrawingFont *font=(void *)object; if (!font || font->tag!=(Word)&drawing_font_tag) return 0;
    struct HostObject *data=(void *)font->data; size_t size=data->content[0]; const unsigned char *bytes=(void *)data_buffer(data); size_t offset=0;
    if (size>=16 && !memcmp(bytes,"ttcf",4)) offset=big_endian32(bytes+12);
    if (offset>size || size-offset<12) return 0;
    unsigned count=((unsigned)bytes[offset+4]<<8)|bytes[offset+5]; if (count>(size-offset-12)/16) return 0;
    for (unsigned i=0;i<count;++i) { const unsigned char *entry=bytes+offset+12+i*16; if (big_endian32(entry)!=tag) continue; size_t start=big_endian32(entry+8),length=big_endian32(entry+12); return start<=size && length<=size-start ? data_with_bytes(external_class("_OBJC_CLASS_$_NSData"),"CGFontCopyTableForTag",bytes+start,length) : 0; }
    return 0;
}
static Word geometry_export(const char *name) {
    if (!strcmp(name,"_CATransform3DIdentity")) return (Word)&transform3d_identity;
#define GEOMETRY(api,function) if (!strcmp(name,"_" #api)) return (Word)function
    GEOMETRY(CATransform3DMakeScale,transform3d_scale); GEOMETRY(CATransform3DMakeTranslation,transform3d_translation);
    GEOMETRY(CATransform3DConcat,transform3d_concat); GEOMETRY(CACurrentMediaTime,ca_current_time);
    GEOMETRY(CLLocationCoordinate2DMake,location_coordinate_make);
    GEOMETRY(CGColorSpaceCreateDeviceRGB,color_space_device_rgb); GEOMETRY(CGColorSpaceRelease,color_space_release);
    GEOMETRY(CGColorSpaceCreateDeviceGray,color_space_device_gray);
    GEOMETRY(CGPathCreateMutable,drawing_path_create); GEOMETRY(CGPathCreateCopy,drawing_path_copy); GEOMETRY(CGPathCreateMutableCopy,drawing_path_mutable_copy);
    GEOMETRY(CGPathRetain,drawing_resource_keep); GEOMETRY(CGPathRelease,drawing_resource_drop); GEOMETRY(CGPathMoveToPoint,drawing_path_move); GEOMETRY(CGPathAddLineToPoint,drawing_path_line);
    GEOMETRY(CGPathAddCurveToPoint,drawing_path_curve); GEOMETRY(CGPathAddQuadCurveToPoint,drawing_path_quad); GEOMETRY(CGPathCloseSubpath,drawing_path_close); GEOMETRY(CGPathAddLines,drawing_path_lines);
    GEOMETRY(CGPathAddArc,drawing_path_arc); GEOMETRY(CGPathAddArcToPoint,drawing_path_arc_to); GEOMETRY(CGPathAddPath,drawing_path_add); GEOMETRY(CGPathAddRect,drawing_path_rect); GEOMETRY(CGPathGetCurrentPoint,drawing_path_current); GEOMETRY(CGPathIsEmpty,drawing_path_empty);
    GEOMETRY(CGContextSaveGState,drawing_save); GEOMETRY(CGContextRestoreGState,drawing_restore); GEOMETRY(CGContextConcatCTM,drawing_transform); GEOMETRY(CGContextTranslateCTM,drawing_translate); GEOMETRY(CGContextScaleCTM,drawing_scale); GEOMETRY(CGContextRotateCTM,drawing_rotate);
    GEOMETRY(CGContextSetRGBFillColor,drawing_fill); GEOMETRY(CGContextSetRGBStrokeColor,drawing_stroke); GEOMETRY(CGContextSetGrayFillColor,drawing_fill_gray); GEOMETRY(CGContextSetGrayStrokeColor,drawing_stroke_gray); GEOMETRY(CGContextSetFillColorWithColor,drawing_fill_color); GEOMETRY(CGContextSetStrokeColorWithColor,drawing_stroke_color);
    GEOMETRY(CGContextSetLineWidth,drawing_line_width); GEOMETRY(CGContextSetLineJoin,drawing_line_join); GEOMETRY(CGContextSetMiterLimit,drawing_miter); GEOMETRY(CGContextSetAlpha,drawing_alpha); GEOMETRY(CGContextSetBlendMode,drawing_blend); GEOMETRY(CGContextSetShadowWithColor,drawing_shadow); GEOMETRY(CGContextSetLineDash,drawing_dash);
    GEOMETRY(CGContextBeginPath,drawing_begin); GEOMETRY(CGContextClosePath,drawing_close); GEOMETRY(CGContextMoveToPoint,drawing_move); GEOMETRY(CGContextAddLineToPoint,drawing_line); GEOMETRY(CGContextAddCurveToPoint,drawing_curve); GEOMETRY(CGContextAddQuadCurveToPoint,drawing_quad);
    GEOMETRY(CGContextAddArc,drawing_arc); GEOMETRY(CGContextAddArcToPoint,drawing_arc_to); GEOMETRY(CGContextAddPath,drawing_add_path); GEOMETRY(CGContextAddLines,drawing_add_lines); GEOMETRY(CGContextAddRect,drawing_add_rect);
    GEOMETRY(CGContextDrawPath,drawing_paint); GEOMETRY(CGContextFillPath,drawing_fill_path); GEOMETRY(CGContextEOFillPath,drawing_even_fill_path); GEOMETRY(CGContextStrokePath,drawing_stroke_path); GEOMETRY(CGContextClip,drawing_clip); GEOMETRY(CGContextEOClip,drawing_even_clip); GEOMETRY(CGContextClipToRect,drawing_clip_rect);
    GEOMETRY(CGContextFillRect,drawing_fill_rect); GEOMETRY(CGContextStrokeRect,drawing_stroke_rect); GEOMETRY(CGContextClearRect,drawing_clear_rect); GEOMETRY(CGContextFillEllipseInRect,drawing_fill_ellipse); GEOMETRY(CGContextStrokeEllipseInRect,drawing_stroke_ellipse);
    GEOMETRY(CGGradientCreateWithColorComponents,drawing_gradient_create); GEOMETRY(CGGradientCreateWithColors,drawing_gradient_colors); GEOMETRY(CGGradientRetain,drawing_resource_keep); GEOMETRY(CGGradientRelease,drawing_resource_drop); GEOMETRY(CGContextDrawLinearGradient,drawing_linear_gradient); GEOMETRY(CGContextDrawRadialGradient,drawing_radial_gradient);
    GEOMETRY(CGContextDrawImage,drawing_image); GEOMETRY(CGContextClipToMask,drawing_mask); GEOMETRY(CGContextBeginTransparencyLayer,drawing_layer_begin); GEOMETRY(CGContextEndTransparencyLayer,drawing_layer_end);
    GEOMETRY(UIGraphicsBeginImageContext,image_context_begin_plain); GEOMETRY(UIGraphicsBeginImageContextWithOptions,image_context_begin); GEOMETRY(UIGraphicsEndImageContext,image_context_end); GEOMETRY(UIGraphicsGetCurrentContext,image_context_current); GEOMETRY(UIGraphicsGetImageFromCurrentImageContext,image_context_image); GEOMETRY(UIImageJPEGRepresentation,image_jpeg);
    GEOMETRY(CGFontCreateWithFontName,drawing_font_create); GEOMETRY(CGFontCopyTableForTag,drawing_font_table); GEOMETRY(CGFontRetain,drawing_resource_keep); GEOMETRY(CGFontRelease,drawing_resource_drop);
    GEOMETRY(CGColorGetComponents,color_components); GEOMETRY(CGColorGetNumberOfComponents,color_component_count); GEOMETRY(CGColorGetAlpha,color_alpha);
    GEOMETRY(CGRectGetMinX,rect_min_x); GEOMETRY(CGRectGetMaxX,rect_max_x); GEOMETRY(CGRectGetMidX,rect_mid_x);
    GEOMETRY(CGRectGetMinY,rect_min_y); GEOMETRY(CGRectGetMaxY,rect_max_y); GEOMETRY(CGRectGetMidY,rect_mid_y);
    GEOMETRY(CGRectGetWidth,rect_width); GEOMETRY(CGRectGetHeight,rect_height); GEOMETRY(CGRectIsNull,rect_null); GEOMETRY(CGRectIsEmpty,rect_empty);
    GEOMETRY(CGRectStandardize,rect_standard); GEOMETRY(CGRectEqualToRect,rect_equal); GEOMETRY(CGPointEqualToPoint,point_equal); GEOMETRY(CGSizeEqualToSize,size_equal);
    GEOMETRY(CGRectIntegral,rect_integral);
    GEOMETRY(CGRectInset,rect_inset); GEOMETRY(CGRectOffset,rect_offset); GEOMETRY(CGRectIntersection,rect_intersection);
    GEOMETRY(CGRectIntersectsRect,rect_intersects); GEOMETRY(CGRectContainsPoint,rect_contains_point);
    GEOMETRY(CGAffineTransformMake,affine_make); GEOMETRY(CGAffineTransformMakeScale,affine_scale); GEOMETRY(CGAffineTransformMakeTranslation,affine_translation);
    GEOMETRY(CGAffineTransformMakeRotation,affine_rotation); GEOMETRY(CGAffineTransformConcat,affine_concat); GEOMETRY(CGAffineTransformTranslate,affine_translate);
    GEOMETRY(CGAffineTransformScale,affine_scaled); GEOMETRY(CGAffineTransformRotate,affine_rotated); GEOMETRY(CGPointApplyAffineTransform,affine_point); GEOMETRY(CGSizeApplyAffineTransform,affine_size);
#undef GEOMETRY
    return 0;
}
