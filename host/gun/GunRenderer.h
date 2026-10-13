// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light guns in the headset: the model in each gun hand, with its trigger and side buttons
// moving as they are pressed, the beam from its muzzle and the dot where it lands. Drawn per eye
// into an alpha-blended OpenXR projection layer of its own, which goes on top of the other layers
// (the guns are nearer than anything else). Smaller than the eye's recommended size: it only holds
// the guns and beams.

#pragma once

#include "XrApp.h"
#include "gun/GunModel.h"

#include <array>
#include <string>
#include <vector>

class GunRenderer
{
public:
	~GunRenderer();

	// Loads the model from model_path and creates the layer; false if either fails.
	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index,
		const std::string& model_path);
	void Destroy();
	bool IsCreated() const { return m_created; }

	const GunModel& Model() const { return m_model; }

	struct Gun
	{
		bool visible = false;
		XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // the model's origin (grip centre), LOCAL space
		uint32_t body_color = 0x8C8C91; // sRGB 0xRRGGBB
		float trigger = 0.0f; // how far it is pulled, 0..1
		float press_left = 0.0f, press_right = 0.0f; // the side buttons, 0..1
		bool beam = false; // from the muzzle to beam_end
		bool dot = false; // at beam_end
		XrVector3f beam_end{};
		uint32_t beam_color = 0xFF3030; // sRGB
	};
	// Returns the layer for xrEndFrame, or null when no gun is visible.
	const XrCompositionLayerBaseHeader* Render(XrTime display_time, const std::array<Gun, 2>& guns);

	// The muzzle's pose, LOCAL space, for a gun at `pose`.
	XrPosef MuzzlePose(const XrPosef& pose) const;

private:
	struct Eye
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		uint32_t width = 0, height = 0;
		std::vector<VkImageView> views;
		std::vector<VkFramebuffer> framebuffers;
		VkImage depth = VK_NULL_HANDLE;
		VkDeviceMemory depth_memory = VK_NULL_HANDLE;
		VkImageView depth_view = VK_NULL_HANDLE;
	};

	bool CreateEye(Eye& eye, const XrViewConfigurationView& config);
	void DestroyEye(Eye& eye);
	bool CreatePipelines();
	bool CreateMeshBuffer();
	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

	XrApp* m_xr = nullptr;
	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	int64_t m_format = 0;
	bool m_created = false;
	static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

	GunModel m_model;
	std::array<Eye, 2> m_eyes;
	VkRenderPass m_render_pass = VK_NULL_HANDLE;
	VkPipelineLayout m_mesh_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_beam_layout = VK_NULL_HANDLE;
	VkPipeline m_mesh_pipeline = VK_NULL_HANDLE;
	VkPipeline m_beam_pipeline = VK_NULL_HANDLE; // depth tested against the guns
	VkPipeline m_dot_pipeline = VK_NULL_HANDLE; // on top

	// The model's vertices then its indices, in one host-visible buffer.
	VkBuffer m_mesh = VK_NULL_HANDLE;
	VkDeviceMemory m_mesh_memory = VK_NULL_HANDLE;
	VkDeviceSize m_index_offset = 0;

	static constexpr size_t NUM_SLOTS = 3;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, NUM_SLOTS> m_cmd{};
	std::array<VkFence, NUM_SLOTS> m_fences{};
	size_t m_slot = 0;

	std::array<XrCompositionLayerProjectionView, 2> m_layer_views{};
	XrCompositionLayerProjection m_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};
