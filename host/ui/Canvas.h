// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A CPU-drawn RGBA8 image for the VR menu: anti-aliased rounded rectangles, circles and text.
// Pixels are premultiplied by alpha, as OpenXR quad layers expect, and their colour is
// sRGB-encoded, which the sRGB swapchain decodes.

#pragma once

#include "Font.h"

#include <cstdint>
#include <string_view>
#include <vector>

struct Color
{
	uint8_t r = 0, g = 0, b = 0, a = 255;

	constexpr Color WithAlpha(float alpha) const
	{
		return {r, g, b, static_cast<uint8_t>(static_cast<float>(a) * alpha + 0.5f)};
	}
};

constexpr Color Rgb(uint32_t rgb, uint8_t a = 255)
{
	return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), a};
}

struct Rect
{
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

	bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	float Right() const { return x + w; }
	float Bottom() const { return y + h; }
	Rect Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2.0f * dx, h - 2.0f * dy}; }
};

class Canvas
{
public:
	void Resize(int width, int height);
	int Width() const { return m_width; }
	int Height() const { return m_height; }
	const uint32_t* Pixels() const { return m_pixels.data(); }

	void Clear(Color color = {0, 0, 0, 0});
	// Drawing outside the clip rectangle is dropped; the default is the whole canvas.
	void SetClip(const Rect& clip);
	void ResetClip();

	void FillRect(const Rect& rect, Color color, float radius = 0.0f);
	// A border of the given thickness inside rect.
	void StrokeRect(const Rect& rect, Color color, float radius, float thickness);
	void FillCircle(float cx, float cy, float radius, Color color);

	// Text with its line box's top at y; returns the advance width.
	float DrawText(Font& font, float size, float x, float y, std::string_view utf8, Color color);
	// One code point (an icon) centred in rect.
	void DrawIcon(Font& font, float size, const Rect& rect, uint32_t codepoint, Color color);

private:
	void Blend(int x, int y, Color color, float coverage);
	// Fully covered pixels [x0, x1) of row y.
	void FillSpan(int x0, int x1, int y, Color color);
	// Coverage of a rounded rectangle (or, with thickness > 0, its border) over each pixel.
	void FillShape(const Rect& rect, Color color, float radius, float thickness);

	int m_width = 0, m_height = 0;
	std::vector<uint32_t> m_pixels;
	int m_clip_x0 = 0, m_clip_y0 = 0, m_clip_x1 = 0, m_clip_y1 = 0;
};
