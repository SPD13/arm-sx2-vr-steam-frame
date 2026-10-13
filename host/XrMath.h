// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Vector and quaternion helpers for OpenXR poses.

#pragma once

#include <openxr/openxr.h>

#include <cmath>

inline XrVector3f operator+(const XrVector3f& a, const XrVector3f& b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline XrVector3f operator-(const XrVector3f& a, const XrVector3f& b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline XrVector3f operator*(const XrVector3f& v, float s)
{
	return {v.x * s, v.y * s, v.z * s};
}

inline float Dot(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline XrVector3f Cross(const XrVector3f& a, const XrVector3f& b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline XrVector3f Normalize(const XrVector3f& v)
{
	const float len = std::sqrt(Dot(v, v));
	return len > 1e-6f ? v * (1.0f / len) : XrVector3f{0.0f, 0.0f, 0.0f};
}

inline XrQuaternionf Conjugate(const XrQuaternionf& q)
{
	return {-q.x, -q.y, -q.z, q.w};
}

// v rotated by q.
inline XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
{
	const XrVector3f u{q.x, q.y, q.z};
	const XrVector3f t = Cross(u, v) * 2.0f;
	return v + t * q.w + Cross(u, t);
}

// A point given in pose's frame, in the pose's parent space.
inline XrVector3f Transform(const XrPosef& pose, const XrVector3f& local)
{
	return pose.position + Rotate(pose.orientation, local);
}

// The rotation whose axes are x, y and z (orthonormal, right-handed).
inline XrQuaternionf QuatFromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z)
{
	XrQuaternionf q;
	const float trace = x.x + y.y + z.z;
	if (trace > 0.0f)
	{
		const float s = 0.5f / std::sqrt(trace + 1.0f);
		q = {(y.z - z.y) * s, (z.x - x.z) * s, (x.y - y.x) * s, 0.25f / s};
	}
	else if (x.x > y.y && x.x > z.z)
	{
		const float s = 2.0f * std::sqrt(1.0f + x.x - y.y - z.z);
		q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
	}
	else if (y.y > z.z)
	{
		const float s = 2.0f * std::sqrt(1.0f + y.y - x.x - z.z);
		q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
	}
	else
	{
		const float s = 2.0f * std::sqrt(1.0f + z.z - x.x - y.y);
		q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
	}
	return q;
}
