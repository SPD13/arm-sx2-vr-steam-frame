// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// The embedded ARMSX2 core: settings, the CPU thread that runs the VM, and the Host:: callbacks
// the core expects from a frontend. The VR side only talks to the core through this interface.

#pragma once

#include <optional>
#include <string>

namespace Emu
{
	struct Options
	{
		std::string boot_path; // disc image or ELF; empty with bios_only
		bool bios_only = false;
		bool fast_boot = true;
		std::optional<int> upscale; // internal resolution multiplier, unset keeps the ini value
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

	// Lets the CPU thread close the GS, which destroys the Vulkan device, then joins it and returns
	// its exit code. Call once the XR side no longer uses the device.
	int Join();

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
