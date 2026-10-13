// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// ARMSX2 VR: ARMSX2 running inside a native OpenXR app on Steam Frame.
//
// Startup order matters because the core and the XR runtime share one Vulkan device:
//   1. load Vulkan and install VKLibretro's wraps, so GSDeviceVK's vkCreateDevice/vkQueueSubmit
//      go through them;
//   2. create the XrInstance and, from it, the VkInstance and GPU (VKLibretro::Init);
//   3. start the CPU thread, which opens the GS: GSDeviceVK adopts them and creates the device;
//   4. bind the XR session to that device and run the frame loop on this thread.
// Teardown runs the other way: the session goes before the GS destroys the device.

#include "Controls.h"
#include "DepthView.h"
#include "Emu.h"
#include "Profile.h"
#include "ScreenLayer.h"
#include "Settings.h"
#include "VolumetricWindow.h"
#include "XrApp.h"
#include "XrMath.h"
#include "gun/GunAim.h"
#include "gun/GunDepth.h"
#include "gun/GunGames.h"
#include "gun/GunRenderer.h"
#include "ui/VrMenu.h"
#include "web/WebServer.h"

#include "GS/GSVolumetric.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKLibretro.h"

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/Config.h"

#include "fmt/format.h"

#define VK_NO_PROTOTYPES
#include "libretro_vulkan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Debugging views and dumps, from the command line only. The user's settings are in VrSettings.
struct DebugOptions
{
	std::string eye_dump_dir; // when set, write what both eyes see there every 10 s
	bool eye_panels = false; // show each eye's render on a flat panel instead of in the eyes
	bool stereo_alternate = false; // --stereo-alternate: the frame-alternating prototype instead of multiview
	bool depth_panel = false; // depth debug view beside the screen
	std::string depth_dump_dir; // when set, dump depth + colour PNGs there every 10 s
	int depth_log_ms = 5000; // interval of the "Depth:" log line
	// --gun-script: shots gun 1 fires by itself, for testing without the headset.
	struct GunShot
	{
		float time = 0.0f; // seconds after the game started
		bool offscreen = false;
		float u = 0.5f, v = 0.5f; // where in the game's picture (0..1, top left origin)
		uint32_t button = 0; // a tap of these Emu::GunButton bits instead of a shot
	};
	std::vector<GunShot> gun_script;
};

// "T:U:V" (a shot at U, V of the picture T seconds in), "T:off" (a reload) or "T:a", "T:b", "T:c",
// "T:start", "T:cal" (a tap of that button; cal: the calibration shot, at the centre), comma separated.
static bool ParseGunScript(const char* text, std::vector<DebugOptions::GunShot>* shots)
{
	std::string spec(text);
	size_t start = 0;
	while (start <= spec.size())
	{
		const size_t end = std::min(spec.find(',', start), spec.size());
		const std::string item = spec.substr(start, end - start);
		DebugOptions::GunShot shot;
		char rest[16] = {};
		static constexpr std::pair<const char*, uint32_t> buttons[] = {
			{"a", Emu::GUN_A}, {"b", Emu::GUN_B}, {"c", Emu::GUN_C}, {"start", Emu::GUN_START},
			{"cal", Emu::GUN_RECALIBRATE}};
		if (std::sscanf(item.c_str(), "%f:%f:%f", &shot.time, &shot.u, &shot.v) == 3)
			shots->push_back(shot);
		else if (std::sscanf(item.c_str(), "%f:%15s", &shot.time, rest) == 2)
		{
			shot.offscreen = !std::strcmp(rest, "off");
			for (const auto& [name, bit] : buttons)
				shot.button = !std::strcmp(rest, name) ? bit : shot.button;
			if (!shot.offscreen && !shot.button)
				return false;
			shots->push_back(shot);
		}
		else
			return false;
		start = end + 1;
	}
	return !shots->empty();
}

// Command-line overrides of VrSettings for this run: (setting key, value).
using SettingOverrides = std::vector<std::pair<std::string, std::string>>;

static void PrintUsage(const char* argv0)
{
	std::fprintf(stderr,
		"Usage: %s [options] [disc image]\n"
		"  Without a disc image, the game list opens (games are in the data folder's roms/).\n"
		"  --bios-only            Boot the BIOS without a disc\n"
		"  --no-fast-boot         Run the BIOS intro before the game\n"
		"  --upscale N            Internal resolution multiplier (1-8)\n"
		"  --screen-width M       Virtual screen width in meters (default 2.4)\n"
		"  --screen-distance M    Virtual screen distance in meters (default 2.2)\n"
		"  --flat                 Flat screen instead of the volumetric window\n"
		"  --depth-range M        How far behind the screen the scene goes (default 0.3)\n"
		"  --edge-fill            Fill depth edges with a background layer instead of stretching\n"
		"  --eye-dump DIR         Write both eyes' images to DIR every 10 seconds\n"
		"  --eye-panels           Show what each eye renders on two flat panels, side by side\n"
		"  --stereo               Geometric stereo: the GS draws every frame for both eyes (multiview),\n"
		"                         from where each eye is in front of the screen (window parallax)\n"
		"  --stereo-alternate     With --stereo, draw alternate frames for each eye instead (prototype)\n"
		"  --stereo-depth M       How far behind the screen the farthest geometry appears (default 1.0)\n"
		"  --stereo-conv F        Convergence: Q (or normalised Z with --stereo-z) on the screen plane (default 1)\n"
		"  --stereo-z             Drive the parallax by Z instead of Q\n"
		"  --pop-out F            Share of that depth in front of the screen (default 0)\n"
		"  --depth-panel          Show the depth debug view beside the screen\n"
		"  --no-hud-overlay       Leave the HUD painted on the scene instead of flat on the screen plane\n"
		"  --sdl-pad              Drive pad 1 from the first SDL gamepad (testing)\n"
		"  --load-state FILE      Start from a save state (SIGUSR1 saves debug/state.p2s, SIGUSR2 a snapshot)\n"
		"  --depth-trace          Log which colour/depth targets the 3D draws use\n"
		"  --depth-log-ms N       Log the depth statistics every N ms (default 5000)\n"
		"  --depth-dump DIR       Write depth and colour PNGs to DIR every 10 seconds\n"
		"  --gun-script SHOTS     Light gun 1 fires by itself: T:U:V shoots at U,V of the picture T s into\n"
		"                         the game, T:off reloads, T:a/b/c/start taps that button; comma separated,\n"
		"                         with the pedal held for shots from a second before the first (testing)\n"
		"The screen, window and stereo options are saved settings too (VR.ini in the settings folder,\n"
		"read again when edited); on the command line they apply to this run only.\n"
		"In the headset: both sticks clicked recenters the screen.\n",
		argv0);
}

static bool ParseArgs(int argc, char* argv[], Emu::Options* emu, DebugOptions* debug, SettingOverrides* settings)
{
	const auto set = [settings](const char* key, std::string value) { settings->emplace_back(key, std::move(value)); };
	for (int i = 1; i < argc; i++)
	{
		const char* arg = argv[i];
		const bool has_value = i + 1 < argc;
		if (!std::strcmp(arg, "--help") || !std::strcmp(arg, "-h"))
			return false;
		else if (!std::strcmp(arg, "--bios-only"))
			emu->bios_only = true;
		else if (!std::strcmp(arg, "--no-fast-boot"))
			emu->fast_boot = false;
		else if (!std::strcmp(arg, "--upscale") && has_value)
			emu->upscale = std::atoi(argv[++i]);
		else if (!std::strcmp(arg, "--screen-width") && has_value)
			set("screen_width", argv[++i]);
		else if (!std::strcmp(arg, "--screen-distance") && has_value)
			set("screen_distance", argv[++i]);
		else if (!std::strcmp(arg, "--sdl-pad"))
			emu->sdl_pad = true;
		else if (!std::strcmp(arg, "--load-state") && has_value)
			emu->load_state = argv[++i];
		else if (!std::strcmp(arg, "--depth-trace"))
			GSVolumetric::SetTrace(true);
		else if (!std::strcmp(arg, "--depth-log-ms") && has_value)
			debug->depth_log_ms = std::max(100, std::atoi(argv[++i]));
		else if (!std::strcmp(arg, "--flat"))
			set("flat", "true");
		else if (!std::strcmp(arg, "--depth-range") && has_value)
			set("depth_range", argv[++i]);
		else if (!std::strcmp(arg, "--pop-out") && has_value)
			set("pop_out", argv[++i]);
		else if (!std::strcmp(arg, "--depth-panel"))
			debug->depth_panel = true;
		else if (!std::strcmp(arg, "--no-hud-overlay"))
			set("hud_overlay", "false");
		else if (!std::strcmp(arg, "--edge-fill"))
			set("edge_fill", "true");
		else if (!std::strcmp(arg, "--eye-dump") && has_value)
			debug->eye_dump_dir = argv[++i];
		else if (!std::strcmp(arg, "--eye-panels"))
			debug->eye_panels = true;
		else if (!std::strcmp(arg, "--stereo"))
			set("stereo", "true");
		else if (!std::strcmp(arg, "--stereo-alternate"))
			debug->stereo_alternate = true;
		else if (!std::strcmp(arg, "--stereo-depth") && has_value)
			set("stereo_depth", argv[++i]);
		else if (!std::strcmp(arg, "--stereo-conv") && has_value)
			set("stereo_convergence", argv[++i]);
		else if (!std::strcmp(arg, "--stereo-z"))
			set("stereo_z_mode", "true");
		else if (!std::strcmp(arg, "--depth-dump") && has_value)
			debug->depth_dump_dir = argv[++i];
		else if (!std::strcmp(arg, "--gun-script") && has_value)
		{
			if (!ParseGunScript(argv[++i], &debug->gun_script))
			{
				std::fprintf(stderr, "Bad --gun-script: %s\n", argv[i]);
				return false;
			}
		}
		else if (arg[0] == '-')
		{
			std::fprintf(stderr, "Unknown argument: %s\n", arg);
			return false;
		}
		else
			emu->boot_path = arg;
	}
	if (emu->upscale && (*emu->upscale < 1 || *emu->upscale > 8))
	{
		std::fprintf(stderr, "--upscale must be between 1 and 8.\n");
		return false;
	}
	return true;
}

// The GS thread submits through VKLibretro's wrapped vkQueueSubmit, which locks through this.
static void LockQueue(void*)
{
	GraphicsQueueMutex().lock();
}

static void UnlockQueue(void*)
{
	GraphicsQueueMutex().unlock();
}

static retro_hw_render_interface_vulkan s_queue_interface = [] {
	retro_hw_render_interface_vulkan iface{};
	iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
	iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	iface.lock_queue = &LockQueue;
	iface.unlock_queue = &UnlockQueue;
	return iface;
}();

// Places the screen straight ahead of the head (yaw only), at eye height, facing the viewer.
static XrPosef ScreenPoseInFrontOf(const XrPosef& head, float distance)
{
	const XrQuaternionf& q = head.orientation;
	// Head forward is -Z rotated by q.
	const float fx = -2.0f * (q.x * q.z + q.w * q.y);
	const float fz = -1.0f + 2.0f * (q.x * q.x + q.y * q.y);
	const float len = std::sqrt(fx * fx + fz * fz);
	const float dx = len > 1e-4f ? fx / len : 0.0f;
	const float dz = len > 1e-4f ? fz / len : -1.0f;
	const float yaw = std::atan2(-dx, -dz);

	XrPosef pose;
	pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
	pose.position = {head.position.x + dx * distance, head.position.y, head.position.z + dz * distance};
	return pose;
}

// A panel beside the screen, `offset` meters to its right in the screen's plane, turned to face a
// viewer `distance` in front of the screen.
static XrPosef PanelBeside(const XrPosef& screen, float offset, float distance)
{
	const XrQuaternionf& q = screen.orientation; // yaw only, see ScreenPoseInFrontOf
	const float yaw = 2.0f * std::atan2(q.y, q.w);
	const float turn = std::atan2(-offset, distance);
	XrPosef pose;
	pose.orientation = {0.0f, std::sin((yaw + turn) * 0.5f), 0.0f, std::cos((yaw + turn) * 0.5f)};
	pose.position = {screen.position.x + std::cos(yaw) * offset, screen.position.y,
		screen.position.z - std::sin(yaw) * offset};
	return pose;
}

// The screen moved `delta` meters further away along its axis, as when its distance changes.
static XrPosef PushScreen(const XrPosef& screen, float delta)
{
	const XrQuaternionf& q = screen.orientation; // yaw only, see ScreenPoseInFrontOf
	const float yaw = 2.0f * std::atan2(q.y, q.w);
	XrPosef pose = screen;
	pose.position.x -= std::sin(yaw) * delta;
	pose.position.z -= std::cos(yaw) * delta;
	return pose;
}

static GSVolumetric::StereoSettings GSStereo(const VrSettings& vr, bool enabled, bool multiview)
{
	GSVolumetric::StereoSettings stereo;
	stereo.enabled = enabled;
	stereo.multiview = multiview;
	stereo.z_mode = vr.stereo_z_mode;
	stereo.convergence = vr.stereo_convergence;
	return stereo;
}

int main(int argc, char* argv[])
{
	Emu::Options emu_options;
	DebugOptions debug_options;
	SettingOverrides setting_overrides;
	if (!ParseArgs(argc, argv, &emu_options, &debug_options, &setting_overrides))
	{
		PrintUsage(argv[0]);
		return EXIT_FAILURE;
	}

	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);

	if (!Emu::InitializeConfig(emu_options))
		return EXIT_FAILURE;

	VrSettingsStore settings;
	settings.Load(Path::Combine(EmuFolders::Settings, "VR.ini"));
	for (const auto& [key, value] : setting_overrides)
	{
		if (!settings.Override(key, value))
		{
			Console.ErrorFmt("Bad value for {}: {}", key, value);
			return EXIT_FAILURE;
		}
	}
	// Wi-Fi transfer, switched on from the menu: the BIOS folder and the games folder.
	WebServer web;
	web.SetFolders({
		{"bios", EmuFolders::Bios, {".bin", ".rom0", ".rom1", ".rom2", ".erom", ".mec", ".nvm"}},
		{"roms", Emu::GetGamesFolder(), Emu::GetGameExtensions()},
	});

	// What the controllers do in the game, set in Settings › Controls.
	ControlMap controls;
	controls.Load(Path::Combine(EmuFolders::Settings, "Controls.ini"));

	// Light gun mode per game: the guns are plugged in as a game starts, before it looks for them.
	GunGames gun_games;
	gun_games.Load(Path::Combine(EmuFolders::Settings, "lightgun.ini"));
	// How many guns: VR.ini's, read on the CPU thread as a game starts.
	std::atomic<int> gun_count{settings.Get().gun_players > 1.5f ? 2 : 1};
	Emu::SetGunsPolicy([&gun_games, &gun_count](const std::string& serial) {
		return gun_games.IsOn(serial) ? gun_count.load() : 0;
	});

	const VrSettings& vr = settings.Get(); // the current settings, as changed while running
	// The mode is fixed for the run. The stereo prototype shows the GS's own per-eye frames on a
	// flat screen.
	const bool stereo = vr.stereo;
	const bool flat = vr.flat || stereo;
	const bool multiview = !debug_options.stereo_alternate; // both eyes every frame; the prototype alternates

	Error error;
	if (!Vulkan::LoadVulkanLibrary(&error))
	{
		Console.ErrorFmt("Failed to load Vulkan: {}", error.GetDescription());
		return EXIT_FAILURE;
	}
	VKLibretro::InstallWraps();

	XrApp xr;
	if (!xr.CreateInstance())
		return EXIT_FAILURE;
	VKLibretro::SetHWRenderInterface(&s_queue_interface);
	VKLibretro::Active = true;
	GSVolumetric::SetEnabled(!stereo);
	GSVolumetric::SetStereo(GSStereo(vr, stereo, multiview));
	if (stereo)
		Console.WriteLnFmt("Stereo: {}, {} mode, depth {} m, convergence {}", multiview ? "multiview" : "alternating frames",
			vr.stereo_z_mode ? "Z" : "Q", vr.stereo_depth, vr.stereo_convergence);
	if (!debug_options.depth_dump_dir.empty())
		FileSystem::EnsureDirectoryExists(debug_options.depth_dump_dir.c_str(), true);
	if (!debug_options.eye_dump_dir.empty())
		FileSystem::EnsureDirectoryExists(debug_options.eye_dump_dir.c_str(), true);

	Emu::Start(emu_options);
	if (!Emu::WaitForGS())
	{
		Console.Error("The GS did not come up; exiting.");
		Emu::RequestShutdown();
		Emu::Join();
		return EXIT_FAILURE;
	}

	GSDeviceVK* gs = GSDeviceVK::GetInstance();
	GunRenderer gun_renderer;
	GunDepth gun_depth;
	ScreenLayer screen;
	ScreenLayer screen_right; // with --stereo, the right eye's frames; screen then shows the left eye's
	ScreenLayer depth_panel;
	std::array<ScreenLayer, 2> eye_panels;
	DepthView depth_view;
	VolumetricWindow window;
	VrMenu menu(settings, web, controls, stereo, flat);
	const VkDevice device = gs->GetDevice();
	const VkQueue queue = gs->GetGraphicsQueue();
	const uint32_t queue_family = gs->GetGraphicsQueueFamilyIndex();
	if (!xr.CreateSession(device, queue_family) || !screen.Create(xr, device, queue, queue_family) ||
		(stereo && !screen_right.Create(xr, device, queue, queue_family)) ||
		!depth_panel.Create(xr, device, queue, queue_family) ||
		!depth_view.Create(device, gs->GetPhysicalDevice(), queue, queue_family) ||
		(debug_options.eye_panels && (!eye_panels[0].Create(xr, device, queue, queue_family) ||
										  !eye_panels[1].Create(xr, device, queue, queue_family))) ||
		(!flat && !window.Create(xr, device, gs->GetPhysicalDevice(), queue, queue_family)))
	{
		Emu::RequestShutdown();
		menu.Destroy();
		for (ScreenLayer& panel : eye_panels)
			panel.Destroy();
		window.Destroy();
		depth_view.Destroy();
		depth_panel.Destroy();
		screen_right.Destroy();
		screen.Destroy();
		xr.DestroySession();
		VKLibretro::AbortPacing();
		Emu::Join();
		return EXIT_FAILURE;
	}
	if (!menu.Create(xr, device, gs->GetPhysicalDevice(), queue, queue_family))
		Console.Error("The settings menu is unavailable.");
	if (!gun_renderer.Create(xr, device, gs->GetPhysicalDevice(), queue, queue_family,
			Path::Combine(EmuFolders::AppRoot, "assets" FS_OSPATH_SEPARATOR_STR "lightgun.glb")))
		Console.Error("Light gun: no gun model; light gun mode shows no guns.");
	if (stereo && !gun_depth.Create(device, gs->GetPhysicalDevice(), queue, queue_family))
		Console.Error("Light gun: no depth; shots land on the screen glass.");
	if (stereo)
	{
		screen.SetEyeVisibility(XR_EYE_VISIBILITY_LEFT);
		screen_right.SetEyeVisibility(XR_EYE_VISIBILITY_RIGHT);
	}
	VolumetricWindow::Settings window_settings;
	window.SetCaptureEyes(debug_options.eye_panels);
	constexpr float eye_panel_width = 1.4f;
	for (ScreenLayer& panel : eye_panels)
		panel.SetWidth(eye_panel_width);
	constexpr float depth_panel_width = 1.2f;
	depth_panel.SetWidth(depth_panel_width);

	using Clock = std::chrono::steady_clock;
	Clock::time_point next_stats_log = Clock::now() + std::chrono::seconds(5);
	Clock::time_point next_eye_log = next_stats_log; // apart: no game frames come while paused or on the game list
	Clock::time_point next_dump = Clock::now() + std::chrono::seconds(10);
	int dump_index = 0;
	Clock::time_point next_eye_dump = Clock::now() + std::chrono::seconds(10);
	int eye_dump_index = 0;

	// Hands the settings to what keeps a copy of them.
	const auto apply_settings = [&] {
		screen.SetWidth(vr.screen_width);
		screen_right.SetWidth(vr.screen_width);
		window_settings.width = vr.screen_width;
		window_settings.depth_range = vr.depth_range;
		window_settings.pop_out = vr.pop_out;
		window_settings.hud_overlay = vr.hud_overlay;
		window_settings.edge_fill = vr.edge_fill;
		window_settings.viewer_distance = vr.screen_distance;
		window.SetSettings(window_settings);
		// The GS thread reads these per draw: a change can land in the middle of a frame.
		GSVolumetric::SetStereo(GSStereo(vr, stereo, multiview));
	};
	apply_settings();
	VrSettings applied = vr;
	unsigned applied_version = settings.Version();

	bool screen_placed = false;
	XrPosef screen_pose_now{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -vr.screen_distance}};
	// Puts the screen at pose, and the debug panels beside it.
	const auto place_screen = [&](const XrPosef& pose) {
		screen_pose_now = pose;
		screen.SetPose(pose);
		screen_right.SetPose(pose);
		window.SetScreen(pose, window_settings);
		depth_panel.SetPose(PanelBeside(pose, 0.5f * (vr.screen_width + depth_panel_width) + 0.15f, vr.screen_distance));
		// Eye panels: left eye's render left of centre, right eye's right of it.
		eye_panels[0].SetPose(PanelBeside(pose, -0.5f * eye_panel_width - 0.05f, vr.screen_distance));
		eye_panels[1].SetPose(PanelBeside(pose, 0.5f * eye_panel_width + 0.05f, vr.screen_distance));
	};

	std::string profile_serial;
	// --stereo: when one eye's frames stop coming (a movie shown at half the frame rate lands on the
	// same eye every time), both eyes see the other's for a while.
	std::array<Clock::time_point, 2> last_eye_frame{};
	float screen_aspect = 4.0f / 3.0f;
	bool menu_recenter = false; // the menu's Recenter, done the next frame
	bool pad_recenter = false; // both sticks clicked, done the next frame
	// Started without a game: the game list opens once the screen is in place.
	bool show_game_list = emu_options.boot_path.empty() && !emu_options.bios_only;

	// Light gun mode for the running game (GunGames), and the guns' state between frames.
	bool gun_mode = false;
	bool gun_chosen = false;
	std::array<float, 2> gun_press_left{}, gun_press_right{}; // eased button presses, per gun
	std::array<bool, 2> gun_fired{}; // the trigger was down last frame, per gun (recoil on a new pull)
	std::optional<SettingsMenu::GunAction> gun_action; // asked in the menu, done once it has closed
	Clock::time_point gun_action_at{}, gun_action_until{};
	Clock::time_point gun_script_start{}; // --gun-script: when the game started
	size_t gun_script_next = 0;
	int gun_script_logged = -1;
	// The presented frame, as the light guns aim at it.
	float frame_size[2] = {1280.0f, 960.0f};
	float frame_draw_rect[4] = {0.0f, 0.0f, 1280.0f, 960.0f};
	// Depth for the guns in stereo: only while light gun mode is on (it costs copies on the GS).
	const auto apply_gun_depth = [&] {
		if (stereo)
			GSVolumetric::SetEnabled(gun_mode);
	};
	const auto set_gun_mode = [&](bool on) {
		if (on == gun_mode)
			return;
		gun_mode = on;
		Emu::SetGunsPlugged(on ? gun_count.load() : 0);
		if (!on)
			Emu::ReleaseGuns();
		apply_gun_depth();
		Console.WriteLnFmt("Light gun: mode {} for {}", on ? "on" : "off", profile_serial);
	};
	while (!Emu::ShutdownRequested() && xr.PollEvents())
	{
		if (!xr.IsSessionRunning())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// Settings changed while running (VR.ini edited for now, later the menu).
		settings.Update();
		if (settings.Version() != applied_version)
		{
			applied_version = settings.Version();
			for (const VrSettingInfo& info : VrSettingList())
			{
				if (info.restart && !SameVrSetting(info, applied, vr))
					Console.WriteLnFmt("VR settings: {} takes effect at the next start", info.key);
			}
			apply_settings();
			// A new distance moves the screen along its axis; a new width moves the panels beside it.
			if (screen_placed && (vr.screen_distance != applied.screen_distance || vr.screen_width != applied.screen_width))
				place_screen(PushScreen(screen_pose_now, vr.screen_distance - applied.screen_distance));
			// One gun or two: plug the second in or out.
			if (const int guns = vr.gun_players > 1.5f ? 2 : 1; guns != gun_count.load())
			{
				gun_count.store(guns);
				if (gun_mode)
					Emu::SetGunsPlugged(guns);
			}
			applied = vr;
		}

		// The game's profile, once its serial is known.
		if (const std::string serial = Emu::GetSerial(); serial != profile_serial)
		{
			profile_serial = serial;
			web.SetProtectedFile(Emu::GetRunningGamePath());
			const GameProfile profile = LoadGameProfile(serial);
			window_settings.hud_rects = profile.hud_rects;
			window_settings.hud_rect_flat = profile.hud_rect_flat;
			window.SetSettings(window_settings);
			// The CPU thread plugged the guns in or out as the game started (Emu::SetGunsPolicy).
			gun_mode = !serial.empty() && gun_games.IsOn(serial);
			gun_chosen = gun_games.IsChosen(serial);
			if (!gun_mode)
				Emu::ReleaseGuns();
			apply_gun_depth();
			gun_script_start = serial.empty() ? Clock::time_point{} : Clock::now();
			gun_script_next = 0;
			gun_script_logged = -1;
			if (!serial.empty())
				Console.WriteLnFmt("Light gun: mode {} for {}{}", gun_mode ? "on" : "off", serial, gun_chosen ? " (chosen)" : "");
		}

		const XrApp::Frame frame = xr.BeginFrame();
		const XrApp::Input input = xr.ReadInput();

		XrPosef head;
		// In front of the head whenever the app gains focus (the headset was put on: before that the
		// head pose is wherever it lies; or the dashboard closed), and on request.
		if (xr.TakeFocusGained())
			screen_placed = false;
		const bool recenter = pad_recenter || menu_recenter;
		menu_recenter = pad_recenter = false;
		if ((!screen_placed || recenter) && xr.LocateHead(frame.display_time, &head))
		{
			place_screen(ScreenPoseInFrontOf(head, vr.screen_distance));
			Console.WriteLnFmt("Screen placed ({}): head ({:.3f} {:.3f} {:.3f}) q ({:.3f} {:.3f} {:.3f} {:.3f})",
				recenter ? "recenter" : "first", head.position.x, head.position.y, head.position.z,
				head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w);
			screen_placed = true;
		}

		// The light guns: which hands hold one, and where the guns are. Gun 1 is in the gun hand,
		// gun 2 (with two guns) in the other.
		const int num_guns = gun_mode ? (vr.gun_players > 1.5f ? 2 : 1) : 0;
		const int first_hand = vr.gun_left_hand ? 0 : 1;
		std::array<int, 2> gun_of_hand = {-1, -1};
		for (int g = 0; g < num_guns; g++)
			gun_of_hand[g == 0 ? first_hand : 1 - first_hand] = g;
		std::array<bool, 2> aim_valid{};
		std::array<XrPosef, 2> aim_pose{}, gun_pose{}, muzzle_pose{};
		for (int h = 0; h < 2; h++)
		{
			aim_valid[h] = xr.LocateAim(h, frame.display_time, &aim_pose[h]);
			if (!aim_valid[h] || gun_of_hand[h] < 0)
				continue;
			// The barrel along the controller's pointing ray (tilted by the gun angle), held at the
			// grip; without a grip pose, a little below and behind the ray's origin.
			gun_pose[h].orientation = aim_pose[h].orientation *
			                          QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, vr.gun_pitch * 3.14159265f / 180.0f);
			XrPosef grip;
			gun_pose[h].position = xr.LocateGrip(h, frame.display_time, &grip) ?
			                           grip.position :
			                           Transform(aim_pose[h], {0.0f, -0.06f, 0.08f});
			muzzle_pose[h] = Compose(gun_pose[h], gun_renderer.Model().Muzzle());
		}

		// The game's controls. A trigger is a menu click while a menu is open or the pointer is on a
		// menu button; only otherwise does it count as the game button it is mapped to. A hand with a
		// gun points at the menus with the gun.
		ControllerState controller = input.controller;
		VrMenu::Result ui;
		if (screen_placed)
		{
			if (std::exchange(show_game_list, false))
				menu.OpenGameList();
			std::array<VrMenu::Pointer, 2> pointers;
			for (int h = 0; h < 2; h++)
			{
				const bool gun = gun_of_hand[h] >= 0 && gun_renderer.IsCreated();
				pointers[h] = {aim_valid[h], gun ? muzzle_pose[h] : aim_pose[h], gun};
			}
			menu.SetGunState(!profile_serial.empty(), gun_mode, gun_chosen, vr.gun_players > 1.5f ? 2 : 1);
			ui = menu.Update(frame.display_time, input, pointers, screen_pose_now, vr.screen_width,
				vr.screen_width / screen_aspect);
			menu_recenter = ui.recenter;
			if (ui.open)
				controller = {};
			if (ui.block_trigger[0])
				controller[ControlInput::LeftTrigger] = 0.0f;
			if (ui.block_trigger[1])
				controller[ControlInput::RightTrigger] = 0.0f;
			// Light gun requests from the buttons beside the screen and the settings page.
			if (ui.gun_mode && !profile_serial.empty())
			{
				if (*ui.gun_mode == SettingsMenu::GunModeRequest::Auto)
					gun_games.Reset(profile_serial);
				else
					gun_games.Set(profile_serial, *ui.gun_mode == SettingsMenu::GunModeRequest::On);
				gun_chosen = gun_games.IsChosen(profile_serial);
				set_gun_mode(gun_games.IsOn(profile_serial));
			}
			if (ui.toggle_guns)
			{
				VrSettings changed = vr;
				changed.gun_players = vr.gun_players > 1.5f ? 1.0f : 2.0f;
				settings.Set(changed);
			}
			if (ui.gun_action)
			{
				gun_action = ui.gun_action;
				gun_action_at = Clock::now() + std::chrono::milliseconds(400); // once the game runs again
			}
		}

		if (!gun_mode)
		{
			const ControlMap::Output game = controls.Apply(controller);
			pad_recenter = game.recenter;
			if (!emu_options.sdl_pad) // with --sdl-pad, the SDL gamepad drives pad 1 alone
				Emu::SetPad(game.pad);
		}
		std::array<GunRenderer::Gun, 2> guns_drawn;
		if (gun_mode)
		{
			// The pedal presses the gun button the game takes cover with.
			static constexpr uint32_t pedal_buttons[] = {Emu::GUN_A, Emu::GUN_B, Emu::GUN_C};
			const ControlMap::GunsOutput out =
				controls.ApplyGuns(controller, num_guns, pedal_buttons[std::clamp(static_cast<int>(vr.gun_pedal + 0.5f), 0, 2)]);
			pad_recenter = out.recenter;
			if (!emu_options.sdl_pad)
				Emu::SetPad({}); // the guns are in the hands: the pad stays still

			// Where the guns aim: the screen as shown, and in stereo the depth they shoot into.
			GunScreen gun_screen;
			gun_screen.pose = screen_pose_now;
			gun_screen.width = vr.screen_width;
			gun_screen.height = vr.screen_width / screen_aspect;
			std::copy(std::begin(frame_draw_rect), std::end(frame_draw_rect), gun_screen.draw_rect);
			if (flat)
			{
				gun_screen.view[2] = frame_size[0];
				gun_screen.view[3] = frame_size[1];
			}
			else
			{
				// The volumetric window shows the picture only, at its own aspect.
				std::copy(std::begin(frame_draw_rect), std::end(frame_draw_rect), gun_screen.view);
				gun_screen.height = vr.screen_width * (frame_draw_rect[3] - frame_draw_rect[1]) /
				                    std::max(frame_draw_rect[2] - frame_draw_rect[0], 1.0f);
			}
			GunStereo gun_stereo;
			const GunDepthGrid* depth_grid = nullptr;
			XrView eye_views[2];
			if (stereo && vr.stereo_z_mode && xr.LocateEyes(frame.display_time, eye_views))
			{
				gun_stereo.z_mode = true;
				gun_stereo.convergence = vr.stereo_convergence;
				gun_stereo.depth = vr.stereo_depth;
				gun_stereo.left_eye = eye_views[0].pose.position;
				// A grid from a few frames back still fits; older (a menu, a movie), the glass will do.
				depth_grid = gun_depth.Latest();
				if (depth_grid && Clock::now() - depth_grid->time > std::chrono::milliseconds(300))
					depth_grid = nullptr;
			}

			const Clock::time_point now = Clock::now();
			if (gun_action && now >= gun_action_at && !ui.open)
			{
				gun_action_until = now + std::chrono::milliseconds(150);
				gun_action_at = Clock::time_point::max();
			}
			const bool action_held = gun_action && now < gun_action_until;
			if (gun_action && !action_held && gun_action_at == Clock::time_point::max())
				gun_action.reset();

			for (int g = 0; g < Emu::NUM_GUNS; g++)
			{
				Emu::GunState state;
				if (g >= num_guns)
				{
					Emu::SetGun(g, state); // plugged in but idle: off the screen, nothing pressed
					continue;
				}
				const int h = g == 0 ? first_hand : 1 - first_hand;
				state.buttons = out.gun[g].buttons;
				GunRenderer::Gun& drawn = guns_drawn[g];
				GunAim aim;
				if (aim_valid[h])
				{
					aim = AimGun(muzzle_pose[h], gun_screen, gun_stereo.z_mode ? &gun_stereo : nullptr, depth_grid);
					const bool on_ui = ui.on_ui[h] || ui.open;
					state.aim_valid = aim.on_picture && !on_ui;
					state.x = aim.window_x;
					state.y = aim.window_y;

					drawn.visible = gun_renderer.IsCreated();
					drawn.pose = gun_pose[h];
					drawn.body_color = vr.gun_color;
					drawn.trigger = controls.GunTrigger(input.controller, g);
					const float ease = 0.35f; // per frame, about 40 ms to settle at 90 Hz
					gun_press_left[g] += ((state.buttons & Emu::GUN_A) ? 1.0f - gun_press_left[g] : -gun_press_left[g]) * ease;
					gun_press_right[g] += ((state.buttons & Emu::GUN_B) ? 1.0f - gun_press_right[g] : -gun_press_right[g]) * ease;
					drawn.press_left = gun_press_left[g];
					drawn.press_right = gun_press_right[g];
					drawn.beam_color = g == 0 ? 0xFF3A30 : 0x3A8CFF;
					const int beam_mode = std::clamp(static_cast<int>(vr.gun_beam + 0.5f), 0, 2);
					if (ui.on_ui[h])
					{
						// On a menu or a button: the beam is the menu's pointer, always with its dot.
						drawn.beam_end = ui.ui_point[h];
						drawn.beam = beam_mode == 0;
						drawn.dot = true;
					}
					else if (aim.on_screen)
					{
						drawn.beam_end = aim.end;
						drawn.beam = beam_mode == 0;
						drawn.dot = beam_mode <= 1 && aim.on_picture;
					}
					else
					{
						// Beside the screen: the beam ends on its plane, as the menu pointer's does;
						// pointing away from it, a short beam shows where.
						drawn.beam_end = aim.on_plane ? aim.end : Transform(muzzle_pose[h], {0.0f, 0.0f, -1.0f});
						drawn.beam = beam_mode == 0;
					}
				}
				if (g == 0 && action_held)
					state.buttons |= *gun_action == SettingsMenu::GunAction::Recalibrate ? Emu::GUN_RECALIBRATE :
					                                                                       Emu::GUN_SHOOT_OFFSCREEN;

				// --gun-script: gun 1 fires by itself, aimed in the picture, until its last shot.
				const auto& shots = debug_options.gun_script;
				const float t = std::chrono::duration<float>(now - gun_script_start).count();
				while (gun_script_next < shots.size() && t >= shots[gun_script_next].time + 0.15f)
					gun_script_next++;
				if (g == 0 && gun_script_next < shots.size() && gun_script_start != Clock::time_point{})
				{
					const DebugOptions::GunShot& shot = shots[gun_script_next];
					const bool firing = t >= shot.time;
					const float* r = frame_draw_rect;
					state.aim_valid = !shot.offscreen;
					state.x = r[0] + shot.u * (r[2] - r[0]);
					state.y = r[1] + shot.v * (r[3] - r[1]);
					// For a shot, the pedal is held from a second before the first one (out of cover);
					// it is let go for an off-screen shot (back into cover, as players reload) and for
					// button taps.
					const bool aimed = !shot.offscreen && !shot.button;
					const bool pedal = aimed && t >= shots.front().time - 1.0f;
					state.buttons = (firing ? (shot.button ? shot.button : Emu::GUN_TRIGGER) : 0u) |
					                (pedal ? pedal_buttons[std::clamp(static_cast<int>(vr.gun_pedal + 0.5f), 0, 2)] : 0u);
					if (firing && static_cast<int>(gun_script_next) != gun_script_logged)
					{
						gun_script_logged = static_cast<int>(gun_script_next);
						Console.WriteLnFmt("Gun script: {} {} at {:.1f} s: {}", shot.button ? "tap" : "shot",
							gun_script_next + 1, t,
							shot.button ? fmt::format("buttons {:#x}", shot.button) :
							shot.offscreen ? std::string("off screen") :
							                 fmt::format("({:.0f}, {:.0f})", state.x, state.y));
					}
				}

				// Recoil on each new pull of the trigger.
				const bool fired = (state.buttons & (Emu::GUN_TRIGGER | Emu::GUN_SHOOT_OFFSCREEN)) != 0;
				if (fired && !gun_fired[g])
				{
					xr.Vibrate(h, 0.03f, vr.gun_haptics);
					// Where each shot went, to compare with where the game shows it.
					Console.WriteLnFmt("Light gun: gun {} shot {} ({:.0f}, {:.0f}) of {:.0f}x{:.0f}, {}", g + 1,
						state.aim_valid ? "at" : "off the picture,", state.x, state.y, frame_size[0], frame_size[1],
						!aim_valid[h]       ? "no pose" :
						aim.through_depth   ? fmt::format("beam ends {:.2f} m behind the glass", -Dot(aim.end -
						                          screen_pose_now.position, Rotate(screen_pose_now.orientation, {0.0f, 0.0f, 1.0f}))) :
						aim.on_screen       ? std::string("beam ends on the glass") :
						                      std::string("off the screen"));
				}
				gun_fired[g] = fired;
				Emu::SetGun(g, state);
			}
		}

		// --stereo: where each eye is in front of the screen, for the GS's window parallax.
		if (stereo && screen_placed)
		{
			XrView views[2];
			if (xr.LocateEyes(frame.display_time, views))
			{
				const bool log_eyes = Clock::now() >= next_eye_log;
				if (log_eyes)
					next_eye_log = Clock::now() + std::chrono::milliseconds(debug_options.depth_log_ms);
				const XrQuaternionf& q = screen_pose_now.orientation; // yaw only
				const float yaw = 2.0f * std::atan2(q.y, q.w);
				const float c = std::cos(yaw), s = std::sin(yaw);
				const float half_w = 0.5f * vr.screen_width;
				const float half_h = half_w / screen_aspect;
				for (int i = 0; i < 2; i++)
				{
					// The eye in the screen's frame: x right, y up, z toward the viewer.
					const float dx = views[i].pose.position.x - screen_pose_now.position.x;
					const float dy = views[i].pose.position.y - screen_pose_now.position.y;
					const float dz = views[i].pose.position.z - screen_pose_now.position.z;
					const float lx = c * dx - s * dz;
					const float lz = s * dx + c * dz;
					const float distance = std::max(lz, 0.1f);
					// Far geometry follows the eye on the screen (NDC y points down).
					GSVolumetric::SetEyeView(i, lx / half_w, -dy / half_h,
						vr.stereo_depth / (distance + vr.stereo_depth));
					if (log_eyes)
						Console.WriteLnFmt("Stereo: eye {} at ({:.3f}, {:.3f}) m, {:.2f} m from the screen -> offset ({:.3f}, {:.3f}) NDC, depth factor {:.3f}",
							i, lx, dy, distance, lx / half_w, -dy / half_h,
							vr.stereo_depth / (distance + vr.stereo_depth));
				}
			}
		}

		VKLibretro::Frame emu_frame;
		if (VKLibretro::ConsumeFrame(&emu_frame))
		{
			if (emu_frame.width && emu_frame.height)
			{
				screen_aspect = static_cast<float>(emu_frame.width) / static_cast<float>(emu_frame.height);
				frame_size[0] = static_cast<float>(emu_frame.width);
				frame_size[1] = static_cast<float>(emu_frame.height);
				const float* r = emu_frame.draw_rect;
				const bool has_rect = r[2] > r[0] && r[3] > r[1];
				frame_draw_rect[0] = has_rect ? r[0] : 0.0f;
				frame_draw_rect[1] = has_rect ? r[1] : 0.0f;
				frame_draw_rect[2] = has_rect ? r[2] : frame_size[0];
				frame_draw_rect[3] = has_rect ? r[3] : frame_size[1];
			}
			// Stereo: the guns' depth (with multiview, a layer per eye).
			if (stereo && gun_mode && emu_frame.depth_image)
				gun_depth.Process(emu_frame, emu_frame.right_image != VK_NULL_HANDLE);
			if (stereo && emu_frame.right_image)
			{
				// Multiview: both eyes in one frame.
				screen.Update(emu_frame);
				VKLibretro::Frame right_frame = emu_frame;
				right_frame.image = emu_frame.right_image;
				right_frame.view = emu_frame.right_view;
				screen_right.Update(right_frame);
				last_eye_frame[0] = last_eye_frame[1] = Clock::now();
			}
			else if (stereo && emu_frame.stereo_eye == 1)
				screen_right.Update(emu_frame);
			else if (flat)
				screen.Update(emu_frame);
			if (emu_frame.stereo_eye >= 0)
				last_eye_frame[emu_frame.stereo_eye] = Clock::now();
			const Clock::time_point now = Clock::now();
			if (!debug_options.depth_dump_dir.empty() && now >= next_dump && emu_frame.depth_image)
			{
				depth_view.RequestDump(fmt::format("{}/frame{:03d}", debug_options.depth_dump_dir, dump_index++));
				next_dump = now + std::chrono::seconds(10);
			}
			VKLibretro::Frame depth_frame;
			// In stereo the depth is only there for the light guns (and can have a layer per eye).
			const bool has_depth = !stereo && depth_view.Process(emu_frame, &depth_frame);
			if (has_depth && debug_options.depth_panel)
				depth_panel.Update(depth_frame);
			if (!flat)
				window.SetFrame(emu_frame, has_depth);
			if (now >= next_stats_log)
			{
				next_stats_log = now + std::chrono::milliseconds(debug_options.depth_log_ms);
				if (!emu_frame.depth_image)
					Console.WriteLn("Depth: none this frame");
				else if (const auto stats = depth_view.LastStats())
					Console.WriteLnFmt("Depth: coverage {:.0f}%, HUD {:.0f}%{}, range {:.6f}..{:.6f} -> {:.6f}..{:.6f}, "
									   "canvas {}x{}, scale {:.1f}",
						stats->coverage * 100.0f, stats->hud * 100.0f, emu_frame.prehud_view ? "" : " (no pre-HUD copy)",
						stats->zmin, stats->zmax, stats->lo, stats->hi, emu_frame.depth_width, emu_frame.depth_height,
						emu_frame.depth_scale);
			}
		}

		const XrCompositionLayerBaseHeader* layers[16];
		uint32_t layer_count = 0;
		if (flat && stereo)
		{
			const Clock::time_point now = Clock::now();
			const bool left_live = screen.HasImage() && now - last_eye_frame[0] < std::chrono::milliseconds(150);
			const bool right_live = screen_right.HasImage() && now - last_eye_frame[1] < std::chrono::milliseconds(150);
			if (left_live && right_live)
			{
				screen.SetEyeVisibility(XR_EYE_VISIBILITY_LEFT);
				screen_right.SetEyeVisibility(XR_EYE_VISIBILITY_RIGHT);
				layers[layer_count++] = screen.Layer();
				layers[layer_count++] = screen_right.Layer();
			}
			else if (ScreenLayer* only = left_live || !right_live ? &screen : &screen_right; only->HasImage())
			{
				only->SetEyeVisibility(XR_EYE_VISIBILITY_BOTH);
				layers[layer_count++] = only->Layer();
			}
		}
		else if (flat)
		{
			if (screen.HasImage())
				layers[layer_count++] = screen.Layer();
		}
		else if (frame.should_render && window.HasFrame())
		{
			if (!debug_options.eye_dump_dir.empty() && Clock::now() >= next_eye_dump)
			{
				window.RequestEyeDump(fmt::format("{}/eyes{:03d}", debug_options.eye_dump_dir, eye_dump_index++));
				next_eye_dump = Clock::now() + std::chrono::seconds(10);
			}
			const XrCompositionLayerBaseHeader* layer = window.Render(frame.display_time, depth_view.NormalizedView(),
				depth_view.BackgroundView(), depth_view.GridWidth(), depth_view.GridHeight());
			if (layer && !debug_options.eye_panels)
				layers[layer_count++] = layer;
			for (int i = 0; layer && debug_options.eye_panels && i < 2; i++)
			{
				VKLibretro::Frame eye_image;
				if (window.GetEyeImage(i, &eye_image))
					eye_panels[i].Update(eye_image);
			}
		}
		if (debug_options.depth_panel && depth_panel.HasImage())
			layers[layer_count++] = depth_panel.Layer();
		for (ScreenLayer& panel : eye_panels)
		{
			if (debug_options.eye_panels && panel.HasImage())
				layers[layer_count++] = panel.Layer();
		}
		if (screen_placed)
			menu.AddLayers(layers, &layer_count);
		// The guns last, over everything: they are nearer than anything else.
		if (frame.should_render)
		{
			gun_renderer.Render(frame.display_time, guns_drawn, layers, &layer_count);
		}
		xr.EndFrame(frame, layers, layer_count);
		if (static int frames_logged = 0; frame.should_render && frames_logged < 3)
		{
			frames_logged++;
			Console.WriteLnFmt("Frame: {} layer(s), window has frame: {}", layer_count, window.HasFrame());
		}
	}

	Console.WriteLn("Shutting down.");
	settings.Update(true);
	web.Stop();
	Emu::RequestShutdown();
	gun_depth.Destroy();
	gun_renderer.Destroy();
	menu.Destroy();
	for (ScreenLayer& panel : eye_panels)
		panel.Destroy();
	window.Destroy();
	depth_view.Destroy();
	depth_panel.Destroy();
	screen_right.Destroy();
	screen.Destroy();
	xr.DestroySession();
	VKLibretro::AbortPacing();
	const int ret = Emu::Join();
	xr.DestroyInstance(VKLibretro::Init.device);
	VKLibretro::Active = false;
	VKLibretro::Shutdown();
	return ret;
}
