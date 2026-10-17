// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The user's VR settings, kept across runs in VR.ini in the settings folder (next to PCSX2.ini).
// What the app runs with is the defaults, then the ini, then the command line. Changes made while
// running (Set, or VR.ini edited by hand) are saved to / read from the ini; command-line overrides
// last for the run only, unless such a change replaces them.
//
// Everything here runs on the XR thread.

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class INISettingsInterface;

struct VrSettings
{
	// Screen
	float screen_width = 2.4f; // meters
	float screen_distance = 2.2f; // meters in front of the head
	// Mode: volumetric (geometric stereo: the GS draws the game from where each eye is, parallax by
	// Z), or flat (one picture for both eyes, drawn as stock ARMSX2 draws it). Switches live: the GS
	// rebuilds its render targets at the next vsync (a short pause).
	bool stereo = true;
	// Volumetric+ (with stereo on): a test mode, the scene at a fixed size in the room behind the
	// screen, seen from wherever the eyes are, leaning in or out too (GSVolumetric's portal). Depth
	// then comes from the screen distance and the screen plane, not stereo_depth.
	bool stereo_plus = false;
	// What surrounds the screen: the room through the headset's cameras (passthrough), or the dark void.
	// Off by default (the void); the last choice is kept in VR.ini for the next start.
	bool passthrough = false;
	// Geometric stereo
	float stereo_depth = 3.0f; // meters behind the screen for the farthest geometry
	float stereo_convergence = 0.12f; // normalised Z on the screen plane
	// Wi-Fi transfer
	bool web_enabled = false; // on when the app last closed: started again at launch
	bool web_require_code = true; // the page asks for a 6-digit code shown in the menu
	// Controllers
	bool show_controllers = true; // the hands without a light gun show SteamVR's controller models
	// Graphics
	bool show_perf = false; // the performance window left of the screen (PerfWindow)
	// Light gun (whether it is on is per game, in lightgun.ini: see GunGames)
	float gun_players = 1.0f; // 1 or 2 guns, one per hand
	bool gun_left_hand = false; // the first gun is in the left hand
	float gun_beam = 0.0f; // 0 beam and dot, 1 dot only, 2 neither
	float gun_pedal = 0.0f; // the GunCon button the pedal input presses: 0 A, 1 B, 2 C
	float gun_haptics = 0.6f; // strength of the recoil pulse, 0 off
	float gun_pitch = 0.0f; // degrees the gun is tilted up in the hand
	// The guns' bodies, sRGB 0xRRGGBB: gun 1 alone, gun 1 with two guns, and gun 2.
	uint32_t gun_color = 0x8C8C91; // gray
	uint32_t gun_color_2p = 0xC0282D; // red
	uint32_t gun2_color = 0x2F5FD0; // blue

	// The 3D mode as one value: 0 flat, 1 volumetric, 2 volumetric+.
	int Mode3D() const { return stereo ? (stereo_plus ? 2 : 1) : 0; }
	void SetMode3D(int mode)
	{
		stereo = mode > 0;
		stereo_plus = mode > 1;
	}
	const char* Mode3DName() const { return stereo ? (stereo_plus ? "volumetric+" : "volumetric") : "flat"; }
};

// One setting, for the code that handles them all alike (the ini, the command line, later the menu).
struct VrSettingInfo
{
	const char* section; // in the ini
	const char* key; // in the ini, and the name Override takes
	float VrSettings::*number; // exactly one of number, toggle and color is set
	bool VrSettings::*toggle;
	float min, max; // a number's range; the ini and Override are clamped to it
	bool restart; // only takes effect when the app starts
	uint32_t VrSettings::*color = nullptr; // 0xRRGGBB, "#rrggbb" in the ini
};
std::span<const VrSettingInfo> VrSettingList();
const VrSettingInfo* FindVrSetting(std::string_view key);
bool SameVrSetting(const VrSettingInfo& info, const VrSettings& a, const VrSettings& b);
// "#rrggbb" (or "rrggbb") to 0xRRGGBB, and back.
std::optional<uint32_t> ParseColor(std::string_view text);
std::string FormatColor(uint32_t rgb);

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
