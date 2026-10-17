// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The trailer capture's files (--capture DIR), read back on a computer by tools/trailer:
//
//   capture.json      what was captured: the version, when it started, the totals at the end
//   frames/NNNNNN.bin each emulator frame shown in the headset: a CaptureFrameHeader, then its
//                     planes one after the other, each LZ4-compressed (a raw LZ4 block) or stored as is
//                     when packed_size == raw_size
//   poses.jsonl       a line per headset frame: the head, the eyes, the screen, the guns, the hands and
//                     what was on (see TrailerCapture::RecordState), and events (a game starting)
//   audio.wav         the game's sound, recorded beside it (tools/trailer/capture-frame.sh)
//
// Planes: the left and right eye's pictures (RGBA8, sRGB-encoded as shown, width x height), and each
// eye's depth (float32 normalised Z, depth_width x depth_height over the same picture, larger is
// nearer, -1 outside the game's picture). Flat captures have the left picture only; without depth (a
// movie, a menu) there are no depth planes. All numbers are little-endian.

#pragma once

#include <cstddef>
#include <cstdint>

namespace CaptureFormat
{
	inline constexpr char MAGIC[8] = {'P', 'S', '2', 'V', 'C', 'A', 'P', '1'};

	enum PlaneKind : uint32_t
	{
		PLANE_COLOR_LEFT = 1,
		PLANE_COLOR_RIGHT = 2,
		PLANE_DEPTH_LEFT = 3,
		PLANE_DEPTH_RIGHT = 4,
	};

	enum Mode : uint32_t
	{
		MODE_FLAT = 0,
		MODE_VOLUMETRIC = 1,
		MODE_VOLUMETRIC_PLUS = 2,
	};

	inline constexpr int MAX_PLANES = 4;

	struct Plane
	{
		uint32_t kind;
		uint32_t raw_size;
		uint32_t packed_size; // == raw_size: stored uncompressed
		uint32_t reserved;
	};

	struct FrameHeader
	{
		char magic[8];
		uint32_t header_size; // sizeof(FrameHeader); the planes start here
		uint32_t index; // counts the frames captured, dropped ones included
		int64_t time_ns; // since the capture started (steady clock), when the frame reached the host
		int64_t realtime_ns; // the same moment on the system clock, to line up the sound
		uint32_t width, height; // the pictures
		uint32_t depth_width, depth_height; // the depth planes, 0 without
		uint32_t flags; // reserved
		uint32_t dropped_before; // frames not captured just before this one (the writers were behind)
		float draw_rect[4]; // where the game's picture is in the pictures (x0, y0, x1, y1 pixels)
		uint32_t mode; // Mode
		float convergence; // normalised Z on the glass
		float stereo_depth; // meters behind the glass for the farthest geometry (Volumetric)
		float screen_width, screen_height, screen_distance; // meters
		// Per eye, as the host gave it to the GS for this frame: its position in the screen's frame
		// (x right, y up, z its distance in front of the glass, meters) and the depth factor.
		float eye[2][4];
		float distance_ratio[2]; // Volumetric+: the eye's distance over the screen distance
		uint32_t plane_count;
		uint32_t reserved;
		Plane planes[MAX_PLANES];
	};

	// The layout tools/trailer reads.
	static_assert(offsetof(FrameHeader, time_ns) == 16);
	static_assert(offsetof(FrameHeader, width) == 32);
	static_assert(offsetof(FrameHeader, draw_rect) == 56);
	static_assert(offsetof(FrameHeader, mode) == 72);
	static_assert(offsetof(FrameHeader, eye) == 96);
	static_assert(offsetof(FrameHeader, plane_count) == 136);
	static_assert(offsetof(FrameHeader, planes) == 144);
	static_assert(sizeof(FrameHeader) == 208);
} // namespace CaptureFormat
