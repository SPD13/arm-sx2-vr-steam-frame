// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#include "SettingsMenu.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>

// Font Awesome 6 code points (see IconsFontAwesome.h).
namespace Icon
{
	static constexpr uint32_t gear = 0xf013;
	static constexpr uint32_t display = 0xf390; // desktop
	static constexpr uint32_t stereo = 0xf729; // vr-cardboard
	static constexpr uint32_t graphics = 0xe2ca; // wand-magic-sparkles
	static constexpr uint32_t audio = 0xf028; // volume-high
	static constexpr uint32_t game = 0xf0c7; // floppy-disk
	static constexpr uint32_t controls = 0xf11b; // gamepad
	static constexpr uint32_t exit = 0xf011; // power-off
	static constexpr uint32_t close = 0xf00d; // xmark
	static constexpr uint32_t recenter = 0xf05b; // crosshairs
	static constexpr uint32_t save = 0xf019; // download
	static constexpr uint32_t load = 0xf093; // upload
	static constexpr uint32_t restart = 0xf2ea; // rotate-left
	static constexpr uint32_t info = 0xf05a; // circle-info
	static constexpr uint32_t transfer = 0xf1eb; // wifi
} // namespace Icon

namespace
{
	constexpr float NAV_WIDTH = 300.0f;
	constexpr float PAD = 48.0f;
	constexpr float CONTENT_X = NAV_WIDTH + PAD;
	constexpr float CONTENT_RIGHT = SettingsMenu::WIDTH - PAD;
	constexpr float CONTROL_WIDTH = 420.0f;
	constexpr float LABEL_WIDTH = CONTENT_RIGHT - CONTROL_WIDTH - 40.0f - CONTENT_X;
	constexpr float PAGE_TOP = 128.0f;
	constexpr float RADIUS = 28.0f;
} // namespace

SettingsMenu::SettingsMenu(
	VrSettingsStore& settings, WebServer& web, bool running_stereo, bool running_flat, Font& text, Font& icons)
	: m_settings(settings)
	, m_web(web)
	, m_running_stereo(running_stereo)
	, m_running_flat(running_flat)
	, m_gui(text, icons)
{
	m_canvas.Resize(WIDTH, HEIGHT);
}

void SettingsMenu::Refresh()
{
	m_core = Emu::GetCoreSettings();
	Emu::RefreshSaveSlots();
	m_slots = Emu::GetSaveSlots();
	m_dialog = Dialog::None;
	m_needs_draw = true;
}

bool SettingsMenu::Update(const GuiInput& input)
{
	// Changes from elsewhere: VR.ini edited, a save slot written, a notice.
	if (m_settings.Version() != m_settings_version)
	{
		m_settings_version = m_settings.Version();
		m_needs_draw = true;
	}
	if (std::vector<Emu::SaveSlot> slots = Emu::GetSaveSlots();
		!std::equal(slots.begin(), slots.end(), m_slots.begin(), m_slots.end(),
			[](const Emu::SaveSlot& a, const Emu::SaveSlot& b) { return a.used == b.used && a.time == b.time; }))
	{
		m_slots = std::move(slots);
		m_needs_draw = true;
	}
	const Clock::time_point now = Clock::now();
	if (std::string notice = Emu::TakeNotice(); !notice.empty())
	{
		m_toast = std::move(notice);
		m_toast_until = now + std::chrono::seconds(3);
		m_needs_draw = true;
	}
	// The transfer page follows the server: its address, code and uploads.
	if (WebServer::Status web = m_web.GetStatus(); web.running != m_web_status.running || web.url != m_web_status.url ||
		web.code != m_web_status.code || web.error != m_web_status.error || web.transfer != m_web_status.transfer ||
		static_cast<int>(web.progress * 100.0f) != static_cast<int>(m_web_status.progress * 100.0f))
	{
		m_web_status = std::move(web);
		m_needs_draw |= m_page == Page::Transfer;
	}
	if (!m_toast.empty() && now >= m_toast_until)
	{
		m_toast.clear();
		m_needs_draw = true;
	}

	m_gui.Begin(&m_canvas, input, false);
	Run();
	m_gui.End();
	if (!m_gui.Changed() && !m_needs_draw)
		return false;

	m_needs_draw = false;
	m_canvas.Clear();
	m_gui.Begin(&m_canvas, input, true);
	Run();
	m_gui.End();
	return true;
}

void SettingsMenu::SetVr(const VrSettings& settings)
{
	m_settings.Set(settings);
	m_settings_version = m_settings.Version();
}

void SettingsMenu::SetCore(const Emu::CoreSettings& settings)
{
	m_core = settings;
	Emu::SetCoreSettings(settings);
}

void SettingsMenu::Run()
{
	if (m_gui.Drawing())
	{
		Canvas& c = m_gui.Draw();
		c.FillRect({0, 0, WIDTH, HEIGHT}, Theme::panel, RADIUS);
		// The nav bar: the panel's left part, square on its right edge.
		c.SetClip({0, 0, NAV_WIDTH, HEIGHT});
		c.FillRect({0, 0, NAV_WIDTH + RADIUS, HEIGHT}, Theme::nav, RADIUS);
		c.ResetClip();
	}

	m_gui.SetBlocked(m_dialog != Dialog::None);
	RunNav();

	static constexpr std::array<std::string_view, 7> titles = {
		"Display", "3D", "Graphics", "Audio", "Game", "Wi-Fi Transfer", "Controls"};
	m_gui.Text(CONTENT_X, 42.0f, titles[static_cast<size_t>(m_page)], Theme::title_size);
	if (m_gui.IconButton(GuiId("close"), {CONTENT_RIGHT - 52.0f, 36.0f, 60.0f, 60.0f}, Icon::close, 28.0f))
		m_close_requested = true;

	m_y = PAGE_TOP;
	switch (m_page)
	{
		case Page::Display: RunDisplay(); break;
		case Page::Stereo: RunStereo(); break;
		case Page::Graphics: RunGraphics(); break;
		case Page::Audio: RunAudio(); break;
		case Page::Game: RunGame(); break;
		case Page::Transfer: RunTransfer(); break;
		case Page::Controls: RunControls(); break;
	}

	if (!m_toast.empty() && m_gui.Drawing())
	{
		const float w = m_gui.TextWidth(m_toast, Theme::body_size) + 56.0f;
		const Rect r{std::round(CONTENT_X + 0.5f * (CONTENT_RIGHT - CONTENT_X - w)), HEIGHT - 92.0f, w, 52.0f};
		m_gui.Draw().FillRect(r, Theme::control_hover, 26.0f);
		m_gui.TextCentered(r, m_toast, Theme::body_size, Theme::text);
	}

	m_gui.SetBlocked(false);
	if (m_dialog != Dialog::None)
		RunDialog();
}

void SettingsMenu::RunNav()
{
	if (m_gui.Drawing())
	{
		m_gui.Draw().DrawIcon(m_gui.IconFont(), 30.0f, {28.0f, 36.0f, 40.0f, 60.0f}, Icon::gear, Theme::accent);
		m_gui.Text(80.0f, 46.0f, "Settings", Theme::title_size - 4.0f);
	}

	struct Item
	{
		Page page;
		uint32_t icon;
		std::string_view label;
	};
	static constexpr std::array<Item, 7> items = {{
		{Page::Display, Icon::display, "Display"},
		{Page::Stereo, Icon::stereo, "3D"},
		{Page::Graphics, Icon::graphics, "Graphics"},
		{Page::Audio, Icon::audio, "Audio"},
		{Page::Game, Icon::game, "Game"},
		{Page::Transfer, Icon::transfer, "Wi-Fi Transfer"},
		{Page::Controls, Icon::controls, "Controls"},
	}};
	float y = PAGE_TOP;
	for (const Item& item : items)
	{
		if (m_gui.NavItem(GuiId("nav", static_cast<int>(item.page)), {16.0f, y, NAV_WIDTH - 32.0f, 64.0f}, item.icon,
				item.label, m_page == item.page) &&
			m_page != item.page)
		{
			m_page = item.page;
			if (m_page == Page::Game)
				Emu::RefreshSaveSlots();
			m_needs_draw = true;
		}
		y += 72.0f;
	}

	// Exit, last and apart from the categories.
	if (m_gui.Drawing())
		m_gui.Draw().FillRect({32.0f, HEIGHT - 112.0f, NAV_WIDTH - 64.0f, 2.0f}, Theme::border);
	if (m_gui.NavItem(GuiId("exit"), {16.0f, HEIGHT - 96.0f, NAV_WIDTH - 32.0f, 64.0f}, Icon::exit, "Exit", false,
			Gui::Style::Danger))
	{
		m_dialog = Dialog::Exit;
		m_needs_draw = true;
	}
}

Rect SettingsMenu::Row(std::string_view label, std::string_view help, float control_height, bool with_note)
{
	const float top = m_y;
	m_gui.Text(CONTENT_X, top, label, Theme::label_size);
	float text_h = m_gui.TextFont().LineHeight(Theme::label_size) + 4.0f;
	if (!help.empty())
		text_h += m_gui.TextWrapped(CONTENT_X, top + text_h, LABEL_WIDTH, help, Theme::small_size, Theme::text_muted);
	const float note_h = with_note ? 40.0f : 0.0f;
	const float h = std::max(text_h, control_height + note_h);
	m_last_control = {CONTENT_RIGHT - CONTROL_WIDTH, std::round(top + 0.5f * (h - control_height - note_h)), CONTROL_WIDTH,
		control_height};
	m_y = top + h + 22.0f;
	if (m_gui.Drawing())
		m_gui.Draw().FillRect({CONTENT_X, m_y - 11.0f, CONTENT_RIGHT - CONTENT_X, 1.0f}, Theme::border.WithAlpha(0.6f));
	return m_last_control;
}

void SettingsMenu::Section(std::string_view title)
{
	m_y += 8.0f;
	m_gui.Text(CONTENT_X, m_y, title, Theme::small_size, Theme::accent);
	m_y += m_gui.TextFont().LineHeight(Theme::small_size) + 14.0f;
}

void SettingsMenu::Note(std::string_view text, Color color)
{
	// Under the last control, inside the row.
	if (!m_gui.Drawing())
		return;
	const float y = m_last_control.Bottom() + 8.0f;
	m_gui.Draw().DrawIcon(m_gui.IconFont(), Theme::small_size, {m_last_control.x, y, 24.0f, 26.0f}, Icon::info, color);
	m_gui.Text(m_last_control.x + 32.0f, y, text, Theme::small_size, color);
}

void SettingsMenu::RunDisplay()
{
	VrSettings vr = m_settings.Get();
	bool changed = false;
	changed |= m_gui.Slider(GuiId("screen_width"), Row("Screen size", "Width of the virtual screen."), &vr.screen_width,
		1.0f, 4.0f, 0.1f, fmt::format("{:.1f} m", vr.screen_width));
	changed |= m_gui.Slider(GuiId("screen_distance"), Row("Screen distance", "How far in front of you the screen stands."),
		&vr.screen_distance, 1.0f, 5.0f, 0.1f, fmt::format("{:.1f} m", vr.screen_distance));
	if (changed)
		SetVr(vr);

	const Rect r = Row("Recenter", "Place the screen straight ahead of you. Clicking both thumbsticks does the same.");
	if (m_gui.Button(GuiId("recenter"), {r.Right() - 220.0f, r.y, 220.0f, r.h}, "Recenter", Icon::recenter))
		m_recenter_requested = true;
}

void SettingsMenu::RunStereo()
{
	VrSettings vr = m_settings.Get();
	bool changed = false;

	static constexpr std::array<std::string_view, 2> modes = {"Off", "Portal"};
	int mode = vr.stereo ? 1 : 0;
	if (m_gui.Segmented(GuiId("stereo"),
			Row("3D effect", "Portal draws the game for each eye, so the world sits behind the screen like a window. "
							 "Off shows a flat picture.", 52.0f, true),
			modes, &mode))
	{
		vr.stereo = mode == 1;
		if (!vr.stereo)
			vr.flat = true;
		changed = true;
	}
	if (vr.stereo != m_running_stereo || (!vr.stereo && vr.flat != m_running_flat))
		Note("Applies the next time the emulator starts.", Theme::accent);

	changed |= m_gui.Slider(GuiId("stereo_depth"),
		Row("Depth", "How far behind the screen the farthest scenery appears."), &vr.stereo_depth, 0.0f, 5.0f, 0.1f,
		fmt::format("{:.1f} m", vr.stereo_depth));

	Section("ADVANCED");
	static constexpr std::array<std::string_view, 2> sources = {"Perspective", "Depth buffer"};
	int source = vr.stereo_z_mode ? 1 : 0;
	if (m_gui.Segmented(GuiId("stereo_z_mode"),
			Row("Depth source", "What tells how far away each object is. Try the other one when the 3D looks flat or "
								"wrong in a game."),
			sources, &source))
	{
		vr.stereo_z_mode = source == 1;
		changed = true;
	}
	// The screen plane's units differ by source: Q (1/w) for perspective, normalised Z otherwise.
	const float lo = vr.stereo_z_mode ? 0.01f : 0.1f, hi = vr.stereo_z_mode ? 1.0f : 5.0f;
	const float step = vr.stereo_z_mode ? 0.005f : 0.05f;
	float convergence = std::clamp(vr.stereo_convergence, lo, hi);
	if (m_gui.Slider(GuiId("stereo_convergence"),
			Row("Screen plane", "Which depth in the scene sits on the screen glass. Set it so the nearest objects "
								"just touch the glass."),
			&convergence, lo, hi, step, vr.stereo_z_mode ? fmt::format("{:.3f}", convergence) : fmt::format("{:.2f}", convergence)))
	{
		vr.stereo_convergence = convergence;
		changed = true;
	}
	if (changed)
		SetVr(vr);
}

void SettingsMenu::RunGraphics()
{
	Emu::CoreSettings core = m_core;
	static constexpr std::array<std::string_view, 4> scales = {"Native", "2x", "3x", "4x"};
	int scale = std::clamp(core.upscale, 1, 4) - 1;
	if (m_gui.Segmented(GuiId("upscale"),
			Row("Internal resolution", "Renders the game sharper than the PS2 did. Higher values cost GPU time."),
			scales, &scale))
	{
		core.upscale = scale + 1;
		SetCore(core);
	}

	// The menu's order (index) to BiFiltering values.
	static constexpr std::array<std::string_view, 3> filters = {"Nearest", "PS2", "Smooth"};
	static constexpr std::array<int, 3> filter_values = {0, 2, 1};
	int filter = static_cast<int>(std::find(filter_values.begin(), filter_values.end(), core.texture_filtering) -
								  filter_values.begin());
	if (m_gui.Segmented(GuiId("filter"),
			Row("Texture filtering", "PS2 smooths textures the way the console did. Smooth filters all of them, "
									 "Nearest keeps every texel sharp."),
			filters, &filter))
	{
		core.texture_filtering = filter_values[static_cast<size_t>(filter)];
		SetCore(core);
	}
}

void SettingsMenu::RunAudio()
{
	Emu::CoreSettings core = m_core;
	float volume = static_cast<float>(core.volume);
	if (m_gui.Slider(GuiId("volume"), Row("Volume", "Game sound level."), &volume, 0.0f, 100.0f, 5.0f,
			fmt::format("{}%", static_cast<int>(volume))))
	{
		core.volume = static_cast<int>(volume);
		SetCore(core);
	}
	if (m_gui.Toggle(GuiId("mute"), Row("Mute", "Silence the game."), &core.muted))
		SetCore(core);
}

void SettingsMenu::RunGame()
{
	// One row: the slot choice, what the slot holds, then Save and Load.
	const Rect r = Row("Save slot", "Save states keep the whole game as it is now, for this game only.", 52.0f + 40.0f + 56.0f);
	m_last_control = {r.x, r.y, r.w, 52.0f};
	static constexpr std::array<std::string_view, Emu::NUM_SAVE_SLOTS> slots = {"1", "2", "3", "4", "5"};
	int slot = m_slot - 1;
	if (m_gui.Segmented(GuiId("slot"), m_last_control, slots, &slot))
		m_slot = slot + 1;
	const Emu::SaveSlot& info = m_slots[static_cast<size_t>(m_slot - 1)];
	if (info.used)
	{
		char when[64];
		std::tm tm{};
		localtime_r(&info.time, &tm);
		std::strftime(when, sizeof(when), "%d %b %Y, %H:%M", &tm);
		Note(fmt::format("Slot {}: saved {}", m_slot, when), Theme::text_muted);
	}
	else
		Note(fmt::format("Slot {} is empty", m_slot), Theme::text_muted);

	const float w = 0.5f * (r.w - 16.0f);
	const float by = r.Bottom() - 56.0f;
	if (m_gui.Button(GuiId("save"), {r.x, by, w, 56.0f}, "Save", Icon::save, Gui::Style::Primary))
		Emu::SaveState(m_slot);
	m_gui.SetDisabled(!info.used);
	if (m_gui.Button(GuiId("load"), {r.Right() - w, by, w, 56.0f}, "Load", Icon::load))
	{
		Emu::LoadState(m_slot);
		m_close_requested = true;
	}
	m_gui.SetDisabled(false);

	const Rect restart = Row("Restart game", "Start the game over from the beginning, as a console reset does.");
	if (m_gui.Button(GuiId("restart"), {restart.Right() - 220.0f, restart.y, 220.0f, restart.h}, "Restart", Icon::restart))
	{
		m_dialog = Dialog::Restart;
		m_needs_draw = true;
	}
}

void SettingsMenu::RunTransfer()
{
	const WebServer::Status& web = m_web_status;
	bool on = web.running;
	if (m_gui.Toggle(GuiId("web"),
			Row("Wi-Fi transfer", "Add games and BIOS files from a computer or phone on the same Wi-Fi network, in "
								  "its web browser.", 52.0f, !on && !web.error.empty()),
			&on))
	{
		if (on)
			m_web.Start(m_settings.Get().web_require_code);
		else
			m_web.Stop();
		m_web_status = m_web.GetStatus();
	}
	if (!on && !web.error.empty())
		Note(web.error, Theme::danger);

	VrSettings vr = m_settings.Get();
	if (m_gui.Toggle(GuiId("web_require_code"),
			Row("Access code", "The page asks for a 6-digit code shown here. A new one is made each time the "
							   "transfer starts."),
			&vr.web_require_code))
	{
		SetVr(vr);
		m_web.SetRequireCode(vr.web_require_code);
		m_web_status = m_web.GetStatus();
	}

	if (!web.running)
		return;
	// Where to go, in large type.
	const Rect box{CONTENT_X, m_y + 4.0f, CONTENT_RIGHT - CONTENT_X, web.code.empty() ? 128.0f : 196.0f};
	if (m_gui.Drawing())
		m_gui.Draw().FillRect(box, Theme::nav, 16.0f);
	float y = box.y + 22.0f;
	if (web.url.empty())
	{
		m_gui.TextWrapped(box.x + 28.0f, y, box.w - 56.0f,
			"No network connection. Connect the headset to Wi-Fi to use the transfer.", Theme::body_size, Theme::text_muted);
	}
	else
	{
		m_gui.Text(box.x + 28.0f, y, "Open this address in a browser", Theme::small_size, Theme::text_muted);
		m_gui.Text(box.x + 28.0f, y + 30.0f, web.url, Theme::title_size + 4.0f, Theme::accent);
		if (!web.code.empty())
		{
			y += 96.0f;
			m_gui.Text(box.x + 28.0f, y, "Access code", Theme::small_size, Theme::text_muted);
			m_gui.Text(box.x + 28.0f, y + 30.0f, web.code.substr(0, 3) + " " + web.code.substr(3), Theme::title_size + 4.0f);
			if (m_gui.Button(GuiId("web_new_code"), {box.Right() - 28.0f - 200.0f, y + 18.0f, 200.0f, 52.0f}, "New code"))
			{
				m_web.SetRequireCode(true);
				m_web_status = m_web.GetStatus();
				m_needs_draw = true;
			}
		}
	}
	m_y = box.Bottom() + 20.0f;

	// The upload in progress.
	if (!web.transfer.empty())
	{
		m_gui.Text(CONTENT_X, m_y, fmt::format("Receiving {}", web.transfer), Theme::body_size);
		const std::string percent = fmt::format("{}%", static_cast<int>(web.progress * 100.0f));
		m_gui.Text(CONTENT_RIGHT - m_gui.TextWidth(percent, Theme::body_size), m_y, percent, Theme::body_size, Theme::text_muted);
		if (m_gui.Drawing())
		{
			const Rect bar{CONTENT_X, m_y + 40.0f, CONTENT_RIGHT - CONTENT_X, 8.0f};
			m_gui.Draw().FillRect(bar, Theme::control, 4.0f);
			m_gui.Draw().FillRect({bar.x, bar.y, bar.w * web.progress, bar.h}, Theme::accent, 4.0f);
		}
	}
	else
		m_gui.Text(CONTENT_X, m_y, "Files sent from the browser appear in the emulator's BIOS and games folders.",
			Theme::small_size, Theme::text_muted);
}

void SettingsMenu::RunControls()
{
	if (!m_gui.Drawing())
		return;
	struct Line
	{
		std::string_view control, ps2;
	};
	static constexpr Line frame[] = {
		{"A / B / X / Y", "Cross / Circle / Square / Triangle"},
		{"Left D-pad", "D-pad"},
		{"Left / right stick", "Left / right analog stick"},
		{"Stick clicks", "L3 / R3"},
		{"Left / right grip", "L1 / R1"},
		{"Left / right trigger", "L2 / R2"},
		{"View button", "Start (hold: Select)"},
		{"Both sticks clicked", "Recenter the screen"},
		{"Point at the gear, trigger", "Open this menu"},
	};
	static constexpr Line touch[] = {
		{"A / B", "Cross / Circle"},
		{"X / Y (left)", "Square / Triangle"},
		{"Left stick pushed fully", "D-pad"},
		{"Left / right stick", "Left / right analog stick"},
		{"Stick clicks", "L3 / R3"},
		{"Left / right grip", "L1 / R1"},
		{"Left / right trigger", "L2 / R2"},
		{"Menu button", "Start (hold: Select)"},
		{"Both sticks clicked", "Recenter the screen"},
		{"Point at the gear, trigger", "Open this menu"},
	};
	const std::span<const Line> lines = m_frame_controllers ? std::span<const Line>(frame) : std::span<const Line>(touch);
	float y = m_y;
	m_gui.Text(CONTENT_X, y, m_frame_controllers ? "Steam Frame controllers" : "Controllers (Touch layout)",
		Theme::small_size, Theme::accent);
	y += 44.0f;
	const float split = CONTENT_X + 360.0f;
	for (const Line& line : lines)
	{
		m_gui.Text(CONTENT_X, y, line.control, Theme::body_size, Theme::text_muted);
		m_gui.Text(split, y, line.ps2, Theme::body_size);
		y += 46.0f;
		m_gui.Draw().FillRect({CONTENT_X, y - 9.0f, CONTENT_RIGHT - CONTENT_X, 1.0f}, Theme::border.WithAlpha(0.5f));
	}
}

void SettingsMenu::RunDialog()
{
	const bool exit = m_dialog == Dialog::Exit;
	const Rect box{0.5f * (WIDTH - 600.0f), 0.5f * (HEIGHT - 300.0f), 600.0f, 300.0f};
	if (m_gui.Drawing())
	{
		Canvas& c = m_gui.Draw();
		c.FillRect({0, 0, WIDTH, HEIGHT}, Rgb(0x000000, 140), RADIUS);
		c.FillRect(box, Theme::nav, 24.0f);
		c.StrokeRect(box, Theme::border, 24.0f, 2.0f);
		m_gui.Text(box.x + 40.0f, box.y + 36.0f, exit ? "Quit PS2 Volumetric?" : "Restart the game?", Theme::title_size - 4.0f);
		m_gui.TextWrapped(box.x + 40.0f, box.y + 96.0f, box.w - 80.0f,
			exit ? "The game stops. Progress since your last save is lost." :
				   "The game starts over. Progress since your last save is lost.",
			Theme::body_size, Theme::text_muted);
	}
	const float bw = 200.0f, by = box.Bottom() - 40.0f - 56.0f;
	if (m_gui.Button(GuiId("dialog_cancel"), {box.Right() - 40.0f - 2.0f * bw - 16.0f, by, bw, 56.0f}, "Cancel"))
	{
		m_dialog = Dialog::None;
		m_needs_draw = true;
	}
	if (m_gui.Button(GuiId("dialog_ok"), {box.Right() - 40.0f - bw, by, bw, 56.0f}, exit ? "Quit" : "Restart",
			exit ? Icon::exit : Icon::restart, Gui::Style::Danger))
	{
		if (exit)
			Emu::RequestShutdown();
		else
		{
			Emu::ResetGame();
			m_close_requested = true;
		}
		m_dialog = Dialog::None;
		m_needs_draw = true;
	}
	// A press outside the dialog dismisses it.
	if (!m_gui.Drawing() && m_gui.Input().pressed && !m_gui.Hovered(box))
	{
		m_dialog = Dialog::None;
		m_needs_draw = true;
	}
}
