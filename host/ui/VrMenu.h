// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// The settings menu in the headset. A gear button sits in the screen's top right corner; pointing
// a controller at it and pulling the trigger opens the SettingsMenu panel in front of the screen
// and pauses the game. The controller that clicked last is the pointer, drawn as a beam and a dot;
// its trigger clicks. A click away from the panel, its close button, or Load / Restart close it.

#pragma once

#include "Settings.h"
#include "XrApp.h"
#include "ui/Canvas.h"
#include "ui/Font.h"
#include "ui/OverlayLayer.h"
#include "ui/SettingsMenu.h"

#include <memory>

class VrMenu
{
public:
	// running_stereo / running_flat: the mode this run started in, for the 3D page.
	VrMenu(VrSettingsStore& settings, WebServer& web, bool running_stereo, bool running_flat);
	~VrMenu();

	// Loads the fonts and creates the layers. On failure the menu stays off.
	bool Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index);
	void Destroy();

	struct Result
	{
		bool open = false; // the menu has the controllers: the game gets no input
		bool block_trigger[2] = {}; // left, right: the menu uses this trigger, keep it from the game
		bool recenter = false; // place the screen in front of the head again
	};
	// screen: the game screen's centre pose and size in meters.
	Result Update(XrTime time, const XrApp::Input& input, const XrPosef& screen, float screen_width, float screen_height);

	// Appends the visible layers (at most 3).
	void AddLayers(const XrCompositionLayerBaseHeader** layers, uint32_t* count);

private:
	struct Hit
	{
		bool hit = false;
		float u = 0.0f, v = 0.0f; // on the quad, (0, 0) top left
		float x = 0.0f, y = 0.0f; // meters from the quad's centre, +y up
		XrVector3f point{};
		XrVector3f normal{};
	};
	static Hit HitQuad(const XrPosef& quad, float width, float height, const XrPosef& aim);

	void Open();
	void Close();
	void AnchorPanel();
	void DrawGear();
	void PlacePointer(XrTime time, const XrPosef& aim, const Hit& hit);

	VrSettingsStore& m_settings;
	WebServer& m_web;
	bool m_running_stereo;
	bool m_running_flat;
	XrApp* m_xr = nullptr;
	bool m_created = false;

	Font m_text;
	Font m_icons;
	std::unique_ptr<SettingsMenu> m_menu;
	OverlayLayer m_panel;
	OverlayLayer m_gear;
	OverlayLayer m_cursor;
	OverlayLayer m_beam;
	Canvas m_gear_canvas;

	bool m_open = false;
	bool m_gear_hovered = false;
	bool m_reanchor = false;
	bool m_show_pointer = false;
	int m_hand = 1; // the pointer: 0 left, 1 right
	bool m_trigger_down[2] = {};
	bool m_swallow[2] = {}; // a trigger held through an open or close, ignored until released

	XrPosef m_screen{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
	float m_screen_width = 2.4f;
};
