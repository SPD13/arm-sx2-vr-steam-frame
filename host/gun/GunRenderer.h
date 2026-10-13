// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light guns in the headset: the model in each gun hand, with its trigger and side buttons
// moving as they are pressed, the beam from its muzzle and the dot where it lands. Drawn per eye,
// from the eye through a quad that covers the guns and beams (GunView), shown as alpha-blended quad
// layers. Quad layers, not a projection layer: SteamVR puts quad layers (the screen, the menus) over
// projection layers whatever their order, and the guns must be over everything.
//
// Each eye has two quads (panes). The one showing the guns never moves; when the guns near its edges,
// the other pane is fitted while it is still empty, and shows the guns from the next frame on. A pane
// is moved only after a frame empty at the same place. So SteamVR, which applies a quad's new pose a
// frame late, never shows a picture on a quad still at its old place.

#pragma once

#include "XrApp.h"
#include "gun/GunModel.h"
#include "gun/GunView.h"

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
	static constexpr int NUM_LAYERS = 4;
	// Its layers (two per eye) for xrEndFrame, in slots[0] to slots[NUM_LAYERS - 1]; null when not in use.
	void Render(XrTime display_time, const std::array<Gun, 2>& guns, const XrCompositionLayerBaseHeader** slots);

	// The muzzle's pose, LOCAL space, for a gun at `pose`.
	XrPosef MuzzlePose(const XrPosef& pose) const;

private:
	struct Pane
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		uint32_t width = 0, height = 0; // the swapchain's; each frame draws in its top-left part
		GunView view;
		int64_t shown = -2; // the last frame it showed the guns
		std::vector<VkImageView> views;
		std::vector<VkFramebuffer> framebuffers;
		VkImage depth = VK_NULL_HANDLE;
		VkDeviceMemory depth_memory = VK_NULL_HANDLE;
		VkImageView depth_view = VK_NULL_HANDLE;
	};

	bool CreatePane(Pane& pane, const XrViewConfigurationView& config);
	void DestroyPane(Pane& pane);
	void ResetPanes();
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
	// Pane eye + 2 * k is eye's k-th, in the layer list in this order.
	std::array<Pane, NUM_LAYERS> m_panes;
	std::array<int, 2> m_showing{-1, -1}; // each eye's pane with the guns
	std::array<int, 2> m_fitted{-1, -1}; // each eye's pane fitted this frame, to show them from the next
	int64_t m_frame = 0;
	// For the log: panes fitted, and frames a pane was wanted but the other was not free yet.
	uint32_t m_fits = 0, m_waits = 0;
	int64_t m_next_log = 0;
	uint32_t m_logs = 0;
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

	std::array<XrCompositionLayerQuad, NUM_LAYERS> m_layers{};
};
