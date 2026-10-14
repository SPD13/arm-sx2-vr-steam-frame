// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A controller lit as the light guns are (gun.frag), its colour from its texture (with the
// ambient occlusion baked in). Opaque, premultiplied alpha.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "gun_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D albedo;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

void main()
{
	const vec3 view = vec3(0.0, 0.0, 1.0); // toward the eye
	vec3 n = normalize(v_normal);
	if (dot(n, view) < 0.0)
		n = -n;
	const vec3 light = normalize(vec3(-0.35, 0.6, 0.7));
	const float roughness = clamp(colour.a, 0.05, 1.0);
	const float diffuse = max(dot(n, light), 0.0);
	const float shininess = mix(96.0, 6.0, roughness);
	const float highlight = pow(max(dot(n, normalize(light + view)), 0.0), shininess) * (1.0 - roughness) * 0.6;
	const float rim = pow(1.0 - max(dot(n, view), 0.0), 3.0) * 0.18;
	const vec3 base = texture(albedo, v_uv).rgb * colour.rgb;
	out_colour = vec4(base * (0.22 + 0.85 * diffuse) + vec3(highlight + rim), 1.0);
}
