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

#include "Emu.h"
#include "ScreenLayer.h"
#include "XrApp.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKLibretro.h"

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"

#define VK_NO_PROTOTYPES
#include "libretro_vulkan.h"

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
	if (!xr.CreateSession(gs->GetDevice(), gs->GetGraphicsQueueFamilyIndex()) ||
		!screen.Create(xr, gs->GetDevice(), gs->GetGraphicsQueue(), gs->GetGraphicsQueueFamilyIndex()))
	{
		Emu::RequestShutdown();
		screen.Destroy();
		xr.DestroySession();
		VKLibretro::AbortPacing();
		Emu::Join();
		return EXIT_FAILURE;
	}
	screen.SetWidth(screen_options.width);

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
			screen.SetPose(ScreenPoseInFrontOf(head, screen_options.distance));
			screen_placed = true;
		}

		VKLibretro::Frame emu_frame;
		if (VKLibretro::ConsumeFrame(&emu_frame))
			screen.Update(emu_frame);

		const XrCompositionLayerBaseHeader* layers[1];
		uint32_t layer_count = 0;
		if (screen.HasImage())
			layers[layer_count++] = screen.Layer();
		xr.EndFrame(frame, layers, layer_count);
	}

	Console.WriteLn("Shutting down.");
	Emu::RequestShutdown();
	screen.Destroy();
	xr.DestroySession();
	VKLibretro::AbortPacing();
	const int ret = Emu::Join();
	xr.DestroyInstance(VKLibretro::Init.device);
	VKLibretro::Active = false;
	VKLibretro::Shutdown();
	return ret;
}
