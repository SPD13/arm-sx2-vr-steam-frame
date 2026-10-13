// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Canvas.h"

#include <algorithm>
#include <cmath>

void Canvas::Resize(int width, int height)
{
	m_width = width;
	m_height = height;
	m_pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
	ResetClip();
}

void Canvas::Clear(Color color)
{
	const uint32_t a = color.a;
	const uint32_t value = ((color.r * a + 127) / 255) | (((color.g * a + 127) / 255) << 8) |
	                       (((color.b * a + 127) / 255) << 16) | (a << 24);
	std::fill(m_pixels.begin(), m_pixels.end(), value);
}

void Canvas::SetClip(const Rect& clip)
{
	m_clip_x0 = std::clamp(static_cast<int>(std::floor(clip.x)), 0, m_width);
	m_clip_y0 = std::clamp(static_cast<int>(std::floor(clip.y)), 0, m_height);
	m_clip_x1 = std::clamp(static_cast<int>(std::ceil(clip.Right())), 0, m_width);
	m_clip_y1 = std::clamp(static_cast<int>(std::ceil(clip.Bottom())), 0, m_height);
}

void Canvas::ResetClip()
{
	m_clip_x0 = m_clip_y0 = 0;
	m_clip_x1 = m_width;
	m_clip_y1 = m_height;
}

void Canvas::Blend(int x, int y, Color color, float coverage)
{
	if (x < m_clip_x0 || y < m_clip_y0 || x >= m_clip_x1 || y >= m_clip_y1 || coverage <= 0.0f)
		return;
	// Source over, premultiplied.
	const float sa = static_cast<float>(color.a) / 255.0f * std::min(coverage, 1.0f);
	uint32_t& pixel = m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)];
	const float keep = 1.0f - sa;
	const auto channel = [&](int shift, uint8_t src) {
		const float dst = static_cast<float>((pixel >> shift) & 0xFF);
		return static_cast<uint32_t>(std::min(255.0f, static_cast<float>(src) * sa + dst * keep + 0.5f)) << shift;
	};
	pixel = channel(0, color.r) | channel(8, color.g) | channel(16, color.b) | channel(24, 255);
}

void Canvas::FillSpan(int x0, int x1, int y, Color color)
{
	x0 = std::max(x0, m_clip_x0);
	x1 = std::min(x1, m_clip_x1);
	if (y < m_clip_y0 || y >= m_clip_y1 || x0 >= x1 || color.a == 0)
		return;
	uint32_t* row = &m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width)];
	const uint32_t a = color.a;
	const uint32_t r = (color.r * a + 127) / 255, g = (color.g * a + 127) / 255, b = (color.b * a + 127) / 255;
	if (a == 255)
	{
		std::fill(row + x0, row + x1, r | (g << 8) | (b << 16) | (255u << 24));
		return;
	}
	// Premultiplied source over, in 8-bit fixed point.
	const uint32_t keep = 255 - a;
	for (int x = x0; x < x1; x++)
	{
		const uint32_t d = row[x];
		const auto mix = [&](int shift, uint32_t src) { return (src + (((d >> shift) & 0xFF) * keep + 127) / 255) << shift; };
		row[x] = mix(0, r) | mix(8, g) | mix(16, b) | mix(24, a);
	}
}

void Canvas::FillShape(const Rect& rect, Color color, float radius, float thickness)
{
	radius = std::clamp(radius, 0.0f, 0.5f * std::min(rect.w, rect.h));
	const float cx = rect.x + 0.5f * rect.w, cy = rect.y + 0.5f * rect.h;
	const float hx = 0.5f * rect.w - radius, hy = 0.5f * rect.h - radius;
	const int x0 = std::max(m_clip_x0, static_cast<int>(std::floor(rect.x)));
	const int y0 = std::max(m_clip_y0, static_cast<int>(std::floor(rect.y)));
	const int x1 = std::min(m_clip_x1, static_cast<int>(std::ceil(rect.Right())));
	const int y1 = std::min(m_clip_y1, static_cast<int>(std::ceil(rect.Bottom())));
	// Signed distance to the rounded rectangle's edge, negative inside.
	const auto distance = [&](float qx, float qy) {
		const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
		return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
	};
	const auto shade = [&](int x, int y, float qy) {
		const float qx = std::abs(static_cast<float>(x) + 0.5f - cx) - hx;
		const float d = distance(qx, qy);
		float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
		if (thickness > 0.0f)
			coverage = std::min(coverage, std::clamp(0.5f + d + thickness, 0.0f, 1.0f));
		Blend(x, y, color, coverage);
	};
	for (int y = y0; y < y1; y++)
	{
		const float py = static_cast<float>(y) + 0.5f;
		const float qy = std::abs(py - cy) - hy;
		// The row's half width at this height, inset by a pixel: everything nearer the centre is
		// fully inside (and, for a border, a further thickness in is fully outside it).
		const float dy = std::max(qy, 0.0f);
		const float half = hx + (dy < radius ? std::sqrt(radius * radius - dy * dy) : 0.0f);
		// For a border, the same applies to its inner edge: nearer the centre, nothing is drawn.
		float inner = half - 1.0f;
		if (thickness > 0.0f)
		{
			inner = -1.0f;
			const float ay = std::abs(py - cy);
			if (radius >= thickness)
			{
				const float r_in = radius - thickness;
				const float dy_in = std::max(ay - hy, 0.0f);
				if (dy_in < r_in || (r_in == 0.0f && ay < hy))
					inner = hx + std::sqrt(std::max(r_in * r_in - dy_in * dy_in, 0.0f)) - 1.0f;
			}
			else if (ay < 0.5f * rect.h - thickness - 0.5f)
				inner = 0.5f * rect.w - thickness - 1.0f;
		}
		const int span0 = inner > 0.0f ? std::max(x0, static_cast<int>(std::ceil(cx - inner))) : x1;
		const int span1 = inner > 0.0f ? std::min(x1, static_cast<int>(std::floor(cx + inner))) : x1;
		for (int x = x0; x < std::min(span0, x1); x++)
			shade(x, y, qy);
		if (span0 < span1 && thickness <= 0.0f)
			FillSpan(span0, span1, y, color);
		for (int x = std::max(span1, x0); x < x1; x++)
			shade(x, y, qy);
	}
}

void Canvas::FillRect(const Rect& rect, Color color, float radius)
{
	FillShape(rect, color, radius, 0.0f);
}

void Canvas::StrokeRect(const Rect& rect, Color color, float radius, float thickness)
{
	FillShape(rect, color, radius, thickness);
}

void Canvas::FillCircle(float cx, float cy, float radius, Color color)
{
	FillShape({cx - radius, cy - radius, 2.0f * radius, 2.0f * radius}, color, radius, 0.0f);
}

float Canvas::DrawText(Font& font, float size, float x, float y, std::string_view utf8, Color color)
{
	const float baseline = std::round(y + font.Ascent(size));
	float pen = x;
	uint32_t previous = 0;
	for (size_t pos = 0; pos < utf8.size();)
	{
		const uint32_t cp = NextCodepoint(utf8, &pos);
		if (previous)
			pen += font.Kerning(previous, cp, size);
		const Font::Glyph& glyph = font.GetGlyph(cp, size);
		const int gx = static_cast<int>(std::round(pen)) + glyph.left;
		const int gy = static_cast<int>(baseline) + glyph.top;
		for (int row = 0; row < glyph.height; row++)
		{
			for (int col = 0; col < glyph.width; col++)
			{
				const uint8_t c = glyph.coverage[static_cast<size_t>(row) * static_cast<size_t>(glyph.width) + col];
				if (c)
					Blend(gx + col, gy + row, color, static_cast<float>(c) / 255.0f);
			}
		}
		pen += glyph.advance;
		previous = cp;
	}
	return pen - x;
}

void Canvas::DrawIcon(Font& font, float size, const Rect& rect, uint32_t codepoint, Color color)
{
	const Font::Glyph& glyph = font.GetGlyph(codepoint, size);
	// Centre the glyph's ink, not its advance box.
	const int gx = static_cast<int>(std::round(rect.x + 0.5f * (rect.w - static_cast<float>(glyph.width))));
	const int gy = static_cast<int>(std::round(rect.y + 0.5f * (rect.h - static_cast<float>(glyph.height))));
	for (int row = 0; row < glyph.height; row++)
	{
		for (int col = 0; col < glyph.width; col++)
		{
			const uint8_t c = glyph.coverage[static_cast<size_t>(row) * static_cast<size_t>(glyph.width) + col];
			if (c)
				Blend(gx + col, gy + row, color, static_cast<float>(c) / 255.0f);
		}
	}
}
