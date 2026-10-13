// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Beam: a white core in a coloured glow, fading in from the muzzle. Dot: a white centre in a
// coloured glow, over a dark rim so it shows on light pictures too. Premultiplied alpha, with real
// coverage: SteamVR's quad layers drop light that has no alpha (purely additive).

#version 450

layout(push_constant) uniform Quad
{
	vec4 corners[4];
	vec4 colour;
	vec4 params;
};

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

void main()
{
	if (params.x < 0.5)
	{
		const float across = abs(v_uv.y * 2.0 - 1.0);
		const float glow = exp(-across * across * 6.0);
		const float core = exp(-across * across * 60.0);
		const float along = smoothstep(0.0, 0.25, v_uv.x);
		const float alpha = max(core, glow * 0.75) * along * colour.a;
		const vec3 rgb = mix(colour.rgb, vec3(1.0), core / max(core + glow * 0.75, 1e-4));
		out_colour = vec4(rgb * alpha, alpha);
	}
	else
	{
		const float r = length(v_uv * 2.0 - 1.0);
		if (r > 1.0)
			discard;
		const float centre = 1.0 - smoothstep(0.22, 0.32, r);
		const float glow = (1.0 - smoothstep(0.3, 0.75, r)) * (1.0 - centre);
		const float rim = smoothstep(0.7, 0.8, r) * (1.0 - smoothstep(0.9, 1.0, r));
		const vec3 rgb = vec3(centre) + colour.rgb * glow;
		const float alpha = max(centre, rim * 0.6);
		out_colour = vec4(rgb * colour.a, alpha * colour.a);
	}
}
