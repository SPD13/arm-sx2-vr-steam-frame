// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The settings page the gear button opens: a navigation bar of categories on the left (Display,
// 3D, Graphics, Audio, Game, Wi-Fi Transfer, Controls, Light Gun, and Exit last) and the selected
// category's settings on the right. It draws on a Canvas and knows nothing of OpenXR; VrMenu shows it in the headset and
// feeds it the pointer.

#pragma once

#include "Controls.h"
#include "Emu.h"
#include "Settings.h"
#include "web/WebServer.h"
#include "ui/Canvas.h"
#include "ui/Gui.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SettingsMenu
{
public:
	static constexpr int WIDTH = 1200;
	static constexpr int HEIGHT = 800;

	// web: the Wi-Fi transfer server the menu switches on and off; controls: the controller mapping.
	SettingsMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls, Font& text, Font& icons);

	// Reads the emulator's settings and save slots again, for a menu that is being opened.
	void Refresh();
	// Which controller layout the Controls page shows.
	void SetFrameControllers(bool frame) { m_frame_controllers = frame; }
	// The controllers as they are this frame, for mapping a control (call before Update).
	void SetControllerState(const ControllerState& state) { m_controller = state; }
	// While mapping a control, the triggers are inputs to map, not clicks: no click, no closing.
	bool IsMappingControl() const { return m_mapping.has_value(); }
	// Light gun mode for the running game, for the Light Gun page.
	void SetGunMode(bool has_game, bool on, bool chosen);

	enum class GunModeRequest
	{
		Off,
		On,
		Auto, // the automatic choice again
	};
	enum class GunAction
	{
		Recalibrate, // Time Crisis's calibration shot
		Reload, // a shot off the screen
	};

	// One frame of input; redraws the canvas when something visible changed and returns whether it did.
	bool Update(const GuiInput& input);
	const Canvas& GetCanvas() const { return m_canvas; }

	// Requests for VrMenu, each reported once.
	bool TakeCloseRequest() { return std::exchange(m_close_requested, false); }
	bool TakeRecenterRequest() { return std::exchange(m_recenter_requested, false); }
	// Whether a control was just mapped (the input that did it may still be held).
	bool TakeMappingDone() { return std::exchange(m_mapping_done, false); }
	std::optional<GunModeRequest> TakeGunModeRequest() { return std::exchange(m_gun_mode_request, std::nullopt); }
	// Asked from the menu, done once it has closed (the game runs again).
	std::optional<GunAction> TakeGunAction() { return std::exchange(m_gun_action, std::nullopt); }

private:
	enum class Page
	{
		Display,
		Stereo,
		Graphics,
		Audio,
		Game,
		Transfer,
		Controls,
		LightGun,
	};
	// What the Controls page maps: a DualShock 2 control, or a control of gun 0 or 1.
	struct MapTarget
	{
		int gun = -1; // -1: the DualShock 2
		int control = 0; // Ps2Control or GunControl
	};
	enum class Dialog
	{
		None,
		Exit,
		Restart,
	};

	// The whole UI, run once per pass (see Gui).
	void Run();
	void RunNav();
	void RunDisplay();
	void RunStereo();
	void RunGraphics();
	void RunAudio();
	void RunGame();
	void RunTransfer();
	void RunControls();
	void RunLightGun();
	void RunMappingDialog();
	// What a control is called, and what its input is called, on the Controls page.
	std::string TargetName(const MapTarget& target) const;
	std::string InputLabel(const MapTarget& target) const;
	void UpdateMapping();
	void RunDialog();

	// Starts a settings row: label and help on the left; returns the control's rect on the right.
	// with_note leaves room under the control for a Note.
	Rect Row(std::string_view label, std::string_view help, float control_height = 52.0f, bool with_note = false);
	float m_row_gap = 22.0f; // between rows; a page with many rows makes it smaller
	void Section(std::string_view title);
	// A line of text under the last row's control (a row made with_note).
	void Note(std::string_view text, Color color);

	void SetVr(const VrSettings& settings);
	void SetCore(const Emu::CoreSettings& settings);

	VrSettingsStore& m_settings;
	WebServer& m_web;
	ControlMap& m_controls;
	WebServer::Status m_web_status;
	Canvas m_canvas;
	Gui m_gui;

	Page m_page = Page::Display;
	Dialog m_dialog = Dialog::None;
	bool m_frame_controllers = true;
	bool m_needs_draw = true;
	bool m_close_requested = false;
	bool m_recenter_requested = false;

	Emu::CoreSettings m_core;
	std::vector<Emu::SaveSlot> m_slots;
	int m_slot = 1;
	unsigned m_settings_version = 0;

	using Clock = std::chrono::steady_clock;
	// Mapping a control: which, since when the controllers are all let go (armed), until when.
	std::optional<MapTarget> m_mapping;
	bool m_mapping_armed = false;
	bool m_mapping_done = false;
	Clock::time_point m_mapping_until{};
	ControllerState m_controller, m_controller_before;

	// The Controls page's tab: the DualShock 2's controls, or a gun's (which one).
	bool m_controls_guncon = false;
	int m_controls_gun = 0;

	// Light gun mode for the running game, and what the Light Gun page asked for.
	bool m_has_game = false;
	bool m_gun_on = false;
	bool m_gun_chosen = false;
	std::optional<GunModeRequest> m_gun_mode_request;
	std::optional<GunAction> m_gun_action;

	std::string m_toast;
	Clock::time_point m_toast_until{};

	// Layout cursor for the rows of the current page.
	float m_y = 0.0f;
	Rect m_last_control;
};
