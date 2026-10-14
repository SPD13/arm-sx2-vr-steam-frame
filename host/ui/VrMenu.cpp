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
	constexpr float METERS_PER_PX = BUTTON_SIZE / static_cast<float>(BUTTON_PX);
	constexpr float BUTTON_GROUP_GAP = 0.05f; // more between the menu buttons and the light gun's
	constexpr uint32_t GEAR_ICON = 0xf013;
	constexpr uint32_t GAMES_ICON = 0xf11b; // gamepad
	constexpr uint32_t VOLUMETRIC_ICON = 0xf1b2; // cube
	constexpr uint32_t FLAT_ICON = 0xf0c8; // square
	constexpr uint32_t GUN_MODE_ICON = 0xf05b; // crosshairs
	constexpr uint32_t ONE_GUN_ICON = 0xf007; // user
	constexpr uint32_t TWO_GUNS_ICON = 0xf0c0; // users
	constexpr uint32_t PAUSE_ICON = 0xf04c;
	constexpr uint32_t PLAY_ICON = 0xf04b;
	constexpr uint32_t POWER_ICON = 0xf011; // power-off
	constexpr float SCROLL_SPEED = 22.0f; // pixels per frame with a thumbstick pushed all the way
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
	// The groups' layouts: each button's place below its group's first, with a gap before the light gun's.
	const float spacing = BUTTON_SIZE + BUTTON_GAP;
	for (int b = 0; b < NUM_BUTTONS; b++)
	{
		Button& button = m_buttons[b];
		button.group = b >= PAUSE ? 1 : 0;
		const int first = button.group == 0 ? GEAR : PAUSE;
		button.offset = (b - first) * spacing + (b >= GUN_MODE && b < PAUSE ? BUTTON_GROUP_GAP : 0.0f);
		m_groups[button.group].height = button.offset + BUTTON_SIZE;
	}
	bool buttons_made = true;
	for (ButtonGroup& group : m_groups)
	{
		const int px = static_cast<int>(std::lround(group.height / METERS_PER_PX));
		buttons_made = buttons_made && group.layer.Create(xr, device, gpu, queue, queue_family_index, BUTTON_PX, px);
		group.canvas.Resize(BUTTON_PX, px);
		group.layer.SetSize(BUTTON_SIZE, group.height);
	}
	if (!m_panel.Create(xr, device, gpu, queue, queue_family_index, SettingsMenu::WIDTH, SettingsMenu::HEIGHT) ||
		!buttons_made || !m_cursor.Create(xr, device, gpu, queue, queue_family_index, CURSOR_PX, CURSOR_PX) ||
		!m_beam.Create(xr, device, gpu, queue, queue_family_index, BEAM_W_PX, BEAM_H_PX) ||
		!m_tooltip.Create(xr, device, gpu, queue, queue_family_index, TOOLTIP_W_PX, TOOLTIP_H_PX))
	{
		Destroy();
		return false;
	}
	m_settings_menu = std::make_unique<SettingsMenu>(m_settings, m_web, m_controls, m_text, m_icons);
	m_game_list = std::make_unique<GameListMenu>(Path::Combine(EmuFolders::Settings, "favorites.txt"), m_text, m_icons);

	// The pointer's dot: white with a dark rim, so it shows on light and dark pictures alike.
	Canvas canvas;
	canvas.Resize(CURSOR_PX, CURSOR_PX);
	canvas.FillCircle(0.5f * CURSOR_PX, 0.5f * CURSOR_PX, 0.5f * CURSOR_PX - 2.0f, Rgb(0x000000, 140));
	canvas.FillCircle(0.5f * CURSOR_PX, 0.5f * CURSOR_PX, 0.5f * CURSOR_PX - 9.0f, Rgb(0xffffff));
	m_cursor.Upload(canvas);
	m_cursor.SetSize(CURSOR_SIZE, CURSOR_SIZE);

	// The beam fades in from the controller towards the dot.
	canvas.Resize(BEAM_W_PX, BEAM_H_PX);
	for (int x = 0; x < BEAM_W_PX; x++)
	{
		const float alpha = 0.1f + 0.6f * static_cast<float>(x) / static_cast<float>(BEAM_W_PX - 1);
		canvas.FillRect({static_cast<float>(x), 3.0f, 1.0f, 10.0f}, Theme::accent.WithAlpha(alpha * 0.5f));
		canvas.FillRect({static_cast<float>(x), 6.0f, 1.0f, 4.0f}, Rgb(0xffffff).WithAlpha(alpha));
	}
	m_beam.Upload(canvas);

	m_tooltip_canvas.Resize(TOOLTIP_W_PX, TOOLTIP_H_PX);
	m_tooltip.SetSize(TOOLTIP_W_PX * METERS_PER_PX, TOOLTIP_H_PX * METERS_PER_PX);
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
	return true;
}

void VrMenu::Destroy()
{
	m_created = false;
	m_show_tooltip = false;
	m_tooltip.Destroy();
	m_beam.Destroy();
	m_cursor.Destroy();
	for (ButtonGroup& group : m_groups)
		group.layer.Destroy();
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
		if (button.group != group)
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
	m_groups[group].layer.Upload(c);
}

std::string VrMenu::TooltipText(int button) const
{
	switch (button)
	{
		case GEAR:
			return "Settings";
		case GAMES:
			return "Game list";
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
		m_tooltip.Upload(c);
		m_tooltip_text = std::move(text);
	}
	XrPosef pose = m_screen;
	pose.position = Transform(m_screen,
		{button_x + 0.5f * BUTTON_SIZE + TOOLTIP_GAP + 0.5f * m_tooltip.Size().width, m_buttons[button].y, 0.0f});
	m_tooltip.SetPose(pose);
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
	if (m_pauses_game && !m_user_paused)
		Emu::SetPaused(false);
	Console.WriteLn("Menu: closed");
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
	m_cursor.SetPose(cursor);

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
	m_beam.SetPose({QuatFromAxes(dir, Cross(z, dir), z), centre});
	m_beam.SetSize(length, BEAM_WIDTH);
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
	// with its top, the bottom one down to level with its bottom (kept below the top one on a small screen).
	const float button_x = 0.5f * screen_width + BUTTON_MARGIN + 0.5f * BUTTON_SIZE;
	m_groups[0].top_y = 0.5f * screen_height - 0.5f * BUTTON_SIZE;
	m_groups[1].top_y = std::min(-0.5f * screen_height - 0.5f * BUTTON_SIZE + m_groups[1].height,
		m_groups[0].top_y - m_groups[0].height - BUTTON_GAP - BUTTON_GROUP_GAP);
	for (Button& button : m_buttons)
		button.y = m_groups[button.group].top_y - button.offset;
	for (ButtonGroup& group : m_groups)
	{
		XrPosef pose = screen;
		pose.position = Transform(screen, {button_x, group.top_y + 0.5f * BUTTON_SIZE - 0.5f * group.height, 0.0f});
		group.layer.SetPose(pose);
	}
	UpdateModeButton();
	if (m_open != Panel::None && m_reanchor)
		AnchorPanel();
	m_reanchor = false;

	// Where each controller points: on the open panel's plane, else on the screen's. Without a
	// menu, the pointer is out while a controller points anywhere but at the game.
	XrPosef aim[2] = {{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}};
	Hit hits[2];
	bool pointing[2] = {}; // at the panel, or away from the game
	const auto on_panel = [&](const Hit& hit) {
		return hit.hit && std::abs(hit.x) <= 0.5f * m_panel.Size().width && std::abs(hit.y) <= 0.5f * m_panel.Size().height;
	};
	for (int h = 0; h < 2; h++)
	{
		if (!pointers[h].valid)
			continue;
		aim[h] = pointers[h].pose;
		if (m_open != Panel::None)
		{
			hits[h] = HitPlane(m_panel.Pose(), aim[h]);
			pointing[h] = on_panel(hits[h]);
		}
		else
		{
			hits[h] = HitPlane(screen, aim[h]);
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

	bool just_opened = false;
	if (m_open == Panel::None)
	{
		// The button each hand points at, if any: the nearest, within its circle, or within the hold
		// radius of the one it was already on.
		int on_button[2] = {-1, -1};
		for (int h = 0; h < 2; h++)
		{
			const Hit& hit = hits[h];
			int nearest = -1;
			float distance = 0.0f;
			for (int b = 0; b < NUM_BUTTONS && pointers[h].valid && hit.hit; b++)
			{
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
		// Only on the buttons themselves: a trigger aimed off the screen still reaches the game (light
		// gun games reload that way).
		for (int h = 0; h < 2; h++)
		{
			if (on_button[h] >= 0)
			{
				result.block_trigger[h] = true;
				result.on_ui[h] = true;
				result.ui_point[h] = hits[h].point;
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
			else
			{
				// The gear, the game list, or the power button's confirmation.
				Open(hovered == GAMES ? Panel::Games : Panel::Settings);
				if (hovered == POWER)
					m_settings_menu->ShowQuitDialog();
				for (Button& button : m_buttons)
				{
					if (button.hovered)
					{
						button.hovered = false;
						DrawButton(button);
					}
				}
				ShowTooltip(-1, button_x);
				just_opened = true;
				hits[hand] = HitPlane(m_panel.Pose(), aim[hand]);
				pointing[hand] = on_panel(hits[hand]);
			}
		}
		else if (pointing[hand] && pointers[hand].valid)
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
		// While a control is being mapped, a trigger press is the input to map: neither a click nor
		// a click away that closes the menu.
		const bool mapping = m_open == Panel::Settings && m_settings_menu->IsMappingControl();
		const bool usable = !just_opened && !m_swallow[hand] && !mapping;
		gui.down = usable && m_trigger_down[hand];
		gui.pressed = usable && pressed[hand];
		gui.released = released[hand];

		bool close = false;
		if (m_open == Panel::Settings)
		{
			m_settings_menu->SetControllerState(input.controller);
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
			// Either thumbstick scrolls the list.
			const ControllerState& c = input.controller;
			const float stick = std::abs(c.left_stick.y) > std::abs(c.right_stick.y) ? c.left_stick.y : c.right_stick.y;
			if (std::abs(stick) > 0.2f)
				m_game_list->Scroll(-stick * SCROLL_SPEED);
			if (m_game_list->Update(gui) || just_opened)
				m_panel.Upload(m_game_list->GetCanvas());
			close = m_game_list->TakeCloseRequest();
		}
		if (close || (usable && pressed[hand] && !hand_on_panel))
			Close();
		else if (pointers[hand].valid) // no beam from a controller not tracked yet
			PlacePointer(time, hand, aim[hand], panel_hit, m_panel.Pose().orientation);
		if (just_opened || m_open == Panel::None)
		{
			// The trigger that opened or closed the menu stays the menu's until it is let go.
			for (int h = 0; h < 2; h++)
				m_swallow[h] = m_trigger_down[h];
		}
	}

	for (int h = 0; h < 2; h++)
		result.block_trigger[h] |= m_swallow[h];
	return result;
}

void VrMenu::GetLayers(const XrCompositionLayerBaseHeader** slots)
{
	static_assert(NUM_LAYER_SLOTS == 4 + NUM_GROUPS, "the panel, the button groups, the tooltip, the beam and the dot");
	for (int i = 0; i < NUM_LAYER_SLOTS; i++)
		slots[i] = nullptr;
	if (!m_created)
		return;
	if (m_open != Panel::None && m_panel.HasImage())
		slots[0] = m_panel.Layer();
	for (int g = 0; m_open == Panel::None && g < NUM_GROUPS; g++)
	{
		if (m_groups[g].layer.HasImage())
			slots[1 + g] = m_groups[g].layer.Layer();
	}
	if (m_show_tooltip && m_open == Panel::None && m_tooltip.HasImage())
		slots[1 + NUM_GROUPS] = m_tooltip.Layer();
	if (m_show_pointer)
		slots[2 + NUM_GROUPS] = m_beam.Layer();
	if (m_show_cursor)
		slots[3 + NUM_GROUPS] = m_cursor.Layer();
}
