// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Where a light gun's beam lands, and where the GunCon must report it.
//
// On a flat screen that is where the beam crosses the screen. In geometric stereo (Z mode) the GS
// draws each vertex shifted by eye_offset * k * f, k = R / (D + R) for an eye D in front of the
// screen and the farthest scenery R behind it, f = clamp(1 - Znorm / convergence, 0, 1). Both eyes'
// images then agree on one point for every unshifted screen point S: with g = k f, it lies g D /
// (1 - g) behind the screen, on the line from the screen centre's viewer through S. So the beam is
// marched into the portal: each step is seen from the left eye, whose image the depth grid comes
// from, until it reaches the surface there; the GunCon gets that surface point's unshifted S.
//
// Volumetric+ (the GS's portal) places S's point D0 (1/u - 1) behind the screen, u = Znorm /
// convergence, for the screen distance D0, and draws it at S r / den + eye (1 - u) / den from an eye
// D = r D0 away, den = 1 + u (r - 1); the march is the same with that surface and that image.

#pragma once

#include <openxr/openxr.h>

#include <chrono>
#include <cstdint>
#include <vector>

// Normalised depth (Znorm = Z / the format's maximum, larger is nearer) over the presented frame,
// sampled on a coarse grid, as the left eye saw it; negative outside the picture.
struct GunDepthGrid
{
	std::vector<float> znorm;
	uint32_t width = 0, height = 0;
	std::chrono::steady_clock::time_point time{}; // when its frame came

	// Nearest cell at uv (0..1 of the presented frame, top left origin).
	float Sample(float u, float v) const;
};

struct GunScreen
{
	XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}}; // the screen quad's centre, facing +Z
	float width = 2.4f, height = 1.8f; // meters
	// The part of the presented frame the quad shows (x0, y0, x1, y1 pixels): all of it on the flat
	// and stereo screens, the picture only in the volumetric window.
	float view[4] = {0.0f, 0.0f, 1280.0f, 960.0f};
	float draw_rect[4] = {0.0f, 0.0f, 1280.0f, 960.0f}; // the game's picture in the frame
};

// Geometric stereo, as the GS draws it (see GSVolumetric::StereoSettings).
struct GunStereo
{
	bool z_mode = false; // only Z mode can be marched: the depth grid holds Z, not Q
	float convergence = 1.0f;
	float depth = 1.0f; // R, meters
	bool portal = false; // Volumetric+: depth from portal_distance instead
	float portal_distance = 2.2f; // D0, meters
	XrVector3f left_eye{}; // LOCAL space
};

struct GunAim
{
	bool on_screen = false; // the beam crosses the screen quad
	bool on_plane = false; // ... or the screen's plane beside it (end is there)
	bool on_picture = false; // ... within the game's picture: the gun aims at the game
	float window_x = 0.0f, window_y = 0.0f; // where, in pixels of the presented frame (for Emu::GunState)
	XrVector3f end{}; // where the beam ends: the surface it reaches, or the glass
	bool through_depth = false; // the beam went into the portal
	float znorm = -1.0f; // ... and the depth (Znorm) of the surface it reached there
};

// muzzle: the gun's ray (along its -Z). stereo and depth may be null: the beam then ends on the glass.
GunAim AimGun(const XrPosef& muzzle, const GunScreen& screen, const GunStereo* stereo, const GunDepthGrid* depth);
