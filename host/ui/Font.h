// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A TrueType font rasterised with stb_truetype, glyphs cached per pixel size. The VR menu draws on
// the CPU (see Canvas) because PCSX2's ImGui context is a process-wide global that the GS threads
// drive, so the XR thread cannot run an ImGui frame of its own.

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

class Font
{
public:
	struct Glyph
	{
		int width = 0, height = 0; // bitmap size
		int left = 0, top = 0; // bitmap offset from the pen position on the baseline (top is negative)
		float advance = 0.0f;
		std::vector<uint8_t> coverage; // width * height
	};

	Font();
	~Font();

	// The font file's contents; the Font keeps them.
	bool Load(std::vector<uint8_t> data);
	bool IsLoaded() const { return m_info != nullptr; }
	bool HasGlyph(uint32_t codepoint) const;

	// size is the pixel height from the ascender to the descender.
	const Glyph& GetGlyph(uint32_t codepoint, float size);
	float Ascent(float size) const;
	float LineHeight(float size) const;
	float Kerning(uint32_t a, uint32_t b, float size) const;
	float Measure(std::string_view utf8, float size);

private:
	std::vector<uint8_t> m_data;
	std::unique_ptr<struct stbtt_fontinfo> m_info;
	int m_ascent = 0, m_descent = 0, m_line_gap = 0;
	std::unordered_map<uint64_t, Glyph> m_cache;
};

// Decodes one UTF-8 code point at text[*pos] and advances *pos; malformed bytes come out as U+FFFD.
uint32_t NextCodepoint(std::string_view text, size_t* pos);
