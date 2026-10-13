// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Which controller input drives which DualShock 2 control, set in Settings › Controls and kept in
// Controls.ini in the settings folder.
//
// XrApp reports the controllers as they are (ControllerState: buttons, grips, triggers, sticks);
// ControlMap turns that into the pad state for the game. The triggers can be mapped like any
// input, but they are also the menus' click: while a menu is open, or the pointer is on a menu
// button, the caller blanks them before Apply so the game never sees a menu click.
//
// Fixed, whatever the mapping: clicking both sticks recenters the screen (the controls on the stick
// clicks are released meanwhile), and when Start and Select share an input, a tap is Start and a
// hold is Select.

#pragma once

#include "Emu.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class INISettingsInterface;

// The controllers' inputs. On the Steam Frame all four face buttons are on the right controller and
// the D-pad on the left; with the Touch layout X and Y are on the left and there is no D-pad.
enum class ControlInput : uint8_t
{
	None,
	A,
	B,
	X,
	Y,
	DpadUp,
	DpadDown,
	DpadLeft,
	DpadRight,
	Menu, // View on the Frame, Menu on Touch
	LeftGrip,
	RightGrip,
	LeftTrigger,
	RightTrigger,
	LeftStickClick,
	RightStickClick,
	LeftStick,
	RightStick,
	Count
};

// The DualShock 2's controls, in the order the Controls page lists them.
enum class Ps2Control : uint8_t
{
	Cross,
	Circle,
	Square,
	Triangle,
	Up,
	Down,
	Left,
	Right,
	Start,
	L1,
	R1,
	L2,
	R2,
	L3,
	R3,
	Select,
	LeftStick,
	RightStick,
	Count
};

struct ControllerState
{
	struct Stick
	{
		float x = 0.0f, y = 0.0f; // -1..1, +y is up
	};
	// Per input, 0..1: buttons are 0 or 1, grips and triggers analog. Sticks: their deflection.
	std::array<float, static_cast<size_t>(ControlInput::Count)> value{};
	Stick left_stick, right_stick;

	float operator[](ControlInput input) const { return value[static_cast<size_t>(input)]; }
	float& operator[](ControlInput input) { return value[static_cast<size_t>(input)]; }
};

class ControlMap
{
public:
	ControlMap();
	~ControlMap();

	static const char* Name(Ps2Control control);
	// As the user sees it on their controllers.
	static const char* Name(ControlInput input, bool frame_controllers);
	static bool IsStick(Ps2Control control) { return control == Ps2Control::LeftStick || control == Ps2Control::RightStick; }
	static bool IsStick(ControlInput input) { return input == ControlInput::LeftStick || input == ControlInput::RightStick; }

	// Reads the mapping from path (defaults for what it lacks); changes are saved there.
	void Load(std::string path);
	void ResetToDefaults();

	ControlInput Get(Ps2Control control) const { return m_map[static_cast<size_t>(control)]; }
	// Maps control to input. A control that had input takes control's old input instead, or nothing
	// when another control still has that one.
	void Set(Ps2Control control, ControlInput input);

	struct Output
	{
		Emu::PadState pad;
		bool recenter = false; // both sticks were just clicked
	};
	// The pad state for this frame's controller state (call every frame: taps and holds are timed).
	Output Apply(const ControllerState& state);

	// For mapping: the input that went from released to pressed between before and now, sticks
	// (pushed most of the way) or buttons; None when there is none.
	static ControlInput NewlyPressed(const ControllerState& before, const ControllerState& now, bool sticks);
	// Whether nothing is pressed or pushed.
	static bool AllReleased(const ControllerState& state);

private:
	void Save();

	std::array<ControlInput, static_cast<size_t>(Ps2Control::Count)> m_map;
	std::unique_ptr<INISettingsInterface> m_ini;

	// A shared Start / Select input: tap or hold.
	using Clock = std::chrono::steady_clock;
	bool m_shared_down = false;
	Clock::time_point m_shared_down_since{};
	Clock::time_point m_start_pulse_until{};
	bool m_recenter_held = false;
};
