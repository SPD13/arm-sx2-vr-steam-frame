// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Per-game settings, from profiles/<disc serial>.ini: the user's copy in the data folder first, then
// the one shipped next to the binary (see profiles/README.txt).

#pragma once

#include <string>

struct GameProfile
{
	std::string path; // where it was read from; empty when the game has none
	// A light gun game the core does not know as one: light gun mode starts on.
	bool lightgun = false;
};

GameProfile LoadGameProfile(const std::string& serial);
