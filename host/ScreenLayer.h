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

	// The present copy (VKLibretro::ArmPresentCopy): rather than copying each frame here, behind
	// whatever the GS queued since, the next image is acquired ahead and handed to the GS, which
	// copies the frame into it at the end of its present. Arm acquires and hands over an image for
	// the eye (0 left, 1 right) once the swapchain fits the frames; ReleaseArmed releases it when
	// the consumed frame was copied into it (its Frame::copied_left/right id) and returns whether it
	// was. Otherwise Update copies the frame into that same image.
	void Arm(int eye);
	bool ReleaseArmed(uint64_t copied_id);

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

	// The present copy's image: acquired (m_armed), and handed to the GS for m_armed_eye.
	bool AcquireArmed();
	void DisarmGiven();
	bool m_armed = false;
	bool m_armed_given = false;
	int m_armed_eye = 0;
	uint32_t m_armed_index = 0;
	uint64_t m_armed_id = 0;
	static inline uint64_t s_next_armed_id = 1;

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
