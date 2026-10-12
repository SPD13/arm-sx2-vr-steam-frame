// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// OpenXR side of the host: instance, session, frame loop and controller input.
//
// Vulkan ownership is split with the core. XR_KHR_vulkan_enable (v1) lets us create the VkInstance
// with the runtime's extensions and ask it for the VkPhysicalDevice; GSDeviceVK adopts both through
// VKLibretro::Init and creates the VkDevice itself, merging in the runtime's device extensions. The
// session is then bound to that device and its graphics queue, which the GS thread, the runtime and
// our frame copies share under GraphicsQueueMutex().

#pragma once

#include "Emu.h"

#include "GS/Renderers/Vulkan/VKLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

// Serialises every use of the shared graphics queue: GS submits (via VKLibretro's lock_queue),
// our own submits, and the OpenXR calls that may touch the queue. Recursive because the core's
// wrapped vkQueueSubmit takes it too, also when we submit while holding it.
std::recursive_mutex& GraphicsQueueMutex();

class XrApp
{
public:
	~XrApp();

	// Creates the XrInstance, finds the headset and creates the VkInstance it needs. On success
	// VKLibretro::Init holds the instance, GPU and required device extensions.
	bool CreateInstance();

	// Binds a session to the device GSDeviceVK created.
	bool CreateSession(VkDevice device, uint32_t queue_family_index);

	void DestroySession();

	// Destroys the shared VkDevice (GSDeviceVK leaves a frontend-provided context to its owner),
	// our VkInstance and the XrInstance. Call after the GS has shut down.
	void DestroyInstance(VkDevice gs_device);

	// Handles runtime events. Returns false once the app should exit.
	bool PollEvents();
	bool IsSessionRunning() const { return m_session_running; }
	// Whether the session became FOCUSED since the last call: the headset was put on, or the
	// system dashboard closed.
	bool TakeFocusGained()
	{
		const bool gained = m_focus_gained;
		m_focus_gained = false;
		return gained;
	}

	struct Frame
	{
		XrTime display_time = 0;
		bool should_render = false;
	};
	Frame BeginFrame();
	void EndFrame(const Frame& frame, const XrCompositionLayerBaseHeader* const* layers, uint32_t layer_count);

	struct Input
	{
		Emu::PadState pad;
		bool recenter = false; // both sticks clicked: place the screen in front of the head again
		float trigger[2] = {}; // left, right: the menu's pointer clicks with them
	};
	Input ReadInput();

	// Head pose in the LOCAL space at the given time, if tracked.
	bool LocateHead(XrTime time, XrPosef* pose) const;
	// Both eyes' views in the LOCAL space at the given time, if tracked.
	bool LocateEyes(XrTime time, XrView views[2]) const;
	// A controller's aim pose (0 left, 1 right) in the LOCAL space: its ray points along -Z.
	bool LocateAim(int hand, XrTime time, XrPosef* pose) const;
	// Whether the controllers are Steam Frame ones (otherwise the Touch layout fallback).
	bool HasFrameControllers() const { return m_profile_has_dpad; }

	XrInstance Instance() const { return m_instance; }
	XrSystemId System() const { return m_system; }
	XrSession Session() const { return m_session; }
	XrSpace LocalSpace() const { return m_local_space; }

private:
	bool CreateActions();
	bool SuggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings);
	void UpdateInteractionProfile();
	bool GetBool(XrAction action) const;
	float GetFloat(XrAction action) const;
	XrVector2f GetVector2(XrAction action) const;

	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_system = XR_NULL_SYSTEM_ID;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_local_space = XR_NULL_HANDLE;
	XrSpace m_view_space = XR_NULL_HANDLE;
	XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
	bool m_session_running = false;
	bool m_focus_gained = false;
	bool m_exit_requested = false;
	bool m_has_frame_controller_ext = false;
	bool m_profile_has_dpad = false;

	VkInstance m_vk_instance = VK_NULL_HANDLE;
	std::string m_vk_device_extension_storage;
	std::vector<const char*> m_vk_device_extensions;

	XrActionSet m_action_set = XR_NULL_HANDLE;
	XrPath m_hand_paths[2] = {};
	XrAction m_cross = XR_NULL_HANDLE, m_circle = XR_NULL_HANDLE, m_square = XR_NULL_HANDLE,
	         m_triangle = XR_NULL_HANDLE;
	XrAction m_dpad_up = XR_NULL_HANDLE, m_dpad_down = XR_NULL_HANDLE, m_dpad_left = XR_NULL_HANDLE,
	         m_dpad_right = XR_NULL_HANDLE;
	XrAction m_menu = XR_NULL_HANDLE; // short press = Start, long press = Select
	XrAction m_l1 = XR_NULL_HANDLE, m_r1 = XR_NULL_HANDLE, m_l2 = XR_NULL_HANDLE, m_r2 = XR_NULL_HANDLE;
	XrAction m_l3 = XR_NULL_HANDLE, m_r3 = XR_NULL_HANDLE;
	XrAction m_left_stick = XR_NULL_HANDLE, m_right_stick = XR_NULL_HANDLE;
	XrAction m_aim = XR_NULL_HANDLE; // per hand
	XrSpace m_aim_spaces[2] = {};

	using Clock = std::chrono::steady_clock;
	bool m_menu_down = false;
	Clock::time_point m_menu_down_since{};
	Clock::time_point m_start_pulse_until{};
	bool m_recenter_held = false;
};
