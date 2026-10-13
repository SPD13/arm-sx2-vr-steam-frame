// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The depth the light gun aims with (GunAim): for each emulator frame with depth, a compute pass on
// the shared queue samples it on a coarse grid over the presented frame (gun_depth.glsl) into
// host-visible memory, read back once the GPU is done, a frame or so later.

#pragma once

#include "XrApp.h"
#include "gun/GunAim.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include <array>

class GunDepth
{
public:
	~GunDepth();

	bool Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	// Samples a frame's depth, when it has some (layered: a layer per eye, the left one is read).
	void Process(const VKLibretro::Frame& frame, bool layered);

	// The newest grid the GPU has finished, if any.
	const GunDepthGrid* Latest();

private:
	struct Slot
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool pending = false;
		std::chrono::steady_clock::time_point time{};
	};

	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;
	VkPipeline CreatePipeline(const uint32_t* spirv, size_t size);

	static constexpr uint32_t GRID_WIDTH = 160, GRID_HEIGHT = 120;
	static constexpr size_t NUM_SLOTS = 3;

	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE;
	VkPipeline m_layered_pipeline = VK_NULL_HANDLE;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<Slot, NUM_SLOTS> m_slots{};
	size_t m_next = 0;
	GunDepthGrid m_latest;
};
