// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The trailer capture (TrailerCapture): the HUD mask, one byte per picture pixel (four to a word), 1
// where the depth target at vsync is at the Z format's maximum. The depth plane is a snapshot from
// before the HUD; the live depth is what a HUD drawn with Z writes (Time Crisis's, at the maximum)
// leaves, so the rebuilt scene can keep it on the glass. LAYERED: the left eye's layer (a HUD is not
// shifted, it is at the same place in both).

layout(local_size_x = 8, local_size_y = 8) in;

#ifdef LAYERED
layout(binding = 0) uniform sampler2DArray live_depth;
#else
layout(binding = 0) uniform sampler2D live_depth;
#endif
layout(std430, binding = 1) writeonly buffer Texels
{
	uint texels[];
};

layout(push_constant) uniform Params
{
	vec4 src_uv; // the presented region of the depth image
	vec4 draw_rect; // where it lands in the presented frame, pixels
	vec2 frame_size; // the presented frame, pixels
	uvec2 size; // the capture's picture; a row is (size.x + 3) / 4 words
	float depth_scale; // to normalised Z
	uint offset; // where the mask starts in the buffer, in words
};

float LiveZ(vec2 p)
{
	if (any(lessThan(p, draw_rect.xy)) || any(greaterThanEqual(p, draw_rect.zw)))
		return 0.0;
	const vec2 uv = mix(src_uv.xy, src_uv.zw, (p - draw_rect.xy) / (draw_rect.zw - draw_rect.xy));
#ifdef LAYERED
	return textureLod(live_depth, vec3(uv, 0.0), 0.0).r * depth_scale;
#else
	return textureLod(live_depth, uv, 0.0).r * depth_scale;
#endif
}

void main()
{
	const uvec2 cell = gl_GlobalInvocationID.xy;
	const uint words = (size.x + 3u) / 4u;
	if (cell.x >= words || cell.y >= size.y)
		return;
	uint packed = 0u;
	for (uint i = 0u; i < 4u; i++)
	{
		const uint x = cell.x * 4u + i;
		if (x >= size.x)
			break;
		const vec2 p = (vec2(x, cell.y) + 0.5) / vec2(size) * frame_size;
		if (LiveZ(p) >= 0.999)
			packed |= 1u << (8u * i);
	}
	texels[offset + cell.y * words + cell.x] = packed;
}
