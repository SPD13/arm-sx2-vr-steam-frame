// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Controls.h"

#include "common/Console.h"
#include "common/Error.h"

#include "pcsx2/INISettingsInterface.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr size_t NUM_CONTROLS = static_cast<size_t>(Ps2Control::Count);
	constexpr size_t NUM_INPUTS = static_cast<size_t>(ControlInput::Count);
	constexpr float PRESSED = 0.5f; // an analog input counts as a button press from here
	constexpr float STICK_PUSHED = 0.7f; // a stick counts as moved, when mapping, from here
	constexpr float RELEASED = 0.25f;

	// The ini keys.
	constexpr std::array<const char*, NUM_CONTROLS> control_keys = {"cross", "circle", "square", "triangle", "up",
		"down", "left", "right", "start", "l1", "r1", "l2", "r2", "l3", "r3", "select", "left_stick", "right_stick"};
	constexpr std::array<const char*, NUM_INPUTS> input_keys = {"none", "a", "b", "x", "y", "dpad_up", "dpad_down",
		"dpad_left", "dpad_right", "menu", "left_grip", "right_grip", "left_trigger", "right_trigger", "left_stick_click",
		"right_stick_click", "left_stick", "right_stick"};

	// What the controllers did before mapping existed.
	constexpr std::array<ControlInput, NUM_CONTROLS> defaults = {
		ControlInput::A, ControlInput::B, ControlInput::X, ControlInput::Y, // face buttons
		ControlInput::DpadUp, ControlInput::DpadDown, ControlInput::DpadLeft, ControlInput::DpadRight,
		ControlInput::Menu, // Start (tap)
		ControlInput::LeftGrip, ControlInput::RightGrip, ControlInput::LeftTrigger, ControlInput::RightTrigger,
		ControlInput::LeftStickClick, ControlInput::RightStickClick,
		ControlInput::Menu, // Select (hold)
		ControlInput::LeftStick, ControlInput::RightStick};
} // namespace

ControlMap::ControlMap()
	: m_map(defaults)
{
}

ControlMap::~ControlMap() = default;

const char* ControlMap::Name(Ps2Control control)
{
	static constexpr std::array<const char*, NUM_CONTROLS> names = {"Cross", "Circle", "Square", "Triangle",
		"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start", "L1", "R1", "L2", "R2", "L3", "R3", "Select",
		"Left stick", "Right stick"};
	return names[static_cast<size_t>(control)];
}

const char* ControlMap::Name(ControlInput input, bool frame_controllers)
{
	switch (input)
	{
		case ControlInput::None: return "Nothing";
		case ControlInput::A: return "A";
		case ControlInput::B: return "B";
		case ControlInput::X: return frame_controllers ? "X" : "X (left)";
		case ControlInput::Y: return frame_controllers ? "Y" : "Y (left)";
		case ControlInput::DpadUp: return frame_controllers ? "D-pad up" : "Left stick up";
		case ControlInput::DpadDown: return frame_controllers ? "D-pad down" : "Left stick down";
		case ControlInput::DpadLeft: return frame_controllers ? "D-pad left" : "Left stick left";
		case ControlInput::DpadRight: return frame_controllers ? "D-pad right" : "Left stick right";
		case ControlInput::Menu: return frame_controllers ? "View button" : "Menu button";
		case ControlInput::LeftGrip: return "Left grip";
		case ControlInput::RightGrip: return "Right grip";
		case ControlInput::LeftTrigger: return "Left trigger";
		case ControlInput::RightTrigger: return "Right trigger";
		case ControlInput::LeftStickClick: return "Left stick click";
		case ControlInput::RightStickClick: return "Right stick click";
		case ControlInput::LeftStick: return "Left stick";
		case ControlInput::RightStick: return "Right stick";
		default: return "?";
	}
}

void ControlMap::Load(std::string path)
{
	m_ini = std::make_unique<INISettingsInterface>(std::move(path));
	m_map = defaults;
	if (!m_ini->Load())
		return;
	for (size_t c = 0; c < NUM_CONTROLS; c++)
	{
		std::string value;
		if (!m_ini->GetStringValue("Pad1", control_keys[c], &value))
			continue;
		const auto it = std::find_if(input_keys.begin(), input_keys.end(), [&](const char* key) { return value == key; });
		const ControlInput input = it == input_keys.end() ? ControlInput::None : static_cast<ControlInput>(it - input_keys.begin());
		// A stick drives a stick, a button a button.
		if (it == input_keys.end() || IsStick(static_cast<Ps2Control>(c)) != IsStick(input))
			Console.WarningFmt("Controls: ignoring {} = {} in {}", control_keys[c], value, m_ini->GetFileName());
		else
			m_map[c] = input;
	}
}

void ControlMap::Save()
{
	if (!m_ini)
		return;
	for (size_t c = 0; c < NUM_CONTROLS; c++)
		m_ini->SetStringValue("Pad1", control_keys[c], input_keys[static_cast<size_t>(m_map[c])]);
	Error error;
	if (!m_ini->Save(&error))
		Console.ErrorFmt("Controls: cannot save {}: {}", m_ini->GetFileName(), error.GetDescription());
}

void ControlMap::ResetToDefaults()
{
	m_map = defaults;
	Save();
}

void ControlMap::Set(Ps2Control control, ControlInput input)
{
	const size_t index = static_cast<size_t>(control);
	if (IsStick(control) != IsStick(input) || m_map[index] == input)
		return;
	// The controls on input take control's old input, unless another control still has that one
	// (Start and Select share an input by default): then they have nothing until mapped again.
	const ControlInput previous = m_map[index];
	bool previous_kept = false;
	for (size_t c = 0; c < NUM_CONTROLS; c++)
		previous_kept |= c != index && m_map[c] == previous && m_map[c] != input;
	for (size_t c = 0; c < NUM_CONTROLS; c++)
	{
		if (c != index && m_map[c] == input)
			m_map[c] = previous_kept ? ControlInput::None : previous;
	}
	m_map[index] = input;
	Save();
}

ControlMap::Output ControlMap::Apply(const ControllerState& state)
{
	Output out;
	Emu::PadState& pad = out.pad;
	const auto value = [&](Ps2Control control) { return state[Get(control)]; };
	const auto button = [&](Ps2Control control) { return value(control) > PRESSED; };

	// Both stick clicks: recenter, and neither click reaches the game meanwhile.
	const bool recenter = state[ControlInput::LeftStickClick] > PRESSED && state[ControlInput::RightStickClick] > PRESSED;
	out.recenter = recenter && !m_recenter_held;
	m_recenter_held = recenter;
	const auto released_by_recenter = [&](Ps2Control control) {
		const ControlInput input = Get(control);
		return recenter && (input == ControlInput::LeftStickClick || input == ControlInput::RightStickClick);
	};
	const auto pressed = [&](Ps2Control control) { return button(control) && !released_by_recenter(control); };
	const auto analog = [&](Ps2Control control) { return released_by_recenter(control) ? 0.0f : value(control); };

	pad.cross = pressed(Ps2Control::Cross);
	pad.circle = pressed(Ps2Control::Circle);
	pad.square = pressed(Ps2Control::Square);
	pad.triangle = pressed(Ps2Control::Triangle);
	pad.up = pressed(Ps2Control::Up);
	pad.down = pressed(Ps2Control::Down);
	pad.left = pressed(Ps2Control::Left);
	pad.right = pressed(Ps2Control::Right);
	pad.l1 = analog(Ps2Control::L1);
	pad.r1 = analog(Ps2Control::R1);
	pad.l2 = analog(Ps2Control::L2);
	pad.r2 = analog(Ps2Control::R2);
	pad.l3 = pressed(Ps2Control::L3);
	pad.r3 = pressed(Ps2Control::R3);

	if (Get(Ps2Control::Start) == Get(Ps2Control::Select))
	{
		// One input for both: a tap taps Start, holding it holds Select.
		const Clock::time_point now = Clock::now();
		constexpr auto long_press = std::chrono::milliseconds(500);
		const bool down = pressed(Ps2Control::Start);
		if (down && !m_shared_down)
			m_shared_down_since = now;
		if (!down && m_shared_down && now - m_shared_down_since < long_press)
			m_start_pulse_until = now + std::chrono::milliseconds(120);
		m_shared_down = down;
		pad.select = down && now - m_shared_down_since >= long_press;
		pad.start = now < m_start_pulse_until;
	}
	else
	{
		pad.start = pressed(Ps2Control::Start);
		pad.select = pressed(Ps2Control::Select);
	}

	const auto stick = [&](Ps2Control control) {
		return Get(control) == ControlInput::RightStick ? state.right_stick : state.left_stick;
	};
	pad.lx = stick(Ps2Control::LeftStick).x;
	pad.ly = stick(Ps2Control::LeftStick).y;
	pad.rx = stick(Ps2Control::RightStick).x;
	pad.ry = stick(Ps2Control::RightStick).y;
	return out;
}

ControlInput ControlMap::NewlyPressed(const ControllerState& before, const ControllerState& now, bool sticks)
{
	for (size_t i = 1; i < NUM_INPUTS; i++)
	{
		const ControlInput input = static_cast<ControlInput>(i);
		if (IsStick(input) != sticks)
			continue;
		const float threshold = sticks ? STICK_PUSHED : PRESSED;
		if (now[input] > threshold && before[input] <= threshold)
			return input;
	}
	return ControlInput::None;
}

bool ControlMap::AllReleased(const ControllerState& state)
{
	return std::all_of(state.value.begin() + 1, state.value.end(), [](float v) { return v < RELEASED; });
}
