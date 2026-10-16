// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The menus in the headset. Round buttons sit just outside the screen's right edge, from its top
// down: a gear for the SettingsMenu, a gamepad for the GameListMenu, then after a gap the light
// gun's two: light gun mode on or off (a gun, lit while on) and one or two guns (one or two
// people, only while light gun mode is on). At mid height, the 3D mode (volumetric, a lit cube, or
// flat, a square; switched at once with a click) and, below it, passthrough (the room around the
// screen, a lit eye, or the dark void, a crossed-out eye; greyed where SteamVR offers no
// passthrough). At the bottom, level with the screen's bottom edge: pause (lit while the
// user paused the game; menus opened and closed meanwhile leave it paused) and, below it, a red
// power button that quits after a confirmation. One controller is the pointer: the one that pulled its trigger last (the
// other takes over by pulling its own). As soon as it points away from the game, its beam and dot
// show; pointing at a button and pulling the trigger opens its panel in front of the screen and
// pauses the game. A tooltip beside the button the pointer is on says what it does. Its trigger
// clicks, and the thumbsticks scroll the game list and the texture pack list. A click away from the
// panel, its close button, or starting a game close it. While a panel is open, the gear, the game
// list and the power button stay beside it, the open panel's own lit, and a click on another goes
// to its panel: the welcome screen (the game list, when no game runs) and a game alike. The open
// panel's own button goes back to the game; with no game, settings that close go back to the list.
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
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class VrMenu
{
public:
	VrMenu(VrSettingsStore& settings, WebServer& web, ControlMap& controls);
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
	// Another game (or none) started: a new game is not paused.
	void OnGameChanged();

	// Opens the game list (at start, when no game was given).
	void OpenGameList();

	// A short message centred below the screen for `seconds` (e.g. a gun calibrated).
	void ShowNotice(std::string text, float seconds);
	// The screen was placed again: an open panel moves with it.
	void Reanchor() { m_reanchor = true; }

	// Its layers, each always in the same slot (null when hidden): the panel; the sheet, one quad in the
	// screen's plane around it with the buttons, the tooltip, what shows below the screen (the Wi-Fi
	// line, the notice) and the emulator's messages above it; the pointer's beam and its dot. SteamVR
	// shows quad layers as overlays matched by their place in the layer list, so a layer that moved in
	// the list was shown for a frame where another had been.
	static constexpr int NUM_LAYER_SLOTS = 4;
	void GetLayers(const XrCompositionLayerBaseHeader** slots);
	// An invisible quad, to stand in for a hidden layer and keep the others in their place in the
	// list (see main.cpp). False until Create.
	bool GetPadLayer(XrCompositionLayerQuad* quad);

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

	// A round button beside the screen, drawn in its group's layer.
	struct Button
	{
		int group = 0;
		float offset = 0.0f; // meters below its group's first button
		uint32_t icon = 0;
		bool hovered = false;
		bool shown = true; // hidden beside an open panel, but for the game list's gear and power
		bool lit = false; // drawn in the accent colour (light gun mode on)
		bool disabled = false; // dimmed, and clicks do nothing
		bool danger = false; // red (the power button)
		float y = 0.0f; // its centre, meters above the screen's centre
	};
	enum ButtonId
	{
		GEAR,
		GAMES,
		MODE_3D,
		PASSTHROUGH,
		GUN_MODE,
		GUNS,
		PAUSE,
		POWER,
		NUM_BUTTONS
	};
	// Buttons are drawn per group, each a piece of the sheet: the top ones (the gear, the game list and
	// the light gun's), the middle ones (3D and passthrough), and the bottom ones (pause and power).
	struct ButtonGroup
	{
		OverlayLayer::Part part;
		Canvas canvas;
		float height = 0.0f; // meters, from its first button's top to its last's bottom
		float top_y = 0.0f; // its first button's centre, meters above the screen's centre
	};
	static constexpr int NUM_GROUPS = 3;
	// Redraws the group button is in.
	void DrawButton(const Button& button);
	void DrawGroup(int group);
	// The 3D mode button as the setting is (it can change on the settings page too).
	void UpdateModeButton();
	// The passthrough button as the setting is, greyed where the runtime has no passthrough.
	void UpdatePassthroughButton();
	void UpdatePauseButton();
	// What the button does, as its tooltip says it.
	std::string TooltipText(int button) const;
	// Shows the tooltip of button (ButtonId) beside it, or hides it (-1).
	void ShowTooltip(int button, float button_x);

	void Open(Panel panel);
	void Close();
	// Which buttons show beside m_open, none hovered.
	void UpdateShownButtons();
	// x of the buttons' centres, meters right of the screen's centre: outside the screen, and outside
	// the open panel when it is wider.
	float ButtonX() const;
	void AnchorPanel();
	// hand: whose pointer it is; nothing is drawn for a hand with a gun (it draws its own beam).
	void PlacePointer(XrTime time, int hand, const XrPosef& aim, const Hit& hit, const XrQuaternionf& facing);

	VrSettingsStore& m_settings;
	WebServer& m_web;
	ControlMap& m_controls;
	XrApp* m_xr = nullptr;
	bool m_created = false;

	Font m_text;
	Font m_icons;
	std::unique_ptr<SettingsMenu> m_settings_menu;
	std::unique_ptr<GameListMenu> m_game_list;
	OverlayLayer m_panel; // whichever menu is open
	// Every other picture is a part of one canvas, the atlas, uploaded to the GPU, from which the sheet's
	// image is composed: the pieces that show, scaled into their places around the screen, and along its
	// top the beam, the dot and the pad, shown as quads of their own. One layer (SteamVR on the Frame
	// allows 16) and one swapchain (16 too) for all of them. The panel keeps its own: it is most of the
	// pixels, redraws every frame while it scrolls, and only shows while the game is paused.
	OverlayLayer m_sheet;
	OverlayLayer::Part m_sheet_part; // the sheet's quad: its part of the image, pose and size
	std::vector<OverlayLayer::Blit> m_sheet_blits; // what the last image was composed from
	bool m_sheet_shown = false; // a piece shows
	// Composes a new image when a piece changed, showed, hid or moved.
	void ComposeSheet();
	Canvas m_atlas_canvas;
	bool m_atlas_dirty = false;
	// Copies canvas (as large as the part) into the part's place in the atlas, uploaded with the next frame's layers.
	void Put(const OverlayLayer::Part& part, const Canvas& canvas);
	// The atlas's parts. The sheet's pieces have their pose relative to the screen (their centre, meters
	// right of and above its centre); the beam, the dot and the pad, quads of their own, in LOCAL space.
	OverlayLayer::Part m_pad; // transparent
	std::array<Button, NUM_BUTTONS> m_buttons;
	std::array<ButtonGroup, NUM_GROUPS> m_groups;
	std::array<Pointer, 2> m_pointers;
	OverlayLayer::Part m_cursor;
	OverlayLayer::Part m_beam;
	OverlayLayer::Part m_tooltip;
	Canvas m_tooltip_canvas;
	std::string m_tooltip_text; // what m_tooltip shows
	// While Wi-Fi transfer runs, a line below the screen with its address; a click opens its page.
	void UpdateWebLine();
	OverlayLayer::Part m_web_line;
	Canvas m_web_line_canvas;
	std::string m_web_line_text; // what it shows, empty while hidden
	bool m_web_line_hovered = false; // the pointer is on it
	bool m_web_line_drawn_hovered = false; // as drawn
	float m_web_line_width = 0.0f; // the pill's, meters
	float m_web_line_y = 0.0f; // its centre, meters above the screen's centre
	OverlayLayer::Part m_notice; // below the Wi-Fi line when it shows
	Canvas m_notice_canvas;
	std::chrono::steady_clock::time_point m_notice_until{};
	// The emulator's messages (Emu::GetMessages), stacked above the screen: outside the game's
	// picture, where the light gun's sensor does not see them.
	void UpdateMessages();
	OverlayLayer::Part m_messages;
	Canvas m_messages_canvas;
	std::vector<std::string> m_messages_text; // what it shows, empty while hidden

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
	float m_screen_height = 1.8f;
	bool m_pauses_game = true;
	bool m_has_game = false;
	bool m_gun_on = false;
	bool m_user_paused = false; // with the pause button: stays paused when a menu closes
	int m_guns = 1;
};
