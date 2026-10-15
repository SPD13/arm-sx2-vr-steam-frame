// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Where an eye's light gun picture goes: a quad standing still in the room between the eye and the
// guns, fitted to face the eye and to cover the guns and their beams as that eye sees them. The
// picture is drawn from wherever the eye is, through the quad (an off-centre frustum), so it lands
// where it was drawn.
//
// The quad must not move while it shows the guns: SteamVR shows quad layers as overlays and does not
// apply a quad's new pose on the frame its new picture comes. A quad that turned with the head made
// the guns stutter as the head turned; one fitted again as a shaken gun neared its edges made them
// jump. GunRenderer therefore gives each eye two quads, and a new fit goes to the one not in use.

#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <span>
#include <vector>

class GunView
{
public:
	// What of the shapes an eye can see, with a margin around its view: appends to out points (LOCAL
	// space) whose directions from the eye cover it. Shape k is the convex hull of points
	// [shape_ends[k - 1], shape_ends[k]) (LOCAL space): a model's bounds, a beam. What is out of view
	// is left out, so it neither widens the quad (blurring the picture) nor gets the quad to its
	// widest, where what goes past is cut.
	static void Visible(const XrPosef& eye, const XrFovf& fov, std::span<const XrVector3f> points,
		std::span<const uint32_t> shape_ends, std::vector<XrVector3f>* out);

	// A new quad, distance meters in front of eye, facing it and covering points (LOCAL space) with a
	// margin; its picture gets pixels_per_degree at its centre, at most max_pixels on a side. False
	// when there is nothing to show.
	bool Fit(const XrVector3f& eye, std::span<const XrVector3f> points, float distance, float pixels_per_degree,
		uint32_t max_pixels);
	// Whether the points, seen from eye, are still well inside the quad, it is not much larger than
	// they need, and a quad fitted to them now would not be much sharper (after the guns were far
	// out, its picture spread over a wide quad).
	bool Fits(const XrVector3f& eye, std::span<const XrVector3f> points) const;
	bool IsValid() const { return m_valid; }
	void Reset() { m_valid = false; }

	// The camera to draw with: at the eye, looking along the quad's -Z.
	XrPosef Camera(const XrVector3f& eye) const { return {m_orientation, eye}; }
	// Its frustum, through the quad.
	XrFovf Fov(const XrVector3f& eye) const;
	XrPosef QuadPose() const { return {m_orientation, m_center}; }
	XrExtent2Df QuadSize() const { return {2.0f * m_half_width, 2.0f * m_half_height}; }
	// The picture's size in pixels, fixed with the quad.
	uint32_t Width() const { return m_width; }
	uint32_t Height() const { return m_height; }

private:
	// The eye in the quad's frame (the quad at z = 0, the eye in front at z > 0).
	XrVector3f EyeInQuad(const XrVector3f& eye) const;

	bool m_valid = false;
	XrQuaternionf m_orientation{0.0f, 0.0f, 0.0f, 1.0f};
	XrVector3f m_center{};
	float m_half_width = 0.0f, m_half_height = 0.0f;
	// Sides that reached the widest view when fitted: nothing beyond them can be taken in.
	bool m_at_limit[4] = {}; // left, right, bottom, top
	uint32_t m_width = 16, m_height = 16;
	// Its picture's pixels per radian at its centre, and what Fit was given for it.
	float m_sharpness = 0.0f;
	float m_pixels_per_radian = 0.0f;
	uint32_t m_max_pixels = 0;
};
