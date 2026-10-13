// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light gun's beam and dot: a quad the host places in clip space (a triangle strip of four).

#version 450

layout(push_constant) uniform Quad
{
	vec4 corners[4]; // clip space: start left, start right, end left, end right
	vec4 colour; // linear rgb, intensity
	vec4 params; // x: 0 beam, 1 dot
};

layout(location = 0) out vec2 v_uv; // x along the beam, y across it

void main()
{
	gl_Position = corners[gl_VertexIndex];
	v_uv = vec2(gl_VertexIndex >> 1, gl_VertexIndex & 1);
}
