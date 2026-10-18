// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Profile.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/Config.h"

#include <fstream>
#include <sstream>

// same_as: the serial whose profile this one is (the same game's other regions), read in its place.
static bool ParseProfile(const std::string& path, GameProfile* profile, std::string* same_as)
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
		if (key == "lightgun")
			value >> profile->lightgun;
		else if (key == "shot_overlays")
			value >> profile->shot_overlays;
		else if (key == "fx_overlays")
			value >> profile->fx_overlays;
		else if (key == "same_as")
			value >> *same_as;
		else
			Console.WarningFmt("Profile {}:{}: unknown key '{}'", path, number, key);
	}
	return true;
}

GameProfile LoadGameProfile(const std::string& serial)
{
	if (serial.empty())
		return {};
	std::string name = serial;
	for (int link = 0; link < 2; link++) // one same_as, no chains
	{
		for (const std::string& dir : {Path::Combine(EmuFolders::DataRoot, "profiles"), Path::Combine(EmuFolders::AppRoot, "profiles")})
		{
			GameProfile profile;
			std::string same_as;
			if (!ParseProfile(Path::Combine(dir, name + ".ini"), &profile, &same_as))
				continue;
			if (!same_as.empty() && link == 0)
			{
				Console.WriteLnFmt("Profile for {}: {}, same as {}", serial, profile.path, same_as);
				name = same_as;
				break;
			}
			Console.WriteLnFmt("Profile for {}: {}", serial, profile.path);
			return profile;
		}
		if (name == serial)
			break;
	}
	Console.WriteLnFmt("No profile for {}", serial);
	return {};
}
