// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// PS2 Volumetric: ARMSX2 running inside a native OpenXR app on Steam Frame.
//
// Startup order matters because the core and the XR runtime share one Vulkan device:
//   1. load Vulkan and install VKLibretro's wraps, so GSDeviceVK's vkCreateDevice/vkQueueSubmit
//      go through them;
//   2. create the XrInstance and, from it, the VkInstance and GPU (VKLibretro::Init);
//   3. start the CPU thread, which opens the GS: GSDeviceVK adopts them and creates the device;
//   4. bind the XR session to that device and run the frame loop on this thread.
// Teardown runs the other way: the session goes before the GS destroys the device.

#include "DepthView.h"
#include "Emu.h"
#include "ScreenLayer.h"
#include "XrApp.h"

#include "GS/GSVolumetric.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKLibretro.h"

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#define VK_NO_PROTOTYPES
#include "libretro_vulkan.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

struct ScreenOptions
{
	float width = 2.4f; // meters
	float distance = 2.2f; // meters in front of the head
	bool depth_panel = true; // M2 debug view of the depth beside the screen
	std::string depth_dump_dir; // when set, dump depth + colour PNGs there every 10 s
};

static void PrintUsage(const char* argv0)
{
	std::fprintf(stderr,
		"Usage: %s [options] <disc image>\n"
		"  --bios-only            Boot the BIOS without a disc\n"
		"  --no-fast-boot         Run the BIOS intro before the game\n"
		"  --upscale N            Internal resolution multiplier (1-8)\n"
		"  --screen-width M       Virtual screen width in meters (default 2.4)\n"
		"  --screen-distance M    Virtual screen distance in meters (default 2.2)\n"
		"  --no-depth-panel       Hide the depth debug view beside the screen\n"
		"  --sdl-pad              Drive pad 1 from the first SDL gamepad (testing)\n"
		"  --depth-trace          Log which colour/depth targets the 3D draws use\n"
		"  --depth-dump DIR       Write depth and colour PNGs to DIR every 10 seconds\n"
		"In the headset: both sticks clicked recenters the screen.\n",
		argv0);
}

static bool ParseArgs(int argc, char* argv[], Emu::Options* emu, ScreenOptions* screen)
{
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
			screen->width = std::strtof(argv[++i], nullptr);
		else if (!std::strcmp(arg, "--screen-distance") && has_value)
			screen->distance = std::strtof(argv[++i], nullptr);
		else if (!std::strcmp(arg, "--sdl-pad"))
			emu->sdl_pad = true;
		else if (!std::strcmp(arg, "--depth-trace"))
			GSVolumetric::SetTrace(true);
		else if (!std::strcmp(arg, "--no-depth-panel"))
			screen->depth_panel = false;
		else if (!std::strcmp(arg, "--depth-dump") && has_value)
			screen->depth_dump_dir = argv[++i];
		else if (arg[0] == '-')
		{
			std::fprintf(stderr, "Unknown argument: %s\n", arg);
			return false;
		}
		else
			emu->boot_path = arg;
	}
	if (emu->boot_path.empty() && !emu->bios_only)
	{
		std::fprintf(stderr, "No disc image given (use --bios-only to boot the BIOS).\n");
		return false;
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

int main(int argc, char* argv[])
{
	Emu::Options emu_options;
	ScreenOptions screen_options;
	if (!ParseArgs(argc, argv, &emu_options, &screen_options))
	{
		PrintUsage(argv[0]);
		return EXIT_FAILURE;
	}

	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);

	if (!Emu::InitializeConfig(emu_options))
		return EXIT_FAILURE;

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
	GSVolumetric::SetEnabled(true);
	if (!screen_options.depth_dump_dir.empty())
		FileSystem::EnsureDirectoryExists(screen_options.depth_dump_dir.c_str(), true);

	Emu::Start(emu_options);
	if (!Emu::WaitForGS())
	{
		Console.Error("The GS did not come up; exiting.");
		Emu::RequestShutdown();
		Emu::Join();
		return EXIT_FAILURE;
	}

	GSDeviceVK* gs = GSDeviceVK::GetInstance();
	ScreenLayer screen;
	ScreenLayer depth_panel;
	DepthView depth_view;
	const VkDevice device = gs->GetDevice();
	const VkQueue queue = gs->GetGraphicsQueue();
	const uint32_t queue_family = gs->GetGraphicsQueueFamilyIndex();
	if (!xr.CreateSession(device, queue_family) || !screen.Create(xr, device, queue, queue_family) ||
		!depth_panel.Create(xr, device, queue, queue_family) ||
		!depth_view.Create(device, gs->GetPhysicalDevice(), queue, queue_family))
	{
		Emu::RequestShutdown();
		depth_view.Destroy();
		depth_panel.Destroy();
		screen.Destroy();
		xr.DestroySession();
		VKLibretro::AbortPacing();
		Emu::Join();
		return EXIT_FAILURE;
	}
	screen.SetWidth(screen_options.width);
	constexpr float depth_panel_width = 1.2f;
	depth_panel.SetWidth(depth_panel_width);

	using Clock = std::chrono::steady_clock;
	Clock::time_point next_stats_log = Clock::now() + std::chrono::seconds(5);
	Clock::time_point next_dump = Clock::now() + std::chrono::seconds(10);
	int dump_index = 0;

	bool screen_placed = false;
	while (!Emu::ShutdownRequested() && xr.PollEvents())
	{
		if (!xr.IsSessionRunning())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		const XrApp::Frame frame = xr.BeginFrame();
		const XrApp::Input input = xr.ReadInput();
		Emu::SetPad(input.pad);

		XrPosef head;
		if ((!screen_placed || input.recenter) && xr.LocateHead(frame.display_time, &head))
		{
			const XrPosef screen_pose = ScreenPoseInFrontOf(head, screen_options.distance);
			screen.SetPose(screen_pose);
			depth_panel.SetPose(PanelBeside(screen_pose, 0.5f * (screen_options.width + depth_panel_width) + 0.15f,
				screen_options.distance));
			screen_placed = true;
		}

		VKLibretro::Frame emu_frame;
		if (VKLibretro::ConsumeFrame(&emu_frame))
		{
			screen.Update(emu_frame);
			const Clock::time_point now = Clock::now();
			if (!screen_options.depth_dump_dir.empty() && now >= next_dump && emu_frame.depth_image)
			{
				depth_view.RequestDump(fmt::format("{}/frame{:03d}", screen_options.depth_dump_dir, dump_index++));
				next_dump = now + std::chrono::seconds(10);
			}
			VKLibretro::Frame depth_frame;
			if ((screen_options.depth_panel || !screen_options.depth_dump_dir.empty()) &&
				depth_view.Process(emu_frame, &depth_frame))
				depth_panel.Update(depth_frame);
			if (now >= next_stats_log)
			{
				next_stats_log = now + std::chrono::seconds(5);
				if (!emu_frame.depth_image)
					Console.WriteLn("Depth: none this frame");
				else if (const auto stats = depth_view.LastStats())
					Console.WriteLnFmt("Depth: coverage {:.0f}%, HUD {:.0f}%{}, range {:.6f}..{:.6f}, canvas {}x{}, scale {:.1f}",
						stats->coverage * 100.0f, stats->hud * 100.0f, emu_frame.prehud_view ? "" : " (no pre-HUD copy)",
						stats->zmin, stats->zmax, emu_frame.depth_width, emu_frame.depth_height, emu_frame.depth_scale);
			}
		}

		const XrCompositionLayerBaseHeader* layers[2];
		uint32_t layer_count = 0;
		if (screen.HasImage())
			layers[layer_count++] = screen.Layer();
		if (screen_options.depth_panel && depth_panel.HasImage())
			layers[layer_count++] = depth_panel.Layer();
		xr.EndFrame(frame, layers, layer_count);
	}

	Console.WriteLn("Shutting down.");
	Emu::RequestShutdown();
	depth_view.Destroy();
	depth_panel.Destroy();
	screen.Destroy();
	xr.DestroySession();
	VKLibretro::AbortPacing();
	const int ret = Emu::Join();
	xr.DestroyInstance(VKLibretro::Init.device);
	VKLibretro::Active = false;
	VKLibretro::Shutdown();
	return ret;
}
