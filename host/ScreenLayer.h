// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// M1 flat screen: each frame the GS publishes (VKLibretro::PublishFrame, RGBA8 in
// SHADER_READ_ONLY_OPTIMAL) is copied into an OpenXR swapchain and shown as a quad layer, so the
// runtime's compositor does the filtering and reprojection. The volumetric window replaces this.

#pragma once

#include "XrApp.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include <array>

class ScreenLayer
{
public:
	~ScreenLayer();

	bool Create(XrApp& xr, VkDevice device, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	// Copies a newly published emulator frame into the swapchain.
	void Update(const VKLibretro::Frame& frame);

	bool HasImage() const { return m_has_image; }

	// Screen placement in LOCAL space; the quad faces +Z of the pose.
	void SetPose(const XrPosef& pose) { m_pose = pose; }
	void SetWidth(float meters) { m_width = meters; }
	// Which eyes see the layer (a stereo pair is two layers, one per eye).
	void SetEyeVisibility(XrEyeVisibility eyes) { m_eyes = eyes; }

	const XrCompositionLayerBaseHeader* Layer();

private:
	bool CreateSwapchain(uint32_t width, uint32_t height);
	void DestroySwapchain();

	XrApp* m_xr = nullptr;
	VkDevice m_device = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	int64_t m_format = 0;
	bool m_srgb = false;

	XrSwapchain m_swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageVulkanKHR> m_images;
	uint32_t m_width_px = 0, m_height_px = 0;
	bool m_has_image = false;

	static constexpr size_t NUM_CMD = 3;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, NUM_CMD> m_cmd{};
	std::array<VkFence, NUM_CMD> m_fences{};
	size_t m_cmd_index = 0;

	XrPosef m_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	float m_width = 2.4f;
	XrEyeVisibility m_eyes = XR_EYE_VISIBILITY_BOTH;
	XrCompositionLayerQuad m_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
