// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunView.h"

#include "XrMath.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	constexpr float DEG = 3.14159265f / 180.0f;
	constexpr float MAX_OFF_AXIS = 75.0f * DEG; // points farther from the axis are pulled in (the quad has to end)
	constexpr float MARGIN = 10.0f * DEG; // room left around what is shown, when fitting
	constexpr float EDGE = 2.0f * DEG; // fitted again when something comes this near an edge ...
	constexpr float SHRINK = 0.45f; // ... or when what is shown spans less than this share of the view
	constexpr float MAX_SPAN = 150.0f * DEG; // on each axis

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

bool GunView::Fit(const XrVector3f& eye, std::span<const XrVector3f> points)
{
	if (points.empty())
		return false;
	m_eye = eye;

	// Do the points still fit the last view well?
	if (m_valid)
	{
		const XrQuaternionf to_view = Conjugate(m_orientation);
		float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
		bool fits = true;
		for (const XrVector3f& p : points)
		{
			const XrVector3f local = Rotate(to_view, p - eye);
			float x, y;
			Tangent(local, &x, &y);
			const float ax = std::atan(x), ay = std::atan(y);
			fits = fits && local.z < 0.0f && ax > std::atan(m_left) + EDGE && ax < std::atan(m_right) - EDGE &&
			       ay > std::atan(m_bottom) + EDGE && ay < std::atan(m_top) - EDGE;
			x0 = std::min(x0, ax), x1 = std::max(x1, ax), y0 = std::min(y0, ay), y1 = std::max(y1, ay);
		}
		const float span_x = std::atan(m_right) - std::atan(m_left), span_y = std::atan(m_top) - std::atan(m_bottom);
		const bool too_big = (x1 - x0) + 2.0f * MARGIN < SHRINK * span_x && (y1 - y0) + 2.0f * MARGIN < SHRINK * span_y;
		if (fits && !too_big)
			return true;
	}

	// Fit again: look toward the points, upright, and take them in with a margin.
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
	m_left = std::tan(std::max(x0 - MARGIN, -limit));
	m_right = std::tan(std::min(x1 + MARGIN, limit));
	m_bottom = std::tan(std::max(y0 - MARGIN, -limit));
	m_top = std::tan(std::min(y1 + MARGIN, limit));
	m_valid = true;
	m_refits++;
	return true;
}

XrFovf GunView::Fov() const
{
	return {std::atan(m_left), std::atan(m_right), std::atan(m_top), std::atan(m_bottom)};
}

XrPosef GunView::QuadPose(float distance) const
{
	return {m_orientation,
		Transform(Camera(), {0.5f * (m_left + m_right) * distance, 0.5f * (m_bottom + m_top) * distance, -distance})};
}

XrExtent2Df GunView::QuadSize(float distance) const
{
	return {(m_right - m_left) * distance, (m_top - m_bottom) * distance};
}

void GunView::PixelSize(float pixels_per_degree, uint32_t max_pixels, uint32_t* width, uint32_t* height) const
{
	const float w = (std::atan(m_right) - std::atan(m_left)) / DEG * pixels_per_degree;
	const float h = (std::atan(m_top) - std::atan(m_bottom)) / DEG * pixels_per_degree;
	const float scale = std::min(1.0f, static_cast<float>(max_pixels) / std::max(w, h));
	*width = std::max(16u, static_cast<uint32_t>(w * scale));
	*height = std::max(16u, static_cast<uint32_t>(h * scale));
}
