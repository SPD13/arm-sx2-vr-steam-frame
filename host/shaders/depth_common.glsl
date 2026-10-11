// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// Shared by the depth passes: the GS depth canvas (R32F, GS Z / 2^32, larger is nearer) sampled
// over the presented region, on a grid of out_size cells, and the HUD test that compares the
// presented colour with the colour from when the depth was taken.

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D colour_tex; // the presented canvas
layout(set = 0, binding = 4) uniform sampler2D prehud_tex; // in the depth canvas' space

layout(set = 0, binding = 1, std430) buffer Stats
{
	uint zmin; // float bits of the smallest valid depth (farthest)
	uint zmax; // float bits of the largest valid depth (nearest)
	uint valid; // cells with depth
	uint total; // cells sampled
	uint hud; // cells drawn after the 3D
} stats;

layout(push_constant) uniform Params
{
	vec4 src_uv; // presented region of the depth canvas (x0, y0, x1, y1)
	vec4 draw_rect; // where it lands in the presented canvas, pixels
	ivec2 canvas_size; // depth canvas
	ivec2 out_size;
	ivec2 colour_size; // presented canvas
	float depth_scale; // GS depth * this = 0..1 of the Z format's range
	float hud_threshold; // per-channel colour change that counts as HUD; 0 disables the test
} params;

// Depth of a grid cell in 0..1 of the Z range; 0 where nothing with depth was drawn.
float SampleDepth(ivec2 cell)
{
	const vec2 uv = mix(params.src_uv.xy, params.src_uv.zw, (vec2(cell) + 0.5) / vec2(params.out_size));
	const ivec2 texel = clamp(ivec2(uv * vec2(params.canvas_size)), ivec2(0), params.canvas_size - 1);
	return texelFetch(depth_tex, texel, 0).r * params.depth_scale;
}

// Whether the presented colour at a cell differs from the colour when the depth was taken.
bool IsHud(ivec2 cell)
{
	if (params.hud_threshold <= 0.0)
		return false;
	const vec2 f = (vec2(cell) + 0.5) / vec2(params.out_size);
	const vec2 uv = mix(params.src_uv.xy, params.src_uv.zw, f);
	const vec4 before = textureLod(prehud_tex, uv, 0.0);
	const vec2 px = mix(params.draw_rect.xy, params.draw_rect.zw, f);
	const vec4 after = textureLod(colour_tex, px / vec2(params.colour_size), 0.0);
	return any(greaterThan(abs(after.rgb - before.rgb), vec3(params.hud_threshold)));
}
