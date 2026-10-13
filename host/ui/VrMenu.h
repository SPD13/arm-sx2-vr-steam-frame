// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The menus in the headset. Round buttons sit just outside the screen's right edge, from its top
// down: a gear for the SettingsMenu, a gamepad for the GameListMenu, then the light gun's two: light
// gun mode on or off (a crosshair, lit while on) and one or two guns (one or two people, only while
// light gun mode is on). One controller is the pointer: the one that pulled its trigger last (the
// other takes over by pulling its own). As soon as it points away from the game, its beam and dot
// show; pointing at a button and pulling the trigger opens its panel in front of the screen and
// pauses the game. A tooltip beside the button the pointer is on says what it does. Its trigger
// clicks, and the thumbsticks scroll the game list. A click away from the panel, its close button,
// or starting a game close it.
//
// A hand holding a light gun points with the gun's muzzle instead, and draws its own beam
// (GunRenderer): the menu then only says where the ray meets the menu, if it does.

#pragma once

#include "Settings.h"
#include "XrApp.h"
#include "ui/Canvas.h"
#include "ui/Font.h"
#include "ui/GameListMenu.h"
#include "ui/OverlayLayer.h"
#include "ui/SettingsMenu.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

class VrMenu
{
public:
	// running_stereo / running_flat: the mode this run started in, for the 3D page.
	VrMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls, bool running_stereo, bool running_flat);
	~VrMenu();

	// Loads the fonts and creates the layers. On failure the menus stay off.
	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	// Where each hand points from: its aim pose, or its gun's muzzle.
	struct Pointer
	{
		bool valid = false;
		XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // the ray goes along its -Z
		bool gun = false; // the hand holds a light gun, which draws the beam
	};

	struct Result
	{
		bool open = false; // a menu has the controllers: the game gets no input
		bool block_trigger[2] = {}; // left, right: the menu uses this trigger, keep it from the game
		bool recenter = false; // place the screen in front of the head again
		// Per hand: its ray ends on a menu panel or button (a gun's beam stops there), at ui_point.
		bool on_ui[2] = {};
		XrVector3f ui_point[2] = {};
		// Light gun requests: the mode button or the settings page (SettingsMenu::GunModeRequest),
		// the one/two guns button, and the settings page's Recalibrate / Reload (SettingsMenu::GunAction).
		std::optional<SettingsMenu::GunModeRequest> gun_mode;
		bool toggle_guns = false;
		std::optional<SettingsMenu::GunAction> gun_action;
	};
	// screen: the game screen's centre pose and size in meters.
	Result Update(XrTime time, const XrApp::Input& input, const std::array<Pointer, 2>& pointers, const XrPosef& screen,
		float screen_width, float screen_height);

	// The light gun buttons' state: whether a game runs, whether light gun mode is on for it (and
	// whether the user chose that), and how many guns.
	void SetGunState(bool has_game, bool on, bool chosen, int guns);

	// Opens the game list (at start, when no game was given).
	void OpenGameList();

	// Its layers, each always in the same slot (null when hidden): the panel, the four buttons, the
	// tooltip, the pointer's beam and its dot. SteamVR shows quad layers as overlays matched by their place in the
	// layer list, so a layer that moved in the list was shown for a frame where another had been.
	static constexpr int NUM_LAYER_SLOTS = 8;
	void GetLayers(const XrCompositionLayerBaseHeader** slots);

	// Whether opening a menu pauses the game (debugging: --no-menu-pause).
	void SetPausesGame(bool pauses) { m_pauses_game = pauses; }

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
		bool lit = false; // drawn in the accent colour (light gun mode on)
		bool disabled = false; // dimmed, and clicks do nothing
		float y = 0.0f; // its centre, meters above the screen's centre
	};
	enum ButtonId
	{
		GEAR,
		GAMES,
		GUN_MODE,
		GUNS,
		NUM_BUTTONS
	};
	void DrawButton(Button& button);
	// What the button does, as its tooltip says it.
	std::string TooltipText(int button) const;
	// Shows the tooltip of button (ButtonId) beside it, or hides it (-1).
	void ShowTooltip(int button, float button_x);

	void Open(Panel panel);
	void Close();
	void AnchorPanel();
	// hand: whose pointer it is; nothing is drawn for a hand with a gun (it draws its own beam).
	void PlacePointer(XrTime time, int hand, const XrPosef& aim, const Hit& hit, const XrQuaternionf& facing);

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
	std::array<Button, NUM_BUTTONS> m_buttons;
	std::array<Pointer, 2> m_pointers;
	OverlayLayer m_cursor;
	OverlayLayer m_beam;
	OverlayLayer m_tooltip;
	Canvas m_tooltip_canvas;
	std::string m_tooltip_text; // what m_tooltip shows

	Panel m_open = Panel::None;
	bool m_reanchor = false;
	bool m_show_pointer = false; // the beam
	bool m_show_cursor = false; // the dot at its end, when it ends on something
	bool m_show_tooltip = false;
	int m_hand = 1; // the pointer: 0 left, 1 right
	bool m_trigger_down[2] = {};
	bool m_swallow[2] = {}; // a trigger held through an open or close, ignored until released
	int m_on_button[2] = {-1, -1}; // per hand, the button it points at (ButtonId), -1 for none

	XrPosef m_screen{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	float m_screen_width = 2.4f;
	bool m_pauses_game = true;
	bool m_has_game = false;
	bool m_gun_on = false;
	int m_guns = 1;
};
