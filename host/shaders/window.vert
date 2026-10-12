// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#version 450
#extension GL_GOOGLE_include_directive : require

#include "window_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D depth_grid;
layout(set = 0, binding = 3) uniform sampler2D background_grid;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_scene_uv;
layout(location = 2) out float v_stretch;
layout(location = 3) out vec2 v_grid; // position on the grid, 0..1

float Grid(ivec2 cell)
{
	return texelFetch(depth_grid, clamp(cell, ivec2(0), window.grid.xy - 1), 0).r;
}

void main()
{
	// One vertex per grid cell, row by row from the top left (indexed draw).
	const ivec2 cell = ivec2(gl_VertexIndex % window.grid.x, gl_VertexIndex / window.grid.x);
	const vec2 f = vec2(cell) / vec2(max(window.grid.xy - 1, ivec2(1)));

	float n = 1.0;
	float stretch = 0.0;
	float behind = 0.0;
	vec2 colour_f = f; // where the colour comes from
	if (window.grid.z != 0 && pass.mode != 2)
	{
		if (pass.mode == 0)
		{
			const vec4 background = texelFetch(background_grid, cell, 0);
			n = background.r;
			colour_f = (vec2(cell) + background.gb) / vec2(max(window.grid.xy - 1, ivec2(1)));
			behind = 0.01; // keep it behind the scene where both agree
		}
		else
		{
			// The largest step to a neighbour: the triangles around this vertex span it.
			n = Grid(cell);
			stretch = max(max(abs(n - Grid(cell + ivec2(1, 0))), abs(n - Grid(cell - ivec2(1, 0)))),
				max(abs(n - Grid(cell + ivec2(0, 1))), abs(n - Grid(cell - ivec2(0, 1)))));
		}
	}

	const vec3 p = window.origin.xyz + window.right.xyz * (f.x * 2.0 - 1.0) + window.up.xyz * (1.0 - f.y * 2.0) +
	               window.back.xyz * ((1.0 - n) - window.back.w + behind);
	gl_Position = window.view_proj[pass.eye] * vec4(p, 1.0);
	v_uv = mix(window.colour_rect.xy, window.colour_rect.zw, colour_f);
	v_scene_uv = mix(window.scene_rect.xy, window.scene_rect.zw, colour_f);
	v_stretch = stretch;
	v_grid = f;
}
