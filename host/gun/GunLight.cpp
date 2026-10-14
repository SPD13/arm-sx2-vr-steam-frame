// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunLight.h"

#include <cstdint>

#include "gun_light.comp.h"

#include "common/Console.h"

#include <cstring>

namespace
{
	// A sample brighter than this (its brightest channel, 0..1) is light the sensor would see ...
	constexpr float LIGHT_THRESHOLD = 0.1f;
	// ... and a picture with fewer lit samples than this share is dark: a black frame, with a small
	// margin (a stray lit pixel). The emulator's messages are not in the picture (VrMenu shows them
	// above the screen).
	constexpr float DARK_SHARE = 0.02f;

	// The push constants of gun_light.comp.
	struct Params
	{
		float rect_uv[4];
		uint32_t grid_size[2];
		float threshold;
	};
	// Its result buffer.
	struct Result
	{
		uint32_t lit;
		uint32_t brightest;
	};
} // namespace

GunLight::~GunLight()
{
	Destroy();
}

uint32_t GunLight::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
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

bool GunLight::Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index)
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

	VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module_info.codeSize = sizeof(gun_light_comp);
	module_info.pCode = gun_light_comp;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS)
		return false;
	VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = m_pipeline_layout;
	vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline);
	vkDestroyShaderModule(device, module, nullptr);
	if (!m_pipeline)
	{
		Console.Error("Light gun: could not create the light sensor pipeline.");
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
		buffer_info.size = sizeof(Result);
		buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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

void GunLight::Destroy()
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
	if (m_pipeline)
		vkDestroyPipeline(m_device, m_pipeline, nullptr);
	if (m_pipeline_layout)
		vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
	if (m_set_layout)
		vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
	if (m_sampler)
		vkDestroySampler(m_device, m_sampler, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_pipeline = VK_NULL_HANDLE;
	m_pipeline_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

void GunLight::Process(const VKLibretro::Frame& frame)
{
	const float* r = frame.draw_rect;
	if (!m_pool || !frame.view || !frame.width || !frame.height)
		return;
	// A slot the GPU is done with; when all are busy, this picture goes without.
	Slot* slot = nullptr;
	for (size_t i = 0; i < NUM_SLOTS && !slot; i++)
	{
		Slot& candidate = m_slots[(m_next + i) % NUM_SLOTS];
		if (vkGetFenceStatus(m_device, candidate.fence) == VK_SUCCESS)
		{
			Latest(); // takes its reading first, if it holds the newest
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
	vkCmdFillBuffer(cmd, slot->buffer, 0, sizeof(Result), 0);
	VkMemoryBarrier cleared{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	cleared.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cleared, 0,
		nullptr, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
	const VkDescriptorImageInfo image{m_sampler, frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
	// The game's picture only, not the border around it.
	const bool has_rect = r[2] > r[0] && r[3] > r[1];
	const float w = static_cast<float>(frame.width), h = static_cast<float>(frame.height);
	Params params{};
	params.rect_uv[0] = has_rect ? r[0] / w : 0.0f;
	params.rect_uv[1] = has_rect ? r[1] / h : 0.0f;
	params.rect_uv[2] = has_rect ? r[2] / w : 1.0f;
	params.rect_uv[3] = has_rect ? r[3] / h : 1.0f;
	params.grid_size[0] = GRID_WIDTH;
	params.grid_size[1] = GRID_HEIGHT;
	params.threshold = LIGHT_THRESHOLD;
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
		Console.Error("Light gun: light sensor submit failed");
		return;
	}
	slot->pending = true;
	slot->time = std::chrono::steady_clock::now();
}

const GunLight::Reading& GunLight::Latest()
{
	// The newest finished slot replaces the reading.
	Slot* newest = nullptr;
	for (Slot& slot : m_slots)
	{
		if (slot.pending && vkGetFenceStatus(m_device, slot.fence) == VK_SUCCESS && (!newest || slot.time > newest->time))
			newest = &slot;
	}
	if (newest && newest->time > m_latest.time)
	{
		Result result;
		std::memcpy(&result, newest->mapped, sizeof(result));
		float brightest;
		std::memcpy(&brightest, &result.brightest, sizeof(brightest));
		m_latest.valid = true;
		m_latest.lit_share = static_cast<float>(result.lit) / static_cast<float>(GRID_WIDTH * GRID_HEIGHT);
		m_latest.brightest = brightest;
		m_latest.dark = m_latest.lit_share < DARK_SHARE;
		m_latest.time = newest->time;
	}
	for (Slot& slot : m_slots)
	{
		if (slot.pending && vkGetFenceStatus(m_device, slot.fence) == VK_SUCCESS && slot.time <= m_latest.time)
			slot.pending = false;
	}
	return m_latest;
}
