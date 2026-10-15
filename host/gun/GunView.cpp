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
	constexpr float SHARPER = 1.25f; // fitted again when a new quad's picture would be this much sharper
	// Room left around an eye's view: what is just out of it still counts, so the quad is fitted
	// before a head turn brings it in.
	constexpr float VIEW_MARGIN = 10.0f * DEG;
	constexpr float MAX_VIEW_ANGLE = 80.0f * DEG; // from the eye's axis, with the margin
	constexpr float VIEW_NEAR = 0.01f; // meters; what is nearer the eye's plane is cut there

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

	// A quad's view of points from eye: toward them, upright, taking them in with a margin (angles
	// from its axis).
	struct Frame
	{
		XrQuaternionf orientation;
		float left, right, bottom, top;

		// Its picture's pixels per radian at its centre.
		float Sharpness(float pixels_per_radian, uint32_t max_pixels) const
		{
			const float most = std::max(std::tan(right) - std::tan(left), std::tan(top) - std::tan(bottom)) * pixels_per_radian;
			return most > static_cast<float>(max_pixels) ? pixels_per_radian * static_cast<float>(max_pixels) / most :
			                                                 pixels_per_radian;
		}
	};

	Frame Around(const XrVector3f& eye, std::span<const XrVector3f> points)
	{
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
		Frame f;
		f.orientation = QuatFromAxes(x, y, z);

		const XrQuaternionf to_view = Conjugate(f.orientation);
		float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
		for (const XrVector3f& p : points)
		{
			float tx, ty;
			Tangent(Rotate(to_view, p - eye), &tx, &ty);
			x0 = std::min(x0, std::atan(tx)), x1 = std::max(x1, std::atan(tx));
			y0 = std::min(y0, std::atan(ty)), y1 = std::max(y1, std::atan(ty));
		}
		const float limit = 0.5f * MAX_SPAN;
		f.left = std::max(x0 - MARGIN, -limit), f.right = std::min(x1 + MARGIN, limit);
		f.bottom = std::max(y0 - MARGIN, -limit), f.top = std::min(y1 + MARGIN, limit);
		return f;
	}
} // namespace

void GunView::Visible(const XrPosef& eye, const XrFovf& fov, std::span<const XrVector3f> points,
	std::span<const uint32_t> shape_ends, std::vector<XrVector3f>* out)
{
	// The view with its margin, in tangent space.
	const float min_x = std::tan(std::max(fov.angleLeft - VIEW_MARGIN, -MAX_VIEW_ANGLE));
	const float max_x = std::tan(std::min(fov.angleRight + VIEW_MARGIN, MAX_VIEW_ANGLE));
	const float min_y = std::tan(std::max(fov.angleDown - VIEW_MARGIN, -MAX_VIEW_ANGLE));
	const float max_y = std::tan(std::min(fov.angleUp + VIEW_MARGIN, MAX_VIEW_ANGLE));
	const XrQuaternionf to_view = Conjugate(eye.orientation);

	uint32_t begin = 0;
	for (const uint32_t end : shape_ends)
	{
		// The shape's part in front of the eye: its points there, and where the lines between them
		// cross the near plane (the cut hull's corners are among them). Its bounds in tangent space.
		float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
		const auto take = [&](const XrVector3f& d) {
			const float tx = d.x / -d.z, ty = d.y / -d.z;
			x0 = std::min(x0, tx), x1 = std::max(x1, tx), y0 = std::min(y0, ty), y1 = std::max(y1, ty);
		};
		for (uint32_t a = begin; a < end; a++)
		{
			const XrVector3f da = Rotate(to_view, points[a] - eye.position);
			if (da.z <= -VIEW_NEAR)
				take(da);
			for (uint32_t b = a + 1; b < end; b++)
			{
				const XrVector3f db = Rotate(to_view, points[b] - eye.position);
				if ((da.z <= -VIEW_NEAR) != (db.z <= -VIEW_NEAR))
				{
					const float t = (-VIEW_NEAR - da.z) / (db.z - da.z);
					take(da + (db - da) * t);
				}
			}
		}
		begin = end;

		// What of it is in view: its bounds cut to the view.
		x0 = std::max(x0, min_x), x1 = std::min(x1, max_x), y0 = std::max(y0, min_y), y1 = std::min(y1, max_y);
		if (x0 > x1 || y0 > y1)
			continue;
		for (const float x : {x0, x1})
		{
			for (const float y : {y0, y1})
				out->push_back(eye.position + Rotate(eye.orientation, Normalize({x, y, -1.0f})));
		}
	}
}

bool GunView::Fit(const XrVector3f& eye, std::span<const XrVector3f> points, float distance, float pixels_per_degree,
	uint32_t max_pixels)
{
	if (points.empty())
		return false;

	const Frame f = Around(eye, points);
	m_orientation = f.orientation;
	const float limit = 0.5f * MAX_SPAN;
	m_at_limit[0] = f.left <= -limit, m_at_limit[1] = f.right >= limit;
	m_at_limit[2] = f.bottom <= -limit, m_at_limit[3] = f.top >= limit;

	const float tl = std::tan(f.left), tr = std::tan(f.right), tb = std::tan(f.bottom), tt = std::tan(f.top);
	m_center = Transform({m_orientation, eye}, {0.5f * (tl + tr) * distance, 0.5f * (tb + tt) * distance, -distance});
	m_half_width = 0.5f * (tr - tl) * distance;
	m_half_height = 0.5f * (tt - tb) * distance;

	// pixels_per_degree at the centre (the picture is flat: sharper toward its edges).
	m_pixels_per_radian = pixels_per_degree / DEG;
	m_max_pixels = max_pixels;
	m_sharpness = f.Sharpness(m_pixels_per_radian, max_pixels);
	m_width = std::max(16u, static_cast<uint32_t>((tr - tl) * m_sharpness));
	m_height = std::max(16u, static_cast<uint32_t>((tt - tb) * m_sharpness));
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
	const bool too_big = (x1 - x0) + 2.0f * MARGIN < SHRINK * (right - left) ||
	                     (y1 - y0) + 2.0f * MARGIN < SHRINK * (top - bottom);
	return !too_big && Around(eye, points).Sharpness(m_pixels_per_radian, m_max_pixels) < SHARPER * m_sharpness;
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
