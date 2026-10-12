// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// Per-game settings for the volumetric window, from profiles/<disc serial>.ini: the user's copy in
// the data folder first, then the one shipped next to the binary.

#pragma once

#include <array>
#include <string>
#include <vector>

struct GameProfile
{
	std::string path; // where it was read from; empty when the game has none
	// Where the HUD is, as fractions of the presented picture (x0, y0, x1, y1).
	std::vector<std::array<float, 4>> hud_rects;
	// Per HUD area: lay all of it flat, not only what the HUD drew at the maximum depth.
	std::vector<bool> hud_rect_flat;
};

GameProfile LoadGameProfile(const std::string& serial);
