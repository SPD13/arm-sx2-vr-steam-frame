// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The menus in the headset. Two round buttons sit just outside the screen's right edge, level with
// its top: a gear for the SettingsMenu and, below it, a gamepad for the GameListMenu. One controller
// is the pointer: the one that pulled its trigger last (the other takes over by pulling its own).
// As soon as it points away from the game, its beam and dot show; pointing at a button and pulling
// the trigger opens its panel in front of the screen and pauses the game. Its trigger clicks, and
// the thumbsticks scroll the game list. A click away from the panel, its close button, or starting
// a game close it.

#pragma once

#include "Settings.h"
#include "XrApp.h"
#include "ui/Canvas.h"
#include "ui/Font.h"
#include "ui/GameListMenu.h"
#include "ui/OverlayLayer.h"
#include "ui/SettingsMenu.h"

#include <memory>

class VrMenu
{
public:
	// running_stereo / running_flat: the mode this run started in, for the 3D page.
	VrMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls, bool running_stereo, bool running_flat);
	~VrMenu();

	// Loads the fonts and creates the layers. On failure the menus stay off.
	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	struct Result
	{
		bool open = false; // a menu has the controllers: the game gets no input
		bool block_trigger[2] = {}; // left, right: the menu uses this trigger, keep it from the game
		bool recenter = false; // place the screen in front of the head again
	};
	// screen: the game screen's centre pose and size in meters.
	Result Update(XrTime time, const XrApp::Input& input, const XrPosef& screen, float screen_width, float screen_height);

	// Opens the game list (at start, when no game was given).
	void OpenGameList();

	// Appends the visible layers (at most 4).
	void AddLayers(const XrCompositionLayerBaseHeader** layers, uint32_t* count);

private:
	enum class Panel
	{
		None,
		Settings,
		Games,
	};

	struct Hit
	{
		bool hit = false;
		float x = 0.0f, y = 0.0f; // meters from the quad's centre, +y up
		XrVector3f point{};
		XrVector3f normal{};
	};
	// Where aim's ray meets quad's plane, from the front and within reach.
	static Hit HitPlane(const XrPosef& quad, const XrPosef& aim);

	// A round button beside the screen.
	struct Button
	{
		OverlayLayer layer;
		Canvas canvas;
		uint32_t icon = 0;
		bool hovered = false;
	};
	void DrawButton(Button& button);

	void Open(Panel panel);
	void Close();
	void AnchorPanel();
	void PlacePointer(XrTime time, const XrPosef& aim, const Hit& hit, const XrQuaternionf& facing);

	VrSettingsStore& m_settings;
	WebServer& m_web;
	ControlMap& m_controls;
	bool m_running_stereo;
	bool m_running_flat;
	XrApp* m_xr = nullptr;
	bool m_created = false;

	Font m_text;
	Font m_icons;
	std::unique_ptr<SettingsMenu> m_settings_menu;
	std::unique_ptr<GameListMenu> m_game_list;
	OverlayLayer m_panel; // whichever menu is open
	Button m_gear;
	Button m_games;
	OverlayLayer m_cursor;
	OverlayLayer m_beam;

	Panel m_open = Panel::None;
	bool m_reanchor = false;
	bool m_show_pointer = false; // the beam
	bool m_show_cursor = false; // the dot at its end, when it ends on something
	int m_hand = 1; // the pointer: 0 left, 1 right
	bool m_trigger_down[2] = {};
	bool m_swallow[2] = {}; // a trigger held through an open or close, ignored until released

	XrPosef m_screen{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	float m_screen_width = 2.4f;
};
