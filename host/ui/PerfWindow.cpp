// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ui/PerfWindow.h"

#include "Emu.h"
#include "ui/Gui.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr float PAD_PX = 22.0f;
	constexpr float SECTION_PX = 244.0f; // header, legend, graph and the gap below
	constexpr float LEGEND_Y_PX = 44.0f; // below the section's top
	constexpr float GRAPH_Y_PX = 76.0f;
	constexpr float GRAPH_H_PX = 146.0f;
	constexpr float VALUE_SIZE = 38.0f;
	constexpr float LEGEND_SIZE = 20.0f;
	constexpr float AXIS_SIZE = 18.0f;
	constexpr float GRID_SIZE = 15.0f;

	constexpr Color SERIES_COLOR[] = {
		Rgb(0x4f8cff), // game fps
		Rgb(0x9097a6), // headset fps
		Rgb(0xf5a524), // core thread
		Rgb(0xd98cff), // GS thread
		Rgb(0x4fd1c5), // VU thread
		Rgb(0x46c37b), // GPU
	};

	constexpr float NONE = std::numeric_limits<float>::quiet_NaN();
} // namespace

bool PerfWindow::Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index,
	Font* text, float meters_per_px)
{
	if (!m_layer.Create(xr, device, gpu, queue, queue_family_index, WIDTH_PX, HEIGHT_PX))
	{
		m_layer.Destroy();
		return false;
	}
	m_layer.SetSize(WIDTH_PX * meters_per_px, HEIGHT_PX * meters_per_px);
	m_canvas.Resize(WIDTH_PX, HEIGHT_PX);
	m_text = text;
	m_created = true;
	return true;
}

void PerfWindow::Destroy()
{
	m_created = false;
	m_layer.Destroy();
}

void PerfWindow::Update(bool shown)
{
	if (!m_created)
		return;
	const Clock::time_point now = Clock::now();
	if (m_period_start == Clock::time_point{})
		m_period_start = now;
	m_frames++;
	const float seconds = std::chrono::duration<float>(now - m_period_start).count();
	if (seconds >= SAMPLE_SECONDS)
	{
		TakeSample(seconds);
		m_period_start = now;
		m_frames = 0;
	}
	if (shown && (m_dirty || !m_layer.HasImage()))
	{
		Draw();
		m_layer.Upload(m_canvas);
		m_dirty = false;
	}
}

void PerfWindow::TakeSample(float seconds)
{
	const Clock::time_point now = Clock::now();
	const Emu::PerfMetrics emu = Emu::GetPerfMetrics();
	if (emu.updates != m_emu_updates)
	{
		m_emu_updates = emu.updates;
		m_emu_updated_at = now;
	}
	// The emulator measures every 0.5 s while it runs: nothing new for longer, and it is paused or stopped.
	const bool running = m_emu_updates > 0 && now - m_emu_updated_at < std::chrono::milliseconds(1200);
	Sample& s = m_samples[m_next];
	s[GAME_FPS] = running ? emu.fps : NONE;
	s[VR_FPS] = static_cast<float>(m_frames) / seconds;
	s[EE] = running ? emu.ee : NONE;
	s[GS] = running ? emu.gs : NONE;
	s[VU] = running ? emu.vu : NONE;
	s[GPU] = running ? emu.gpu : NONE;
	m_next = (m_next + 1) % NUM_SAMPLES;
	m_count = std::min(m_count + 1, NUM_SAMPLES);
	m_dirty = true;
}

float PerfWindow::Average(int series) const
{
	float sum = 0.0f;
	int n = 0;
	for (int i = 1; i <= std::min(m_count, AVERAGE_SAMPLES); i++)
	{
		const float v = m_samples[(m_next - i + NUM_SAMPLES) % NUM_SAMPLES][series];
		if (!std::isnan(v))
		{
			sum += v;
			n++;
		}
	}
	return n ? sum / static_cast<float>(n) : NONE;
}

void PerfWindow::DrawGraph(const Rect& area, float max, const std::vector<int>& series, const std::vector<float>& lines)
{
	Canvas& c = m_canvas;
	c.FillRect(area, Theme::nav.WithAlpha(0.8f), 6.0f);
	// The grid lines, labelled at the left (the oldest samples, away from the current ones): fps, or %
	// with a scale of 100.
	for (const float line : lines)
	{
		const float y = std::round(area.Bottom() - area.h * line / max);
		c.FillRect({area.x + 4.0f, y, area.w - 8.0f, 1.0f}, Theme::border.WithAlpha(0.7f));
		const std::string label = max == 100.0f ? fmt::format("{:.0f}%", line) : fmt::format("{:.0f}", line);
		c.DrawText(*m_text, GRID_SIZE, area.x + 8.0f, y - m_text->LineHeight(GRID_SIZE), label,
			Theme::text_muted.WithAlpha(0.8f));
	}
	const Rect inner = area.Inset(4.0f, 4.0f);
	c.SetClip(inner);
	const float dx = inner.w / static_cast<float>(NUM_SAMPLES - 1);
	const auto x_of = [&](int k) { return inner.x + dx * static_cast<float>(NUM_SAMPLES - m_count + k); };
	const auto y_of = [&](float v) { return inner.Bottom() - inner.h * std::clamp(v, 0.0f, max) / max; };
	bool first = true;
	for (const int s : series)
	{
		const Color color = SERIES_COLOR[s];
		for (int k = 1; k < m_count; k++)
		{
			const float v0 = m_samples[(m_next - m_count + k - 1 + NUM_SAMPLES) % NUM_SAMPLES][s];
			const float v1 = m_samples[(m_next - m_count + k + NUM_SAMPLES) % NUM_SAMPLES][s];
			if (std::isnan(v0) || std::isnan(v1))
				continue;
			const float x0 = x_of(k - 1), x1 = x_of(k);
			const float y0 = y_of(v0), y1 = y_of(v1);
			// The first series filled below its line.
			if (first)
				c.FillRect({x0, y1, x1 - x0, inner.Bottom() - y1}, color.WithAlpha(0.18f));
			const float top = std::min(y0, y1) - 1.0f, bottom = std::max(y0, y1) + 1.0f;
			c.FillRect({x0, top, std::max(x1 - x0, 1.5f), bottom - top}, color);
		}
		first = false;
	}
	c.ResetClip();
}

void PerfWindow::Draw()
{
	Canvas& c = m_canvas;
	Font& font = *m_text;
	c.Clear();
	const Rect all{0.0f, 0.0f, static_cast<float>(WIDTH_PX), static_cast<float>(HEIGHT_PX)};
	c.FillRect(all, Theme::panel.WithAlpha(0.88f), 24.0f);
	c.StrokeRect(all, Theme::border, 24.0f, 2.0f);

	const float graph_w = WIDTH_PX - 2.0f * PAD_PX;
	const auto number = [](float v, const char* format) {
		return std::isnan(v) ? std::string("–") : fmt::format(fmt::runtime(format), v);
	};
	// A section: its label at the left, value (the first series' 2-second average) at the right, a
	// legend line with each series' average, and the graph.
	const auto section = [&](int index, std::string_view label, std::string value, Color value_color,
							 const std::vector<std::pair<int, std::string>>& legend, float max,
							 const std::vector<int>& series, const std::vector<float>& lines) {
		const float top = PAD_PX + SECTION_PX * static_cast<float>(index);
		c.DrawText(font, Theme::label_size, PAD_PX, top + 6.0f, label, Theme::text);
		const float value_w = font.Measure(value, VALUE_SIZE);
		c.DrawText(font, VALUE_SIZE, WIDTH_PX - PAD_PX - value_w, top, value, value_color);
		float x = PAD_PX;
		const float legend_y = top + LEGEND_Y_PX;
		const float dot_y = legend_y + 0.5f * font.LineHeight(LEGEND_SIZE);
		for (const auto& [s, text] : legend)
		{
			c.FillCircle(x + 6.0f, dot_y, 6.0f, SERIES_COLOR[s]);
			x += 18.0f;
			x += c.DrawText(font, LEGEND_SIZE, x, legend_y, text, Theme::text_muted) + 22.0f;
		}
		DrawGraph({PAD_PX, top + GRAPH_Y_PX, graph_w, GRAPH_H_PX}, max, series, lines);
	};

	const float game = Average(GAME_FPS), headset = Average(VR_FPS);
	section(0, "Frame rate", number(game, "{:.1f} fps"), SERIES_COLOR[GAME_FPS],
		{{GAME_FPS, "game " + number(game, "{:.1f}")}, {VR_FPS, "headset " + number(headset, "{:.0f}")}}, 80.0f,
		{GAME_FPS, VR_FPS}, {60.0f, 72.0f});

	// VU1 only has a thread of its own with MTVU on.
	bool vu_thread = false;
	for (int k = 0; k < m_count; k++)
		vu_thread |= m_samples[k][VU] > 0.5f;
	const float ee = Average(EE);
	std::vector<std::pair<int, std::string>> cpu_legend = {
		{EE, "core " + number(ee, "{:.0f}%")}, {GS, "GS " + number(Average(GS), "{:.0f}%")}};
	std::vector<int> cpu_series = {EE, GS};
	if (vu_thread)
	{
		cpu_legend.push_back({VU, "VU1 " + number(Average(VU), "{:.0f}%")});
		cpu_series.push_back(VU);
	}
	section(1, "CPU", number(ee, "{:.0f}%"), SERIES_COLOR[EE], cpu_legend, 100.0f, cpu_series, {25.0f, 50.0f, 75.0f});

	const float gpu = Average(GPU);
	section(2, "GPU", number(gpu, "{:.0f}%"), SERIES_COLOR[GPU], {{GPU, "emulator " + number(gpu, "{:.0f}%")}}, 100.0f,
		{GPU}, {25.0f, 50.0f, 75.0f});

	const float axis_y = PAD_PX + 3.0f * SECTION_PX - 14.0f;
	c.DrawText(font, AXIS_SIZE, PAD_PX, axis_y, "3 min ago", Theme::text_muted);
	const float now_w = font.Measure("now", AXIS_SIZE);
	c.DrawText(font, AXIS_SIZE, WIDTH_PX - PAD_PX - now_w, axis_y, "now", Theme::text_muted);
}
