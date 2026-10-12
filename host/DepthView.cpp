// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#include "DepthView.h"

#include <cstdint>

#include "depth_background.comp.h"
#include "depth_hist.comp.h"
#include "depth_hud.comp.h"
#include "depth_normalize.comp.h"
#include "depth_range.comp.h"
#include "depth_stats.comp.h"
#include "depth_view.comp.h"

#include "common/Console.h"
#include "common/Image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	struct Params
	{
		float src_uv[4];
		float draw_rect[4];
		int32_t canvas_size[2];
		int32_t out_size[2];
		int32_t colour_size[2];
		float depth_scale;
		float hud_threshold;
		int32_t live_depth;
	};

	// The stats and range blocks of depth_common.glsl.
	struct StatsBlock
	{
		uint32_t zmin, zmax, valid, total, hud;
	};
	struct RangeBlock
	{
		float lo, hi;
		uint32_t initialized, pad;
	};
	constexpr VkDeviceSize HISTOGRAM_SIZE = 256 * sizeof(uint32_t);

	// The presented canvas is a filtered resample of the frame the pre-HUD copy comes from, so
	// small differences are expected everywhere; a HUD changes colours a lot.
	constexpr float HUD_THRESHOLD = 0.15f;

	// Grid resolution on the long side: the window mesh has a vertex per cell.
	constexpr uint32_t MAX_VIEW_SIZE = 384;

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

DepthView::~DepthView()
{
	Destroy();
}

uint32_t DepthView::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
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

bool DepthView::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer)
{
	VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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
	buffer->size = size;
	return true;
}

void DepthView::DestroyBuffer(Buffer* buffer)
{
	if (buffer->buffer)
		vkDestroyBuffer(m_device, buffer->buffer, nullptr);
	if (buffer->memory)
		vkFreeMemory(m_device, buffer->memory, nullptr);
	*buffer = {};
}

VkPipeline DepthView::CreatePipeline(const uint32_t* spirv, size_t size)
{
	VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module_info.codeSize = size;
	module_info.pCode = spirv;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(m_device, &module_info, nullptr, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = m_pipeline_layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
	vkDestroyShaderModule(m_device, module, nullptr);
	return pipeline;
}

bool DepthView::Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index)
{
	m_device = device;
	m_gpu = gpu;
	m_queue = queue;

	if (!vkCmdPushDescriptorSetKHR)
	{
		Console.Error("Depth view: VK_KHR_push_descriptor is not available.");
		return false;
	}

	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(device, &sampler, nullptr, &m_sampler) != VK_SUCCESS)
		return false;
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	if (vkCreateSampler(device, &sampler, nullptr, &m_linear_sampler) != VK_SUCCESS)
		return false;

	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	set_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &m_set_layout) != VK_SUCCESS)
		return false;

	const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
	VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &m_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &range;
	if (vkCreatePipelineLayout(device, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS)
		return false;

	m_stats_pipeline = CreatePipeline(depth_stats_comp, sizeof(depth_stats_comp));
	m_hist_pipeline = CreatePipeline(depth_hist_comp, sizeof(depth_hist_comp));
	m_range_pipeline = CreatePipeline(depth_range_comp, sizeof(depth_range_comp));
	m_normalize_pipeline = CreatePipeline(depth_normalize_comp, sizeof(depth_normalize_comp));
	m_background_pipeline = CreatePipeline(depth_background_comp, sizeof(depth_background_comp));
	m_hud_pipeline = CreatePipeline(depth_hud_comp, sizeof(depth_hud_comp));
	m_view_pipeline = CreatePipeline(depth_view_comp, sizeof(depth_view_comp));
	if (!m_stats_pipeline || !m_hist_pipeline || !m_range_pipeline || !m_normalize_pipeline || !m_background_pipeline ||
		!m_hud_pipeline || !m_view_pipeline)
	{
		Console.Error("Depth view: could not create the compute pipelines.");
		return false;
	}

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
			!CreateBuffer(sizeof(StatsBlock), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				&m_stats[i]) ||
			!CreateBuffer(HISTOGRAM_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				&m_histogram[i]))
			return false;
	}
	if (!CreateBuffer(sizeof(RangeBlock), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &m_range))
		return false;
	std::memset(m_range.mapped, 0, sizeof(RangeBlock));
	return true;
}

void DepthView::Destroy()
{
	if (!m_device)
		return;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
	}
	DestroyOutput();
	for (size_t i = 0; i < NUM_SLOTS; i++)
	{
		if (m_fences[i])
			vkDestroyFence(m_device, m_fences[i], nullptr);
		m_fences[i] = VK_NULL_HANDLE;
		DestroyBuffer(&m_stats[i]);
		DestroyBuffer(&m_histogram[i]);
	}
	DestroyBuffer(&m_range);
	for (VkPipeline* pipeline :
		{&m_hist_pipeline, &m_range_pipeline, &m_normalize_pipeline, &m_background_pipeline, &m_hud_pipeline})
	{
		if (*pipeline)
			vkDestroyPipeline(m_device, *pipeline, nullptr);
		*pipeline = VK_NULL_HANDLE;
	}
	if (m_pool)
		vkDestroyCommandPool(m_device, m_pool, nullptr);
	if (m_stats_pipeline)
		vkDestroyPipeline(m_device, m_stats_pipeline, nullptr);
	if (m_view_pipeline)
		vkDestroyPipeline(m_device, m_view_pipeline, nullptr);
	if (m_pipeline_layout)
		vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
	if (m_set_layout)
		vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
	if (m_sampler)
		vkDestroySampler(m_device, m_sampler, nullptr);
	if (m_linear_sampler)
		vkDestroySampler(m_device, m_linear_sampler, nullptr);
	m_linear_sampler = VK_NULL_HANDLE;
	m_pool = VK_NULL_HANDLE;
	m_stats_pipeline = m_view_pipeline = VK_NULL_HANDLE;
	m_pipeline_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

bool DepthView::EnsureOutput(uint32_t width, uint32_t height)
{
	if (m_output && width == m_output_width && height == m_output_height)
		return true;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
	}
	DestroyOutput();

	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent = {width, height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_device, &info, nullptr, &m_output) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_device, m_output, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &m_output_memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, m_output, m_output_memory, 0);

	VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view.image = m_output;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = info.format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(m_device, &view, nullptr, &m_output_view) != VK_SUCCESS)
		return false;

	// The normalised grid, same size.
	info.format = VK_FORMAT_R32_SFLOAT;
	info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (vkCreateImage(m_device, &info, nullptr, &m_normalized) != VK_SUCCESS)
		return false;
	vkGetImageMemoryRequirements(m_device, m_normalized, &req);
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX ||
		vkAllocateMemory(m_device, &alloc, nullptr, &m_normalized_memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, m_normalized, m_normalized_memory, 0);
	view.image = m_normalized;
	view.format = info.format;
	if (vkCreateImageView(m_device, &view, nullptr, &m_normalized_view) != VK_SUCCESS)
		return false;

	info.format = VK_FORMAT_R32G32B32A32_SFLOAT; // depth + offset to the far cell
	if (vkCreateImage(m_device, &info, nullptr, &m_background) != VK_SUCCESS)
		return false;
	vkGetImageMemoryRequirements(m_device, m_background, &req);
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX ||
		vkAllocateMemory(m_device, &alloc, nullptr, &m_background_memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, m_background, m_background_memory, 0);
	view.image = m_background;
	view.format = info.format;
	if (vkCreateImageView(m_device, &view, nullptr, &m_background_view) != VK_SUCCESS)
		return false;

	info.format = VK_FORMAT_R32_SFLOAT;
	info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (vkCreateImage(m_device, &info, nullptr, &m_hud_history) != VK_SUCCESS)
		return false;
	vkGetImageMemoryRequirements(m_device, m_hud_history, &req);
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX ||
		vkAllocateMemory(m_device, &alloc, nullptr, &m_hud_history_memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(m_device, m_hud_history, m_hud_history_memory, 0);
	view.image = m_hud_history;
	view.format = info.format;
	if (vkCreateImageView(m_device, &view, nullptr, &m_hud_history_view) != VK_SUCCESS)
		return false;

	m_output_width = width;
	m_output_height = height;
	m_output_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	m_normalized_initialized = false;
	return true;
}

void DepthView::DestroyOutput()
{
	if (m_output_view)
		vkDestroyImageView(m_device, m_output_view, nullptr);
	if (m_output)
		vkDestroyImage(m_device, m_output, nullptr);
	if (m_output_memory)
		vkFreeMemory(m_device, m_output_memory, nullptr);
	m_output_view = VK_NULL_HANDLE;
	m_output = VK_NULL_HANDLE;
	m_output_memory = VK_NULL_HANDLE;
	if (m_normalized_view)
		vkDestroyImageView(m_device, m_normalized_view, nullptr);
	if (m_normalized)
		vkDestroyImage(m_device, m_normalized, nullptr);
	if (m_normalized_memory)
		vkFreeMemory(m_device, m_normalized_memory, nullptr);
	m_normalized_view = VK_NULL_HANDLE;
	m_normalized = VK_NULL_HANDLE;
	m_normalized_memory = VK_NULL_HANDLE;
	if (m_background_view)
		vkDestroyImageView(m_device, m_background_view, nullptr);
	if (m_background)
		vkDestroyImage(m_device, m_background, nullptr);
	if (m_background_memory)
		vkFreeMemory(m_device, m_background_memory, nullptr);
	m_background_view = VK_NULL_HANDLE;
	m_background = VK_NULL_HANDLE;
	m_background_memory = VK_NULL_HANDLE;
	if (m_hud_history_view)
		vkDestroyImageView(m_device, m_hud_history_view, nullptr);
	if (m_hud_history)
		vkDestroyImage(m_device, m_hud_history, nullptr);
	if (m_hud_history_memory)
		vkFreeMemory(m_device, m_hud_history_memory, nullptr);
	m_hud_history_view = VK_NULL_HANDLE;
	m_hud_history = VK_NULL_HANDLE;
	m_hud_history_memory = VK_NULL_HANDLE;
	m_output_width = m_output_height = 0;
}

bool DepthView::Process(const VKLibretro::Frame& frame, VKLibretro::Frame* view)
{
	if (!frame.depth_image || !frame.depth_view || !frame.depth_width || !frame.depth_height)
	{
		// The GPU may still be easing the range: wait before resetting it.
		if (++m_frames_without_depth == 30 && m_range.mapped)
		{
			std::lock_guard lock(GraphicsQueueMutex());
			vkQueueWaitIdle(m_queue);
			std::memset(m_range.mapped, 0, sizeof(RangeBlock));
		}
		return false;
	}
	m_frames_without_depth = 0;

	// The view has the presented region's aspect, at most MAX_VIEW_SIZE on its long side.
	const float draw_w = frame.depth_draw_rect[2] - frame.depth_draw_rect[0];
	const float draw_h = frame.depth_draw_rect[3] - frame.depth_draw_rect[1];
	if (draw_w < 1.0f || draw_h < 1.0f)
		return false;
	const float fit = std::min(1.0f, static_cast<float>(MAX_VIEW_SIZE) / std::max(draw_w, draw_h));
	const uint32_t out_w = std::max(16u, static_cast<uint32_t>(std::lround(draw_w * fit)));
	const uint32_t out_h = std::max(16u, static_cast<uint32_t>(std::lround(draw_h * fit)));
	if (!EnsureOutput(out_w, out_h))
		return false;

	const size_t slot = m_slot;
	m_slot = (m_slot + 1) % NUM_SLOTS;
	vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
	vkResetFences(m_device, 1, &m_fences[slot]);
	if (m_stats_pending[slot])
	{
		const StatsBlock* s = static_cast<const StatsBlock*>(m_stats[slot].mapped);
		Stats stats;
		stats.coverage = s->total ? static_cast<float>(s->valid) / static_cast<float>(s->total) : 0.0f;
		stats.hud = s->total ? static_cast<float>(s->hud) / static_cast<float>(s->total) : 0.0f;
		if (s->valid)
		{
			std::memcpy(&stats.zmin, &s->zmin, sizeof(float));
			std::memcpy(&stats.zmax, &s->zmax, sizeof(float));
		}
		const RangeBlock* r = static_cast<const RangeBlock*>(m_range.mapped);
		stats.lo = r->lo;
		stats.hi = r->hi;
		m_last_stats = stats;
	}

	const bool dump = !m_dump_prefix.empty();
	Buffer depth_readback, colour_readback;
	const uint32_t colour_x = static_cast<uint32_t>(std::max(0.0f, frame.depth_draw_rect[0]));
	const uint32_t colour_y = static_cast<uint32_t>(std::max(0.0f, frame.depth_draw_rect[1]));
	const uint32_t colour_w = std::min(static_cast<uint32_t>(draw_w), frame.width - colour_x);
	const uint32_t colour_h = std::min(static_cast<uint32_t>(draw_h), frame.height - colour_y);
	if (dump && (!CreateBuffer(VkDeviceSize(out_w) * out_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &depth_readback) ||
					!CreateBuffer(VkDeviceSize(colour_w) * colour_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &colour_readback)))
	{
		DestroyBuffer(&depth_readback);
		DestroyBuffer(&colour_readback);
		m_dump_prefix.clear();
		return false;
	}

	VkCommandBuffer cmd = m_cmd[slot];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	// Reset the stats (zmin starts at the largest bit pattern).
	const VkBuffer stats_buffer = m_stats[slot].buffer;
	vkCmdFillBuffer(cmd, stats_buffer, 0, 4, 0xFFFFFFFFu);
	vkCmdFillBuffer(cmd, stats_buffer, 4, sizeof(StatsBlock) - 4, 0u);
	vkCmdFillBuffer(cmd, m_histogram[slot].buffer, 0, HISTOGRAM_SIZE, 0u);

	// The GS rendered the depth canvas earlier on this queue; the last panel copy read the output.
	VkMemoryBarrier global{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	global.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	global.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	// The window's last draw sampled the normalised grid; it stays in GENERAL after the first frame.
	const VkImageLayout grid_layout = m_normalized_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
	if (!m_normalized_initialized)
	{
		// The HUD history starts empty.
		VkImageMemoryBarrier to_dst = ImageBarrier(m_hud_history, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			nullptr, 1, &to_dst);
		const VkClearColorValue zero{};
		const VkImageSubresourceRange range_all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, m_hud_history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range_all);
	}
	VkImageMemoryBarrier to_general[4] = {
		ImageBarrier(m_output, m_output_layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_SHADER_WRITE_BIT),
		ImageBarrier(m_normalized, grid_layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
			VK_ACCESS_SHADER_WRITE_BIT),
		ImageBarrier(m_background, grid_layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
			VK_ACCESS_SHADER_WRITE_BIT),
		ImageBarrier(m_hud_history,
			m_normalized_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &global, 0,
		nullptr, 4, to_general);
	m_normalized_initialized = true;

	const VkDescriptorImageInfo depth_info{m_sampler, frame.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorBufferInfo stats_info{stats_buffer, 0, sizeof(StatsBlock)};
	const VkDescriptorImageInfo out_info{VK_NULL_HANDLE, m_output_view, VK_IMAGE_LAYOUT_GENERAL};
	// Without a pre-HUD copy, compare the colour with itself: nothing counts as HUD.
	const VkDescriptorImageInfo colour_info{m_linear_sampler, frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorImageInfo prehud_info{
		m_linear_sampler, frame.prehud_view ? frame.prehud_view : frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorBufferInfo histogram_info{m_histogram[slot].buffer, 0, HISTOGRAM_SIZE};
	const VkDescriptorBufferInfo range_info{m_range.buffer, 0, sizeof(RangeBlock)};
	const VkDescriptorImageInfo normalized_info{VK_NULL_HANDLE, m_normalized_view, VK_IMAGE_LAYOUT_GENERAL};
	const VkDescriptorImageInfo normalized_read_info{m_sampler, m_normalized_view, VK_IMAGE_LAYOUT_GENERAL};
	const VkDescriptorImageInfo background_info{VK_NULL_HANDLE, m_background_view, VK_IMAGE_LAYOUT_GENERAL};
	// Without a live depth, the depth image stands in (the shaders do not read it then).
	const VkDescriptorImageInfo live_info{m_sampler, frame.live_depth_view ? frame.live_depth_view : frame.depth_view,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorImageInfo history_info{VK_NULL_HANDLE, m_hud_history_view, VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet writes[12] = {};
	for (VkWriteDescriptorSet& w : writes)
	{
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.descriptorCount = 1;
	}
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &depth_info;
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pBufferInfo = &stats_info;
	writes[2].dstBinding = 2;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[2].pImageInfo = &out_info;
	writes[3].dstBinding = 3;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[3].pImageInfo = &colour_info;
	writes[4].dstBinding = 4;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[4].pImageInfo = &prehud_info;
	writes[5].dstBinding = 5;
	writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[5].pBufferInfo = &histogram_info;
	writes[6].dstBinding = 6;
	writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[6].pBufferInfo = &range_info;
	writes[7].dstBinding = 7;
	writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[7].pImageInfo = &normalized_info;
	writes[8].dstBinding = 8;
	writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[8].pImageInfo = &normalized_read_info;
	writes[9].dstBinding = 9;
	writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[9].pImageInfo = &background_info;
	writes[10].dstBinding = 10;
	writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[10].pImageInfo = &live_info;
	writes[11].dstBinding = 11;
	writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[11].pImageInfo = &history_info;

	Params params{};
	std::memcpy(params.src_uv, frame.depth_src_uv, sizeof(params.src_uv));
	std::memcpy(params.draw_rect, frame.depth_draw_rect, sizeof(params.draw_rect));
	params.colour_size[0] = static_cast<int32_t>(frame.width);
	params.colour_size[1] = static_cast<int32_t>(frame.height);
	params.hud_threshold = frame.prehud_view ? HUD_THRESHOLD : 0.0f;
	params.live_depth = frame.live_depth_view ? 1 : 0;
	params.canvas_size[0] = static_cast<int32_t>(frame.depth_width);
	params.canvas_size[1] = static_cast<int32_t>(frame.depth_height);
	params.out_size[0] = static_cast<int32_t>(out_w);
	params.out_size[1] = static_cast<int32_t>(out_h);
	params.depth_scale = frame.depth_scale;
	const uint32_t groups_x = (out_w + 15) / 16, groups_y = (out_h + 15) / 16;

	// stats -> histogram -> range -> normalised grid and debug view, each reading the previous.
	VkMemoryBarrier pass_done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	pass_done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	pass_done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	const auto run = [&](VkPipeline pipeline, uint32_t gx, uint32_t gy, bool barrier_after) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 12, writes);
		vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
		vkCmdDispatch(cmd, gx, gy, 1);
		if (barrier_after)
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
				&pass_done, 0, nullptr, 0, nullptr);
	};
	run(m_stats_pipeline, groups_x, groups_y, true);
	run(m_hist_pipeline, groups_x, groups_y, true);
	run(m_range_pipeline, 1, 1, true);
	run(m_hud_pipeline, groups_x, groups_y, true);
	run(m_normalize_pipeline, groups_x, groups_y, true);
	run(m_background_pipeline, groups_x, groups_y, false);
	run(m_view_pipeline, groups_x, groups_y, false);

	// The window's vertex shader samples the grid next.
	VkMemoryBarrier grid_ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	grid_ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	grid_ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
		&grid_ready, 0, nullptr, 0, nullptr);

	VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	host_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	host_read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0,
		nullptr, 0, nullptr);

	if (dump)
	{
		VkImageMemoryBarrier to_src[2] = {
			ImageBarrier(m_output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
			ImageBarrier(frame.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			nullptr, 2, to_src);
		VkBufferImageCopy depth_copy{};
		depth_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		depth_copy.imageExtent = {out_w, out_h, 1};
		vkCmdCopyImageToBuffer(cmd, m_output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_readback.buffer, 1, &depth_copy);
		VkBufferImageCopy colour_copy{};
		colour_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		colour_copy.imageOffset = {static_cast<int32_t>(colour_x), static_cast<int32_t>(colour_y), 0};
		colour_copy.imageExtent = {colour_w, colour_h, 1};
		vkCmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colour_readback.buffer, 1, &colour_copy);
		VkImageMemoryBarrier back[2] = {
			ImageBarrier(m_output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT),
			ImageBarrier(frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT),
		};
		VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		readback.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		readback.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &readback, 0, nullptr, 2, back);
	}
	else
	{
		// ScreenLayer copies from SHADER_READ_ONLY_OPTIMAL.
		VkImageMemoryBarrier to_read = ImageBarrier(m_output, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &to_read);
	}
	m_output_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueSubmit(m_queue, 1, &submit, m_fences[slot]);
	}
	m_stats_pending[slot] = true;

	if (dump)
	{
		vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
		WriteDump(depth_readback, out_w, out_h, colour_readback, colour_w, colour_h);
		DestroyBuffer(&depth_readback);
		DestroyBuffer(&colour_readback);
		m_dump_prefix.clear();
	}

	*view = {};
	view->image = m_output;
	view->view = m_output_view;
	view->format = VK_FORMAT_R8G8B8A8_UNORM;
	view->width = out_w;
	view->height = out_h;
	return true;
}

void DepthView::WriteDump(const Buffer& depth, uint32_t dw, uint32_t dh, const Buffer& colour, uint32_t cw, uint32_t ch)
{
	const std::string depth_path = m_dump_prefix + "-depth.png";
	const std::string colour_path = m_dump_prefix + "-colour.png";
	RGBA8Image depth_image(dw, dh, static_cast<const u32*>(depth.mapped));
	RGBA8Image colour_image(cw, ch, static_cast<const u32*>(colour.mapped));
	// The GS output's alpha is not meaningful for display; keep the dump opaque.
	for (u32 i = 0; i < cw * ch; i++)
		colour_image.GetPixels()[i] |= 0xFF000000u;
	const bool ok = depth_image.SaveToFile(depth_path.c_str()) && colour_image.SaveToFile(colour_path.c_str());
	Console.WriteLnFmt("Depth view: dump {} ({}).", m_dump_prefix, ok ? "written" : "failed");
}
