// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The trailer capture (--capture DIR): what the headset showed, kept for tools/trailer to render
// camera moves around the screen on a computer. Armed from the start, it records only between the
// record button's clicks (SetRecording), so a take starts where the player wants it: after the light
// gun's calibration, at the scene to show. Each emulator frame's pictures (both eyes in 3D) and
// each eye's depth are copied by compute passes on the shared queue into host-visible slots, read
// once the GPU is done, and handed to CaptureWriter's threads; every headset frame adds a line with
// the head, the eyes, the screen and the guns (CaptureFormat.h has the files). A frame that finds
// no free slot is dropped and counted.

#pragma once

#include "XrApp.h"
#include "capture/CaptureFormat.h"
#include "capture/CaptureWriter.h"
#include "gun/GunRenderer.h"
#include "ui/VrMenu.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

class TrailerCapture
{
public:
	~TrailerCapture();

	struct Options
	{
		std::string dir;
		uint32_t max_width = 0; // pictures wider than this are scaled down (0: as shown)
		double max_gb = 40.0; // the capture stops at this size on disk
		bool record_now = false; // record from the start (scripted takes)
	};
	bool Create(VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index, const Options& options);
	// Writes what is pending and the capture's summary.
	void Destroy();

	// Armed: created, and not stopped by the size limit. The screens and the GS keep what the capture
	// reads (readable pictures, depth) while armed, so recording starts without a pause.
	bool Active() const { return m_pool != VK_NULL_HANDLE && !m_writer.Full(); }
	// Recording: frames and poses are written.
	bool Recording() const { return Active() && m_recording; }
	// Starts or stops recording; returns the seconds just recorded when it stops.
	double SetRecording(bool recording);
	// True once, when the size limit stopped the capture.
	bool TakeStopped();

	// What the GS drew this frame with (CaptureFormat::FrameHeader).
	struct View
	{
		CaptureFormat::Mode mode = CaptureFormat::MODE_FLAT;
		float convergence = 0.12f, stereo_depth = 3.0f;
		float screen_width = 2.4f, screen_height = 1.8f, screen_distance = 2.2f;
		float eye[2][4] = {}; // x, y, distance, depth factor
		float distance_ratio[2] = {1.0f, 1.0f};
	};
	// A frame the host consumed: each eye's picture as shown (right: null in flat), sampled before the
	// screens release them, and its depth. Call on the frame loop's thread.
	void Process(const VKLibretro::Frame& frame, VkImageView left, VkImageView right, const View& view);
	// Hands the frames the GPU has finished to the writers; call every headset frame.
	void Poll();

	// A headset frame's line in poses.jsonl.
	struct State
	{
		bool head_valid = false;
		XrPosef head{};
		bool eyes_valid = false;
		XrView eyes[2]{};
		bool screen_placed = false;
		XrPosef screen{};
		float screen_width = 0.0f, screen_height = 0.0f;
		int mode = 0; // CaptureFormat::Mode
		bool passthrough = false;
		bool menu_open = false;
		bool gun_mode = false;
		std::array<GunRenderer::Gun, 2> guns{};
		std::array<bool, 2> fired{}; // the gun's trigger (or calibration shot) is down
		std::array<GunRenderer::Hand, 2> hands{};
	};
	// ui: what the menus show (their images are written when they change).
	void RecordState(const State& state, const VrMenu::CaptureView* ui = nullptr);
	// An event line: {"t": ..., "event": name, "value": value}.
	void RecordEvent(const char* name, const std::string& value);

private:
	enum SlotState : int
	{
		SLOT_FREE,
		SLOT_GPU,
		SLOT_WRITING,
	};
	struct Slot
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkDeviceSize capacity = 0;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		std::atomic<int> state{SLOT_FREE};
		CaptureFormat::FrameHeader header{};
		std::array<VkDeviceSize, CaptureFormat::MAX_PLANES> offsets{}; // bytes into the buffer
	};

	bool EnsureCapacity(Slot& slot, VkDeviceSize bytes);
	void FreeSlotMemory(Slot& slot);
	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;
	VkPipeline CreatePipeline(const uint32_t* spirv, size_t size, VkPipelineLayout layout);
	int64_t Now() const;
	void WriteSummary(bool final);

	// At 3x both eyes are ~25 MB a frame: LZ4 on one big core takes ~70 ms of it, so five writers keep
	// up with 60 frames a second (three dropped 12% on the Frame).
	static constexpr size_t NUM_SLOTS = 12;
	static constexpr int NUM_WRITERS = 5;

	Options m_options;
	VkDevice m_device = VK_NULL_HANDLE;
	VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	VkSampler m_linear = VK_NULL_HANDLE, m_nearest = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE; // both pipelines'
	VkPipeline m_color = VK_NULL_HANDLE, m_depth = VK_NULL_HANDLE, m_depth_layered = VK_NULL_HANDLE;
	VkPipeline m_hud = VK_NULL_HANDLE, m_hud_layered = VK_NULL_HANDLE;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	uint32_t m_memory_type = UINT32_MAX;
	bool m_coherent = true;
	std::array<Slot, NUM_SLOTS> m_slots;
	size_t m_next = 0;

	CaptureWriter m_writer;
	std::chrono::steady_clock::time_point m_start{};
	int64_t m_start_realtime_ns = 0;
	uint32_t m_index = 0;
	uint32_t m_dropped = 0, m_dropped_total = 0;
	bool m_stopped_told = false;
	bool m_recording = false;
	int64_t m_recording_since = 0;

	// The menu images written so far: which canvas and version each file holds.
	struct UiImage
	{
		const Canvas* canvas = nullptr;
		uint64_t version = 0;
		uint32_t id = 0;
	};
	UiImage m_ui_panel, m_ui_atlas;
	uint32_t m_next_ui_id = 1;
	// The id of canvas's current image, written first if it changed.
	uint32_t UiImageId(UiImage& written, const Canvas* canvas, uint64_t version);
	std::chrono::steady_clock::time_point m_next_log{};
};
