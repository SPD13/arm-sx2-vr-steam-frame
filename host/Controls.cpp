// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Controls.h"

#include "common/Console.h"
#include "common/Error.h"

#include "pcsx2/INISettingsInterface.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

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

	constexpr size_t NUM_GUN_CONTROLS = static_cast<size_t>(GunControl::Count);
	constexpr std::array<const char*, NUM_GUN_CONTROLS> gun_keys = {
		"trigger", "a", "b", "c", "start", "select", "up", "down", "left", "right", "reload", "calibrate", "pedal"};
	constexpr std::array<const char*, ControlMap::NUM_GUNS> gun_sections = {"LightGun1", "LightGun2"};
	// Their names before 2026-10-18, still read when the new ones are absent.
	constexpr std::array<const char*, ControlMap::NUM_GUNS> old_gun_sections = {"GunCon1", "GunCon2"};

	// The first gun in the right hand: the other hand's grip is the pedal, Y the calibration shot.
	// The second gun in the left hand: on the Steam Frame that controller has no face buttons, so its
	// grip, D-pad and stick click stand in.
	constexpr std::array<std::array<ControlInput, NUM_GUN_CONTROLS>, ControlMap::NUM_GUNS> gun_defaults = {{
		{ControlInput::RightTrigger, ControlInput::A, ControlInput::B, ControlInput::RightGrip, ControlInput::Menu,
			ControlInput::Menu, ControlInput::DpadUp, ControlInput::DpadDown, ControlInput::DpadLeft,
			ControlInput::DpadRight, ControlInput::None, ControlInput::Y, ControlInput::LeftGrip},
		{ControlInput::LeftTrigger, ControlInput::LeftGrip, ControlInput::DpadDown, ControlInput::DpadUp,
			ControlInput::LeftStickClick, ControlInput::None, ControlInput::None, ControlInput::None, ControlInput::None,
			ControlInput::None, ControlInput::None, ControlInput::DpadLeft, ControlInput::None},
	}};

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
	, m_gun(gun_defaults)
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

const char* ControlMap::Name(GunControl control)
{
	static constexpr std::array<const char*, NUM_GUN_CONTROLS> names = {"Trigger", "A", "B", "C", "Start", "Select",
		"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Reload", "Calibration shot", "Pedal (cover)"};
	return names[static_cast<size_t>(control)];
}

// The input a key names in the ini, if it is one that `stick` (a stick or not) can take.
static std::optional<ControlInput> ParseInput(const std::string& value, bool stick)
{
	const auto it = std::find_if(input_keys.begin(), input_keys.end(), [&](const char* key) { return value == key; });
	if (it == input_keys.end())
		return std::nullopt;
	const ControlInput input = static_cast<ControlInput>(it - input_keys.begin());
	// A stick drives a stick, a button a button.
	if (input != ControlInput::None && ControlMap::IsStick(input) != stick)
		return std::nullopt;
	return input;
}

void ControlMap::Load(std::string path)
{
	m_ini = std::make_unique<INISettingsInterface>(std::move(path));
	m_map = defaults;
	m_gun = gun_defaults;
	if (!m_ini->Load())
		return;
	for (size_t c = 0; c < NUM_CONTROLS; c++)
	{
		std::string value;
		if (!m_ini->GetStringValue("Pad1", control_keys[c], &value))
			continue;
		if (const std::optional<ControlInput> input = ParseInput(value, IsStick(static_cast<Ps2Control>(c))))
			m_map[c] = *input;
		else
			Console.WarningFmt("Controls: ignoring {} = {} in {}", control_keys[c], value, m_ini->GetFileName());
	}
	for (int gun = 0; gun < NUM_GUNS; gun++)
	{
		const char* section = m_ini->ContainsValue(gun_sections[gun], gun_keys[0]) ? gun_sections[gun] : old_gun_sections[gun];
		for (size_t c = 0; c < NUM_GUN_CONTROLS; c++)
		{
			std::string value;
			if (!m_ini->GetStringValue(section, gun_keys[c], &value))
				continue;
			if (const std::optional<ControlInput> input = ParseInput(value, false))
				m_gun[gun][c] = *input;
			else
				Console.WarningFmt("Controls: ignoring [{}] {} = {} in {}", section, gun_keys[c], value,
					m_ini->GetFileName());
		}
	}
}

void ControlMap::Save()
{
	if (!m_ini)
		return;
	for (size_t c = 0; c < NUM_CONTROLS; c++)
		m_ini->SetStringValue("Pad1", control_keys[c], input_keys[static_cast<size_t>(m_map[c])]);
	for (int gun = 0; gun < NUM_GUNS; gun++)
	{
		for (size_t c = 0; c < NUM_GUN_CONTROLS; c++)
			m_ini->SetStringValue(gun_sections[gun], gun_keys[c], input_keys[static_cast<size_t>(m_gun[gun][c])]);
		m_ini->RemoveSection(old_gun_sections[gun]);
	}
	Error error;
	if (!m_ini->Save(&error))
		Console.ErrorFmt("Controls: cannot save {}: {}", m_ini->GetFileName(), error.GetDescription());
}

void ControlMap::ResetToDefaults()
{
	m_map = defaults;
	Save();
}

// Maps map[index] to input. The entries on input take index's old input, unless another entry still
// has that one (Start and Select share an input by default): then they have nothing until mapped again.
template <size_t N>
static void Remap(std::array<ControlInput, N>& map, size_t index, ControlInput input)
{
	const ControlInput previous = map[index];
	bool previous_kept = false;
	for (size_t c = 0; c < N; c++)
		previous_kept |= c != index && map[c] == previous && map[c] != input;
	for (size_t c = 0; c < N; c++)
	{
		if (c != index && map[c] == input)
			map[c] = previous_kept ? ControlInput::None : previous;
	}
	map[index] = input;
}

void ControlMap::Set(Ps2Control control, ControlInput input)
{
	const size_t index = static_cast<size_t>(control);
	if (IsStick(control) != IsStick(input) || m_map[index] == input)
		return;
	Remap(m_map, index, input);
	Save();
}

void ControlMap::Set(int gun, GunControl control, ControlInput input)
{
	const size_t index = static_cast<size_t>(control);
	if (IsStick(input) || m_gun[gun][index] == input)
		return;
	Remap(m_gun[gun], index, input);
	Save();
}

void ControlMap::ResetGunToDefaults(int gun)
{
	m_gun[gun] = gun_defaults[gun];
	Save();
}

bool ControlMap::GunsShare(ControlInput input) const
{
	return input != ControlInput::None && std::find(m_gun[0].begin(), m_gun[0].end(), input) != m_gun[0].end() &&
	       std::find(m_gun[1].begin(), m_gun[1].end(), input) != m_gun[1].end();
}

void ControlMap::TapHold::Update(bool pressed, bool* start, bool* select)
{
	const Clock::time_point now = Clock::now();
	constexpr auto long_press = std::chrono::milliseconds(500);
	if (pressed && !down)
		down_since = now;
	if (!pressed && down && now - down_since < long_press)
		start_pulse_until = now + std::chrono::milliseconds(120);
	down = pressed;
	*select = pressed && now - down_since >= long_press;
	*start = now < start_pulse_until;
}

bool ControlMap::UpdateRecenter(const ControllerState& state, bool* now)
{
	const bool recenter = state[ControlInput::LeftStickClick] > PRESSED && state[ControlInput::RightStickClick] > PRESSED;
	*now = recenter && !m_recenter_held;
	m_recenter_held = recenter;
	return recenter;
}

ControlMap::Output ControlMap::Apply(const ControllerState& state)
{
	Output out;
	Emu::PadState& pad = out.pad;
	const auto value = [&](Ps2Control control) { return state[Get(control)]; };
	const auto button = [&](Ps2Control control) { return value(control) > PRESSED; };

	// Both stick clicks: recenter, and neither click reaches the game meanwhile.
	const bool recenter = UpdateRecenter(state, &out.recenter);
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
		m_pad_start_select.Update(pressed(Ps2Control::Start), &pad.start, &pad.select);
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

ControlMap::GunsOutput ControlMap::ApplyGuns(const ControllerState& state, int guns, uint32_t pedal_button)
{
	GunsOutput out;
	const bool recenter = UpdateRecenter(state, &out.recenter);
	for (int gun = 0; gun < std::min(guns, NUM_GUNS); gun++)
	{
		const auto& map = m_gun[gun];
		const auto pressed = [&](GunControl control) {
			const ControlInput input = map[static_cast<size_t>(control)];
			if (input == ControlInput::None || state[input] <= PRESSED)
				return false;
			// Recentering holds both stick clicks: they do nothing else meanwhile.
			if (recenter && (input == ControlInput::LeftStickClick || input == ControlInput::RightStickClick))
				return false;
			// With two guns, an input of the second gun's is not the first's too.
			if (gun == 0 && guns > 1 && std::find(m_gun[1].begin(), m_gun[1].end(), input) != m_gun[1].end())
				return false;
			return true;
		};
		uint32_t& b = out.gun[gun].buttons;
		const auto set = [&](GunControl control, uint32_t bit) {
			if (pressed(control))
				b |= bit;
		};
		set(GunControl::Trigger, Emu::GUN_TRIGGER);
		set(GunControl::A, Emu::GUN_A);
		set(GunControl::B, Emu::GUN_B);
		set(GunControl::C, Emu::GUN_C);
		set(GunControl::Up, Emu::GUN_UP);
		set(GunControl::Down, Emu::GUN_DOWN);
		set(GunControl::Left, Emu::GUN_LEFT);
		set(GunControl::Right, Emu::GUN_RIGHT);
		set(GunControl::Reload, Emu::GUN_SHOOT_OFFSCREEN);
		set(GunControl::Calibrate, Emu::GUN_RECALIBRATE);
		set(GunControl::Pedal, pedal_button);
		bool start, select;
		if (map[static_cast<size_t>(GunControl::Start)] == map[static_cast<size_t>(GunControl::Select)])
			m_gun_start_select[gun].Update(pressed(GunControl::Start), &start, &select);
		else
		{
			start = pressed(GunControl::Start);
			select = pressed(GunControl::Select);
		}
		if (start)
			b |= Emu::GUN_START;
		if (select)
			b |= Emu::GUN_SELECT;
	}
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
