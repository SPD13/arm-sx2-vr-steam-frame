// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
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

// Valid cells per 1/256 of the [zmin, zmax] range.
layout(set = 0, binding = 5, std430) buffer Histogram
{
	uint bins[256];
} histogram;

// The depth range mapped onto 0..1, smoothed over frames. Persists between frames.
layout(set = 0, binding = 6, std430) buffer Range
{
	float lo; // 2nd percentile (far end)
	float hi; // 98th percentile (near end)
	uint initialized;
	uint pad;
} range;

// Normalised depth, one texel per grid cell: 1 = at the screen plane (nearest), 0 = farthest, also
// where nothing with depth was drawn. Under the HUD it is the scene's depth: the window draws the
// scene from the pre-HUD colour and lays the HUD flat on top.
layout(set = 0, binding = 7, r32f) uniform writeonly image2D normalized_image;
// The same grid read back, and its erosion toward far: the layer the window draws behind the
// scene so depth edges reveal background colours instead of stretched triangles. r = depth,
// gb = offset in cells to the far cell it came from, whose colour that layer shows, a = the HUD
// history of the cell (how steadily its live depth has been at the maximum).
layout(set = 0, binding = 8) uniform sampler2D normalized_tex;
layout(set = 0, binding = 9, rgba32f) uniform writeonly image2D background_image;
// The depth target at vsync (VKLibretro::Frame::live_depth_*), for the HUD test; params.live_depth
// says whether it is bound.
layout(set = 0, binding = 10) uniform sampler2D live_depth_tex;
// Per cell, how steadily the live depth has been at the maximum (depth_hud.comp); persists.
layout(set = 0, binding = 11, r32f) uniform image2D hud_history;

layout(push_constant) uniform Params
{
	vec4 src_uv; // presented region of the depth canvas (x0, y0, x1, y1)
	vec4 draw_rect; // where it lands in the presented canvas, pixels
	ivec2 canvas_size; // depth canvas
	ivec2 out_size;
	ivec2 colour_size; // presented canvas
	float depth_scale; // GS depth * this = 0..1 of the Z format's range
	float hud_threshold; // per-channel colour change that counts as HUD; 0 disables the test
	int live_depth; // 1 when live_depth_tex is bound: HUD = live depth at the Z format's maximum
} params;

// Depth at (or within float precision of) the Z format's maximum: what a HUD drawn with Z writes
// leaves. It belongs on the screen plane and stays out of the range statistics.
const float MAX_DEPTH = 0.9999;

bool IsSceneDepth(float z)
{
	return z > 0.0 && z < MAX_DEPTH;
}

// Depth of a grid cell in 0..1 of the Z range; 0 where nothing with depth was drawn.
float SampleDepth(ivec2 cell)
{
	const vec2 uv = mix(params.src_uv.xy, params.src_uv.zw, (vec2(cell) + 0.5) / vec2(params.out_size));
	const ivec2 texel = clamp(ivec2(uv * vec2(params.canvas_size)), ivec2(0), params.canvas_size - 1);
	return texelFetch(depth_tex, texel, 0).r * params.depth_scale;
}

bool IsHud(ivec2 cell);

bool IsChanged(ivec2 cell);

// Normalised depth straight from the depth canvas.
float RawNormalizedDepth(ivec2 cell)
{
	const float z = SampleDepth(clamp(cell, ivec2(0), params.out_size - 1));
	if (z >= MAX_DEPTH)
		return 1.0;
	if (z <= 0.0 || range.initialized == 0u)
		return 0.0;
	return clamp((z - range.lo) / max(range.hi - range.lo, 1e-7), 0.0, 1.0);
}

// Normalised depth of a cell (see normalized_image). The HUD does not change it: the window lays
// the HUD flat on top instead, so nothing in the scene moves when HUD elements come and go. Smoke
// and lights drawn after the depth was taken sit on the surface behind them.
float NormalizedDepth(ivec2 cell)
{
	return RawNormalizedDepth(cell);
}

// Whether the live depth at a cell is at the maximum: a HUD drawn with Z writes (only where its
// texels are opaque, so this marks letters, not the boxes around them).
bool IsLiveMax(ivec2 cell)
{
	if (params.live_depth == 0)
		return false;
	const ivec2 c = clamp(cell, ivec2(0), params.out_size - 1);
	const vec2 uv = mix(params.src_uv.xy, params.src_uv.zw, (vec2(c) + 0.5) / vec2(params.out_size));
	return textureLod(live_depth_tex, uv, 0.0).r * params.depth_scale >= MAX_DEPTH;
}

// Within a few cells of a cell whose live depth has steadily been at the maximum (about 2 s, see
// depth_hud.comp): part of a HUD element (a digit that just changed counts at once), not a flash
// or an impact elsewhere.
const float HUD_STEADY = 0.9;
const int HUD_NEAR = 3;
bool IsSteadyHud(ivec2 cell)
{
	for (int dy = -HUD_NEAR; dy <= HUD_NEAR; dy++)
	{
		for (int dx = -HUD_NEAR; dx <= HUD_NEAR; dx++)
		{
			if (imageLoad(hud_history, clamp(cell + ivec2(dx, dy), ivec2(0), params.out_size - 1)).r >= HUD_STEADY)
				return true;
		}
	}
	return false;
}

// Whether the presented colour at a cell differs from the colour when the depth was taken.
bool IsChanged(ivec2 cell)
{
	if (params.hud_threshold <= 0.0)
		return false;
	const vec2 f = (vec2(clamp(cell, ivec2(0), params.out_size - 1)) + 0.5) / vec2(params.out_size);
	const vec4 before = textureLod(prehud_tex, mix(params.src_uv.xy, params.src_uv.zw, f), 0.0);
	const vec2 px = mix(params.draw_rect.xy, params.draw_rect.zw, f);
	const vec4 after = textureLod(colour_tex, px / vec2(params.colour_size), 0.0);
	return any(greaterThan(abs(after.rgb - before.rgb), vec3(params.hud_threshold)));
}

// Whether a cell is HUD. With a live depth: at the maximum now and steadily so. Without one:
// changed since the depth was taken.
bool IsHud(ivec2 cell)
{
	if (params.hud_threshold <= 0.0)
		return false;
	if (params.live_depth != 0)
		return IsLiveMax(cell) && IsSteadyHud(cell);
	return IsChanged(cell);
}
