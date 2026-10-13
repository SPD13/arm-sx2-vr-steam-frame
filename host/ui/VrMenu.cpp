// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
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
	constexpr float POINTER_REACH = 6.0f; // the beam ends on the screen's plane up to this far away
	constexpr float FREE_BEAM_LENGTH = 1.0f; // and is this long when it points elsewhere
	constexpr float PANEL_LIFT = 0.03f; // the panel stands this far in front of the screen
	constexpr float CURSOR_SIZE = 0.035f;
	constexpr float BEAM_WIDTH = 0.006f;
	constexpr int BUTTON_PX = 128;
	constexpr int CURSOR_PX = 64;
	constexpr int BEAM_W_PX = 256, BEAM_H_PX = 16;
	constexpr uint32_t GEAR_ICON = 0xf013;
	constexpr uint32_t GAMES_ICON = 0xf11b; // gamepad
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
	if (!m_panel.Create(xr, device, gpu, queue, queue_family_index, SettingsMenu::WIDTH, SettingsMenu::HEIGHT) ||
		!m_gear.layer.Create(xr, device, gpu, queue, queue_family_index, BUTTON_PX, BUTTON_PX) ||
		!m_games.layer.Create(xr, device, gpu, queue, queue_family_index, BUTTON_PX, BUTTON_PX) ||
		!m_cursor.Create(xr, device, gpu, queue, queue_family_index, CURSOR_PX, CURSOR_PX) ||
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

	for (Button* button : {&m_gear, &m_games})
	{
		button->icon = button == &m_gear ? GEAR_ICON : GAMES_ICON;
		button->canvas.Resize(BUTTON_PX, BUTTON_PX);
		button->layer.SetSize(BUTTON_SIZE, BUTTON_SIZE);
		DrawButton(*button);
	}
	m_created = true;
	return true;
}

void VrMenu::Destroy()
{
	m_created = false;
	m_beam.Destroy();
	m_cursor.Destroy();
	m_games.layer.Destroy();
	m_gear.layer.Destroy();
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
	if (button.hovered)
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::accent);
		c.DrawIcon(m_icons, 64.0f, all, button.icon, Theme::text);
	}
	else
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::panel.WithAlpha(0.6f));
		c.DrawIcon(m_icons, 60.0f, all, button.icon, Theme::text.WithAlpha(0.8f));
	}
	button.layer.Upload(c);
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
	if (m_open == Panel::None)
		Emu::SetPaused(true);
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
	Emu::SetPaused(false);
	Console.WriteLn("Menu: closed");
}

void VrMenu::PlacePointer(XrTime time, const XrPosef& aim, const Hit& hit, const XrQuaternionf& facing)
{
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

VrMenu::Result VrMenu::Update(
	XrTime time, const XrApp::Input& input, const XrPosef& screen, float screen_width, float screen_height)
{
	Result result;
	m_show_pointer = false;
	m_show_cursor = false;
	if (!m_created)
		return result;
	m_screen = screen;
	m_screen_width = screen_width;

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

	// The buttons, one above the other just outside the screen's right edge, level with its top.
	const float spacing = BUTTON_SIZE + BUTTON_GAP;
	const float button_x = 0.5f * screen_width + BUTTON_MARGIN + 0.5f * BUTTON_SIZE;
	const float gear_y = 0.5f * screen_height - 0.5f * BUTTON_SIZE, games_y = gear_y - spacing;
	XrPosef gear_pose = screen, games_pose = screen;
	gear_pose.position = Transform(screen, {button_x, gear_y, 0.0f});
	games_pose.position = Transform(screen, {button_x, games_y, 0.0f});
	m_gear.layer.SetPose(gear_pose);
	m_games.layer.SetPose(games_pose);
	if (m_open != Panel::None && m_reanchor)
		AnchorPanel();
	m_reanchor = false;

	// Where each controller points: on the open panel's plane, else on the screen's. Without a
	// menu, the pointer is out while a controller points anywhere but at the game.
	XrPosef aim[2];
	Hit hits[2];
	bool pointing[2] = {}; // at the panel, or away from the game
	for (int h = 0; h < 2; h++)
	{
		if (!m_xr->LocateAim(h, time, &aim[h]))
			continue;
		if (m_open != Panel::None)
		{
			hits[h] = HitPlane(m_panel.Pose(), aim[h]);
			pointing[h] = hits[h].hit && std::abs(hits[h].x) <= 0.5f * m_panel.Size().width &&
			              std::abs(hits[h].y) <= 0.5f * m_panel.Size().height;
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
		const auto on = [&](const Hit& hit, float centre_y) {
			return hit.hit && std::hypot(hit.x - button_x, hit.y - centre_y) < 0.5f * BUTTON_SIZE;
		};
		const Hit& hit = hits[hand];
		const bool on_gear = on(hit, gear_y), on_games = on(hit, games_y);
		for (Button* button : {&m_gear, &m_games})
		{
			const bool hovered = button == &m_gear ? on_gear : on_games;
			if (hovered != button->hovered)
			{
				button->hovered = hovered;
				DrawButton(*button);
			}
		}
		// Only on the buttons themselves: a trigger aimed off the screen still reaches the game (light
		// gun games reload that way).
		for (int h = 0; h < 2; h++)
			result.block_trigger[h] = on(hits[h], gear_y) || on(hits[h], games_y);
		if ((on_gear || on_games) && pressed[hand])
		{
			Open(on_gear ? Panel::Settings : Panel::Games);
			m_gear.hovered = m_games.hovered = false;
			DrawButton(m_gear);
			DrawButton(m_games);
			just_opened = true;
			hits[hand] = HitPlane(m_panel.Pose(), aim[hand]);
			pointing[hand] = hits[hand].hit && std::abs(hits[hand].x) <= 0.5f * m_panel.Size().width &&
			                 std::abs(hits[hand].y) <= 0.5f * m_panel.Size().height;
		}
		else if (pointing[hand])
			PlacePointer(time, aim[hand], hit, screen.orientation);
	}

	if (m_open != Panel::None)
	{
		result.open = true;
		const bool on_panel = pointing[hand];
		const Hit& panel_hit = hits[hand];
		GuiInput gui;
		gui.pointer = on_panel;
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
		if (close || (usable && pressed[hand] && !on_panel))
			Close();
		else
			PlacePointer(time, aim[hand], panel_hit, m_panel.Pose().orientation);
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

void VrMenu::AddLayers(const XrCompositionLayerBaseHeader** layers, uint32_t* count)
{
	if (!m_created)
		return;
	if (m_open != Panel::None)
	{
		if (m_panel.HasImage())
			layers[(*count)++] = m_panel.Layer();
	}
	else
	{
		for (Button* button : {&m_gear, &m_games})
		{
			if (button->layer.HasImage())
				layers[(*count)++] = button->layer.Layer();
		}
	}
	if (m_show_pointer)
		layers[(*count)++] = m_beam.Layer();
	if (m_show_cursor)
		layers[(*count)++] = m_cursor.Layer();
}
