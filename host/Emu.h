// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The embedded ARMSX2 core: settings, the CPU thread that runs the VM, and the Host:: callbacks
// the core expects from a frontend. The VR side only talks to the core through this interface.

#pragma once

#include <cstdint>
#include <ctime>
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
} // namespace Emu
