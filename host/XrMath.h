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

inline XrQuaternionf operator*(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The rotation by angle radians about a unit axis.
inline XrQuaternionf QuatFromAxisAngle(const XrVector3f& axis, float angle)
{
	const float s = std::sin(0.5f * angle);
	return {axis.x * s, axis.y * s, axis.z * s, std::cos(0.5f * angle)};
}

// pose then local: local given in pose's frame, in the pose's parent space.
inline XrPosef Compose(const XrPosef& pose, const XrPosef& local)
{
	return {pose.orientation * local.orientation, Transform(pose, local.position)};
}

// Column-major 4x4 matrices, as GLSL reads them.
struct Mat4
{
	float m[16] = {};
};

inline Mat4 Multiply(const Mat4& a, const Mat4& b)
{
	Mat4 r;
	for (int col = 0; col < 4; col++)
	{
		for (int row = 0; row < 4; row++)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += a.m[k * 4 + row] * b.m[col * 4 + k];
			r.m[col * 4 + row] = sum;
		}
	}
	return r;
}

// Asymmetric projection for Vulkan clip space (y down, depth 0..1), as xr_linear.h builds it.
inline Mat4 Projection(const XrFovf& fov, float near_z, float far_z)
{
	const float tan_left = std::tan(fov.angleLeft), tan_right = std::tan(fov.angleRight);
	const float tan_down = std::tan(fov.angleDown), tan_up = std::tan(fov.angleUp);
	const float tan_width = tan_right - tan_left;
	const float tan_height = tan_down - tan_up;
	Mat4 p;
	p.m[0] = 2.0f / tan_width;
	p.m[8] = (tan_right + tan_left) / tan_width;
	p.m[5] = 2.0f / tan_height;
	p.m[9] = (tan_up + tan_down) / tan_height;
	p.m[10] = -far_z / (far_z - near_z);
	p.m[14] = -(far_z * near_z) / (far_z - near_z);
	p.m[11] = -1.0f;
	return p;
}

// The rotation part of a pose and its translation: the matrix taking the pose's frame to its parent.
inline Mat4 MatrixFromPose(const XrPosef& pose, const XrVector3f& scale = {1.0f, 1.0f, 1.0f})
{
	const XrQuaternionf& q = pose.orientation;
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	Mat4 r;
	r.m[0] = (1.0f - 2.0f * (yy + zz)) * scale.x;
	r.m[1] = 2.0f * (xy + wz) * scale.x;
	r.m[2] = 2.0f * (xz - wy) * scale.x;
	r.m[4] = 2.0f * (xy - wz) * scale.y;
	r.m[5] = (1.0f - 2.0f * (xx + zz)) * scale.y;
	r.m[6] = 2.0f * (yz + wx) * scale.y;
	r.m[8] = 2.0f * (xz + wy) * scale.z;
	r.m[9] = 2.0f * (yz - wx) * scale.z;
	r.m[10] = (1.0f - 2.0f * (xx + yy)) * scale.z;
	r.m[12] = pose.position.x;
	r.m[13] = pose.position.y;
	r.m[14] = pose.position.z;
	r.m[15] = 1.0f;
	return r;
}

// Inverse of a rigid pose: the view matrix of an eye.
inline Mat4 ViewFromPose(const XrPosef& pose)
{
	const XrQuaternionf& q = pose.orientation;
	// Rotation matrix of q (column-major), then transposed for the inverse.
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	const float r[3][3] = {
		{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)}, // column 0
		{2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)}, // column 1
		{2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)}, // column 2
	};
	const float t[3] = {pose.position.x, pose.position.y, pose.position.z};
	Mat4 v;
	for (int row = 0; row < 3; row++)
	{
		for (int col = 0; col < 3; col++)
			v.m[col * 4 + row] = r[row][col]; // transpose
		v.m[12 + row] = -(r[row][0] * t[0] + r[row][1] * t[1] + r[row][2] * t[2]);
	}
	v.m[15] = 1.0f;
	return v;
}
