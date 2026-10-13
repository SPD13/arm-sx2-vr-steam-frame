// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A small immediate-mode widget set over Canvas, driven by a pointer (the controller's laser).
//
// The UI code runs twice when needed: an input pass every frame, where widgets only react to the
// pointer and report clicks and new values, then a draw pass, only when something visible changed
// (Changed()), where they paint and ignore the input.

#pragma once

#include "Canvas.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct GuiInput
{
	bool pointer = false; // whether the pointer is over the canvas
	float x = 0.0f, y = 0.0f; // canvas pixels
	bool down = false; // the trigger is held
	bool pressed = false; // ... since this frame
	bool released = false;
};

namespace Theme
{
	inline constexpr Color panel = Rgb(0x15171c);
	inline constexpr Color nav = Rgb(0x1c1f26);
	inline constexpr Color item_hover = Rgb(0x272b35);
	inline constexpr Color item_selected = Rgb(0x26365a);
	inline constexpr Color accent = Rgb(0x4f8cff);
	inline constexpr Color accent_hover = Rgb(0x6c9fff);
	inline constexpr Color text = Rgb(0xeceef3);
	inline constexpr Color text_muted = Rgb(0x9097a6);
	inline constexpr Color control = Rgb(0x2a2f3a);
	inline constexpr Color control_hover = Rgb(0x353b48);
	inline constexpr Color border = Rgb(0x3a404d);
	inline constexpr Color danger = Rgb(0xe5484d);
	inline constexpr Color danger_hover = Rgb(0xf06a6e);

	inline constexpr float title_size = 36.0f;
	inline constexpr float label_size = 26.0f;
	inline constexpr float body_size = 23.0f;
	inline constexpr float small_size = 20.0f;
} // namespace Theme

// A widget id from a name and an index, for the widgets that repeat.
constexpr uint32_t GuiId(std::string_view name, int index = 0)
{
	uint32_t hash = 2166136261u;
	for (const char c : name)
		hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
	hash = (hash ^ static_cast<uint32_t>(index)) * 16777619u;
	return hash ? hash : 1;
}

class Gui
{
public:
	enum class Style
	{
		Normal,
		Primary,
		Danger,
	};

	Gui(Font& text, Font& icons) : m_text(text), m_icons(icons) {}

	void Begin(Canvas* canvas, const GuiInput& input, bool draw);
	void End();

	bool Drawing() const { return m_draw; }
	Canvas& Draw() { return *m_canvas; }
	Font& TextFont() { return m_text; }
	Font& IconFont() { return m_icons; }
	const GuiInput& Input() const { return m_input; }

	// Whether the input pass changed anything visible (hover, press, a value); forces a draw pass.
	bool Changed() const { return m_changed; }
	void MarkChanged() { m_changed = true; }

	// Widgets ignore the pointer while blocked (under a dialog) or disabled; disabled ones are greyed.
	void SetBlocked(bool blocked) { m_blocked = blocked; }
	void SetDisabled(bool disabled) { m_disabled = disabled; }
	// Widgets outside the clip rectangle are neither drawn nor hit (a scrolled list); nullopt clears it.
	void SetClip(std::optional<Rect> clip);

	// Each returns true on a click or a new value.
	bool Button(uint32_t id, const Rect& rect, std::string_view label, uint32_t icon = 0, Style style = Style::Normal);
	bool IconButton(uint32_t id, const Rect& rect, uint32_t icon, float icon_size);
	// With its own colours, as the star of a favourite.
	bool IconButton(uint32_t id, const Rect& rect, uint32_t icon, float icon_size, Color ink, Color ink_hover);
	// An area the caller draws: hovered tells whether to highlight it.
	bool Clickable(uint32_t id, const Rect& rect, bool* hovered);
	bool NavItem(uint32_t id, const Rect& rect, uint32_t icon, std::string_view label, bool selected, Style style = Style::Normal);
	bool Toggle(uint32_t id, const Rect& rect, bool* value);
	bool Segmented(uint32_t id, const Rect& rect, std::span<const std::string_view> options, int* index);
	// Snaps to step; value_text is shown at the right of the track.
	bool Slider(uint32_t id, const Rect& rect, float* value, float min, float max, float step, std::string_view value_text);
	// A vertical scroll bar over content taller than the view; offset is the scroll position.
	bool ScrollBar(uint32_t id, const Rect& track, float* offset, float content, float view);
	// Whether the pointer is over rect (for clicks outside a dialog, say).
	bool Hovered(const Rect& rect) const;

	// Draw-pass helpers.
	float Text(float x, float y, std::string_view text, float size = Theme::body_size, Color color = Theme::text);
	void TextCentered(const Rect& rect, std::string_view text, float size, Color color);
	float TextWidth(std::string_view text, float size) { return m_text.Measure(text, size); }
	// text cut to fit width, with an ellipsis.
	std::string Elide(std::string_view text, float size, float width);
	// Wraps at spaces to fit width; returns the height used.
	float TextWrapped(float x, float y, float width, std::string_view text, float size, Color color);

private:
	// Hover and press handling; returns whether the widget is hovered.
	bool Interact(uint32_t id, const Rect& rect);
	bool Clicked(uint32_t id, bool hovered) const;
	Color Faded(Color color) const { return m_disabled ? color.WithAlpha(0.4f) : color; }

	Font& m_text;
	Font& m_icons;
	Canvas* m_canvas = nullptr;
	GuiInput m_input;
	bool m_draw = false;
	bool m_blocked = false;
	bool m_disabled = false;
	std::optional<Rect> m_clip;
	bool m_changed = false;
	uint32_t m_hot = 0; // under the pointer, as of the last input pass
	uint32_t m_next_hot = 0;
	uint32_t m_active = 0; // pressed and not yet released
	float m_drag_from = 0.0f; // a scroll bar drag: pointer y minus the thumb's top
};
