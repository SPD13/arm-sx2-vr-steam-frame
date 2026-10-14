// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// OpenXR side of the host: instance, session, frame loop and controller input.
//
// Vulkan ownership is split with the core. XR_KHR_vulkan_enable (v1) lets us create the VkInstance
// with the runtime's extensions and ask it for the VkPhysicalDevice; GSDeviceVK adopts both through
// VKLibretro::Init and creates the VkDevice itself, merging in the runtime's device extensions. The
// session is then bound to that device and its graphics queue, which the GS thread, the runtime and
// our frame copies share under GraphicsQueueMutex().

#pragma once

#include "Controls.h"

#include "GS/Renderers/Vulkan/VKLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <mutex>
#include <string>
#include <utility>
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
	bool IsFocused() const { return m_session_state == XR_SESSION_STATE_FOCUSED; }
	// When the runtime moves the LOCAL space (a system recenter), the time the move takes effect;
	// 0 when it did not move since the last call.
	XrTime TakeLocalSpaceChange() { return std::exchange(m_local_space_change, 0); }

	struct Frame
	{
		XrTime display_time = 0;
		bool should_render = false;
	};
	Frame BeginFrame();
	// passthrough: the room shows through where the layers are transparent (SupportsPassthrough).
	void EndFrame(const Frame& frame, const XrCompositionLayerBaseHeader* const* layers, uint32_t layer_count,
		bool passthrough = false);

	// Passthrough: SteamVR offers the room seen through the headset's cameras as the alpha-blend
	// environment mode, and shows it only through a projection layer. BackLayer is that layer, fully
	// transparent: `clear` (a transparent image) at both eyes' views, the last ones known when the eyes
	// are not located this frame; null before they were ever located. It goes first in the list.
	bool SupportsPassthrough() const { return m_alpha_blend; }
	const XrCompositionLayerBaseHeader* BackLayer(XrTime time, const XrSwapchainSubImage& clear);

	struct Input
	{
		ControllerState controller; // as the controllers are; ControlMap makes it a pad
		float trigger[2] = {}; // left, right: the menu's pointer clicks with them
	};
	Input ReadInput();

	// Head pose in the LOCAL space at the given time, if valid. *tracked tells whether the runtime
	// tracks it now: a valid pose can be the last one it knew.
	bool LocateHead(XrTime time, XrPosef* pose, bool* tracked = nullptr) const;
	// Both eyes' views in the LOCAL space at the given time, if tracked.
	bool LocateEyes(XrTime time, XrView views[2]) const;
	// A controller's aim pose (0 left, 1 right) in the LOCAL space: its ray points along -Z.
	bool LocateAim(int hand, XrTime time, XrPosef* pose) const;
	// A controller's grip pose (0 left, 1 right) in the LOCAL space: where the hand holds it.
	bool LocateGrip(int hand, XrTime time, XrPosef* pose) const;
	// A short vibration of a controller (0 left, 1 right); amplitude 0..1.
	void Vibrate(int hand, float seconds, float amplitude);
	// Whether the controllers are Steam Frame ones (otherwise the Touch layout fallback).
	bool HasFrameControllers() const { return m_profile_has_dpad; }

	XrInstance Instance() const { return m_instance; }
	XrSystemId System() const { return m_system; }
	XrSession Session() const { return m_session; }
	XrSpace LocalSpace() const { return m_local_space; }

private:
	bool CreateActions();
	bool SuggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings);
	bool LocatePose(XrSpace space, XrTime time, XrPosef* pose) const;
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
	XrTime m_local_space_change = 0;
	bool m_exit_requested = false;
	bool m_has_frame_controller_ext = false;
	bool m_profile_has_dpad = false;
	bool m_alpha_blend = false; // the runtime offers XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
	bool m_back_views_valid = false;
	std::array<XrCompositionLayerProjectionView, 2> m_back_views{};
	XrCompositionLayerProjection m_back_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

	VkInstance m_vk_instance = VK_NULL_HANDLE;
	std::string m_vk_device_extension_storage;
	std::vector<const char*> m_vk_device_extensions;

	XrActionSet m_action_set = XR_NULL_HANDLE;
	XrPath m_hand_paths[2] = {};
	std::array<XrAction, static_cast<size_t>(ControlInput::Count)> m_inputs{}; // one per controller input
	XrAction m_aim = XR_NULL_HANDLE; // per hand
	XrSpace m_aim_spaces[2] = {};
	XrAction m_grip = XR_NULL_HANDLE; // per hand
	XrSpace m_grip_spaces[2] = {};
	XrAction m_haptic = XR_NULL_HANDLE; // per hand

};
