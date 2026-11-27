/* Dynamic Metal state is recorded only after a setter changes it. */
struct MetalViewport { double x,y,width,height,near_depth,far_depth; };
struct MetalScissor { Word x,y,width,height; };
_Static_assert(sizeof(struct MetalViewport)==48,"Metal viewport value ABI");
_Static_assert(sizeof(struct MetalScissor)==32,"Metal scissor value ABI");
static void metal_render_viewport(struct MetalRenderEncoder *encoder,const char *selector,struct MetalViewport value) {
    metal_render_check(encoder,selector);
    const VkPhysicalDeviceLimits *limits=&vulkan.properties.limits;
    if (!isfinite(value.x) || !isfinite(value.y) || !isfinite(value.width) || !isfinite(value.height) ||
        !isfinite(value.near_depth) || !isfinite(value.far_depth) || value.width<0 || value.height<0 ||
        value.width>limits->maxViewportDimensions[0] || value.height>limits->maxViewportDimensions[1] ||
        value.x<limits->viewportBoundsRange[0] || value.y<limits->viewportBoundsRange[0] ||
        value.x+value.width>limits->viewportBoundsRange[1] || value.y+value.height>limits->viewportBoundsRange[1] ||
        value.near_depth<0 || value.near_depth>1 || value.far_depth<0 || value.far_depth>1)
        metal_render_stop("Invalid Metal viewport",selector);
    unsigned empty=value.width==0 || value.height==0;
    VkViewport viewport={(float)value.x,(float)value.y,(float)value.width,(float)value.height,(float)value.near_depth,(float)value.far_depth};
    if (empty) viewport=(VkViewport){0,0,1,1,(float)value.near_depth,(float)value.far_depth};
    if (memcmp(&encoder->state->viewport,&viewport,sizeof(viewport))) {
        encoder->state->viewport=viewport;encoder->state->dynamic_dirty|=METAL_DYNAMIC_VIEWPORT;
    }
    if (encoder->state->viewport_empty!=empty) {
        encoder->state->viewport_empty=empty;encoder->state->dynamic_dirty|=METAL_DYNAMIC_SCISSOR;
    }
}
static void metal_render_scissor(struct MetalRenderEncoder *encoder,const char *selector,struct MetalScissor value) {
    metal_render_check(encoder,selector);
    if (value.x>INT32_MAX || value.y>INT32_MAX || value.width>INT32_MAX-value.x || value.height>INT32_MAX-value.y)
        metal_render_stop("Metal scissor exceeds Vulkan coordinate range",selector);
    VkRect2D scissor={{(int32_t)value.x,(int32_t)value.y},{(uint32_t)value.width,(uint32_t)value.height}};
    if (memcmp(&encoder->state->scissor,&scissor,sizeof(scissor))) {
        encoder->state->scissor=scissor;encoder->state->dynamic_dirty|=METAL_DYNAMIC_SCISSOR;
    }
}
static void metal_render_blend(struct MetalRenderEncoder *encoder,const char *selector,float red,float green,float blue,float alpha) {
    metal_render_check(encoder,selector);
    const float values[]={red,green,blue,alpha};
    for (unsigned i=0;i<COUNT(values);++i) if (!isfinite(values[i])) metal_render_stop("Invalid Metal blend constant",selector);
    if (memcmp(encoder->state->blend,values,sizeof(values))) {
        memcpy(encoder->state->blend,values,sizeof(values));encoder->state->dynamic_dirty|=METAL_DYNAMIC_BLEND;
    }
}
static void metal_render_bias(struct MetalRenderEncoder *encoder,const char *selector,float bias,float slope,float clamp) {
    metal_render_check(encoder,selector);
    if (!isfinite(bias) || !isfinite(slope) || !isfinite(clamp) || (clamp!=0 && !vulkan.features.depthBiasClamp))
        metal_render_stop("Unsupported Metal depth-bias value",selector);
    if (encoder->state->bias!=bias || encoder->state->slope!=slope || encoder->state->clamp!=clamp) {
        encoder->state->bias=bias;encoder->state->slope=slope;encoder->state->clamp=clamp;
        encoder->state->dynamic_dirty|=METAL_DYNAMIC_BIAS;
    }
}
static void metal_render_stencil_value(struct MetalRenderEncoder *encoder,const char *selector,uint32_t value) {
    metal_render_stencil(encoder,selector,value,value);
}
static void metal_render_buffer_offset(struct MetalRenderEncoder *encoder,const char *selector,size_t offset,Word index) {
    metal_render_check(encoder,selector);
    unsigned stage_index=strstr(selector,"Fragment")!=NULL;
    if (index>=31 || !encoder->state->buffers[stage_index][index].buffer || offset>encoder->state->buffers[stage_index][index].buffer->length)
        metal_render_stop("Invalid Metal buffer-offset binding",selector);
    encoder->state->buffers[stage_index][index].offset=offset;
}
static Method metal_render_dynamic_method(const char *selector) {
    if (!strcmp(selector,"setViewport:")) return (Method)metal_render_viewport;
    if (!strcmp(selector,"setScissorRect:")) return (Method)metal_render_scissor;
    if (!strcmp(selector,"setBlendColorRed:green:blue:alpha:")) return (Method)metal_render_blend;
    if (!strcmp(selector,"setDepthBias:slopeScale:clamp:")) return (Method)metal_render_bias;
    if (!strcmp(selector,"setStencilReferenceValue:")) return (Method)metal_render_stencil_value;
    if (!strcmp(selector,"setVertexBufferOffset:atIndex:") || !strcmp(selector,"setFragmentBufferOffset:atIndex:")) return (Method)metal_render_buffer_offset;
    return NULL;
}
