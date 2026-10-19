// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// A light gun's 3D model, assets/lightgun.glb or assets/guncon2.glb (see tools/lightgun/README.md for
// what the host may rely on): its parts as ranges of one vertex and index list, each with its node's pose in the
// model, and the muzzle marker the beam starts from. Metres; +Y up, -Z along the bore, +X the gun's
// right side; the model's origin is the grip centre.

#pragma once

#include <openxr/openxr.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class GunModel
{
public:
	enum class Part
	{
		Shell, // body, material "shell"
		Lens, // body, material "lens"
		Trigger, // pivots at its node origin: pull = negative rotation about local +X
		ButtonLeft, // on the -X side: press = +X
		ButtonRight, // on the +X side: press = -X
		// The GunCon2's extra controls, absent from the PS1 gun. Each node's local +Z points out of the gun.
		ButtonSelect, // press = local -Z
		ButtonStart, // press = local -Z
		ButtonC, // press = local -Z
		Dpad, // local +Y up, +X right: a direction tilts that edge in (about local X or Y)
		Count
	};

	struct Vertex
	{
		float position[3];
		float normal[3];
	};

	struct Draw
	{
		bool present = false;
		uint32_t first_index = 0, index_count = 0;
		XrPosef node{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // the part's node in the model
		float color[3] = {0.5f, 0.5f, 0.5f}; // linear base colour from the file
		float roughness = 0.5f;
	};

	// Reads the file; false (and a log line) when it is unusable. Missing parts are logged and left
	// out.
	bool Load(const std::string& path);
	bool IsLoaded() const { return !m_indices.empty(); }

	const std::vector<Vertex>& Vertices() const { return m_vertices; }
	const std::vector<uint32_t>& Indices() const { return m_indices; }
	const Draw& Get(Part part) const { return m_draws[static_cast<size_t>(part)]; }
	// The aim ray's origin, pointing along its -Z, in the model.
	const XrPosef& Muzzle() const { return m_muzzle; }
	// The corners of a box around the whole gun, in the model (its parts as they rest).
	const std::array<XrVector3f, 8>& BoundsCorners() const { return m_corners; }

private:
	std::vector<Vertex> m_vertices;
	std::vector<uint32_t> m_indices;
	std::array<Draw, static_cast<size_t>(Part::Count)> m_draws{};
	XrPosef m_muzzle{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.09f, -0.2f}};
	std::array<XrVector3f, 8> m_corners{};
};
