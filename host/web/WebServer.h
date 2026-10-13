// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Wi-Fi transfer: a small HTTP server on the headset that serves a web page (web/index.html) for
// managing the BIOS and game folders from a computer or phone on the same network: list, upload,
// delete, and the space left on the device.
//
// It only talks to the local network: clients outside the private IPv4 ranges are dropped, the
// Host header must name an IP address or a .local name (no DNS rebinding from a web site), and
// requests that change something must carry a custom header a cross-site page cannot send. An
// optional 6-digit code, new each time the server starts, guards the page; wrong codes are
// rate-limited.
//
// Uploads are raw PUT bodies streamed to a hidden .part file, renamed when complete.

#pragma once

#include <atomic>
#include <cstdint>
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
		std::string id; // in URLs: bios, roms
		std::string path;
		std::vector<std::string> extensions; // accepted for upload, lower case with the dot
	};

	// What the menu shows about the server.
	struct Status
	{
		bool running = false;
		std::string url; // http://<address>:<port>, empty without a network address
		std::string code; // empty when no code is required
		std::string error; // why it is not running
		std::string transfer; // the file being received, empty when idle
		float progress = 0.0f; // of that file, 0..1
	};

	WebServer();
	~WebServer();

	// What the page manages; set once, before Start.
	void SetFolders(std::vector<Folder> folders) { m_folders = std::move(folders); }
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

	void AcceptLoop();
	void Serve(Connection* connection);
	void Handle(int fd, uint32_t peer, Request& request);
	void HandleLogin(int fd, uint32_t peer, Request& request);
	void HandleList(int fd, const Folder& folder);
	void HandleUpload(int fd, const Folder& folder, const std::string& name, Request& request);
	void HandleDelete(int fd, const Folder& folder, const std::string& name);
	bool Authorized(const Request& request) const;
	const Folder* FindFolder(std::string_view id) const;
	std::string NewCode();

	std::vector<Folder> m_folders;
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
	uint64_t m_transfer_done = 0, m_transfer_size = 0;
	struct Failures
	{
		int count = 0;
		int64_t blocked_until_ms = 0;
	};
	std::unordered_map<uint32_t, Failures> m_failures; // wrong codes per client address
};
