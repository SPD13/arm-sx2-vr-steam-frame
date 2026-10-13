// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Whether light gun mode is on, per game: what the user chose with the button beside the screen
// (kept in lightgun.ini in the settings folder, by disc serial), else on for the games the core
// knows as GunCon 2 games and those whose profile says lightgun = 1. Any thread: the CPU thread asks
// as a game starts, to plug the guns in before it looks for them.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

class INISettingsInterface;

class GunGames
{
public:
	GunGames();
	~GunGames();

	void Load(std::string path);

	bool IsOn(const std::string& serial);
	// The user's choice for this game, saved.
	void Set(const std::string& serial, bool on);
	// Back to the automatic choice.
	void Reset(const std::string& serial);
	// Whether the user chose for this game.
	bool IsChosen(const std::string& serial);
	// The automatic choice.
	bool IsAuto(const std::string& serial);

private:
	void Save();

	std::mutex m_mutex;
	std::unique_ptr<INISettingsInterface> m_ini;
	std::map<std::string, bool> m_auto; // IsAuto's answers (the profile is a file read)
};
