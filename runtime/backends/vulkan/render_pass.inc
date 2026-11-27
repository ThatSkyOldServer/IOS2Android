/* A Metal encoder can split for feedback copies or resource transitions. Record
 * discardable segments in secondary buffers so only the last segment discards.
 * Store-only passes stay inline; command pools recycle segments after the fence. */
static void metal_render_storage_barrier(struct MetalRenderEncoder *);
static void metal_render_segment_begin(struct MetalRenderEncoder *encoder) {
    struct MetalCommandPool *pool=encoder->command->pool_owner;
    if (pool->render_segment_used==pool->render_segment_count) {
        unsigned count=pool->render_segment_count;
        if (count>UINT_MAX-8) metal_render_stop("Too many render segments","draw");
        VkCommandBuffer *segments=realloc(pool->render_segments,(count+8)*sizeof(*segments));
        if (!segments) metal_render_stop("Allocation failed","Render segments");
        pool->render_segments=segments;
        VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool->native,.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY,.commandBufferCount=8};
        metal_render_result(VKD(vkAllocateCommandBuffers)(vulkan.device,&allocate,segments+count),"vkAllocateCommandBuffers render segments");
        pool->render_segment_count=count+8;metal_memory_add(3,8*sizeof(*segments));
    }
    struct MetalRenderState *state=encoder->state;
    state->render_command=pool->render_segments[pool->render_segment_used++];
    VkCommandBufferInheritanceInfo inheritance={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .renderPass=state->first_pass,.framebuffer=state->framebuffer,.subpass=0};
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,.pInheritanceInfo=&inheritance};
    metal_render_result(VKD(vkBeginCommandBuffer)(state->render_command,&begin),"vkBeginCommandBuffer render segment");
    /* Secondary command buffers do not inherit binding or dynamic state. */
    state->bound_pipeline=VK_NULL_HANDLE;state->bound_index=VK_NULL_HANDLE;
    state->bound_descriptor_layout=VK_NULL_HANDLE;
    memset(state->bound_vertices,0,sizeof(state->bound_vertices));state->dynamic_set=0;
}
static void metal_render_finish_pass(struct MetalRenderEncoder *encoder,unsigned final) {
    struct MetalRenderState *state=encoder->state;
    if (!state->begun) return;
    if (state->deferred_store) {
        metal_render_result(VKD(vkEndCommandBuffer)(state->render_command),"vkEndCommandBuffer render segment");
        struct MetalPassBundle *bundle=state->pass_bundle;
        VkRenderPass pass=final ? (state->segment_load ? bundle->final_load_pass : bundle->final_first_pass) :
            (state->segment_load ? bundle->load_pass : bundle->first_pass);
        VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=state->framebuffer,
            .renderArea={{0,0},{state->width,state->height}},.clearValueCount=state->attachment_count,.pClearValues=state->clear};
        VKD(vkCmdBeginRenderPass)(encoder->command->command,&begin,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        VKD(vkCmdExecuteCommands)(encoder->command->command,1,&state->render_command);
    }
    VKD(vkCmdEndRenderPass)(encoder->command->command);
    metal_diagnostic_pass_end(encoder,final && state->deferred_store);state->begun=0;
}
static void metal_render_close_pass(struct MetalRenderEncoder *encoder) { metal_render_finish_pass(encoder,0); }
