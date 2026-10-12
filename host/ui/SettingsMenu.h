// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// The settings page the gear button opens: a navigation bar of categories on the left (Display,
// 3D, Graphics, Audio, Game, Wi-Fi Transfer, Controls, and Exit last) and the selected category's settings on the
// right. It draws on a Canvas and knows nothing of OpenXR; VrMenu shows it in the headset and
// feeds it the pointer.

#pragma once

#include "Emu.h"
#include "Settings.h"
#include "web/WebServer.h"
#include "ui/Canvas.h"
#include "ui/Gui.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

class SettingsMenu
{
public:
	static constexpr int WIDTH = 1200;
	static constexpr int HEIGHT = 800;

	// running_stereo / running_flat: the 3D mode this run started in (changing it needs a restart).
	// web: the Wi-Fi transfer server the menu switches on and off.
	SettingsMenu(VrSettingsStore& settings, WebServer& web, bool running_stereo, bool running_flat, Font& text, Font& icons);

	// Reads the emulator's settings and save slots again, for a menu that is being opened.
	void Refresh();
	// Which controller layout the Controls page shows.
	void SetFrameControllers(bool frame) { m_frame_controllers = frame; }

	// One frame of input; redraws the canvas when something visible changed and returns whether it did.
	bool Update(const GuiInput& input);
	const Canvas& GetCanvas() const { return m_canvas; }

	// Requests for VrMenu, each reported once.
	bool TakeCloseRequest() { return std::exchange(m_close_requested, false); }
	bool TakeRecenterRequest() { return std::exchange(m_recenter_requested, false); }

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
	void RunDialog();

	// Starts a settings row: label and help on the left; returns the control's rect on the right.
	// with_note leaves room under the control for a Note.
	Rect Row(std::string_view label, std::string_view help, float control_height = 52.0f, bool with_note = false);
	void Section(std::string_view title);
	// A line of text under the last row's control (a row made with_note).
	void Note(std::string_view text, Color color);

	void SetVr(const VrSettings& settings);
	void SetCore(const Emu::CoreSettings& settings);

	VrSettingsStore& m_settings;
	WebServer& m_web;
	WebServer::Status m_web_status;
	bool m_running_stereo;
	bool m_running_flat;
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
	std::string m_toast;
	Clock::time_point m_toast_until{};

	// Layout cursor for the rows of the current page.
	float m_y = 0.0f;
	Rect m_last_control;
};
