// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "SettingsMenu.h"

#include "TexturePacks.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iterator>

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
	static constexpr uint32_t lightgun = 0xe19b; // gun
	static constexpr uint32_t textures = 0xf302; // images
	static constexpr uint32_t trash = 0xf1f8;
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
	constexpr float NAV_SPACING = 60.0f;
	constexpr float NAV_ITEM_HEIGHT = 58.0f;
	constexpr float PACK_ROW_HEIGHT = 76.0f;
	constexpr float PACK_ROW_GAP = 8.0f;

	// The gun colours offered, sRGB; the first is the default.
	struct Swatch
	{
		uint32_t rgb;
		const char* name;
	};
	constexpr Swatch GUN_COLORS[] = {{0x8C8C91, "Gray"}, {0x1E1E22, "Black"}, {0xE8E8EA, "White"}, {0xC0282D, "Red"},
		{0x2F5FD0, "Blue"}, {0x2E9E4F, "Green"}, {0xE8892B, "Orange"}, {0x7B45C8, "Purple"}};

	std::string FormatBytes(uint64_t bytes)
	{
		if (bytes >= 1000000000ull)
			return fmt::format("{:.1f} GB", static_cast<double>(bytes) / 1e9);
		if (bytes >= 1000000ull)
			return fmt::format("{} MB", (bytes + 500000) / 1000000);
		return fmt::format("{} KB", (bytes + 999) / 1000);
	}
} // namespace

SettingsMenu::SettingsMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls, Font& text,
	Font& icons)
	: m_settings(settings)
	, m_web(web)
	, m_controls(controls)
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
	m_dialog_only = false;
	m_mapping.reset();
	m_running_serial = Emu::GetSerial();
	if (m_page == Page::Textures)
		LoadTexturePacks();
	m_needs_draw = true;
}

void SettingsMenu::ShowQuitDialog()
{
	m_dialog = Dialog::Exit;
	m_dialog_only = true;
	m_needs_draw = true;
}

void SettingsMenu::ShowTransferPage()
{
	m_page = Page::Transfer;
	m_needs_draw = true;
}

void SettingsMenu::Scroll(float pixels)
{
	if (m_page != Page::Textures || m_dialog != Dialog::None)
		return;
	const float scroll = std::clamp(m_packs_scroll + pixels, 0.0f, m_packs_overflow);
	if (scroll != m_packs_scroll)
	{
		m_packs_scroll = scroll;
		m_needs_draw = true;
	}
}

void SettingsMenu::LoadTexturePacks()
{
	// One read at a time: replacing an unfinished future would wait for it here.
	if (m_packs_loading.valid())
	{
		m_packs_again = true;
		return;
	}
	m_packs_loading = std::async(std::launch::async, [root = Emu::GetTexturesFolder(), remove = std::exchange(m_remove_pack, {})]() {
		PackList list;
		if (!remove.empty())
		{
			const std::string title = Emu::GetGameTitle(remove);
			std::string error;
			if (TexturePacks::Remove(root, remove, &error))
			{
				Emu::TexturePackChanged(remove, false);
				list.notice = fmt::format("Texture pack of {} removed", title.empty() ? remove : title);
			}
			else
				list.notice = error;
		}
		for (const TexturePacks::Pack& pack : TexturePacks::List(root))
			list.rows.push_back({pack.serial, Emu::GetGameTitle(pack.serial), pack.textures, pack.bytes});
		// By title; the unknown games, by serial, last.
		std::sort(list.rows.begin(), list.rows.end(), [](const PackRow& a, const PackRow& b) {
			if (a.title.empty() != b.title.empty())
				return b.title.empty();
			return a.title.empty() ? a.serial < b.serial : a.title < b.title;
		});
		return list;
	});
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
		web.phase != m_web_status.phase ||
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
	// The texture packs: a read done, or an upload over (it may have added a pack and turned them on).
	if (m_packs_loading.valid() && m_packs_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		PackList list = m_packs_loading.get();
		m_packs = std::move(list.rows);
		m_packs_loaded = true;
		if (!list.notice.empty())
		{
			m_toast = std::move(list.notice);
			m_toast_until = now + std::chrono::seconds(3);
		}
		if (std::exchange(m_packs_again, false))
			LoadTexturePacks();
		m_needs_draw |= m_page == Page::Textures;
	}
	if (const bool busy = !m_web_status.transfer.empty(); busy != m_web_busy)
	{
		m_web_busy = busy;
		if (!busy)
		{
			m_core = Emu::GetCoreSettings();
			if (m_page == Page::Textures)
				LoadTexturePacks();
		}
	}

	UpdateMapping();

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
	if (m_dialog_only)
	{
		if (m_dialog != Dialog::None)
			RunDialog();
		if (m_dialog == Dialog::None)
			m_close_requested = true;
		return;
	}
	if (m_gui.Drawing())
	{
		Canvas& c = m_gui.Draw();
		c.FillRect({0, 0, WIDTH, HEIGHT}, Theme::panel, RADIUS);
		// The nav bar: the panel's left part, square on its right edge.
		c.SetClip({0, 0, NAV_WIDTH, HEIGHT});
		c.FillRect({0, 0, NAV_WIDTH + RADIUS, HEIGHT}, Theme::nav, RADIUS);
		c.ResetClip();
	}

	m_gui.SetBlocked(m_dialog != Dialog::None || m_mapping);
	RunNav();

	static constexpr std::array<std::string_view, 9> titles = {
		"Display", "3D", "Graphics", "Texture Packs", "Audio", "Game", "Wi-Fi Transfer", "Controls", "Light Gun"};
	m_gui.Text(CONTENT_X, 42.0f, titles[static_cast<size_t>(m_page)], Theme::title_size);
	if (m_gui.IconButton(GuiId("close"), {CONTENT_RIGHT - 52.0f, 36.0f, 60.0f, 60.0f}, Icon::close, 28.0f))
		m_close_requested = true;

	m_y = PAGE_TOP;
	m_row_gap = 22.0f;
	switch (m_page)
	{
		case Page::Display: RunDisplay(); break;
		case Page::Stereo: RunStereo(); break;
		case Page::Graphics: RunGraphics(); break;
		case Page::Textures: RunTextures(); break;
		case Page::Audio: RunAudio(); break;
		case Page::Game: RunGame(); break;
		case Page::Transfer: RunTransfer(); break;
		case Page::Controls: RunControls(); break;
		case Page::LightGun: RunLightGun(); break;
	}

	if (!m_toast.empty() && m_gui.Drawing())
	{
		const float w = m_gui.TextWidth(m_toast, Theme::body_size) + 56.0f;
		const Rect r{std::round(CONTENT_X + 0.5f * (CONTENT_RIGHT - CONTENT_X - w)), HEIGHT - 72.0f, w, 48.0f};
		m_gui.Draw().FillRect(r, Theme::control_hover, 24.0f);
		m_gui.TextCentered(r, m_toast, Theme::body_size, Theme::text);
	}

	m_gui.SetBlocked(false);
	if (m_dialog != Dialog::None)
		RunDialog();
	if (m_mapping)
		RunMappingDialog();
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
	static constexpr std::array<Item, 9> items = {{
		{Page::Display, Icon::display, "Display"},
		{Page::Stereo, Icon::stereo, "3D"},
		{Page::Graphics, Icon::graphics, "Graphics"},
		{Page::Textures, Icon::textures, "Texture Packs"},
		{Page::Audio, Icon::audio, "Audio"},
		{Page::Game, Icon::game, "Game"},
		{Page::Transfer, Icon::transfer, "Wi-Fi Transfer"},
		{Page::Controls, Icon::controls, "Controls"},
		{Page::LightGun, Icon::lightgun, "Light Gun"},
	}};
	float y = PAGE_TOP;
	for (const Item& item : items)
	{
		if (m_gui.NavItem(GuiId("nav", static_cast<int>(item.page)), {16.0f, y, NAV_WIDTH - 32.0f, NAV_ITEM_HEIGHT},
				item.icon, item.label, m_page == item.page) &&
			m_page != item.page)
		{
			m_page = item.page;
			if (m_page == Page::Game)
				Emu::RefreshSaveSlots();
			if (m_page == Page::Textures)
			{
				m_packs_scroll = 0.0f;
				LoadTexturePacks();
			}
			m_needs_draw = true;
		}
		y += NAV_SPACING;
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
	m_y = top + h + m_row_gap;
	if (m_gui.Drawing())
		m_gui.Draw().FillRect({CONTENT_X, m_y - 0.5f * m_row_gap, CONTENT_RIGHT - CONTENT_X, 1.0f}, Theme::border.WithAlpha(0.6f));
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
	m_gui.SetDisabled(!m_passthrough_available);
	bool passthrough = vr.passthrough && m_passthrough_available;
	if (m_gui.Toggle(GuiId("passthrough"),
			Row("Passthrough", "Shows your room around the screen instead of the dark void. The eye button below "
							   "the cube does the same.",
				52.0f, !m_passthrough_available),
			&passthrough))
	{
		vr.passthrough = passthrough;
		changed = true;
	}
	m_gui.SetDisabled(false);
	if (!m_passthrough_available)
		Note("SteamVR offers no passthrough here.", Theme::text_muted);
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

	static constexpr std::array<std::string_view, 3> modes = {"Flat", "Volumetric", "Volumetric+"};
	int mode = vr.Mode3D();
	if (m_gui.Segmented(GuiId("stereo"),
			Row("3D mode", "Volumetric draws the game for each eye, so the world sits behind the screen like a "
						   "window. Volumetric+ (test) gives the world its full depth, and leaning in shows more of "
						   "it. Flat shows a flat picture. The cube button beside the screen switches it too.",
				52.0f),
			modes, &mode))
	{
		vr.SetMode3D(mode);
		changed = true;
	}

	// Volumetric+ takes its depth from the screen distance: the farthest scenery is at infinity.
	if (!vr.stereo_plus || !vr.stereo)
		changed |= m_gui.Slider(GuiId("stereo_depth"),
			Row("Depth", "How far behind the screen the farthest scenery appears."), &vr.stereo_depth, 0.0f, 5.0f, 0.1f,
			fmt::format("{:.1f} m", vr.stereo_depth));

	Section("ADVANCED");
	// In the game's normalised Z (the depth buffer).
	float convergence = vr.stereo_convergence;
	if (m_gui.Slider(GuiId("stereo_convergence"),
			Row("Screen plane", "Which depth in the scene sits on the screen glass. Set it so the nearest objects "
								"just touch the glass."),
			&convergence, 0.01f, 1.0f, 0.005f, fmt::format("{:.3f}", convergence)))
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

	// AccBlendLevel 0..3; Full and Maximum cost far too much here.
	static constexpr std::array<std::string_view, 4> blendings = {"Minimum", "Basic", "Medium", "High"};
	int blending = std::clamp(core.blending, 0, 3);
	if (m_gui.Segmented(GuiId("blending"),
			Row("Blending accuracy", "How exactly see-through effects are drawn. Higher levels draw some effects "
									 "piece by piece, which can make busy scenes stutter."),
			blendings, &blending))
	{
		core.blending = blending;
		SetCore(core);
	}
}

void SettingsMenu::RunTextures()
{
	// The core's texture replacement settings, as ARMSX2's Android and iOS apps offer them.
	m_row_gap = 16.0f;
	Emu::CoreSettings core = m_core;
	if (m_gui.Toggle(GuiId("texture_packs"),
			Row("Texture packs", "Shows a game with the HD textures of its pack. Add packs with Wi-Fi Transfer."),
			&core.texture_packs))
		SetCore(core);
	m_gui.SetDisabled(!core.texture_packs);
	if (m_gui.Toggle(GuiId("texture_packs_async"),
			Row("Load in the background", "The game runs on while textures load; they show a moment later."),
			&core.texture_packs_async))
		SetCore(core);
	if (m_gui.Toggle(GuiId("texture_packs_precache"),
			Row("Preload", "Loads the whole pack as the game starts: no pop-in, but a slower start and more memory."),
			&core.texture_packs_precache))
		SetCore(core);
	m_gui.SetDisabled(false);

	Section(m_packs.empty() ? std::string("GAMES WITH A TEXTURE PACK") :
	                          fmt::format("GAMES WITH A TEXTURE PACK ({})", m_packs.size()));
	const Rect view{CONTENT_X, m_y - 6.0f, CONTENT_RIGHT - CONTENT_X - 22.0f, HEIGHT - 24.0f - (m_y - 6.0f)};
	if (m_packs.empty())
	{
		if (m_gui.Drawing())
		{
			const Rect box{CONTENT_X, view.y, CONTENT_RIGHT - CONTENT_X, std::min(view.h, 150.0f)};
			m_gui.Draw().FillRect(box, Theme::nav, 16.0f);
			m_gui.Draw().DrawIcon(m_gui.IconFont(), 40.0f, {box.x + 28.0f, box.y, 56.0f, box.h}, Icon::textures,
				Theme::control_hover);
			m_gui.TextWrapped(box.x + 108.0f, box.y + 0.5f * box.h - 28.0f, box.w - 136.0f,
				m_packs_loaded ? "No texture packs yet. Send one from a computer or phone: Wi-Fi Transfer, Texture "
				                 "Packs tab (zip, 7z or RAR)." :
				                 "Looking for texture packs…",
				Theme::body_size, Theme::text_muted);
		}
		m_packs_overflow = 0.0f;
		return;
	}

	const float content = static_cast<float>(m_packs.size()) * (PACK_ROW_HEIGHT + PACK_ROW_GAP) - PACK_ROW_GAP;
	m_packs_overflow = std::max(content - view.h, 0.0f);
	m_gui.ScrollBar(GuiId("packs_scroll"), {view.Right() + 8.0f, view.y, 14.0f, view.h}, &m_packs_scroll, content, view.h);
	m_packs_scroll = std::clamp(m_packs_scroll, 0.0f, m_packs_overflow);
	m_gui.SetClip(view);
	for (size_t i = 0; i < m_packs.size(); i++)
	{
		const PackRow& pack = m_packs[i];
		const Rect row{view.x, view.y + static_cast<float>(i) * (PACK_ROW_HEIGHT + PACK_ROW_GAP) - m_packs_scroll, view.w,
			PACK_ROW_HEIGHT};
		if (row.Bottom() < view.y || row.y > view.Bottom())
			continue;
		const bool running = !m_running_serial.empty() && pack.serial == m_running_serial;
		const Rect remove{row.Right() - 12.0f - 52.0f, row.y + 0.5f * (row.h - 52.0f), 52.0f, 52.0f};
		constexpr float badge_w = 120.0f;
		if (m_gui.Drawing())
		{
			Canvas& c = m_gui.Draw();
			c.FillRect(row, running ? Theme::item_selected.WithAlpha(0.6f) : Theme::nav, 14.0f);
			c.DrawIcon(m_gui.IconFont(), 26.0f, {row.x + 18.0f, row.y, 40.0f, row.h}, Icon::textures,
				running ? Theme::accent : Theme::text_muted);
			const float text_x = row.x + 74.0f;
			const float text_w = remove.x - 12.0f - (running ? badge_w + 12.0f : 0.0f) - text_x;
			m_gui.Text(text_x, row.y + 10.0f,
				m_gui.Elide(pack.title.empty() ? pack.serial : pack.title, Theme::label_size, text_w), Theme::label_size);
			const std::string details = fmt::format("{}{} · {} texture{} · {}", pack.serial,
				pack.title.empty() ? " (not in the game database)" : "", pack.textures, pack.textures == 1 ? "" : "s",
				FormatBytes(pack.bytes));
			m_gui.Text(text_x, row.y + 44.0f, m_gui.Elide(details, Theme::small_size, text_w), Theme::small_size,
				Theme::text_muted);
			if (running)
			{
				const Rect badge{remove.x - 12.0f - badge_w, row.y + 0.5f * (row.h - 40.0f), badge_w, 40.0f};
				c.FillRect(badge, Theme::accent.WithAlpha(0.25f), 20.0f);
				m_gui.TextCentered(badge, "Playing", Theme::small_size, Theme::accent_hover);
			}
		}
		if (m_gui.IconButton(GuiId("pack_remove", static_cast<int>(i)) ^ static_cast<uint32_t>(std::hash<std::string>{}(pack.serial)),
				remove, Icon::trash, 22.0f, Theme::text_muted, Theme::danger_hover))
		{
			m_remove_pack = pack.serial;
			m_dialog = Dialog::RemovePack;
			m_needs_draw = true;
		}
	}
	m_gui.SetClip(std::nullopt);
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
			Row("Wi-Fi transfer", "Add games, BIOS files and texture packs from a computer or phone on the same "
								  "Wi-Fi network, in its web browser.", 52.0f, !on && !web.error.empty()),
			&on))
	{
		if (on)
			m_web.Start(m_settings.Get().web_require_code);
		else
			m_web.Stop();
		m_web_status = m_web.GetStatus();
		// Remembered: on again at the next launch.
		VrSettings remembered = m_settings.Get();
		remembered.web_enabled = on;
		SetVr(remembered);
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
		m_gui.Text(CONTENT_X, m_y, fmt::format("{} {}", web.phase, web.transfer), Theme::body_size);
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
		m_gui.TextWrapped(CONTENT_X, m_y, CONTENT_RIGHT - CONTENT_X,
			"Files sent from the browser go into the emulator's BIOS, games and texture pack folders. Zip, 7z and "
			"RAR archives are unpacked.",
			Theme::small_size, Theme::text_muted);
}

void SettingsMenu::SetGunMode(bool has_game, bool on, bool chosen)
{
	if (has_game == m_has_game && on == m_gun_on && chosen == m_gun_chosen)
		return;
	m_has_game = has_game;
	m_gun_on = on;
	m_gun_chosen = chosen;
	m_needs_draw |= m_page == Page::LightGun;
}

std::string SettingsMenu::TargetName(const MapTarget& target) const
{
	if (target.gun < 0)
		return ControlMap::Name(static_cast<Ps2Control>(target.control));
	return fmt::format("Gun {} {}", target.gun + 1, ControlMap::Name(static_cast<GunControl>(target.control)));
}

std::string SettingsMenu::InputLabel(const MapTarget& target) const
{
	const auto get = [this, &target](int control) {
		return target.gun < 0 ? m_controls.Get(static_cast<Ps2Control>(control)) :
		                        m_controls.Get(target.gun, static_cast<GunControl>(control));
	};
	const int start = target.gun < 0 ? static_cast<int>(Ps2Control::Start) : static_cast<int>(GunControl::Start);
	const int select = target.gun < 0 ? static_cast<int>(Ps2Control::Select) : static_cast<int>(GunControl::Select);
	const ControlInput input = get(target.control);
	std::string label = ControlMap::Name(input, m_frame_controllers);
	// Start and Select on one input: a tap is Start, a hold Select.
	if ((target.control == start || target.control == select) && input != ControlInput::None && get(start) == get(select))
		label += target.control == start ? " (tap)" : " (hold)";
	return label;
}

void SettingsMenu::RunControls()
{
	// Which set: the DualShock 2's, or a light gun's (and which gun).
	static constexpr std::array<std::string_view, 2> tabs = {"DualShock 2", "GunCon"};
	int tab = m_controls_guncon ? 1 : 0;
	if (m_gui.Segmented(GuiId("controls_tab"), {CONTENT_X, m_y - 4.0f, 300.0f, 48.0f}, tabs, &tab))
	{
		m_controls_guncon = tab == 1;
		m_needs_draw = true;
	}
	if (m_controls_guncon)
	{
		static constexpr std::array<std::string_view, 2> guns = {"Gun 1", "Gun 2"};
		if (m_gui.Segmented(GuiId("controls_gun"), {CONTENT_X + 316.0f, m_y - 4.0f, 200.0f, 48.0f}, guns, &m_controls_gun))
			m_needs_draw = true;
	}
	if (m_gui.Button(GuiId("controls_reset"), {CONTENT_RIGHT - 240.0f, m_y - 4.0f, 240.0f, 48.0f}, "Reset to defaults",
			Icon::restart))
	{
		if (m_controls_guncon)
			m_controls.ResetGunToDefaults(m_controls_gun);
		else
			m_controls.ResetToDefaults();
		m_toast = m_controls_guncon ? fmt::format("Gun {} controls reset to the defaults", m_controls_gun + 1) :
		                              "Controls reset to the defaults";
		m_toast_until = Clock::now() + std::chrono::seconds(3);
		m_needs_draw = true;
	}
	// Whether the hands without a gun show their controllers, at the right; the help beside it.
	constexpr float toggle_w = 290.0f;
	const Rect show{CONTENT_RIGHT - toggle_w, m_y + 58.0f, toggle_w, 44.0f};
	VrSettings vr = m_settings.Get();
	if (m_gui.Toggle(GuiId("show_controllers"), show, &vr.show_controllers))
		SetVr(vr);
	m_gui.Text(show.x, std::round(show.y + 0.5f * (show.h - m_gui.TextFont().LineHeight(Theme::body_size))), "Show VR Controllers",
		Theme::body_size);
	const std::string_view controllers = m_frame_controllers ? "Steam Frame controllers" : "Touch layout";
	m_gui.TextWrapped(CONTENT_X, m_y + 56.0f, show.x - 32.0f - CONTENT_X,
		m_controls_guncon ?
			fmt::format("{}: gun 1 is in your gun hand, gun 2 in the other. Used while light gun mode is on.", controllers) :
			fmt::format("{}: click a control, then press what you want for it. Triggers still click in menus.", controllers),
		Theme::small_size, Theme::text_muted);

	// Two columns: nine rows for the pad, seven for a gun.
	const int count = m_controls_guncon ? static_cast<int>(GunControl::Count) : static_cast<int>(Ps2Control::Count);
	const int rows = m_controls_guncon ? 7 : 9;
	constexpr float gap = 16.0f, row_h = 48.0f, row_gap = 6.0f; // ends above the toasts
	const float col_w = 0.5f * (CONTENT_RIGHT - CONTENT_X - gap);
	const float top = m_y + 110.0f;
	for (int i = 0; i < count; i++)
	{
		const MapTarget target{m_controls_guncon ? m_controls_gun : -1, i};
		const Rect cell{CONTENT_X + static_cast<float>(i / rows) * (col_w + gap),
			top + static_cast<float>(i % rows) * (row_h + row_gap), col_w, row_h};
		bool hovered = false;
		if (m_gui.Clickable(GuiId(m_controls_guncon ? "map_gun" : "map", i), cell, &hovered))
		{
			m_mapping = target;
			m_mapping_armed = false;
			m_mapping_until = Clock::now() + std::chrono::seconds(5);
			m_needs_draw = true;
		}
		if (!m_gui.Drawing())
			continue;
		Canvas& c = m_gui.Draw();
		c.FillRect(cell, hovered ? Theme::item_hover : Theme::nav, 12.0f);
		const float text_y = std::round(cell.y + 0.5f * (cell.h - m_gui.TextFont().LineHeight(Theme::body_size)));
		m_gui.Text(cell.x + 18.0f, text_y,
			m_controls_guncon ? ControlMap::Name(static_cast<GunControl>(i)) : ControlMap::Name(static_cast<Ps2Control>(i)),
			Theme::body_size);
		const ControlInput input = m_controls_guncon ? m_controls.Get(m_controls_gun, static_cast<GunControl>(i)) :
		                                               m_controls.Get(static_cast<Ps2Control>(i));
		// A gun input both guns use goes to gun 2 when there are two.
		const bool shared = m_controls_guncon && m_controls.GunsShare(input);
		const std::string label = InputLabel(target) + (shared ? " *" : "");
		const float label_w = m_gui.TextWidth(label, Theme::small_size) + 28.0f;
		const Rect pill{cell.Right() - label_w - 10.0f, cell.y + 8.0f, label_w, cell.h - 16.0f};
		c.FillRect(pill, hovered ? Theme::accent.WithAlpha(0.25f) : Theme::control, 0.5f * pill.h);
		// An unmapped pad control is a gap; a gun has more buttons than hands, so some go without.
		const Color ink = input == ControlInput::None ? (m_controls_guncon ? Theme::text_muted.WithAlpha(0.6f) : Theme::danger) :
		                  shared                      ? Theme::accent :
		                  hovered                     ? Theme::accent_hover :
		                                                Theme::text_muted;
		m_gui.TextCentered(pill, label, Theme::small_size, ink);
	}
	if (!m_controls_guncon)
		return;
	// The free cell below the second column: the beam, for both guns (the Light Gun page's Beam
	// setting: off leaves the dot, as its Dot choice does).
	const Rect beam_cell{CONTENT_X + col_w + gap, top + 6.0f * (row_h + row_gap), col_w, row_h};
	vr = m_settings.Get();
	bool beam = vr.gun_beam < 0.5f;
	if (m_gui.Drawing())
	{
		m_gui.Draw().FillRect(beam_cell, Theme::nav, 12.0f);
		m_gui.Text(beam_cell.x + 18.0f, std::round(beam_cell.y + 0.5f * (beam_cell.h - m_gui.TextFont().LineHeight(Theme::body_size))),
			"Gun Beam", Theme::body_size);
	}
	if (m_gui.Toggle(GuiId("gun_beam_toggle"), beam_cell.Inset(10.0f, 0.0f), &beam))
	{
		vr.gun_beam = beam ? 0.0f : 1.0f;
		SetVr(vr);
	}
	const float below = top + 7.0f * (row_h + row_gap);
	m_gui.Text(CONTENT_X, below + 6.0f,
		"* in both guns' sets: with two guns, gun 2 has it. Pedal presses the Light Gun page's pedal button.",
		Theme::small_size, Theme::text_muted);

	// The gun's colour: gun 1 has one for playing alone and one for two players.
	vr = m_settings.Get();
	bool changed = false;
	const Rect left{CONTENT_X, below + 48.0f, col_w, 72.0f};
	const Rect right{CONTENT_X + col_w + gap, left.y, col_w, left.h};
	if (m_controls_gun == 0)
	{
		changed |= GunColor("gun_color", "Colour, one player", left, &vr.gun_color);
		changed |= GunColor("gun_color_2p", "Colour, two players", right, &vr.gun_color_2p);
	}
	else
		changed |= GunColor("gun2_color", "Colour", left, &vr.gun2_color);
	if (changed)
		SetVr(vr);
}

bool SettingsMenu::GunColor(std::string_view id, std::string_view label, const Rect& area, uint32_t* rgb)
{
	// A colour not offered here (set in VR.ini) leaves none selected.
	const auto preset = std::find_if(std::begin(GUN_COLORS), std::end(GUN_COLORS),
		[&](const Swatch& sw) { return sw.rgb == *rgb; });
	m_gui.Text(area.x, area.y, fmt::format("{}: {}", label, preset == std::end(GUN_COLORS) ? FormatColor(*rgb) : preset->name),
		Theme::small_size, Theme::text_muted);
	const Rect row{area.x, area.y + 30.0f, area.w, area.h - 30.0f};
	constexpr int num_colours = static_cast<int>(std::size(GUN_COLORS));
	const float swatch_gap = 7.0f;
	const float swatch_w = (row.w - swatch_gap * (num_colours - 1)) / num_colours;
	bool picked = false;
	for (int i = 0; i < num_colours; i++)
	{
		const Rect r{row.x + static_cast<float>(i) * (swatch_w + swatch_gap), row.y, swatch_w, row.h};
		bool hovered = false;
		if (m_gui.Clickable(GuiId(id, i), r, &hovered))
		{
			*rgb = GUN_COLORS[i].rgb;
			picked = true;
		}
		if (m_gui.Drawing())
		{
			Canvas& c = m_gui.Draw();
			const bool selected = GUN_COLORS[i].rgb == *rgb;
			c.FillRect(r.Inset(selected || hovered ? 5.0f : 3.0f, selected || hovered ? 5.0f : 3.0f), Rgb(GUN_COLORS[i].rgb), 10.0f);
			if (selected)
				c.StrokeRect(r, Theme::accent, 12.0f, 3.0f);
			else if (hovered)
				c.StrokeRect(r, Theme::border, 12.0f, 2.0f);
		}
	}
	return picked;
}

void SettingsMenu::RunLightGun()
{
	// Many rows: tighter than the other pages.
	m_row_gap = 16.0f;
	constexpr float h = 46.0f;
	VrSettings vr = m_settings.Get();
	bool changed = false;

	// Per game: automatic (on for light gun games), or the user's choice.
	const std::string automatic = fmt::format("Auto ({})", m_gun_on && !m_gun_chosen ? "on" : "off");
	const std::array<std::string_view, 3> modes = {m_gun_chosen ? std::string_view("Auto") : std::string_view(automatic),
		"Off", "On"};
	int mode = m_gun_chosen ? (m_gun_on ? 2 : 1) : 0;
	m_gui.SetDisabled(!m_has_game);
	if (m_gui.Segmented(GuiId("gun_mode"),
			Row("Light gun mode", m_has_game ? "For this game." : "Start a game first.", h),
			modes, &mode))
	{
		m_gun_mode_request = mode == 0 ? GunModeRequest::Auto : mode == 1 ? GunModeRequest::Off : GunModeRequest::On;
	}
	m_gui.SetDisabled(false);

	static constexpr std::array<std::string_view, 2> guns = {"One gun", "Two guns"};
	int two = vr.gun_players > 1.5f ? 1 : 0;
	if (m_gui.Segmented(GuiId("gun_players"), Row("Guns", "Two: one gun in each hand.", h), guns, &two))
	{
		vr.gun_players = two ? 2.0f : 1.0f;
		changed = true;
	}
	static constexpr std::array<std::string_view, 2> hands = {"Right", "Left"};
	int hand = vr.gun_left_hand ? 1 : 0;
	if (m_gui.Segmented(GuiId("gun_hand"), Row("Gun hand", "", h), hands, &hand))
	{
		vr.gun_left_hand = hand == 1;
		changed = true;
	}

	static constexpr std::array<std::string_view, 3> beams = {"Beam and dot", "Dot", "Off"};
	int beam = std::clamp(static_cast<int>(vr.gun_beam + 0.5f), 0, 2);
	if (m_gui.Segmented(GuiId("gun_beam"), Row("Beam", "", h), beams, &beam))
	{
		vr.gun_beam = static_cast<float>(beam);
		changed = true;
	}
	static constexpr std::array<std::string_view, 3> pedals = {"A", "B", "C"};
	int pedal = std::clamp(static_cast<int>(vr.gun_pedal + 0.5f), 0, 2);
	if (m_gui.Segmented(GuiId("gun_pedal"), Row("Pedal button", "Takes cover in the game.", h), pedals,
			&pedal))
	{
		vr.gun_pedal = static_cast<float>(pedal);
		changed = true;
	}
	float recoil = std::round(vr.gun_haptics * 100.0f);
	if (m_gui.Slider(GuiId("gun_haptics"), Row("Recoil", "", h), &recoil, 0.0f,
			100.0f, 10.0f, recoil > 0.0f ? fmt::format("{}%", static_cast<int>(recoil)) : std::string("Off")))
	{
		vr.gun_haptics = recoil / 100.0f;
		changed = true;
	}
	if (m_gui.Slider(GuiId("gun_pitch"), Row("Gun angle", "", h),
			&vr.gun_pitch, -30.0f, 30.0f, 1.0f, fmt::format("{}°", static_cast<int>(std::round(vr.gun_pitch)))))
		changed = true;
	if (changed)
		SetVr(vr);

	// Done once the menu has closed and the game runs again.
	const Rect r = Row("Calibration", "", h);
	const float w = 0.5f * (r.w - 16.0f);
	m_gui.SetDisabled(!m_has_game || !m_gun_on);
	if (m_gui.Button(GuiId("gun_recalibrate"), {r.x, r.y, w, r.h}, "Calibrate", Icon::lightgun))
	{
		m_gun_action = GunAction::Recalibrate;
		m_close_requested = true;
	}
	if (m_gui.Button(GuiId("gun_reload"), {r.Right() - w, r.y, w, r.h}, "Reload", Icon::restart))
	{
		m_gun_action = GunAction::Reload;
		m_close_requested = true;
	}
	m_gui.SetDisabled(false);
}

void SettingsMenu::UpdateMapping()
{
	const Clock::time_point now = Clock::now();
	if (m_mapping)
	{
		// Wait for everything to be let go (the trigger that clicked the control, say), then take the
		// first input pressed: a stick for a stick, a button (triggers included) otherwise.
		const MapTarget target = *m_mapping;
		const bool stick = target.gun < 0 && ControlMap::IsStick(static_cast<Ps2Control>(target.control));
		if (!m_mapping_armed)
			m_mapping_armed = ControlMap::AllReleased(m_controller);
		else if (const ControlInput input = ControlMap::NewlyPressed(m_controller_before, m_controller, stick);
				 input != ControlInput::None)
		{
			if (target.gun < 0)
				m_controls.Set(static_cast<Ps2Control>(target.control), input);
			else
				m_controls.Set(target.gun, static_cast<GunControl>(target.control), input);
			m_toast = fmt::format("{}: {}", TargetName(target), InputLabel(target));
			m_toast_until = now + std::chrono::seconds(3);
			m_mapping.reset();
			m_mapping_done = true;
		}
		if (m_mapping && now >= m_mapping_until)
		{
			m_toast = fmt::format("{} unchanged", TargetName(target));
			m_toast_until = now + std::chrono::seconds(3);
			m_mapping.reset();
		}
		m_needs_draw = true; // the countdown moves, or the dialog went away
	}
	m_controller_before = m_controller;
}

void SettingsMenu::RunMappingDialog()
{
	if (!m_gui.Drawing())
		return;
	const MapTarget target = *m_mapping;
	const bool stick = target.gun < 0 && ControlMap::IsStick(static_cast<Ps2Control>(target.control));
	const std::string name = TargetName(target);
	const Rect box{0.5f * (WIDTH - 620.0f), 0.5f * (HEIGHT - 300.0f), 620.0f, 300.0f};
	Canvas& c = m_gui.Draw();
	c.FillRect({0, 0, WIDTH, HEIGHT}, Rgb(0x000000, 140), RADIUS);
	c.FillRect(box, Theme::nav, 24.0f);
	c.StrokeRect(box, Theme::border, 24.0f, 2.0f);
	m_gui.Text(box.x + 40.0f, box.y + 36.0f, fmt::format("Map {}", name), Theme::title_size - 4.0f);
	m_gui.TextWrapped(box.x + 40.0f, box.y + 96.0f, box.w - 80.0f,
		stick ? fmt::format("Move the stick you want for the {}.", name) :
				fmt::format("Press the button, grip or trigger you want for {}. It now has: {}.", name, InputLabel(target)),
		Theme::body_size, Theme::text_muted);
	// The time left, as a bar that runs out.
	const float left = std::chrono::duration<float>(m_mapping_until - Clock::now()).count();
	const Rect bar{box.x + 40.0f, box.Bottom() - 72.0f, box.w - 80.0f, 8.0f};
	c.FillRect(bar, Theme::control, 4.0f);
	c.FillRect({bar.x, bar.y, bar.w * std::clamp(left / 5.0f, 0.0f, 1.0f), bar.h}, Theme::accent, 4.0f);
	m_gui.Text(bar.x, bar.y + 18.0f, fmt::format("Cancels in {} s", static_cast<int>(std::ceil(std::max(left, 0.0f)))),
		Theme::small_size, Theme::text_muted);
}

void SettingsMenu::RunDialog()
{
	const bool exit = m_dialog == Dialog::Exit;
	const bool remove = m_dialog == Dialog::RemovePack;
	const Rect box{0.5f * (WIDTH - 600.0f), 0.5f * (HEIGHT - 300.0f), 600.0f, 300.0f};
	if (m_gui.Drawing())
	{
		Canvas& c = m_gui.Draw();
		c.FillRect({0, 0, WIDTH, HEIGHT}, Rgb(0x000000, 140), RADIUS);
		c.FillRect(box, Theme::nav, 24.0f);
		c.StrokeRect(box, Theme::border, 24.0f, 2.0f);
		std::string title = exit ? "Quit ARMSX2 VR?" : remove ? "Remove the texture pack?" : "Restart the game?";
		std::string text = exit ? "The game stops. Progress since your last save is lost." :
		                          "The game starts over. Progress since your last save is lost.";
		if (remove)
		{
			const auto pack = std::find_if(m_packs.begin(), m_packs.end(), [this](const PackRow& p) { return p.serial == m_remove_pack; });
			const std::string game = pack != m_packs.end() && !pack->title.empty() ? pack->title : m_remove_pack;
			text = fmt::format("The HD textures of {} are deleted from the headset. The game itself stays.", game);
		}
		m_gui.Text(box.x + 40.0f, box.y + 36.0f, title, Theme::title_size - 4.0f);
		m_gui.TextWrapped(box.x + 40.0f, box.y + 96.0f, box.w - 80.0f, text, Theme::body_size, Theme::text_muted);
	}
	const float bw = 200.0f, by = box.Bottom() - 40.0f - 56.0f;
	const auto dismiss = [this]() {
		m_dialog = Dialog::None;
		m_remove_pack.clear();
		m_needs_draw = true;
	};
	if (m_gui.Button(GuiId("dialog_cancel"), {box.Right() - 40.0f - 2.0f * bw - 16.0f, by, bw, 56.0f}, "Cancel"))
		dismiss();
	if (m_gui.Button(GuiId("dialog_ok"), {box.Right() - 40.0f - bw, by, bw, 56.0f}, exit ? "Quit" : remove ? "Remove" : "Restart",
			exit ? Icon::exit : remove ? Icon::trash : Icon::restart, Gui::Style::Danger))
	{
		if (exit)
			Emu::RequestShutdown();
		else if (remove)
			LoadTexturePacks(); // removes m_remove_pack first
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
		dismiss();
}
