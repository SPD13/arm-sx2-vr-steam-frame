// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A controller as SteamVR draws it: the render model SteamVR ships for it (OBJ parts, a JSON that
// says how each part moves, and a colour texture), read from the SteamVR install, so the controllers
// in the headset look like the ones in the hands. SteamVR offers no OpenXR extension for its models,
// hence the files.
//
// The model's frame is OpenVR's: metres, +Y up, -Z forward. The JSON places the OpenXR grip and aim
// poses in it, so the model goes where the runtime's poses say. Its parts move as the inputs do:
// buttons slide in, the trigger and grip turn about their pivots, the stick and D-pad tilt.

#pragma once

#include "Controls.h"

#include <openxr/openxr.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class ControllerModel
{
public:
	struct Vertex
	{
		float position[3];
		float normal[3];
		float uv[2];
	};

	// How a part moves (from the JSON's "motion").
	enum class Motion
	{
		None,
		Translate, // along axis, by value_mapping
		Rotate, // about axis through pivot, value_mapping in degrees
		Joystick, // tilts about center in its own frame (frame: X right, Y up the stick)
		Dpad, // as Joystick, from the four directions
	};

	struct Part
	{
		std::string name;
		uint32_t first_index = 0, index_count = 0;
		Motion motion = Motion::None;
		ControlInput input = ControlInput::None; // what moves it; Dpad: DpadUp (all four)
		float range[2] = {}; // value 0 and 1 map to these
		XrVector3f axis{}, pivot{}; // Translate, Rotate
		XrQuaternionf frame{0.0f, 0.0f, 0.0f, 1.0f}; // Joystick, Dpad
		float tilt_x[2] = {}, tilt_y[2] = {}; // degrees at -1 and +1 of each direction
		XrVector3f press{}; // Joystick, Dpad: moved by when clicked, in frame
	};

	// The colour texture, ready for the GPU: sRGB RGBA, each level half the one before, down to 1x1.
	struct Texture
	{
		uint32_t width = 0, height = 0;
		std::vector<std::vector<uint32_t>> levels;
	};

	// Reads the model SteamVR has for name (e.g. "frame_controller_left"); false (and a log line)
	// when there is none. hand (0 left, 1 right) picks the inputs that move its parts.
	bool Load(const std::string& name, int hand);
	bool IsLoaded() const { return !m_indices.empty(); }
	const std::string& Name() const { return m_name; }

	const std::vector<Vertex>& Vertices() const { return m_vertices; }
	const std::vector<uint32_t>& Indices() const { return m_indices; }
	const std::vector<Part>& Parts() const { return m_parts; }
	const Texture& GetTexture() const { return m_texture; }
	// Frees the texture's pixels once they are on the GPU.
	void ReleaseTexture() { m_texture.levels = {}; }

	// The model's pose in the room, from the runtime's grip or aim pose for this controller.
	XrPosef FromGrip(const XrPosef& grip) const;
	XrPosef FromAim(const XrPosef& aim) const;
	// A part's pose in the model as the controller's inputs are now.
	XrPosef PartPose(const Part& part, const ControllerState& state) const;
	// The corners of a box around the whole model.
	const std::array<XrVector3f, 8>& BoundsCorners() const { return m_corners; }

	// Where SteamVR is installed, from OpenVR's paths file (else the usual places); empty if not found.
	static std::string FindSteamVR();

private:
	bool LoadObj(const std::string& path, Part* part);
	bool LoadTexture(const std::string& colour_path, const std::string& ao_path);

	std::string m_name;
	std::vector<Vertex> m_vertices;
	std::vector<uint32_t> m_indices;
	std::vector<Part> m_parts;
	Texture m_texture;
	XrPosef m_grip{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // the OpenXR grip pose in the model
	XrPosef m_aim{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // and the aim pose
	std::array<XrVector3f, 8> m_corners{};
};
