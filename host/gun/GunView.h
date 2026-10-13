// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Where an eye's light gun picture goes: a quad between the eye and the guns, facing the eye, that
// covers the guns and their beams as that eye sees them. The picture is drawn from the eye through
// the quad (an off-centre frustum), so it lands where it was drawn.
//
// The quad must not follow the head's rotation: SteamVR shows quad layers as overlays and does not
// apply a quad's new pose on the frame its new picture comes, so a quad that turned with the head
// every frame made the guns stutter as the head turned. Its orientation and extent are kept from
// frame to frame while what it shows still fits well inside, and fitted again only when something
// nears an edge or it has become much larger than needed; its position follows the eye, which head
// rotation moves little.

#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <span>

class GunView
{
public:
	// Fits the view to points (LOCAL space) seen from eye, keeping last frame's orientation and extent
	// when they still do. False when there is nothing to show.
	bool Fit(const XrVector3f& eye, std::span<const XrVector3f> points);
	void Reset() { m_valid = false; }

	// The camera to draw with: at the eye, looking along the quad's -Z.
	XrPosef Camera() const { return {m_orientation, m_eye}; }
	// Its frustum, through the quad.
	XrFovf Fov() const;
	// The quad, distance meters from the eye, facing it.
	XrPosef QuadPose(float distance) const;
	XrExtent2Df QuadSize(float distance) const;
	// Pixels for the picture: pixels_per_degree across the view, at most max_pixels on a side.
	void PixelSize(float pixels_per_degree, uint32_t max_pixels, uint32_t* width, uint32_t* height) const;

	// How often the view was fitted again (for logging).
	uint32_t Refits() const { return m_refits; }

private:
	bool m_valid = false;
	XrQuaternionf m_orientation{0.0f, 0.0f, 0.0f, 1.0f};
	XrVector3f m_eye{};
	// Extent in tangent space (x right, y up, at distance 1): left, right, bottom, top.
	float m_left = -1.0f, m_right = 1.0f, m_bottom = -1.0f, m_top = 1.0f;
	uint32_t m_refits = 0;
};
