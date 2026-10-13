// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Gui.h"

#include <algorithm>
#include <cmath>
#include <string>

void Gui::Begin(Canvas* canvas, const GuiInput& input, bool draw)
{
	m_canvas = canvas;
	m_input = input;
	m_draw = draw;
	m_blocked = m_disabled = false;
	SetClip(std::nullopt);
	if (!draw)
	{
		m_changed = false;
		m_next_hot = 0;
	}
}

void Gui::End()
{
	if (m_draw)
		return;
	if (m_next_hot != m_hot)
		m_changed = true;
	m_hot = m_next_hot;
	if (m_active && (m_input.released || !m_input.down))
	{
		m_active = 0;
		m_changed = true;
	}
}

bool Gui::Hovered(const Rect& rect) const
{
	return m_input.pointer && rect.Contains(m_input.x, m_input.y);
}

void Gui::SetClip(std::optional<Rect> clip)
{
	m_clip = clip;
	if (m_canvas && m_draw)
	{
		if (clip)
			m_canvas->SetClip(*clip);
		else
			m_canvas->ResetClip();
	}
}

bool Gui::Interact(uint32_t id, const Rect& rect)
{
	if (m_draw)
		return m_hot == id;
	const bool hovered = !m_blocked && !m_disabled && Hovered(rect) && (!m_clip || Hovered(*m_clip));
	if (hovered)
	{
		m_next_hot = id;
		if (m_input.pressed)
		{
			m_active = id;
			m_changed = true;
		}
	}
	return hovered;
}

bool Gui::Clicked(uint32_t id, bool hovered) const
{
	return !m_draw && hovered && m_input.released && m_active == id;
}

bool Gui::Button(uint32_t id, const Rect& rect, std::string_view label, uint32_t icon, Style style)
{
	const bool hovered = Interact(id, rect);
	if (m_draw)
	{
		const bool pressed = m_active == id;
		Color fill = hovered ? Theme::control_hover : Theme::control;
		Color ink = Theme::text;
		if (style == Style::Primary)
			fill = hovered ? Theme::accent_hover : Theme::accent;
		else if (style == Style::Danger)
			fill = hovered ? Theme::danger_hover : Theme::danger;
		if (pressed)
			fill = fill.WithAlpha(0.8f);
		Canvas& c = *m_canvas;
		c.FillRect(rect, Faded(fill), 12.0f);
		const float size = Theme::body_size;
		const float icon_w = icon ? size + 12.0f : 0.0f;
		const float w = icon_w + m_text.Measure(label, size);
		float x = std::round(rect.x + 0.5f * (rect.w - w));
		if (icon)
		{
			c.DrawIcon(m_icons, size, {x, rect.y, size, rect.h}, icon, Faded(ink));
			x += icon_w;
		}
		c.DrawText(m_text, size, x, std::round(rect.y + 0.5f * (rect.h - m_text.LineHeight(size))), label, Faded(ink));
	}
	return Clicked(id, hovered);
}

bool Gui::IconButton(uint32_t id, const Rect& rect, uint32_t icon, float icon_size)
{
	const bool hovered = Interact(id, rect);
	if (m_draw)
	{
		if (hovered)
			m_canvas->FillRect(rect, Theme::control_hover, 0.5f * std::min(rect.w, rect.h));
		m_canvas->DrawIcon(m_icons, icon_size, rect, icon, hovered ? Theme::text : Theme::text_muted);
	}
	return Clicked(id, hovered);
}

bool Gui::IconButton(uint32_t id, const Rect& rect, uint32_t icon, float icon_size, Color ink, Color ink_hover)
{
	const bool hovered = Interact(id, rect);
	if (m_draw)
	{
		if (hovered)
			m_canvas->FillRect(rect, Theme::control_hover, 0.5f * std::min(rect.w, rect.h));
		m_canvas->DrawIcon(m_icons, icon_size, rect, icon, hovered ? ink_hover : ink);
	}
	return Clicked(id, hovered);
}

bool Gui::Clickable(uint32_t id, const Rect& rect, bool* hovered)
{
	*hovered = Interact(id, rect);
	return Clicked(id, *hovered);
}

bool Gui::ScrollBar(uint32_t id, const Rect& track, float* offset, float content, float view)
{
	const float max_offset = std::max(content - view, 0.0f);
	if (max_offset <= 0.0f)
	{
		*offset = 0.0f;
		return false;
	}
	const float thumb_h = std::max(track.h * view / content, 48.0f);
	const float travel = track.h - thumb_h;
	const auto thumb_rect = [&](float o) { return Rect{track.x, track.y + travel * o / max_offset, track.w, thumb_h}; };
	const bool was_active = m_active == id;
	const bool hovered = Interact(id, track);
	bool changed = false;
	if (!m_draw && m_active == id && m_input.pointer)
	{
		if (!was_active)
		{
			// Grabbing the thumb keeps the grip; pressing the track centres the thumb on the pointer.
			const Rect thumb = thumb_rect(*offset);
			m_drag_from = thumb.Contains(m_input.x, m_input.y) ? m_input.y - thumb.y : 0.5f * thumb_h;
		}
		const float t = travel > 0.0f ? std::clamp((m_input.y - m_drag_from - track.y) / travel, 0.0f, 1.0f) : 0.0f;
		const float o = t * max_offset;
		if (std::abs(o - *offset) > 0.5f)
		{
			*offset = o;
			changed = m_changed = true;
		}
	}
	if (m_draw)
	{
		m_canvas->FillRect(track, Theme::nav, 0.5f * track.w);
		m_canvas->FillRect(thumb_rect(*offset).Inset(2.0f, 2.0f),
			hovered || m_active == id ? Theme::text_muted : Theme::control_hover, 0.5f * track.w - 2.0f);
	}
	return changed;
}

bool Gui::NavItem(uint32_t id, const Rect& rect, uint32_t icon, std::string_view label, bool selected, Style style)
{
	const bool hovered = Interact(id, rect);
	if (m_draw)
	{
		Canvas& c = *m_canvas;
		if (selected)
			c.FillRect(rect, Theme::item_selected, 12.0f);
		else if (hovered)
			c.FillRect(rect, Theme::item_hover, 12.0f);
		if (selected)
			c.FillRect({rect.x, rect.y + 14.0f, 5.0f, rect.h - 28.0f}, Theme::accent, 2.5f);
		Color ink = selected ? Theme::text : (hovered ? Theme::text : Theme::text_muted);
		if (style == Style::Danger)
			ink = hovered ? Theme::danger_hover : Theme::danger;
		c.DrawIcon(m_icons, 26.0f, {rect.x + 20.0f, rect.y, 40.0f, rect.h}, icon, selected ? Theme::accent : ink);
		c.DrawText(m_text, Theme::label_size, rect.x + 76.0f,
			std::round(rect.y + 0.5f * (rect.h - m_text.LineHeight(Theme::label_size))), label, ink);
	}
	return Clicked(id, hovered);
}

bool Gui::Toggle(uint32_t id, const Rect& rect, bool* value)
{
	// The switch sits at the right of rect; the whole rect is clickable.
	const bool hovered = Interact(id, rect);
	const bool clicked = Clicked(id, hovered);
	if (clicked)
	{
		*value = !*value;
		m_changed = true;
	}
	if (m_draw)
	{
		Canvas& c = *m_canvas;
		const float w = 76.0f, h = 42.0f;
		const Rect track{rect.Right() - w, rect.y + 0.5f * (rect.h - h), w, h};
		Color fill = *value ? (hovered ? Theme::accent_hover : Theme::accent) : (hovered ? Theme::control_hover : Theme::control);
		c.FillRect(track, Faded(fill), 0.5f * h);
		if (!*value)
			c.StrokeRect(track, Faded(Theme::border), 0.5f * h, 2.0f);
		const float knob = 0.5f * h - 5.0f;
		const float kx = *value ? track.Right() - 5.0f - knob : track.x + 5.0f + knob;
		c.FillCircle(kx, track.y + 0.5f * h, knob, Faded(Theme::text));
	}
	return clicked;
}

bool Gui::Segmented(uint32_t id, const Rect& rect, std::span<const std::string_view> options, int* index)
{
	bool changed = false;
	const float gap = 6.0f;
	const float w = (rect.w - gap * static_cast<float>(options.size() - 1)) / static_cast<float>(options.size());
	if (m_draw)
		m_canvas->FillRect(rect.Inset(-4.0f, -4.0f), Faded(Theme::nav), 14.0f);
	for (size_t i = 0; i < options.size(); i++)
	{
		const uint32_t item = id ^ GuiId("segment", static_cast<int>(i));
		const Rect r{std::round(rect.x + static_cast<float>(i) * (w + gap)), rect.y, std::round(w), rect.h};
		const bool hovered = Interact(item, r);
		if (Clicked(item, hovered) && *index != static_cast<int>(i))
		{
			*index = static_cast<int>(i);
			changed = true;
			m_changed = true;
		}
		if (m_draw)
		{
			const bool selected = *index == static_cast<int>(i);
			if (selected)
				m_canvas->FillRect(r, Faded(hovered ? Theme::accent_hover : Theme::accent), 10.0f);
			else if (hovered)
				m_canvas->FillRect(r, Theme::control_hover, 10.0f);
			TextCentered(r, options[i], Theme::body_size, Faded(selected || hovered ? Theme::text : Theme::text_muted));
		}
	}
	return changed;
}

bool Gui::Slider(uint32_t id, const Rect& rect, float* value, float min, float max, float step, std::string_view value_text)
{
	const float value_w = 120.0f;
	const Rect track{rect.x + 14.0f, rect.y, rect.w - value_w - 28.0f, rect.h};
	const bool hovered = Interact(id, {rect.x, rect.y, rect.w - value_w, rect.h});
	bool changed = false;
	if (!m_draw && m_active == id && m_input.pointer)
	{
		const float t = std::clamp((m_input.x - track.x) / track.w, 0.0f, 1.0f);
		float v = min + t * (max - min);
		if (step > 0.0f)
			v = min + std::round((v - min) / step) * step;
		v = std::clamp(v, min, max);
		if (std::abs(v - *value) > 1e-6f)
		{
			*value = v;
			changed = true;
			m_changed = true;
		}
	}
	if (m_draw)
	{
		Canvas& c = *m_canvas;
		const float t = max > min ? std::clamp((*value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
		const float cy = rect.y + 0.5f * rect.h;
		const float knob_x = track.x + t * track.w;
		c.FillRect({track.x, cy - 4.0f, track.w, 8.0f}, Faded(Theme::control), 4.0f);
		c.FillRect({track.x, cy - 4.0f, knob_x - track.x, 8.0f}, Faded(Theme::accent), 4.0f);
		const bool lit = hovered || m_active == id;
		c.FillCircle(knob_x, cy, lit ? 16.0f : 14.0f, Faded(Theme::text));
		const float tw = m_text.Measure(value_text, Theme::body_size);
		c.DrawText(m_text, Theme::body_size, rect.Right() - tw, std::round(cy - 0.5f * m_text.LineHeight(Theme::body_size)),
			value_text, Faded(Theme::text));
	}
	return changed;
}

float Gui::Text(float x, float y, std::string_view text, float size, Color color)
{
	return m_draw ? m_canvas->DrawText(m_text, size, x, y, text, Faded(color)) : 0.0f;
}

void Gui::TextCentered(const Rect& rect, std::string_view text, float size, Color color)
{
	if (!m_draw)
		return;
	const float w = m_text.Measure(text, size);
	m_canvas->DrawText(m_text, size, std::round(rect.x + 0.5f * (rect.w - w)),
		std::round(rect.y + 0.5f * (rect.h - m_text.LineHeight(size))), text, color);
}

std::string Gui::Elide(std::string_view text, float size, float width)
{
	if (m_text.Measure(text, size) <= width)
		return std::string(text);
	constexpr std::string_view dots = "\xe2\x80\xa6"; // ellipsis
	const float room = width - m_text.Measure(dots, size);
	std::string out;
	for (size_t pos = 0; pos < text.size();)
	{
		const size_t start = pos;
		NextCodepoint(text, &pos);
		if (m_text.Measure(std::string_view(text.data(), pos), size) > room)
			break;
		out.append(text.substr(start, pos - start));
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out + std::string(dots);
}

float Gui::TextWrapped(float x, float y, float width, std::string_view text, float size, Color color)
{
	const float line_h = std::round(m_text.LineHeight(size) * 1.1f);
	float cy = y;
	size_t start = 0;
	while (start < text.size())
	{
		// Take words while they fit.
		size_t end = start, best = std::string_view::npos;
		while (end <= text.size())
		{
			const size_t space = text.find(' ', end);
			const size_t stop = space == std::string_view::npos ? text.size() : space;
			if (m_text.Measure(text.substr(start, stop - start), size) > width && best != std::string_view::npos)
				break;
			best = stop;
			if (stop == text.size())
				break;
			end = stop + 1;
		}
		if (m_draw)
			m_canvas->DrawText(m_text, size, x, cy, text.substr(start, best - start), Faded(color));
		cy += line_h;
		start = best + 1;
	}
	return cy - y;
}
