// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Wi-Fi transfer: a small HTTP server on the headset that serves a web page (web/index.html) for
// managing the BIOS, game and texture pack folders from a computer or phone on the same network:
// list, upload, delete, and the space left on the device.
//
// It only talks to the local network: clients outside the private IPv4 ranges are dropped, the
// Host header must name an IP address or a .local name (no DNS rebinding from a web site), and
// requests that change something must carry a custom header a cross-site page cannot send. An
// optional 6-digit code, new each time the server starts, guards the page; wrong codes are
// rate-limited.
//
// Uploads are raw PUT bodies streamed to a hidden .part file, renamed when complete. A zip, 7z or RAR
// archive is unpacked instead (Unarchive.h) and then deleted: its files of the folder's kinds go into
// the folder, its textures into a game's texture pack (TexturePacks.h). When that needs the user's
// answer (files already there, or which game a pack is for), the unpacked files wait in the staging
// folder for it.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

class WebServer
{
public:
	struct Folder
	{
		enum class Kind
		{
			Files, // a flat folder of files
			TexturePacks, // a folder of texture packs, one per game (TexturePacks.h)
		};
		std::string id; // in URLs: bios, roms, textures
		std::string path;
		std::vector<std::string> extensions; // accepted for upload, lower case with the dot
		Kind kind = Kind::Files;
		bool archives = false; // zip, 7z and RAR archives are accepted too, and unpacked
	};

	// What the texture pack page needs from the emulator. Called on the server's threads.
	struct GameRef
	{
		std::string serial;
		std::string title;
	};
	struct TextureHooks
	{
		std::function<std::vector<GameRef>()> games; // the games in the library, to pair a pack with
		std::function<std::string(const std::string& serial)> title; // any game's title, empty if unknown
		std::function<std::string()> running_serial; // empty when no game runs
		std::function<void(const std::string& serial, bool added)> changed; // a pack was added to or removed
	};

	// What the menu shows about the server.
	struct Status
	{
		bool running = false;
		std::string url; // http://<address>:<port>, empty without a network address
		std::string code; // empty when no code is required
		std::string error; // why it is not running
		std::string transfer; // the file being received or unpacked, empty when idle
		std::string phase; // what happens to it: Receiving, Unpacking, Installing
		float progress = 0.0f; // of that phase, 0..1
	};

	WebServer();
	~WebServer();

	// What the page manages; set once, before Start.
	void SetFolders(std::vector<Folder> folders) { m_folders = std::move(folders); }
	// Where archives are received and unpacked, on the folders' file system; set once, before Start.
	void SetStagingFolder(std::string path) { m_staging = std::move(path); }
	void SetTextureHooks(TextureHooks hooks) { m_texture_hooks = std::move(hooks); }
	// Starts serving; require_code makes a new access code.
	bool Start(bool require_code);
	void Stop();
	// A new code (or none), signing every browser out.
	void SetRequireCode(bool require_code);
	// Files that must not be deleted (the running game).
	void SetProtectedFile(std::string path);
	Status GetStatus() const;

	// Exposed for tests.
	static bool IsLocalAddress(uint32_t ipv4_host_order);
	static bool IsLocalHostName(std::string_view host);
	static bool IsValidFileName(std::string_view name);

private:
	struct Request;
	struct Connection;
	// Unpacked files waiting for the user's answer.
	struct Pending
	{
		std::string id; // in URLs: /api/pending/<id>
		std::string folder; // Folder::id
		std::string name; // the archive's
		std::string dir; // in the staging folder
		std::vector<std::string> files; // relative to dir
		std::string serial; // a texture pack's game, empty when unknown
		int64_t created_ms = 0;
	};

	void AcceptLoop();
	void Serve(Connection* connection);
	void Handle(int fd, uint32_t peer, Request& request);
	void HandleLogin(int fd, uint32_t peer, Request& request);
	void HandleList(int fd, const Folder& folder);
	void HandleTextureList(int fd, const Folder& folder);
	// A game's title: the library's, else the game database's.
	std::string GameTitle(const std::string& serial) const;
	void HandleUpload(int fd, const Folder& folder, const std::string& name, Request& request);
	void HandleDelete(int fd, const Folder& folder, const std::string& name);
	void HandlePending(int fd, const Request& request, const std::string& id);
	// Unpacks a received archive (then deletes it) and installs what it held, or keeps it Pending.
	void HandleArchive(int fd, const Folder& folder, const std::string& name, const std::string& archive,
		const std::string& query);
	void Install(int fd, const Folder& folder, Pending pending, const std::string& query);
	void KeepPending(Pending pending);
	// One upload or unpacking at a time: false when another is under way.
	bool BeginTransfer(const std::string& name, const char* phase, uint64_t size);
	void SetTransferPhase(const char* phase);
	void SetTransferProgress(float progress);
	void EndTransfer();
	bool Authorized(const Request& request) const;
	const Folder* FindFolder(std::string_view id) const;
	std::string NewCode();

	std::vector<Folder> m_folders;
	std::string m_staging;
	TextureHooks m_texture_hooks;
	int m_listen_fd = -1;
	int m_port = 0;
	std::atomic<bool> m_stop{false};
	std::thread m_accept_thread;

	mutable std::mutex m_mutex; // guards everything below
	std::vector<std::unique_ptr<Connection>> m_connections;
	std::string m_address;
	std::string m_code;
	std::string m_error;
	std::vector<std::string> m_sessions;
	std::string m_protected_file;
	std::string m_transfer;
	const char* m_transfer_phase = "";
	uint64_t m_transfer_done = 0, m_transfer_size = 0;
	float m_transfer_progress = 0.0f; // while unpacking
	std::vector<Pending> m_pending;
	struct Failures
	{
		int count = 0;
		int64_t blocked_until_ms = 0;
	};
	std::unordered_map<uint32_t, Failures> m_failures; // wrong codes per client address
};
