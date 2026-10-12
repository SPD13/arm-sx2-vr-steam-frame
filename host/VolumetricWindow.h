// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// M3 volumetric window: the game rendered per eye into an OpenXR projection layer as a grid mesh
// over the virtual screen, each vertex pushed back from the screen plane by DepthView's normalised
// depth, so the picture has real parallax: a window into a box behind the screen. The scene is
// drawn from the pre-HUD colour, over a background layer that fills depth edges, and the HUD is
// laid flat on the screen plane on top (see window_common.glsl).

#pragma once

#include "XrApp.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include <array>
#include <string>
#include <vector>

class VolumetricWindow
{
public:
	struct Settings
	{
		float width = 2.4f; // screen width, meters
		float depth_range = 0.3f; // how far behind the screen the farthest depth goes, meters
		float pop_out = 0.0f; // share of the range that comes out in front of the screen
		// Lay the HUD flat on the screen plane over the scene, which shows the pre-HUD colour under
		// it. HUD pixels: at the maximum depth and steadily so (with a live depth), or else changed
		// since the depth was taken.
		bool hud_overlay = true;
		// Fill depth edges with a background layer; off, triangles just stretch across them.
		bool edge_fill = false;
		float viewer_distance = 2.2f; // where the fixed inspection cameras stand in front of the screen
		// The game profile's HUD areas (x0, y0, x1, y1 fractions of the presented picture), at most 4.
		std::vector<std::array<float, 4>> hud_rects;
		std::vector<bool> hud_rect_flat; // per HUD area: lay all of it flat
	};

	~VolumetricWindow();

	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	// A new emulator frame; its presented region is copied during the next Render. has_depth says
	// whether the depth grid passed to Render belongs to it (otherwise the window stays flat).
	void SetFrame(const VKLibretro::Frame& frame, bool has_depth);
	bool HasFrame() const { return m_colour.image != VK_NULL_HANDLE || m_has_pending; }

	void SetScreen(const XrPosef& pose, const Settings& settings);
	void SetSettings(const Settings& settings) { m_settings = settings; }

	// Writes <prefix>.png, both eyes side by side as rendered, after the next Render.
	void RequestEyeDump(std::string prefix) { m_eye_dump_prefix = std::move(prefix); }

	// Inspection mode: each Render also draws the window from two fixed cameras, eyes 64 mm apart
	// standing viewer_distance in front of the screen, into images of their own, to show on flat
	// panels: each eye's picture, unaffected by head movement.
	void SetCaptureEyes(bool capture) { m_capture_eyes = capture; }
	// The inspection images as frames for ScreenLayer (swapchain format, SHADER_READ_ONLY_OPTIMAL).
	bool GetEyeImage(int eye, VKLibretro::Frame* frame) const;

	// Renders both eyes; returns the projection layer for xrEndFrame, or null when there is
	// nothing to show yet. depth_grid and background_grid are DepthView's grids (GENERAL layout).
	const XrCompositionLayerBaseHeader* Render(XrTime display_time, VkImageView depth_grid, VkImageView background_grid,
		uint32_t grid_width, uint32_t grid_height);

private:
	struct Eye
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		uint32_t width = 0, height = 0;
		std::vector<VkImage> images;
		std::vector<VkImageView> views;
		std::vector<VkFramebuffer> framebuffers;
		VkImage depth = VK_NULL_HANDLE;
		VkDeviceMemory depth_memory = VK_NULL_HANDLE;
		VkImageView depth_view = VK_NULL_HANDLE;
	};

	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
	};

	bool CreateEye(Eye& eye, const XrViewConfigurationView& config);
	void DestroyEye(Eye& eye);
	bool CreatePipeline();
	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer);
	void DestroyBuffer(Buffer* buffer);
	bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
		VkImageAspectFlags aspect, VkImage* image, VkDeviceMemory* memory, VkImageView* view);
	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint32_t width = 0, height = 0;
	};
	// An image the emulator's bits are copied into: sRGB for its RGBA8 colour, so sampling decodes
	// it to linear; R32F for depth.
	bool EnsureImage(Image& image, VkFormat format, uint32_t width, uint32_t height);
	void DestroyImage(Image& image);
	void RecordCopy(VkCommandBuffer cmd, VkImage src, const Image& dst);
	bool EnsureIndices(uint32_t grid_width, uint32_t grid_height);
	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

	XrApp* m_xr = nullptr;
	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	int64_t m_format = 0;
	static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

	std::array<Eye, 2> m_eyes;
	VkRenderPass m_render_pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE; // background and scene: depth tested
	VkPipeline m_overlay_pipeline = VK_NULL_HANDLE; // HUD: always on top
	VkSampler m_nearest = VK_NULL_HANDLE;
	VkSampler m_linear = VK_NULL_HANDLE;

	static constexpr size_t NUM_SLOTS = 3;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, NUM_SLOTS> m_cmd{};
	std::array<VkFence, NUM_SLOTS> m_fences{};
	std::array<Buffer, NUM_SLOTS> m_uniforms{};
	size_t m_slot = 0;

	Buffer m_indices;
	uint32_t m_index_count = 0;
	uint32_t m_grid_width = 0, m_grid_height = 0;

	// Copies of the latest emulator frame and of its pre-HUD colour.
	Image m_colour;
	Image m_scene;
	Image m_live_depth;
	VKLibretro::Frame m_pending{}; // copied at the next Render
	bool m_has_pending = false;
	bool m_has_depth = false;
	bool m_has_scene = false;
	bool m_has_live_depth = false;
	uint32_t m_frames_without_hud_data = 0; // how long the last pre-HUD colour and live depth are reused
	float m_depth_scale = 0.0f;
	float m_colour_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	float m_scene_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	float m_aspect = 4.0f / 3.0f; // presented height / width is 1 / this

	XrPosef m_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	Settings m_settings;

	std::array<XrCompositionLayerProjectionView, 2> m_layer_views{};
	std::string m_eye_dump_prefix;
	bool m_capture_eyes = false;
	struct InspectionTarget
	{
		Image colour;
		VkImage depth = VK_NULL_HANDLE;
		VkDeviceMemory depth_memory = VK_NULL_HANDLE;
		VkImageView depth_view = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
	};
	std::array<InspectionTarget, 2> m_inspection;
	std::array<Buffer, NUM_SLOTS> m_inspection_uniforms{};
	static constexpr uint32_t INSPECTION_WIDTH = 1024, INSPECTION_HEIGHT = 768;
	bool EnsureInspectionTargets();
	void DestroyInspectionTargets();
	XrCompositionLayerProjection m_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};
