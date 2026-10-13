// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light gun's model, one part per draw (see gun_common.glsl).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "gun_common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 0) out vec3 v_normal; // view space

void main()
{
	gl_Position = mvp * vec4(in_position, 1.0);
	v_normal = mat3(normal_to_view[0].xyz, normal_to_view[1].xyz, normal_to_view[2].xyz) * in_normal;
}
