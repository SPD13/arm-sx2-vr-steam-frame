// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A synthetic trailer capture, to test tools/trailer without the headset: a small shooting-gallery
// scene drawn for each eye the way the GS draws Volumetric (every draw shifted by the eye's offset
// times its depth factor, GSVolumetric.h), with a HUD stamped at the Z maximum, an overlay drawn
// without Z and not shifted, a swaying head and a light gun firing at the target. Written through
// the host's own CaptureWriter, so the files are the real format.
//
//   synth_capture OUT_DIR [seconds=4] [width=1280] [mode=volumetric|plus|flat]

#include "capture/CaptureWriter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
	constexpr float PI = 3.14159265f;

	struct Rgba
	{
		float r, g, b;
	};

	struct Setup
	{
		int width = 1280, height = 896;
		CaptureFormat::Mode mode = CaptureFormat::MODE_VOLUMETRIC;
		float convergence = 0.12f, stereo_depth = 3.0f;
		float screen_width = 2.4f, screen_distance = 2.2f;
		float ScreenHeight() const { return screen_width * height / width; }
	};

	struct Eye
	{
		float x, y, distance, depth_factor, ratio;
	};

	// The scene as the game draws it (unshifted), in picture pixels; t in seconds.
	float FloorZ(const Setup& s, float y)
	{
		const float horizon = s.height * 0.52f;
		return 0.012f + std::clamp((y - horizon) / (s.height - horizon), 0.0f, 1.0f) * 0.105f;
	}
	float EnemyX(const Setup& s, float t)
	{
		return s.width * (0.5f + 0.22f * std::sin(t * 1.3f));
	}
	constexpr float ENEMY_Z = 0.09f;

	// The GS's shift for normalised Z, in pixels (x right, y down).
	void Shift(const Setup& s, const Eye& e, float z, float* dx, float* dy)
	{
		float k;
		if (s.mode == CaptureFormat::MODE_FLAT)
			k = 0.0f;
		else if (s.mode == CaptureFormat::MODE_VOLUMETRIC)
			k = e.depth_factor * std::clamp(1.0f - z / s.convergence, 0.0f, 1.0f);
		else
			k = 1.0f - std::clamp(z / s.convergence, 0.0f, 1.0f); // portal at the nominal distance (r = 1)
		*dx = e.x * k * s.width / s.screen_width;
		*dy = -e.y * k * s.height / s.ScreenHeight();
	}

	struct Sample
	{
		Rgba c;
		float z; // written to the depth buffer; < 0: not written
	};

	// One eye's picture and depth at output pixel (x, y): the draws front to back, each sampled
	// where it was before its shift.
	Sample Shade(const Setup& s, const Eye& e, float x, float y, float t, bool flash)
	{
		const float W = float(s.width), H = float(s.height);
		Sample out{{0, 0, 0}, -1.0f};
		float under_z = 0.0f; // depth already written by draws below an overlay

		// Overlay drawn without Z and not shifted (a hit flash ring), over everything but the HUD.
		bool overlay = false;
		Rgba overlay_c{};
		if (flash)
		{
			const float r = std::hypot(x - W * 0.5f, y - H * 0.80f);
			if (r > H * 0.035f && r < H * 0.05f)
			{
				overlay = true;
				overlay_c = {1.0f, 0.85f, 0.2f};
			}
		}
		// HUD at the Z maximum: never shifted (it is on the glass), writes Z.
		{
			const bool time_box = x > W * 0.04f && x < W * 0.26f && y > H * 0.04f && y < H * 0.12f;
			const bool digits = time_box && x > W * 0.15f && x < W * 0.25f && y > H * 0.055f && y < H * 0.105f &&
			                    (int(x / (W * 0.012f)) % 3 != 0);
			bool bullet = false;
			for (int i = 0; i < 6; i++)
			{
				const float bx = W * (0.94f - i * 0.03f), by = H * 0.90f;
				bullet |= std::abs(x - bx) < W * 0.008f && std::abs(y - by) < H * 0.04f;
			}
			if (digits)
				return {{1.0f, 0.9f, 0.1f}, 1.0f};
			if (time_box)
				return {{0.05f, 0.05f, 0.25f}, 1.0f};
			if (bullet)
				return {{0.9f, 0.75f, 0.3f}, 1.0f};
		}
		if (overlay)
			out.c = overlay_c;

		const auto shifted = [&](float z, float* xs, float* ys) {
			float dx, dy;
			Shift(s, e, z, &dx, &dy);
			*xs = x - dx;
			*ys = y - dy;
		};
		Sample scene{{0, 0, 0}, 0.0f};
		bool found = false;
		// Enemy: a capsule, near.
		{
			float xs, ys;
			shifted(ENEMY_Z, &xs, &ys);
			const float cx = EnemyX(s, t), cy = H * 0.62f;
			const float rx = W * 0.035f, ry = H * 0.16f;
			const float u = (xs - cx) / rx, v = (ys - cy) / ry;
			if (u * u + v * v * v * v < 1.0f)
			{
				const bool head = v < -0.6f;
				scene = {head ? Rgba{0.95f, 0.75f, 0.6f} : Rgba{0.75f, 0.1f + 0.1f * (u > 0), 0.12f}, ENEMY_Z};
				found = true;
			}
		}
		// Crates: middle distance.
		if (!found)
		{
			for (int i = 0; i < 3 && !found; i++)
			{
				const float z = 0.05f + 0.012f * i;
				float xs, ys;
				shifted(z, &xs, &ys);
				const float cx = W * (0.22f + 0.28f * i), cy = H * (0.66f - 0.02f * i), half = H * (0.08f + 0.01f * i);
				if (std::abs(xs - cx) < half && std::abs(ys - cy) < half)
				{
					const bool plank = int((ys - cy + half) / (half * 0.4f)) % 2 == 0;
					const bool edge = std::abs(xs - cx) > half * 0.88f || std::abs(ys - cy) > half * 0.88f;
					const float shade = edge ? 0.35f : plank ? 0.62f : 0.52f;
					scene = {{shade, shade * 0.72f, shade * 0.4f}, z};
					found = true;
				}
			}
		}
		// Pillars: far.
		if (!found)
		{
			float xs, ys;
			shifted(0.028f, &xs, &ys);
			for (int i = 0; i < 4 && !found; i++)
			{
				const float cx = W * (0.12f + 0.25f * i);
				if (std::abs(xs - cx) < W * 0.03f && ys > H * 0.18f && ys < H * 0.55f)
				{
					const bool brick = (int(ys / (H * 0.025f)) + int(xs / (W * 0.02f))) % 2 == 0;
					scene = {brick ? Rgba{0.55f, 0.55f, 0.6f} : Rgba{0.45f, 0.45f, 0.5f}, 0.028f};
					found = true;
				}
			}
		}
		// Floor: depth per row, so its shift depends on where it was (a few fixed-point steps).
		if (!found)
		{
			float xs = x, ys = y;
			for (int i = 0; i < 4; i++)
				shifted(FloorZ(s, ys), &xs, &ys);
			const float horizon = H * 0.52f;
			if (ys > horizon + 1.0f)
			{
				const float depth = H * 0.3f / (ys - horizon);
				const float lateral = (xs - W * 0.5f) / (ys - horizon) * 2.0f;
				const bool check = (int(std::floor(depth * 4.0f)) + int(std::floor(lateral * 2.0f))) % 2 == 0;
				const float fog = std::clamp(1.0f / depth, 0.0f, 1.0f);
				const float g = check ? 0.42f : 0.25f;
				scene = {{g * fog + 0.55f * (1 - fog), g * fog + 0.5f * (1 - fog), g * 0.9f * fog + 0.6f * (1 - fog)},
					FloorZ(s, ys)};
				found = true;
			}
		}
		// Sky: no depth (Z 0), the farthest; shifted with it.
		if (!found)
		{
			float xs, ys;
			shifted(0.0f, &xs, &ys);
			const float v = std::clamp(ys / (H * 0.52f), 0.0f, 1.0f);
			const float hills = H * (0.42f + 0.04f * std::sin(xs / W * 9.0f) + 0.02f * std::sin(xs / W * 23.0f));
			if (ys > hills)
				scene = {{0.25f, 0.3f, 0.35f}, 0.0f};
			else
				scene = {{0.2f + 0.7f * v, 0.35f + 0.4f * v, 0.75f - 0.2f * v}, 0.0f};
		}
		under_z = scene.z;
		if (overlay)
			return {overlay_c, under_z};
		return scene;
	}

	uint32_t Pack(const Rgba& c)
	{
		const auto b = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | 0xFF000000u;
	}

	std::string Pose(float px, float py, float pz, float qx = 0, float qy = 0, float qz = 0, float qw = 1)
	{
		char text[160];
		std::snprintf(text, sizeof(text), "[%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f]", px, py, pz, qx, qy, qz, qw);
		return text;
	}

	// A quaternion turning -Z toward dir.
	void LookQuat(float dx, float dy, float dz, float q[4])
	{
		const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
		dx /= len, dy /= len, dz /= len;
		// rotate (0,0,-1) onto d: axis = (0,0,-1) x d, angle = acos(-dz)
		float ax = dy, ay = -dx, az = 0.0f;
		const float alen = std::sqrt(ax * ax + ay * ay);
		const float angle = std::acos(std::clamp(-dz, -1.0f, 1.0f));
		if (alen < 1e-6f)
		{
			q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
			return;
		}
		ax /= alen, ay /= alen;
		const float s = std::sin(angle * 0.5f);
		q[0] = ax * s, q[1] = ay * s, q[2] = az * s, q[3] = std::cos(angle * 0.5f);
	}

	void WriteWav(const std::string& path, float seconds, const std::vector<float>& shots)
	{
		const int rate = 48000;
		const int frames = int(seconds * rate);
		std::vector<int16_t> data(size_t(frames) * 2);
		uint32_t noise = 1;
		for (int i = 0; i < frames; i++)
		{
			const float t = float(i) / rate;
			float v = 0.05f * std::sin(2 * PI * 110.0f * t) + 0.03f * std::sin(2 * PI * 165.0f * t);
			for (const float shot : shots)
			{
				if (t >= shot && t < shot + 0.25f)
				{
					noise = noise * 1664525u + 1013904223u;
					const float n = float(int32_t(noise)) / 2147483648.0f;
					v += 0.6f * n * std::exp(-(t - shot) * 18.0f);
				}
			}
			data[size_t(i) * 2] = data[size_t(i) * 2 + 1] = int16_t(std::clamp(v, -1.0f, 1.0f) * 32000.0f);
		}
		std::FILE* f = std::fopen(path.c_str(), "wb");
		if (!f)
			return;
		const uint32_t bytes = uint32_t(data.size() * 2);
		const auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
		const auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
		std::fwrite("RIFF", 1, 4, f);
		u32(36 + bytes);
		std::fwrite("WAVEfmt ", 1, 8, f);
		u32(16), u16(1), u16(2), u32(rate), u32(rate * 4), u16(4), u16(16);
		std::fwrite("data", 1, 4, f);
		u32(bytes);
		std::fwrite(data.data(), 1, bytes, f);
		std::fclose(f);
	}
} // namespace

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::fprintf(stderr, "Usage: %s OUT_DIR [seconds=4] [width=1280] [mode=volumetric|plus|flat]\n", argv[0]);
		return 1;
	}
	const std::string dir = argv[1];
	const float seconds = argc > 2 ? float(std::atof(argv[2])) : 4.0f;
	Setup s;
	if (argc > 3)
	{
		s.width = std::max(64, std::atoi(argv[3]));
		s.height = s.width * 7 / 10;
	}
	if (argc > 4)
		s.mode = !std::strcmp(argv[4], "flat") ? CaptureFormat::MODE_FLAT :
		         !std::strcmp(argv[4], "plus") ? CaptureFormat::MODE_VOLUMETRIC_PLUS : CaptureFormat::MODE_VOLUMETRIC;

	CaptureWriter writer;
	if (!writer.Start(dir, 3, uint64_t(40e9)))
		return 1;
	const int64_t start_realtime = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();

	// The room: the screen 2.2 m ahead at eye height, facing back (LOCAL space, +Y up).
	const float sx = 0.0f, sy = 1.6f, sz = -s.screen_distance;
	const int frames = int(seconds * 60.0f);
	const int depth_w = s.width / 2, depth_h = s.height / 2; // as at 2x upscale: depth at the PS2's resolution
	const bool stereo = s.mode != CaptureFormat::MODE_FLAT;
	const auto eye_at = [&](float t, int i) {
		const float head_x = 0.12f * std::sin(2 * PI * t / 3.0f);
		const float head_y = 0.02f * std::sin(2 * PI * t / 1.7f);
		const float x = head_x + (i ? 0.032f : -0.032f);
		Eye e{x, head_y, s.screen_distance, 0.0f, 1.0f};
		e.depth_factor = s.mode == CaptureFormat::MODE_VOLUMETRIC ? s.stereo_depth / (e.distance + s.stereo_depth) : 1.0f;
		return e;
	};
	std::vector<float> shots;
	for (float t = 0.6f; t < seconds; t += 0.7f)
		shots.push_back(t);
	const auto flash_at = [&](float t) {
		return std::any_of(shots.begin(), shots.end(), [t](float shot) { return t >= shot && t < shot + 0.12f; });
	};

	std::vector<uint32_t> colour[2];
	std::vector<float> depth[2];
	for (int frame = 0; frame < frames; frame++)
	{
		const float t = frame / 60.0f;
		for (int i = 0; i < (stereo ? 2 : 1); i++)
		{
			const Eye e = stereo ? eye_at(t, i) : Eye{0, 0, s.screen_distance, 0, 1};
			colour[i].assign(size_t(s.width) * s.height, 0);
			depth[i].assign(size_t(depth_w) * depth_h, 0.0f);
			for (int y = 0; y < s.height; y++)
				for (int x = 0; x < s.width; x++)
					colour[i][size_t(y) * s.width + x] = Pack(Shade(s, e, x + 0.5f, y + 0.5f, t, flash_at(t)).c);
			for (int y = 0; y < depth_h; y++)
				for (int x = 0; x < depth_w; x++)
					depth[i][size_t(y) * depth_w + x] = Shade(s, e, (x + 0.5f) * 2.0f, (y + 0.5f) * 2.0f, t, flash_at(t)).z;
		}
		CaptureWriter::Job job;
		CaptureFormat::FrameHeader& h = job.header;
		h.index = uint32_t(frame);
		h.time_ns = int64_t(frame) * 16666667;
		h.realtime_ns = start_realtime + h.time_ns;
		h.width = uint32_t(s.width);
		h.height = uint32_t(s.height);
		h.depth_width = uint32_t(depth_w);
		h.depth_height = uint32_t(depth_h);
		h.draw_rect[2] = float(s.width);
		h.draw_rect[3] = float(s.height);
		h.mode = s.mode;
		h.convergence = s.convergence;
		h.stereo_depth = s.stereo_depth;
		h.screen_width = s.screen_width;
		h.screen_height = s.ScreenHeight();
		h.screen_distance = s.screen_distance;
		for (int i = 0; i < 2; i++)
		{
			const Eye e = stereo ? eye_at(t, i) : Eye{0, 0, s.screen_distance, 0, 1};
			h.eye[i][0] = e.x, h.eye[i][1] = e.y, h.eye[i][2] = e.distance, h.eye[i][3] = stereo ? e.depth_factor : 0.0f;
			h.distance_ratio[i] = 1.0f;
		}
		const auto add = [&](uint32_t kind, const void* data, size_t bytes) {
			const uint32_t p = h.plane_count++;
			h.planes[p].kind = kind;
			h.planes[p].raw_size = uint32_t(bytes);
			job.data[p] = data;
		};
		// The writers read the planes until done: copies they own.
		auto* owned = new std::vector<std::vector<char>>();
		const auto own = [&](const void* data, size_t bytes) {
			owned->emplace_back(static_cast<const char*>(data), static_cast<const char*>(data) + bytes);
			return static_cast<const void*>(owned->back().data());
		};
		owned->reserve(4);
		const size_t cbytes = colour[0].size() * 4, dbytes = depth[0].size() * 4;
		add(CaptureFormat::PLANE_COLOR_LEFT, own(colour[0].data(), cbytes), cbytes);
		if (stereo)
			add(CaptureFormat::PLANE_COLOR_RIGHT, own(colour[1].data(), cbytes), cbytes);
		add(CaptureFormat::PLANE_DEPTH_LEFT, own(depth[0].data(), dbytes), dbytes);
		if (stereo)
			add(CaptureFormat::PLANE_DEPTH_RIGHT, own(depth[1].data(), dbytes), dbytes);
		job.done = [owned] { delete owned; };
		writer.Submit(std::move(job));
		while (writer.Backlog() > 8)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	// A menu from 1.5 s to 3 s: a panel (image 1) in front of the screen, and a button beside it from
	// the atlas (image 2), as VrMenu::DescribeForCapture describes them.
	{
		const auto image = [&](const char* path, uint32_t w, uint32_t h, auto shade) {
			CaptureWriter::Job job;
			job.image_path = path;
			job.image_width = w;
			job.image_height = h;
			job.image.resize(size_t(w) * h);
			for (uint32_t y = 0; y < h; y++)
				for (uint32_t x = 0; x < w; x++)
					job.image[size_t(y) * w + x] = shade(x, y);
			writer.Submit(std::move(job));
		};
		image("ui/000001.bin", 800, 600, [](uint32_t x, uint32_t y) {
			const bool border = x < 6 || y < 6 || x >= 794 || y >= 594;
			const bool row = y > 80 && (y / 60) % 2 == 0 && x > 40 && x < 760;
			const uint32_t a = 235, v = border ? 200 : row ? 70 : 38;
			return v * a / 255 | (v * a / 255) << 8 | ((v + 20) * a / 255) << 16 | a << 24; // premultiplied
		});
		image("ui/000002.bin", 128, 64, [](uint32_t x, uint32_t y) {
			const float d = std::hypot(float(x % 64) - 31.5f, float(y) - 31.5f);
			if (d > 30.0f)
				return 0u;
			const bool red = x >= 64;
			return (red ? 220u : 60u) | (red ? 40u : 120u) << 8 | (red ? 40u : 220u) << 16 | 255u << 24;
		});
	}

	// Poses at 90 Hz.
	writer.WriteLine("{\"t\":0,\"event\":\"game\",\"value\":\"SYNTH-00001 Synthetic Gallery\"}");
	const float half_h = s.ScreenHeight() * 0.5f;
	for (int n = 0; n < int(seconds * 90.0f); n++)
	{
		const float t = n / 90.0f;
		const Eye l = eye_at(t, 0), r = eye_at(t, 1);
		std::string line = "{\"t\":" + std::to_string(int64_t(t * 1e9));
		line += ",\"head\":" + Pose(sx + (l.x + r.x) * 0.5f, sy + l.y, 0.0f);
		line += ",\"eyes\":[{\"p\":" + Pose(sx + l.x, sy + l.y, 0.0f) + ",\"fov\":[-0.87,0.87,0.87,-0.87]}";
		line += ",{\"p\":" + Pose(sx + r.x, sy + r.y, 0.0f) + ",\"fov\":[-0.87,0.87,0.87,-0.87]}]";
		line += ",\"screen\":" + Pose(sx, sy, sz);
		char buf[512];
		std::snprintf(buf, sizeof(buf), ",\"sw\":%.4f,\"sh\":%.4f,\"mode\":%u,\"pt\":0,\"menu\":0,\"gun\":1", s.screen_width,
			s.ScreenHeight(), unsigned(s.mode));
		line += buf;
		// The gun in the right hand, aimed at the target's point behind the glass.
		const float W = float(s.width);
		const float ex = (EnemyX(s, t) / W - 0.5f) * s.screen_width, ey = (0.5f - 0.6f) * 2.0f * half_h;
		float k = 0.0f;
		if (s.mode == CaptureFormat::MODE_VOLUMETRIC)
			k = s.stereo_depth / (s.screen_distance + s.stereo_depth) * (1.0f - ENEMY_Z / s.convergence);
		else if (s.mode == CaptureFormat::MODE_VOLUMETRIC_PLUS)
			k = 1.0f - ENEMY_Z / s.convergence;
		const float d = s.screen_distance * k / (1.0f - k);
		const float tx = sx + ex / (1.0f - k), ty = sy + ey / (1.0f - k), tz = sz - d;
		const float gx = 0.22f + 0.03f * std::sin(t * 2.0f), gy = 1.32f, gz = -0.35f;
		float q[4];
		LookQuat(tx - gx, ty - (gy + 0.09f), tz - gz, q);
		const bool firing = flash_at(t);
		std::snprintf(buf, sizeof(buf),
			",\"guns\":[{\"v\":1,\"p\":%s,\"c\":\"8C8C91\",\"tr\":%.3f,\"bl\":0,\"br\":0,\"beam\":1,\"dot\":1,"
			"\"end\":[%.4f,%.4f,%.4f],\"bc\":\"FF3A30\",\"fire\":%d},{\"v\":0}]",
			Pose(gx, gy, gz, q[0], q[1], q[2], q[3]).c_str(), firing ? 1.0f : 0.0f, tx, ty, tz, firing ? 1 : 0);
		line += buf;
		line += ",\"hands\":[{\"v\":0},{\"v\":0}]";
		if (t >= 1.5f && t < 3.0f)
		{
			std::snprintf(buf, sizeof(buf),
				",\"ui\":{\"panel\":{\"img\":1,\"p\":%s,\"w\":1.9000,\"h\":1.4250},\"atlas\":2,\"pieces\":["
				"{\"r\":[0,0,64,64],\"p\":%s,\"w\":0.1600,\"h\":0.1600,\"s\":1},"
				"{\"r\":[64,0,64,64],\"p\":%s,\"w\":0.1600,\"h\":0.1600,\"s\":1}]}",
				Pose(sx, sy, sz + 0.15f).c_str(), Pose(s.screen_width * 0.5f + 0.12f, 0.2f, 0.0f).c_str(),
				Pose(s.screen_width * 0.5f + 0.12f, -0.0f, 0.0f).c_str());
			line += buf;
		}
		line += "}";
		writer.WriteLine(line);
	}
	writer.Finish();

	char summary[512];
	std::snprintf(summary, sizeof(summary),
		"{\n  \"format\": \"ps2v-trailer-capture\",\n  \"version\": 1,\n  \"started_realtime_ns\": %lld,\n"
		"  \"complete\": true,\n  \"frames_captured\": %d,\n  \"frames_written\": %u,\n  \"frames_dropped\": 0,\n"
		"  \"bytes\": %llu,\n  \"max_width\": 0,\n  \"stopped_by_size_limit\": false,\n  \"synthetic\": true\n}\n",
		(long long)start_realtime, frames, writer.FramesWritten(), (unsigned long long)writer.BytesWritten());
	if (std::FILE* f = std::fopen((dir + "/capture.json").c_str(), "wb"))
	{
		std::fputs(summary, f);
		std::fclose(f);
	}
	WriteWav(dir + "/audio.wav", seconds, shots);
	if (std::FILE* f = std::fopen((dir + "/audio.json").c_str(), "wb"))
	{
		std::fprintf(f, "{\"started_realtime_ns\": %lld}\n", (long long)start_realtime);
		std::fclose(f);
	}
	std::printf("%d frames (%.1f MB) in %s\n", frames, writer.BytesWritten() / 1e6, dir.c_str());
	return 0;
}
