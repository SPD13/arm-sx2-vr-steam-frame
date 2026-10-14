// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ControllerModel.h"

#include "XrMath.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Image.h"

#include "rapidjson/document.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace
{
	constexpr float DEG = 3.14159265f / 180.0f;
	// The texture's largest level: the controllers are small in the picture, and SteamVR's are 2048.
	constexpr uint32_t MAX_TEXTURE_SIZE = 1024;

	XrVector3f Vec3(const rapidjson::Value& v, XrVector3f fallback = {})
	{
		if (!v.IsArray() || v.Size() < 3 || !v[0].IsNumber() || !v[1].IsNumber() || !v[2].IsNumber())
			return fallback;
		return {v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat()};
	}

	const rapidjson::Value* Member(const rapidjson::Value& object, const char* name)
	{
		if (!object.IsObject())
			return nullptr;
		const auto it = object.FindMember(name);
		return it == object.MemberEnd() ? nullptr : &it->value;
	}

	// The JSON's rotate_xyz, degrees: about X, then Y, then Z (fixed axes).
	XrQuaternionf RotateXyz(const XrVector3f& degrees)
	{
		return QuatFromAxisAngle({0.0f, 0.0f, 1.0f}, degrees.z * DEG) * QuatFromAxisAngle({0.0f, 1.0f, 0.0f}, degrees.y * DEG) *
		       QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, degrees.x * DEG);
	}

	// A component's component_local pose: where e.g. the OpenXR grip pose is in the model.
	bool LocalPose(const rapidjson::Value& components, const char* name, XrPosef* pose)
	{
		const rapidjson::Value* component = Member(components, name);
		const rapidjson::Value* local = component ? Member(*component, "component_local") : nullptr;
		if (!local)
			return false;
		const rapidjson::Value* origin = Member(*local, "origin");
		const rapidjson::Value* rotate = Member(*local, "rotate_xyz");
		pose->position = origin ? Vec3(*origin) : XrVector3f{};
		pose->orientation = rotate ? RotateXyz(Vec3(*rotate)) : XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
		return true;
	}

	XrPosef Inverse(const XrPosef& pose)
	{
		const XrQuaternionf inv = Conjugate(pose.orientation);
		return {inv, Rotate(inv, pose.position) * -1.0f};
	}

	// The input that moves a part, from its OpenVR input path, on this hand.
	ControlInput InputForPath(std::string_view path, int hand)
	{
		const bool left = hand == 0;
		if (path.starts_with("/input/trigger"))
			return left ? ControlInput::LeftTrigger : ControlInput::RightTrigger;
		if (path.starts_with("/input/grip"))
			return left ? ControlInput::LeftGrip : ControlInput::RightGrip;
		if (path.starts_with("/input/thumbstick"))
			return left ? ControlInput::LeftStick : ControlInput::RightStick;
		if (path == "/input/a/click")
			return ControlInput::A;
		if (path == "/input/b/click")
			return ControlInput::B;
		if (path == "/input/x/click")
			return ControlInput::X;
		if (path == "/input/y/click")
			return ControlInput::Y;
		if (path == "/input/view/click") // the Frame's View button, our Menu input (see XrApp)
			return ControlInput::Menu;
		return ControlInput::None; // inputs the app does not read: system, the right menu button, bumpers
	}

	float Lerp(const float range[2], float t)
	{
		return range[0] + (range[1] - range[0]) * t;
	}

	float SrgbToLinear(float c)
	{
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	}

	uint8_t LinearToSrgb8(float c)
	{
		c = std::clamp(c, 0.0f, 1.0f);
		const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8_t>(s * 255.0f + 0.5f);
	}

	// Parses a float at p, moving p past it; 0 when there is none.
	float ParseFloat(const char*& p, const char* end)
	{
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		float value = 0.0f;
		const auto [next, ec] = std::from_chars(p, end, value);
		if (ec == std::errc())
			p = next;
		return value;
	}
} // namespace

std::string ControllerModel::FindSteamVR()
{
	std::vector<std::string> candidates;
	const char* home = std::getenv("HOME");
	const char* config = std::getenv("XDG_CONFIG_HOME");
	const std::string config_dir = config && *config ? std::string(config) : home ? std::string(home) + "/.config" : std::string();
	if (!config_dir.empty())
	{
		// OpenVR's own record of where its runtime is.
		if (const std::optional<std::string> text = FileSystem::ReadFileToString((config_dir + "/openvr/openvrpaths.vrpath").c_str()))
		{
			rapidjson::Document doc;
			doc.Parse(text->c_str());
			const rapidjson::Value* runtime = doc.HasParseError() ? nullptr : Member(doc, "runtime");
			if (runtime && runtime->IsArray())
			{
				for (const rapidjson::Value& path : runtime->GetArray())
				{
					if (path.IsString())
						candidates.emplace_back(path.GetString());
				}
			}
		}
	}
	candidates.emplace_back("/opt/steamvr"); // SteamOS on the Steam Frame
	if (home)
	{
		candidates.push_back(std::string(home) + "/.local/share/Steam/steamapps/common/SteamVR");
		candidates.push_back(std::string(home) + "/.steam/steam/steamapps/common/SteamVR");
	}
	for (const std::string& dir : candidates)
	{
		if (FileSystem::DirectoryExists((dir + "/resources/rendermodels").c_str()))
			return dir;
	}
	return {};
}

bool ControllerModel::Load(const std::string& name, int hand)
{
	const auto start = std::chrono::steady_clock::now();
	m_name = name;
	m_vertices.clear();
	m_indices.clear();
	m_parts.clear();
	m_texture = {};

	const std::string steamvr = FindSteamVR();
	if (steamvr.empty())
	{
		Console.Warning("Controllers: SteamVR not found; no controller models.");
		return false;
	}
	// SteamVR's own models, then each driver's.
	std::string dir = steamvr + "/resources/rendermodels/" + name;
	if (!FileSystem::DirectoryExists(dir.c_str()))
	{
		dir.clear();
		FileSystem::FindResultsArray drivers;
		FileSystem::FindFiles((steamvr + "/drivers").c_str(), "*", FILESYSTEM_FIND_FOLDERS, &drivers);
		for (const FILESYSTEM_FIND_DATA& driver : drivers)
		{
			const std::string candidate = driver.FileName + "/resources/rendermodels/" + name;
			if (FileSystem::DirectoryExists(candidate.c_str()))
			{
				dir = candidate;
				break;
			}
		}
	}
	if (dir.empty())
	{
		Console.WarningFmt("Controllers: SteamVR ({}) has no model {}.", steamvr, name);
		return false;
	}

	const std::string json_path = dir + "/" + name + ".json";
	const std::optional<std::string> json = FileSystem::ReadFileToString(json_path.c_str());
	rapidjson::Document doc;
	if (json)
		doc.Parse(json->c_str());
	const rapidjson::Value* components = json && !doc.HasParseError() ? Member(doc, "components") : nullptr;
	if (!components || !components->IsObject())
	{
		Console.WarningFmt("Controllers: {} is unreadable.", json_path);
		return false;
	}
	if (!LocalPose(*components, "openxr_grip", &m_grip) || !LocalPose(*components, "openxr_aim", &m_aim))
	{
		Console.WarningFmt("Controllers: {} does not place the OpenXR poses.", json_path);
		return false;
	}

	// Every part that is shown by default, with how it moves.
	for (const auto& member : components->GetObject())
	{
		const rapidjson::Value& component = member.value;
		const rapidjson::Value* filename = Member(component, "filename");
		if (!filename || !filename->IsString())
			continue; // a pose marker
		const rapidjson::Value* visibility = Member(component, "visibility");
		const rapidjson::Value* shown = visibility ? Member(*visibility, "default") : nullptr;
		if (shown && shown->IsBool() && !shown->GetBool())
			continue; // e.g. the status light

		Part part;
		part.name = member.name.GetString();
		if (const rapidjson::Value* motion = Member(component, "motion"))
		{
			const rapidjson::Value* type = Member(*motion, "type");
			const std::string_view kind = type && type->IsString() ? type->GetString() : "";
			const auto path = [&](const char* key) {
				const rapidjson::Value* v = Member(*motion, key);
				return v && v->IsString() ? std::string_view(v->GetString()) : std::string_view();
			};
			const auto pair = [&](const char* key, float out[2]) {
				const rapidjson::Value* v = Member(*motion, key);
				if (v && v->IsArray() && v->Size() >= 2 && (*v)[0].IsNumber() && (*v)[1].IsNumber())
				{
					out[0] = (*v)[0].GetFloat();
					out[1] = (*v)[1].GetFloat();
				}
			};
			const auto vec = [&](const char* key) {
				const rapidjson::Value* v = Member(*motion, key);
				return v ? Vec3(*v) : XrVector3f{};
			};
			if (kind == "translate" || kind == "rotate")
			{
				part.motion = kind == "translate" ? Motion::Translate : Motion::Rotate;
				part.input = InputForPath(!path("pressed_path").empty() ? path("pressed_path") : path("trigger_path"), hand);
				pair("value_mapping", part.range);
				part.axis = Normalize(vec("axis"));
				part.pivot = vec("pivot");
			}
			else if (kind == "joystick" || kind == "dpad")
			{
				part.motion = kind == "joystick" ? Motion::Joystick : Motion::Dpad;
				part.input = kind == "joystick" ? InputForPath(path("joystick_path"), hand) : ControlInput::DpadUp;
				part.pivot = vec("center");
				part.frame = RotateXyz(vec("rotate_xyz"));
				pair("joystick_rotation_x", part.tilt_x);
				pair("joystick_rotation_y", part.tilt_y);
				part.press = vec("press_translate");
			}
			if (part.input == ControlInput::None)
				part.motion = Motion::None;
		}
		if (!LoadObj(dir + "/" + filename->GetString(), &part))
		{
			Console.WarningFmt("Controllers: {} has an unreadable part {}.", name, filename->GetString());
			return false;
		}
		m_parts.push_back(std::move(part));
	}
	if (m_indices.empty())
	{
		Console.WarningFmt("Controllers: {} has no parts.", name);
		return false;
	}

	// The colour texture, with the ambient occlusion baked in (the model's other maps are left out).
	if (!LoadTexture(dir + "/" + name + "_color.png", dir + "/" + name + "_ao.png"))
	{
		Console.WarningFmt("Controllers: {} has no colour texture.", name);
		m_indices.clear();
		return false;
	}

	XrVector3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const Vertex& v : m_vertices)
	{
		lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2])};
		hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2])};
	}
	for (int i = 0; i < 8; i++)
		m_corners[i] = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
	Console.WriteLnFmt("Controllers: {} from {}, {} parts, {} triangles, read in {} ms", name, dir, m_parts.size(),
		m_indices.size() / 3,
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	return true;
}

bool ControllerModel::LoadObj(const std::string& path, Part* part)
{
	const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
	if (!text)
		return false;
	std::vector<XrVector3f> positions, normals;
	std::vector<std::array<float, 2>> uvs;
	// One vertex per distinct position/uv/normal triple, in this part.
	struct Key
	{
		int v, t, n;
		bool operator==(const Key&) const = default;
	};
	struct KeyHash
	{
		size_t operator()(const Key& k) const { return (static_cast<size_t>(k.v) * 73856093u) ^ (k.t * 19349663u) ^ (k.n * 83492791u); }
	};
	std::unordered_map<Key, uint32_t, KeyHash> index_of;
	part->first_index = static_cast<uint32_t>(m_indices.size());

	const char* p = text->data();
	const char* const end = p + text->size();
	while (p < end)
	{
		const char* line_end = static_cast<const char*>(std::memchr(p, '\n', end - p));
		if (!line_end)
			line_end = end;
		if (line_end - p > 2 && p[0] == 'v' && p[1] == ' ')
		{
			const char* q = p + 2;
			const float x = ParseFloat(q, line_end), y = ParseFloat(q, line_end), z = ParseFloat(q, line_end);
			positions.push_back({x, y, z});
		}
		else if (line_end - p > 3 && p[0] == 'v' && p[1] == 'n' && p[2] == ' ')
		{
			const char* q = p + 3;
			const float x = ParseFloat(q, line_end), y = ParseFloat(q, line_end), z = ParseFloat(q, line_end);
			normals.push_back({x, y, z});
		}
		else if (line_end - p > 3 && p[0] == 'v' && p[1] == 't' && p[2] == ' ')
		{
			const char* q = p + 3;
			const float u = ParseFloat(q, line_end), v = ParseFloat(q, line_end);
			uvs.push_back({u, 1.0f - v}); // OBJ's v goes up, images' rows down
		}
		else if (line_end - p > 2 && p[0] == 'f' && p[1] == ' ')
		{
			// A polygon of v/t/n corners (1-based, negative from the end), as a fan of triangles.
			uint32_t corners[16];
			int count = 0;
			const char* q = p + 2;
			while (q < line_end && count < 16)
			{
				while (q < line_end && (*q == ' ' || *q == '\t' || *q == '\r'))
					q++;
				if (q >= line_end)
					break;
				int idx[3] = {0, 0, 0};
				for (int k = 0; k < 3 && q < line_end && *q != ' '; k++)
				{
					if (*q != '/')
					{
						const auto [next, ec] = std::from_chars(q, line_end, idx[k]);
						if (ec != std::errc())
							return false;
						q = next;
					}
					if (q < line_end && *q == '/')
						q++;
				}
				const auto resolve = [](int i, size_t size) { return i < 0 ? static_cast<int>(size) + i : i - 1; };
				const Key key{resolve(idx[0], positions.size()), idx[1] ? resolve(idx[1], uvs.size()) : -1,
					idx[2] ? resolve(idx[2], normals.size()) : -1};
				if (key.v < 0 || key.v >= static_cast<int>(positions.size()) || key.t >= static_cast<int>(uvs.size()) ||
					key.n >= static_cast<int>(normals.size()))
					return false;
				auto [it, added] = index_of.try_emplace(key, static_cast<uint32_t>(m_vertices.size()));
				if (added)
				{
					Vertex vertex{};
					const XrVector3f& pos = positions[key.v];
					const XrVector3f n = key.n >= 0 ? normals[key.n] : XrVector3f{0.0f, 1.0f, 0.0f};
					vertex.position[0] = pos.x, vertex.position[1] = pos.y, vertex.position[2] = pos.z;
					vertex.normal[0] = n.x, vertex.normal[1] = n.y, vertex.normal[2] = n.z;
					if (key.t >= 0)
						vertex.uv[0] = uvs[key.t][0], vertex.uv[1] = uvs[key.t][1];
					m_vertices.push_back(vertex);
				}
				corners[count++] = it->second;
			}
			for (int k = 2; k < count; k++)
				m_indices.insert(m_indices.end(), {corners[0], corners[k - 1], corners[k]});
		}
		p = line_end + 1;
	}
	part->index_count = static_cast<uint32_t>(m_indices.size()) - part->first_index;
	return part->index_count > 0;
}

bool ControllerModel::LoadTexture(const std::string& colour_path, const std::string& ao_path)
{
	RGBA8Image colour, ao;
	if (!colour.LoadFromFile(colour_path.c_str()) || !colour.GetWidth() || !colour.GetHeight())
		return false;
	const bool has_ao = ao.LoadFromFile(ao_path.c_str()) && ao.GetWidth() == colour.GetWidth() && ao.GetHeight() == colour.GetHeight();

	// Linear light, so the levels average right.
	uint32_t w = colour.GetWidth(), h = colour.GetHeight();
	std::vector<float> linear(static_cast<size_t>(w) * h * 3);
	float to_linear[256];
	for (int i = 0; i < 256; i++)
		to_linear[i] = SrgbToLinear(i / 255.0f);
	for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
	{
		const uint32_t c = colour.GetPixels()[i];
		const float occlusion = has_ao ? (ao.GetPixels()[i] & 0xFF) / 255.0f : 1.0f; // grey: the red byte
		for (int ch = 0; ch < 3; ch++)
			linear[i * 3 + ch] = to_linear[(c >> (8 * ch)) & 0xFF] * occlusion;
	}

	const auto halve = [](std::vector<float>& pixels, uint32_t& w, uint32_t& h) {
		const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
		std::vector<float> out(static_cast<size_t>(nw) * nh * 3);
		for (uint32_t y = 0; y < nh; y++)
		{
			for (uint32_t x = 0; x < nw; x++)
			{
				for (int ch = 0; ch < 3; ch++)
				{
					float sum = 0.0f;
					for (uint32_t dy = 0; dy < 2; dy++)
					{
						for (uint32_t dx = 0; dx < 2; dx++)
						{
							const uint32_t sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
							sum += pixels[(static_cast<size_t>(sy) * w + sx) * 3 + ch];
						}
					}
					out[(static_cast<size_t>(y) * nw + x) * 3 + ch] = 0.25f * sum;
				}
			}
		}
		pixels = std::move(out);
		w = nw;
		h = nh;
	};
	while (w > MAX_TEXTURE_SIZE || h > MAX_TEXTURE_SIZE)
		halve(linear, w, h);

	m_texture.width = w;
	m_texture.height = h;
	for (;;)
	{
		std::vector<uint32_t> level(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < level.size(); i++)
		{
			level[i] = LinearToSrgb8(linear[i * 3]) | (LinearToSrgb8(linear[i * 3 + 1]) << 8) |
			           (LinearToSrgb8(linear[i * 3 + 2]) << 16) | 0xFF000000u;
		}
		m_texture.levels.push_back(std::move(level));
		if (w == 1 && h == 1)
			break;
		halve(linear, w, h);
	}
	return true;
}

XrPosef ControllerModel::FromGrip(const XrPosef& grip) const
{
	return Compose(grip, Inverse(m_grip));
}

XrPosef ControllerModel::FromAim(const XrPosef& aim) const
{
	return Compose(aim, Inverse(m_aim));
}

XrPosef ControllerModel::PartPose(const Part& part, const ControllerState& state) const
{
	XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	// A turn q about the pivot, then a shift.
	const auto about_pivot = [&](const XrQuaternionf& q, const XrVector3f& shift) {
		pose.orientation = q;
		pose.position = part.pivot - Rotate(q, part.pivot) + shift;
	};
	switch (part.motion)
	{
		case Motion::Translate:
			pose.position = part.axis * Lerp(part.range, std::clamp(state[part.input], 0.0f, 1.0f));
			break;
		case Motion::Rotate:
			about_pivot(QuatFromAxisAngle(part.axis, Lerp(part.range, std::clamp(state[part.input], 0.0f, 1.0f)) * DEG), {});
			break;
		case Motion::Joystick:
		case Motion::Dpad:
		{
			float x, y;
			bool pressed;
			if (part.motion == Motion::Joystick)
			{
				const bool left = part.input == ControlInput::LeftStick;
				const ControllerState::Stick& stick = left ? state.left_stick : state.right_stick;
				x = stick.x;
				y = stick.y;
				pressed = state[left ? ControlInput::LeftStickClick : ControlInput::RightStickClick] > 0.5f;
			}
			else
			{
				x = state[ControlInput::DpadRight] - state[ControlInput::DpadLeft];
				y = state[ControlInput::DpadUp] - state[ControlInput::DpadDown];
				pressed = x != 0.0f || y != 0.0f;
			}
			// In the stick's frame: sideways turns about its Z, forward about its X.
			const float side = Lerp(part.tilt_x, 0.5f * (std::clamp(x, -1.0f, 1.0f) + 1.0f)) * DEG;
			const float forward = Lerp(part.tilt_y, 0.5f * (std::clamp(y, -1.0f, 1.0f) + 1.0f)) * DEG;
			const XrQuaternionf tilt =
				QuatFromAxisAngle({0.0f, 0.0f, 1.0f}, side) * QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, forward);
			about_pivot(part.frame * tilt * Conjugate(part.frame), pressed ? Rotate(part.frame, part.press) : XrVector3f{});
			break;
		}
		case Motion::None:
			break;
	}
	return pose;
}
