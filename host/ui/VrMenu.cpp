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
	constexpr float BUTTON_GROUP_GAP = 0.05f; // more between the menu buttons and the light gun's
	constexpr uint32_t GEAR_ICON = 0xf013;
	constexpr uint32_t GAMES_ICON = 0xf11b; // gamepad
	constexpr uint32_t GUN_MODE_ICON = 0xf05b; // crosshairs
	constexpr uint32_t ONE_GUN_ICON = 0xf007; // user
	constexpr uint32_t TWO_GUNS_ICON = 0xf0c0; // users
	constexpr float SCROLL_SPEED = 22.0f; // pixels per frame with a thumbstick pushed all the way
} // namespace

static_assert(SettingsMenu::WIDTH == GameListMenu::WIDTH && SettingsMenu::HEIGHT == GameListMenu::HEIGHT,
	"the menus share one panel layer");

VrMenu::VrMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls, bool running_stereo, bool running_flat)
	: m_settings(settings)
	, m_web(web)
	, m_controls(controls)
	, m_running_stereo(running_stereo)
	, m_running_flat(running_flat)
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
	bool buttons_made = true;
	for (Button& button : m_buttons)
		buttons_made = buttons_made && button.layer.Create(xr, device, gpu, queue, queue_family_index, BUTTON_PX, BUTTON_PX);
	if (!m_panel.Create(xr, device, gpu, queue, queue_family_index, SettingsMenu::WIDTH, SettingsMenu::HEIGHT) ||
		!buttons_made || !m_cursor.Create(xr, device, gpu, queue, queue_family_index, CURSOR_PX, CURSOR_PX) ||
		!m_beam.Create(xr, device, gpu, queue, queue_family_index, BEAM_W_PX, BEAM_H_PX))
	{
		Destroy();
		return false;
	}
	m_settings_menu = std::make_unique<SettingsMenu>(m_settings, m_web, m_controls, m_running_stereo, m_running_flat, m_text, m_icons);
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

	m_buttons[GEAR].icon = GEAR_ICON;
	m_buttons[GAMES].icon = GAMES_ICON;
	m_buttons[GUN_MODE].icon = GUN_MODE_ICON;
	m_buttons[GUNS].icon = m_guns > 1 ? TWO_GUNS_ICON : ONE_GUN_ICON;
	m_buttons[GUN_MODE].lit = m_gun_on;
	m_buttons[GUN_MODE].disabled = !m_has_game;
	m_buttons[GUNS].disabled = !m_gun_on;
	for (Button& button : m_buttons)
	{
		button.canvas.Resize(BUTTON_PX, BUTTON_PX);
		button.layer.SetSize(BUTTON_SIZE, BUTTON_SIZE);
		DrawButton(button);
	}
	m_created = true;
	return true;
}

void VrMenu::Destroy()
{
	m_created = false;
	m_beam.Destroy();
	m_cursor.Destroy();
	for (Button& button : m_buttons)
		button.layer.Destroy();
	m_panel.Destroy();
	m_game_list.reset();
	m_settings_menu.reset();
}

void VrMenu::DrawButton(Button& button)
{
	Canvas& c = button.canvas;
	c.Clear();
	const float mid = 0.5f * BUTTON_PX;
	const Rect all{0, 0, BUTTON_PX, BUTTON_PX};
	if (button.disabled)
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::panel.WithAlpha(0.35f));
		c.DrawIcon(m_icons, 56.0f, all, button.icon, Theme::text.WithAlpha(0.3f));
	}
	else if (button.hovered)
	{
		c.FillCircle(mid, mid, mid - 2.0f, button.lit ? Theme::accent_hover : Theme::accent);
		c.DrawIcon(m_icons, 64.0f, all, button.icon, Theme::text);
	}
	else if (button.lit)
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::accent.WithAlpha(0.85f));
		c.DrawIcon(m_icons, 60.0f, all, button.icon, Theme::text);
	}
	else
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::panel.WithAlpha(0.6f));
		c.DrawIcon(m_icons, 60.0f, all, button.icon, Theme::text.WithAlpha(0.8f));
	}
	button.layer.Upload(c);
}

void VrMenu::SetGunState(bool has_game, bool on, bool chosen, int guns)
{
	if (m_settings_menu)
		m_settings_menu->SetGunMode(has_game, on, chosen);
	if (has_game == m_has_game && on == m_gun_on && guns == m_guns)
		return;
	m_has_game = has_game;
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
	if (m_pauses_game)
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

	// The buttons, one above the other just outside the screen's right edge, from level with its top.
	const float spacing = BUTTON_SIZE + BUTTON_GAP;
	const float button_x = 0.5f * screen_width + BUTTON_MARGIN + 0.5f * BUTTON_SIZE;
	m_buttons[GEAR].y = 0.5f * screen_height - 0.5f * BUTTON_SIZE;
	m_buttons[GAMES].y = m_buttons[GEAR].y - spacing;
	m_buttons[GUN_MODE].y = m_buttons[GAMES].y - spacing - BUTTON_GROUP_GAP;
	m_buttons[GUNS].y = m_buttons[GUN_MODE].y - spacing;
	for (Button& button : m_buttons)
	{
		XrPosef pose = screen;
		pose.position = Transform(screen, {button_x, button.y, 0.0f});
		button.layer.SetPose(pose);
	}
	if (m_open != Panel::None && m_reanchor)
		AnchorPanel();
	m_reanchor = false;

	// Where each controller points: on the open panel's plane, else on the screen's. Without a
	// menu, the pointer is out while a controller points anywhere but at the game.
	XrPosef aim[2];
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
			else
			{
				Open(hovered == GEAR ? Panel::Settings : Panel::Games);
				for (Button& button : m_buttons)
				{
					if (button.hovered)
					{
						button.hovered = false;
						DrawButton(button);
					}
				}
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
			if (m_settings_menu->TakeRecenterRequest())
			{
				result.recenter = true;
				m_reanchor = true;
			}
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
		else
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
	for (int i = 0; i < NUM_LAYER_SLOTS; i++)
		slots[i] = nullptr;
	if (!m_created)
		return;
	if (m_open != Panel::None && m_panel.HasImage())
		slots[0] = m_panel.Layer();
	for (int b = 0; m_open == Panel::None && b < NUM_BUTTONS; b++)
	{
		if (m_buttons[b].layer.HasImage())
			slots[1 + b] = m_buttons[b].layer.Layer();
	}
	if (m_show_pointer)
		slots[5] = m_beam.Layer();
	if (m_show_cursor)
		slots[6] = m_cursor.Layer();
}
