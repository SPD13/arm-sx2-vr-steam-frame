// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GameListMenu.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>

namespace Icon
{
	static constexpr uint32_t games = 0xf11b; // gamepad
	static constexpr uint32_t close = 0xf00d; // xmark
	static constexpr uint32_t star = 0xf005;
	static constexpr uint32_t play = 0xf04b;
	static constexpr uint32_t restart = 0xf2ea; // rotate-left
} // namespace Icon

namespace
{
	constexpr float PAD = 40.0f;
	constexpr float RADIUS = 28.0f;
	constexpr float LIST_TOP = 168.0f;
	constexpr float LIST_BOTTOM = GameListMenu::HEIGHT - 28.0f;
	constexpr float LIST_LEFT = 28.0f;
	constexpr float LIST_RIGHT = GameListMenu::WIDTH - 28.0f - 22.0f; // the scroll bar to its right
	constexpr float ROW_HEIGHT = 84.0f;
	constexpr float ROW_GAP = 6.0f;
	constexpr Color star_on = Rgb(0xf5c542);

	std::string Canonical(const std::string& path)
	{
		std::error_code ec;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
		return ec ? path : canonical.string();
	}

	std::string FormatSize(uint64_t bytes)
	{
		if (bytes >= 1000000000ull)
			return fmt::format("{:.1f} GB", static_cast<double>(bytes) / 1e9);
		return fmt::format("{} MB", (bytes + 500000) / 1000000);
	}

	std::string FormatPlayed(std::time_t seconds)
	{
		if (seconds < 60)
			return "Played less than a minute";
		if (seconds < 3600)
			return fmt::format("Played {} min", seconds / 60);
		return fmt::format("Played {} h {} min", seconds / 3600, (seconds % 3600) / 60);
	}

	// "<what> today at 14:32", "<what> yesterday at 09:10", "<what> 12 Oct 2026".
	std::string FormatWhen(std::string_view what, std::time_t when)
	{
		const std::time_t now = std::time(nullptr);
		std::tm then{}, today{};
		localtime_r(&when, &then);
		localtime_r(&now, &today);
		char clock[16];
		std::strftime(clock, sizeof(clock), "%H:%M", &then);
		const auto day_number = [](const std::tm& tm) { return tm.tm_year * 400 + tm.tm_yday; };
		const int days = day_number(today) - day_number(then);
		if (days == 0)
			return fmt::format("{} today at {}", what, clock);
		if (days == 1)
			return fmt::format("{} yesterday at {}", what, clock);
		char date[32];
		std::strftime(date, sizeof(date), "%d %b %Y", &then);
		return fmt::format("{} {}", what, date[0] == '0' ? date + 1 : date);
	}
} // namespace

GameListMenu::GameListMenu(std::string favorites_path, Font& text, Font& icons)
	: m_favorites_path(std::move(favorites_path))
	, m_gui(text, icons)
{
	m_canvas.Resize(WIDTH, HEIGHT);
	LoadFavorites();
}

void GameListMenu::LoadFavorites()
{
	std::ifstream file(m_favorites_path);
	std::string line;
	while (std::getline(file, line))
	{
		if (!line.empty())
			m_favorites.insert(line);
	}
}

void GameListMenu::SaveFavorites() const
{
	// Written aside, then moved over the old list.
	const std::string temp = m_favorites_path + ".new";
	{
		std::ofstream file(temp, std::ios::trunc);
		for (const std::string& path : m_favorites)
			file << path << '\n';
		if (!file)
			return;
	}
	std::error_code ec;
	std::filesystem::rename(temp, m_favorites_path, ec);
}

void GameListMenu::Refresh()
{
	Emu::RefreshGames();
	m_confirm = nullptr;
	m_needs_draw = true;
}

void GameListMenu::Scroll(float pixels)
{
	const float max_scroll = std::max(m_content_height - (LIST_BOTTOM - LIST_TOP), 0.0f);
	const float scroll = std::clamp(m_scroll + pixels, 0.0f, max_scroll);
	if (scroll != m_scroll && !m_confirm)
	{
		m_scroll = scroll;
		m_needs_draw = true;
	}
}

std::vector<const GameListMenu::Game*> GameListMenu::Shown() const
{
	std::vector<const Game*> shown;
	for (const Game& game : m_games)
	{
		const bool keep = m_tab == Tab::All || m_tab == Tab::RecentlyAdded ||
		                  (m_tab == Tab::RecentPlay && (game.info.last_played || IsRunning(game))) ||
		                  (m_tab == Tab::MostPlayed && game.info.played_seconds > 0) ||
		                  (m_tab == Tab::Favorites && m_favorites.count(game.key));
		if (keep)
			shown.push_back(&game);
	}
	const auto by_title = [](const Game* a, const Game* b) { return a->info.title < b->info.title; };
	if (m_tab == Tab::RecentlyAdded)
	{
		std::stable_sort(shown.begin(), shown.end(), [&](const Game* a, const Game* b) {
			return a->info.added != b->info.added ? a->info.added > b->info.added : by_title(a, b);
		});
	}
	else if (m_tab == Tab::RecentPlay)
	{
		// The running game first: it is being played now.
		std::stable_sort(shown.begin(), shown.end(), [&](const Game* a, const Game* b) {
			constexpr std::time_t now = std::numeric_limits<std::time_t>::max();
			const std::time_t ta = IsRunning(*a) ? now : a->info.last_played;
			const std::time_t tb = IsRunning(*b) ? now : b->info.last_played;
			return ta != tb ? ta > tb : by_title(a, b);
		});
	}
	else if (m_tab == Tab::MostPlayed)
	{
		std::stable_sort(shown.begin(), shown.end(), [&](const Game* a, const Game* b) {
			return a->info.played_seconds != b->info.played_seconds ? a->info.played_seconds > b->info.played_seconds :
			                                                          by_title(a, b);
		});
	}
	else
		std::stable_sort(shown.begin(), shown.end(), by_title);
	return shown;
}

bool GameListMenu::Update(const GuiInput& input)
{
	// A new list from the scan, or a game started or stopped.
	if (const unsigned version = Emu::GetGamesVersion(); version != m_games_version)
	{
		m_games_version = version;
		m_confirm = nullptr; // it pointed into the old list
		m_games.clear();
		for (Emu::GameInfo& info : Emu::GetGames())
		{
			std::string key = Canonical(info.path);
			m_games.push_back({std::move(info), std::move(key)});
		}
		m_needs_draw = true;
	}
	if (const bool refreshing = Emu::IsRefreshingGames(); refreshing != m_refreshing)
	{
		m_refreshing = refreshing;
		m_needs_draw = true;
	}
	if (std::string running = Emu::GetRunningGamePath(); running != m_running_raw)
	{
		m_running = running.empty() ? std::string() : Canonical(running);
		m_running_raw = std::move(running);
		m_needs_draw = true;
	}
	const Clock::time_point now = Clock::now();
	if (std::string notice = Emu::TakeNotice(); !notice.empty())
	{
		m_toast = std::move(notice);
		m_toast_until = now + std::chrono::seconds(4);
		m_needs_draw = true;
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

void GameListMenu::Run()
{
	if (m_gui.Drawing())
	{
		Canvas& c = m_gui.Draw();
		c.FillRect({0, 0, WIDTH, HEIGHT}, Theme::panel, RADIUS);
		c.DrawIcon(m_gui.IconFont(), 30.0f, {PAD - 4.0f, 34.0f, 44.0f, 60.0f}, Icon::games, Theme::accent);
		m_gui.Text(PAD + 52.0f, 44.0f, "Games", Theme::title_size);
	}
	m_gui.SetBlocked(m_confirm != nullptr);

	static constexpr std::array<std::string_view, 5> tabs = {"All", "Recently Added", "Recent Play", "Most Played", "Favorites"};
	int tab = static_cast<int>(m_tab);
	if (m_gui.Segmented(GuiId("tabs"), {226.0f, 40.0f, 868.0f, 52.0f}, tabs, &tab))
	{
		m_tab = static_cast<Tab>(tab);
		m_scroll = 0.0f;
	}
	if (m_gui.IconButton(GuiId("close"), {WIDTH - PAD - 56.0f, 36.0f, 60.0f, 60.0f}, Icon::close, 28.0f))
		m_close_requested = true;

	RunList();

	if (!m_toast.empty() && m_gui.Drawing())
	{
		const float w = m_gui.TextWidth(m_toast, Theme::body_size) + 56.0f;
		const Rect r{std::round(0.5f * (WIDTH - w)), HEIGHT - 92.0f, w, 52.0f};
		m_gui.Draw().FillRect(r, Theme::control_hover, 26.0f);
		m_gui.TextCentered(r, m_toast, Theme::body_size, Theme::text);
	}

	m_gui.SetBlocked(false);
	if (m_confirm)
		RunDialog();
}

void GameListMenu::RunList()
{
	const std::vector<const Game*> shown = Shown();

	// Above the list: how many, and whether the folder is being read.
	const std::string count = shown.size() == 1 ? "1 game" : fmt::format("{} games", shown.size());
	m_gui.Text(LIST_LEFT + 12.0f, 120.0f, m_refreshing ? count + " · looking for new games…" : count, Theme::small_size,
		Theme::text_muted);

	const Rect view{LIST_LEFT, LIST_TOP, LIST_RIGHT - LIST_LEFT, LIST_BOTTOM - LIST_TOP};
	if (shown.empty())
	{
		static constexpr std::array<std::string_view, 5> empty = {
			"No games yet. Add them in Settings › Wi-Fi Transfer, from a computer or phone.",
			"No games yet. Add them in Settings › Wi-Fi Transfer, from a computer or phone.",
			"Games you play show up here.",
			"Games you play show up here, the most played first.",
			"Click the star next to a game to add it here.",
		};
		if (m_gui.Drawing())
		{
			m_gui.Draw().DrawIcon(m_gui.IconFont(), 56.0f, {view.x, view.y + 120.0f, view.w, 80.0f},
				m_tab == Tab::Favorites ? Icon::star : Icon::games, Theme::control_hover);
			const std::string_view text = m_refreshing && m_games.empty() ? "Looking for games…" : empty[static_cast<size_t>(m_tab)];
			m_gui.TextCentered({view.x, view.y + 210.0f, view.w, 40.0f}, text, Theme::body_size, Theme::text_muted);
		}
		m_content_height = 0.0f;
		return;
	}

	m_content_height = static_cast<float>(shown.size()) * (ROW_HEIGHT + ROW_GAP) - ROW_GAP;
	m_gui.ScrollBar(GuiId("scroll"), {LIST_RIGHT + 8.0f, LIST_TOP, 14.0f, view.h}, &m_scroll, m_content_height, view.h);
	m_scroll = std::clamp(m_scroll, 0.0f, std::max(m_content_height - view.h, 0.0f));

	m_gui.SetClip(view);
	const Game* clicked = nullptr;
	for (size_t i = 0; i < shown.size(); i++)
	{
		const Game& game = *shown[i];
		const Rect row{view.x, view.y + static_cast<float>(i) * (ROW_HEIGHT + ROW_GAP) - m_scroll, view.w, ROW_HEIGHT};
		if (row.Bottom() < view.y || row.y > view.Bottom())
			continue;
		const int index = static_cast<int>(i);
		const bool running = IsRunning(game);
		const bool favorite = m_favorites.count(game.key) != 0;

		// The row first, then the star on it, which takes the click when hit.
		bool hovered = false;
		if (m_gui.Clickable(GuiId("game", index) ^ static_cast<uint32_t>(std::hash<std::string>{}(game.key)), row, &hovered))
			clicked = &game;
		if (m_gui.Drawing())
		{
			Canvas& c = m_gui.Draw();
			if (hovered)
				c.FillRect(row, Theme::item_hover, 14.0f);
			else if (running)
				c.FillRect(row, Theme::item_selected.WithAlpha(0.6f), 14.0f);
		}
		const Rect star{row.x + 14.0f, row.y + 0.5f * (ROW_HEIGHT - 56.0f), 56.0f, 56.0f};
		if (m_gui.IconButton(GuiId("star", index) ^ static_cast<uint32_t>(std::hash<std::string>{}(game.key)), star,
				Icon::star, 26.0f, favorite ? star_on : Theme::control_hover, favorite ? star_on : Theme::text_muted))
		{
			if (favorite)
				m_favorites.erase(game.key);
			else
				m_favorites.insert(game.key);
			SaveFavorites();
			clicked = nullptr;
			m_needs_draw = true;
		}
		// The running game: Restart, left of its Resume badge, also on the row.
		constexpr float badge_w = 150.0f, restart_w = 150.0f;
		const Rect restart{row.Right() - badge_w - 18.0f - 12.0f - restart_w, row.y + 0.5f * (ROW_HEIGHT - 44.0f), restart_w, 44.0f};
		if (running && m_gui.Button(GuiId("restart") ^ static_cast<uint32_t>(std::hash<std::string>{}(game.key)), restart,
						   "Restart", Icon::restart))
		{
			m_confirm = &game;
			m_confirm_restart = true;
			clicked = nullptr;
			m_needs_draw = true;
		}

		if (m_gui.Drawing())
		{
			// Title and details; at the right, what a click does.
			const float text_x = star.Right() + 14.0f;
			const float text_w = (running ? restart.x - 12.0f : row.Right() - badge_w - 24.0f) - text_x;
			m_gui.Text(text_x, row.y + 14.0f, m_gui.Elide(game.info.title, Theme::label_size, text_w), Theme::label_size);
			std::string details;
			// On Recently Added, when it arrived even for the running game: its badge says it runs.
			if (m_tab == Tab::RecentlyAdded)
				details = FormatWhen("Added", game.info.added);
			else if (running)
				details = "Playing now";
			else if (m_tab == Tab::RecentPlay)
				details = FormatWhen("Last played", game.info.last_played);
			else if (m_tab == Tab::MostPlayed)
				details = FormatPlayed(game.info.played_seconds);
			else
				details = game.info.serial.empty() ? FormatSize(game.info.size) :
				                                     fmt::format("{} · {}", game.info.serial, FormatSize(game.info.size));
			m_gui.Text(text_x, row.y + 48.0f, m_gui.Elide(details, Theme::small_size, text_w), Theme::small_size, Theme::text_muted);

			const Rect badge{row.Right() - badge_w - 18.0f, row.y + 0.5f * (ROW_HEIGHT - 44.0f), badge_w, 44.0f};
			if (running)
			{
				m_gui.Draw().FillRect(badge, Theme::accent.WithAlpha(0.25f), 22.0f);
				m_gui.TextCentered(badge, hovered ? "Resume" : "Playing", Theme::small_size, Theme::accent_hover);
			}
			else if (hovered)
			{
				m_gui.Draw().FillRect(badge, Theme::accent, 22.0f);
				m_gui.Draw().DrawIcon(m_gui.IconFont(), 16.0f, {badge.x + 22.0f, badge.y, 24.0f, badge.h}, Icon::play, Theme::text);
				m_gui.Text(badge.x + 56.0f, badge.y + 0.5f * (badge.h - m_gui.TextFont().LineHeight(Theme::small_size)),
					"Play", Theme::small_size);
			}
		}
	}
	m_gui.SetClip(std::nullopt);

	if (clicked)
		Launch(*clicked);
}

void GameListMenu::Launch(const Game& game)
{
	if (IsRunning(game))
		m_close_requested = true; // back to it
	else if (!m_running.empty())
	{
		m_confirm = &game;
		m_confirm_restart = false;
	}
	else
	{
		Emu::BootGame(game.info.path);
		m_close_requested = true;
	}
	m_needs_draw = true;
}

void GameListMenu::RunDialog()
{
	const Game& game = *m_confirm;
	const Rect box{0.5f * (WIDTH - 640.0f), 0.5f * (HEIGHT - 300.0f), 640.0f, 300.0f};
	if (m_gui.Drawing())
	{
		Canvas& c = m_gui.Draw();
		c.FillRect({0, 0, WIDTH, HEIGHT}, Rgb(0x000000, 140), RADIUS);
		c.FillRect(box, Theme::nav, 24.0f);
		c.StrokeRect(box, Theme::border, 24.0f, 2.0f);
		const std::string title = (m_confirm_restart ? "Restart " : "Start ") + game.info.title + "?";
		m_gui.Text(box.x + 40.0f, box.y + 36.0f, m_gui.Elide(title, Theme::title_size - 4.0f, box.w - 80.0f),
			Theme::title_size - 4.0f);
		std::string running_title = "The game";
		for (const Game& other : m_games)
		{
			if (IsRunning(other))
				running_title = other.info.title;
		}
		m_gui.TextWrapped(box.x + 40.0f, box.y + 96.0f, box.w - 80.0f,
			m_confirm_restart ?
				std::string("The game starts over, as a console reset does. Its current state and your play since "
							"your last save are lost.") :
				fmt::format("{} is running. It stops, and progress since your last save is lost.", running_title),
			Theme::body_size, Theme::text_muted);
	}
	const float bw = 200.0f, by = box.Bottom() - 40.0f - 56.0f;
	if (m_gui.Button(GuiId("dialog_cancel"), {box.Right() - 40.0f - 2.0f * bw - 16.0f, by, bw, 56.0f}, "Cancel"))
	{
		m_confirm = nullptr;
		m_needs_draw = true;
		return;
	}
	if (m_confirm_restart ?
			m_gui.Button(GuiId("dialog_ok"), {box.Right() - 40.0f - bw, by, bw, 56.0f}, "Restart", Icon::restart, Gui::Style::Danger) :
			m_gui.Button(GuiId("dialog_ok"), {box.Right() - 40.0f - bw, by, bw, 56.0f}, "Start", Icon::play, Gui::Style::Primary))
	{
		if (m_confirm_restart)
			Emu::ResetGame();
		else
			Emu::BootGame(game.info.path);
		m_confirm = nullptr;
		m_close_requested = true;
		m_needs_draw = true;
		return;
	}
	if (!m_gui.Drawing() && m_gui.Input().pressed && !m_gui.Hovered(box))
	{
		m_confirm = nullptr;
		m_needs_draw = true;
	}
}
