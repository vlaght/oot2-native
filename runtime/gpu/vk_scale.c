// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The oot2-native contributors
//
// The picture scaler (oot2/vk_present.h, vk_present_int.h): where the window is larger than the game's picture, the picture
// is enlarged by a fragment shader (shaders/scale.frag: bicubic and a contrast-adaptive sharpening) instead of the plain
// bilinear blit. One pipeline for the swapchain's format, one sampler, the picture bound by a push descriptor. It draws with
// dynamic rendering into the swapchain image (in COLOR_ATTACHMENT_OPTIMAL) inside the rectangle the picture occupies.
#include <string.h>

#include "oot2/gpu.h"
#include "oot2/gpu_vk.h"
#include "oot2/runtime.h"
#include "vk_present_int.h"

typedef struct {
    float rect[4];
    float src_size[2];
    float sharp;
    float pad;
} Push;

static VkDescriptorSetLayout g_set_layout;
static VkPipelineLayout g_layout;
static VkPipeline g_pipeline;
static VkSampler g_sampler;
static int g_ready;

static VkShaderModule shader_module(const char *name) {
    const GpuShader *s = gpu_shader(name);
    if (!s) rt_fatal("gfx: vulkan: the scaler's shader %s is not in the program", name);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = s->size, .pCode = s->code};
    VkShaderModule m;
    GPU_VK_OK(vkCreateShaderModule(gpu_vk.device, &ci, NULL, &m), "vkCreateShaderModule (the scaler)");
    return m;
}

static void make_pipeline(VkFormat format) {
    VkShaderModule vs = shader_module("scale_vert"), fs = shader_module("scale_frag");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = g_layout};
    GPU_VK_OK(vkCreateGraphicsPipelines(gpu_vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &g_pipeline),
              "vkCreateGraphicsPipelines (the scaler)");
    vkDestroyShaderModule(gpu_vk.device, vs, NULL);
    vkDestroyShaderModule(gpu_vk.device, fs, NULL);
}

int vks_init(VkFormat color_format) {
    if (g_ready) return 1;
    if (!gpu_shader("scale_vert") || !gpu_shader("scale_frag") || !vkCmdPushDescriptorSetKHR) return 0;
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = 1, .pBindings = &b};
    GPU_VK_OK(vkCreateDescriptorSetLayout(gpu_vk.device, &li, NULL, &g_set_layout), "vkCreateDescriptorSetLayout (the scaler)");
    VkPushConstantRange pr = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .size = sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &g_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    GPU_VK_OK(vkCreatePipelineLayout(gpu_vk.device, &pli, NULL, &g_layout), "vkCreatePipelineLayout (the scaler)");
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f};
    GPU_VK_OK(vkCreateSampler(gpu_vk.device, &si, NULL, &g_sampler), "vkCreateSampler (the scaler)");
    make_pipeline(color_format);
    g_ready = 1;
    return 1;
}

void vks_close(void) {
    if (!g_ready) return;
    vkDestroyPipeline(gpu_vk.device, g_pipeline, NULL);
    vkDestroySampler(gpu_vk.device, g_sampler, NULL);
    vkDestroyPipelineLayout(gpu_vk.device, g_layout, NULL);
    vkDestroyDescriptorSetLayout(gpu_vk.device, g_set_layout, NULL);
    g_ready = 0;
}

void vks_record(VkCommandBuffer cmd, VkImageView src, uint32_t src_w, uint32_t src_h, VkImageView dst, VkExtent2D extent,
                int x, int y, int w, int h, float sharp) {
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &att};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
    VkViewport vp = {(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D sc = {{x, y}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDescriptorImageInfo di = {.sampler = g_sampler, .imageView = src, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di};
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &wr);
    Push pc = {{(float)x, (float)y, (float)w, (float)h}, {(float)src_w, (float)src_h}, sharp, 0.0f};
    vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}
