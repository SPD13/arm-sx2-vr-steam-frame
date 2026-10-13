// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Profile.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/Config.h"

#include <fstream>
#include <sstream>

static bool ParseProfile(const std::string& path, GameProfile* profile)
{
	std::ifstream file(path);
	if (!file)
		return false;
	profile->path = path;
	std::string line;
	for (int number = 1; std::getline(file, line); number++)
	{
		line = line.substr(0, line.find('#'));
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		key.erase(0, key.find_first_not_of(" \t"));
		key.erase(key.find_last_not_of(" \t") + 1);
		std::istringstream value(line.substr(eq + 1));
		if (key == "hud_rect")
		{
			std::array<float, 4> r{};
			if (value >> r[0] >> r[1] >> r[2] >> r[3])
			{
				std::string flag;
				value >> flag;
				profile->hud_rects.push_back(r);
				profile->hud_rect_flat.push_back(flag == "flat");
			}
			else
				Console.WarningFmt("Profile {}:{}: hud_rect needs four numbers", path, number);
		}
		else if (key == "lightgun")
			value >> profile->lightgun;
		else
			Console.WarningFmt("Profile {}:{}: unknown key '{}'", path, number, key);
	}
	return true;
}

GameProfile LoadGameProfile(const std::string& serial)
{
	GameProfile profile;
	if (serial.empty())
		return profile;
	for (const std::string& dir : {Path::Combine(EmuFolders::DataRoot, "profiles"), Path::Combine(EmuFolders::AppRoot, "profiles")})
	{
		if (ParseProfile(Path::Combine(dir, serial + ".ini"), &profile))
		{
			Console.WriteLnFmt("Profile for {}: {} ({} HUD areas)", serial, profile.path, profile.hud_rects.size());
			return profile;
		}
	}
	Console.WriteLnFmt("No profile for {}", serial);
	return profile;
}
