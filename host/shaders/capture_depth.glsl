// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The trailer capture (TrailerCapture): an eye's depth over the whole presented frame, as normalised
// Z (larger is nearer), -1 outside the game's picture. LAYERED: the depth image has a layer per eye
// (multiview stereo).

layout(local_size_x = 8, local_size_y = 8) in;

#ifdef LAYERED
layout(binding = 0) uniform sampler2DArray depth_image;
#else
layout(binding = 0) uniform sampler2D depth_image;
#endif
layout(std430, binding = 1) writeonly buffer Texels
{
	float texels[];
};

layout(push_constant) uniform Params
{
	vec4 src_uv; // the presented region of the depth image
	vec4 draw_rect; // where it lands in the presented frame, pixels
	vec2 frame_size; // the presented frame, pixels
	uvec2 size; // the capture's depth plane, over the whole frame
	float depth_scale; // to normalised Z
	uint offset; // where the plane starts in the buffer, in texels
	float layer; // the eye (LAYERED)
};

void main()
{
	const uvec2 cell = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(cell, size)))
		return;
	const vec2 p = (vec2(cell) + 0.5) / vec2(size) * frame_size;
	float z = -1.0;
	if (all(greaterThanEqual(p, draw_rect.xy)) && all(lessThan(p, draw_rect.zw)))
	{
		const vec2 uv = mix(src_uv.xy, src_uv.zw, (p - draw_rect.xy) / (draw_rect.zw - draw_rect.xy));
#ifdef LAYERED
		z = textureLod(depth_image, vec3(uv, layer), 0.0).r * depth_scale;
#else
		z = textureLod(depth_image, uv, 0.0).r * depth_scale;
#endif
	}
	texels[offset + cell.y * size.x + cell.x] = z;
}
