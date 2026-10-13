// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "VolumetricWindow.h"
#include "XrMath.h"

#include <cstdint>

#include "window.frag.h"
#include "window.vert.h"

#include "common/Console.h"
#include "common/Image.h"

#include <cmath>
#include <span>
#include <cstring>

namespace
{
	// Rate-limited reason for not producing a layer.
	void ReportSkip(const char* reason)
	{
		static int reported = 0;
		if (reported++ < 10)
			Console.WarningFmt("Window: no layer this frame ({})", reason);
	}

	// std140 layout of the Window block in window_common.glsl.
	struct WindowUniforms
	{
		float view_proj[2][16];
		float origin[4];
		float right[4];
		float up[4];
		float back[4];
		int32_t grid[4];
		float colour_rect[4];
		float scene_rect[4];
		float thresholds[4];
		float hud_rects[4][4];
		int32_t hud_info[4];
		int32_t hud_flat[4];
	};

	struct PassConstants
	{
		int32_t eye, mode;
	};
	enum Mode : int32_t
	{
		MODE_BACKGROUND = 0,
		MODE_SCENE = 1,
		MODE_HUD = 2,
	};

	// Colour change (in roughly perceptual units) that marks a HUD pixel, and the normalised depth
	// step between neighbouring cells that marks a depth edge.
	constexpr float HUD_THRESHOLD = 0.1f;
	constexpr float EDGE_THRESHOLD = 0.1f;

	constexpr float NEAR_Z = 0.05f;
	constexpr float FAR_Z = 100.0f;
	constexpr float CLEAR_COLOUR[4] = {0.01f, 0.01f, 0.012f, 1.0f};

	VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
	{
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = src;
		b.dstAccessMask = dst;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		return b;
	}
} // namespace

VolumetricWindow::~VolumetricWindow()
{
	Destroy();
}

uint32_t VolumetricWindow::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(m_gpu, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
	{
		if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}
	return UINT32_MAX;
}

bool VolumetricWindow::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer)
{
	VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = size;
	info.usage = usage;
	if (vkCreateBuffer(m_device, &info, nullptr, &buffer->buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(m_device, buffer->buffer, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex =
		FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &buffer->memory) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(m_device, buffer->buffer, buffer->memory, 0);
	vkMapMemory(m_device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped);
	return true;
}

void VolumetricWindow::DestroyBuffer(Buffer* buffer)
{
	if (buffer->buffer)
		vkDestroyBuffer(m_device, buffer->buffer, nullptr);
	if (buffer->memory)
		vkFreeMemory(m_device, buffer->memory, nullptr);
	*buffer = {};
}

bool VolumetricWindow::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
	VkImageAspectFlags aspect, VkImage* image, VkDeviceMemory* memory, VkImageView* view)
{
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = {width, height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_device, &info, nullptr, image) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_device, *image, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, *image, *memory, 0);
	VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view_info.image = *image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.subresourceRange = {aspect, 0, 1, 0, 1};
	return vkCreateImageView(m_device, &view_info, nullptr, view) == VK_SUCCESS;
}

bool VolumetricWindow::Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index)
{
	m_xr = &xr;
	m_device = device;
	m_gpu = gpu;
	m_queue = queue;

	uint32_t count = 0;
	xrEnumerateSwapchainFormats(xr.Session(), 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(xr.Session(), count, &count, formats.data());
	for (const int64_t preferred : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
	{
		for (const int64_t f : formats)
		{
			if (f == preferred && !m_format)
				m_format = f;
		}
	}
	if (!m_format)
	{
		Console.Error("Window: the runtime offers no sRGB swapchain format.");
		return false;
	}

	XrViewConfigurationView configs[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	count = 0;
	if (XR_FAILED(xrEnumerateViewConfigurationViews(xr.Instance(), xr.System(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			2, &count, configs)) ||
		count != 2)
	{
		Console.Error("Window: no stereo view configuration.");
		return false;
	}

	// Render pass: colour (cleared to the room's dark) and a depth buffer for the mesh.
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = static_cast<VkFormat>(m_format);
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[1] = attachments[0];
	attachments[1].format = DEPTH_FORMAT;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	const VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colour_ref;
	subpass.pDepthStencilAttachment = &depth_ref;
	VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rp_info.attachmentCount = 2;
	rp_info.pAttachments = attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	if (vkCreateRenderPass(device, &rp_info, nullptr, &m_render_pass) != VK_SUCCESS)
		return false;

	for (int i = 0; i < 2; i++)
	{
		if (!CreateEye(m_eyes[i], configs[i]))
			return false;
	}
	Console.WriteLnFmt("Window: {}x{} per eye", m_eyes[0].width, m_eyes[0].height);

	if (!CreatePipeline())
		return false;

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index;
	if (vkCreateCommandPool(device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
		return false;
	VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = m_pool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = NUM_SLOTS;
	if (vkAllocateCommandBuffers(device, &alloc, m_cmd.data()) != VK_SUCCESS)
		return false;
	VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (size_t i = 0; i < NUM_SLOTS; i++)
	{
		if (vkCreateFence(device, &fence_info, nullptr, &m_fences[i]) != VK_SUCCESS ||
			!CreateBuffer(sizeof(WindowUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &m_uniforms[i]))
			return false;
	}
	return true;
}

bool VolumetricWindow::CreateEye(Eye& eye, const XrViewConfigurationView& config)
{
	eye.width = config.recommendedImageRectWidth;
	eye.height = config.recommendedImageRectHeight;
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
	info.format = m_format;
	info.sampleCount = 1;
	info.width = eye.width;
	info.height = eye.height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		if (XR_FAILED(xrCreateSwapchain(m_xr->Session(), &info, &eye.swapchain)))
		{
			Console.ErrorFmt("Window: xrCreateSwapchain {}x{} failed", eye.width, eye.height);
			eye.swapchain = XR_NULL_HANDLE;
			return false;
		}
	}
	uint32_t count = 0;
	xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	xrEnumerateSwapchainImages(eye.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

	if (!CreateImage(eye.width, eye.height, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT, &eye.depth, &eye.depth_memory, &eye.depth_view))
		return false;

	for (const XrSwapchainImageVulkanKHR& image : images)
	{
		VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view_info.image = image.image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = static_cast<VkFormat>(m_format);
		view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view = VK_NULL_HANDLE;
		if (vkCreateImageView(m_device, &view_info, nullptr, &view) != VK_SUCCESS)
			return false;
		const VkImageView attachments[2] = {view, eye.depth_view};
		VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fb_info.renderPass = m_render_pass;
		fb_info.attachmentCount = 2;
		fb_info.pAttachments = attachments;
		fb_info.width = eye.width;
		fb_info.height = eye.height;
		fb_info.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if (vkCreateFramebuffer(m_device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS)
			return false;
		eye.images.push_back(image.image);
		eye.views.push_back(view);
		eye.framebuffers.push_back(framebuffer);
	}
	return true;
}

void VolumetricWindow::DestroyEye(Eye& eye)
{
	for (VkFramebuffer fb : eye.framebuffers)
		vkDestroyFramebuffer(m_device, fb, nullptr);
	for (VkImageView view : eye.views)
		vkDestroyImageView(m_device, view, nullptr);
	if (eye.depth_view)
		vkDestroyImageView(m_device, eye.depth_view, nullptr);
	if (eye.depth)
		vkDestroyImage(m_device, eye.depth, nullptr);
	if (eye.depth_memory)
		vkFreeMemory(m_device, eye.depth_memory, nullptr);
	if (eye.swapchain)
		xrDestroySwapchain(eye.swapchain);
	eye = {};
}

bool VolumetricWindow::CreatePipeline()
{
	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(m_device, &sampler, nullptr, &m_nearest) != VK_SUCCESS)
		return false;
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	if (vkCreateSampler(m_device, &sampler, nullptr, &m_linear) != VK_SUCCESS)
		return false;

	constexpr VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
		{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, both, nullptr},
		{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	set_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(m_device, &set_info, nullptr, &m_set_layout) != VK_SUCCESS)
		return false;
	const VkPushConstantRange push{both, 0, sizeof(PassConstants)};
	VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &m_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(m_device, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS)
		return false;

	const auto module = [this](const uint32_t* code, size_t size) {
		VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		info.codeSize = size;
		info.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(m_device, &info, nullptr, &m);
		return m;
	};
	const VkShaderModule vert = module(window_vert, sizeof(window_vert));
	const VkShaderModule frag = module(window_frag, sizeof(window_frag));
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;
	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;

	VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depth;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = m_pipeline_layout;
	info.renderPass = m_render_pass;
	VkResult res = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline);
	if (res == VK_SUCCESS)
	{
		// The HUD goes over everything at the screen plane.
		depth.depthTestEnable = VK_FALSE;
		depth.depthWriteEnable = VK_FALSE;
		res = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_overlay_pipeline);
	}
	vkDestroyShaderModule(m_device, vert, nullptr);
	vkDestroyShaderModule(m_device, frag, nullptr);
	if (res != VK_SUCCESS)
	{
		Console.Error("Window: could not create the graphics pipeline.");
		return false;
	}
	return true;
}

void VolumetricWindow::Destroy()
{
	if (!m_device)
		return;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
		for (Eye& eye : m_eyes)
			DestroyEye(eye);
	}
	DestroyImage(m_colour);
	DestroyImage(m_scene);
	DestroyImage(m_live_depth);
	DestroyInspectionTargets();
	DestroyBuffer(&m_indices);
	for (size_t i = 0; i < NUM_SLOTS; i++)
	{
		if (m_fences[i])
			vkDestroyFence(m_device, m_fences[i], nullptr);
		m_fences[i] = VK_NULL_HANDLE;
		DestroyBuffer(&m_uniforms[i]);
	}
	if (m_pool)
		vkDestroyCommandPool(m_device, m_pool, nullptr);
	if (m_pipeline)
		vkDestroyPipeline(m_device, m_pipeline, nullptr);
	if (m_overlay_pipeline)
		vkDestroyPipeline(m_device, m_overlay_pipeline, nullptr);
	m_overlay_pipeline = VK_NULL_HANDLE;
	if (m_pipeline_layout)
		vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
	if (m_set_layout)
		vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
	if (m_render_pass)
		vkDestroyRenderPass(m_device, m_render_pass, nullptr);
	if (m_nearest)
		vkDestroySampler(m_device, m_nearest, nullptr);
	if (m_linear)
		vkDestroySampler(m_device, m_linear, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_pipeline = VK_NULL_HANDLE;
	m_pipeline_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_render_pass = VK_NULL_HANDLE;
	m_nearest = m_linear = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

bool VolumetricWindow::EnsureImage(Image& image, VkFormat format, uint32_t width, uint32_t height)
{
	if (image.image && width == image.width && height == image.height)
		return true;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
	}
	DestroyImage(image);
	if (!CreateImage(width, height, format,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, &image.image, &image.memory, &image.view))
	{
		DestroyImage(image);
		return false;
	}
	image.width = width;
	image.height = height;
	return true;
}

void VolumetricWindow::DestroyImage(Image& image)
{
	if (image.view)
		vkDestroyImageView(m_device, image.view, nullptr);
	if (image.image)
		vkDestroyImage(m_device, image.image, nullptr);
	if (image.memory)
		vkFreeMemory(m_device, image.memory, nullptr);
	image = {};
}

// A raw copy of an emulator image (RGBA8, SHADER_READ_ONLY_OPTIMAL, left that way) into ours.
void VolumetricWindow::RecordCopy(VkCommandBuffer cmd, VkImage src, const Image& dst)
{
	VkImageMemoryBarrier before[2] = {
		ImageBarrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
		ImageBarrier(dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
		nullptr, 2, before);
	VkImageCopy region{};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent = {dst.width, dst.height, 1};
	vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		&region);
	VkImageMemoryBarrier after[2] = {
		ImageBarrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT),
		ImageBarrier(dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		nullptr, 2, after);
}

bool VolumetricWindow::EnsureIndices(uint32_t grid_width, uint32_t grid_height)
{
	if (m_indices.buffer && grid_width == m_grid_width && grid_height == m_grid_height)
		return true;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
	}
	DestroyBuffer(&m_indices);
	const uint32_t quads = (grid_width - 1) * (grid_height - 1);
	if (!CreateBuffer(VkDeviceSize(quads) * 6 * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &m_indices))
		return false;
	uint32_t* out = static_cast<uint32_t*>(m_indices.mapped);
	for (uint32_t y = 0; y + 1 < grid_height; y++)
	{
		for (uint32_t x = 0; x + 1 < grid_width; x++)
		{
			const uint32_t i = y * grid_width + x;
			*out++ = i;
			*out++ = i + grid_width;
			*out++ = i + 1;
			*out++ = i + 1;
			*out++ = i + grid_width;
			*out++ = i + grid_width + 1;
		}
	}
	m_index_count = quads * 6;
	m_grid_width = grid_width;
	m_grid_height = grid_height;
	return true;
}

void VolumetricWindow::SetFrame(const VKLibretro::Frame& frame, bool has_depth)
{
	if (!frame.image || !frame.width || !frame.height)
		return;
	m_pending = frame;
	m_has_pending = true;
	m_has_depth = has_depth;
	if (has_depth && frame.prehud_image)
		std::memcpy(m_scene_rect, frame.depth_src_uv, sizeof(m_scene_rect));

	// Without depth there is no presented region from the core: show the whole canvas.
	float rect[4] = {0.0f, 0.0f, static_cast<float>(frame.width), static_cast<float>(frame.height)};
	if (frame.depth_image)
		std::memcpy(rect, frame.depth_draw_rect, sizeof(rect));
	const float w = rect[2] - rect[0], h = rect[3] - rect[1];
	if (w >= 1.0f && h >= 1.0f)
	{
		m_colour_rect[0] = rect[0] / frame.width;
		m_colour_rect[1] = rect[1] / frame.height;
		m_colour_rect[2] = rect[2] / frame.width;
		m_colour_rect[3] = rect[3] / frame.height;
		m_aspect = w / h;
	}
}

void VolumetricWindow::SetScreen(const XrPosef& pose, const Settings& settings)
{
	m_pose = pose;
	m_settings = settings;
}

const XrCompositionLayerBaseHeader* VolumetricWindow::Render(XrTime display_time, VkImageView depth_grid,
	VkImageView background_grid, uint32_t grid_width, uint32_t grid_height)
{
	if (m_has_pending && !EnsureImage(m_colour, VK_FORMAT_R8G8B8A8_SRGB, m_pending.width, m_pending.height))
		m_has_pending = false;
	if (!m_colour.image)
		return nullptr;
	const bool use_depth = m_has_depth && depth_grid && background_grid && grid_width >= 2 && grid_height >= 2;
	// A flat window still needs a grid; any size works.
	const uint32_t gw = use_depth ? grid_width : 2, gh = use_depth ? grid_height : 2;
	if (!EnsureIndices(gw, gh))
		return nullptr;

	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = display_time;
	locate.space = m_xr->LocalSpace();
	XrViewState view_state{XR_TYPE_VIEW_STATE};
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t view_count = 0;
	if (XR_FAILED(xrLocateViews(m_xr->Session(), &locate, &view_state, 2, &view_count, views)) || view_count != 2 ||
		!(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
		!(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
	{
		ReportSkip("views not located");
		return nullptr;
	}

	if (static int logged = 0; logged++ % 450 == 0)
	{
		const auto yaw_of = [](const XrQuaternionf& q) {
			return std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;
		};
		for (int i = 0; i < 2; i++)
		{
			const XrView& v = views[i];
			Console.WriteLnFmt("Window: eye {} pos ({:.3f} {:.3f} {:.3f}) q ({:.3f} {:.3f} {:.3f} {:.3f}) yaw {:.1f} "
							   "fov L{:.1f} R{:.1f} U{:.1f} D{:.1f}",
				i, v.pose.position.x, v.pose.position.y, v.pose.position.z, v.pose.orientation.x, v.pose.orientation.y,
				v.pose.orientation.z, v.pose.orientation.w, yaw_of(v.pose.orientation), v.fov.angleLeft * 57.29578f,
				v.fov.angleRight * 57.29578f, v.fov.angleUp * 57.29578f, v.fov.angleDown * 57.29578f);
		}
		Console.WriteLnFmt("Window: screen pos ({:.3f} {:.3f} {:.3f}) yaw {:.1f}, aspect {:.3f}, depth {}", m_pose.position.x,
			m_pose.position.y, m_pose.position.z, yaw_of(m_pose.orientation), m_aspect, use_depth);
	}

	const size_t slot = m_slot;
	m_slot = (m_slot + 1) % NUM_SLOTS;
	vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
	vkResetFences(m_device, 1, &m_fences[slot]);

	// Uniforms for this frame.
	WindowUniforms& u = *static_cast<WindowUniforms*>(m_uniforms[slot].mapped);
	for (int i = 0; i < 2; i++)
	{
		const Mat4 vp = Multiply(Projection(views[i].fov, NEAR_Z, FAR_Z), ViewFromPose(views[i].pose));
		std::memcpy(u.view_proj[i], vp.m, sizeof(vp.m));
	}
	const float half_w = 0.5f * m_settings.width;
	const float half_h = half_w / m_aspect;
	const XrVector3f right = Rotate(m_pose.orientation, {half_w, 0.0f, 0.0f});
	const XrVector3f up = Rotate(m_pose.orientation, {0.0f, half_h, 0.0f});
	const XrVector3f back = Rotate(m_pose.orientation, {0.0f, 0.0f, -m_settings.depth_range});
	const float origin[4] = {m_pose.position.x, m_pose.position.y, m_pose.position.z, 1.0f};
	std::memcpy(u.origin, origin, sizeof(origin));
	const float r4[4] = {right.x, right.y, right.z, 0.0f}, u4[4] = {up.x, up.y, up.z, 0.0f},
				b4[4] = {back.x, back.y, back.z, m_settings.pop_out};
	std::memcpy(u.right, r4, sizeof(r4));
	std::memcpy(u.up, u4, sizeof(u4));
	std::memcpy(u.back, b4, sizeof(b4));
	u.grid[0] = static_cast<int32_t>(gw);
	u.grid[1] = static_cast<int32_t>(gh);
	u.grid[2] = use_depth ? 1 : 0;
	std::memcpy(u.colour_rect, m_colour_rect, sizeof(m_colour_rect));
	std::memcpy(u.scene_rect, m_scene_rect, sizeof(m_scene_rect));
	u.thresholds[0] = HUD_THRESHOLD;
	u.thresholds[1] = m_settings.edge_fill ? EDGE_THRESHOLD : 1e9f;

	// Acquire both eyes' images before recording: the framebuffers depend on the indices.
	uint32_t image_index[2] = {};
	for (int i = 0; i < 2; i++)
	{
		XrResult res;
		{
			std::lock_guard lock(GraphicsQueueMutex());
			res = xrAcquireSwapchainImage(m_eyes[i].swapchain, nullptr, &image_index[i]);
		}
		XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait.timeout = XR_INFINITE_DURATION;
		if (XR_SUCCEEDED(res))
			res = xrWaitSwapchainImage(m_eyes[i].swapchain, &wait);
		if (XR_FAILED(res))
		{
			Console.ErrorFmt("Window: swapchain image for eye {} failed ({})", i, static_cast<int>(res));
			// An image acquired for the other eye must still be released.
			std::lock_guard lock(GraphicsQueueMutex());
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			for (int j = 0; j < i; j++)
				xrReleaseSwapchainImage(m_eyes[j].swapchain, &release);
			return nullptr;
		}
	}

	VkCommandBuffer cmd = m_cmd[slot];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	// The latest emulator frame, and the colour from before its HUD when the frame has one.
	if (m_has_pending)
	{
		RecordCopy(cmd, m_pending.image, m_colour);
		// A frame without the HUD data (the pre-HUD colour and the live depth) keeps the last ones
		// for up to half a second, so the HUD does not drop onto the scene for single frames.
		const bool has_hud_data = m_has_depth && m_pending.prehud_image && m_pending.live_depth_image &&
		                          m_pending.depth_width && m_pending.depth_height &&
		                          EnsureImage(m_scene, VK_FORMAT_R8G8B8A8_SRGB, m_pending.depth_width, m_pending.depth_height) &&
		                          EnsureImage(m_live_depth, VK_FORMAT_R32_SFLOAT, m_pending.depth_width, m_pending.depth_height);
		if (has_hud_data)
		{
			RecordCopy(cmd, m_pending.prehud_image, m_scene);
			RecordCopy(cmd, m_pending.live_depth_image, m_live_depth);
			m_depth_scale = m_pending.depth_scale;
			m_frames_without_hud_data = 0;
			m_has_scene = m_has_live_depth = true;
		}
		else if (!m_has_depth || ++m_frames_without_hud_data > 30)
		{
			m_has_scene = m_has_live_depth = false;
		}
		m_has_pending = false;
	}
	const bool use_scene = use_depth && m_has_scene && m_settings.hud_overlay;
	const bool use_live_depth = use_scene && m_has_live_depth;
	u.grid[3] = (use_scene ? 1 : 0) | (use_live_depth ? 2 : 0);
	u.thresholds[2] = m_depth_scale;
	const size_t rects = std::min<size_t>(m_settings.hud_rects.size(), 4);
	for (size_t r = 0; r < rects; r++)
		std::memcpy(u.hud_rects[r], m_settings.hud_rects[r].data(), sizeof(u.hud_rects[r]));
	u.hud_info[0] = static_cast<int32_t>(rects);
	bool any_flat = false;
	for (size_t r = 0; r < 4; r++)
	{
		const bool flat = r < rects && r < m_settings.hud_rect_flat.size() && m_settings.hud_rect_flat[r];
		u.hud_flat[r] = flat ? 1 : 0;
		any_flat |= flat;
	}

	// DepthView wrote the grid earlier on this queue.
	VkMemoryBarrier grid{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	grid.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	grid.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &grid, 0,
		nullptr, 0, nullptr);

	const VkDescriptorBufferInfo uniform_info{m_uniforms[slot].buffer, 0, sizeof(WindowUniforms)};
	// Bindings the passes do not use still need a valid image: the colour image stands in (the
	// shaders never read a grid without depth, nor the pre-HUD colour without one).
	const auto grid_image = [&](VkImageView view) {
		return use_depth ? VkDescriptorImageInfo{m_nearest, view, VK_IMAGE_LAYOUT_GENERAL} :
		                   VkDescriptorImageInfo{m_nearest, m_colour.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	};
	const VkDescriptorImageInfo grid_info = grid_image(depth_grid);
	const VkDescriptorImageInfo background_info = grid_image(background_grid);
	const VkDescriptorImageInfo colour_info{m_linear, m_colour.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorImageInfo scene_info{
		m_linear, use_scene ? m_scene.view : m_colour.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorImageInfo live_info{
		m_nearest, use_live_depth ? m_live_depth.view : m_colour.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[6] = {};
	for (int i = 0; i < 6; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstBinding = static_cast<uint32_t>(i);
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &uniform_info;
	writes[1].pImageInfo = &grid_info;
	writes[2].pImageInfo = &colour_info;
	writes[3].pImageInfo = &background_info;
	writes[4].pImageInfo = &scene_info;
	writes[5].pImageInfo = &live_info;

	for (int i = 0; i < 2; i++)
	{
		Eye& eye = m_eyes[i];
		VkClearValue clears[2];
		std::memcpy(clears[0].color.float32, CLEAR_COLOUR, sizeof(CLEAR_COLOUR));
		clears[1].depthStencil = {1.0f, 0};
		VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rp.renderPass = m_render_pass;
		rp.framebuffer = eye.framebuffers[image_index[i]];
		rp.renderArea = {{0, 0}, {eye.width, eye.height}};
		rp.clearValueCount = 2;
		rp.pClearValues = clears;
		vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
		const VkViewport viewport{0.0f, 0.0f, static_cast<float>(eye.width), static_cast<float>(eye.height), 0.0f, 1.0f};
		const VkRect2D scissor{{0, 0}, {eye.width, eye.height}};
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
		const auto draw = [&](VkPipeline pipeline, int32_t mode) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 6, writes);
			const PassConstants pass{i, mode};
			vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				sizeof(pass), &pass);
			vkCmdDrawIndexed(cmd, m_index_count, 1, 0, 0, 0);
		};
		if (use_depth && m_settings.edge_fill)
			draw(m_pipeline, MODE_BACKGROUND);
		draw(m_pipeline, MODE_SCENE);
		if (use_scene || (use_depth && any_flat))
			draw(m_overlay_pipeline, MODE_HUD);
		vkCmdEndRenderPass(cmd);
	}

	// Inspection mode: the same passes from two fixed cameras in front of the screen.
	if (m_capture_eyes && EnsureInspectionTargets())
	{
		WindowUniforms& iu = *static_cast<WindowUniforms*>(m_inspection_uniforms[slot].mapped);
		iu = u;
		const XrVector3f normal = Rotate(m_pose.orientation, {0.0f, 0.0f, 1.0f});
		const XrVector3f side = Rotate(m_pose.orientation, {1.0f, 0.0f, 0.0f});
		const float dist = m_settings.viewer_distance;
		// Fit the screen (plus a margin) in the view, at the inspection images' aspect.
		const float half_h = std::atan(1.15f * std::max(half_w / m_aspect,
			half_w * static_cast<float>(INSPECTION_HEIGHT) / INSPECTION_WIDTH) / dist);
		const float half_wa = std::atan(std::tan(half_h) * INSPECTION_WIDTH / INSPECTION_HEIGHT);
		const XrFovf fov{-half_wa, half_wa, half_h, -half_h};
		for (int i = 0; i < 2; i++)
		{
			const float offset = (i == 0 ? -0.032f : 0.032f);
			XrPosef cam;
			cam.orientation = m_pose.orientation;
			cam.position = {m_pose.position.x + normal.x * dist + side.x * offset,
				m_pose.position.y + normal.y * dist + side.y * offset,
				m_pose.position.z + normal.z * dist + side.z * offset};
			const Mat4 vp = Multiply(Projection(fov, NEAR_Z, FAR_Z), ViewFromPose(cam));
			std::memcpy(iu.view_proj[i], vp.m, sizeof(vp.m));
		}
		VkDescriptorBufferInfo inspection_uniform{m_inspection_uniforms[slot].buffer, 0, sizeof(WindowUniforms)};
		VkWriteDescriptorSet iwrites[6];
		std::memcpy(iwrites, writes, sizeof(iwrites));
		iwrites[0].pBufferInfo = &inspection_uniform;
		for (int i = 0; i < 2; i++)
		{
			InspectionTarget& t = m_inspection[i];
			VkClearValue clears[2];
			std::memcpy(clears[0].color.float32, CLEAR_COLOUR, sizeof(CLEAR_COLOUR));
			clears[1].depthStencil = {1.0f, 0};
			VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			rp.renderPass = m_render_pass;
			rp.framebuffer = t.framebuffer;
			rp.renderArea = {{0, 0}, {INSPECTION_WIDTH, INSPECTION_HEIGHT}};
			rp.clearValueCount = 2;
			rp.pClearValues = clears;
			vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
			const VkViewport viewport{0.0f, 0.0f, static_cast<float>(INSPECTION_WIDTH),
				static_cast<float>(INSPECTION_HEIGHT), 0.0f, 1.0f};
			const VkRect2D scissor{{0, 0}, {INSPECTION_WIDTH, INSPECTION_HEIGHT}};
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdSetScissor(cmd, 0, 1, &scissor);
			vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
			const auto draw = [&](VkPipeline pipeline, int32_t mode) {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 6, iwrites);
				const PassConstants pass{i, mode};
				vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
					sizeof(pass), &pass);
				vkCmdDrawIndexed(cmd, m_index_count, 1, 0, 0, 0);
			};
			if (use_depth && m_settings.edge_fill)
				draw(m_pipeline, MODE_BACKGROUND);
			draw(m_pipeline, MODE_SCENE);
			if (use_scene || (use_depth && any_flat))
				draw(m_overlay_pipeline, MODE_HUD);
			vkCmdEndRenderPass(cmd);
			VkImageMemoryBarrier to_read = ImageBarrier(t.colour.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
				0, nullptr, 0, nullptr, 1, &to_read);
		}
	}

	// Eye capture: both images read back as rendered.
	const bool dump = !m_eye_dump_prefix.empty();
	Buffer readback;
	const uint32_t eye_w = m_eyes[0].width, eye_h = m_eyes[0].height;
	if (dump && CreateBuffer(VkDeviceSize(eye_w) * eye_h * 4 * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback))
	{
		for (int i = 0; i < 2; i++)
		{
			const VkImage image = m_eyes[i].images[image_index[i]];
			VkImageMemoryBarrier to_src = ImageBarrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
				nullptr, 0, nullptr, 1, &to_src);
			// Side by side: each row of the readback holds the left then the right eye.
			VkBufferImageCopy copy{};
			copy.bufferOffset = VkDeviceSize(i) * eye_w * 4;
			copy.bufferRowLength = eye_w * 2;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageExtent = {std::min(eye_w, m_eyes[i].width), std::min(eye_h, m_eyes[i].height), 1};
			vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
			VkImageMemoryBarrier back = ImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
				0, nullptr, 1, &back);
		}
	}
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueSubmit(m_queue, 1, &submit, m_fences[slot]);
		XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		for (Eye& eye : m_eyes)
			xrReleaseSwapchainImage(eye.swapchain, &release);
	}
	if (dump)
	{
		if (readback.buffer)
		{
			vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
			RGBA8Image image(eye_w * 2, eye_h, static_cast<const u32*>(readback.mapped));
			if (m_format == VK_FORMAT_B8G8R8A8_SRGB)
			{
				for (u32& p : std::span(image.GetPixels(), size_t(eye_w) * 2 * eye_h))
					p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
			}
			const std::string path = m_eye_dump_prefix + ".png";
			Console.WriteLnFmt("Window: eye dump {} ({})", path, image.SaveToFile(path.c_str()) ? "written" : "failed");
			DestroyBuffer(&readback);
		}
		m_eye_dump_prefix.clear();
	}

	for (int i = 0; i < 2; i++)
	{
		XrCompositionLayerProjectionView& v = m_layer_views[i];
		v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		v.pose = views[i].pose;
		v.fov = views[i].fov;
		v.subImage.swapchain = m_eyes[i].swapchain;
		v.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(m_eyes[i].width), static_cast<int32_t>(m_eyes[i].height)}};
	}
	m_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	m_layer.space = m_xr->LocalSpace();
	m_layer.viewCount = 2;
	m_layer.views = m_layer_views.data();
	return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_layer);
}

bool VolumetricWindow::EnsureInspectionTargets()
{
	if (m_inspection[0].framebuffer)
		return true;
	for (InspectionTarget& t : m_inspection)
	{
		if (!CreateImage(INSPECTION_WIDTH, INSPECTION_HEIGHT, static_cast<VkFormat>(m_format),
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				VK_IMAGE_ASPECT_COLOR_BIT, &t.colour.image, &t.colour.memory, &t.colour.view) ||
			!CreateImage(INSPECTION_WIDTH, INSPECTION_HEIGHT, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT, &t.depth, &t.depth_memory, &t.depth_view))
			return false;
		t.colour.width = INSPECTION_WIDTH;
		t.colour.height = INSPECTION_HEIGHT;
		const VkImageView attachments[2] = {t.colour.view, t.depth_view};
		VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fb_info.renderPass = m_render_pass;
		fb_info.attachmentCount = 2;
		fb_info.pAttachments = attachments;
		fb_info.width = INSPECTION_WIDTH;
		fb_info.height = INSPECTION_HEIGHT;
		fb_info.layers = 1;
		if (vkCreateFramebuffer(m_device, &fb_info, nullptr, &t.framebuffer) != VK_SUCCESS)
			return false;
	}
	for (Buffer& b : m_inspection_uniforms)
	{
		if (!CreateBuffer(sizeof(WindowUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &b))
			return false;
	}
	return true;
}

void VolumetricWindow::DestroyInspectionTargets()
{
	for (InspectionTarget& t : m_inspection)
	{
		if (t.framebuffer)
			vkDestroyFramebuffer(m_device, t.framebuffer, nullptr);
		if (t.depth_view)
			vkDestroyImageView(m_device, t.depth_view, nullptr);
		if (t.depth)
			vkDestroyImage(m_device, t.depth, nullptr);
		if (t.depth_memory)
			vkFreeMemory(m_device, t.depth_memory, nullptr);
		DestroyImage(t.colour);
		t = {};
	}
	for (Buffer& b : m_inspection_uniforms)
		DestroyBuffer(&b);
}

bool VolumetricWindow::GetEyeImage(int eye, VKLibretro::Frame* frame) const
{
	const Image& colour = m_inspection[eye].colour;
	if (!colour.image)
		return false;
	*frame = {};
	frame->image = colour.image;
	frame->view = colour.view;
	frame->format = static_cast<VkFormat>(m_format);
	frame->width = colour.width;
	frame->height = colour.height;
	return true;
}
