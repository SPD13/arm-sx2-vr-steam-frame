// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The embedded ARMSX2 core: settings, the CPU thread that runs the VM, and the Host:: callbacks
// the core expects from a frontend. The VR side only talks to the core through this interface.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Emu
{
	struct Options
	{
		std::string boot_path; // disc image or ELF; empty: start without a game (the game list opens)
		bool bios_only = false;
		bool fast_boot = true;
		std::optional<int> upscale; // internal resolution multiplier, unset keeps the ini value
		bool sdl_pad = false; // drive pad 1 from the first SDL gamepad (testing without the headset)
		std::string load_state; // save state file to start from (debugging)
	};

	// Folders, fonts and the ini at $XDG_CONFIG_HOME/ps2v/inis/PCSX2.ini (created on first run).
	bool InitializeConfig(const Options& options);

	// Starts the CPU thread: it opens the GS (which adopts the VkInstance/GPU set up in
	// VKLibretro::Init) and then boots options.boot_path.
	void Start(const Options& options);

	// Blocks until the GS device exists (true) or the GS failed to open / the thread exited (false).
	bool WaitForGS();

	void RequestShutdown();
	bool ShutdownRequested();

	// Serial of the running disc (e.g. SLUS-20219), empty before one has booted.
	std::string GetSerial();
	// The running game's file, empty when none runs.
	std::string GetRunningGamePath();

	// The folder games are listed from (and Wi-Fi transfer uploads to), and the files it lists.
	std::string GetGamesFolder();
	const std::vector<std::string>& GetGameExtensions();
	// The core's texture pack folder: textures/<serial>/replacements (see TexturePacks.h).
	std::string GetTexturesFolder();
	// A game's title from its serial: the game list's, else the game database's; empty if unknown. Any thread.
	std::string GetGameTitle(const std::string& serial);
	// The texture pack of a game was added to (turns texture packs on, as ARMSX2 does when it
	// installs one) or removed; the running game's is loaded again. Any thread.
	void TexturePackChanged(const std::string& serial, bool added);

	struct GameInfo
	{
		std::string path;
		std::string title;
		std::string serial; // empty until the game list has read the file
		uint64_t size = 0;
		std::time_t added = 0; // when the file arrived in the games folder
		std::time_t last_played = 0;
		std::time_t played_seconds = 0;
	};
	// Lists the games folder again, in the background; GetGames has the result.
	void RefreshGames();
	bool IsRefreshingGames();
	std::vector<GameInfo> GetGames();
	unsigned GetGamesVersion(); // changes with every new list
	// Stops the running game, if any, and starts this one.
	void BootGame(const std::string& path);
	// Whether a game failed to start since the last call (TakeNotice says why).
	bool TakeBootFailure();

	// Lets the CPU thread close the GS, which destroys the Vulkan device, then joins it and returns
	// its exit code. Call once the XR side no longer uses the device.
	int Join();

	// Pauses or resumes the game (the VR menu pauses it while it is open).
	void SetPaused(bool paused);

	// The emulator settings the VR menu offers, kept in PCSX2.ini. Any thread.
	struct CoreSettings
	{
		int upscale = 1; // internal resolution multiplier
		int texture_filtering = 2; // BiFiltering: 0 nearest, 1 bilinear forced, 2 bilinear as the PS2 asks
		int volume = 100; // percent
		bool muted = false;
		// Texture packs (LoadTextureReplacements, LoadTextureReplacementsAsync, PrecacheTextureReplacements).
		bool texture_packs = false;
		bool texture_packs_async = true; // the game runs on while textures load
		bool texture_packs_precache = false; // the whole pack is loaded into memory as the game starts
	};
	CoreSettings GetCoreSettings();
	// Saves them and has the CPU thread apply them.
	void SetCoreSettings(const CoreSettings& settings);

	// Save states of the running game, in slots 1 to NUM_SAVE_SLOTS. These run on the CPU thread;
	// their outcome comes back through TakeNotice.
	inline constexpr int NUM_SAVE_SLOTS = 5;
	struct SaveSlot
	{
		bool used = false;
		std::time_t time = 0; // when it was saved
	};
	// Reads the slots again (asynchronously); GetSaveSlots has the result.
	void RefreshSaveSlots();
	std::vector<SaveSlot> GetSaveSlots(); // NUM_SAVE_SLOTS entries, slot 1 first
	void SaveState(int slot);
	void LoadState(int slot);
	void ResetGame();
	// A short message for the user about the last of those, once; empty when there is none.
	std::string TakeNotice();

	// DualShock 2 state for port 1, applied from the XR thread each frame.
	struct PadState
	{
		bool up = false, down = false, left = false, right = false;
		bool cross = false, circle = false, square = false, triangle = false;
		bool start = false, select = false, l3 = false, r3 = false;
		float l1 = 0.0f, r1 = 0.0f, l2 = 0.0f, r2 = 0.0f;
		float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f; // -1..1, +y is up
	};
	void SetPad(const PadState& state);

	// Light guns: a GunCon 2 on each USB port, aimed and fired from the XR thread.
	enum GunButton : uint32_t // as usb_lightgun::HostButton
	{
		GUN_C = 1u << 1,
		GUN_B = 1u << 2,
		GUN_A = 1u << 3,
		GUN_UP = 1u << 4,
		GUN_RIGHT = 1u << 5,
		GUN_DOWN = 1u << 6,
		GUN_LEFT = 1u << 7,
		GUN_TRIGGER = 1u << 13,
		GUN_SELECT = 1u << 14,
		GUN_START = 1u << 15,
		GUN_SHOOT_OFFSCREEN = 1u << 16, // the trigger, aimed off the screen: a reload
		GUN_RECALIBRATE = 1u << 17, // Time Crisis's calibration shot
	};
	inline constexpr int NUM_GUNS = 2;
	struct GunState
	{
		bool aim_valid = false; // false: pointing off the picture
		float x = 0.0f, y = 0.0f; // where it points, in pixels of the presented frame (VKLibretro::Frame)
		uint32_t buttons = 0; // GunButton bits
	};
	// The gun on USB port `port` (0 or 1). Any thread; the game reads it when it polls the gun.
	void SetGun(int port, const GunState& state);
	// Hands the USB ports back to their own bindings (light gun mode off).
	void ReleaseGuns();
	// Plugs a GunCon 2 into the first `guns` USB ports (0 to 2) and unplugs the rest, in the running
	// game and for the next ones (PCSX2.ini). Runs on the CPU thread. A port without a gun matters:
	// Time Crisis asks to calibrate every gun plugged in when it starts.
	void SetGunsPlugged(int guns);
	// Decides, as each game starts, how many guns are plugged in for it (called on the CPU thread
	// with the disc serial).
	void SetGunsPolicy(std::function<int(const std::string& serial)> policy);
	// Whether the core knows the game as a GunCon 2 game.
	bool IsLightgunGame(const std::string& serial);
} // namespace Emu
