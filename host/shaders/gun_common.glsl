// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light gun's mesh passes (GunRenderer): one part of the model per draw.
layout(push_constant) uniform Draw
{
	mat4 mvp;
	vec4 normal_to_view[3]; // columns: the part's normals to view space
	vec4 colour; // linear rgb, roughness
};
