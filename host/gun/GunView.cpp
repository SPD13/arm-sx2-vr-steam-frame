// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunView.h"

#include "XrMath.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float DEG = 3.14159265f / 180.0f;
	constexpr float MAX_OFF_AXIS = 75.0f * DEG; // points farther from the axis are pulled in (the quad has to end)
	constexpr float MARGIN = 15.0f * DEG; // room left around what is shown, when fitting
	constexpr float EDGE = 5.0f * DEG; // fitted again when something comes this near an edge ...
	constexpr float SHRINK = 0.45f; // ... or when what is shown spans less than this share of the view
	constexpr float MAX_SPAN = 150.0f * DEG; // on each axis
	constexpr float MIN_EYE_DISTANCE = 0.15f; // meters; an eye nearer the quad (leaning in) fits it again

	// A direction's offset from the view axis, in tangent space; directions too far off are pulled in.
	void Tangent(const XrVector3f& local, float* x, float* y)
	{
		XrVector3f d = Normalize(local);
		const float off = std::acos(std::clamp(-d.z, -1.0f, 1.0f));
		if (off > MAX_OFF_AXIS)
		{
			const float side = std::sqrt(d.x * d.x + d.y * d.y);
			const float s = side > 1e-6f ? std::sin(MAX_OFF_AXIS) / side : 0.0f;
			d = {d.x * s, d.y * s, -std::cos(MAX_OFF_AXIS)};
		}
		*x = d.x / -d.z;
		*y = d.y / -d.z;
	}
} // namespace

bool GunView::Fit(const XrVector3f& eye, std::span<const XrVector3f> points, float distance, float pixels_per_degree,
	uint32_t max_pixels)
{
	if (points.empty())
		return false;

	// Look toward the points, upright, and take them in with a margin.
	XrVector3f axis{};
	for (const XrVector3f& p : points)
		axis = axis + Normalize(p - eye);
	axis = Normalize(axis);
	if (Dot(axis, axis) < 0.5f)
		axis = Normalize(points[0] - eye);
	const XrVector3f z = axis * -1.0f;
	XrVector3f x = Normalize(Cross({0.0f, 1.0f, 0.0f}, z));
	if (Dot(x, x) < 0.5f)
		x = {1.0f, 0.0f, 0.0f};
	const XrVector3f y = Cross(z, x);
	m_orientation = QuatFromAxes(x, y, z);

	const XrQuaternionf to_view = Conjugate(m_orientation);
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	for (const XrVector3f& p : points)
	{
		float tx, ty;
		Tangent(Rotate(to_view, p - eye), &tx, &ty);
		x0 = std::min(x0, std::atan(tx)), x1 = std::max(x1, std::atan(tx));
		y0 = std::min(y0, std::atan(ty)), y1 = std::max(y1, std::atan(ty));
	}
	const float limit = 0.5f * MAX_SPAN;
	const float left = std::max(x0 - MARGIN, -limit), right = std::min(x1 + MARGIN, limit);
	const float bottom = std::max(y0 - MARGIN, -limit), top = std::min(y1 + MARGIN, limit);
	m_at_limit[0] = left <= -limit, m_at_limit[1] = right >= limit;
	m_at_limit[2] = bottom <= -limit, m_at_limit[3] = top >= limit;

	const float tl = std::tan(left), tr = std::tan(right), tb = std::tan(bottom), tt = std::tan(top);
	m_center = Transform({m_orientation, eye}, {0.5f * (tl + tr) * distance, 0.5f * (tb + tt) * distance, -distance});
	m_half_width = 0.5f * (tr - tl) * distance;
	m_half_height = 0.5f * (tt - tb) * distance;

	const float w = (right - left) / DEG * pixels_per_degree;
	const float h = (top - bottom) / DEG * pixels_per_degree;
	const float scale = std::min(1.0f, static_cast<float>(max_pixels) / std::max(w, h));
	m_width = std::max(16u, static_cast<uint32_t>(w * scale));
	m_height = std::max(16u, static_cast<uint32_t>(h * scale));
	m_valid = true;
	return true;
}

bool GunView::Fits(const XrVector3f& eye, std::span<const XrVector3f> points) const
{
	if (!m_valid || points.empty())
		return false;
	const XrVector3f e = EyeInQuad(eye);
	if (e.z < MIN_EYE_DISTANCE)
		return false;
	// The quad's edges as angles from where the eye is now.
	const float left = std::atan((-m_half_width - e.x) / e.z), right = std::atan((m_half_width - e.x) / e.z);
	const float bottom = std::atan((-m_half_height - e.y) / e.z), top = std::atan((m_half_height - e.y) / e.z);

	const XrQuaternionf to_view = Conjugate(m_orientation);
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	for (const XrVector3f& p : points)
	{
		float tx, ty;
		Tangent(Rotate(to_view, p - eye), &tx, &ty);
		const float ax = std::atan(tx), ay = std::atan(ty);
		// A side at the widest view can't do better: what goes past it is cut.
		if ((!m_at_limit[0] && ax < left + EDGE) || (!m_at_limit[1] && ax > right - EDGE) ||
			(!m_at_limit[2] && ay < bottom + EDGE) || (!m_at_limit[3] && ay > top - EDGE))
			return false;
		x0 = std::min(x0, ax), x1 = std::max(x1, ax), y0 = std::min(y0, ay), y1 = std::max(y1, ay);
	}
	const bool too_big = (x1 - x0) + 2.0f * MARGIN < SHRINK * (right - left) &&
	                     (y1 - y0) + 2.0f * MARGIN < SHRINK * (top - bottom);
	return !too_big;
}

XrFovf GunView::Fov(const XrVector3f& eye) const
{
	const XrVector3f e = EyeInQuad(eye);
	const float z = std::max(e.z, 0.01f);
	return {std::atan((-m_half_width - e.x) / z), std::atan((m_half_width - e.x) / z),
		std::atan((m_half_height - e.y) / z), std::atan((-m_half_height - e.y) / z)};
}

XrVector3f GunView::EyeInQuad(const XrVector3f& eye) const
{
	return Rotate(Conjugate(m_orientation), eye - m_center);
}
