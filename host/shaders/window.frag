// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#version 450
#extension GL_GOOGLE_include_directive : require

#include "window_common.glsl"

// sRGB views of the game frame and of the pre-HUD colour, so filtering happens in linear light
// and the sRGB swapchain encodes it back; and the live depth for the HUD test.
layout(set = 0, binding = 2) uniform sampler2D colour;
layout(set = 0, binding = 4) uniform sampler2D scene_colour;
layout(set = 0, binding = 5) uniform sampler2D live_depth;
layout(set = 0, binding = 3) uniform sampler2D background_grid; // a = HUD region

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_scene_uv;
layout(location = 2) in float v_stretch;
layout(location = 3) in vec2 v_grid;
layout(location = 0) out vec4 o_colour;

bool InHudRect(int i)
{
	const vec4 r = window.hud_rects[i];
	return all(greaterThanEqual(v_grid, r.xy)) && all(lessThanEqual(v_grid, r.zw));
}

bool IsHud(vec3 presented)
{
	// A profile area laid flat as a whole.
	for (int i = 0; i < window.hud_info.x; i++)
	{
		if (window.hud_flat[i] != 0 && InHudRect(i))
			return true;
	}
	if ((window.grid.w & HUD_PREHUD) == 0)
		return false;
	if ((window.grid.w & HUD_LIVE_DEPTH) != 0)
	{
		// Exactly the pixels the HUD wrote at the maximum depth, where that has been steady for a
		// couple of seconds (its cell's history), so flashes and impacts drawn at the maximum do not
		// count. Lights and effects elsewhere stay in the scene.
		if (textureLod(live_depth, v_scene_uv, 0.0).r * window.thresholds.z < 0.9999)
			return false;
		// With the game's HUD areas known, inside them is HUD at once and outside never.
		if (window.hud_info.x > 0)
		{
			for (int i = 0; i < window.hud_info.x; i++)
			{
				if (InHudRect(i))
					return true;
			}
			return false;
		}
		// A digit that just changed next to a steady one counts at once.
		const ivec2 cell = ivec2(v_grid * vec2(window.grid.xy - 1) + 0.5);
		for (int dy = -3; dy <= 3; dy++)
		{
			for (int dx = -3; dx <= 3; dx++)
			{
				const ivec2 c = clamp(cell + ivec2(dx, dy), ivec2(0), window.grid.xy - 1);
				if (texelFetch(background_grid, c, 0).a >= 0.9)
					return true;
			}
		}
		return false;
	}
	// Compared in a roughly perceptual space.
	const vec3 scene = texture(scene_colour, v_scene_uv).rgb;
	return any(greaterThan(abs(sqrt(presented) - sqrt(scene)), vec3(window.thresholds.x)));
}

void main()
{
	const vec3 presented = texture(colour, v_uv).rgb;
	const bool hud = IsHud(presented);
	if (pass.mode == 2)
	{
		if (!hud)
			discard;
		o_colour = vec4(presented, 1.0);
		return;
	}

	if (pass.mode == 1 && v_stretch > window.thresholds.y)
		discard;
	o_colour = vec4(hud ? texture(scene_colour, v_scene_uv).rgb : presented, 1.0);
}
