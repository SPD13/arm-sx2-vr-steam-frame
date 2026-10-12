// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#include "VrMenu.h"

#include "Emu.h"
#include "XrMath.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include "pcsx2/Config.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float GEAR_SIZE = 0.12f; // meters
	constexpr float GEAR_INSET = 0.05f; // from the screen's corner
	constexpr float GEAR_ZONE = 3.0f; // the pointer shows within this many gear sizes around it
	constexpr float PANEL_LIFT = 0.03f; // the panel stands this far in front of the screen
	constexpr float CURSOR_SIZE = 0.035f;
	constexpr float BEAM_WIDTH = 0.006f;
	constexpr int GEAR_PX = 128;
	constexpr int CURSOR_PX = 64;
	constexpr int BEAM_W_PX = 256, BEAM_H_PX = 16;
	constexpr uint32_t GEAR_ICON = 0xf013;
} // namespace

VrMenu::VrMenu(VrSettingsStore& settings, WebServer& web, bool running_stereo, bool running_flat)
	: m_settings(settings)
	, m_web(web)
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
		!m_gear.Create(xr, device, gpu, queue, queue_family_index, GEAR_PX, GEAR_PX) ||
		!m_cursor.Create(xr, device, gpu, queue, queue_family_index, CURSOR_PX, CURSOR_PX) ||
		!m_beam.Create(xr, device, gpu, queue, queue_family_index, BEAM_W_PX, BEAM_H_PX))
	{
		Destroy();
		return false;
	}
	m_menu = std::make_unique<SettingsMenu>(m_settings, m_web, m_running_stereo, m_running_flat, m_text, m_icons);

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

	m_gear_canvas.Resize(GEAR_PX, GEAR_PX);
	DrawGear();
	m_created = true;
	return true;
}

void VrMenu::Destroy()
{
	m_created = false;
	m_beam.Destroy();
	m_cursor.Destroy();
	m_gear.Destroy();
	m_panel.Destroy();
	m_menu.reset();
}

void VrMenu::DrawGear()
{
	Canvas& c = m_gear_canvas;
	c.Clear();
	const float mid = 0.5f * GEAR_PX;
	if (m_gear_hovered)
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::accent);
		c.DrawIcon(m_icons, 64.0f, {0, 0, GEAR_PX, GEAR_PX}, GEAR_ICON, Theme::text);
	}
	else
	{
		c.FillCircle(mid, mid, mid - 2.0f, Theme::panel.WithAlpha(0.6f));
		c.DrawIcon(m_icons, 60.0f, {0, 0, GEAR_PX, GEAR_PX}, GEAR_ICON, Theme::text.WithAlpha(0.8f));
	}
	m_gear.Upload(c);
}

VrMenu::Hit VrMenu::HitQuad(const XrPosef& quad, float width, float height, const XrPosef& aim)
{
	Hit hit;
	const XrVector3f dir = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
	const XrQuaternionf to_quad = Conjugate(quad.orientation);
	const XrVector3f o = Rotate(to_quad, aim.position - quad.position);
	const XrVector3f d = Rotate(to_quad, dir);
	// Only from in front of the quad (+Z side), pointing at it.
	if (o.z <= 0.0f || d.z >= -1e-4f)
		return hit;
	const float t = -o.z / d.z;
	const float x = o.x + t * d.x, y = o.y + t * d.y;
	if (std::abs(x) > 0.5f * width || std::abs(y) > 0.5f * height)
		return hit;
	hit.hit = true;
	hit.x = x;
	hit.y = y;
	hit.u = x / width + 0.5f;
	hit.v = 0.5f - y / height;
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

void VrMenu::Open()
{
	m_open = true;
	m_menu->SetFrameControllers(m_xr->HasFrameControllers());
	m_menu->Refresh();
	AnchorPanel();
	Emu::SetPaused(true);
	Console.WriteLn("Menu: open");
}

void VrMenu::Close()
{
	m_open = false;
	Emu::SetPaused(false);
	Console.WriteLn("Menu: closed");
}

void VrMenu::PlacePointer(XrTime time, const XrPosef& aim, const Hit& hit)
{
	m_show_pointer = true;
	XrPosef cursor;
	cursor.position = hit.point + hit.normal * 0.004f;
	cursor.orientation = (m_open ? m_panel : m_gear).Pose().orientation;
	m_cursor.SetPose(cursor);

	// A thin quad from the controller to the dot, turned about its length to face the head.
	const XrVector3f dir = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
	const XrVector3f start = aim.position + dir * 0.02f;
	const float length = std::sqrt(Dot(hit.point - start, hit.point - start));
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
	if (!m_created)
		return result;
	m_screen = screen;
	m_screen_width = screen_width;

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

	XrPosef gear_pose = screen;
	gear_pose.position = Transform(screen, {0.5f * screen_width - GEAR_INSET - 0.5f * GEAR_SIZE,
											   0.5f * screen_height - GEAR_INSET - 0.5f * GEAR_SIZE, 0.01f});
	m_gear.SetPose(gear_pose);
	m_gear.SetSize(GEAR_SIZE, GEAR_SIZE);
	if (m_open && m_reanchor)
		AnchorPanel();
	m_reanchor = false;

	// Where each controller points: the panel when open, else the area around the gear.
	XrPosef aim[2];
	Hit hits[2];
	for (int h = 0; h < 2; h++)
	{
		if (!m_xr->LocateAim(h, time, &aim[h]))
			continue;
		if (m_open)
			hits[h] = HitQuad(m_panel.Pose(), m_panel.Size().width, m_panel.Size().height, aim[h]);
		else
			hits[h] = HitQuad(gear_pose, GEAR_ZONE * GEAR_SIZE, GEAR_ZONE * GEAR_SIZE, aim[h]);
	}
	// The hand that clicks is the pointer; otherwise the one that points at something.
	for (int h = 0; h < 2; h++)
	{
		if (pressed[h])
			m_hand = h;
	}
	if (!hits[m_hand].hit && hits[1 - m_hand].hit && !m_trigger_down[m_hand])
		m_hand = 1 - m_hand;
	const int hand = m_hand;
	const Hit& hit = hits[hand];

	bool just_opened = false;
	if (!m_open)
	{
		const bool on_gear = hit.hit && std::hypot(hit.x, hit.y) < 0.5f * GEAR_SIZE;
		if (on_gear != m_gear_hovered)
		{
			m_gear_hovered = on_gear;
			DrawGear();
		}
		// Only on the gear itself: aiming near the corner in a game still shoots.
		for (int h = 0; h < 2; h++)
			result.block_trigger[h] = hits[h].hit && std::hypot(hits[h].x, hits[h].y) < 0.5f * GEAR_SIZE;
		if (on_gear && pressed[hand])
		{
			Open();
			just_opened = true;
			hits[hand] = HitQuad(m_panel.Pose(), m_panel.Size().width, m_panel.Size().height, aim[hand]);
		}
		else if (hit.hit)
			PlacePointer(time, aim[hand], hit);
	}

	if (m_open)
	{
		result.open = true;
		const Hit& panel_hit = hits[hand];
		GuiInput gui;
		gui.pointer = panel_hit.hit;
		gui.x = panel_hit.u * static_cast<float>(SettingsMenu::WIDTH);
		gui.y = panel_hit.v * static_cast<float>(SettingsMenu::HEIGHT);
		const bool usable = !just_opened && !m_swallow[hand];
		gui.down = usable && m_trigger_down[hand];
		gui.pressed = usable && pressed[hand];
		gui.released = released[hand];
		if (m_menu->Update(gui))
			m_panel.Upload(m_menu->GetCanvas());

		if (m_menu->TakeRecenterRequest())
		{
			result.recenter = true;
			m_reanchor = true;
		}
		if (m_menu->TakeCloseRequest() || (usable && pressed[hand] && !panel_hit.hit))
			Close();
		else if (panel_hit.hit)
			PlacePointer(time, aim[hand], panel_hit);
		if (just_opened || !m_open)
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
	if (m_open)
	{
		if (m_panel.HasImage())
			layers[(*count)++] = m_panel.Layer();
	}
	else if (m_gear.HasImage())
		layers[(*count)++] = m_gear.Layer();
	if (m_show_pointer)
	{
		layers[(*count)++] = m_beam.Layer();
		layers[(*count)++] = m_cursor.Layer();
	}
}
