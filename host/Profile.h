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
	// Volumetric: 2D overlays centred on a recent shot go to the scene's depth there (on unless a game
	// shows it misplaces some).
	bool shot_overlays = true;
	// Volumetric: additive 2D overlays that come and go go to the scene's depth under them, wherever
	// they are (GSVolumetric::SetSceneDepth). For the games known to draw their effects that way.
	bool fx_overlays = false;
	// Volumetric: how many times the game's depth values must be scaled up to spread its scene like the
	// other games' (the convergence setting is divided by it). For games with a very small Z range.
	float z_scale = 1.0f;
};

GameProfile LoadGameProfile(const std::string& serial);
