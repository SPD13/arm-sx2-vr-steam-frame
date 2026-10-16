// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A Canvas shown in the headset as an alpha-blended OpenXR quad layer: the menu panel, or the sheet
// around the screen with the pointer's dot and beam. Each Upload copies the canvas through a staging
// buffer into the next swapchain image; each Compose builds the next image on the GPU from parts of a
// source image instead. Between them the compositor keeps showing the last one.

#pragma once

#include "XrApp.h"
#include "ui/Canvas.h"

#include <array>
#include <vector>

class OverlayLayer
{
public:
	~OverlayLayer();

	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index,
		int width, int height);
	void Destroy();

	// canvas must have the size the layer was created with.
	void Upload(const Canvas& canvas);

	// A part of the source image scaled into the layer's image (linear filtering, 1:1 when the sizes
	// match). The rest of the image is transparent.
	struct Blit
	{
		XrRect2Di from; // pixels of the source
		XrRect2Di to; // pixels of the layer's image
	};
	// Builds the next image from blits of the source: an image the size of source, uploaded from it
	// when given (null: unchanged since the last Compose).
	void Compose(const Canvas* source, const std::vector<Blit>& blits);
	bool HasImage() const { return m_has_image; }

	// Placement in LOCAL space; the quad faces +Z of the pose.
	void SetPose(const XrPosef& pose) { m_pose = pose; }
	void SetSize(float width, float height) { m_size = {width, height}; }
	const XrPosef& Pose() const { return m_pose; }
	XrExtent2Df Size() const { return m_size; }

	const XrCompositionLayerBaseHeader* Layer();

	// Part of the image shown as a quad of its own, so that several layers share one swapchain
	// (SteamVR on the Frame allows 16 swapchains). Placed like the whole layer, in LOCAL space.
	struct Part
	{
		XrRect2Di rect{}; // pixels of the image
		XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
		XrExtent2Df size{1.0f, 1.0f};
		XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	};
	const XrCompositionLayerBaseHeader* Layer(Part& part);

private:
	struct Staging
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
	};

	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;
	// Staging buffers of at least size bytes, made on first use: the panel needs its own size, the
	// sheet only its source's.
	bool EnsureStaging(VkDeviceSize size);
	bool CreateSource(int width, int height);
	void DestroySource();
	// Waits for the next slot's last submission, acquires the next image, starts the slot's command
	// buffer; false if the image cannot be had.
	bool Begin(size_t* slot, uint32_t* index);
	// Puts the image in the layout OpenXR expects, submits and releases it.
	void End(size_t slot, VkImage image);

	XrApp* m_xr = nullptr;
	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;

	XrSwapchain m_swapchain = XR_NULL_HANDLE;
	VkFormat m_format = VK_FORMAT_UNDEFINED;
	std::vector<XrSwapchainImageVulkanKHR> m_images;
	int m_width = 0, m_height = 0;
	bool m_has_image = false;

	static constexpr size_t NUM_SLOTS = 2;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, NUM_SLOTS> m_cmd{};
	std::array<VkFence, NUM_SLOTS> m_fences{};
	std::array<Staging, NUM_SLOTS> m_staging{};
	VkDeviceSize m_staging_size = 0;
	size_t m_slot = 0;

	// Compose's source.
	VkImage m_source = VK_NULL_HANDLE;
	VkDeviceMemory m_source_memory = VK_NULL_HANDLE;
	int m_source_width = 0, m_source_height = 0;
	bool m_source_valid = false; // uploaded once: in TRANSFER_SRC_OPTIMAL

	XrPosef m_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	XrExtent2Df m_size{1.0f, 1.0f};
	XrCompositionLayerQuad m_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
