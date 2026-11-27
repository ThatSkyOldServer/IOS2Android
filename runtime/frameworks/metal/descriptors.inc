 
#include "descriptor_schema.h"
struct MetalDescriptor {
    Word isa;
    enum MetalDescriptorKind kind;
    Word properties[COUNT(metal_properties)];
    Word colors,attributes,layouts,depth,stencil,front,back;
    double clear_color[4],clear_depth;
    float lod_min,lod_max;
    struct MetalLifetime lifetime;
};
struct MetalDescriptorArray { Word isa; enum MetalDescriptorKind item_kind; unsigned count; Word items[31]; struct MetalLifetime lifetime; };
static pthread_once_t metal_descriptor_references_once=PTHREAD_ONCE_INIT;
static int metal_descriptor_references_ready;
static unsigned char metal_descriptor_reference_flags[COUNT(metal_properties)];
static unsigned metal_descriptor_reference_indices[COUNT(metal_properties)],metal_descriptor_reference_count;
static void metal_descriptor_references_init(void) {
    for (unsigned i=0;i<COUNT(metal_properties);++i) if (metal_properties[i].reference) {
        metal_descriptor_reference_flags[i]=1;
        metal_descriptor_reference_indices[metal_descriptor_reference_count++]=i;
    }
    __atomic_store_n(&metal_descriptor_references_ready,1,__ATOMIC_RELEASE);
}
static void metal_descriptor_destroy(Word value) {
    struct MetalDescriptor *object=(void *)value;
    for (unsigned i=0;i<metal_descriptor_reference_count;++i)
        metal_object_release(object->properties[metal_descriptor_reference_indices[i]]);
    Word children[]={object->colors,object->attributes,object->layouts,object->depth,object->stencil,object->front,object->back};
    for (unsigned i=0;i<COUNT(children);++i) metal_object_release(children[i]);
    metal_memory_sub(2,sizeof(*object)); free(object);
}
static void metal_descriptor_array_destroy(Word value) {
    struct MetalDescriptorArray *array=(void *)value;
    for (unsigned i=0;i<array->count;++i) metal_object_release(array->items[i]);
    metal_memory_sub(2,sizeof(*array)); free(array);
}
static int metal_descriptor_kind(const char *receiver) {
    if (!receiver || strncmp(receiver,"_OBJC_CLASS_$_",14)) return -1;
    for (unsigned i=0;i<MD_COUNT;++i) if (!strcmp(receiver+14,metal_descriptor_names[i])) return (int)i;
    return -1;
}
static int metal_property_index(const char *name) {
    for (unsigned i=0;i<COUNT(metal_properties);++i) if (!strcmp(name,metal_properties[i].name)) return (int)i;
    return -1;
}
static Word metal_property(struct MetalDescriptor *object,const char *name) {
    int index=metal_property_index(name);
    if (index<0) stop("Unknown Metal descriptor property",name);
    return object->properties[index];
}
static void metal_property_assign(struct MetalDescriptor *object,const char *name,Word value) {
    int index=metal_property_index(name);
    if (index<0) stop("Unknown Metal descriptor property",name);
    object->properties[index]=value;
}
static Word metal_descriptor_create(unsigned kind) {
    if (kind>=MD_COUNT) stop("Unknown Metal descriptor","create");
    struct MetalDescriptor *object=calloc(1,sizeof(*object));
    if (!object) stop("Allocation failed",metal_descriptor_names[kind]);
    metal_memory_add(2,sizeof(*object));
    object->isa=class_from_name(metal_descriptor_names[kind]); object->kind=kind;
    if (!object->isa) stop("Missing descriptor class identity",metal_descriptor_names[kind]);
    if (!__atomic_load_n(&metal_descriptor_references_ready,__ATOMIC_ACQUIRE))
        pthread_once(&metal_descriptor_references_once,metal_descriptor_references_init);
    metal_lifetime_init((Word)object,__builtin_offsetof(struct MetalDescriptor,lifetime),metal_descriptor_destroy);
    const struct MetalDefaultRange range=metal_default_ranges[kind];
    for (unsigned i=range.first;i<range.first+range.count;++i)
        object->properties[metal_defaults[i].property]=metal_defaults[i].value;
    if (kind==MD_SAMPLER) object->lod_max=3.402823466e38F;
    if (kind==MD_PASS_COLOR) object->clear_color[3]=1;
    object->clear_depth=1;
    return (Word)object;
}
static Word metal_descriptor_factory(Word cls,const char *selector) {
    Word object=metal_descriptor_create((unsigned)metal_descriptor_kind(host_symbol(cls)));
    return !strcmp(selector,"alloc") || !strcmp(selector,"allocWithZone:") || !strcmp(selector,"new") ? object : metal_object_autorelease(object);
}
static Word metal_descriptor_array(unsigned kind,unsigned count) {
    struct MetalDescriptorArray *array=calloc(1,sizeof(*array));
    if (!array) stop("Allocation failed","Metal descriptor array");
    metal_memory_add(2,sizeof(*array));
    array->isa=external_class("_OBJC_CLASS_$_I2AMetalDescriptorArray"); array->item_kind=kind; array->count=count;
    metal_lifetime_init((Word)array,__builtin_offsetof(struct MetalDescriptorArray,lifetime),metal_descriptor_array_destroy);
    return (Word)array;
}
static Word metal_descriptor_at(struct MetalDescriptorArray *array,const char *selector,Word index) {
    if (index>=array->count) stop("Metal descriptor index out of bounds",selector);
    if (!array->items[index]) array->items[index]=metal_descriptor_create(array->item_kind);
    return array->items[index];
}
static void metal_descriptor_set_at(struct MetalDescriptorArray *array,const char *selector,Word value,Word index) {
    if (index>=array->count || (value && metal_descriptor_kind(class_name(read_word(value)))!=(int)array->item_kind)) stop("Invalid Metal descriptor array assignment",selector);
    store_strong(&array->items[index],value);
}
static Word metal_descriptor_children(struct MetalDescriptor *object,const char *selector) {
    Word *slot=NULL; unsigned kind=0,count=0;
    if (!strcmp(selector,"colorAttachments")) { slot=&object->colors; kind=object->kind==MD_PIPELINE ? MD_PIPELINE_COLOR : MD_PASS_COLOR; count=8; }
    else if (!strcmp(selector,"attributes")) { slot=&object->attributes; kind=MD_ATTRIBUTE; count=31; }
    else if (!strcmp(selector,"layouts")) { slot=&object->layouts; kind=MD_LAYOUT; count=31; }
    else if (!strcmp(selector,"depthAttachment")) { slot=&object->depth; kind=MD_PASS_DEPTH; }
    else if (!strcmp(selector,"stencilAttachment")) { slot=&object->stencil; kind=MD_PASS_STENCIL; }
    else if (!strcmp(selector,"frontFaceStencil")) { slot=&object->front; kind=MD_STENCIL; }
    else if (!strcmp(selector,"backFaceStencil")) { slot=&object->back; kind=MD_STENCIL; }
    if (!slot) stop("Unknown descriptor child",selector);
    if (!*slot) *slot=count ? metal_descriptor_array(kind,count) : metal_descriptor_create(kind);
    return *slot;
}
static Word metal_descriptor_get(struct MetalDescriptor *object,const char *selector) {
    if (object->kind==MD_PIPELINE && !strcmp(selector,"vertexDescriptor") && !metal_property(object,selector))
        metal_property_assign(object,selector,metal_descriptor_create(MD_VERTEX));
    return metal_property(object,selector);
}
static int metal_setter_property(const char *selector,char name[96]) {
    size_t size=strlen(selector);
    if (size<5 || size>98 || strncmp(selector,"set",3) || selector[size-1]!=':') return -1;
    memcpy(name,selector+3,size-4); name[size-4]=0;
    if (name[0]>='A' && name[0]<='Z') name[0]+='a'-'A';
    return metal_property_index(name);
}
static void metal_descriptor_set(struct MetalDescriptor *object,const char *selector,Word value) {
    char name[96]; int index=metal_setter_property(selector,name);
    if (index<0) stop("Unknown Metal descriptor setter",selector);
    if (metal_descriptor_reference_flags[index]) store_strong(&object->properties[index],value);
    else object->properties[index]=value;
    if (object->kind==MD_TEXTURE) {
        if (!strcmp(name,"resourceOptions")) {
            metal_property_assign(object,"cpuCacheMode",value&15); metal_property_assign(object,"storageMode",(value>>4)&15);
        } else if (!strcmp(name,"storageMode")) metal_property_assign(object,"resourceOptions",(metal_property(object,"resourceOptions")&~(Word)0xf0)|(value<<4));
        else if (!strcmp(name,"cpuCacheMode")) metal_property_assign(object,"resourceOptions",(metal_property(object,"resourceOptions")&~(Word)15)|value);
    }
}
struct MetalClearColor { double red,green,blue,alpha; };
_Static_assert(sizeof(struct MetalClearColor)==32, "Metal clear color HFA ABI");
static void metal_descriptor_clear_color(struct MetalDescriptor *object,const char *selector,struct MetalClearColor color) { memcpy(object->clear_color,&color,sizeof(color)); }
static struct MetalClearColor metal_descriptor_color(struct MetalDescriptor *object) { struct MetalClearColor color; memcpy(&color,object->clear_color,sizeof(color)); return color; }
static void metal_descriptor_clear_depth(struct MetalDescriptor *object,const char *selector,double depth) { object->clear_depth=depth; }
static double metal_descriptor_depth(struct MetalDescriptor *object) { return object->clear_depth; }
static void metal_descriptor_set_lod(struct MetalDescriptor *object,const char *selector,float value) { if (!strcmp(selector,"setLodMinClamp:")) object->lod_min=value; else object->lod_max=value; }
static float metal_descriptor_lod(struct MetalDescriptor *object,const char *selector) { return !strcmp(selector,"lodMinClamp") ? object->lod_min : object->lod_max; }
static Word metal_texture_2d(Word cls,const char *selector,Word format,Word width,Word height,unsigned char mipmapped) {
    struct MetalDescriptor *object=(void *)metal_descriptor_create(MD_TEXTURE);
    metal_property_assign(object,"pixelFormat",format); metal_property_assign(object,"width",width); metal_property_assign(object,"height",height);
    Word levels=1,maximum=width>height ? width : height;
    if (mipmapped) while (maximum>1) { ++levels; maximum>>=1; }
    metal_property_assign(object,"mipmapLevelCount",levels); return metal_object_autorelease((Word)object);
}
static Method metal_descriptors_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver) return NULL;
    if (!class_receiver && !strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDescriptorArray")) {
        if (!strcmp(selector,"objectAtIndexedSubscript:")) return (Method)metal_descriptor_at;
        if (!strcmp(selector,"setObject:atIndexedSubscript:")) return (Method)metal_descriptor_set_at;
        return NULL;
    }
    int kind=metal_descriptor_kind(receiver);
    if (kind<0) return NULL;
    if (class_receiver) {
        if (!strcmp(selector,"alloc") || !strcmp(selector,"allocWithZone:") || !strcmp(selector,"new") ||
            (kind==MD_PASS && !strcmp(selector,"renderPassDescriptor")) || (kind==MD_VERTEX && !strcmp(selector,"vertexDescriptor"))) return (Method)metal_descriptor_factory;
        if (kind==MD_TEXTURE && !strcmp(selector,"texture2DDescriptorWithPixelFormat:width:height:mipmapped:")) return (Method)metal_texture_2d;
        return NULL;
    }
    int property=metal_property_index(selector); char name[96];
    if (property>=0 && (metal_properties[property].kinds&(1U<<kind))) return (Method)metal_descriptor_get;
    property=metal_setter_property(selector,name);
    if (property>=0 && (metal_properties[property].kinds&(1U<<kind))) return (Method)metal_descriptor_set;
    if (((kind==MD_PIPELINE || kind==MD_PASS) && !strcmp(selector,"colorAttachments")) ||
        (kind==MD_PASS && (!strcmp(selector,"depthAttachment") || !strcmp(selector,"stencilAttachment"))) ||
        (kind==MD_VERTEX && (!strcmp(selector,"attributes") || !strcmp(selector,"layouts"))) ||
        (kind==MD_DEPTH && (!strcmp(selector,"frontFaceStencil") || !strcmp(selector,"backFaceStencil")))) return (Method)metal_descriptor_children;
    if (kind==MD_PASS_COLOR && !strcmp(selector,"setClearColor:")) return (Method)metal_descriptor_clear_color;
    if (kind==MD_PASS_COLOR && !strcmp(selector,"clearColor")) return (Method)metal_descriptor_color;
    if (kind==MD_PASS_DEPTH && !strcmp(selector,"setClearDepth:")) return (Method)metal_descriptor_clear_depth;
    if (kind==MD_PASS_DEPTH && !strcmp(selector,"clearDepth")) return (Method)metal_descriptor_depth;
    if (kind==MD_SAMPLER && (!strcmp(selector,"setLodMinClamp:") || !strcmp(selector,"setLodMaxClamp:"))) return (Method)metal_descriptor_set_lod;
    if (kind==MD_SAMPLER && (!strcmp(selector,"lodMinClamp") || !strcmp(selector,"lodMaxClamp"))) return (Method)metal_descriptor_lod;
    return NULL;
}
