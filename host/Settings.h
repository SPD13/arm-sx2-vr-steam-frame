// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// The user's VR settings, kept across runs in VR.ini in the settings folder (next to PCSX2.ini).
// What the app runs with is the defaults, then the ini, then the command line. Changes made while
// running (Set, or VR.ini edited by hand) are saved to / read from the ini; command-line overrides
// last for the run only, unless such a change replaces them.
//
// Everything here runs on the XR thread.

#pragma once

#include <chrono>
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <string_view>

class INISettingsInterface;

struct VrSettings
{
	// Screen
	float screen_width = 2.4f; // meters
	float screen_distance = 2.2f; // meters in front of the head
	// Mode, fixed when the app starts
	bool flat = false; // a flat screen (a quad layer) instead of the volumetric window
	bool stereo = false; // geometric stereo: the GS draws the game from where each eye is
	// Volumetric window (depth-map reprojection)
	float depth_range = 0.3f; // meters behind the screen for the farthest depth
	float pop_out = 0.0f; // share of the range in front of the screen
	bool hud_overlay = true; // flat HUD on the screen plane over the scene
	bool edge_fill = false; // background layer behind depth edges
	// Geometric stereo
	float stereo_depth = 1.0f; // meters behind the screen for the farthest geometry
	float stereo_convergence = 1.0f; // Q (or normalised Z in Z mode) on the screen plane
	bool stereo_z_mode = false; // drive the parallax by Z instead of Q
};

// One setting, for the code that handles them all alike (the ini, the command line, later the menu).
struct VrSettingInfo
{
	const char* section; // in the ini
	const char* key; // in the ini, and the name Override takes
	float VrSettings::*number; // exactly one of number and toggle is set
	bool VrSettings::*toggle;
	float min, max; // a number's range; the ini and Override are clamped to it
	bool restart; // only takes effect when the app starts
};
std::span<const VrSettingInfo> VrSettingList();
const VrSettingInfo* FindVrSetting(std::string_view key);
bool SameVrSetting(const VrSettingInfo& info, const VrSettings& a, const VrSettings& b);

class VrSettingsStore
{
public:
	VrSettingsStore();
	~VrSettingsStore();

	// Reads the ini at path; without one, the settings keep their defaults until something changes.
	void Load(std::string path);

	// Sets one setting from text for this run only, as from the command line. False when the key is
	// unknown or the value does not parse.
	bool Override(std::string_view key, std::string_view value);

	const VrSettings& Get() const { return m_current; }
	// Increases with every change, so the app can tell when to apply the settings again.
	unsigned Version() const { return m_version; }

	// Runtime change: the settings that differ from the current ones are saved a moment later.
	void Set(const VrSettings& settings);

	// Call every frame: picks up edits made to the ini by hand (about once a second) and writes it
	// once changes have settled. force writes pending changes now (at exit).
	void Update(bool force = false);

private:
	void ReadIni(const INISettingsInterface& ini, VrSettings* settings) const;
	bool StatIni(std::time_t* mtime, long long* size) const;

	using Clock = std::chrono::steady_clock;

	std::unique_ptr<INISettingsInterface> m_ini; // what the file holds (or will, once saved)
	VrSettings m_current;
	unsigned m_version = 0;
	bool m_dirty = false;
	Clock::time_point m_save_at{};
	Clock::time_point m_next_poll{};
	std::time_t m_file_mtime = 0; // the file as last read or written, to tell edits by hand
	long long m_file_size = -1;
};
