// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light gun's light sensor. A real GunCon 2 reads the CRT: it sees the picture where it aims, and
// reports "off the screen" (0, 0) when there is no light. Games rely on that: Time Crisis calibrates
// by showing a black frame after the calibration shot and waiting for the gun to see nothing. The
// emulated gun aims without seeing, so the host tells it, for each presented picture, whether the
// picture is dark: a compute pass samples it on a coarse grid (gun_light.comp) into host-visible
// memory, read back once the GPU is done, a frame or so later.

#pragma once

#include "XrApp.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include <array>
#include <chrono>

class GunLight
{
public:
	~GunLight();

	bool Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	// Samples a presented picture (call after the screen has copied it, on the same queue).
	void Process(const VKLibretro::Frame& frame);

	// The newest picture the GPU has finished: whether it is dark (almost nothing lit), the share of
	// its samples that are lit, and its brightest value (0..1).
	struct Reading
	{
		bool valid = false;
		bool dark = false;
		float lit_share = 1.0f;
		float brightest = 1.0f;
		std::chrono::steady_clock::time_point time{};
	};
	const Reading& Latest();

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

	static constexpr uint32_t GRID_WIDTH = 96, GRID_HEIGHT = 72;
	static constexpr size_t NUM_SLOTS = 3;

	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<Slot, NUM_SLOTS> m_slots{};
	size_t m_next = 0;
	Reading m_latest;
};
