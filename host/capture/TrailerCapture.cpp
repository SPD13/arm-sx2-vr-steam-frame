// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "capture/TrailerCapture.h"

#include "capture_color.comp.h"
#include "capture_depth.comp.h"
#include "capture_depth_layered.comp.h"
#include "capture_hud.comp.h"
#include "capture_hud_layered.comp.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace
{
	// The push constants of capture_color.comp and capture_depth.glsl (one layout for both).
	struct ColorParams
	{
		uint32_t size[2];
		uint32_t offset;
	};
	struct DepthParams
	{
		float src_uv[4];
		float draw_rect[4];
		float frame_size[2];
		uint32_t size[2];
		float depth_scale;
		uint32_t offset;
		float layer;
	};
	struct HudParams
	{
		float src_uv[4];
		float draw_rect[4];
		float frame_size[2];
		uint32_t size[2];
		float depth_scale;
		uint32_t offset;
	};
	constexpr uint32_t PUSH_SIZE = 64;
	// The depth planes at about the PS2's resolution: with the depth extras on (the HUD mask needs
	// them) the GS hands over depth at the upscaled size.
	constexpr uint32_t MAX_DEPTH_WIDTH = 960;
	static_assert(sizeof(ColorParams) <= PUSH_SIZE && sizeof(DepthParams) <= PUSH_SIZE && sizeof(HudParams) <= PUSH_SIZE);

	void AppendPose(std::string& out, const XrPosef& p)
	{
		fmt::format_to(std::back_inserter(out), "[{:.4f},{:.4f},{:.4f},{:.5f},{:.5f},{:.5f},{:.5f}]", p.position.x,
			p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
	}
} // namespace

TrailerCapture::~TrailerCapture()
{
	Destroy();
}

uint32_t TrailerCapture::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
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

VkPipeline TrailerCapture::CreatePipeline(const uint32_t* spirv, size_t size, VkPipelineLayout layout)
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
	info.layout = layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
	vkDestroyShaderModule(m_device, module, nullptr);
	return pipeline;
}

bool TrailerCapture::Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index,
	const Options& options)
{
	m_device = device;
	m_gpu = gpu;
	m_queue = queue;
	m_options = options;
	if (!vkCmdPushDescriptorSetKHR)
	{
		Console.Error("Capture: VK_KHR_push_descriptor is not available.");
		return false;
	}

	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	if (vkCreateSampler(device, &sampler, nullptr, &m_linear) != VK_SUCCESS)
		return false;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	if (vkCreateSampler(device, &sampler, nullptr, &m_nearest) != VK_SUCCESS)
		return false;

	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	set_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &m_set_layout) != VK_SUCCESS)
		return false;
	const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE};
	VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &m_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(device, &layout_info, nullptr, &m_layout) != VK_SUCCESS)
		return false;
	m_color = CreatePipeline(capture_color_comp, sizeof(capture_color_comp), m_layout);
	m_depth = CreatePipeline(capture_depth_comp, sizeof(capture_depth_comp), m_layout);
	m_depth_layered = CreatePipeline(capture_depth_layered_comp, sizeof(capture_depth_layered_comp), m_layout);
	m_hud = CreatePipeline(capture_hud_comp, sizeof(capture_hud_comp), m_layout);
	m_hud_layered = CreatePipeline(capture_hud_layered_comp, sizeof(capture_hud_layered_comp), m_layout);
	if (!m_color || !m_depth || !m_depth_layered || !m_hud || !m_hud_layered)
	{
		Console.Error("Capture: could not create the pipelines.");
		return false;
	}

	// The CPU reads every byte the GPU writes: cached memory reads several times faster than
	// write-combined, so it comes first (invalidated before reading when it is not coherent).
	VkBufferCreateInfo probe_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	probe_info.size = 4096;
	probe_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer probe = VK_NULL_HANDLE;
	if (vkCreateBuffer(device, &probe_info, nullptr, &probe) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, probe, &req);
	vkDestroyBuffer(device, probe, nullptr);
	constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	constexpr VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	m_memory_type = FindMemoryType(req.memoryTypeBits, visible | cached | coherent);
	m_coherent = true;
	if (m_memory_type == UINT32_MAX)
	{
		m_memory_type = FindMemoryType(req.memoryTypeBits, visible | cached);
		m_coherent = false;
	}
	if (m_memory_type == UINT32_MAX)
	{
		m_memory_type = FindMemoryType(req.memoryTypeBits, visible | coherent);
		m_coherent = true;
	}
	if (m_memory_type == UINT32_MAX)
		return false;

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index;
	VkCommandPool pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
		return false;
	for (Slot& slot : m_slots)
	{
		VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		alloc.commandPool = pool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 1;
		VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkAllocateCommandBuffers(device, &alloc, &slot.cmd) != VK_SUCCESS ||
			vkCreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS)
		{
			vkDestroyCommandPool(device, pool, nullptr);
			return false;
		}
	}

	if (!m_writer.Start(options.dir, NUM_WRITERS, static_cast<uint64_t>(options.max_gb * 1e9)))
	{
		Console.ErrorFmt("Capture: cannot write to {} (it must not hold a capture already)", options.dir);
		vkDestroyCommandPool(device, pool, nullptr);
		return false;
	}
	m_pool = pool; // Active() from here
	m_start = std::chrono::steady_clock::now();
	m_start_realtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	m_next_log = m_start + std::chrono::seconds(10);
	WriteSummary(false);
	if (options.record_now)
		SetRecording(true);
	Console.WriteLnFmt("Capture: armed, into {} (memory {}cached, {}coherent; up to {:.0f} GB); the record button starts it", options.dir,
		FindMemoryType(1u << m_memory_type, cached) == m_memory_type ? "" : "not ", m_coherent ? "" : "not ", options.max_gb);
	return true;
}

void TrailerCapture::FreeSlotMemory(Slot& slot)
{
	if (slot.buffer)
		vkDestroyBuffer(m_device, slot.buffer, nullptr);
	if (slot.memory)
		vkFreeMemory(m_device, slot.memory, nullptr);
	slot.buffer = VK_NULL_HANDLE;
	slot.memory = VK_NULL_HANDLE;
	slot.mapped = nullptr;
	slot.capacity = 0;
}

bool TrailerCapture::EnsureCapacity(Slot& slot, VkDeviceSize bytes)
{
	if (slot.capacity >= bytes)
		return true;
	FreeSlotMemory(slot);
	VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_info.size = bytes;
	buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (vkCreateBuffer(m_device, &buffer_info, nullptr, &slot.buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(m_device, slot.buffer, &req);
	VkMemoryAllocateInfo mem{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mem.allocationSize = req.size;
	mem.memoryTypeIndex = m_memory_type;
	if (!(req.memoryTypeBits & (1u << m_memory_type)) || vkAllocateMemory(m_device, &mem, nullptr, &slot.memory) != VK_SUCCESS)
	{
		FreeSlotMemory(slot);
		return false;
	}
	vkBindBufferMemory(m_device, slot.buffer, slot.memory, 0);
	if (vkMapMemory(m_device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS)
	{
		FreeSlotMemory(slot);
		return false;
	}
	slot.capacity = bytes;
	return true;
}

void TrailerCapture::Destroy()
{
	if (!m_device)
		return;
	if (m_pool)
	{
		{
			std::lock_guard lock(GraphicsQueueMutex());
			vkQueueWaitIdle(m_queue);
		}
		Poll(); // the frames still on the GPU go to the writers too
		m_writer.Finish();
		WriteSummary(true);
		Console.WriteLnFmt("Capture: {} frames written ({:.2f} GB), {} dropped, in {}", m_writer.FramesWritten(),
			m_writer.BytesWritten() / 1e9, m_dropped_total, m_options.dir);
	}
	for (Slot& slot : m_slots)
	{
		FreeSlotMemory(slot);
		if (slot.fence)
			vkDestroyFence(m_device, slot.fence, nullptr);
		slot.fence = VK_NULL_HANDLE;
		slot.cmd = VK_NULL_HANDLE;
	}
	if (m_pool)
		vkDestroyCommandPool(m_device, m_pool, nullptr);
	for (VkPipeline pipeline : {m_color, m_depth, m_depth_layered, m_hud, m_hud_layered})
	{
		if (pipeline)
			vkDestroyPipeline(m_device, pipeline, nullptr);
	}
	if (m_layout)
		vkDestroyPipelineLayout(m_device, m_layout, nullptr);
	if (m_set_layout)
		vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
	for (VkSampler sampler : {m_linear, m_nearest})
	{
		if (sampler)
			vkDestroySampler(m_device, sampler, nullptr);
	}
	m_pool = VK_NULL_HANDLE;
	m_color = m_depth = m_depth_layered = m_hud = m_hud_layered = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_linear = m_nearest = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

bool TrailerCapture::TakeStopped()
{
	if (m_pool && m_writer.Full() && !m_stopped_told)
	{
		m_stopped_told = true;
		Console.WarningFmt("Capture: stopped at {:.1f} GB ({} frames)", m_writer.BytesWritten() / 1e9, m_writer.FramesWritten());
		return true;
	}
	return false;
}

int64_t TrailerCapture::Now() const
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
}

double TrailerCapture::SetRecording(bool recording)
{
	if (!Active() || recording == m_recording)
		return 0.0;
	m_recording = recording;
	const int64_t now = Now();
	RecordEvent("record", recording ? "start" : "stop");
	if (recording)
	{
		m_recording_since = now;
		m_ui_panel = m_ui_atlas = {}; // the images on show are written again with this part
		Console.WriteLnFmt("Capture: recording ({} frames so far)", m_writer.FramesWritten());
		return 0.0;
	}
	const double seconds = (now - m_recording_since) / 1e9;
	Console.WriteLnFmt("Capture: stopped after {:.1f} s ({} frames written, {} dropped)", seconds, m_writer.FramesWritten(),
		m_dropped_total);
	return seconds;
}

void TrailerCapture::Process(const VKLibretro::Frame& frame, VkImageView left, VkImageView right, const View& view)
{
	if (!Recording() || !left || !frame.width || !frame.height)
		return;
	const uint32_t index = m_index++;

	// The pictures' size, scaled down to the widest allowed.
	uint32_t width = frame.width, height = frame.height;
	if (m_options.max_width && width > m_options.max_width)
	{
		height = std::max(1u, static_cast<uint32_t>(std::lround(double(height) * m_options.max_width / width)));
		width = m_options.max_width;
	}
	// The depth over the whole frame, at about the depth image's own resolution.
	const float* dr = frame.depth_draw_rect;
	const float* uv = frame.depth_src_uv;
	const bool has_depth = frame.depth_view && frame.depth_scale > 0.0f && frame.depth_width && dr[2] > dr[0] &&
	                       dr[3] > dr[1] && uv[2] > uv[0];
	const bool layered = frame.right_image != VK_NULL_HANDLE; // multiview: a depth layer per eye
	uint32_t depth_width = 0, depth_height = 0;
	if (has_depth)
	{
		const double texels_per_pixel = (uv[2] - uv[0]) * frame.depth_width / (dr[2] - dr[0]);
		depth_width = std::clamp(static_cast<uint32_t>(std::lround(frame.width * texels_per_pixel)), 1u,
			std::min(width, MAX_DEPTH_WIDTH));
		depth_height = std::max(1u, static_cast<uint32_t>(std::lround(double(depth_width) * height / width)));
	}

	Slot* slot = nullptr;
	for (size_t i = 0; i < NUM_SLOTS && !slot; i++)
	{
		Slot& candidate = m_slots[(m_next + i) % NUM_SLOTS];
		if (candidate.state.load(std::memory_order_acquire) == SLOT_FREE)
		{
			slot = &candidate;
			m_next = (m_next + i + 1) % NUM_SLOTS;
		}
	}
	if (!slot)
	{
		m_dropped++;
		m_dropped_total++;
		return;
	}

	// The planes, one after the other in the slot's buffer.
	CaptureFormat::FrameHeader& header = slot->header;
	header = {};
	const VkDeviceSize color_bytes = VkDeviceSize(width) * height * 4;
	const VkDeviceSize depth_bytes = VkDeviceSize(depth_width) * depth_height * 4;
	VkDeviceSize total = 0;
	const auto add_plane = [&](uint32_t kind, VkDeviceSize bytes) {
		const uint32_t i = header.plane_count++;
		header.planes[i].kind = kind;
		header.planes[i].raw_size = static_cast<uint32_t>(bytes);
		slot->offsets[i] = total;
		total += bytes;
		return i;
	};
	const uint32_t left_plane = add_plane(CaptureFormat::PLANE_COLOR_LEFT, color_bytes);
	const uint32_t right_plane = right ? add_plane(CaptureFormat::PLANE_COLOR_RIGHT, color_bytes) : 0;
	const uint32_t left_depth = has_depth ? add_plane(CaptureFormat::PLANE_DEPTH_LEFT, depth_bytes) : 0;
	const uint32_t right_depth = has_depth && layered && right ? add_plane(CaptureFormat::PLANE_DEPTH_RIGHT, depth_bytes) : 0;
	// The HUD mask, when the GS kept the depth at vsync apart from the snapshot (its depth extras).
	const bool has_hud = has_depth && frame.live_depth_view;
	const uint32_t hud_words = (width + 3) / 4;
	const uint32_t hud_plane = has_hud ? add_plane(CaptureFormat::PLANE_HUD_MASK, VkDeviceSize(hud_words) * 4 * height) : 0;
	if (!EnsureCapacity(*slot, total))
	{
		Console.ErrorFmt("Capture: no memory for a {} MB frame", total >> 20);
		m_dropped++;
		m_dropped_total++;
		return;
	}

	header.index = index;
	header.time_ns = Now();
	header.realtime_ns = m_start_realtime_ns + header.time_ns;
	header.width = width;
	header.height = height;
	header.depth_width = depth_width;
	header.depth_height = depth_height;
	header.dropped_before = std::exchange(m_dropped, 0);
	const float* r = frame.draw_rect;
	const bool has_rect = r[2] > r[0] && r[3] > r[1];
	const float sx = float(width) / frame.width, sy = float(height) / frame.height;
	header.draw_rect[0] = has_rect ? r[0] * sx : 0.0f;
	header.draw_rect[1] = has_rect ? r[1] * sy : 0.0f;
	header.draw_rect[2] = has_rect ? r[2] * sx : float(width);
	header.draw_rect[3] = has_rect ? r[3] * sy : float(height);
	header.mode = view.mode;
	header.convergence = view.convergence;
	header.stereo_depth = view.stereo_depth;
	header.screen_width = view.screen_width;
	header.screen_height = view.screen_height;
	header.screen_distance = view.screen_distance;
	std::memcpy(header.eye, view.eye, sizeof(header.eye));
	std::memcpy(header.distance_ratio, view.distance_ratio, sizeof(header.distance_ratio));

	VkCommandBuffer cmd = slot->cmd;
	vkResetFences(m_device, 1, &slot->fence);
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	const auto bind = [&](VkImageView image_view, VkSampler sampler) {
		const VkDescriptorImageInfo image{sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		const VkDescriptorBufferInfo buffer{slot->buffer, 0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &image;
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].pBufferInfo = &buffer;
		vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 2, writes);
	};
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_color);
	for (const auto& [image_view, plane] : {std::pair{left, left_plane}, std::pair{right, right_plane}})
	{
		if (!image_view)
			continue;
		bind(image_view, m_linear);
		const ColorParams params{{width, height}, static_cast<uint32_t>(slot->offsets[plane] / 4)};
		vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
		vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
	}
	if (has_depth)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layered ? m_depth_layered : m_depth);
		bind(frame.depth_view, m_nearest);
		for (int eye = 0; eye < (right_depth ? 2 : 1); eye++)
		{
			DepthParams params{};
			std::memcpy(params.src_uv, frame.depth_src_uv, sizeof(params.src_uv));
			std::memcpy(params.draw_rect, frame.depth_draw_rect, sizeof(params.draw_rect));
			params.frame_size[0] = float(frame.width);
			params.frame_size[1] = float(frame.height);
			params.size[0] = depth_width;
			params.size[1] = depth_height;
			params.depth_scale = frame.depth_scale;
			params.offset = static_cast<uint32_t>(slot->offsets[eye ? right_depth : left_depth] / 4);
			params.layer = float(eye);
			vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
			vkCmdDispatch(cmd, (depth_width + 7) / 8, (depth_height + 7) / 8, 1);
		}
	}
	if (has_hud)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layered ? m_hud_layered : m_hud);
		bind(frame.live_depth_view, m_nearest);
		HudParams params{};
		std::memcpy(params.src_uv, frame.depth_src_uv, sizeof(params.src_uv));
		std::memcpy(params.draw_rect, frame.depth_draw_rect, sizeof(params.draw_rect));
		params.frame_size[0] = float(frame.width);
		params.frame_size[1] = float(frame.height);
		params.size[0] = width;
		params.size[1] = height;
		params.depth_scale = frame.depth_scale;
		params.offset = static_cast<uint32_t>(slot->offsets[hud_plane] / 4);
		vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
		vkCmdDispatch(cmd, (hud_words + 7) / 8, (height + 7) / 8, 1);
	}
	VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr,
		0, nullptr);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	std::lock_guard lock(GraphicsQueueMutex());
	if (vkQueueSubmit(m_queue, 1, &submit, slot->fence) != VK_SUCCESS)
	{
		Console.Error("Capture: submit failed");
		return;
	}
	slot->state.store(SLOT_GPU, std::memory_order_release);
}

void TrailerCapture::Poll()
{
	if (!m_pool)
		return;
	for (Slot& slot : m_slots)
	{
		if (slot.state.load(std::memory_order_acquire) != SLOT_GPU || vkGetFenceStatus(m_device, slot.fence) != VK_SUCCESS)
			continue;
		if (!m_coherent)
		{
			VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
			range.memory = slot.memory;
			range.size = VK_WHOLE_SIZE;
			vkInvalidateMappedMemoryRanges(m_device, 1, &range);
		}
		slot.state.store(SLOT_WRITING, std::memory_order_release);
		CaptureWriter::Job job;
		job.header = slot.header;
		for (uint32_t i = 0; i < slot.header.plane_count; i++)
			job.data[i] = static_cast<const char*>(slot.mapped) + slot.offsets[i];
		job.done = [&slot] { slot.state.store(SLOT_FREE, std::memory_order_release); };
		m_writer.Submit(std::move(job));
	}
	if (const auto now = std::chrono::steady_clock::now(); now >= m_next_log)
	{
		m_next_log = now + std::chrono::seconds(10);
		if (m_recording)
			Console.WriteLnFmt("Capture: {} frames written ({:.2f} GB), {} dropped, {} waiting to be written",
				m_writer.FramesWritten(), m_writer.BytesWritten() / 1e9, m_dropped_total, m_writer.Backlog());
	}
}

uint32_t TrailerCapture::UiImageId(UiImage& written, const Canvas* canvas, uint64_t version)
{
	if (written.id && written.canvas == canvas && written.version == version)
		return written.id;
	written = {canvas, version, m_next_ui_id++};
	CaptureWriter::Job job;
	char name[32];
	std::snprintf(name, sizeof(name), "ui/%06u.bin", written.id);
	job.image_path = name;
	job.image_width = static_cast<uint32_t>(canvas->Width());
	job.image_height = static_cast<uint32_t>(canvas->Height());
	job.image.assign(canvas->Pixels(), canvas->Pixels() + size_t(canvas->Width()) * size_t(canvas->Height()));
	m_writer.Submit(std::move(job));
	return written.id;
}

void TrailerCapture::RecordState(const State& state, const VrMenu::CaptureView* ui)
{
	if (!Recording())
		return;
	std::string line;
	line.reserve(1024);
	fmt::format_to(std::back_inserter(line), "{{\"t\":{}", Now());
	if (state.head_valid)
	{
		line += ",\"head\":";
		AppendPose(line, state.head);
	}
	if (state.eyes_valid)
	{
		line += ",\"eyes\":[";
		for (int i = 0; i < 2; i++)
		{
			const XrFovf& fov = state.eyes[i].fov;
			line += i ? ",{\"p\":" : "{\"p\":";
			AppendPose(line, state.eyes[i].pose);
			fmt::format_to(std::back_inserter(line), ",\"fov\":[{:.4f},{:.4f},{:.4f},{:.4f}]}}", fov.angleLeft, fov.angleRight,
				fov.angleUp, fov.angleDown);
		}
		line += "]";
	}
	if (state.screen_placed)
	{
		line += ",\"screen\":";
		AppendPose(line, state.screen);
		fmt::format_to(std::back_inserter(line), ",\"sw\":{:.4f},\"sh\":{:.4f}", state.screen_width, state.screen_height);
	}
	fmt::format_to(std::back_inserter(line), ",\"mode\":{},\"pt\":{},\"menu\":{},\"gun\":{}", state.mode,
		int(state.passthrough), int(state.menu_open), int(state.gun_mode));
	if (state.gun_mode)
	{
		line += ",\"guns\":[";
		for (int g = 0; g < 2; g++)
		{
			const GunRenderer::Gun& gun = state.guns[g];
			fmt::format_to(std::back_inserter(line), "{}{{\"v\":{}", g ? "," : "", int(gun.visible));
			if (gun.visible)
			{
				line += ",\"p\":";
				AppendPose(line, gun.pose);
				fmt::format_to(std::back_inserter(line),
					",\"c\":\"{:06X}\",\"tr\":{:.3f},\"bl\":{:.3f},\"br\":{:.3f},\"beam\":{},\"dot\":{},\"end\":[{:.4f},{:.4f},{:.4f}],"
					"\"bc\":\"{:06X}\",\"fire\":{}",
					gun.body_color, gun.trigger, gun.press_left, gun.press_right, int(gun.beam), int(gun.dot), gun.beam_end.x,
					gun.beam_end.y, gun.beam_end.z, gun.beam_color, int(state.fired[g]));
			}
			line += "}";
		}
		line += "]";
	}
	line += ",\"hands\":[";
	for (int h = 0; h < 2; h++)
	{
		fmt::format_to(std::back_inserter(line), "{}{{\"v\":{}", h ? "," : "", int(state.hands[h].visible));
		if (state.hands[h].visible)
		{
			line += ",\"p\":";
			AppendPose(line, state.hands[h].pose);
		}
		line += "}";
	}
	line += "]";
	if (ui && ui->atlas && (ui->panel || !ui->pieces.empty()))
	{
		line += ",\"ui\":{";
		if (ui->panel)
		{
			fmt::format_to(std::back_inserter(line), "\"panel\":{{\"img\":{},\"p\":", UiImageId(m_ui_panel, ui->panel, ui->panel_version));
			AppendPose(line, ui->panel_pose);
			fmt::format_to(std::back_inserter(line), ",\"w\":{:.4f},\"h\":{:.4f}}},", ui->panel_size.width, ui->panel_size.height);
		}
		fmt::format_to(std::back_inserter(line), "\"atlas\":{},\"pieces\":[", UiImageId(m_ui_atlas, ui->atlas, ui->atlas_version));
		for (size_t i = 0; i < ui->pieces.size(); i++)
		{
			const VrMenu::CaptureView::Piece& piece = ui->pieces[i];
			fmt::format_to(std::back_inserter(line), "{}{{\"r\":[{},{},{},{}],\"p\":", i ? "," : "", piece.rect.offset.x,
				piece.rect.offset.y, piece.rect.extent.width, piece.rect.extent.height);
			AppendPose(line, piece.pose);
			fmt::format_to(std::back_inserter(line), ",\"w\":{:.4f},\"h\":{:.4f},\"s\":{}}}", piece.size.width, piece.size.height,
				int(piece.on_screen));
		}
		line += "]}";
	}
	line += "}";
	m_writer.WriteLine(std::move(line));
}

void TrailerCapture::RecordEvent(const char* name, const std::string& value)
{
	if (!Active())
		return;
	std::string escaped;
	for (const char c : value)
	{
		if (c == '"' || c == '\\')
			escaped += '\\';
		if (static_cast<unsigned char>(c) >= 0x20)
			escaped += c;
	}
	m_writer.WriteLine(fmt::format("{{\"t\":{},\"event\":\"{}\",\"value\":\"{}\"}}", Now(), name, escaped));
}

void TrailerCapture::WriteSummary(bool final)
{
	const std::string path = m_options.dir + "/capture.json";
	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (!file)
		return;
	const std::string text = fmt::format(
		"{{\n  \"format\": \"ps2v-trailer-capture\",\n  \"version\": 1,\n  \"started_realtime_ns\": {},\n"
		"  \"complete\": {},\n  \"frames_captured\": {},\n  \"frames_written\": {},\n  \"frames_dropped\": {},\n"
		"  \"bytes\": {},\n  \"max_width\": {},\n  \"stopped_by_size_limit\": {}\n}}\n",
		m_start_realtime_ns, final ? "true" : "false", m_index, m_writer.FramesWritten(), m_dropped_total,
		m_writer.BytesWritten(), m_options.max_width, m_writer.Full() ? "true" : "false");
	std::fwrite(text.data(), 1, text.size(), file);
	std::fclose(file);
}
