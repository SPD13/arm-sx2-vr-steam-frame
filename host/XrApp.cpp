// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#include "XrApp.h"

#include "GS/Renderers/Vulkan/VKLibretro.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include <cstdlib>
#include <cstring>
#include <thread>

std::recursive_mutex& GraphicsQueueMutex()
{
	static std::recursive_mutex mutex;
	return mutex;
}

#define XR_CHECK(call)                                                                  \
	do                                                                                  \
	{                                                                                   \
		const XrResult xr_result_ = (call);                                             \
		if (XR_FAILED(xr_result_))                                                      \
		{                                                                               \
			Console.ErrorFmt("XR: {} failed: {}", #call, static_cast<int>(xr_result_)); \
			return false;                                                               \
		}                                                                               \
	} while (0)

template <typename T>
static T GetXrFunction(XrInstance instance, const char* name)
{
	PFN_xrVoidFunction fn = nullptr;
	xrGetInstanceProcAddr(instance, name, &fn);
	return reinterpret_cast<T>(fn);
}

// Splits the runtime's space-separated extension list in place.
static std::vector<const char*> SplitExtensionList(std::string& storage)
{
	std::vector<const char*> names;
	for (size_t pos = 0; pos < storage.size();)
	{
		while (pos < storage.size() && storage[pos] == ' ')
			storage[pos++] = '\0';
		if (pos < storage.size())
			names.push_back(&storage[pos]);
		while (pos < storage.size() && storage[pos] != ' ')
			pos++;
	}
	return names;
}

XrApp::~XrApp()
{
	DestroySession();
	DestroyInstance(VK_NULL_HANDLE);
}

bool XrApp::CreateInstance()
{
	uint32_t ext_count = 0;
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr));
	std::vector<XrExtensionProperties> available(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, available.data()));
	const auto offered = [&available](const char* name) {
		for (const XrExtensionProperties& ext : available)
		{
			if (!std::strcmp(ext.extensionName, name))
				return true;
		}
		return false;
	};

	if (!offered(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
	{
		Console.Error("XR: the runtime does not offer XR_KHR_vulkan_enable.");
		return false;
	}
	std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
	m_has_frame_controller_ext = offered("XR_VALVE_frame_controller_interaction");
	if (m_has_frame_controller_ext)
		extensions.push_back("XR_VALVE_frame_controller_interaction");

	XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(create_info.applicationInfo.applicationName, "PS2 Volumetric", XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "ARMSX2", XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	create_info.enabledExtensionNames = extensions.data();
	XR_CHECK(xrCreateInstance(&create_info, &m_instance));

	XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
		Console.WriteLnFmt("XR: runtime {} {}.{}.{}", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
			XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

	// The headset may still be waking up; give it a while.
	XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	for (int attempt = 0;; attempt++)
	{
		const XrResult res = xrGetSystem(m_instance, &system_info, &m_system);
		if (XR_SUCCEEDED(res))
			break;
		if (res != XR_ERROR_FORM_FACTOR_UNAVAILABLE || attempt >= 60)
		{
			Console.ErrorFmt("XR: no headset available ({}).", static_cast<int>(res));
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	auto pfnGetRequirements =
		GetXrFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(m_instance, "xrGetVulkanGraphicsRequirementsKHR");
	auto pfnGetInstanceExtensions =
		GetXrFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(m_instance, "xrGetVulkanInstanceExtensionsKHR");
	auto pfnGetDeviceExtensions =
		GetXrFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(m_instance, "xrGetVulkanDeviceExtensionsKHR");
	auto pfnGetGraphicsDevice =
		GetXrFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(m_instance, "xrGetVulkanGraphicsDeviceKHR");
	if (!pfnGetRequirements || !pfnGetInstanceExtensions || !pfnGetDeviceExtensions || !pfnGetGraphicsDevice)
	{
		Console.Error("XR: XR_KHR_vulkan_enable entry points are missing.");
		return false;
	}

	// GSDeviceVK targets Vulkan 1.1, like its own instance.
	XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	XR_CHECK(pfnGetRequirements(m_instance, m_system, &requirements));
	const XrVersion vk_version = XR_MAKE_VERSION(1, 1, 0);
	if (vk_version < requirements.minApiVersionSupported)
	{
		Console.ErrorFmt("XR: runtime needs Vulkan {}.{}, the renderer targets 1.1.",
			XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported));
		return false;
	}

	uint32_t size = 0;
	XR_CHECK(pfnGetInstanceExtensions(m_instance, m_system, 0, &size, nullptr));
	std::string instance_ext_storage(size, '\0');
	XR_CHECK(pfnGetInstanceExtensions(m_instance, m_system, size, &size, instance_ext_storage.data()));
	instance_ext_storage.resize(std::strlen(instance_ext_storage.c_str()));
	const std::vector<const char*> instance_exts = SplitExtensionList(instance_ext_storage);

	VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app_info.pApplicationName = "PS2 Volumetric";
	app_info.pEngineName = "ARMSX2";
	app_info.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vk_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vk_info.pApplicationInfo = &app_info;
	vk_info.enabledExtensionCount = static_cast<uint32_t>(instance_exts.size());
	vk_info.ppEnabledExtensionNames = instance_exts.data();
	if (const VkResult res = vkCreateInstance(&vk_info, nullptr, &m_vk_instance); res != VK_SUCCESS)
	{
		Console.ErrorFmt("XR: vkCreateInstance failed: {}", static_cast<int>(res));
		return false;
	}
	for (const char* ext : instance_exts)
		Console.WriteLnFmt("XR: Vulkan instance extension {}", ext);

	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	XR_CHECK(pfnGetGraphicsDevice(m_instance, m_system, m_vk_instance, &gpu));

	XR_CHECK(pfnGetDeviceExtensions(m_instance, m_system, 0, &size, nullptr));
	m_vk_device_extension_storage.assign(size, '\0');
	XR_CHECK(pfnGetDeviceExtensions(m_instance, m_system, size, &size, m_vk_device_extension_storage.data()));
	m_vk_device_extension_storage.resize(std::strlen(m_vk_device_extension_storage.c_str()));
	m_vk_device_extensions = SplitExtensionList(m_vk_device_extension_storage);
	for (const char* ext : m_vk_device_extensions)
		Console.WriteLnFmt("XR: Vulkan device extension {}", ext);

	VKLibretro::Init.instance = m_vk_instance;
	VKLibretro::Init.gpu = gpu;
	VKLibretro::Init.get_instance_proc_addr = vkGetInstanceProcAddr;
	VKLibretro::Init.required_device_extensions = m_vk_device_extensions.data();
	VKLibretro::Init.num_required_device_extensions = static_cast<unsigned>(m_vk_device_extensions.size());
	return true;
}

bool XrApp::CreateSession(VkDevice device, uint32_t queue_family_index)
{
	XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance = m_vk_instance;
	binding.physicalDevice = VKLibretro::Init.gpu;
	binding.device = device;
	binding.queueFamilyIndex = queue_family_index;
	binding.queueIndex = 0; // GSDeviceVK uses queue 0 of its graphics family

	XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
	info.next = &binding;
	info.systemId = m_system;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		XR_CHECK(xrCreateSession(m_instance, &info, &m_session));
	}

	XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_local_space));
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_view_space));

	return CreateActions();
}

void XrApp::SuggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings)
{
	std::vector<XrActionSuggestedBinding> suggested;
	for (const auto& [action, path] : bindings)
	{
		XrPath xr_path = XR_NULL_PATH;
		if (XR_SUCCEEDED(xrStringToPath(m_instance, path, &xr_path)))
			suggested.push_back({action, xr_path});
	}
	XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	xrStringToPath(m_instance, profile, &info.interactionProfile);
	info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
	info.suggestedBindings = suggested.data();
	if (const XrResult res = xrSuggestInteractionProfileBindings(m_instance, &info); XR_FAILED(res))
		Console.WarningFmt("XR: bindings for {} rejected ({})", profile, static_cast<int>(res));
}

bool XrApp::CreateActions()
{
	XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(set_info.actionSetName, "gamepad");
	std::strcpy(set_info.localizedActionSetName, "DualShock 2");
	XR_CHECK(xrCreateActionSet(m_instance, &set_info, &m_action_set));

	xrStringToPath(m_instance, "/user/hand/left", &m_hand_paths[0]);
	xrStringToPath(m_instance, "/user/hand/right", &m_hand_paths[1]);

	const auto make = [this](XrAction* action, const char* name, const char* label, XrActionType type) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(info.actionName, name);
		std::strcpy(info.localizedActionName, label);
		info.actionType = type;
		return XR_SUCCEEDED(xrCreateAction(m_action_set, &info, action));
	};
	constexpr XrActionType B = XR_ACTION_TYPE_BOOLEAN_INPUT, F = XR_ACTION_TYPE_FLOAT_INPUT,
	                       V = XR_ACTION_TYPE_VECTOR2F_INPUT;
	const bool made = make(&m_cross, "cross", "Cross", B) && make(&m_circle, "circle", "Circle", B) &&
	                  make(&m_square, "square", "Square", B) && make(&m_triangle, "triangle", "Triangle", B) &&
	                  make(&m_dpad_up, "dpad_up", "D-pad up", B) && make(&m_dpad_down, "dpad_down", "D-pad down", B) &&
	                  make(&m_dpad_left, "dpad_left", "D-pad left", B) &&
	                  make(&m_dpad_right, "dpad_right", "D-pad right", B) &&
	                  make(&m_menu, "start_select", "Start (hold: Select)", B) && make(&m_l1, "l1", "L1", F) &&
	                  make(&m_r1, "r1", "R1", F) && make(&m_l2, "l2", "L2", F) && make(&m_r2, "r2", "R2", F) &&
	                  make(&m_l3, "l3", "L3", B) && make(&m_r3, "r3", "R3", B) &&
	                  make(&m_left_stick, "left_stick", "Left stick", V) &&
	                  make(&m_right_stick, "right_stick", "Right stick", V);
	if (!made)
	{
		Console.Error("XR: could not create the controller actions.");
		return false;
	}

	// Shared by both profiles.
	std::vector<std::pair<XrAction, const char*>> common = {
		{m_cross, "/user/hand/right/input/a/click"},
		{m_l1, "/user/hand/left/input/squeeze/value"},
		{m_r1, "/user/hand/right/input/squeeze/value"},
		{m_l2, "/user/hand/left/input/trigger/value"},
		{m_r2, "/user/hand/right/input/trigger/value"},
		{m_l3, "/user/hand/left/input/thumbstick/click"},
		{m_r3, "/user/hand/right/input/thumbstick/click"},
		{m_left_stick, "/user/hand/left/input/thumbstick"},
		{m_right_stick, "/user/hand/right/input/thumbstick"},
	};

	// Steam Frame native profile: all four face buttons on the right controller, a D-pad on the
	// left one (paths as used by AstroQuest-Frame). SteamVR's Touch emulation maps B, X and Y to
	// the same button, so the native profile matters.
	if (m_has_frame_controller_ext)
	{
		auto frame = common;
		frame.insert(frame.end(), {
			{m_circle, "/user/hand/right/input/b/click"},
			{m_square, "/user/hand/right/input/x/click"},
			{m_triangle, "/user/hand/right/input/y/click"},
			{m_dpad_up, "/user/hand/left/input/dpad_up/click"},
			{m_dpad_down, "/user/hand/left/input/dpad_down/click"},
			{m_dpad_left, "/user/hand/left/input/dpad_left/click"},
			{m_dpad_right, "/user/hand/left/input/dpad_right/click"},
			{m_menu, "/user/hand/left/input/view/click"},
		});
		SuggestBindings("/interaction_profiles/valve/frame_controller_valve", frame);
	}

	// Touch layout fallback: no D-pad, so it comes from the left stick (see ReadInput).
	auto touch = common;
	touch.insert(touch.end(), {
		{m_circle, "/user/hand/right/input/b/click"},
		{m_square, "/user/hand/left/input/x/click"},
		{m_triangle, "/user/hand/left/input/y/click"},
		{m_menu, "/user/hand/left/input/menu/click"},
	});
	SuggestBindings("/interaction_profiles/oculus/touch_controller", touch);

	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &m_action_set;
	XR_CHECK(xrAttachSessionActionSets(m_session, &attach));
	return true;
}

void XrApp::UpdateInteractionProfile()
{
	XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
	if (XR_FAILED(xrGetCurrentInteractionProfile(m_session, m_hand_paths[0], &state)) ||
		state.interactionProfile == XR_NULL_PATH)
		return;
	char name[XR_MAX_PATH_LENGTH] = {};
	uint32_t len = 0;
	xrPathToString(m_instance, state.interactionProfile, sizeof(name), &len, name);
	m_profile_has_dpad = std::strstr(name, "frame_controller") != nullptr;
	Console.WriteLnFmt("XR: controllers use {}", name);
}

void XrApp::DestroySession()
{
	if (m_session == XR_NULL_HANDLE)
		return;
	std::lock_guard lock(GraphicsQueueMutex());
	if (m_action_set)
		xrDestroyActionSet(m_action_set);
	if (m_view_space)
		xrDestroySpace(m_view_space);
	if (m_local_space)
		xrDestroySpace(m_local_space);
	xrDestroySession(m_session);
	m_action_set = XR_NULL_HANDLE;
	m_view_space = m_local_space = XR_NULL_HANDLE;
	m_session = XR_NULL_HANDLE;
	m_session_running = false;
}

void XrApp::DestroyInstance(VkDevice gs_device)
{
	if (m_vk_instance)
	{
		// GSDeviceVK unloads the Vulkan library when it closes, so load it again for the teardown.
		if (!vkGetInstanceProcAddr && !Vulkan::LoadVulkanLibrary(nullptr))
		{
			Console.Error("XR: cannot reload Vulkan to tear down; leaking the device and instance.");
		}
		else
		{
			const auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(
				vkGetInstanceProcAddr(m_vk_instance, "vkDestroyDevice"));
			const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
				vkGetInstanceProcAddr(m_vk_instance, "vkDestroyInstance"));
			if (gs_device && destroy_device)
				destroy_device(gs_device, nullptr);
			if (destroy_instance)
				destroy_instance(m_vk_instance, nullptr);
			Vulkan::UnloadVulkanLibrary();
		}
		m_vk_instance = VK_NULL_HANDLE;
	}
	if (m_instance)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
}

bool XrApp::PollEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
				m_focus_gained |= changed.state == XR_SESSION_STATE_FOCUSED;
				m_session_state = changed.state;
				Console.WriteLnFmt("XR: session state {}", static_cast<int>(m_session_state));
				if (m_session_state == XR_SESSION_STATE_READY)
				{
					XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
					begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					std::lock_guard lock(GraphicsQueueMutex());
					m_session_running = XR_SUCCEEDED(xrBeginSession(m_session, &begin));
				}
				else if (m_session_state == XR_SESSION_STATE_STOPPING)
				{
					std::lock_guard lock(GraphicsQueueMutex());
					xrEndSession(m_session);
					m_session_running = false;
				}
				else if (m_session_state == XR_SESSION_STATE_EXITING || m_session_state == XR_SESSION_STATE_LOSS_PENDING)
				{
					m_exit_requested = true;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
				UpdateInteractionProfile();
				break;
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				m_exit_requested = true;
				break;
			default:
				break;
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
	return !m_exit_requested;
}

XrApp::Frame XrApp::BeginFrame()
{
	Frame frame;
	XrFrameState state{XR_TYPE_FRAME_STATE};
	if (XR_FAILED(xrWaitFrame(m_session, nullptr, &state)))
		return frame;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		xrBeginFrame(m_session, nullptr);
	}
	frame.display_time = state.predictedDisplayTime;
	frame.should_render = state.shouldRender;
	return frame;
}

void XrApp::EndFrame(const Frame& frame, const XrCompositionLayerBaseHeader* const* layers, uint32_t layer_count)
{
	XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
	info.displayTime = frame.display_time;
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.layerCount = frame.should_render ? layer_count : 0;
	info.layers = layers;
	std::lock_guard lock(GraphicsQueueMutex());
	if (const XrResult res = xrEndFrame(m_session, &info); XR_FAILED(res))
	{
		static int reported = 0;
		if (reported++ < 10)
			Console.ErrorFmt("XR: xrEndFrame failed ({}) with {} layers", static_cast<int>(res), info.layerCount);
	}
}

bool XrApp::GetBool(XrAction action) const
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = action;
	XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &info, &state)) && state.isActive && state.currentState;
}

float XrApp::GetFloat(XrAction action) const
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = action;
	XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
	return (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &info, &state)) && state.isActive) ? state.currentState : 0.0f;
}

XrVector2f XrApp::GetVector2(XrAction action) const
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
	info.action = action;
	XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
	if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &info, &state)) && state.isActive)
		return state.currentState;
	return {0.0f, 0.0f};
}

XrApp::Input XrApp::ReadInput()
{
	Input input;
	if (m_session_state != XR_SESSION_STATE_FOCUSED)
		return input;

	XrActiveActionSet active{m_action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(m_session, &sync)))
		return input;

	Emu::PadState& pad = input.pad;
	pad.cross = GetBool(m_cross);
	pad.circle = GetBool(m_circle);
	pad.square = GetBool(m_square);
	pad.triangle = GetBool(m_triangle);
	pad.l1 = GetFloat(m_l1);
	pad.r1 = GetFloat(m_r1);
	pad.l2 = GetFloat(m_l2);
	pad.r2 = GetFloat(m_r2);
	const XrVector2f ls = GetVector2(m_left_stick);
	const XrVector2f rs = GetVector2(m_right_stick);
	pad.lx = ls.x;
	pad.ly = ls.y;
	pad.rx = rs.x;
	pad.ry = rs.y;

	if (m_profile_has_dpad)
	{
		pad.up = GetBool(m_dpad_up);
		pad.down = GetBool(m_dpad_down);
		pad.left = GetBool(m_dpad_left);
		pad.right = GetBool(m_dpad_right);
	}
	else
	{
		// No D-pad on the controllers: a firmly pushed left stick also presses it, for menus.
		constexpr float threshold = 0.75f;
		pad.up = ls.y > threshold;
		pad.down = ls.y < -threshold;
		pad.left = ls.x < -threshold;
		pad.right = ls.x > threshold;
	}

	// Both sticks clicked recenters the screen instead of pressing L3 + R3.
	const bool l3 = GetBool(m_l3), r3 = GetBool(m_r3);
	const bool recenter = l3 && r3;
	input.recenter = recenter && !m_recenter_held;
	m_recenter_held = recenter;
	pad.l3 = l3 && !recenter;
	pad.r3 = r3 && !recenter;

	// One button for Start and Select: a short press taps Start, holding it holds Select.
	const Clock::time_point now = Clock::now();
	constexpr auto long_press = std::chrono::milliseconds(500);
	const bool menu = GetBool(m_menu);
	if (menu && !m_menu_down)
		m_menu_down_since = now;
	if (!menu && m_menu_down && now - m_menu_down_since < long_press)
		m_start_pulse_until = now + std::chrono::milliseconds(120);
	m_menu_down = menu;
	pad.select = menu && now - m_menu_down_since >= long_press;
	pad.start = now < m_start_pulse_until;
	return input;
}

bool XrApp::LocateEyes(XrTime time, XrView views[2]) const
{
	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = time;
	locate.space = m_local_space;
	XrViewState state{XR_TYPE_VIEW_STATE};
	views[0] = {XR_TYPE_VIEW};
	views[1] = {XR_TYPE_VIEW};
	uint32_t count = 0;
	return XR_SUCCEEDED(xrLocateViews(m_session, &locate, &state, 2, &count, views)) && count == 2 &&
	       (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
}

bool XrApp::LocateHead(XrTime time, XrPosef* pose) const
{
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(xrLocateSpace(m_view_space, m_local_space, time, &location)))
		return false;
	constexpr XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((location.locationFlags & needed) != needed)
		return false;
	*pose = location.pose;
	return true;
}
