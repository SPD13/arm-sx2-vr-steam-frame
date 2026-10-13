// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunRenderer.h"

#include "XrMath.h"

#include <cstdint>

#include "gun.frag.h"
#include "gun.vert.h"
#include "gun_beam.frag.h"
#include "gun_beam.vert.h"

#include "common/Console.h"

#include <cmath>
#include <cstring>
#include <iterator>

namespace
{
	constexpr float RESOLUTION_SCALE = 0.7f; // of the eye's recommended size
	constexpr float NEAR_Z = 0.02f;
	constexpr float FAR_Z = 50.0f;

	// Fixed colours of the moving parts (sRGB) and their roughness.
	constexpr uint32_t TRIGGER_COLOR = 0x141414;
	constexpr float TRIGGER_ROUGHNESS = 0.4f;
	constexpr uint32_t BUTTON_COLOR = 0xC81E1E;
	constexpr float BUTTON_ROUGHNESS = 0.45f;

	// From the model's contract (tools/lightgun/README.md).
	constexpr float TRIGGER_TRAVEL = -12.0f * 3.14159265f / 180.0f; // negative: back toward the grip
	constexpr float BUTTON_TRAVEL = 0.001f; // meters into the body

	constexpr float BEAM_WIDTH = 0.004f;
	constexpr float DOT_SIZE = 0.01f; // radius at the eye, growing with distance
	constexpr float DOT_GROWTH = 0.006f; // per meter

	// The push constants of gun_common.glsl and gun_beam.*.
	struct MeshConstants
	{
		float mvp[16];
		float normal_to_view[3][4];
		float colour[4];
	};
	static_assert(sizeof(MeshConstants) == 128);
	struct QuadConstants
	{
		float corners[4][4];
		float colour[4];
		float params[4];
	};

	float SrgbToLinear(float c)
	{
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	}

	void LinearColor(uint32_t rgb, float* out)
	{
		for (int i = 0; i < 3; i++)
			out[i] = SrgbToLinear(static_cast<float>((rgb >> (16 - 8 * i)) & 0xFFu) / 255.0f);
	}

	void ClipPoint(const Mat4& vp, const XrVector3f& p, float* out)
	{
		for (int row = 0; row < 4; row++)
			out[row] = vp.m[row] * p.x + vp.m[4 + row] * p.y + vp.m[8 + row] * p.z + vp.m[12 + row];
	}
} // namespace

GunRenderer::~GunRenderer()
{
	Destroy();
}

uint32_t GunRenderer::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
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

bool GunRenderer::Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index,
	const std::string& model_path)
{
	m_xr = &xr;
	m_device = device;
	m_gpu = gpu;
	m_queue = queue;
	if (!m_model.Load(model_path))
		return false;

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
		Console.Error("Light gun: the runtime offers no sRGB swapchain format.");
		return false;
	}
	XrViewConfigurationView configs[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	count = 0;
	if (XR_FAILED(xrEnumerateViewConfigurationViews(xr.Instance(), xr.System(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			2, &count, configs)) ||
		count != 2)
	{
		Console.Error("Light gun: no stereo view configuration.");
		return false;
	}

	// Colour cleared to transparent (the layer is blended over the others) and a depth buffer.
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
	if (!CreatePipelines() || !CreateMeshBuffer())
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
	for (VkFence& fence : m_fences)
	{
		if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
			return false;
	}
	Console.WriteLnFmt("Light gun: layer {}x{} per eye", m_eyes[0].width, m_eyes[0].height);
	m_created = true;
	return true;
}

bool GunRenderer::CreateEye(Eye& eye, const XrViewConfigurationView& config)
{
	eye.width = std::max(64u, static_cast<uint32_t>(config.recommendedImageRectWidth * RESOLUTION_SCALE));
	eye.height = std::max(64u, static_cast<uint32_t>(config.recommendedImageRectHeight * RESOLUTION_SCALE));
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
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
			Console.ErrorFmt("Light gun: xrCreateSwapchain {}x{} failed", eye.width, eye.height);
			eye.swapchain = XR_NULL_HANDLE;
			return false;
		}
	}
	uint32_t count = 0;
	xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	xrEnumerateSwapchainImages(eye.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

	// The depth buffer.
	VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = DEPTH_FORMAT;
	image_info.extent = {eye.width, eye.height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_device, &image_info, nullptr, &eye.depth) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_device, eye.depth, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &eye.depth_memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, eye.depth, eye.depth_memory, 0);
	VkImageViewCreateInfo depth_view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	depth_view.image = eye.depth;
	depth_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	depth_view.format = DEPTH_FORMAT;
	depth_view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(m_device, &depth_view, nullptr, &eye.depth_view) != VK_SUCCESS)
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
		eye.views.push_back(view);
		const VkImageView fb_attachments[2] = {view, eye.depth_view};
		VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fb_info.renderPass = m_render_pass;
		fb_info.attachmentCount = 2;
		fb_info.pAttachments = fb_attachments;
		fb_info.width = eye.width;
		fb_info.height = eye.height;
		fb_info.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if (vkCreateFramebuffer(m_device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS)
			return false;
		eye.framebuffers.push_back(framebuffer);
	}
	return true;
}

void GunRenderer::DestroyEye(Eye& eye)
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

bool GunRenderer::CreatePipelines()
{
	constexpr VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	const auto layout = [this](uint32_t size, VkPipelineLayout* out) {
		const VkPushConstantRange push{both, 0, size};
		VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges = &push;
		return vkCreatePipelineLayout(m_device, &info, nullptr, out) == VK_SUCCESS;
	};
	if (!layout(sizeof(MeshConstants), &m_mesh_layout) || !layout(sizeof(QuadConstants), &m_beam_layout))
		return false;

	const auto module = [this](const uint32_t* code, size_t size) {
		VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		info.codeSize = size;
		info.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(m_device, &info, nullptr, &m);
		return m;
	};
	const VkShaderModule mesh_vert = module(gun_vert, sizeof(gun_vert));
	const VkShaderModule mesh_frag = module(gun_frag, sizeof(gun_frag));
	const VkShaderModule beam_vert = module(gun_beam_vert, sizeof(gun_beam_vert));
	const VkShaderModule beam_frag = module(gun_beam_frag, sizeof(gun_beam_frag));
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].pName = "main";

	const VkVertexInputBindingDescription binding{0, sizeof(GunModel::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription attributes[2] = {
		{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GunModel::Vertex, position)},
		{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GunModel::Vertex, normal)},
	};
	VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = 2;
	vertex_input.pVertexAttributeDescriptions = attributes;
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
	// Premultiplied alpha over what is already drawn; the beam adds light (alpha 0).
	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
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
	info.renderPass = m_render_pass;

	stages[0].module = mesh_vert;
	stages[1].module = mesh_frag;
	info.layout = m_mesh_layout;
	VkResult res = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_mesh_pipeline);

	// The beam and dot: four vertices from the push constants.
	VkPipelineVertexInputStateCreateInfo no_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	info.pVertexInputState = &no_input;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	stages[0].module = beam_vert;
	stages[1].module = beam_frag;
	info.layout = m_beam_layout;
	depth.depthWriteEnable = VK_FALSE; // the beam goes behind the gun, not through it
	if (res == VK_SUCCESS)
		res = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_beam_pipeline);
	depth.depthTestEnable = VK_FALSE; // the dot shows wherever it is
	if (res == VK_SUCCESS)
		res = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_dot_pipeline);

	for (VkShaderModule m : {mesh_vert, mesh_frag, beam_vert, beam_frag})
		vkDestroyShaderModule(m_device, m, nullptr);
	if (res != VK_SUCCESS)
	{
		Console.Error("Light gun: could not create the pipelines.");
		return false;
	}
	return true;
}

bool GunRenderer::CreateMeshBuffer()
{
	const auto& vertices = m_model.Vertices();
	const auto& indices = m_model.Indices();
	const VkDeviceSize vertex_bytes = vertices.size() * sizeof(GunModel::Vertex);
	m_index_offset = (vertex_bytes + 15) & ~VkDeviceSize(15);
	VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = m_index_offset + indices.size() * sizeof(uint32_t);
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	if (vkCreateBuffer(m_device, &info, nullptr, &m_mesh) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(m_device, m_mesh, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex =
		FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &m_mesh_memory) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(m_device, m_mesh, m_mesh_memory, 0);
	void* mapped = nullptr;
	if (vkMapMemory(m_device, m_mesh_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		return false;
	std::memcpy(mapped, vertices.data(), vertex_bytes);
	std::memcpy(static_cast<char*>(mapped) + m_index_offset, indices.data(), indices.size() * sizeof(uint32_t));
	vkUnmapMemory(m_device, m_mesh_memory);
	return true;
}

void GunRenderer::Destroy()
{
	m_created = false;
	if (!m_device)
		return;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
		for (Eye& eye : m_eyes)
			DestroyEye(eye);
	}
	for (VkFence& fence : m_fences)
	{
		if (fence)
			vkDestroyFence(m_device, fence, nullptr);
		fence = VK_NULL_HANDLE;
	}
	if (m_pool)
		vkDestroyCommandPool(m_device, m_pool, nullptr);
	if (m_mesh)
		vkDestroyBuffer(m_device, m_mesh, nullptr);
	if (m_mesh_memory)
		vkFreeMemory(m_device, m_mesh_memory, nullptr);
	for (VkPipeline pipeline : {m_mesh_pipeline, m_beam_pipeline, m_dot_pipeline})
	{
		if (pipeline)
			vkDestroyPipeline(m_device, pipeline, nullptr);
	}
	for (VkPipelineLayout layout : {m_mesh_layout, m_beam_layout})
	{
		if (layout)
			vkDestroyPipelineLayout(m_device, layout, nullptr);
	}
	if (m_render_pass)
		vkDestroyRenderPass(m_device, m_render_pass, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_mesh = VK_NULL_HANDLE;
	m_mesh_memory = VK_NULL_HANDLE;
	m_mesh_pipeline = m_beam_pipeline = m_dot_pipeline = VK_NULL_HANDLE;
	m_mesh_layout = m_beam_layout = VK_NULL_HANDLE;
	m_render_pass = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

XrPosef GunRenderer::MuzzlePose(const XrPosef& pose) const
{
	return Compose(pose, m_model.Muzzle());
}

const XrCompositionLayerBaseHeader* GunRenderer::Render(XrTime display_time, const std::array<Gun, 2>& guns)
{
	if (!m_created || (!guns[0].visible && !guns[1].visible))
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
		return nullptr;

	const size_t slot = m_slot;
	m_slot = (m_slot + 1) % NUM_SLOTS;
	vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
	vkResetFences(m_device, 1, &m_fences[slot]);

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
			Console.ErrorFmt("Light gun: swapchain image for eye {} failed ({})", i, static_cast<int>(res));
			std::lock_guard lock(GraphicsQueueMutex());
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			for (int j = 0; j < i; j++)
				xrReleaseSwapchainImage(m_eyes[j].swapchain, &release);
			// Nothing was submitted with the fence: signal it again with an empty submit.
			std::lock_guard queue_lock(GraphicsQueueMutex());
			vkQueueSubmit(m_queue, 0, nullptr, m_fences[slot]);
			return nullptr;
		}
	}

	// Each part of a gun: its pose in the room (animated) and colour.
	struct PartDraw
	{
		const GunModel::Draw* draw;
		XrPosef pose;
		float colour[4];
	};
	std::vector<PartDraw> parts;
	for (const Gun& gun : guns)
	{
		if (!gun.visible)
			continue;
		for (size_t p = 0; p < static_cast<size_t>(GunModel::Part::Count); p++)
		{
			const GunModel::Part part = static_cast<GunModel::Part>(p);
			const GunModel::Draw& draw = m_model.Get(part);
			if (!draw.present)
				continue;
			PartDraw d{&draw, draw.node, {}};
			float roughness = draw.roughness;
			switch (part)
			{
				case GunModel::Part::Shell:
					LinearColor(gun.body_color, d.colour);
					break;
				case GunModel::Part::Lens:
					std::memcpy(d.colour, draw.color, sizeof(draw.color));
					break;
				case GunModel::Part::Trigger:
					LinearColor(TRIGGER_COLOR, d.colour);
					roughness = TRIGGER_ROUGHNESS;
					d.pose.orientation = d.pose.orientation *
					                     QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, TRIGGER_TRAVEL * std::clamp(gun.trigger, 0.0f, 1.0f));
					break;
				case GunModel::Part::ButtonLeft:
				case GunModel::Part::ButtonRight:
				{
					LinearColor(BUTTON_COLOR, d.colour);
					roughness = BUTTON_ROUGHNESS;
					const bool left = part == GunModel::Part::ButtonLeft;
					const float press = std::clamp(left ? gun.press_left : gun.press_right, 0.0f, 1.0f);
					d.pose.position.x += (left ? BUTTON_TRAVEL : -BUTTON_TRAVEL) * press;
					break;
				}
				default:
					break;
			}
			d.colour[3] = roughness;
			d.pose = Compose(gun.pose, d.pose);
			parts.push_back(d);
		}
	}

	VkCommandBuffer cmd = m_cmd[slot];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	for (int i = 0; i < 2; i++)
	{
		const Eye& eye = m_eyes[i];
		const XrPosef& eye_pose = views[i].pose;
		const Mat4 view = ViewFromPose(eye_pose);
		const Mat4 vp = Multiply(Projection(views[i].fov, NEAR_Z, FAR_Z), view);
		const XrQuaternionf to_view = Conjugate(eye_pose.orientation);

		VkClearValue clears[2] = {};
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

		// The guns.
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_mesh_pipeline);
		const VkDeviceSize vertex_offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &m_mesh, &vertex_offset);
		vkCmdBindIndexBuffer(cmd, m_mesh, m_index_offset, VK_INDEX_TYPE_UINT32);
		for (const PartDraw& d : parts)
		{
			MeshConstants c{};
			const Mat4 mvp = Multiply(vp, MatrixFromPose(d.pose));
			std::memcpy(c.mvp, mvp.m, sizeof(c.mvp));
			const Mat4 normal = MatrixFromPose({to_view * d.pose.orientation, {0.0f, 0.0f, 0.0f}});
			for (int col = 0; col < 3; col++)
				std::memcpy(c.normal_to_view[col], &normal.m[col * 4], 3 * sizeof(float));
			std::memcpy(c.colour, d.colour, sizeof(c.colour));
			vkCmdPushConstants(cmd, m_mesh_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
			vkCmdDrawIndexed(cmd, d.draw->index_count, 1, d.draw->first_index, 0, 0);
		}

		// The beams, then the dots on top.
		const XrVector3f eye_pos = eye_pose.position;
		for (int pass = 0; pass < 2; pass++)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? m_beam_pipeline : m_dot_pipeline);
			for (const Gun& gun : guns)
			{
				if (!gun.visible || !(pass == 0 ? gun.beam : gun.dot))
					continue;
				QuadConstants q{};
				LinearColor(gun.beam_color, q.colour);
				XrVector3f corners[4];
				if (pass == 0)
				{
					// A strip from the muzzle to the end, turned about its length to face the eye.
					const XrVector3f start = MuzzlePose(gun.pose).position;
					const XrVector3f mid = (start + gun.beam_end) * 0.5f;
					XrVector3f side = Normalize(Cross(gun.beam_end - start, eye_pos - mid));
					side = side * (0.5f * BEAM_WIDTH);
					corners[0] = start - side;
					corners[1] = start + side;
					corners[2] = gun.beam_end - side;
					corners[3] = gun.beam_end + side;
					q.colour[3] = 0.9f;
				}
				else
				{
					const XrVector3f to_eye = eye_pos - gun.beam_end;
					const float distance = std::sqrt(Dot(to_eye, to_eye));
					const XrVector3f forward = Normalize(to_eye);
					XrVector3f right = Normalize(Cross({0.0f, 1.0f, 0.0f}, forward));
					if (Dot(right, right) < 0.5f)
						right = {1.0f, 0.0f, 0.0f};
					const XrVector3f up = Cross(forward, right);
					const float size = DOT_SIZE + DOT_GROWTH * distance;
					const XrVector3f r = right * size, u = up * size;
					corners[0] = gun.beam_end - r - u;
					corners[1] = gun.beam_end - r + u;
					corners[2] = gun.beam_end + r - u;
					corners[3] = gun.beam_end + r + u;
					q.colour[3] = 1.0f;
					q.params[0] = 1.0f;
				}
				for (int c = 0; c < 4; c++)
					ClipPoint(vp, corners[c], q.corners[c]);
				vkCmdPushConstants(cmd, m_beam_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(q), &q);
				vkCmdDraw(cmd, 4, 1, 0, 0);
			}
		}
		vkCmdEndRenderPass(cmd);
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
	m_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	m_layer.space = m_xr->LocalSpace();
	m_layer.viewCount = 2;
	m_layer.views = m_layer_views.data();
	return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_layer);
}
