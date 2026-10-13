// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunDepth.h"

#include <cstdint>

#include "gun_depth.comp.h"
#include "gun_depth_layered.comp.h"

#include "common/Console.h"

#include <cstring>

namespace
{
	// The push constants of gun_depth.glsl.
	struct Params
	{
		float src_uv[4];
		float draw_rect[4];
		float frame_size[2];
		uint32_t grid_size[2];
		float depth_scale;
	};
} // namespace

GunDepth::~GunDepth()
{
	Destroy();
}

uint32_t GunDepth::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
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

VkPipeline GunDepth::CreatePipeline(const uint32_t* spirv, size_t size)
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

bool GunDepth::Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index)
{
	m_device = device;
	m_gpu = gpu;
	m_queue = queue;
	if (!vkCmdPushDescriptorSetKHR)
	{
		Console.Error("Light gun: VK_KHR_push_descriptor is not available.");
		return false;
	}

	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(device, &sampler, nullptr, &m_sampler) != VK_SUCCESS)
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
	const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
	VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &m_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(device, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS)
		return false;
	m_pipeline = CreatePipeline(gun_depth_comp, sizeof(gun_depth_comp));
	m_layered_pipeline = CreatePipeline(gun_depth_layered_comp, sizeof(gun_depth_layered_comp));
	if (!m_pipeline || !m_layered_pipeline)
	{
		Console.Error("Light gun: could not create the depth pipelines.");
		return false;
	}

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index;
	if (vkCreateCommandPool(device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
		return false;
	for (Slot& slot : m_slots)
	{
		VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		alloc.commandPool = m_pool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 1;
		VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer_info.size = VkDeviceSize(GRID_WIDTH) * GRID_HEIGHT * sizeof(float);
		buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (vkAllocateCommandBuffers(device, &alloc, &slot.cmd) != VK_SUCCESS ||
			vkCreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS ||
			vkCreateBuffer(device, &buffer_info, nullptr, &slot.buffer) != VK_SUCCESS)
			return false;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, slot.buffer, &req);
		VkMemoryAllocateInfo mem{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mem.allocationSize = req.size;
		mem.memoryTypeIndex =
			FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (mem.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mem, nullptr, &slot.memory) != VK_SUCCESS)
			return false;
		vkBindBufferMemory(device, slot.buffer, slot.memory, 0);
		vkMapMemory(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped);
	}
	return true;
}

void GunDepth::Destroy()
{
	if (!m_device)
		return;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
	}
	for (Slot& slot : m_slots)
	{
		if (slot.fence)
			vkDestroyFence(m_device, slot.fence, nullptr);
		if (slot.buffer)
			vkDestroyBuffer(m_device, slot.buffer, nullptr);
		if (slot.memory)
			vkFreeMemory(m_device, slot.memory, nullptr);
		slot = {};
	}
	if (m_pool)
		vkDestroyCommandPool(m_device, m_pool, nullptr);
	for (VkPipeline pipeline : {m_pipeline, m_layered_pipeline})
	{
		if (pipeline)
			vkDestroyPipeline(m_device, pipeline, nullptr);
	}
	if (m_pipeline_layout)
		vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
	if (m_set_layout)
		vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
	if (m_sampler)
		vkDestroySampler(m_device, m_sampler, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_pipeline = m_layered_pipeline = VK_NULL_HANDLE;
	m_pipeline_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

void GunDepth::Process(const VKLibretro::Frame& frame, bool layered)
{
	if (!m_pool || !frame.depth_view || !frame.width || !frame.height || frame.depth_scale <= 0.0f ||
		frame.depth_draw_rect[2] <= frame.depth_draw_rect[0] || frame.depth_draw_rect[3] <= frame.depth_draw_rect[1])
		return;
	// A slot the GPU is done with; when all are busy, this frame goes without.
	Slot* slot = nullptr;
	for (size_t i = 0; i < NUM_SLOTS && !slot; i++)
	{
		Slot& candidate = m_slots[(m_next + i) % NUM_SLOTS];
		if (vkGetFenceStatus(m_device, candidate.fence) == VK_SUCCESS)
		{
			Latest(); // takes its grid first, if it holds the newest
			slot = &candidate;
			m_next = (m_next + i + 1) % NUM_SLOTS;
		}
	}
	if (!slot)
		return;
	vkResetFences(m_device, 1, &slot->fence);

	VkCommandBuffer cmd = slot->cmd;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layered ? m_layered_pipeline : m_pipeline);
	const VkDescriptorImageInfo image{m_sampler, frame.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
	vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 2, writes);
	Params params{};
	std::memcpy(params.src_uv, frame.depth_src_uv, sizeof(params.src_uv));
	std::memcpy(params.draw_rect, frame.depth_draw_rect, sizeof(params.draw_rect));
	params.frame_size[0] = static_cast<float>(frame.width);
	params.frame_size[1] = static_cast<float>(frame.height);
	params.grid_size[0] = GRID_WIDTH;
	params.grid_size[1] = GRID_HEIGHT;
	params.depth_scale = frame.depth_scale;
	vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
	vkCmdDispatch(cmd, (GRID_WIDTH + 7) / 8, (GRID_HEIGHT + 7) / 8, 1);
	// The host reads the buffer once the fence signals.
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
		Console.Error("Light gun: depth pass submit failed");
		return;
	}
	slot->pending = true;
	slot->time = std::chrono::steady_clock::now();
}

const GunDepthGrid* GunDepth::Latest()
{
	// The newest finished slot replaces the grid.
	Slot* newest = nullptr;
	for (Slot& slot : m_slots)
	{
		if (slot.pending && vkGetFenceStatus(m_device, slot.fence) == VK_SUCCESS && (!newest || slot.time > newest->time))
			newest = &slot;
	}
	if (newest && newest->time > m_latest.time)
	{
		m_latest.width = GRID_WIDTH;
		m_latest.height = GRID_HEIGHT;
		m_latest.znorm.resize(size_t(GRID_WIDTH) * GRID_HEIGHT);
		std::memcpy(m_latest.znorm.data(), newest->mapped, m_latest.znorm.size() * sizeof(float));
		m_latest.time = newest->time;
	}
	for (Slot& slot : m_slots)
	{
		if (slot.pending && vkGetFenceStatus(m_device, slot.fence) == VK_SUCCESS && slot.time <= m_latest.time)
			slot.pending = false;
	}
	return m_latest.width ? &m_latest : nullptr;
}
