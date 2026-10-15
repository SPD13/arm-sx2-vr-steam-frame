// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
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
#include <string_view>
#include <thread>
#include <utility>

#include <algorithm>
#include <filesystem>
#include <map>

#include <sys/stat.h>

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/StringUtil.h"
#include "common/SettingsWrapper.h"
#include "common/Threading.h"

#include "fmt/format.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/GS.h"
#include "GS/GSVolumetric.h"
#include "pcsx2/GameDatabase.h"
#include "pcsx2/GameList.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
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
#include "pcsx2/USB/USB.h"
#include "pcsx2/USB/usb-lightgun/guncon2.h"
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

static std::mutex s_serial_mutex;
static std::string s_serial;

// The VR menu's view of the save slots and its notices.
static std::mutex s_menu_mutex;
static std::vector<Emu::SaveSlot> s_save_slots(Emu::NUM_SAVE_SLOTS);
static std::string s_notice;
static std::atomic<bool> s_apply_pending{false};

// Game switching: the game the list asked for (CPU thread only), and how to boot it.
static std::optional<std::string> s_boot_request;
static bool s_fast_boot = true;
static std::atomic<bool> s_boot_failed{false};

// The game list as the menu sees it.
static std::mutex s_games_mutex;
static std::vector<Emu::GameInfo> s_games;
static std::atomic<unsigned> s_games_version{0};
static std::atomic<bool> s_games_busy{false};
static std::atomic<bool> s_games_again{false};
static std::atomic<bool> s_has_bios{false};

static void SetNotice(std::string notice);
static void OnOSDMessage(std::string_view key, std::string_view text, float duration);

// The core's OSD messages (OnOSDMessage), until they expire. Not OSDMessage: the core has one.
namespace
{
	struct CoreMessage
	{
		std::string key; // empty for messages that cannot be replaced
		std::string text;
		std::chrono::steady_clock::time_point until;
	};
} // namespace
static std::mutex s_osd_mutex;
static std::vector<CoreMessage> s_osd_messages;

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

// Debugging without the headset: SIGUSR1 saves a state to <data>/debug/state.p2s, SIGUSR2 writes
// a snapshot of the presented frame to <data>/debug/snapNNN.png.
static std::atomic<int> s_debug_request{0};
static std::string s_debug_dir;

static void HandleDebugSignal(int sig)
{
	s_debug_request.store(sig == SIGUSR1 ? 1 : 2, std::memory_order_release);
}

static void ServiceDebugRequest()
{
	const int request = s_debug_request.exchange(0, std::memory_order_acq_rel);
	if (!request || s_debug_dir.empty())
		return;
	FileSystem::EnsureDirectoryExists(s_debug_dir.c_str(), true);
	if (request == 1)
	{
		const std::string path = Path::Combine(s_debug_dir, "state.p2s");
		VMManager::SaveState(path.c_str(), false, false, [](const std::string& error) {
			Console.ErrorFmt("Debug: save state failed: {}", error);
		});
		Console.WriteLnFmt("Debug: saved state to {}", path);
	}
	else
	{
		static int s_snap = 0;
		const std::string path = Path::Combine(s_debug_dir, fmt::format("snap{:03d}.png", s_snap++));
		MTGS::RunOnGSThread([path]() { GSQueueSnapshot(path); });
		GSVolumetric::RequestPresentDump(Path::Combine(s_debug_dir, fmt::format("present{:03d}", s_snap - 1)));
		GSVolumetric::ArmDrawTrace(1);
		Console.WriteLnFmt("Debug: snapshot queued to {}", path);
	}
}

// The BIOS games boot with: the one chosen in PCSX2.ini while its file is there, else (there is no
// setup wizard) the first console BIOS in the folder, which becomes the chosen one. Whether there is one.
static bool PickBios()
{
	{
		auto lock = Host::GetSettingsLock();
		const std::string chosen = s_base_settings->GetStringValue("Filenames", "BIOS");
		if (!chosen.empty() &&
			FileSystem::FileExists((Path::IsAbsolute(chosen) ? chosen : Path::Combine(EmuFolders::Bios, chosen)).c_str()))
			return true;
	}
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
		return true;
	}
	return false;
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
	s_debug_dir = Path::Combine(EmuFolders::DataRoot, "debug");

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
	// The OSD messages go above the screen, not into the game's picture.
	ImGuiManager::SetOSDMessageHook(&OnOSDMessage);

	const std::string ini_path = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
	const bool ini_exists = FileSystem::FileExists(ini_path.c_str());
	Console.WriteLnFmt("Loading config from {}.", ini_path);
	s_base_settings = std::make_unique<INISettingsInterface>(ini_path);
	Host::Internal::SetBaseSettingsLayer(s_base_settings.get());
	if (!ini_exists || !s_base_settings->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		Console.WriteLnFmt("Initialising fresh config at {}.", ini_path);
		VMManager::SetDefaultSettings(*s_base_settings, true, true, true, true, true);
		// Minimum blending accuracy: at Basic, effects that read the framebuffer are split into a draw
		// and a barrier per primitive, and with the per-eye render passes Time Crisis II's gunfights
		// overloaded the Frame's GPU (SteamVR then halves the frame rate). Settings > Graphics.
		s_base_settings->SetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(AccBlendLevel::Minimum));
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
				{"Start", "Start"}, {"Select", "Back"}, {"L1", "LeftShoulder"}, {"R1", "RightShoulder"},
				{"L2", "+LeftTrigger"}, {"R2", "+RightTrigger"}};
			for (const auto& [button, sdl] : bindings)
				s_base_settings->SetStringValue("Pad1", button, fmt::format("SDL-0/{}", sdl).c_str());
		}
		if (options.upscale)
			s_base_settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(*options.upscale));
		// The game list scans the games folder.
		FileSystem::EnsureDirectoryExists(GetGamesFolder().c_str(), false);
		s_base_settings->AddToStringList("GameList", "RecursivePaths", GetGamesFolder().c_str());
	}

	Error save_error;
	if (!s_base_settings->Save(&save_error))
		Console.ErrorFmt("Failed to save config: {}", save_error.GetDescription());

	VMManager::Internal::LoadStartupSettings();

	s_has_bios = PickBios();
	return true;
}

// Fills the game list from the games folder, and looks for a BIOS. The scan needs the CDVD lock, which a running game
// holds, so files added meanwhile only show by file name until no game runs.
static void ListGames()
{
	// A BIOS sent over Wi-Fi meanwhile is found here too.
	s_has_bios = PickBios();
	GameList::Refresh(false, false, nullptr);

	std::vector<Emu::GameInfo> games;
	std::map<std::string, size_t> by_path;
	{
		auto lock = GameList::GetLock();
		for (u32 i = 0; i < GameList::GetEntryCount(); i++)
		{
			const GameList::Entry* entry = GameList::GetEntryByIndex(i);
			// PS1 discs show up too, but this emulator cannot run them.
			if (!entry || (entry->type != GameList::EntryType::PS2Disc && entry->type != GameList::EntryType::ELF))
				continue;
			by_path[entry->path] = games.size();
			games.push_back({entry->path, entry->GetTitle(), entry->serial, entry->total_size, 0, entry->last_played_time,
				entry->total_played_time});
		}
	}
	std::error_code ec;
	for (auto it = std::filesystem::recursive_directory_iterator(Emu::GetGamesFolder(), ec);
		 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
	{
		const std::filesystem::path& path = it->path();
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
		const auto& exts = Emu::GetGameExtensions();
		if (!it->is_regular_file(ec) || path.filename().string().starts_with(".") ||
			std::find(exts.begin(), exts.end(), ext) == exts.end() || by_path.count(path.string()))
			continue;
		// The data half of a cue sheet or an MDS image is not a game of its own.
		if ((ext == ".bin" && std::filesystem::exists(std::filesystem::path(path).replace_extension(".cue"), ec)) ||
			(ext == ".mdf" && std::filesystem::exists(std::filesystem::path(path).replace_extension(".mds"), ec)))
			continue;
		games.push_back({path.string(), path.stem().string(), {}, static_cast<uint64_t>(it->file_size(ec)), 0, 0, 0});
	}
	// When each arrived: the inode change time, which a copy, move or upload into the folder sets
	// even when it keeps the file's modification time.
	for (Emu::GameInfo& game : games)
	{
		struct stat st;
		if (stat(game.path.c_str(), &st) == 0)
			game.added = st.st_ctime;
	}
	std::lock_guard lock(s_games_mutex);
	s_games = std::move(games);
}

static void CPUThreadMain(std::optional<VMBootParameters> params)
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

	// Once before booting: while a game runs, the game list cannot scan.
	ListGames();
	s_games_version++;

	std::optional<VMBootParameters> pending_boot = std::move(params);
	bool clean_shutdown = true;
	bool boot_from_list = false;

	while (!s_shutdown_requested.load(std::memory_order_acquire))
	{
		DrainCPUThreadQueue();

		switch (VMManager::GetState())
		{
			case VMState::Initializing:
				continue;

			case VMState::Running:
				ServiceDebugRequest();
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
				// A game picked in the game list, once the previous one has shut down.
				if (!pending_boot && s_boot_request && VMManager::GetState() == VMState::Shutdown)
				{
					pending_boot.emplace();
					pending_boot->filename = std::move(*s_boot_request);
					pending_boot->fast_boot = s_fast_boot;
					pending_boot->fullscreen = true;
					s_boot_request.reset();
					boot_from_list = true;
				}
				if (pending_boot)
				{
					VMBootParameters bp = std::move(*pending_boot);
					pending_boot.reset();
					// A game list scan holds the CDVD lock the boot needs.
					while (s_games_busy.load())
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					const VMBootResult br = VMManager::Initialize(bp);
					if (br != VMBootResult::StartupSuccess)
					{
						Console.ErrorFmt("VMManager::Initialize failed (result {}).", static_cast<int>(br));
						if (boot_from_list)
						{
							// Back to the list, which says what went wrong.
							s_has_bios = PickBios();
							SetNotice(s_has_bios ? fmt::format("Could not start {}", Path::GetFileName(bp.filename)) :
							                       std::string("Could not start the game: there is no PS2 BIOS"));
							s_boot_failed = true;
							continue;
						}
						clean_shutdown = false;
						s_shutdown_requested.store(true, std::memory_order_release);
						break;
					}
					VMManager::SetState(VMState::Running);
					continue;
				}
				// No game: wait for one from the game list.

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
	s_fast_boot = options.fast_boot;
	std::optional<VMBootParameters> params;
	if (!options.boot_path.empty() || options.bios_only)
	{
		params.emplace();
		params->filename = options.boot_path;
		if (options.bios_only)
			params->source_type = CDVD_SourceType::NoDisc;
		params->fast_boot = options.fast_boot;
		params->save_state = options.load_state;
		params->fullscreen = true;
	}

	std::signal(SIGTERM, &HandleSignal);
	std::signal(SIGINT, &HandleSignal);
	std::signal(SIGHUP, &HandleSignal);
	std::signal(SIGUSR1, &HandleDebugSignal);
	std::signal(SIGUSR2, &HandleDebugSignal);

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

std::string Emu::GetSerial()
{
	std::lock_guard lock(s_serial_mutex);
	return s_serial;
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

void Emu::SetPaused(bool paused)
{
	Host::RunOnCPUThread([paused]() {
		// Resuming would cancel a game switch (or the app quitting) under way.
		if (VMManager::HasValidVM() && VMManager::GetState() != VMState::Stopping)
			VMManager::SetPaused(paused);
	});
}

Emu::CoreSettings Emu::GetCoreSettings()
{
	CoreSettings settings;
	auto lock = Host::GetSettingsLock();
	if (!s_base_settings)
		return settings;
	settings.upscale = static_cast<int>(s_base_settings->GetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f));
	settings.texture_filtering = s_base_settings->GetIntValue("EmuCore/GS", "filter", static_cast<int>(BiFiltering::PS2));
	settings.blending =
		s_base_settings->GetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(AccBlendLevel::Minimum));
	settings.volume = static_cast<int>(s_base_settings->GetUIntValue("SPU2/Output", "StandardVolume", 100));
	settings.muted = s_base_settings->GetBoolValue("SPU2/Output", "OutputMuted", false);
	settings.texture_packs = s_base_settings->GetBoolValue("EmuCore/GS", "LoadTextureReplacements", false);
	settings.texture_packs_async = s_base_settings->GetBoolValue("EmuCore/GS", "LoadTextureReplacementsAsync", true);
	settings.texture_packs_precache = s_base_settings->GetBoolValue("EmuCore/GS", "PrecacheTextureReplacements", false);
	return settings;
}

void Emu::SetCoreSettings(const CoreSettings& settings)
{
	{
		auto lock = Host::GetSettingsLock();
		if (!s_base_settings)
			return;
		s_base_settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(settings.upscale));
		s_base_settings->SetIntValue("EmuCore/GS", "filter", settings.texture_filtering);
		s_base_settings->SetIntValue("EmuCore/GS", "accurate_blending_unit", settings.blending);
		s_base_settings->SetUIntValue("SPU2/Output", "StandardVolume", static_cast<u32>(settings.volume));
		s_base_settings->SetBoolValue("SPU2/Output", "OutputMuted", settings.muted);
		s_base_settings->SetBoolValue("EmuCore/GS", "LoadTextureReplacements", settings.texture_packs);
		s_base_settings->SetBoolValue("EmuCore/GS", "LoadTextureReplacementsAsync", settings.texture_packs_async);
		s_base_settings->SetBoolValue("EmuCore/GS", "PrecacheTextureReplacements", settings.texture_packs_precache);
		Host::CommitBaseSettingChanges();
	}
	// A slider sends a change per frame: queue one apply at a time, which reads the latest values.
	if (!s_apply_pending.exchange(true))
	{
		Host::RunOnCPUThread([]() {
			s_apply_pending.store(false);
			if (VMManager::HasValidVM())
				VMManager::ApplySettings();
		});
	}
}

// Any thread: the core's OSD messages, shown by the VR menu above the screen (Emu::GetMessages).
static void OnOSDMessage(std::string_view key, std::string_view text, float duration)
{
	std::lock_guard lock(s_osd_mutex);
	if (text.empty() && duration <= 0.0f)
	{
		if (key.empty())
			s_osd_messages.clear();
		else
			std::erase_if(s_osd_messages, [&key](const CoreMessage& m) { return m.key == key; });
		return;
	}
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(duration * 1000.0f));
	if (!key.empty())
	{
		// A keyed message replaces the one with its key, where it was.
		for (CoreMessage& m : s_osd_messages)
		{
			if (m.key == key)
			{
				m.text = text;
				m.until = until;
				return;
			}
		}
	}
	s_osd_messages.push_back({std::string(key), std::string(text), until});
}

std::vector<std::string> Emu::GetMessages()
{
	std::lock_guard lock(s_osd_mutex);
	const auto now = std::chrono::steady_clock::now();
	std::erase_if(s_osd_messages, [now](const CoreMessage& m) { return m.until <= now; });
	std::vector<std::string> texts;
	for (const CoreMessage& m : s_osd_messages)
		texts.push_back(m.text);
	return texts;
}

static void SetNotice(std::string notice)
{
	std::lock_guard lock(s_menu_mutex);
	s_notice = std::move(notice);
}

// On the CPU thread.
static void ReadSaveSlots()
{
	std::vector<Emu::SaveSlot> slots(Emu::NUM_SAVE_SLOTS);
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	if (VMManager::HasValidVM() && (!serial.empty() || crc))
	{
		for (int slot = 1; slot <= Emu::NUM_SAVE_SLOTS; slot++)
		{
			FILESYSTEM_STAT_DATA sd;
			const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
			if (!path.empty() && FileSystem::StatFile(path.c_str(), &sd))
				slots[slot - 1] = {true, sd.ModificationTime};
		}
	}
	std::lock_guard lock(s_menu_mutex);
	s_save_slots = std::move(slots);
}

void Emu::RefreshSaveSlots()
{
	Host::RunOnCPUThread(&ReadSaveSlots);
}

std::vector<Emu::SaveSlot> Emu::GetSaveSlots()
{
	std::lock_guard lock(s_menu_mutex);
	return s_save_slots;
}

void Emu::SaveState(int slot)
{
	Host::RunOnCPUThread([slot]() {
		if (!VMManager::HasValidVM())
			return;
		bool failed = false;
		// Written before this returns, so the slot list below sees it.
		VMManager::SaveStateToSlot(slot, false, [&failed](const std::string& error) {
			failed = true;
			SetNotice(fmt::format("Could not save: {}", error));
		});
		if (!failed)
			SetNotice(fmt::format("Saved to slot {}", slot));
		ReadSaveSlots();
	});
}

void Emu::LoadState(int slot)
{
	Host::RunOnCPUThread([slot]() {
		if (!VMManager::HasValidVM())
			return;
		Error error;
		if (VMManager::LoadStateFromSlot(slot, false, &error))
			SetNotice(fmt::format("Loaded slot {}", slot));
		else
			SetNotice(fmt::format("Could not load slot {}: {}", slot, error.GetDescription()));
	});
}

void Emu::ResetGame()
{
	Host::RunOnCPUThread([]() {
		if (VMManager::HasValidVM())
			VMManager::Reset();
	});
}

std::string Emu::GetRunningGamePath()
{
	return VMManager::HasValidVM() ? VMManager::GetDiscPath() : std::string();
}

std::string Emu::GetGamesFolder()
{
	return Path::Combine(EmuFolders::DataRoot, "roms");
}

const std::vector<std::string>& Emu::GetGameExtensions()
{
	static const std::vector<std::string> extensions = {
		".iso", ".chd", ".cso", ".zso", ".gz", ".bin", ".cue", ".img", ".mdf", ".mds", ".nrg", ".elf"};
	return extensions;
}

std::string Emu::GetTexturesFolder()
{
	return EmuFolders::Textures;
}

std::string Emu::GetGameTitle(const std::string& serial)
{
	if (serial.empty())
		return {};
	{
		std::lock_guard lock(s_games_mutex);
		for (const GameInfo& game : s_games)
		{
			if (game.serial == serial && !game.title.empty())
				return game.title;
		}
	}
	const GameDatabaseSchema::GameEntry* entry = GameDatabase::findGame(serial);
	if (!entry)
		return {};
	// The English title when the database has one: the menu's font is Latin only.
	return entry->name_en.empty() ? entry->name : entry->name_en;
}

void Emu::TexturePackChanged(const std::string& serial, bool added)
{
	if (added)
	{
		CoreSettings settings = GetCoreSettings();
		if (!settings.texture_packs)
		{
			Console.WriteLn("Texture packs: turned on for the pack just added.");
			settings.texture_packs = true;
			SetCoreSettings(settings); // turning them on loads the running game's pack
			return;
		}
	}
	Host::RunOnCPUThread([serial]() {
		if (!MTGS::IsOpen() || VMManager::GetDiscSerial() != serial || !EmuConfig.GS.LoadTextureReplacements)
			return;
		// As the core's Reload Texture Replacements hotkey does.
		MTGS::RunOnGSThread([]() {
			if (!g_gs_renderer)
				return;
			GSTextureReplacements::ReloadReplacementMap();
			g_gs_renderer->PurgeTextureCache(true, false, true);
		});
	});
}

void Emu::RefreshGames()
{
	if (s_games_busy.exchange(true))
	{
		s_games_again = true;
		return;
	}
	if (s_gamelist_thread.joinable())
		s_gamelist_thread.join(); // finished
	s_gamelist_thread = std::thread([]() {
		Threading::SetNameOfCurrentThread("Game List");
		do
		{
			s_games_again = false;
			ListGames();
			s_games_version++;
		} while (s_games_again.load());
		s_games_busy = false;
	});
}

bool Emu::IsRefreshingGames()
{
	return s_games_busy.load();
}

std::vector<Emu::GameInfo> Emu::GetGames()
{
	std::lock_guard lock(s_games_mutex);
	return s_games;
}

unsigned Emu::GetGamesVersion()
{
	return s_games_version.load();
}

void Emu::BootGame(const std::string& path)
{
	Host::RunOnCPUThread([path]() {
		s_boot_request = path;
		if (VMManager::HasValidVM())
			VMManager::SetState(VMState::Stopping);
	});
}

bool Emu::HasBios()
{
	return s_has_bios.load();
}

bool Emu::TakeBootFailure()
{
	return s_boot_failed.exchange(false);
}

std::string Emu::TakeNotice()
{
	std::lock_guard lock(s_menu_mutex);
	return std::exchange(s_notice, {});
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

static_assert(static_cast<u32>(Emu::GUN_C) == usb_lightgun::HOST_C && static_cast<u32>(Emu::GUN_A) == usb_lightgun::HOST_A &&
                  static_cast<u32>(Emu::GUN_DOWN) == usb_lightgun::HOST_DPAD_DOWN &&
                  static_cast<u32>(Emu::GUN_TRIGGER) == usb_lightgun::HOST_TRIGGER &&
                  static_cast<u32>(Emu::GUN_START) == usb_lightgun::HOST_START &&
                  static_cast<u32>(Emu::GUN_SHOOT_OFFSCREEN) == usb_lightgun::HOST_SHOOT_OFFSCREEN &&
                  static_cast<u32>(Emu::GUN_RECALIBRATE) == usb_lightgun::HOST_RECALIBRATE,
	"Emu::GunButton follows usb_lightgun::HostButton");

void Emu::SetGun(int port, const GunState& state)
{
	usb_lightgun::SetHostState(static_cast<u32>(port), true, state.aim_valid, state.x, state.y, state.buttons);
}

unsigned Emu::GetGunCalibration(int port, int* x, int* y)
{
	s16 px = 0, py = 0;
	const u32 count = usb_lightgun::GetGameParams(static_cast<u32>(port), &px, &py);
	*x = px;
	*y = py;
	return count;
}

void Emu::ReleaseGuns()
{
	for (u32 port = 0; port < USB::NUM_PORTS; port++)
		usb_lightgun::SetHostState(port, false, false, 0.0f, 0.0f, 0);
}

// The policy for each new game, and the serial it was last asked about (CPU thread only).
static std::function<int(const std::string&)> s_guns_policy;
static std::string s_guns_policy_serial;

// On the CPU thread: the USB ports as the ini has them, in the running game too.
static void ApplyGunPorts(int guns)
{
	{
		auto lock = Host::GetSettingsLock();
		if (!s_base_settings)
			return;
		for (u32 port = 0; port < USB::NUM_PORTS; port++)
			USB::SetConfigDevice(*s_base_settings, port, static_cast<int>(port) < guns ? "guncon2" : "None");
		Host::CommitBaseSettingChanges();
	}
	if (!VMManager::HasValidVM())
		return; // the next game's USB opens with them
	const Pcsx2Config old_config(EmuConfig);
	{
		auto lock = Host::GetSettingsLock();
		SettingsLoadWrapper wrap(*Host::GetSettingsInterface());
		EmuConfig.USB.LoadSave(wrap);
	}
	if (EmuConfig.USB == old_config.USB)
		return;
	USB::CheckForConfigChanges(old_config);
	Console.WriteLnFmt("Light gun: {} GunCon 2 plugged in", guns);
}

void Emu::SetGunsPlugged(int guns)
{
	Host::RunOnCPUThread([guns]() { ApplyGunPorts(guns); });
}

void Emu::SetGunsPolicy(std::function<int(const std::string& serial)> policy)
{
	Host::RunOnCPUThread([policy = std::move(policy)]() mutable { s_guns_policy = std::move(policy); });
}

bool Emu::IsLightgunGame(const std::string& serial)
{
	return usb_lightgun::IsKnownGame(serial);
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
	{
		std::lock_guard lock(s_serial_mutex);
		s_serial = disc_serial;
	}
	// A new game: plug the guns in or out for it, before it looks for them. Queued, so it runs once
	// the VM is up (this can come in the middle of its start).
	if (s_guns_policy && !disc_serial.empty() && disc_serial != s_guns_policy_serial)
	{
		s_guns_policy_serial = disc_serial;
		Host::RunOnCPUThread([guns = s_guns_policy(disc_serial)]() { ApplyGunPorts(guns); });
	}
	else if (disc_serial.empty())
		s_guns_policy_serial.clear();
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
	ServiceDebugRequest();
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
