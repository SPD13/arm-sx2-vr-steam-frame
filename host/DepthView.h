// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// Depth processing for the volumetric window. For each emulator frame that carries depth
// (VKLibretro::Frame::depth_*), compute passes on the shared queue sample the presented region on a
// grid at its aspect ratio: range, coverage and HUD share (stats), a histogram, the 2nd..98th
// percentile range eased over frames, then the normalised depth grid the window renders from and
// a colour-mapped debug image of it for a ScreenLayer panel. A frame can be dumped to PNG (debug
// image plus the matching crop of the colour frame) to check alignment off-headset.

#pragma once

#include "XrApp.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include <array>
#include <optional>
#include <string>

class DepthView
{
public:
	~DepthView();

	bool Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	// Runs the passes for a frame with depth and returns the debug image (RGBA8, in
	// SHADER_READ_ONLY_OPTIMAL) as a frame for ScreenLayer. False when the frame has no depth; after
	// half a second of those (a menu, a movie) the depth range starts over.
	bool Process(const VKLibretro::Frame& frame, VKLibretro::Frame* view);

	struct Stats
	{
		float zmin = 0.0f, zmax = 0.0f; // 0..1 of the Z range, valid cells only
		float lo = 0.0f, hi = 0.0f; // the smoothed range mapped onto 0..1
		float coverage = 0.0f; // fraction of the presented region with depth
		float hud = 0.0f; // fraction drawn after the 3D (HUD, overlays)
	};
	// From the most recent frame whose passes have completed.
	std::optional<Stats> LastStats() const { return m_last_stats; }

	// Writes <prefix>-depth.png and <prefix>-colour.png from the next processed frame.
	void RequestDump(std::string prefix) { m_dump_prefix = std::move(prefix); }

	// The normalised depth grid (R32F, VK_IMAGE_LAYOUT_GENERAL): one texel per grid cell over the
	// presented region, 1 at the screen plane (nearest, HUD) and 0 farthest. Valid once Process
	// has succeeded; later Process calls on the same queue rewrite it in place.
	VkImageView NormalizedView() const { return m_normalized_view; }
	// Same layout and size (RGBA32F): the grid eroded toward far, for the layer behind the scene,
	// with the offset in cells to the far cell it came from in g and b.
	VkImageView BackgroundView() const { return m_background_view; }
	uint32_t GridWidth() const { return m_output_width; }
	uint32_t GridHeight() const { return m_output_height; }

private:
	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkDeviceSize size = 0;
	};

	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer);
	void DestroyBuffer(Buffer* buffer);
	bool EnsureOutput(uint32_t width, uint32_t height);
	void DestroyOutput();
	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;
	VkPipeline CreatePipeline(const uint32_t* spirv, size_t size);
	void WriteDump(const Buffer& depth, uint32_t dw, uint32_t dh, const Buffer& colour, uint32_t cw, uint32_t ch);

	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;

	VkSampler m_sampler = VK_NULL_HANDLE; // nearest, for the depth
	VkSampler m_linear_sampler = VK_NULL_HANDLE; // for the colour comparison
	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_stats_pipeline = VK_NULL_HANDLE;
	VkPipeline m_hist_pipeline = VK_NULL_HANDLE;
	VkPipeline m_range_pipeline = VK_NULL_HANDLE;
	VkPipeline m_normalize_pipeline = VK_NULL_HANDLE;
	VkPipeline m_background_pipeline = VK_NULL_HANDLE;
	VkPipeline m_hud_pipeline = VK_NULL_HANDLE;
	VkPipeline m_view_pipeline = VK_NULL_HANDLE;

	static constexpr size_t NUM_SLOTS = 3;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, NUM_SLOTS> m_cmd{};
	std::array<VkFence, NUM_SLOTS> m_fences{};
	std::array<Buffer, NUM_SLOTS> m_stats{};
	std::array<Buffer, NUM_SLOTS> m_histogram{};
	Buffer m_range; // persists across frames: the eased range
	std::array<bool, NUM_SLOTS> m_stats_pending{};
	size_t m_slot = 0;

	VkImage m_output = VK_NULL_HANDLE;
	VkDeviceMemory m_output_memory = VK_NULL_HANDLE;
	VkImageView m_output_view = VK_NULL_HANDLE;
	uint32_t m_output_width = 0, m_output_height = 0;
	VkImageLayout m_output_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage m_normalized = VK_NULL_HANDLE;
	VkDeviceMemory m_normalized_memory = VK_NULL_HANDLE;
	VkImageView m_normalized_view = VK_NULL_HANDLE;
	VkImage m_background = VK_NULL_HANDLE;
	VkDeviceMemory m_background_memory = VK_NULL_HANDLE;
	VkImageView m_background_view = VK_NULL_HANDLE;
	VkImage m_hud_history = VK_NULL_HANDLE;
	VkDeviceMemory m_hud_history_memory = VK_NULL_HANDLE;
	VkImageView m_hud_history_view = VK_NULL_HANDLE;
	bool m_normalized_initialized = false;

	std::optional<Stats> m_last_stats;
	uint32_t m_frames_without_depth = 0;
	std::string m_dump_prefix;
};
