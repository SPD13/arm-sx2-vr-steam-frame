// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The performance window left of the screen (Settings > Graphics > Performance window): the game's
// frame rate and the headset's, the emulator threads' CPU use and its GPU use, each as a 2-second
// average and as a graph of the last 3 minutes. The emulator measures every 0.5 s of emulation
// (Emu::GetPerfMetrics); a sample is taken every 0.5 s, and while the game is paused or not running
// its values are left out of the graph.

#pragma once

#include "XrApp.h"
#include "ui/Canvas.h"
#include "ui/OverlayLayer.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

class PerfWindow
{
public:
	static constexpr int WIDTH_PX = 600;
	static constexpr int HEIGHT_PX = 780;

	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index, Font* text,
		float meters_per_px);
	void Destroy();

	// Every displayed frame: counts the headset's frame rate, takes a sample every 0.5 s and, when
	// shown, redraws with it.
	void Update(bool shown);
	// Placement in LOCAL space; the quad faces +Z of the pose.
	void SetPose(const XrPosef& pose) { m_layer.SetPose(pose); }
	XrExtent2Df Size() const { return m_layer.Size(); }
	// Null until it has drawn once.
	const XrCompositionLayerBaseHeader* Layer() { return m_layer.HasImage() ? m_layer.Layer() : nullptr; }

private:
	using Clock = std::chrono::steady_clock;
	static constexpr float SAMPLE_SECONDS = 0.5f;
	static constexpr int NUM_SAMPLES = 360; // 3 minutes
	static constexpr int AVERAGE_SAMPLES = 4; // 2 seconds

	enum Series
	{
		GAME_FPS,
		VR_FPS,
		EE,
		GS,
		VU,
		GPU,
		NUM_SERIES
	};
	// A sample's values, NaN where there is none (the game paused or not running).
	using Sample = std::array<float, NUM_SERIES>;

	void TakeSample(float seconds);
	void Draw();
	// The mean of the last AVERAGE_SAMPLES samples that have one, NaN if none.
	float Average(int series) const;
	// One graph: its series over the last NUM_SAMPLES samples, 0 at the bottom, max at the top.
	void DrawGraph(const Rect& area, float max, const std::vector<int>& series, const std::vector<float>& lines);

	OverlayLayer m_layer;
	Canvas m_canvas;
	Font* m_text = nullptr;
	bool m_created = false;

	std::array<Sample, NUM_SAMPLES> m_samples{};
	int m_count = 0; // samples taken, up to NUM_SAMPLES
	int m_next = 0; // where the next goes
	bool m_dirty = false; // a sample since the last draw

	Clock::time_point m_period_start{};
	int m_frames = 0; // displayed in this period
	uint64_t m_emu_updates = 0; // the emulator's last measurement
	Clock::time_point m_emu_updated_at{};
};
