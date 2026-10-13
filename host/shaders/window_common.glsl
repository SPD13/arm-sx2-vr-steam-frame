// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The volumetric window: the presented region of the game on a grid of cells, each pushed back
// from the screen plane by its normalised depth (DepthView's grid: 1 = screen plane, 0 = the full
// depth range behind it). Drawn in up to three passes per eye:
//   mode 0, background: the grid eroded toward far, so depth edges reveal background colours;
//   mode 1, scene: the grid itself, minus triangles stretched across a depth edge;
//   mode 2, HUD: flat on the screen plane over the HUD pixels.
// The scene shows the presented colour, so lights and effects drawn after the depth was taken stay
// at the depth of what they light; under the HUD it shows the pre-HUD colour instead. HUD pixels
// are where the live depth is at the Z format's maximum (a HUD drawn with Z writes), or else where
// the presented colour differs from the pre-HUD colour.

layout(set = 0, binding = 0) uniform Window
{
	mat4 view_proj[2]; // per eye, LOCAL space to clip space
	vec4 origin; // screen centre, LOCAL space
	vec4 right; // half the screen width along its right edge
	vec4 up; // half the screen height along its top edge
	vec4 back; // xyz: away from the viewer, length = depth range; w: pop-out (share of the range in front)
	ivec4 grid; // x, y: grid cells; z: 1 when the frame has depth, 0 to keep it flat; w: HUD_* flags
	vec4 colour_rect; // presented region of the colour image, as UVs
	vec4 scene_rect; // the same region of the pre-HUD colour image, as UVs
	vec4 thresholds; // x: HUD colour difference, y: depth step that counts as an edge, z: live depth scale
	vec4 hud_rects[4]; // the game profile's HUD areas, as fractions of the presented picture
	ivec4 hud_info; // x: how many hud_rects are set
	ivec4 hud_flat; // per HUD area: 1 lays all of it flat
} window;

const int HUD_PREHUD = 1; // the pre-HUD colour is bound
const int HUD_LIVE_DEPTH = 2; // the live depth is bound

layout(push_constant) uniform Pass
{
	int eye;
	int mode;
} pass;
