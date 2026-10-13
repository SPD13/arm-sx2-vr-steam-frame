// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Font.h"

#include <cmath>
#include <cstring>

// ImGui compiles its copy of stb_truetype static too, so the two do not clash.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

Font::Font() = default;

Font::~Font() = default;

bool Font::Load(std::vector<uint8_t> data)
{
	m_data = std::move(data);
	m_cache.clear();
	auto info = std::make_unique<stbtt_fontinfo>();
	if (m_data.empty() || !stbtt_InitFont(info.get(), m_data.data(), stbtt_GetFontOffsetForIndex(m_data.data(), 0)))
	{
		m_info.reset();
		return false;
	}
	stbtt_GetFontVMetrics(info.get(), &m_ascent, &m_descent, &m_line_gap);
	m_info = std::move(info);
	return true;
}

bool Font::HasGlyph(uint32_t codepoint) const
{
	return m_info && stbtt_FindGlyphIndex(m_info.get(), static_cast<int>(codepoint)) != 0;
}

const Font::Glyph& Font::GetGlyph(uint32_t codepoint, float size)
{
	const uint64_t key = (static_cast<uint64_t>(codepoint) << 32) | static_cast<uint32_t>(std::lround(size * 16.0f));
	if (const auto it = m_cache.find(key); it != m_cache.end())
		return it->second;

	Glyph& glyph = m_cache[key];
	if (!m_info)
		return glyph;
	const float scale = stbtt_ScaleForPixelHeight(m_info.get(), size);
	int advance = 0, lsb = 0;
	stbtt_GetCodepointHMetrics(m_info.get(), static_cast<int>(codepoint), &advance, &lsb);
	glyph.advance = static_cast<float>(advance) * scale;
	int x0, y0, x1, y1;
	stbtt_GetCodepointBitmapBox(m_info.get(), static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);
	glyph.width = x1 - x0;
	glyph.height = y1 - y0;
	glyph.left = x0;
	glyph.top = y0;
	if (glyph.width > 0 && glyph.height > 0)
	{
		glyph.coverage.resize(static_cast<size_t>(glyph.width) * static_cast<size_t>(glyph.height));
		stbtt_MakeCodepointBitmap(m_info.get(), glyph.coverage.data(), glyph.width, glyph.height, glyph.width, scale,
			scale, static_cast<int>(codepoint));
	}
	return glyph;
}

float Font::Ascent(float size) const
{
	return m_info ? static_cast<float>(m_ascent) * stbtt_ScaleForPixelHeight(m_info.get(), size) : 0.0f;
}

float Font::LineHeight(float size) const
{
	return m_info ? static_cast<float>(m_ascent - m_descent + m_line_gap) * stbtt_ScaleForPixelHeight(m_info.get(), size) :
	                0.0f;
}

float Font::Kerning(uint32_t a, uint32_t b, float size) const
{
	if (!m_info)
		return 0.0f;
	return static_cast<float>(stbtt_GetCodepointKernAdvance(m_info.get(), static_cast<int>(a), static_cast<int>(b))) *
	       stbtt_ScaleForPixelHeight(m_info.get(), size);
}

float Font::Measure(std::string_view utf8, float size)
{
	float width = 0.0f;
	uint32_t previous = 0;
	for (size_t pos = 0; pos < utf8.size();)
	{
		const uint32_t cp = NextCodepoint(utf8, &pos);
		if (previous)
			width += Kerning(previous, cp, size);
		width += GetGlyph(cp, size).advance;
		previous = cp;
	}
	return width;
}

uint32_t NextCodepoint(std::string_view text, size_t* pos)
{
	const auto byte = [&](size_t i) { return static_cast<uint8_t>(text[i]); };
	const uint8_t lead = byte(*pos);
	int extra;
	uint32_t cp;
	if (lead < 0x80)
		extra = 0, cp = lead;
	else if ((lead & 0xE0) == 0xC0)
		extra = 1, cp = lead & 0x1F;
	else if ((lead & 0xF0) == 0xE0)
		extra = 2, cp = lead & 0x0F;
	else if ((lead & 0xF8) == 0xF0)
		extra = 3, cp = lead & 0x07;
	else
	{
		(*pos)++;
		return 0xFFFD;
	}
	if (*pos + extra >= text.size())
	{
		*pos = text.size();
		return 0xFFFD;
	}
	for (int i = 1; i <= extra; i++)
	{
		const uint8_t b = byte(*pos + i);
		if ((b & 0xC0) != 0x80)
		{
			*pos += i;
			return 0xFFFD;
		}
		cp = (cp << 6) | (b & 0x3F);
	}
	*pos += extra + 1;
	return cp;
}
