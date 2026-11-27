/* Exact original metallib identities select offline GLSL->SPIR-V candidates.
 * Reflection describes the original AIR ABI, not a newly invented UBO layout. */
struct ShaderMemberSpec { const char *name; uint32_t type,offset,size; };
struct ShaderArgumentSpec { const char *name; uint32_t kind,index,type,size,alignment,texture_type,access,active,first_member,member_count; };
struct ShaderAttributeSpec { const char *name; uint32_t index,type,active; };
/* sampled_components is an optional build-time proof. Zero means unknown (all). */
struct ShaderBindingSpec { uint32_t set,binding,type,metal_index,size,metal_size,literal_sampler,sampled_components; };
struct ShaderSamplerSpec { uint32_t min_filter,mag_filter,mip_filter,address_u,address_v,address_w,border; float lod_min,lod_max; };
struct ShaderInterfaceSpec { const char *name,*type; uint32_t location; };
struct ShaderSpec {
    const char *name,*entry,*spirv_entry;
    uint32_t stage,spirv_offset,spirv_size,first_argument,argument_count,first_attribute,attribute_count,
        first_binding,binding_count,first_input,input_count,first_output,output_count,fallback_offset,fallback_size,
        requirements,fallback_requirements;
    uint32_t workgroup[3],workgroup_ids[3];
};
struct ShaderLibrarySpec { const char *name; unsigned char sha256[32]; uint32_t size,first_entry,entry_count,source; };
#include "shaders.h"
_Static_assert(I2A_SHADER_PACK_VERSION == 2, "Shader pack ABI mismatch");
static const unsigned char *guest_spirv_data;
static _Atomic(char *) shader_cache_directory;
static AAsset *shader_pack_asset;
static pthread_once_t shader_pack_once=PTHREAD_ONCE_INIT;
static void shader_pack_initialize(void){
    JNIEnv *env=attached_environment("Shader cache");jclass type=(*env)->GetObjectClass(env,activity);
    jmethodID method=(*env)->GetMethodID(env,type,"shaderCacheDirectory","(J)Ljava/lang/String;");
    jstring directory=(*env)->CallObjectMethod(env,activity,method,(jlong)I2A_SHADER_PACK_BYTES);(*env)->DeleteLocalRef(env,type);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);directory=NULL;}
    if(directory){const char *path=(*env)->GetStringUTFChars(env,directory,NULL);if(path){shader_cache_directory=strdup(path);(*env)->ReleaseStringUTFChars(env,directory,path);}(*env)->DeleteLocalRef(env,directory);}
    if(!I2A_SHADER_PACK_BYTES)return;
    int cached=0;
    if(shader_cache_directory){char *path=NULL;if(asprintf(&path,"%s/shaders.bin",shader_cache_directory)>=0){int fd=open(path,O_RDONLY|O_CLOEXEC);free(path);if(fd>=0){struct stat info;if(!fstat(fd,&info) && info.st_size==I2A_SHADER_PACK_BYTES){void *mapped=mmap(NULL,I2A_SHADER_PACK_BYTES,PROT_READ,MAP_PRIVATE,fd,0);if(mapped!=MAP_FAILED){guest_spirv_data=mapped;cached=1;}}close(fd);}}}
    if(!guest_spirv_data){shader_pack_asset=AAssetManager_open(bundle_assets,"i2a/shaders.bin",AASSET_MODE_BUFFER);if(shader_pack_asset && AAsset_getLength64(shader_pack_asset)==I2A_SHADER_PACK_BYTES)guest_spirv_data=AAsset_getBuffer(shader_pack_asset);}
    if(!guest_spirv_data)stop("Shader pack unavailable","Vulkan resource cache");
    LOG("Vulkan shader pack loaded from %s (%u bytes)",cached ? "private cache" : "APK",(unsigned)I2A_SHADER_PACK_BYTES);
}
static const char *shader_cache_root(void){pthread_once(&shader_pack_once,shader_pack_initialize);return shader_cache_directory;}
static unsigned char *guest_sha256(const void *,uint32_t,unsigned char *);
struct MetalLibrary { struct MetalResource object; const struct ShaderLibrarySpec *spec; };
struct MetalFunction { struct MetalResource object; const struct ShaderSpec *spec; VkShaderModule module; Word attributes,arguments; unsigned compute_fallback,requirements; };
struct MetalReflectionNode { Word isa; const void *spec; Word name,children; };
static struct MetalFunction *metal_function_cache[COUNT(shader_specs)];
static pthread_mutex_t metal_shader_mutex=PTHREAD_MUTEX_INITIALIZER;

static Word metal_error(Word *error,const char *domain,long code,const char *detail) {
    record_failure(domain,detail);
    LOG("%s (%ld): %s",domain,code,detail);
    if (error) {
        struct GuestMap *info=(void *)empty_map(external_class("_OBJC_CLASS_$_NSDictionary"));
        map_set(info,"Metal error",copy_string(detail),copy_string("NSLocalizedDescription"));
        *error=error_with_domain(external_class("_OBJC_CLASS_$_NSError"),"Metal error",copy_string(domain),code,(Word)info);
    }
    return 0;
}
static Word metal_library_data(Word device,const char *selector,void *data,Word *error) {
    if (error) *error=0;
    if (!vulkan.available) return metal_error(error,"MTLLibraryErrorDomain",1,"Vulkan shader backend unavailable");
    if (!data) return metal_error(error,"MTLLibraryErrorDomain",1,"No Metal library data");
    void *(*create_map)(void *,const void **,size_t *)=dispatch_export("dispatch_data_create_map");
    void (*release)(void *)=dispatch_export("dispatch_release");
    if (!create_map || !release) stop("Missing libdispatch data mapping",selector);
    const void *bytes=NULL; size_t size=0;
    void *mapped=create_map(data,&bytes,&size);
    if (!mapped || !bytes || size<4 || size>UINT32_MAX || memcmp(bytes,"MTLB",4)) {
        if (mapped) release(mapped);
        return metal_error(error,"MTLLibraryErrorDomain",1,"Invalid original metallib container");
    }
    unsigned char digest[32]; guest_sha256(bytes,(uint32_t)size,digest);
    release(mapped);
    const struct ShaderLibrarySpec *spec=NULL;
    for (unsigned i=0;i<COUNT(shader_libraries);++i)
        if (!shader_libraries[i].source && shader_libraries[i].size==size && !memcmp(shader_libraries[i].sha256,digest,32)) { spec=&shader_libraries[i]; break; }
    if (!spec) return metal_error(error,"MTLLibraryErrorDomain",1,"Original metallib hash has no matching translation");
    if (!spec->entry_count) {
        char reason[256]; snprintf(reason,sizeof(reason),"Portable translation still required for %s",spec->name);
        return metal_error(error,"MTLLibraryErrorDomain",1,reason);
    }
    struct MetalLibrary *library=calloc(1,sizeof(*library));
    if (!library) return metal_error(error,"MTLLibraryErrorDomain",1,"Cannot allocate library object");
    library->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalLibrary"),.device=device};
    library->spec=spec;
    LOG_LIMITED(32,"Original Metal library matched: %s (%u bytes)",spec->name,spec->size);
    return (Word)library;
}
static Word metal_library_source(Word device,const char *selector,Word source,Word options,Word *error) {
    if (error) *error=0;
    if (!vulkan.available || !source) return metal_error(error,"MTLLibraryErrorDomain",3,"Missing Vulkan backend or Metal source");
     
    if (options) return metal_error(error,"MTLLibraryErrorDomain",3,"Nondefault Metal source options require a matching offline compilation");
    const char *bytes=string_bytes(source,selector); size_t size=strlen(bytes);
    if (size>UINT32_MAX) return metal_error(error,"MTLLibraryErrorDomain",3,"Metal source exceeds hash input limit");
    unsigned char digest[32]; guest_sha256(bytes,(uint32_t)size,digest);
    const struct ShaderLibrarySpec *spec=NULL;
    for (unsigned i=0;i<COUNT(shader_libraries);++i)
        if (shader_libraries[i].source && shader_libraries[i].size==size && !memcmp(shader_libraries[i].sha256,digest,32)) { spec=&shader_libraries[i]; break; }
    if (!spec) return metal_error(error,"MTLLibraryErrorDomain",3,"Metal source has no exact offline SPIR-V translation");
    struct MetalLibrary *library=calloc(1,sizeof(*library));
    if (!library) return metal_error(error,"MTLLibraryErrorDomain",3,"Cannot allocate source library object");
    library->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalLibrary"),.device=device};
    library->spec=spec;
    LOG("Original embedded Metal source matched: %s (%u bytes)",spec->name,spec->size);
    return (Word)library;
}
static struct MetalReflectionNode *metal_reflection_node(const char *cls,const void *spec,const char *name) {
    struct MetalReflectionNode *node=calloc(1,sizeof(*node));
    if (!node) stop("Allocation failed","Metal reflection");
    node->isa=external_class(cls); node->spec=spec; node->name=name ? copy_string(name) : 0;
    return node;
}
static Word metal_member_nodes(const struct ShaderArgumentSpec *argument) {
    Word *items=calloc(argument->member_count ? argument->member_count : 1,sizeof(Word));
    if (!items) stop("Allocation failed","Metal struct members");
    for (unsigned i=0;i<argument->member_count;++i) {
        const struct ShaderMemberSpec *spec=&shader_members[argument->first_member+i];
        items[i]=(Word)metal_reflection_node("_OBJC_CLASS_$_MTLStructMember",spec,spec->name);
    }
    Word result=new_array(external_class("_OBJC_CLASS_$_NSArray"),"Metal struct members",items,argument->member_count);
    free(items); return result;
}
static void metal_function_reflection(struct MetalFunction *function) {
    const struct ShaderSpec *spec=function->spec;
    Word *items=calloc(spec->attribute_count+spec->argument_count+1,sizeof(Word));
    if (!items) stop("Allocation failed","Metal function reflection");
    for (unsigned i=0;i<spec->attribute_count;++i) {
        const struct ShaderAttributeSpec *attribute=&shader_attributes[spec->first_attribute+i];
        items[i]=(Word)metal_reflection_node("_OBJC_CLASS_$_MTLVertexAttribute",attribute,attribute->name);
    }
    function->attributes=new_array(external_class("_OBJC_CLASS_$_NSArray"),"Metal vertex attributes",items,spec->attribute_count);
    for (unsigned i=0;i<spec->argument_count;++i) {
        const struct ShaderArgumentSpec *argument=&shader_arguments[spec->first_argument+i];
        struct MetalReflectionNode *node=metal_reflection_node("_OBJC_CLASS_$_MTLArgument",argument,argument->name);
        if (argument->kind==0) {
            struct MetalReflectionNode *structure=metal_reflection_node("_OBJC_CLASS_$_MTLStructType",argument,NULL);
            structure->children=metal_member_nodes(argument); node->children=(Word)structure;
        }
        items[i]=(Word)node;
    }
    function->arguments=new_array(external_class("_OBJC_CLASS_$_NSArray"),"Metal arguments",items,spec->argument_count);
    free(items);
}
static Word metal_library_function(struct MetalLibrary *library,const char *selector,Word name) {
    const char *entry=string_bytes(name,selector);
    const struct ShaderSpec *spec=NULL;
    for (unsigned i=0;i<library->spec->entry_count;++i) {
        const struct ShaderSpec *candidate=&shader_specs[library->spec->first_entry+i];
        if (!strcmp(entry,candidate->entry)) { spec=candidate; break; }
    }
    if (!spec) { LOG("No original Metal function %s in %s",entry,library->spec->name); return 0; }
#if I2A_DEVELOPMENT && I2A_SHADER_DIAGNOSTIC_COUNT
    LOG("[BUSINESS-shader] loaded=%s stage=%u",spec->name,spec->stage);
    if (spec->stage==2 && !vulkan.features.fragmentStoresAndAtomics)
        for (unsigned i=0;i<spec->binding_count;++i) {
            const struct ShaderBindingSpec *binding=&shader_bindings[spec->first_binding+i];
            if (binding->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && binding->metal_index==UINT32_MAX-1) {
                LOG("Fragment diagnostics require unsupported Vulkan storage atomics: %s",spec->name); return 0;
            }
        }
#endif
    unsigned requirements=spec->requirements;
    unsigned compute_fallback=spec->stage==1 && (requirements&METAL_SHADER_VERTEX_STORES) &&
        !vulkan.features.vertexPipelineStoresAndAtomics && spec->fallback_size;
    if (compute_fallback) requirements=spec->fallback_requirements;
    unsigned missing=requirements&METAL_SHADER_FEATURE_MASK&~vulkan.shader_requirements;
    if (missing) {
        char detail[256];snprintf(detail,sizeof(detail),"shader=%s requested=0x%x enabled=0x%x missing=0x%x",
            spec->name,requirements&METAL_SHADER_FEATURE_MASK,vulkan.shader_requirements,missing);
        return metal_error(NULL,"MTLLibraryErrorDomain",1,detail);
    }
    unsigned index=(unsigned)(spec-shader_specs);
    pthread_once(&shader_pack_once,shader_pack_initialize);
    pthread_mutex_lock(&metal_shader_mutex);
    struct MetalFunction *function=metal_function_cache[index];
    if (!function) {
        function=calloc(1,sizeof(*function));
        if (function) {
            function->object=(struct MetalResource){.isa=external_class("_OBJC_CLASS_$_I2AMetalFunction"),.device=library->object.device};
            function->spec=spec;
            function->compute_fallback=compute_fallback; function->requirements=requirements;
            VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=spec->spirv_size,
                .pCode=(const uint32_t *)(guest_spirv_data+spec->spirv_offset)};
            if (compute_fallback) { info.codeSize=spec->fallback_size; info.pCode=(const uint32_t *)(guest_spirv_data+spec->fallback_offset); }
            PFN_vkCreateShaderModule create=(PFN_vkCreateShaderModule)vulkan.get_device_proc(vulkan.device,"vkCreateShaderModule");
            VkResult result=create(vulkan.device,&info,NULL,&function->module);
            if (result!=VK_SUCCESS) { LOG("Vulkan shader module failed: %s result=%d",spec->name,result); free(function); function=NULL; }
            else {
                metal_function_reflection(function); metal_function_cache[index]=function;
                LOG("Vulkan shader module ready: %s -> %s (%u bytes, %u bindings)",spec->name,spec->entry,spec->spirv_size,spec->binding_count);
            }
        }
    }
    pthread_mutex_unlock(&metal_shader_mutex); return (Word)function;
}
static Word metal_library_names(struct MetalLibrary *library) {
    Word *items=calloc(library->spec->entry_count+1,sizeof(Word));
    if (!items) stop("Allocation failed","Metal function names");
    for (unsigned i=0;i<library->spec->entry_count;++i) items[i]=copy_string(shader_specs[library->spec->first_entry+i].entry);
    Word result=new_array(external_class("_OBJC_CLASS_$_NSArray"),"Metal function names",items,library->spec->entry_count);
    free(items); return result;
}
static Word metal_function_name(struct MetalFunction *function) { return copy_string(function->spec->entry); }
static Word metal_function_type(struct MetalFunction *function) { return function->spec->stage; }
static Word metal_function_attributes(struct MetalFunction *function) { return function->spec->stage==1 ? function->attributes : 0; }
static Word metal_reflection_name(struct MetalReflectionNode *node) { return node->name; }
static Word metal_reflection_children(struct MetalReflectionNode *node) { return node->children; }
static Word metal_struct_member(struct MetalReflectionNode *node,const char *selector,Word name) {
    struct Sequence *members=(void *)node->children;
    for (unsigned i=0;members && i<members->count;++i) {
        struct MetalReflectionNode *member=(void *)members->items[i];
        if (!strcmp(string_bytes(member->name,selector),string_bytes(name,selector))) return (Word)member;
    }
    return 0;
}
static Word metal_argument_value(struct MetalReflectionNode *node,const char *selector) {
    const struct ShaderArgumentSpec *spec=node->spec;
    if (!strcmp(selector,"type")) return spec->kind;
    if (!strcmp(selector,"index")) return spec->index;
    if (!strcmp(selector,"access")) return spec->access;
    if (!strcmp(selector,"isActive")) return spec->active;
    if (!strcmp(selector,"bufferAlignment")) return spec->alignment;
    if (!strcmp(selector,"bufferDataSize")) return spec->size;
    if (!strcmp(selector,"textureType")) return spec->texture_type;
    return spec->type;
}
static Word metal_attribute_value(struct MetalReflectionNode *node,const char *selector) {
    const struct ShaderAttributeSpec *spec=node->spec;
    return !strcmp(selector,"attributeIndex") ? spec->index : !strcmp(selector,"isActive") ? spec->active : spec->type;
}
static Word metal_member_value(struct MetalReflectionNode *node,const char *selector) {
    const struct ShaderMemberSpec *spec=node->spec; return !strcmp(selector,"offset") ? spec->offset : spec->type;
}
static Method metal_shaders_method(Word object,const char *selector,const char *receiver,int class_receiver) {
    if (!receiver || class_receiver) return NULL;
    if (!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalDevice")) {
        if (!strcmp(selector,"newLibraryWithData:error:")) return (Method)metal_library_data;
        if (!strcmp(selector,"newLibraryWithSource:options:error:")) return (Method)metal_library_source;
    }
    int library=!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalLibrary"),function=!strcmp(receiver,"_OBJC_CLASS_$_I2AMetalFunction");
    if (library || function) {
        if (!strcmp(selector,"device")) return (Method)metal_resource_device;
        if (!strcmp(selector,"label")) return (Method)metal_resource_label;
        if (!strcmp(selector,"setLabel:")) return (Method)metal_resource_set_label;
        if (library && !strcmp(selector,"newFunctionWithName:")) return (Method)metal_library_function;
        if (library && !strcmp(selector,"functionNames")) return (Method)metal_library_names;
        if (function && !strcmp(selector,"name")) return (Method)metal_function_name;
        if (function && !strcmp(selector,"functionType")) return (Method)metal_function_type;
        if (function && (!strcmp(selector,"vertexAttributes") || !strcmp(selector,"stageInputAttributes"))) return (Method)metal_function_attributes;
    }
    int arg=!strcmp(receiver,"_OBJC_CLASS_$_MTLArgument"),attr=!strcmp(receiver,"_OBJC_CLASS_$_MTLVertexAttribute"),
        member=!strcmp(receiver,"_OBJC_CLASS_$_MTLStructMember"),structure=!strcmp(receiver,"_OBJC_CLASS_$_MTLStructType");
    if (arg || attr || member) {
        if (!strcmp(selector,"name")) return (Method)metal_reflection_name;
        if (arg) {
            if (!strcmp(selector,"bufferStructType")) return (Method)metal_reflection_children;
            const char *names[]={"type","index","access","isActive","bufferAlignment","bufferDataSize","bufferDataType","textureType","textureDataType"};
            for (unsigned i=0;i<COUNT(names);++i) if (!strcmp(selector,names[i])) return (Method)metal_argument_value;
        }
        if (attr && (!strcmp(selector,"attributeIndex") || !strcmp(selector,"attributeType") || !strcmp(selector,"isActive"))) return (Method)metal_attribute_value;
        if (member && (!strcmp(selector,"offset") || !strcmp(selector,"dataType"))) return (Method)metal_member_value;
    }
    if (structure && !strcmp(selector,"members")) return (Method)metal_reflection_children;
    if (structure && !strcmp(selector,"memberByName:")) return (Method)metal_struct_member;
    return NULL;
}
