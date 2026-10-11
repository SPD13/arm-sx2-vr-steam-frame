// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

// Embedded ARMSX2 core. The settings bootstrap, CPU-thread state machine and Host:: callbacks
// follow pcsx2-sdl/Main.cpp; the differences are a fixed Surfaceless window (frames leave the GS
// through VKLibretro::PublishFrame, see ScreenLayer), no SDL input (the XR controllers drive the
// pad through Emu::SetPad) and our own data directory.

#include "Emu.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "fmt/format.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Memory.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"

static std::unique_ptr<INISettingsInterface> s_base_settings;
static std::unique_ptr<INISettingsInterface> s_secrets_settings;

static std::atomic<bool> s_shutdown_requested{false};

// GS bring-up handshake with the XR thread: 0 pending, 1 open, -1 failed.
static std::mutex s_gs_mutex;
static std::condition_variable s_gs_cv;
static int s_gs_state = 0;
// Set by Join: the XR session and swapchains are gone, so the GS may close and destroy the device.
static bool s_gs_release = false;

static std::thread s_cpu_thread;
static std::atomic<int> s_cpu_thread_ret{EXIT_FAILURE};
static std::atomic<std::thread::id> s_cpu_thread_id{};

// Host::RunOnCPUThread queue, drained by PumpMessagesOnCPUThread and the idle loop.
static std::mutex s_cpu_queue_lock;
static std::deque<std::function<void()>> s_cpu_queue;
static std::condition_variable s_cpu_queue_cv;

static std::thread s_gamelist_thread;

// The GS presents into an offscreen ring (VKLibretro::Active); this size only seeds it.
static constexpr u32 SURFACE_WIDTH = 1280;
static constexpr u32 SURFACE_HEIGHT = 960;

static void SetGSState(int state)
{
	{
		std::lock_guard lock(s_gs_mutex);
		s_gs_state = state;
	}
	s_gs_cv.notify_all();
}

static void DrainCPUThreadQueue()
{
	for (;;)
	{
		std::function<void()> fn;
		{
			std::lock_guard lock(s_cpu_queue_lock);
			if (s_cpu_queue.empty())
				return;
			fn = std::move(s_cpu_queue.front());
			s_cpu_queue.pop_front();
		}
		fn();
	}
}

static void HandleSignal(int)
{
	Emu::RequestShutdown();
}

bool Emu::InitializeConfig(const Options& options)
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory())
		return false;

	const char* xdg_data = std::getenv("XDG_DATA_HOME");
	const char* home = std::getenv("HOME");
	EmuFolders::DataRoot = (xdg_data && *xdg_data) ? Path::Combine(xdg_data, "ps2v") :
	                                                 Path::Combine(home ? home : ".", ".local/share/ps2v");
	EmuFolders::Settings = Path::Combine(EmuFolders::DataRoot, "inis");
	FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), true);
	FileSystem::EnsureDirectoryExists(EmuFolders::Settings.c_str(), false);
	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* hw_check_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hw_check_error))
	{
		Console.ErrorFmt("Early hardware check failed: {}", hw_check_error ? hw_check_error : "unknown");
		return false;
	}

	const std::string roboto_path =
		EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
	auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
	if (roboto_data.empty())
	{
		Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
		return false;
	}
	std::vector<ImGuiManager::FontInfo> fonts(1);
	fonts[0].data = std::move(roboto_data);
	ImGuiManager::SetFonts(std::move(fonts));

	const std::string ini_path = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
	const bool ini_exists = FileSystem::FileExists(ini_path.c_str());
	Console.WriteLnFmt("Loading config from {}.", ini_path);
	s_base_settings = std::make_unique<INISettingsInterface>(ini_path);
	Host::Internal::SetBaseSettingsLayer(s_base_settings.get());
	if (!ini_exists || !s_base_settings->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		Console.WriteLnFmt("Initialising fresh config at {}.", ini_path);
		VMManager::SetDefaultSettings(*s_base_settings, true, true, true, true, true);
	}

	const std::string secrets_path = Path::Combine(EmuFolders::Settings, "secrets.ini");
	s_secrets_settings = std::make_unique<INISettingsInterface>(secrets_path);
	Host::Internal::SetSecretsSettingsLayer(s_secrets_settings.get());
	if (FileSystem::FileExists(secrets_path.c_str()))
		s_secrets_settings->Load();

	{
		auto lock = Host::GetSettingsLock();
		// Vulkan is the only renderer that can hand frames to the XR compositor.
		s_base_settings->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
		// The Frame controllers also show up as an SDL gamepad (28de:11e0); OpenXR owns them.
		s_base_settings->SetBoolValue("InputSources", "SDL", options.sdl_pad);
		if (options.sdl_pad)
		{
			static constexpr std::pair<const char*, const char*> bindings[] = {
				{"Up", "DPadUp"}, {"Down", "DPadDown"}, {"Left", "DPadLeft"}, {"Right", "DPadRight"},
				{"Cross", "FaceSouth"}, {"Circle", "FaceEast"}, {"Square", "FaceWest"}, {"Triangle", "FaceNorth"},
				{"Start", "Start"}, {"Select", "Back"}};
			for (const auto& [button, sdl] : bindings)
				s_base_settings->SetStringValue("Pad1", button, fmt::format("SDL-0/{}", sdl).c_str());
		}
		if (options.upscale)
			s_base_settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(*options.upscale));
	}

	Error save_error;
	if (!s_base_settings->Save(&save_error))
		Console.ErrorFmt("Failed to save config: {}", save_error.GetDescription());

	VMManager::Internal::LoadStartupSettings();

	// No BIOS chosen yet (there is no setup wizard): take the first console BIOS in the folder.
	if (s_base_settings->GetStringValue("Filenames", "BIOS").empty())
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &files);
		for (const FILESYSTEM_FIND_DATA& file : files)
		{
			u32 version, region;
			std::string description, zone;
			if (!IsConsoleBIOS(file.FileName.c_str(), version, description, region, zone))
				continue;
			const std::string name(Path::GetFileName(file.FileName));
			Console.WriteLnFmt("Using BIOS {} ({})", name, description);
			auto lock = Host::GetSettingsLock();
			s_base_settings->SetStringValue("Filenames", "BIOS", name.c_str());
			s_base_settings->Save();
			break;
		}
	}
	return true;
}

static void CPUThreadMain(VMBootParameters params)
{
	s_cpu_thread_ret.store(EXIT_FAILURE);
	s_cpu_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
	Threading::SetNameOfCurrentThread("CPU Thread");

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		Console.Error("CPU thread init failed.");
		VMManager::Internal::CPUThreadShutdown();
		SetGSState(-1);
		return;
	}

	VMManager::ApplySettings();
	InputManager::ReloadDevices();
	VMManager::ReloadInputBindings(true);

	// Open the GS before booting, as the libretro core does during Vulkan negotiation: the XR
	// session needs the device GSDeviceVK creates, and the VM then reuses the open GS.
	if (!MTGS::WaitForOpen())
	{
		Console.Error("Failed to open the GS.");
		VMManager::Internal::CPUThreadShutdown();
		SetGSState(-1);
		return;
	}
	MTGS::SetRunIdle(true);
	SetGSState(1);

	std::optional<VMBootParameters> pending_boot = std::move(params);
	bool clean_shutdown = true;

	while (!s_shutdown_requested.load(std::memory_order_acquire))
	{
		DrainCPUThreadQueue();

		switch (VMManager::GetState())
		{
			case VMState::Initializing:
				continue;

			case VMState::Running:
				VMManager::Execute();
				continue;

			case VMState::Resetting:
				VMManager::Reset();
				continue;

			case VMState::Stopping:
				VMManager::Shutdown(false);
				continue;

			case VMState::Paused:
			case VMState::Shutdown:
			{
				if (pending_boot)
				{
					VMBootParameters bp = std::move(*pending_boot);
					pending_boot.reset();
					const VMBootResult br = VMManager::Initialize(bp);
					if (br != VMBootResult::StartupSuccess)
					{
						Console.ErrorFmt("VMManager::Initialize failed (result {}).", static_cast<int>(br));
						clean_shutdown = false;
						s_shutdown_requested.store(true, std::memory_order_release);
						break;
					}
					VMManager::SetState(VMState::Running);
					continue;
				}

				// The game ended and there is no game picker yet: quit.
				if (VMManager::GetState() == VMState::Shutdown)
				{
					s_shutdown_requested.store(true, std::memory_order_release);
					break;
				}

				VMManager::IdlePollUpdate();
				std::unique_lock lock(s_cpu_queue_lock);
				s_cpu_queue_cv.wait_for(lock, std::chrono::milliseconds(16), []() { return !s_cpu_queue.empty(); });
				continue;
			}

			default:
				continue;
		}
	}

	if (VMManager::HasValidVM())
		VMManager::Shutdown(false);
	{
		std::unique_lock lock(s_gs_mutex);
		s_gs_cv.wait(lock, []() { return s_gs_release; });
	}
	if (MTGS::IsOpen())
	{
		MTGS::SetRunIdle(false);
		MTGS::WaitForClose();
	}
	if (s_gamelist_thread.joinable())
		s_gamelist_thread.join();

	VMManager::Internal::CPUThreadShutdown();
	s_cpu_thread_id.store(std::thread::id{}, std::memory_order_release);
	s_cpu_thread_ret.store(clean_shutdown ? EXIT_SUCCESS : EXIT_FAILURE);
	SetGSState(-1); // wakes a WaitForGS that is still pending
}

void Emu::Start(const Options& options)
{
	VMBootParameters params;
	params.filename = options.boot_path;
	if (options.bios_only)
		params.source_type = CDVD_SourceType::NoDisc;
	params.fast_boot = options.fast_boot;
	params.fullscreen = true;

	std::signal(SIGTERM, &HandleSignal);
	std::signal(SIGINT, &HandleSignal);
	std::signal(SIGHUP, &HandleSignal);

	SysMemory::ReserveMemory();
	s_cpu_thread = std::thread(CPUThreadMain, std::move(params));
}

bool Emu::WaitForGS()
{
	std::unique_lock lock(s_gs_mutex);
	s_gs_cv.wait(lock, []() { return s_gs_state != 0; });
	return s_gs_state > 0;
}

void Emu::RequestShutdown()
{
	s_shutdown_requested.store(true, std::memory_order_release);
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);
	s_cpu_queue_cv.notify_all();
}

bool Emu::ShutdownRequested()
{
	return s_shutdown_requested.load(std::memory_order_acquire);
}

int Emu::Join()
{
	{
		std::lock_guard lock(s_gs_mutex);
		s_gs_release = true;
	}
	s_gs_cv.notify_all();
	if (s_cpu_thread.joinable())
		s_cpu_thread.join();
	return s_cpu_thread_ret.load();
}

void Emu::SetPad(const PadState& s)
{
	using In = PadDualshock2::Inputs;
	const auto button = [](u32 bind, bool pressed) { Pad::SetControllerState(0, bind, pressed ? 1.0f : 0.0f); };
	const auto axis = [](u32 negative, u32 positive, float v) {
		Pad::SetControllerState(0, negative, v < 0.0f ? -v : 0.0f);
		Pad::SetControllerState(0, positive, v > 0.0f ? v : 0.0f);
	};
	button(In::PAD_UP, s.up);
	button(In::PAD_DOWN, s.down);
	button(In::PAD_LEFT, s.left);
	button(In::PAD_RIGHT, s.right);
	button(In::PAD_CROSS, s.cross);
	button(In::PAD_CIRCLE, s.circle);
	button(In::PAD_SQUARE, s.square);
	button(In::PAD_TRIANGLE, s.triangle);
	button(In::PAD_START, s.start);
	button(In::PAD_SELECT, s.select);
	button(In::PAD_L3, s.l3);
	button(In::PAD_R3, s.r3);
	Pad::SetControllerState(0, In::PAD_L1, s.l1);
	Pad::SetControllerState(0, In::PAD_R1, s.r1);
	Pad::SetControllerState(0, In::PAD_L2, s.l2);
	Pad::SetControllerState(0, In::PAD_R2, s.r2);
	axis(In::PAD_L_LEFT, In::PAD_L_RIGHT, s.lx);
	axis(In::PAD_L_DOWN, In::PAD_L_UP, s.ly);
	axis(In::PAD_R_LEFT, In::PAD_R_RIGHT, s.rx);
	axis(In::PAD_R_DOWN, In::PAD_R_UP, s.ry);
}

//////////////////////////////////////////////////////////////////////////
// Host:: callbacks
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges()
{
	if (!s_base_settings)
		return;
	Error err;
	if (!s_base_settings->Save(&err))
		Console.ErrorFmt("Failed to save settings: {}", err.GetDescription());
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("{}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("{}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("{}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("{}", message);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return {};
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

static WindowInfo SurfacelessWindow()
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = SURFACE_WIDTH;
	wi.surface_height = SURFACE_HEIGHT;
	wi.surface_scale = 1.0f;
	wi.surface_refresh_rate = 90.0f;
	return wi;
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return SurfacelessWindow();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
	INFO_LOG("Input device connected: {} ({})", identifier, device_name);
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
	INFO_LOG("Input device disconnected: {}", identifier);
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return SurfacelessWindow();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	if (!title.empty())
		INFO_LOG("Game changed: {} (serial {}, CRC {:08X})", title, disc_serial, current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::PumpMessagesOnCPUThread()
{
	if (s_shutdown_requested.load(std::memory_order_acquire) && VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);
	DrainCPUThreadQueue();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (block)
	{
		if (s_cpu_thread_id.load(std::memory_order_acquire) == std::this_thread::get_id())
		{
			function();
			return;
		}

		std::mutex done_lock;
		std::condition_variable done_cv;
		bool done = false;
		{
			std::lock_guard lock(s_cpu_queue_lock);
			s_cpu_queue.emplace_back([&function, &done_lock, &done_cv, &done]() {
				function();
				std::lock_guard lk(done_lock);
				done = true;
				done_cv.notify_all();
			});
		}
		s_cpu_queue_cv.notify_all();
		std::unique_lock lk(done_lock);
		done_cv.wait(lk, [&done]() { return done; });
		return;
	}

	{
		std::lock_guard lock(s_cpu_queue_lock);
		s_cpu_queue.emplace_back(std::move(function));
	}
	s_cpu_queue_cv.notify_all();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	if (s_gamelist_thread.joinable())
		s_gamelist_thread.join();
	s_gamelist_thread = std::thread([invalidate_cache]() {
		Threading::SetNameOfCurrentThread("GameList Refresh");
		GameList::Refresh(invalidate_cache, false, nullptr);
	});
}

void Host::CancelGameListRefresh()
{
	if (s_gamelist_thread.joinable())
		s_gamelist_thread.join();
}

bool Host::IsFullscreen()
{
	return true;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
	Emu::RequestShutdown();
}

void Host::RequestExitBigPicture()
{
	Emu::RequestShutdown();
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

bool Host::HasNativeAchievementNotifications()
{
	return false;
}

void Host::OnAchievementNotification(const char*, float, const char*, const char*, const char*)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	if (msg.empty())
		return 0;
	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	const TinyString count_str = TinyString::from_format("{}", count);
	std::string ret(msg);
	for (std::string::size_type pos; (pos = ret.find("%n")) != std::string::npos;)
		ret.replace(pos, 2, count_str.view());
	return ret;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
