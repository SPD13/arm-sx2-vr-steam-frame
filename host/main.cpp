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
#include "Emu.h"
#include "Profile.h"
#include "ScreenLayer.h"
#include "Settings.h"
#include "XrApp.h"
#include "XrMath.h"
#include "capture/TrailerCapture.h"
#include "gun/GunAim.h"
#include "gun/GunDepth.h"
#include "gun/GunGames.h"
#include "gun/GunLight.h"
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
	int depth_log_ms = 5000; // interval of the "Stereo: eye" log lines
	bool no_menu_pause = false; // --no-menu-pause: the game keeps running while a menu is open
	bool test_eyes = false; // --test-eyes: eyes in front of an unplaced screen (off-head stereo tests)
	// --gun-script: shots gun 1 fires by itself, for testing without the headset.
	struct GunShot
	{
		float time = 0.0f; // seconds after the game started
		bool offscreen = false;
		float u = 0.5f, v = 0.5f; // where in the game's picture (0..1, top left origin)
		uint32_t button = 0; // a tap of these Emu::GunButton bits instead of a shot
	};
	std::vector<GunShot> gun_script;
	TrailerCapture::Options capture; // --capture: dir empty when off
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
			{"cal", Emu::GUN_RECALIBRATE}, {"up", Emu::GUN_UP}, {"down", Emu::GUN_DOWN}, {"left", Emu::GUN_LEFT},
			{"right", Emu::GUN_RIGHT}};
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
		"  --flat                 Start in flat 2D instead of volumetric 3D (the menu switches it live)\n"
		"  --volumetric-plus      Start in Volumetric+ (the scene at a fixed size, seen from where the eyes are)\n"
		"  --stereo-depth M       How far behind the screen the farthest geometry appears (default 3)\n"
		"  --stereo-conv F        Normalised Z on the screen plane (default 0.12)\n"
		"  --sdl-pad              Drive pad 1 from the first SDL gamepad (testing)\n"
		"  --load-state FILE      Start from a save state (SIGUSR1 saves debug/state.p2s, SIGUSR2 a snapshot)\n"
		"  --depth-trace          Log which colour/depth targets the 3D draws use\n"
		"  --depth-log-ms N       Log where the eyes are every N ms (default 5000)\n"
		"  --no-menu-pause        Keep the game running while a menu is open\n"
		"  --test-eyes            Off the head, eyes 64 mm apart at the screen distance (stereo dumps)\n"
		"  --gun-script SHOTS     Light gun 1 fires by itself: T:U:V shoots at U,V of the picture T s into\n"
		"                         the game, T:off reloads, T:a/b/c/start/up/down/left/right taps that button;\n"
		"                         comma separated, with the pedal held for shots from a second before the\n"
		"                         first (testing)\n"
		"  --capture DIR          Record what the headset shows for a trailer (tools/trailer): both eyes'\n"
		"                         pictures and depth, the head, the screen and the guns, into a new folder,\n"
		"                         between clicks of the record button beside the screen\n"
		"  --capture-now          Record from the start, without the button (scripted takes)\n"
		"  --capture-width N      Scale the captured pictures down to N pixels wide (default: as shown)\n"
		"  --capture-max-gb G     Stop capturing at G GB on disk (default 40)\n"
		"The screen and 3D options are saved settings too (VR.ini in the settings folder,\n"
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
			set("stereo", "false");
		else if (!std::strcmp(arg, "--volumetric-plus"))
		{
			set("stereo", "true");
			set("stereo_plus", "true");
		}
		else if (!std::strcmp(arg, "--stereo-depth") && has_value)
			set("stereo_depth", argv[++i]);
		else if (!std::strcmp(arg, "--stereo-conv") && has_value)
			set("stereo_convergence", argv[++i]);
		else if (!std::strcmp(arg, "--no-menu-pause"))
			debug->no_menu_pause = true;
		else if (!std::strcmp(arg, "--test-eyes"))
			debug->test_eyes = true;
		else if (!std::strcmp(arg, "--gun-script") && has_value)
		{
			if (!ParseGunScript(argv[++i], &debug->gun_script))
			{
				std::fprintf(stderr, "Bad --gun-script: %s\n", argv[i]);
				return false;
			}
		}
		else if (!std::strcmp(arg, "--capture") && has_value)
			debug->capture.dir = argv[++i];
		else if (!std::strcmp(arg, "--capture-now"))
			debug->capture.record_now = true;
		else if (!std::strcmp(arg, "--capture-width") && has_value)
			debug->capture.max_width = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
		else if (!std::strcmp(arg, "--capture-max-gb") && has_value)
			debug->capture.max_gb = std::max(0.1, std::atof(argv[++i]));
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

// Places the screen ahead of the head along the room's forward (the LOCAL space's -Z, which the
// runtime's own recenter sets), at eye height, facing back, whichever way the head looks.
static XrPosef ScreenPoseRoomForward(const XrVector3f& head, float distance)
{
	return {{0.0f, 0.0f, 0.0f, 1.0f}, {head.x, head.y, head.z - distance}};
}

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

// Volumetric: the GS draws both eyes (multiview), with the parallax driven by Z; Volumetric+ is the
// GS's portal. Flat: the GS draws one picture, as stock ARMSX2 does (none of the per-eye rendering),
// and switches between the two at the next vsync, rebuilding its targets (a short pause).
// The game's profile adds its own overlay rules.
static GSVolumetric::StereoSettings GSStereo(const VrSettings& vr, const GameProfile& profile = {})
{
	GSVolumetric::StereoSettings stereo;
	stereo.enabled = vr.stereo;
	stereo.multiview = true; // the GS device can draw both eyes, whichever mode it starts in
	stereo.z_mode = true;
	stereo.convergence = vr.stereo_convergence / profile.z_scale;
	stereo.portal = vr.stereo && vr.stereo_plus;
	stereo.fx_overlays = profile.fx_overlays;
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
	// Wi-Fi transfer, switched on from the menu: the BIOS folder, the games folder and the texture
	// packs. Zip, 7z and RAR archives are unpacked into them.
	WebServer web;
	using WebFolder = WebServer::Folder;
	web.SetFolders({
		{"bios", EmuFolders::Bios, {".bin", ".rom0", ".rom1", ".rom2", ".erom", ".mec", ".nvm"}, WebFolder::Kind::Files, true},
		{"roms", Emu::GetGamesFolder(), Emu::GetGameExtensions(), WebFolder::Kind::Files, true},
		{"textures", Emu::GetTexturesFolder(), {}, WebFolder::Kind::TexturePacks, true},
	});
	web.SetStagingFolder(Path::Combine(EmuFolders::DataRoot, "transfer"));
	web.SetTextureHooks({
		[]() {
			std::vector<WebServer::GameRef> games;
			for (const Emu::GameInfo& game : Emu::GetGames())
			{
				if (!game.serial.empty() && std::none_of(games.begin(), games.end(),
												[&](const WebServer::GameRef& g) { return g.serial == game.serial; }))
					games.push_back({game.serial, game.title});
			}
			return games;
		},
		&Emu::GetGameTitle,
		&Emu::GetSerial,
		&Emu::TexturePackChanged,
	});
	// On again if it was on when the app last closed.
	if (settings.Get().web_enabled)
		web.Start(settings.Get().web_require_code);

	// What the controllers do in the game, set in Settings › Controls.
	ControlMap controls;
	controls.Load(Path::Combine(EmuFolders::Settings, "Controls.ini"));

	// Light gun mode per game: whether the controllers are the guns or the DualShock 2.
	GunGames gun_games;
	gun_games.Load(Path::Combine(EmuFolders::Settings, "lightgun.ini"));
	// The guns plugged into the USB ports (Settings › Controls › GunCons Plugged, and one gun or
	// two), whatever light gun mode is: the game never sees them go when the controllers switch.
	// Read on the CPU thread as a game starts, so they are in before it looks for them.
	const auto guns_to_plug = [](const VrSettings& s) { return s.guns_plugged ? (s.gun_players > 1.5f ? 2 : 1) : 0; };
	std::atomic<int> guns_plugged{guns_to_plug(settings.Get())};
	Emu::SetGunsPolicy([&guns_plugged](const std::string&) { return guns_plugged.load(); });

	const VrSettings& vr = settings.Get(); // the current settings, as changed while running

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
	GSVolumetric::SetEnabled(false); // depth, only for the light guns (see apply_gun_depth)
	GSVolumetric::SetStereo(GSStereo(vr));
	Console.WriteLnFmt("3D: starts {}, depth {} m, convergence {}", vr.Mode3DName(), vr.stereo_depth,
		vr.stereo_convergence);

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
	GunLight gun_light;
	TrailerCapture capture;
	ScreenLayer screen;
	ScreenLayer screen_right; // volumetric: the right eye's picture; screen then shows the left eye's
	VrMenu menu(settings, web, controls);
	const VkDevice device = gs->GetDevice();
	const VkQueue queue = gs->GetGraphicsQueue();
	const uint32_t queue_family = gs->GetGraphicsQueueFamilyIndex();
	if (!xr.CreateSession(device, queue_family) || !screen.Create(xr, device, queue, queue_family) ||
		!screen_right.Create(xr, device, queue, queue_family))
	{
		Emu::RequestShutdown();
		menu.Destroy();
		screen_right.Destroy();
		screen.Destroy();
		xr.DestroySession();
		VKLibretro::AbortPacing();
		Emu::Join();
		return EXIT_FAILURE;
	}
	if (!debug_options.capture.dir.empty())
		menu.EnableRecordButton();
	if (!menu.Create(xr, device, gs->GetPhysicalDevice(), queue, queue_family))
		Console.Error("The settings menu is unavailable.");
	menu.SetPausesGame(!debug_options.no_menu_pause);
	// The gun models by VrSettings::gun_model: the PS1 GunCon, the GunCon2.
	const std::string assets = Path::Combine(EmuFolders::AppRoot, "assets");
	if (!gun_renderer.Create(xr, device, gs->GetPhysicalDevice(), queue, queue_family,
			{Path::Combine(assets, "lightgun.glb"), Path::Combine(assets, "lightgun2.glb")}))
		Console.Error("Light gun: neither guns nor controllers can be shown.");
	else if (!gun_renderer.HasGun(0) && !gun_renderer.HasGun(1))
		Console.Error("Light gun: no gun model; the guns are the VR controllers.");
	if (!gun_light.Create(device, gs->GetPhysicalDevice(), queue, queue_family))
		Console.Error("Light gun: no light sensor; calibration needs the calibration shot.");
	if (!gun_depth.Create(device, gs->GetPhysicalDevice(), queue, queue_family))
		Console.Error("Light gun: no depth; shots land on the screen glass.");
	if (!debug_options.capture.dir.empty() &&
		!capture.Create(device, gs->GetPhysicalDevice(), queue, queue_family, debug_options.capture))
		Console.Error("Capture: off.");
	menu.SetRecording(capture.Recording());
	TrailerCapture::View capture_view; // what the GS draws the next frames with, for the capture
	// An invisible quad for the layer slots that are empty this frame, so that every layer keeps its
	// place in the layer list. SteamVR shows quad layers as overlays matched by their place in the
	// list: when a menu opened or closed and layers moved in the list, the menu and the guns were
	// shown for a frame where other layers had been. It is part of the menu's image (a swapchain of
	// its own would count against SteamVR's 16).
	std::array<XrCompositionLayerQuad, 24> pads{}; // one per layer slot (see below)
	screen_right.SetEyeVisibility(XR_EYE_VISIBILITY_RIGHT);

	using Clock = std::chrono::steady_clock;
	Clock::time_point next_eye_log = Clock::now() + std::chrono::seconds(5);
	Clock::time_point next_grid_log{}; // --depth-trace: the guns' depth grid

	GameProfile game_profile; // the running game's (profiles/<serial>.ini)
	// Hands the settings to what keeps a copy of them.
	const auto apply_settings = [&] {
		screen.SetWidth(vr.screen_width);
		screen_right.SetWidth(vr.screen_width);
		// The GS thread reads these per draw: a change can land in the middle of a frame.
		GSVolumetric::SetStereo(GSStereo(vr, game_profile));
	};
	apply_settings();
	VrSettings applied = vr;
	unsigned applied_version = settings.Version();

	bool screen_placed = false; // shown, once placed the first time
	// The screen waits to be placed until the head is tracked live (see the frame loop).
	bool place_pending = true;
	const char* place_reason = "first";
	bool place_facing_head = false; // a recenter: in front of the head, not along the room's forward
	XrTime place_not_before = 0; // a moved LOCAL space: not before the move
	std::optional<XrPosef> last_head; // the head pose of the frame before, while placing
	int frozen_head_frames = 0;
	XrPosef screen_pose_now{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -vr.screen_distance}};
	const auto place_screen = [&](const XrPosef& pose) {
		screen_pose_now = pose;
		screen.SetPose(pose);
		screen_right.SetPose(pose);
	};

	std::string profile_serial;
	// When one eye's frames stop coming (a movie shown at half the frame rate lands on the
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
	// The guns' buttons as drawn, per gun: each eased toward pressed (1) or not (0), the D-pad toward -1..1.
	struct GunPresses
	{
		float a = 0.0f, b = 0.0f, c = 0.0f, select = 0.0f, start = 0.0f, dpad_x = 0.0f, dpad_y = 0.0f;
	};
	std::array<GunPresses, 2> gun_presses{};
	std::array<bool, 2> gun_fired{}; // the trigger was down last frame, per gun (recoil on a new pull)
	// Where the guns' shots landed lately, with the scene's depth there, for the effects games draw
	// over them (GSVolumetric::SetShotAnchors).
	struct ShotMark
	{
		GSVolumetric::ShotAnchor anchor;
		Clock::time_point time;
	};
	std::vector<ShotMark> shot_marks;
	// The emulator's frames so far, and how many there were when the depth grid last changed: the
	// scene's depth for fx_overlays has to be from the last few frames.
	u64 emu_frames = 0, scene_grid_frame = 0;
	Clock::time_point scene_grid_time{};
	bool gun_saw_dark = false; // the light sensor's last reading, to log changes
	// Per gun: when it last shot (trigger or calibration shot), and how often the game set its
	// calibration, to tell the player when a shot calibrated it.
	std::array<Clock::time_point, 2> gun_shot_at{};
	std::array<bool, 2> gun_shot_down{};
	std::array<unsigned, 2> gun_calibrations{};
	std::optional<SettingsMenu::GunAction> gun_action; // asked in the menu, done once it has closed
	// Per hand: where the grip is in the aim pose's frame, from the last frame that had both (for a
	// frame without a grip pose), and the gun's last pose (to log jumps).
	std::array<XrVector3f, 2> grip_in_aim{{{0.0f, -0.06f, 0.08f}, {0.0f, -0.06f, 0.08f}}};
	std::array<std::optional<XrPosef>, 2> last_gun_pose;
	int pose_logs = 0; // rate limit
	bool ui_open_last = false;
	int frames_copied_by_gs = 0, frames_copied_here = 0; // the present copy, for the pacing log
	std::array<bool, 2> beam_on_ui{}; // per hand, last frame (to log changes)
	Clock::time_point gun_action_at{}, gun_action_until{};
	Clock::time_point gun_script_start{}; // --gun-script: when the game started
	size_t gun_script_next = 0;
	int gun_script_logged = -1;
	// The presented frame, as the light guns aim at it.
	float frame_size[2] = {1280.0f, 960.0f};
	float frame_draw_rect[4] = {0.0f, 0.0f, 1280.0f, 960.0f};
	// Depth for the guns, in volumetric mode: only while light gun mode is on (it costs copies on the GS),
	// while the trailer capture records it, or for a game whose effects are placed by it (fx_overlays).
	const auto apply_gun_depth = [&] {
		GSVolumetric::SetEnabled((gun_mode || capture.Active() || game_profile.fx_overlays) && vr.stereo);
		// The capture's HUD mask reads the depth at vsync (the depth itself is from before the HUD).
		GSVolumetric::SetDepthExtras(capture.Active() && vr.stereo);
	};
	// Only the controls switch: the guns stay plugged in (idle while the controllers are the pad).
	const auto set_gun_mode = [&](bool on) {
		if (on == gun_mode)
			return;
		gun_mode = on;
		apply_gun_depth();
		Console.WriteLnFmt("Light gun: mode {} for {}{}", on ? "on" : "off", profile_serial,
			on && guns_plugged.load() == 0 ? " (no GunCon plugged in)" : "");
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
			apply_gun_depth();
			if (vr.Mode3D() != applied.Mode3D())
				Console.WriteLnFmt("3D: {}", vr.Mode3DName());
			if (vr.passthrough != applied.passthrough)
				Console.WriteLnFmt("Passthrough: {}", vr.passthrough ? "on" : "off");
			// A new distance moves the screen along its axis; a new width moves the panels beside it.
			if (screen_placed && (vr.screen_distance != applied.screen_distance || vr.screen_width != applied.screen_width))
				place_screen(PushScreen(screen_pose_now, vr.screen_distance - applied.screen_distance));
			// GunCons Plugged, one gun or two: plug them in or out, in the running game too.
			if (const int guns = guns_to_plug(vr); guns != guns_plugged.load())
			{
				guns_plugged.store(guns);
				Emu::SetGunsPlugged(guns);
			}
			applied = vr;
		}

		// The game's profile, once its serial is known.
		if (const std::string serial = Emu::GetSerial(); serial != profile_serial)
		{
			profile_serial = serial;
			game_profile = LoadGameProfile(serial);
			GSVolumetric::SetStereo(GSStereo(vr, game_profile));
			menu.OnGameChanged();
			web.SetProtectedFile(Emu::GetRunningGamePath());
			// The CPU thread plugged the guns in as the game started (Emu::SetGunsPolicy).
			gun_mode = !serial.empty() && gun_games.IsOn(serial);
			gun_chosen = gun_games.IsChosen(serial);
			apply_gun_depth();
			gun_script_start = serial.empty() ? Clock::time_point{} : Clock::now();
			gun_script_next = 0;
			gun_script_logged = -1;
			capture.RecordEvent("game", serial.empty() ? std::string() : serial + " " + Emu::GetGameTitle(serial));
			if (!serial.empty())
				Console.WriteLnFmt("Light gun: mode {} for {}{}", gun_mode ? "on" : "off", serial, gun_chosen ? " (chosen)" : "");
		}

		// Frame timing, logged when a frame is slow: where the XR thread spent its time.
		const Clock::time_point t_start = Clock::now();
		const XrApp::Frame frame = xr.BeginFrame();
		const Clock::time_point t_begun = Clock::now();
		const XrApp::Input input = xr.ReadInput();

		// Along the room's forward when the screen first appears and when the runtime moves the LOCAL
		// space (its own recenter); in front of the head on request.
		const auto request_placement = [&](const char* reason, bool facing_head) {
			place_pending = true;
			place_reason = reason;
			place_facing_head = facing_head;
			last_head.reset();
			frozen_head_frames = 0;
		};
		if (const XrTime change = xr.TakeLocalSpaceChange())
		{
			request_placement("LOCAL space moved", false);
			place_not_before = change;
		}
		if (pad_recenter || menu_recenter)
			request_placement("recenter", true);
		menu_recenter = pad_recenter = false;
		// Only from a head the runtime tracks live. At start it reports a valid pose that is the last
		// one it knew (on the Frame: the same pose to the bit for the first second, the headset
		// pitched 80 degrees), and the screen placed from that faces anywhere. A tracked head moves
		// a little from one predicted frame to the next; a frozen pose does not. Until the session has
		// focus the headset may not be worn either.
		XrPosef head;
		bool head_tracked = false;
		if (place_pending && xr.IsFocused() && frame.display_time >= place_not_before &&
			xr.LocateHead(frame.display_time, &head, &head_tracked))
		{
			const bool live = head_tracked && last_head && std::memcmp(&head, &*last_head, sizeof(head)) != 0;
			last_head = head;
			// Not live after 3 s or so: placed anyway, from the LOCAL origin (at eye height where the
			// runtime last recentered) or the head as it is.
			if (!live && ++frozen_head_frames == 270)
				Console.WarningFmt("Screen: the head is not tracked live (tracked {}), placed anyway", head_tracked);
			if (live || frozen_head_frames >= 270)
			{
				if (place_facing_head)
					place_screen(ScreenPoseInFrontOf(head, vr.screen_distance));
				else
					place_screen(ScreenPoseRoomForward(live ? head.position : XrVector3f{}, vr.screen_distance));
				Console.WriteLnFmt("Screen placed ({}, {}): head ({:.3f} {:.3f} {:.3f}) q ({:.3f} {:.3f} {:.3f} {:.3f}), after {} frame(s)",
					place_reason, place_facing_head ? "facing the head" : "room forward", head.position.x,
					head.position.y, head.position.z, head.orientation.x, head.orientation.y, head.orientation.z,
					head.orientation.w, frozen_head_frames + 1);
				screen_placed = true;
				place_pending = false;
				menu.Reanchor();
			}
		}

		// The light guns: which hands hold one, and where the guns are. Gun 1 is in the gun hand,
		// gun 2 (with two guns) in the other.
		const int num_guns = gun_mode ? (vr.gun_players > 1.5f ? 2 : 1) : 0;
		const int first_hand = vr.gun_left_hand ? 0 : 1;
		std::array<int, 2> gun_of_hand = {-1, -1};
		for (int g = 0; g < num_guns; g++)
			gun_of_hand[g == 0 ? first_hand : 1 - first_hand] = g;
		// What the guns look like (Settings > Light Gun): one of the models, or none (-1) when the
		// choice is the VR controller or its model is missing. Without a model, the controller is the
		// gun: its beam starts on the controller's pointing ray.
		const int gun_choice = std::clamp(static_cast<int>(vr.gun_model + 0.5f), 0, 2);
		const int gun_model = gun_renderer.HasGun(gun_choice) ? gun_choice : -1;
		std::array<bool, 2> aim_valid{};
		std::array<XrPosef, 2> aim_pose{}, gun_pose{}, muzzle_pose{};
		for (int h = 0; h < 2; h++)
		{
			aim_valid[h] = xr.LocateAim(h, frame.display_time, &aim_pose[h]);
			if (gun_of_hand[h] < 0)
				continue;
			if (!aim_valid[h])
			{
				if (pose_logs < 200 && last_gun_pose[h])
				{
					pose_logs++;
					Console.WarningFmt("Light gun: no aim pose for hand {} this frame", h);
				}
				last_gun_pose[h].reset();
				continue;
			}
			// The barrel along the controller's pointing ray (tilted by the gun angle), held at the
			// grip; without a grip pose, where the grip was last seen relative to the ray.
			gun_pose[h].orientation = aim_pose[h].orientation *
			                          QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, vr.gun_pitch * 3.14159265f / 180.0f);
			XrPosef grip;
			if (xr.LocateGrip(h, frame.display_time, &grip))
			{
				gun_pose[h].position = grip.position;
				grip_in_aim[h] = Rotate(Conjugate(aim_pose[h].orientation), grip.position - aim_pose[h].position);
			}
			else
			{
				gun_pose[h].position = Transform(aim_pose[h], grip_in_aim[h]);
				if (pose_logs < 200)
				{
					pose_logs++;
					Console.WarningFmt("Light gun: no grip pose for hand {} this frame", h);
				}
			}
			muzzle_pose[h] = gun_model >= 0 ? Compose(gun_pose[h], gun_renderer.Model(gun_model).Muzzle()) :
			                                  XrPosef{gun_pose[h].orientation, aim_pose[h].position};
			// A jump from one frame to the next: log it, to tell a pose glitch from a rendering one.
			if (last_gun_pose[h] && pose_logs < 200)
			{
				const XrVector3f moved = gun_pose[h].position - last_gun_pose[h]->position;
				const XrQuaternionf turn = Conjugate(last_gun_pose[h]->orientation) * gun_pose[h].orientation;
				const float degrees = 2.0f * std::acos(std::min(std::abs(turn.w), 1.0f)) * 57.29578f;
				const float meters = std::sqrt(Dot(moved, moved));
				if (meters > 0.08f || degrees > 25.0f)
				{
					pose_logs++;
					Console.WarningFmt("Light gun: hand {} jumped {:.3f} m, {:.0f} deg in a frame{}", h, meters, degrees,
						ui_open_last ? " (menu open)" : "");
				}
			}
			last_gun_pose[h] = gun_pose[h];
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
			ui_open_last = ui.open;
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
				if (gun_mode && guns_plugged.load() == 0)
					menu.ShowNotice("No GunCon plugged in (Settings › Controls)", 3.0f);
			}
			if (ui.toggle_guns)
			{
				VrSettings changed = vr;
				changed.gun_players = vr.gun_players > 1.5f ? 1.0f : 2.0f;
				settings.Set(changed);
			}
			if (ui.toggle_record && capture.Active())
			{
				const bool start = !capture.Recording();
				const double seconds = capture.SetRecording(start);
				menu.SetRecording(start);
				menu.ShowNotice(start ? std::string("Recording") : fmt::format("Recorded {:.0f} s", seconds), 2.5f);
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
			// The guns put down: plugged in, off the screen, nothing pressed.
			for (int g = 0; g < Emu::NUM_GUNS; g++)
				Emu::SetGun(g, {});
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

			// Where the guns aim: the screen as shown, and in volumetric mode the depth they shoot into.
			GunScreen gun_screen;
			gun_screen.pose = screen_pose_now;
			gun_screen.width = vr.screen_width;
			gun_screen.height = vr.screen_width / screen_aspect;
			std::copy(std::begin(frame_draw_rect), std::end(frame_draw_rect), gun_screen.draw_rect);
			gun_screen.view[2] = frame_size[0];
			gun_screen.view[3] = frame_size[1];
			GunStereo gun_stereo;
			const GunDepthGrid* depth_grid = nullptr;
			XrView eye_views[2];
			if (vr.stereo && xr.LocateEyes(frame.display_time, eye_views))
			{
				gun_stereo.z_mode = true;
				gun_stereo.convergence = vr.stereo_convergence / game_profile.z_scale;
				gun_stereo.depth = vr.stereo_depth;
				gun_stereo.portal = vr.stereo_plus;
				gun_stereo.portal_distance = vr.screen_distance;
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

			// The guns' light sensor: a dark picture is no light, as a real gun reports it (off the screen).
			// Time Crisis's calibration waits for that after the calibration shot.
			const GunLight::Reading& light = gun_light.Latest();
			const bool dark = light.valid && light.dark;
			if (dark != gun_saw_dark && pose_logs < 200)
			{
				pose_logs++;
				Console.WriteLnFmt("Light gun: picture {} ({:.1f}% lit, brightest {:.2f})", dark ? "dark: no light" : "lit again",
					light.lit_share * 100.0f, light.brightest);
			}
			gun_saw_dark = dark;

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
					state.aim_valid = aim.on_picture && !on_ui && !dark;
					state.x = aim.window_x;
					state.y = aim.window_y;

					drawn.visible = gun_renderer.IsCreated();
					drawn.model = gun_model;
					drawn.pose = gun_pose[h];
					drawn.muzzle = muzzle_pose[h];
					drawn.body_color = g == 1 ? vr.gun2_color : num_guns > 1 ? vr.gun_color_2p : vr.gun_color;
					drawn.trigger = controls.GunTrigger(input.controller, g);
					GunPresses& p = gun_presses[g];
					const auto ease = [](float& value, float target) {
						value += (target - value) * 0.35f; // per frame, about 40 ms to settle at 90 Hz
					};
					const auto held = [&state](uint32_t button) { return (state.buttons & button) ? 1.0f : 0.0f; };
					ease(p.a, held(Emu::GUN_A));
					ease(p.b, held(Emu::GUN_B));
					ease(p.c, held(Emu::GUN_C));
					ease(p.select, held(Emu::GUN_SELECT));
					ease(p.start, held(Emu::GUN_START));
					ease(p.dpad_x, held(Emu::GUN_RIGHT) - held(Emu::GUN_LEFT));
					ease(p.dpad_y, held(Emu::GUN_UP) - held(Emu::GUN_DOWN));
					drawn.press_left = p.a;
					drawn.press_right = p.b;
					drawn.press_c = p.c;
					drawn.press_select = p.select;
					drawn.press_start = p.start;
					drawn.dpad_x = p.dpad_x;
					drawn.dpad_y = p.dpad_y;
					drawn.beam_color = g == 0 ? 0xFF3A30 : 0x3A8CFF;
					const int beam_mode = std::clamp(static_cast<int>(vr.gun_beam + 0.5f), 0, 2);
					if (ui.on_ui[h] != beam_on_ui[h] && pose_logs < 200)
					{
						pose_logs++;
						Console.WriteLnFmt("Light gun: hand {} beam {} the menu{}", h, ui.on_ui[h] ? "onto" : "off",
							ui.on_ui[h] ? "" : fmt::format(" (now {})", aim.on_screen ? (aim.through_depth ? "in the window" : "on the glass") :
							                                           aim.on_plane ? "beside the screen" : "away"));
					}
					beam_on_ui[h] = ui.on_ui[h];
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
					state.aim_valid = !shot.offscreen && !dark;
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

				// While the trigger is down (a machine gun keeps firing), where the shots land and the
				// scene's depth there: the surface the beam reached, or the depth grid at that point.
				const float* pr = frame_draw_rect;
				if (vr.stereo && game_profile.shot_overlays && (state.buttons & Emu::GUN_TRIGGER) && state.aim_valid &&
					pr[2] > pr[0] && pr[3] > pr[1])
				{
					GSVolumetric::ShotAnchor anchor;
					anchor.u = (state.x - pr[0]) / (pr[2] - pr[0]);
					anchor.v = (state.y - pr[1]) / (pr[3] - pr[1]);
					anchor.znorm = aim.through_depth ? aim.znorm : -1.0f;
					if (anchor.znorm < 0.0f)
					{
						const GunDepthGrid* grid = gun_depth.Latest();
						if (grid && now - grid->time < std::chrono::milliseconds(300))
							anchor.znorm = grid->Sample(state.x / frame_size[0], state.y / frame_size[1]);
					}
					if (anchor.znorm >= 0.0f)
					{
						auto same = std::find_if(shot_marks.begin(), shot_marks.end(), [&](const ShotMark& m) {
							return std::abs(m.anchor.u - anchor.u) < 0.02f && std::abs(m.anchor.v - anchor.v) < 0.02f;
						});
						if (same == shot_marks.end())
						{
							if (shot_marks.size() >= GSVolumetric::MAX_SHOT_ANCHORS)
								shot_marks.erase(shot_marks.begin());
							shot_marks.push_back({anchor, now});
						}
						else
						{
							*same = {anchor, now};
						}
					}
				}

				// The game sets the gun's calibration (its offset) right after a calibration shot; it
				// also does at start, without one.
				const bool shooting = (state.buttons & (Emu::GUN_TRIGGER | Emu::GUN_RECALIBRATE)) != 0;
				if (shooting && !gun_shot_down[g])
					gun_shot_at[g] = now;
				gun_shot_down[g] = shooting;
				int offset_x = 0, offset_y = 0;
				if (const unsigned calibrations = Emu::GetGunCalibration(g, &offset_x, &offset_y);
					calibrations != gun_calibrations[g])
				{
					gun_calibrations[g] = calibrations;
					if (now - gun_shot_at[g] < std::chrono::milliseconds(1500))
					{
						Console.WriteLnFmt("Light gun: gun {} calibrated by the game (offset {}, {})", g + 1, offset_x, offset_y);
						menu.ShowNotice(num_guns > 1 ? fmt::format("Gun {} calibrated", g + 1) : std::string("Gun calibrated"), 3.0f);
					}
				}
			}
		}

		// A shot's effects last a moment after the trigger is let go (Crisis Zone's bursts ~0.5 s).
		{
			const Clock::time_point now = Clock::now();
			std::erase_if(shot_marks, [&](const ShotMark& m) {
				return !gun_mode || !vr.stereo || now - m.time > std::chrono::milliseconds(1000);
			});
			std::array<GSVolumetric::ShotAnchor, GSVolumetric::MAX_SHOT_ANCHORS> anchors;
			for (size_t i = 0; i < shot_marks.size(); i++)
				anchors[i] = shot_marks[i].anchor;
			GSVolumetric::SetShotAnchors(anchors.data(), shot_marks.size());
		}
		// The game's effects drawn as 2D overlays anywhere (fx_overlays): the scene's depth over the
		// picture, from the depth grid, as long as it is from the last few frames (counted in frames: a
		// slow frame leaves it as right as it was).
		{
			const GunDepthGrid* grid = vr.stereo && game_profile.fx_overlays ? gun_depth.Latest() : nullptr;
			if (grid && grid->time != scene_grid_time)
			{
				scene_grid_time = grid->time;
				scene_grid_frame = emu_frames;
			}
			const float* pr = frame_draw_rect;
			if (grid && emu_frames - scene_grid_frame <= 4 && pr[2] > pr[0] && pr[3] > pr[1])
			{
				constexpr u32 W = GSVolumetric::SCENE_DEPTH_WIDTH, H = GSVolumetric::SCENE_DEPTH_HEIGHT;
				std::array<float, W * H> scene;
				for (u32 y = 0; y < H; y++)
				{
					const float v = (pr[1] + (y + 0.5f) / H * (pr[3] - pr[1])) / frame_size[1];
					for (u32 x = 0; x < W; x++)
						scene[y * W + x] = grid->Sample((pr[0] + (x + 0.5f) / W * (pr[2] - pr[0])) / frame_size[0], v);
				}
				GSVolumetric::SetSceneDepth(scene.data());
			}
			else
			{
				GSVolumetric::SetSceneDepth(nullptr);
			}
		}

		// Where each eye is in front of the screen, for the GS's window parallax; none in flat mode.
		capture_view.mode = static_cast<CaptureFormat::Mode>(vr.Mode3D());
		capture_view.convergence = vr.stereo_convergence / game_profile.z_scale;
		capture_view.stereo_depth = vr.stereo_depth;
		capture_view.screen_width = vr.screen_width;
		capture_view.screen_height = vr.screen_width / screen_aspect;
		capture_view.screen_distance = vr.screen_distance;
		if (!vr.stereo)
		{
			for (int i = 0; i < 2; i++)
			{
				GSVolumetric::SetEyeView(i, 0.0f, 0.0f, 0.0f);
				capture_view.eye[i][3] = 0.0f;
				capture_view.distance_ratio[i] = 1.0f;
			}
		}
		else if (screen_placed)
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
					// Far geometry follows the eye on the screen (NDC y points down). Volumetric+: the
					// farthest is at infinity, and the eye's distance over the screen distance scales
					// the scene for leaning in or out.
					const float depth_factor = vr.stereo_plus ? 1.0f : vr.stereo_depth / (distance + vr.stereo_depth);
					const float distance_ratio = vr.stereo_plus ? distance / std::max(vr.screen_distance, 0.1f) : 1.0f;
					GSVolumetric::SetEyeView(i, lx / half_w, -dy / half_h, depth_factor, distance_ratio);
					capture_view.eye[i][0] = lx;
					capture_view.eye[i][1] = dy;
					capture_view.eye[i][2] = distance;
					capture_view.eye[i][3] = depth_factor;
					capture_view.distance_ratio[i] = distance_ratio;
					if (log_eyes)
						Console.WriteLnFmt("Stereo: eye {} at ({:.3f}, {:.3f}) m, {:.2f} m from the screen -> offset ({:.3f}, {:.3f}) NDC, depth factor {:.3f}, distance ratio {:.3f}",
							i, lx, dy, distance, lx / half_w, -dy / half_h, depth_factor, distance_ratio);
				}
			}
		}
		else if (debug_options.test_eyes)
		{
			// The screen is never placed off the head: eyes 64 mm apart, centred, at the screen distance.
			const float distance = std::max(vr.screen_distance, 0.1f);
			for (int i = 0; i < 2; i++)
			{
				const float lx = i == 0 ? -0.032f : 0.032f;
				const float depth_factor = vr.stereo_plus ? 1.0f : vr.stereo_depth / (distance + vr.stereo_depth);
				GSVolumetric::SetEyeView(i, lx / (0.5f * vr.screen_width), 0.0f, depth_factor, 1.0f);
			}
		}

		const Clock::time_point t_input_done = Clock::now();
		VKLibretro::Frame emu_frame;
		if (VKLibretro::ConsumeFrame(&emu_frame))
		{
			emu_frames++;
			if (emu_frame.width && emu_frame.height)
			{
				const float aspect = static_cast<float>(emu_frame.width) / static_cast<float>(emu_frame.height);
				if (std::abs(aspect - screen_aspect) > 0.001f && pose_logs < 200)
				{
					pose_logs++;
					Console.WriteLnFmt("Screen: aspect {:.3f} -> {:.3f} ({}x{})", screen_aspect, aspect, emu_frame.width,
						emu_frame.height);
				}
				screen_aspect = aspect;
				frame_size[0] = static_cast<float>(emu_frame.width);
				frame_size[1] = static_cast<float>(emu_frame.height);
				const float* r = emu_frame.draw_rect;
				const bool has_rect = r[2] > r[0] && r[3] > r[1];
				frame_draw_rect[0] = has_rect ? r[0] : 0.0f;
				frame_draw_rect[1] = has_rect ? r[1] : 0.0f;
				frame_draw_rect[2] = has_rect ? r[2] : frame_size[0];
				frame_draw_rect[3] = has_rect ? r[3] : frame_size[1];
			}
			// The guns' depth, in volumetric mode (with multiview, a layer per eye).
			if (vr.stereo && (gun_mode || game_profile.fx_overlays) && emu_frame.depth_image)
			{
				gun_depth.Process(emu_frame, emu_frame.right_image != VK_NULL_HANDLE);
				// --depth-trace: what the guns' depth grid holds, every few seconds.
				const GunDepthGrid* grid = GSVolumetric::IsTraceEnabled() ? gun_depth.Latest() : nullptr;
				if (grid && Clock::now() >= next_grid_log)
				{
					next_grid_log = Clock::now() + std::chrono::seconds(3);
					std::vector<float> z;
					for (const float v : grid->znorm)
						if (v >= 0.0f)
							z.push_back(v);
					std::sort(z.begin(), z.end());
					const auto at = [&z](float q) { return z.empty() ? -1.0f : z[static_cast<size_t>(q * (z.size() - 1))]; };
					const float convergence = vr.stereo_convergence / game_profile.z_scale;
					const size_t near = static_cast<size_t>(std::count_if(z.begin(), z.end(), [&](float v) { return v >= convergence; }));
					Console.WriteLnFmt("Light gun: depth grid {} cells, Znorm min {:.4f} p10 {:.4f} median {:.4f} p90 {:.4f} max {:.4f}, "
						"{} at or nearer than the glass ({}), depth scale {:.1f}", z.size(), at(0.0f), at(0.1f), at(0.5f), at(0.9f),
						at(1.0f), near, convergence, emu_frame.depth_scale);
				}
			}
			// The light gun's sensor reads the picture. Drawn straight into the screen's armed image
			// (no view of its own in the frame), it is read there, before that image is released.
			if (gun_mode)
			{
				VKLibretro::Frame light_frame = emu_frame;
				if (!light_frame.view && emu_frame.copied_left && emu_frame.copied_left == screen.ArmedId() &&
					screen.ArmedReadable())
					light_frame.view = screen.ArmedView();
				gun_light.Process(light_frame);
			}
			// The trailer capture reads both eyes' pictures as the screens got them, before they are released.
			if (capture.Active())
			{
				VkImageView left = emu_frame.view;
				if (!left && emu_frame.copied_left && emu_frame.copied_left == screen.ArmedId() && screen.ArmedReadable())
					left = screen.ArmedView();
				VkImageView right = VK_NULL_HANDLE;
				if (vr.stereo && emu_frame.right_image)
				{
					right = emu_frame.right_view;
					if (!right && emu_frame.copied_right && emu_frame.copied_right == screen_right.ArmedId() &&
						screen_right.ArmedReadable())
						right = screen_right.ArmedView();
				}
				capture.Process(emu_frame, left, right, capture_view);
			}
			if (emu_frame.right_image)
			{
				// Multiview: both eyes in one frame, drawn or copied by the GS into the images armed
				// below, or copied here when it did not. Flat mode shows the left picture to both eyes.
				const bool left_copied = screen.ReleaseArmed(emu_frame.copied_left);
				if (!left_copied)
					screen.Update(emu_frame);
				(left_copied ? frames_copied_by_gs : frames_copied_here)++;
				if (!screen_right.ReleaseArmed(emu_frame.copied_right) && vr.stereo)
				{
					VKLibretro::Frame right_frame = emu_frame;
					right_frame.image = emu_frame.right_image;
					right_frame.view = emu_frame.right_view;
					screen_right.Update(right_frame);
				}
				last_eye_frame[0] = last_eye_frame[1] = Clock::now();
			}
			else if (emu_frame.stereo_eye == 1)
				screen_right.Update(emu_frame);
			else if (!screen.ReleaseArmed(emu_frame.copied_left))
				screen.Update(emu_frame);
			if (emu_frame.stereo_eye >= 0)
				last_eye_frame[emu_frame.stereo_eye] = Clock::now();
			else if (!emu_frame.right_image)
				last_eye_frame[0] = Clock::now(); // flat: one picture, in screen (not a stale right eye)
		}

		// The next frame's copies, done by the GS at the end of its present (ScreenLayer::Arm).
		// $PS2V_NO_PRESENT_COPY keeps them here instead, for a measurement.
		static const bool present_copy = !std::getenv("PS2V_NO_PRESENT_COPY");
		if (present_copy)
		{
			screen.Arm(0, gun_mode || capture.Active()); // readable for the light gun's sensor and the capture
			if (vr.stereo)
				screen_right.Arm(1, capture.Active());
		}

		const Clock::time_point t_emu_done = Clock::now();
		// Every layer has a slot of its own, always at the same place in the list, with an invisible
		// quad standing in when it is hidden (see pads).
		enum Slot
		{
			SLOT_BACK, // passthrough's transparent projection layer, behind everything (XrApp::BackLayer)
			SLOT_SCREEN, // the screen: the left eye's (volumetric) or both eyes'
			SLOT_SCREEN_RIGHT, // volumetric: the right eye's
			SLOT_MENU, // VrMenu::NUM_LAYER_SLOTS of them
			SLOT_GUNS = SLOT_MENU + VrMenu::NUM_LAYER_SLOTS, // GunRenderer::NUM_LAYERS of them, last: over everything
			NUM_SLOTS = SLOT_GUNS + GunRenderer::NUM_LAYERS
		};
		static_assert(NUM_SLOTS <= std::tuple_size_v<decltype(pads)>, "a pad per slot");
		std::array<const XrCompositionLayerBaseHeader*, NUM_SLOTS> slots{};
		// Passthrough: SteamVR shows the room only behind a projection layer, whose image is the menu's
		// invisible pad. It covers both eyes' whole view, which the compositor pays for every frame, so
		// it is only there with passthrough on; off, the slot's pad quad keeps the other layers in place.
		if (XrCompositionLayerQuad clear; vr.passthrough && xr.SupportsPassthrough() && menu.GetPadLayer(&clear))
			slots[SLOT_BACK] = xr.BackLayer(frame.display_time, clear.subImage);
		{
			// Volumetric: each eye its own picture. Flat (no parallax: both are the same), or while one
			// eye's frames have stopped: one picture for both eyes.
			const Clock::time_point now = Clock::now();
			const bool left_live = screen.HasImage() && now - last_eye_frame[0] < std::chrono::milliseconds(150);
			const bool right_live = screen_right.HasImage() && now - last_eye_frame[1] < std::chrono::milliseconds(150);
			if (vr.stereo && left_live && right_live)
			{
				screen.SetEyeVisibility(XR_EYE_VISIBILITY_LEFT);
				screen_right.SetEyeVisibility(XR_EYE_VISIBILITY_RIGHT);
				slots[SLOT_SCREEN] = screen.Layer();
				slots[SLOT_SCREEN_RIGHT] = screen_right.Layer();
			}
			else if (ScreenLayer* only = left_live || !right_live ? &screen : &screen_right; only->HasImage())
			{
				only->SetEyeVisibility(XR_EYE_VISIBILITY_BOTH);
				slots[SLOT_SCREEN] = only->Layer();
			}
		}
		if (screen_placed)
			menu.GetLayers(&slots[SLOT_MENU]);
		// The controllers, as SteamVR draws them: in the hands without a gun (Settings > Controls), and
		// as the guns when they have no model (Settings > Light Gun).
		std::array<GunRenderer::Hand, 2> hands_drawn;
		if (xr.HasFrameControllers())
		{
			for (int h = 0; h < 2; h++)
			{
				const bool shown = gun_of_hand[h] >= 0 ? gun_model < 0 : vr.show_controllers;
				if (!shown || !gun_renderer.HasController(h))
					continue;
				const ControllerModel& model = gun_renderer.Controller(h);
				XrPosef grip;
				if (xr.LocateGrip(h, frame.display_time, &grip))
					hands_drawn[h] = {true, model.FromGrip(grip)};
				else if (aim_valid[h])
					hands_drawn[h] = {true, model.FromAim(aim_pose[h])};
			}
		}
		if (frame.should_render)
			gun_renderer.Render(frame.display_time, guns_drawn, hands_drawn, input.controller, &slots[SLOT_GUNS]);
		if (capture.Active())
			capture.Poll();
		if (capture.Recording())
		{
			TrailerCapture::State state;
			state.head_valid = xr.LocateHead(frame.display_time, &state.head);
			state.eyes_valid = xr.LocateEyes(frame.display_time, state.eyes);
			state.screen_placed = screen_placed;
			state.screen = screen_pose_now;
			state.screen_width = vr.screen_width;
			state.screen_height = vr.screen_width / screen_aspect;
			state.mode = vr.Mode3D();
			state.passthrough = vr.passthrough;
			state.menu_open = ui_open_last;
			state.gun_mode = gun_mode;
			state.guns = guns_drawn;
			state.fired = gun_fired;
			state.hands = hands_drawn;
			VrMenu::CaptureView ui_view;
			menu.DescribeForCapture(&ui_view);
			capture.RecordState(state, &ui_view);
		}
		if (capture.TakeStopped())
		{
			menu.SetRecording(false);
			menu.ShowNotice("Capture stopped: size limit reached", 5.0f);
			apply_gun_depth();
		}
		// The list: every slot up to the last one in use, the empty ones filled by invisible quads.
		const XrCompositionLayerBaseHeader* layers[NUM_SLOTS];
		uint32_t layer_count = 0;
		int last_used = -1;
		for (int i = 0; i < NUM_SLOTS; i++)
			last_used = slots[i] ? i : last_used;
		for (int i = 0; i <= last_used; i++)
		{
			if (!slots[i] && menu.GetPadLayer(&pads[i]))
				slots[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pads[i]);
			if (slots[i])
				layers[layer_count++] = slots[i];
		}
		const Clock::time_point t_rendered = Clock::now();
		xr.EndFrame(frame, layers, layer_count, vr.passthrough && slots[SLOT_BACK] != nullptr);
		{
			const auto ms = [](Clock::time_point a, Clock::time_point b) {
				return std::chrono::duration<float, std::milli>(b - a).count();
			};
			const Clock::time_point t_end = Clock::now();
			static Clock::time_point last_start{};
			const float period = last_start == Clock::time_point{} ? 0.0f : ms(last_start, t_start);
			last_start = t_start;
			static int slow_logs = 0;
			if ((period > 25.0f || ms(t_begun, t_end) > 12.0f) && slow_logs < 300)
			{
				slow_logs++;
				Console.WarningFmt("Frame: slow, {:.1f} ms since the last (wait {:.1f}, input+menu+guns {:.1f}, emu frame {:.1f}, "
								   "render {:.1f}, end {:.1f} ms){}",
					period, ms(t_start, t_begun), ms(t_begun, t_input_done), ms(t_input_done, t_emu_done),
					ms(t_emu_done, t_rendered), ms(t_rendered, t_end), ui_open_last ? ", menu open" : "");
			}
		}
		// Pacing, every 10 s: the display slots this app's frames skipped (the compositor showed an old
		// frame again there), and what was on, to tell which feature costs the compositor its slot.
		{
			static XrTime last_display = 0;
			static int pacing_frames = 0, pacing_missed = 0, pacing_worst = 0, pacing_max_layers = 0;
			static Clock::time_point next_pacing_log = Clock::now() + std::chrono::seconds(10);
			if (last_display != 0 && frame.display_period > 0 && frame.display_time > last_display)
			{
				const int slots_moved = static_cast<int>(
					(frame.display_time - last_display + frame.display_period / 2) / frame.display_period);
				pacing_missed += std::max(slots_moved - 1, 0);
				pacing_worst = std::max(pacing_worst, slots_moved - 1);
			}
			last_display = frame.display_time;
			pacing_frames++;
			pacing_max_layers = std::max(pacing_max_layers, static_cast<int>(layer_count));
			if (Clock::now() >= next_pacing_log)
			{
				next_pacing_log = Clock::now() + std::chrono::seconds(10);
				Console.WriteLnFmt("Pacing: {} frames, {} display slots missed ({:.1f}%), worst gap {}, up to {} layers, "
								   "game frames copied by the GS {} / here {} | 3D {}, passthrough {}, gun {}, Wi-Fi {}, controllers {}{}",
					pacing_frames, pacing_missed, 100.0f * pacing_missed / std::max(pacing_frames + pacing_missed, 1), pacing_worst,
					pacing_max_layers, frames_copied_by_gs, frames_copied_here, vr.Mode3DName(), vr.passthrough ? "on" : "off",
					gun_mode ? "on" : "off", vr.web_enabled ? "on" : "off", vr.show_controllers ? "on" : "off",
					ui_open_last ? ", menu open" : "");
				if (gun_mode)
				{
					const GunLight::Reading& light = gun_light.Latest();
					Console.WriteLnFmt("Pacing: light sensor {}, {:.1f}% lit, brightest {:.2f}", light.valid ? "reading" : "no reading",
						light.lit_share * 100.0f, light.brightest);
				}
				frames_copied_by_gs = frames_copied_here = 0;
				// The first stretch at half rate (or worse) in a game: a state of that scene, to replay it
				// headless (--load-state debug/state-stutter.p2s).
				static bool stutter_state_saved = false;
				if (!stutter_state_saved && pacing_frames < 600 && !Emu::GetSerial().empty() && !ui_open_last)
				{
					stutter_state_saved = true;
					Emu::SaveDebugState("state-stutter.p2s");
					Console.WriteLn("Pacing: frames came at half rate; saving debug/state-stutter.p2s");
				}
				pacing_frames = pacing_missed = pacing_worst = pacing_max_layers = 0;
			}
		}
		if (static int frames_logged = 0; frame.should_render && frames_logged < 3)
		{
			frames_logged++;
			Console.WriteLnFmt("Frame: {} layer(s)", layer_count);
		}
	}

	Console.WriteLn("Shutting down.");
	settings.Update(true);
	web.Stop();
	Emu::RequestShutdown();
	gun_depth.Destroy();
	gun_light.Destroy();
	capture.Destroy();
	gun_renderer.Destroy();
	menu.Destroy();
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
