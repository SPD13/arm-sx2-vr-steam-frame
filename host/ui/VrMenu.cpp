// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "VrMenu.h"

#include "Emu.h"
#include "XrMath.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include "pcsx2/Config.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace
{
	constexpr float BUTTON_SIZE = 0.12f; // meters
	constexpr float BUTTON_GAP = 0.03f; // between the two buttons
	constexpr float BUTTON_MARGIN = 0.04f; // between the screen's right edge and the buttons
	// A pointer gets onto a button within its circle, and stays on it (the beam and the trigger are
	// the button's) until it leaves this much around its centre: a hand's tremble at the edge, or the
	// game's parallax moving behind it, does not take the beam back into the game.
	constexpr float BUTTON_HOLD_RADIUS = 0.75f * BUTTON_SIZE;
	constexpr float POINTER_REACH = 6.0f; // the beam ends on the screen's plane up to this far away
	constexpr float FREE_BEAM_LENGTH = 1.0f; // and is this long when it points elsewhere
	constexpr float PANEL_LIFT = 0.03f; // the panel stands this far in front of the screen
	constexpr float CURSOR_SIZE = 0.035f;
	constexpr float BEAM_WIDTH = 0.006f;
	constexpr int BUTTON_PX = 128;
	constexpr int CURSOR_PX = 64;
	constexpr int BEAM_W_PX = 256, BEAM_H_PX = 16;
	// The tooltip: a rounded label left-aligned in its layer, at the buttons' pixel density.
	constexpr int TOOLTIP_W_PX = 640, TOOLTIP_H_PX = 64;
	constexpr float TOOLTIP_TEXT_SIZE = 32.0f;
	constexpr float TOOLTIP_PAD_PX = 20.0f;
	constexpr float TOOLTIP_GAP = 0.02f; // between a button and its tooltip
	// The Wi-Fi line below the screen: smaller than a tooltip.
	constexpr int WEB_LINE_H_PX = 48;
	constexpr float WEB_LINE_TEXT_SIZE = 24.0f;
	// The emulator's messages above the screen: wrapped, oldest on top, bottom-aligned in their layer.
	constexpr int MESSAGES_W_PX = 1280, MESSAGES_H_PX = 320;
	constexpr float MESSAGE_TEXT_SIZE = 28.0f;
	constexpr float MESSAGE_PAD_PX = 12.0f; // above and below a message's text
	constexpr float MESSAGE_GAP_PX = 8.0f; // between two messages
	constexpr float METERS_PER_PX = BUTTON_SIZE / static_cast<float>(BUTTON_PX);
	constexpr int ATLAS_GUTTER_PX = 2; // transparent, between the atlas's parts: filtering does not bleed across
	constexpr float BUTTON_GROUP_GAP = 0.05f; // more between the menu buttons and the light gun's
	constexpr uint32_t GEAR_ICON = 0xf013;
	constexpr uint32_t GAMES_ICON = 0xf11b; // gamepad
	constexpr uint32_t VOLUMETRIC_ICON = 0xf1b2; // cube
	constexpr uint32_t FLAT_ICON = 0xf0c8; // square
	constexpr uint32_t GUN_MODE_ICON = 0xe19b; // gun
	constexpr uint32_t ONE_GUN_ICON = 0xf007; // user
	constexpr uint32_t TWO_GUNS_ICON = 0xf500; // user-group: two people
	constexpr uint32_t PAUSE_ICON = 0xf04c;
	constexpr uint32_t PLAY_ICON = 0xf04b;
	constexpr uint32_t POWER_ICON = 0xf011; // power-off
	constexpr float SCROLL_SPEED = 22.0f; // pixels per frame with a thumbstick pushed all the way

	// text's lines (at its line breaks) wrapped at spaces to fit width.
	std::vector<std::string_view> WrapLines(Font& font, std::string_view text, float size, float width)
	{
		while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
			text.remove_suffix(1);
		std::vector<std::string_view> lines;
		size_t para = 0;
		while (para <= text.size())
		{
			const size_t para_end = std::min(text.find('\n', para), text.size());
			const std::string_view line = text.substr(para, para_end - para);
			size_t start = 0;
			do
			{
				// Take words while they fit (at least one).
				size_t best = std::string_view::npos, end = start;
				for (;;)
				{
					const size_t stop = std::min(line.find(' ', end), line.size());
					if (best != std::string_view::npos && font.Measure(line.substr(start, stop - start), size) > width)
						break;
					best = stop;
					if (stop == line.size())
						break;
					end = stop + 1;
				}
				lines.push_back(line.substr(start, best - start));
				start = best + 1;
			} while (start < line.size());
			para = para_end + 1;
		}
		return lines;
	}
} // namespace

static_assert(SettingsMenu::WIDTH == GameListMenu::WIDTH && SettingsMenu::HEIGHT == GameListMenu::HEIGHT,
	"the menus share one panel layer");

VrMenu::VrMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls)
	: m_settings(settings)
	, m_web(web)
	, m_controls(controls)
{
}

VrMenu::~VrMenu()
{
	Destroy();
}

static bool LoadFont(Font* font, const char* name)
{
	const std::string path = EmuFolders::GetOverridableResourcePath(fmt::format("fonts" FS_OSPATH_SEPARATOR_STR "{}", name));
	std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
	if (!data || !font->Load(std::move(*data)))
	{
		Console.ErrorFmt("Menu: cannot load the font {}", path);
		return false;
	}
	return true;
}

bool VrMenu::Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index)
{
	m_xr = &xr;
	if (!LoadFont(&m_text, "Roboto-Regular.ttf") || !LoadFont(&m_icons, "fa-solid-900.ttf"))
		return false;
	// The groups' layouts: each button's place below its group's first. At the top, the light gun's
	// two come after a gap below the game list.
	const float spacing = BUTTON_SIZE + BUTTON_GAP;
	static constexpr int group_of[NUM_BUTTONS] = {0, 0, 1, 0, 0, 2, 2}; // GEAR ... POWER
	static constexpr float slot_of[NUM_BUTTONS] = {0, 1, 0, 2, 3, 0, 1}; // in spacings
	for (int b = 0; b < NUM_BUTTONS; b++)
	{
		Button& button = m_buttons[b];
		button.group = group_of[b];
		button.offset = slot_of[b] * spacing + (b == GUN_MODE || b == GUNS ? BUTTON_GROUP_GAP : 0.0f);
		m_groups[button.group].height = std::max(m_groups[button.group].height, button.offset + BUTTON_SIZE);
	}
	// The atlas: the button groups side by side; right of them the tooltip, the notice and the Wi-Fi
	// line one above the other, then the beam, the dot and the pad; the messages below all of it.
	const auto place = [](OverlayLayer::Part& part, int x, int y, int w, int h) {
		part.rect = {{x, y}, {w, h}};
		part.size = {w * METERS_PER_PX, h * METERS_PER_PX};
		return y + h + ATLAS_GUTTER_PX; // the next part's y below it
	};
	int x = 0, groups_bottom = 0;
	for (ButtonGroup& group : m_groups)
	{
		const int px = static_cast<int>(std::lround(group.height / METERS_PER_PX));
		groups_bottom = std::max(groups_bottom, place(group.part, x, 0, BUTTON_PX, px));
		group.canvas.Resize(BUTTON_PX, px);
		x += BUTTON_PX + ATLAS_GUTTER_PX;
	}
	int y = place(m_tooltip, x, 0, TOOLTIP_W_PX, TOOLTIP_H_PX);
	y = place(m_notice, x, y, TOOLTIP_W_PX, TOOLTIP_H_PX);
	y = place(m_web_line, x, y, TOOLTIP_W_PX, WEB_LINE_H_PX);
	place(m_beam, x, y, BEAM_W_PX, BEAM_H_PX);
	place(m_cursor, x + BEAM_W_PX + ATLAS_GUTTER_PX, y, CURSOR_PX, CURSOR_PX);
	y = place(m_pad, x + BEAM_W_PX + CURSOR_PX + 2 * ATLAS_GUTTER_PX, y, 4, 4) - 4 + CURSOR_PX;
	const int messages_y = std::max(groups_bottom, y);
	place(m_messages, 0, messages_y, MESSAGES_W_PX, MESSAGES_H_PX);
	const int atlas_w = std::max(x + TOOLTIP_W_PX, MESSAGES_W_PX), atlas_h = messages_y + MESSAGES_H_PX;
	if (!m_panel.Create(xr, device, gpu, queue, queue_family_index, SettingsMenu::WIDTH, SettingsMenu::HEIGHT) ||
		!m_atlas.Create(xr, device, gpu, queue, queue_family_index, atlas_w, atlas_h))
	{
		Destroy();
		return false;
	}
	m_atlas_canvas.Resize(atlas_w, atlas_h);
	m_tooltip_canvas.Resize(TOOLTIP_W_PX, TOOLTIP_H_PX);
	m_notice_canvas.Resize(TOOLTIP_W_PX, TOOLTIP_H_PX);
	m_web_line_canvas.Resize(TOOLTIP_W_PX, WEB_LINE_H_PX);
	m_messages_canvas.Resize(MESSAGES_W_PX, MESSAGES_H_PX);
	m_pad.size = {0.001f, 0.001f};
	m_pad.pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, -50.0f, 0.0f}}; // far below, out of sight
	m_settings_menu = std::make_unique<SettingsMenu>(m_settings, m_web, m_controls, m_text, m_icons);
	m_game_list = std::make_unique<GameListMenu>(Path::Combine(EmuFolders::Settings, "favorites.txt"), m_text, m_icons);

	// The pointer's dot: white with a dark rim, so it shows on light and dark pictures alike.
	Canvas canvas;
	canvas.Resize(CURSOR_PX, CURSOR_PX);
	canvas.FillCircle(0.5f * CURSOR_PX, 0.5f * CURSOR_PX, 0.5f * CURSOR_PX - 2.0f, Rgb(0x000000, 140));
	canvas.FillCircle(0.5f * CURSOR_PX, 0.5f * CURSOR_PX, 0.5f * CURSOR_PX - 9.0f, Rgb(0xffffff));
	Put(m_cursor, canvas);
	m_cursor.size = {CURSOR_SIZE, CURSOR_SIZE};

	// The beam fades in from the controller towards the dot.
	canvas.Resize(BEAM_W_PX, BEAM_H_PX);
	for (int x = 0; x < BEAM_W_PX; x++)
	{
		const float alpha = 0.1f + 0.6f * static_cast<float>(x) / static_cast<float>(BEAM_W_PX - 1);
		canvas.FillRect({static_cast<float>(x), 3.0f, 1.0f, 10.0f}, Theme::accent.WithAlpha(alpha * 0.5f));
		canvas.FillRect({static_cast<float>(x), 6.0f, 1.0f, 4.0f}, Rgb(0xffffff).WithAlpha(alpha));
	}
	Put(m_beam, canvas);
	m_tooltip_text.clear();

	m_buttons[GEAR].icon = GEAR_ICON;
	m_buttons[GAMES].icon = GAMES_ICON;
	m_buttons[MODE_3D].lit = m_settings.Get().stereo;
	m_buttons[MODE_3D].icon = m_buttons[MODE_3D].lit ? VOLUMETRIC_ICON : FLAT_ICON;
	m_buttons[GUN_MODE].icon = GUN_MODE_ICON;
	m_buttons[GUNS].icon = m_guns > 1 ? TWO_GUNS_ICON : ONE_GUN_ICON;
	m_buttons[GUN_MODE].lit = m_gun_on;
	m_buttons[GUN_MODE].disabled = !m_has_game;
	m_buttons[GUNS].disabled = !m_gun_on;
	m_buttons[PAUSE].icon = m_user_paused ? PLAY_ICON : PAUSE_ICON;
	m_buttons[PAUSE].lit = m_user_paused;
	m_buttons[PAUSE].disabled = !m_has_game;
	m_buttons[POWER].icon = POWER_ICON;
	m_buttons[POWER].danger = true;
	m_created = true;
	for (int g = 0; g < NUM_GROUPS; g++)
		DrawGroup(g);
	// Uploaded now, so the pad is there from the first frame.
	m_atlas.Upload(m_atlas_canvas);
	m_atlas_dirty = false;
	return true;
}

void VrMenu::Put(const OverlayLayer::Part& part, const Canvas& canvas)
{
	m_atlas_canvas.Copy(canvas, part.rect.offset.x, part.rect.offset.y);
	m_atlas_dirty = true;
}

void VrMenu::Destroy()
{
	m_created = false;
	m_show_tooltip = false;
	m_atlas.Destroy();
	m_atlas_dirty = false;
	m_web_line_text.clear();
	m_messages_text.clear();
	m_panel.Destroy();
	m_game_list.reset();
	m_settings_menu.reset();
}

void VrMenu::DrawButton(const Button& button)
{
	DrawGroup(button.group);
}

void VrMenu::DrawGroup(int group)
{
	if (!m_created)
		return;
	Canvas& c = m_groups[group].canvas;
	c.Clear();
	for (const Button& button : m_buttons)
	{
		if (button.group != group || !button.shown)
			continue;
		const float top = std::round(button.offset / METERS_PER_PX);
		const float mid = 0.5f * BUTTON_PX;
		const float cx = mid, cy = top + mid;
		const Rect all{0, top, BUTTON_PX, BUTTON_PX};
		if (button.disabled)
		{
			c.FillCircle(cx, cy, mid - 2.0f, Theme::panel.WithAlpha(0.35f));
			c.DrawIcon(m_icons, 56.0f, all, button.icon, Theme::text.WithAlpha(0.3f));
		}
		else if (button.danger)
		{
			c.FillCircle(cx, cy, mid - 2.0f, button.hovered ? Theme::danger_hover : Theme::danger.WithAlpha(0.85f));
			c.DrawIcon(m_icons, button.hovered ? 64.0f : 60.0f, all, button.icon, Theme::text);
		}
		else if (button.hovered)
		{
			c.FillCircle(cx, cy, mid - 2.0f, button.lit ? Theme::accent_hover : Theme::accent);
			c.DrawIcon(m_icons, 64.0f, all, button.icon, Theme::text);
		}
		else if (button.lit)
		{
			c.FillCircle(cx, cy, mid - 2.0f, Theme::accent.WithAlpha(0.85f));
			c.DrawIcon(m_icons, 60.0f, all, button.icon, Theme::text);
		}
		else
		{
			c.FillCircle(cx, cy, mid - 2.0f, Theme::panel.WithAlpha(0.6f));
			c.DrawIcon(m_icons, 60.0f, all, button.icon, Theme::text.WithAlpha(0.8f));
		}
	}
	Put(m_groups[group].part, c);
}

std::string VrMenu::TooltipText(int button) const
{
	switch (button)
	{
		case GEAR:
			return m_open == Panel::Settings && m_has_game ? "Back to the game" : "Settings";
		case GAMES:
			return m_open == Panel::Games && m_has_game ? "Back to the game" : "Game list";
		case MODE_3D:
			return m_settings.Get().stereo ? "3D: volumetric" : "3D: flat";
		case GUN_MODE:
			if (!m_has_game)
				return "Light gun mode (start a game first)";
			return m_gun_on ? "Light gun mode: on" : "Light gun mode: off";
		case GUNS:
			if (!m_gun_on)
				return "One or two light guns (light gun mode is off)";
			return m_guns > 1 ? "Two light guns: two players" : "One light gun: one player";
		case PAUSE:
			if (!m_has_game)
				return "Pause (start a game first)";
			return m_user_paused ? "Paused: click to resume" : "Pause the game";
		case POWER:
			return "Quit ARMSX2 VR";
		default:
			return {};
	}
}

void VrMenu::ShowTooltip(int button, float button_x)
{
	m_show_tooltip = button >= 0;
	if (!m_show_tooltip)
		return;
	std::string text = TooltipText(button);
	if (text != m_tooltip_text)
	{
		// A pill as wide as the text, at the layer's left edge, beside the button.
		Canvas& c = m_tooltip_canvas;
		c.Clear();
		const float text_w = m_text.Measure(text, TOOLTIP_TEXT_SIZE);
		const float pill_w = std::min(text_w + 2.0f * TOOLTIP_PAD_PX, static_cast<float>(TOOLTIP_W_PX));
		const Rect pill{0.0f, 0.0f, pill_w, static_cast<float>(TOOLTIP_H_PX)};
		c.FillRect(pill, Theme::panel.WithAlpha(0.9f), 0.5f * TOOLTIP_H_PX);
		c.StrokeRect(pill, Theme::border, 0.5f * TOOLTIP_H_PX, 2.0f);
		c.SetClip(pill.Inset(TOOLTIP_PAD_PX, 0.0f));
		c.DrawText(m_text, TOOLTIP_TEXT_SIZE, TOOLTIP_PAD_PX, 0.5f * (TOOLTIP_H_PX - m_text.LineHeight(TOOLTIP_TEXT_SIZE)),
			text, Theme::text);
		c.ResetClip();
		Put(m_tooltip, c);
		m_tooltip_text = std::move(text);
	}
	m_tooltip.pose = m_screen;
	m_tooltip.pose.position = Transform(m_screen,
		{button_x + 0.5f * BUTTON_SIZE + TOOLTIP_GAP + 0.5f * m_tooltip.size.width, m_buttons[button].y, 0.0f});
}

void VrMenu::UpdateModeButton()
{
	Button& button = m_buttons[MODE_3D];
	const bool volumetric = m_settings.Get().stereo;
	if (button.lit == volumetric)
		return;
	button.lit = volumetric;
	button.icon = volumetric ? VOLUMETRIC_ICON : FLAT_ICON;
	DrawButton(button);
}

void VrMenu::UpdatePauseButton()
{
	Button& button = m_buttons[PAUSE];
	const uint32_t icon = m_user_paused ? PLAY_ICON : PAUSE_ICON;
	if (button.lit == m_user_paused && button.icon == icon && button.disabled == !m_has_game)
		return;
	button.lit = m_user_paused;
	button.icon = icon;
	button.disabled = !m_has_game;
	if (m_created)
		DrawButton(button);
}

void VrMenu::OnGameChanged()
{
	if (!m_user_paused)
		return;
	m_user_paused = false;
	if (m_open == Panel::None)
		Emu::SetPaused(false);
	UpdatePauseButton();
}

void VrMenu::UpdateWebLine()
{
	const WebServer::Status web = m_web.GetStatus();
	const std::string text = web.running && !web.url.empty() ? "Wi-Fi server ON at " + web.url : std::string();
	const bool hovered = !text.empty() && m_web_line_hovered;
	if (text != m_web_line_text || (hovered != m_web_line_drawn_hovered && !text.empty()))
	{
		m_web_line_text = text;
		m_web_line_drawn_hovered = hovered;
		if (!text.empty())
		{
			// A pill as wide as the text, centred in the layer; lit while pointed at.
			Canvas& c = m_web_line_canvas;
			c.Clear();
			const float text_w = m_text.Measure(text, WEB_LINE_TEXT_SIZE);
			const float pill_w = std::min(text_w + 2.0f * TOOLTIP_PAD_PX, static_cast<float>(TOOLTIP_W_PX));
			const Rect pill{0.5f * (TOOLTIP_W_PX - pill_w), 0.0f, pill_w, static_cast<float>(WEB_LINE_H_PX)};
			c.FillRect(pill, hovered ? Theme::accent : Theme::panel.WithAlpha(0.85f), 0.5f * WEB_LINE_H_PX);
			c.StrokeRect(pill, hovered ? Theme::accent_hover : Theme::border, 0.5f * WEB_LINE_H_PX, 2.0f);
			c.SetClip(pill.Inset(TOOLTIP_PAD_PX, 0.0f));
			c.DrawText(m_text, WEB_LINE_TEXT_SIZE, pill.x + TOOLTIP_PAD_PX,
				0.5f * (WEB_LINE_H_PX - m_text.LineHeight(WEB_LINE_TEXT_SIZE)), text, Theme::text);
			c.ResetClip();
			Put(m_web_line, c);
			m_web_line_width = pill_w * METERS_PER_PX;
		}
	}
	m_web_line_y = -0.5f * m_screen_height - TOOLTIP_GAP - 0.5f * WEB_LINE_H_PX * METERS_PER_PX;
	m_web_line.pose = m_screen;
	m_web_line.pose.position = Transform(m_screen, {0.0f, m_web_line_y, 0.0f});
}

void VrMenu::UpdateMessages()
{
	std::vector<std::string> texts = Emu::GetMessages();
	if (texts != m_messages_text)
	{
		m_messages_text = std::move(texts);
		if (!m_messages_text.empty())
		{
			// Each message a box as wide as its text, centred; the newest at the bottom, nearest the
			// screen, and the oldest left out when they do not all fit.
			Canvas& c = m_messages_canvas;
			c.Clear();
			const float line_h = std::round(m_text.LineHeight(MESSAGE_TEXT_SIZE) * 1.1f);
			const float max_text_w = MESSAGES_W_PX - 2.0f * TOOLTIP_PAD_PX;
			float bottom = static_cast<float>(MESSAGES_H_PX);
			for (auto it = m_messages_text.rbegin(); it != m_messages_text.rend(); ++it)
			{
				const std::vector<std::string_view> lines = WrapLines(m_text, *it, MESSAGE_TEXT_SIZE, max_text_w);
				const float box_h = lines.size() * line_h + 2.0f * MESSAGE_PAD_PX;
				if (bottom - box_h < 0.0f)
					break;
				float text_w = 0.0f;
				for (std::string_view line : lines)
					text_w = std::max(text_w, m_text.Measure(line, MESSAGE_TEXT_SIZE));
				const float box_w = std::min(text_w, max_text_w) + 2.0f * TOOLTIP_PAD_PX;
				const Rect box{0.5f * (MESSAGES_W_PX - box_w), bottom - box_h, box_w, box_h};
				const float radius = std::min(0.5f * box_h, 0.5f * TOOLTIP_H_PX);
				c.FillRect(box, Theme::panel.WithAlpha(0.9f), radius);
				c.StrokeRect(box, Theme::border, radius, 2.0f);
				c.SetClip(box.Inset(TOOLTIP_PAD_PX, 0.0f));
				float y = box.y + MESSAGE_PAD_PX;
				for (std::string_view line : lines)
				{
					c.DrawText(m_text, MESSAGE_TEXT_SIZE, box.x + TOOLTIP_PAD_PX, y, line, Theme::text);
					y += line_h;
				}
				c.ResetClip();
				bottom = box.y - MESSAGE_GAP_PX;
			}
			Put(m_messages, c);
		}
	}
	// Above the screen, and above an open panel that stands higher.
	float top = 0.5f * m_screen_height;
	if (m_open != Panel::None)
		top = std::max(top, 0.5f * m_panel.Size().height);
	XrPosef pose = m_screen;
	pose.position = Transform(m_screen, {0.0f, top + TOOLTIP_GAP + 0.5f * m_messages.size.height, 0.0f});
	m_messages.pose = pose;
}

void VrMenu::ShowNotice(std::string text, float seconds)
{
	if (!m_created)
		return;
	// A pill as wide as the text, centred in the layer.
	Canvas& c = m_notice_canvas;
	c.Clear();
	const float text_w = m_text.Measure(text, TOOLTIP_TEXT_SIZE);
	const float pill_w = std::min(text_w + 2.0f * TOOLTIP_PAD_PX, static_cast<float>(TOOLTIP_W_PX));
	const Rect pill{0.5f * (TOOLTIP_W_PX - pill_w), 0.0f, pill_w, static_cast<float>(TOOLTIP_H_PX)};
	c.FillRect(pill, Theme::panel.WithAlpha(0.9f), 0.5f * TOOLTIP_H_PX);
	c.StrokeRect(pill, Theme::accent, 0.5f * TOOLTIP_H_PX, 2.0f);
	c.SetClip(pill.Inset(TOOLTIP_PAD_PX, 0.0f));
	c.DrawText(m_text, TOOLTIP_TEXT_SIZE, pill.x + TOOLTIP_PAD_PX, 0.5f * (TOOLTIP_H_PX - m_text.LineHeight(TOOLTIP_TEXT_SIZE)),
		text, Theme::text);
	c.ResetClip();
	Put(m_notice, c);
	m_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
	Console.WriteLnFmt("Menu: notice \"{}\"", text);
}

void VrMenu::SetGunState(bool has_game, bool on, bool chosen, int guns)
{
	if (m_settings_menu)
		m_settings_menu->SetGunMode(has_game, on, chosen);
	if (has_game == m_has_game && on == m_gun_on && guns == m_guns)
		return;
	m_has_game = has_game;
	UpdatePauseButton();
	m_gun_on = on;
	m_guns = guns;
	m_buttons[GUN_MODE].lit = on;
	m_buttons[GUN_MODE].disabled = !has_game;
	m_buttons[GUNS].disabled = !on;
	m_buttons[GUNS].icon = guns > 1 ? TWO_GUNS_ICON : ONE_GUN_ICON;
	if (m_created)
	{
		DrawButton(m_buttons[GUN_MODE]);
		DrawButton(m_buttons[GUNS]);
	}
}

VrMenu::Hit VrMenu::HitPlane(const XrPosef& quad, const XrPosef& aim)
{
	Hit hit;
	const XrVector3f dir = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
	const XrQuaternionf to_quad = Conjugate(quad.orientation);
	const XrVector3f o = Rotate(to_quad, aim.position - quad.position);
	const XrVector3f d = Rotate(to_quad, dir);
	// Only from in front of the plane (+Z side), pointing at it, and not too far.
	if (o.z <= 0.0f || d.z >= -1e-4f)
		return hit;
	const float t = -o.z / d.z;
	if (t > POINTER_REACH)
		return hit;
	hit.hit = true;
	hit.x = o.x + t * d.x;
	hit.y = o.y + t * d.y;
	hit.point = aim.position + dir * t;
	hit.normal = Rotate(quad.orientation, {0.0f, 0.0f, 1.0f});
	return hit;
}

void VrMenu::AnchorPanel()
{
	// Sized from the screen, but readable on a small one and not huge on a big one. It stays put
	// while open, so a slider that resizes or moves the screen does not move it under the pointer.
	const float width = std::clamp(0.8f * m_screen_width, 1.4f, 2.4f);
	XrPosef pose = m_screen;
	pose.position = Transform(m_screen, {0.0f, 0.0f, PANEL_LIFT});
	m_panel.SetPose(pose);
	m_panel.SetSize(width, width * static_cast<float>(SettingsMenu::HEIGHT) / static_cast<float>(SettingsMenu::WIDTH));
}

void VrMenu::Open(Panel panel)
{
	if (m_open == Panel::None && m_pauses_game)
		Emu::SetPaused(true);
	m_on_button[0] = m_on_button[1] = -1;
	m_open = panel;
	UpdateShownButtons();
	if (panel == Panel::Settings)
	{
		m_settings_menu->SetFrameControllers(m_xr->HasFrameControllers());
		m_settings_menu->Refresh();
	}
	else
		m_game_list->Refresh();
	AnchorPanel();
	Console.WriteLnFmt("Menu: {} open", panel == Panel::Settings ? "settings" : "game list");
}

void VrMenu::OpenGameList()
{
	if (m_created && m_open != Panel::Games)
		Open(Panel::Games);
}

void VrMenu::Close()
{
	m_open = Panel::None;
	m_on_button[0] = m_on_button[1] = -1;
	UpdateShownButtons();
	if (m_pauses_game && !m_user_paused)
		Emu::SetPaused(false);
	Console.WriteLn("Menu: closed");
}

void VrMenu::UpdateShownButtons()
{
	// Beside an open panel, the buttons that go to the panels (and quit), the open one's lit.
	for (int b = 0; b < NUM_BUTTONS; b++)
	{
		m_buttons[b].shown = m_open == Panel::None || b == GEAR || b == GAMES || b == POWER;
		m_buttons[b].hovered = false;
	}
	m_buttons[GEAR].lit = m_open == Panel::Settings;
	m_buttons[GAMES].lit = m_open == Panel::Games;
	m_show_tooltip = false;
	for (int g = 0; g < NUM_GROUPS; g++)
		DrawGroup(g);
}

float VrMenu::ButtonX() const
{
	const float width = m_open != Panel::None ? std::max(m_screen_width, m_panel.Size().width) : m_screen_width;
	return 0.5f * width + BUTTON_MARGIN + 0.5f * BUTTON_SIZE;
}

void VrMenu::PlacePointer(XrTime time, int hand, const XrPosef& aim, const Hit& hit, const XrQuaternionf& facing)
{
	if (m_pointers[hand].gun)
		return; // the gun draws its beam and dot
	m_show_pointer = true;
	m_show_cursor = hit.hit;
	XrPosef cursor;
	cursor.position = hit.point + hit.normal * 0.004f;
	cursor.orientation = facing;
	m_cursor.pose = cursor;

	// A thin quad from the controller to the dot (or into the room when it hits nothing), turned
	// about its length to face the head.
	const XrVector3f dir = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
	const XrVector3f start = aim.position + dir * 0.02f;
	const float length = hit.hit ? std::sqrt(Dot(hit.point - start, hit.point - start)) : FREE_BEAM_LENGTH;
	const XrVector3f centre = start + dir * (0.5f * length);
	XrPosef head;
	XrVector3f to_head = m_xr->LocateHead(time, &head) ? head.position - centre : Rotate(aim.orientation, {0.0f, 1.0f, 0.0f});
	XrVector3f z = Normalize(to_head - dir * Dot(to_head, dir));
	if (Dot(z, z) < 0.5f)
		z = Normalize(Cross(dir, {0.0f, 1.0f, 0.0f}));
	m_beam.pose = {QuatFromAxes(dir, Cross(z, dir), z), centre};
	m_beam.size = {length, BEAM_WIDTH};
}

VrMenu::Result VrMenu::Update(XrTime time, const XrApp::Input& input, const std::array<Pointer, 2>& pointers,
	const XrPosef& screen, float screen_width, float screen_height)
{
	Result result;
	m_show_pointer = false;
	m_show_cursor = false;
	m_show_tooltip = false;
	if (!m_created)
		return result;
	m_screen = screen;
	m_screen_width = screen_width;
	m_screen_height = screen_height;
	UpdateWebLine();
	UpdateMessages();
	// The notice below the screen, and below the Wi-Fi line when it shows.
	const float notice_top = m_web_line_text.empty() ? -0.5f * screen_height : m_web_line_y - 0.5f * m_web_line.size.height;
	m_notice.pose = screen;
	m_notice.pose.position = Transform(screen, {0.0f, notice_top - TOOLTIP_GAP - 0.5f * m_notice.size.height, 0.0f});
	m_pointers = pointers;

	// A game from the list that would not start: back to the list, which says why.
	if (Emu::TakeBootFailure())
		OpenGameList();

	// Triggers as buttons, with some hysteresis.
	bool pressed[2], released[2];
	for (int h = 0; h < 2; h++)
	{
		const bool down = input.trigger[h] > (m_trigger_down[h] ? 0.35f : 0.65f);
		pressed[h] = down && !m_trigger_down[h];
		released[h] = !down && m_trigger_down[h];
		m_trigger_down[h] = down;
		if (!down)
			m_swallow[h] = false;
	}

	// The buttons, one above the other just outside the screen's right edge: the top group from level
	// with its top, the 3D button at mid height, the bottom group down to level with its bottom; on a
	// small screen each is kept below the one above.
	const auto below = [](const ButtonGroup& above) { return above.top_y - above.height - BUTTON_GAP - BUTTON_GROUP_GAP; };
	m_groups[0].top_y = 0.5f * screen_height - 0.5f * BUTTON_SIZE;
	m_groups[1].top_y = std::min(0.0f, below(m_groups[0]));
	m_groups[2].top_y = std::min(-0.5f * screen_height - 0.5f * BUTTON_SIZE + m_groups[2].height, below(m_groups[1]));
	for (Button& button : m_buttons)
		button.y = m_groups[button.group].top_y - button.offset;
	UpdateModeButton();
	if (m_open != Panel::None && m_reanchor)
		AnchorPanel();
	m_reanchor = false;
	float button_x = ButtonX();

	// Where each controller points: on the open panel's plane, else on the screen's. Without a
	// menu, the pointer is out while a controller points anywhere but at the game.
	XrPosef aim[2] = {{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}};
	Hit hits[2];
	Hit button_hits[2]; // on the screen's plane, where the buttons are
	bool pointing[2] = {}; // at the panel, or away from the game
	const auto on_panel = [&](const Hit& hit) {
		return hit.hit && std::abs(hit.x) <= 0.5f * m_panel.Size().width && std::abs(hit.y) <= 0.5f * m_panel.Size().height;
	};
	for (int h = 0; h < 2; h++)
	{
		if (!pointers[h].valid)
			continue;
		aim[h] = pointers[h].pose;
		button_hits[h] = HitPlane(screen, aim[h]);
		if (m_open != Panel::None)
		{
			hits[h] = HitPlane(m_panel.Pose(), aim[h]);
			pointing[h] = on_panel(hits[h]);
		}
		else
		{
			hits[h] = button_hits[h];
			pointing[h] = !hits[h].hit || std::abs(hits[h].x) > 0.5f * screen_width ||
			              std::abs(hits[h].y) > 0.5f * screen_height;
		}
	}
	// The pointer is the controller that pulled its trigger last: the other one takes over only by
	// pulling its own.
	for (int h = 0; h < 2; h++)
	{
		if (pressed[h])
			m_hand = h;
	}
	const int hand = m_hand;

	// The button each hand points at, if any (not through the open panel): the nearest shown one,
	// within its circle, or within the hold radius of the one it was already on.
	int on_button[2] = {-1, -1};
	for (int h = 0; h < 2; h++)
	{
		const Hit& hit = button_hits[h];
		int nearest = -1;
		float distance = 0.0f;
		for (int b = 0; b < NUM_BUTTONS && pointers[h].valid && hit.hit && !(m_open != Panel::None && pointing[h]); b++)
		{
			if (!m_buttons[b].shown)
				continue;
			const float d = std::hypot(hit.x - button_x, hit.y - m_buttons[b].y);
			if (nearest < 0 || d < distance)
			{
				nearest = b;
				distance = d;
			}
		}
		const float reach = nearest == m_on_button[h] ? BUTTON_HOLD_RADIUS : 0.5f * BUTTON_SIZE;
		on_button[h] = nearest >= 0 && distance < reach ? nearest : -1;
		m_on_button[h] = on_button[h];
	}
	// The Wi-Fi line below the screen, taken like a button.
	bool on_web[2] = {};
	for (int h = 0; h < 2; h++)
	{
		const Hit& hit = button_hits[h];
		on_web[h] = !m_web_line_text.empty() && on_button[h] < 0 && pointers[h].valid && hit.hit &&
		            !(m_open != Panel::None && pointing[h]) && std::abs(hit.x) <= 0.5f * m_web_line_width &&
		            std::abs(hit.y - m_web_line_y) <= 0.5f * WEB_LINE_H_PX * METERS_PER_PX;
	}
	m_web_line_hovered = false;

	// While a control is being mapped, a trigger press is the input to map: not a click on a button
	// or the panel, nor a click away that closes the menu.
	const bool mapping = m_open == Panel::Settings && m_settings_menu->IsMappingControl();
	bool just_opened = false;
	if (!mapping)
	{
		const int hovered = on_button[hand];
		for (int b = 0; b < NUM_BUTTONS; b++)
		{
			Button& button = m_buttons[b];
			const bool now = b == hovered && !button.disabled;
			if (now != button.hovered)
			{
				button.hovered = now;
				DrawButton(button);
			}
		}
		// Disabled buttons too: their tooltip says why.
		ShowTooltip(hovered, button_x);
		m_web_line_hovered = on_web[hand];
		// Only on the buttons themselves: a trigger aimed off the screen still reaches the game (light
		// gun games reload that way).
		for (int h = 0; h < 2; h++)
		{
			if (on_button[h] >= 0 || on_web[h])
			{
				result.block_trigger[h] = true;
				result.on_ui[h] = true;
				result.ui_point[h] = button_hits[h].point;
			}
		}
		if (hovered >= 0 && pressed[hand] && !m_swallow[hand] && !m_buttons[hovered].disabled)
		{
			if (hovered == GUN_MODE)
			{
				result.gun_mode = m_gun_on ? SettingsMenu::GunModeRequest::Off : SettingsMenu::GunModeRequest::On;
				m_swallow[hand] = true; // the click is not a shot when the gun comes on
			}
			else if (hovered == GUNS)
			{
				result.toggle_guns = true;
				m_swallow[hand] = true;
			}
			else if (hovered == MODE_3D)
			{
				VrSettings vr = m_settings.Get();
				vr.stereo = !vr.stereo;
				m_settings.Set(vr);
				UpdateModeButton();
				m_swallow[hand] = true;
			}
			else if (hovered == PAUSE)
			{
				m_user_paused = !m_user_paused;
				Emu::SetPaused(m_user_paused);
				Console.WriteLnFmt("Menu: game {}", m_user_paused ? "paused" : "resumed");
				UpdatePauseButton();
				m_swallow[hand] = true;
			}
			else if (hovered != POWER && m_open == (hovered == GAMES ? Panel::Games : Panel::Settings))
			{
				// The open panel's own button: back to the game, if one runs.
				if (m_has_game)
					Close();
				m_swallow[hand] = true;
			}
			else
			{
				// The gear, the game list, or the power button's confirmation: that panel, in place of
				// the open one.
				Open(hovered == GAMES ? Panel::Games : Panel::Settings);
				if (hovered == POWER)
					m_settings_menu->ShowQuitDialog();
				ShowTooltip(-1, button_x);
				just_opened = true;
				hits[hand] = HitPlane(m_panel.Pose(), aim[hand]);
				pointing[hand] = on_panel(hits[hand]);
				on_button[hand] = -1;
			}
		}
		else if (on_web[hand] && pressed[hand] && !m_swallow[hand])
		{
			// The Wi-Fi line: the settings, on the Wi-Fi Transfer page.
			Open(Panel::Settings);
			m_settings_menu->ShowTransferPage();
			m_web_line_hovered = false;
			just_opened = true;
			hits[hand] = HitPlane(m_panel.Pose(), aim[hand]);
			pointing[hand] = on_panel(hits[hand]);
		}
		else if (m_open == Panel::None && pointing[hand] && pointers[hand].valid)
			PlacePointer(time, hand, aim[hand], hits[hand], screen.orientation);
	}

	if (m_open != Panel::None)
	{
		result.open = true;
		const bool hand_on_panel = pointing[hand];
		const Hit& panel_hit = hits[hand];
		for (int h = 0; h < 2; h++)
		{
			if (pointing[h])
			{
				result.on_ui[h] = true;
				result.ui_point[h] = hits[h].point;
			}
		}
		GuiInput gui;
		gui.pointer = hand_on_panel;
		gui.x = (panel_hit.x / m_panel.Size().width + 0.5f) * static_cast<float>(SettingsMenu::WIDTH);
		gui.y = (0.5f - panel_hit.y / m_panel.Size().height) * static_cast<float>(SettingsMenu::HEIGHT);
		const bool usable = !just_opened && !m_swallow[hand] && !mapping;
		gui.down = usable && m_trigger_down[hand];
		gui.pressed = usable && pressed[hand];
		gui.released = released[hand];

		// Either thumbstick scrolls a list (a stick being mapped is an input, not a scroll).
		const ControllerState& c = input.controller;
		const float stick = std::abs(c.left_stick.y) > std::abs(c.right_stick.y) ? c.left_stick.y : c.right_stick.y;
		const float scroll = std::abs(stick) > 0.2f && !mapping ? -stick * SCROLL_SPEED : 0.0f;
		bool close = false;
		if (m_open == Panel::Settings)
		{
			m_settings_menu->SetControllerState(input.controller);
			if (scroll != 0.0f)
				m_settings_menu->Scroll(scroll);
			if (m_settings_menu->Update(gui) || just_opened)
				m_panel.Upload(m_settings_menu->GetCanvas());
			result.recenter = m_settings_menu->TakeRecenterRequest(); // the panel follows (Reanchor)
			result.gun_mode = m_settings_menu->TakeGunModeRequest();
			result.gun_action = m_settings_menu->TakeGunAction();
			close = m_settings_menu->TakeCloseRequest();
			// The trigger that was just mapped is still held: not a click until let go.
			if (m_settings_menu->TakeMappingDone())
			{
				for (int h = 0; h < 2; h++)
					m_swallow[h] = m_trigger_down[h];
			}
		}
		else
		{
			if (scroll != 0.0f)
				m_game_list->Scroll(scroll);
			if (m_game_list->Update(gui) || just_opened)
				m_panel.Upload(m_game_list->GetCanvas());
			close = m_game_list->TakeCloseRequest();
			// Nothing to play yet: on to the Wi-Fi transfer settings, which come back here. The panel
			// shows the list until they have drawn, next frame.
			if (!close && m_game_list->TakeWifiRequest())
			{
				Open(Panel::Settings);
				m_settings_menu->ShowTransferPage();
				just_opened = true;
			}
		}
		if (close || (usable && pressed[hand] && !hand_on_panel && on_button[hand] < 0))
		{
			// With no game, the settings go back to the game list (the welcome screen).
			if (m_open == Panel::Settings && !m_has_game)
			{
				Open(Panel::Games);
				just_opened = true;
			}
			else
				Close();
		}
		else if (on_button[hand] >= 0)
		{
			if (pointers[hand].valid)
				PlacePointer(time, hand, aim[hand], button_hits[hand], screen.orientation);
		}
		else if (pointers[hand].valid) // no beam from a controller not tracked yet
			PlacePointer(time, hand, aim[hand], panel_hit, m_panel.Pose().orientation);
		if (just_opened || m_open == Panel::None)
		{
			// The trigger that opened or closed the menu stays the menu's until it is let go.
			for (int h = 0; h < 2; h++)
				m_swallow[h] = m_trigger_down[h];
		}
	}

	// The button groups, beside the screen (or the panel open now).
	button_x = ButtonX();
	for (ButtonGroup& group : m_groups)
	{
		XrPosef pose = screen;
		pose.position = Transform(screen, {button_x, group.top_y + 0.5f * BUTTON_SIZE - 0.5f * group.height, 0.0f});
		group.part.pose = pose;
	}

	for (int h = 0; h < 2; h++)
		result.block_trigger[h] |= m_swallow[h];
	return result;
}

void VrMenu::GetLayers(const XrCompositionLayerBaseHeader** slots)
{
	static_assert(NUM_LAYER_SLOTS == 7 + NUM_GROUPS,
		"the panel, the button groups, the tooltip, the notice, the Wi-Fi line, the messages, the beam and the dot");
	for (int i = 0; i < NUM_LAYER_SLOTS; i++)
		slots[i] = nullptr;
	if (!m_created)
		return;
	// What changed in the atlas since the last frame, in one upload.
	if (m_atlas_dirty)
	{
		m_atlas.Upload(m_atlas_canvas);
		m_atlas_dirty = false;
	}
	if (m_open != Panel::None && m_panel.HasImage())
		slots[0] = m_panel.Layer();
	if (!m_atlas.HasImage())
		return;
	for (const Button& button : m_buttons)
	{
		if (button.shown)
			slots[1 + button.group] = m_atlas.Layer(m_groups[button.group].part);
	}
	if (m_show_tooltip)
		slots[1 + NUM_GROUPS] = m_atlas.Layer(m_tooltip);
	if (std::chrono::steady_clock::now() < m_notice_until)
		slots[2 + NUM_GROUPS] = m_atlas.Layer(m_notice);
	if (!m_web_line_text.empty())
		slots[3 + NUM_GROUPS] = m_atlas.Layer(m_web_line);
	if (!m_messages_text.empty())
		slots[4 + NUM_GROUPS] = m_atlas.Layer(m_messages);
	if (m_show_pointer)
		slots[5 + NUM_GROUPS] = m_atlas.Layer(m_beam);
	if (m_show_cursor)
		slots[6 + NUM_GROUPS] = m_atlas.Layer(m_cursor);
}

bool VrMenu::GetPadLayer(XrCompositionLayerQuad* quad)
{
	if (!m_created || !m_atlas.HasImage())
		return false;
	*quad = *reinterpret_cast<const XrCompositionLayerQuad*>(m_atlas.Layer(m_pad));
	return true;
}
