// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The game list the games button opens: tabs for all games, the recently added ones, the recently
// played ones, the most played ones and the favourites, one row per game with a star that marks it as a favourite.
// Clicking a row starts the game, after asking when another one is running; the running game's row
// resumes it, and its Restart button starts it over, after asking. Like SettingsMenu it draws on a
// Canvas; VrMenu shows it and feeds it the pointer and the thumbsticks' scrolling.

#pragma once

#include "Emu.h"
#include "ui/Canvas.h"
#include "ui/Gui.h"

#include <chrono>
#include <set>
#include <string>
#include <utility>
#include <vector>

class GameListMenu
{
public:
	static constexpr int WIDTH = 1200;
	static constexpr int HEIGHT = 800;

	// favorites_path: the file the favourites are kept in, one game path per line.
	GameListMenu(std::string favorites_path, Font& text, Font& icons);

	// For a list that is being opened: looks for new games.
	void Refresh();
	// Scrolls the list by this many pixels (positive: down), from a thumbstick.
	void Scroll(float pixels);

	// One frame of input; redraws the canvas when something visible changed and returns whether it did.
	bool Update(const GuiInput& input);
	const Canvas& GetCanvas() const { return m_canvas; }

	bool TakeCloseRequest() { return std::exchange(m_close_requested, false); }

private:
	enum class Tab
	{
		All,
		RecentlyAdded,
		RecentPlay,
		MostPlayed,
		Favorites,
	};

	struct Game
	{
		Emu::GameInfo info;
		std::string key; // the path, made canonical: for the running game and the favourites
	};

	void Run();
	void RunList();
	void RunDialog();
	// The games the current tab shows, in its order.
	std::vector<const Game*> Shown() const;
	void Launch(const Game& game);
	void LoadFavorites();
	void SaveFavorites() const;
	bool IsRunning(const Game& game) const { return !m_running.empty() && game.key == m_running; }

	std::string m_favorites_path;
	std::set<std::string> m_favorites;
	Canvas m_canvas;
	Gui m_gui;

	std::vector<Game> m_games;
	unsigned m_games_version = ~0u;
	bool m_refreshing = false;
	std::string m_running_raw; // as the emulator reports it
	std::string m_running; // canonical

	Tab m_tab = Tab::All;
	float m_scroll = 0.0f;
	float m_content_height = 0.0f;
	bool m_needs_draw = true;
	bool m_close_requested = false;
	const Game* m_confirm = nullptr; // the game the dialog asks about
	bool m_confirm_restart = false; // ... to restart it (it runs), not to switch to it

	using Clock = std::chrono::steady_clock;
	std::string m_toast;
	Clock::time_point m_toast_until{};
};
