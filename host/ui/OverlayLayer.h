// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A Canvas shown in the headset as an alpha-blended OpenXR quad layer: the menu, its gear button,
// the pointer's dot and beam. Each Upload copies the canvas through a staging buffer into the next
// swapchain image; between uploads the compositor keeps showing the last one.

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
	bool HasImage() const { return m_has_image; }

	// Placement in LOCAL space; the quad faces +Z of the pose.
	void SetPose(const XrPosef& pose) { m_pose = pose; }
	void SetSize(float width, float height) { m_size = {width, height}; }
	const XrPosef& Pose() const { return m_pose; }
	XrExtent2Df Size() const { return m_size; }

	const XrCompositionLayerBaseHeader* Layer();

private:
	struct Staging
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
	};

	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

	XrApp* m_xr = nullptr;
	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;

	XrSwapchain m_swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageVulkanKHR> m_images;
	int m_width = 0, m_height = 0;
	bool m_has_image = false;

	static constexpr size_t NUM_SLOTS = 2;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, NUM_SLOTS> m_cmd{};
	std::array<VkFence, NUM_SLOTS> m_fences{};
	std::array<Staging, NUM_SLOTS> m_staging{};
	size_t m_slot = 0;

	XrPosef m_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	XrExtent2Df m_size{1.0f, 1.0f};
	XrCompositionLayerQuad m_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
