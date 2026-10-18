// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunAim.h"

#include "XrMath.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr int MARCH_STEPS = 96;
	constexpr int REFINE_STEPS = 8;
	constexpr float MIN_EYE_DISTANCE = 0.1f; // as main.cpp gives the GS
	constexpr float MAX_PLANE_DISTANCE = 6.0f; // beside the screen, the beam ends on its plane up to this far
	constexpr float PORTAL_FAR_U = 0.02f; // Volumetric+: the march ends 49 screen distances behind the glass
} // namespace

float GunDepthGrid::Sample(float u, float v) const
{
	if (!width || !height)
		return -1.0f;
	const uint32_t x = std::min(static_cast<uint32_t>(std::max(u, 0.0f) * static_cast<float>(width)), width - 1);
	const uint32_t y = std::min(static_cast<uint32_t>(std::max(v, 0.0f) * static_cast<float>(height)), height - 1);
	return znorm[size_t(y) * width + x];
}

GunAim AimGun(const XrPosef& muzzle, const GunScreen& screen, const GunStereo* stereo, const GunDepthGrid* depth)
{
	GunAim aim;
	// Everything in the screen's frame: x right, y up, z toward the viewer, origin at its centre.
	const XrQuaternionf to_screen = Conjugate(screen.pose.orientation);
	const XrVector3f o = Rotate(to_screen, muzzle.position - screen.pose.position);
	const XrVector3f d = Rotate(to_screen, Rotate(muzzle.orientation, {0.0f, 0.0f, -1.0f}));
	if (o.z <= 0.0f || d.z >= -1e-4f)
		return aim; // behind the screen, or pointing away from it
	const float t_glass = -o.z / d.z;
	const XrVector3f glass = o + d * t_glass;
	const float half_w = 0.5f * screen.width, half_h = 0.5f * screen.height;
	aim.on_plane = t_glass < MAX_PLANE_DISTANCE;
	aim.end = Transform(screen.pose, glass);
	if (std::abs(glass.x) > half_w || std::abs(glass.y) > half_h)
		return aim;
	aim.on_screen = true;

	// The unshifted screen point S (meters) the GunCon reports, and where the beam ends.
	float sx = glass.x, sy = glass.y;
	XrVector3f end = glass;

	if (stereo && stereo->z_mode && depth && depth->width && depth->height && stereo->convergence > 0.0f)
	{
		const XrVector3f eye = Rotate(to_screen, stereo->left_eye - screen.pose.position);
		const float distance = std::max(eye.z, MIN_EYE_DISTANCE);
		const float k = stereo->depth / (distance + stereo->depth);
		const float d0 = std::max(stereo->portal_distance, MIN_EYE_DISTANCE);
		const float ratio = distance / d0; // portal
		// A point P behind the glass, seen from the eye: its image I on the glass, g and the scale
		// there (I = S scale + eye g), and how far behind the glass the surface the eye sees there is.
		struct Probe
		{
			bool inside = false;
			float ix = 0.0f, iy = 0.0f, g = 0.0f, scale = 1.0f, surface = 0.0f, z = -1.0f;
		};
		const auto probe = [&](const XrVector3f& p) {
			Probe r;
			const float scale = distance / (distance - p.z); // p.z <= 0
			r.ix = eye.x + (p.x - eye.x) * scale;
			r.iy = eye.y + (p.y - eye.y) * scale;
			const float u = r.ix / screen.width + 0.5f, v = 0.5f - r.iy / screen.height;
			if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
				return r;
			r.inside = true;
			const float z = depth->Sample(u, v);
			r.z = z;
			// Outside the picture (no depth): as far as it goes.
			const float f = z < 0.0f ? 1.0f : std::clamp(1.0f - z / stereo->convergence, 0.0f, 1.0f);
			if (stereo->portal)
			{
				const float den = 1.0f + (1.0f - f) * (ratio - 1.0f);
				r.g = f / den;
				r.scale = ratio / den;
				r.surface = f < 1.0f ? d0 * f / (1.0f - f) : INFINITY;
				return r;
			}
			r.g = k * f;
			r.surface = r.g * distance / (1.0f - r.g);
			return r;
		};
		// From the glass to just past the farthest surface (R behind it, where f = 1). The portal's
		// goes to infinity: its steps are even in u, out to PORTAL_FAR_U.
		const float t_end = t_glass + 1.02f * stereo->depth / -d.z;
		const auto step_t = [&](int i) {
			const float s = static_cast<float>(i) / MARCH_STEPS;
			if (!stereo->portal)
				return t_glass + (t_end - t_glass) * s;
			const float u = 1.0f - s * (1.0f - PORTAL_FAR_U);
			return t_glass + d0 * (1.0f / u - 1.0f) / -d.z;
		};
		float t_prev = t_glass;
		bool hit = false;
		for (int i = 0; i <= MARCH_STEPS && !hit; i++)
		{
			const float t = step_t(i);
			const XrVector3f p = o + d * t;
			const Probe r = probe(p);
			if (!r.inside)
				break; // past the window's edge: the beam ends on the glass
			if (-p.z >= r.surface)
			{
				// Between the last step and this one: narrow it down.
				float lo = t_prev, hi = t;
				for (int j = 0; j < REFINE_STEPS && i > 0; j++)
				{
					const float mid = 0.5f * (lo + hi);
					const XrVector3f q = o + d * mid;
					const Probe m = probe(q);
					(m.inside && -q.z >= m.surface ? hi : lo) = mid;
				}
				const XrVector3f q = o + d * hi;
				const Probe m = probe(q);
				if (m.inside)
				{
					end = q;
					sx = (m.ix - eye.x * m.g) / m.scale;
					sy = (m.iy - eye.y * m.g) / m.scale;
					aim.through_depth = true;
					aim.znorm = m.z;
				}
				hit = true;
			}
			t_prev = t;
		}
	}

	const float u = sx / screen.width + 0.5f, v = 0.5f - sy / screen.height;
	aim.window_x = screen.view[0] + u * (screen.view[2] - screen.view[0]);
	aim.window_y = screen.view[1] + v * (screen.view[3] - screen.view[1]);
	const float* r = screen.draw_rect;
	aim.on_picture = r[2] > r[0] && r[3] > r[1] && aim.window_x >= r[0] && aim.window_x < r[2] &&
	                 aim.window_y >= r[1] && aim.window_y < r[3];
	aim.end = Transform(screen.pose, end);
	return aim;
}
