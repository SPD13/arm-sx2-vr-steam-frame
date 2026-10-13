// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Which controller input drives which DualShock 2 control, and which drives which button of the two
// light guns (GunCon 2), set in Settings › Controls and kept in Controls.ini in the settings folder.
// The DualShock 2 set is used while light gun mode is off, the gun sets while it is on.
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

// A light gun's (GunCon 2's) controls, in the order the Controls page lists them. Pedal is no button
// of the gun: it takes cover, by pressing the gun button the game uses for that (VrSettings::gun_pedal).
enum class GunControl : uint8_t
{
	Trigger,
	A,
	B,
	C,
	Start,
	Select,
	Up,
	Down,
	Left,
	Right,
	Reload, // shoot off the screen
	Pedal,
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

	// The light guns: gun 0 is the first (USB port 1), in the gun hand; gun 1 the second, in the other.
	static constexpr int NUM_GUNS = 2;
	static const char* Name(GunControl control);
	ControlInput Get(int gun, GunControl control) const { return m_gun[gun][static_cast<size_t>(control)]; }
	// As Set for the pad, within this gun's set.
	void Set(int gun, GunControl control, ControlInput input);
	void ResetGunToDefaults(int gun);
	// Whether the two guns' sets share an input (the second gun's then wins with two guns).
	bool GunsShare(ControlInput input) const;

	struct Output
	{
		Emu::PadState pad;
		bool recenter = false; // both sticks were just clicked
	};
	// The pad state for this frame's controller state (call every frame: taps and holds are timed).
	Output Apply(const ControllerState& state);

	struct GunOutput
	{
		uint32_t buttons = 0; // Emu::GunButton bits
	};
	struct GunsOutput
	{
		GunOutput gun[NUM_GUNS];
		bool recenter = false; // both sticks were just clicked
	};
	// The guns' buttons for this frame (call every frame in light gun mode, instead of Apply).
	// With one gun only the first set is read; with two, an input in the second set does not count
	// for the first. pedal_button: the Emu::GunButton the pedal presses.
	GunsOutput ApplyGuns(const ControllerState& state, int guns, uint32_t pedal_button);
	// How far the gun's trigger input is pulled (0..1), for its model.
	float GunTrigger(const ControllerState& state, int gun) const { return state[Get(gun, GunControl::Trigger)]; }

	// For mapping: the input that went from released to pressed between before and now, sticks
	// (pushed most of the way) or buttons; None when there is none.
	static ControlInput NewlyPressed(const ControllerState& before, const ControllerState& now, bool sticks);
	// Whether nothing is pressed or pushed.
	static bool AllReleased(const ControllerState& state);

private:
	void Save();

	using Clock = std::chrono::steady_clock;
	// A Start / Select pair on one input: a tap is Start, a hold Select.
	struct TapHold
	{
		bool down = false;
		Clock::time_point down_since{};
		Clock::time_point start_pulse_until{};
		// pressed: the shared input is held; sets start and select.
		void Update(bool pressed, bool* start, bool* select);
	};
	// Both stick clicks: recenter. Returns whether they are held; *now is set on the first frame.
	bool UpdateRecenter(const ControllerState& state, bool* now);

	std::array<ControlInput, static_cast<size_t>(Ps2Control::Count)> m_map;
	std::array<std::array<ControlInput, static_cast<size_t>(GunControl::Count)>, NUM_GUNS> m_gun;
	std::unique_ptr<INISettingsInterface> m_ini;

	TapHold m_pad_start_select;
	TapHold m_gun_start_select[NUM_GUNS];
	bool m_recenter_held = false;
};
