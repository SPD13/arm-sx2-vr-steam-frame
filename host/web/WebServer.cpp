// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "WebServer.h"

#include "web_page.h" // s_web_page: web/index.html, embedded at build time

#include "common/Console.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>

namespace
{
	constexpr int FIRST_PORT = 8080;
	constexpr int PORT_TRIES = 10;
	constexpr size_t MAX_CONNECTIONS = 8;
	constexpr size_t MAX_HEAD = 16 * 1024;
	constexpr int IO_TIMEOUT_S = 30;
	constexpr uint64_t SPACE_MARGIN = 64ull << 20; // keep this much free after an upload
	constexpr int CODE_TRIES = 5; // wrong codes before a client is held off
	constexpr size_t MAX_SESSIONS = 32;

	int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
			.count();
	}

	bool SendAll(int fd, const void* data, size_t size)
	{
		const char* p = static_cast<const char*>(data);
		while (size)
		{
#ifdef MSG_NOSIGNAL
			const ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
#else
			const ssize_t n = send(fd, p, size, 0); // SO_NOSIGPIPE is set on the socket
#endif
			if (n <= 0)
				return false;
			p += n;
			size -= static_cast<size_t>(n);
		}
		return true;
	}

	const char* StatusText(int status)
	{
		switch (status)
		{
			case 200: return "OK";
			case 400: return "Bad Request";
			case 401: return "Unauthorized";
			case 403: return "Forbidden";
			case 404: return "Not Found";
			case 405: return "Method Not Allowed";
			case 409: return "Conflict";
			case 411: return "Length Required";
			case 413: return "Payload Too Large";
			case 429: return "Too Many Requests";
			case 500: return "Internal Server Error";
			case 503: return "Service Unavailable";
			case 507: return "Insufficient Storage";
			default: return "Error";
		}
	}

	void Respond(int fd, int status, std::string_view type, std::string_view body, std::string_view extra_headers = {})
	{
		std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
		head += "Content-Type: " + std::string(type) + "\r\n";
		head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		head += "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n";
		head += "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
		head += extra_headers;
		head += "\r\n";
		if (SendAll(fd, head.data(), head.size()))
			SendAll(fd, body.data(), body.size());
	}

	std::string JsonString(std::string_view s)
	{
		std::string out = "\"";
		for (const char c : s)
		{
			switch (c)
			{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					}
					else
						out += c;
			}
		}
		return out + "\"";
	}

	void RespondJson(int fd, int status, std::string_view json, std::string_view extra_headers = {})
	{
		Respond(fd, status, "application/json", json, extra_headers);
	}

	void RespondError(int fd, int status, std::string_view message)
	{
		RespondJson(fd, status, "{\"error\":" + JsonString(message) + "}");
	}

	std::string UrlDecode(std::string_view s, bool* ok)
	{
		std::string out;
		*ok = true;
		for (size_t i = 0; i < s.size(); i++)
		{
			if (s[i] != '%')
			{
				out += s[i];
				continue;
			}
			if (i + 2 >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + 1])) ||
				!std::isxdigit(static_cast<unsigned char>(s[i + 2])))
			{
				*ok = false;
				return {};
			}
			out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
			i += 2;
		}
		return out;
	}

	std::string Lower(std::string_view s)
	{
		std::string out(s);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
		return out;
	}

	bool DiskSpace(const std::string& path, uint64_t* free, uint64_t* total)
	{
		struct statvfs vfs;
		if (statvfs(path.c_str(), &vfs) != 0)
			return false;
		*free = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
		*total = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
		return true;
	}

	// The device's address on the local network: a private IPv4 address, Wi-Fi preferred.
	std::string LocalAddress()
	{
		ifaddrs* list = nullptr;
		if (getifaddrs(&list) != 0)
			return {};
		std::string best;
		bool best_wifi = false;
		for (const ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
		{
			if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || !(ifa->ifa_flags & IFF_UP) ||
				(ifa->ifa_flags & IFF_LOOPBACK))
				continue;
			const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr);
			if (!WebServer::IsLocalAddress(ip) || (ip >> 24) == 127)
				continue;
			const bool wifi = std::strncmp(ifa->ifa_name, "wl", 2) == 0;
			if (best.empty() || (wifi && !best_wifi))
			{
				char text[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr, text, sizeof(text));
				best = text;
				best_wifi = wifi;
			}
		}
		freeifaddrs(list);
		return best;
	}

	std::string RandomHex(size_t bytes)
	{
		std::random_device rd;
		static constexpr char digits[] = "0123456789abcdef";
		std::string out;
		for (size_t i = 0; i < bytes; i++)
		{
			const unsigned v = rd() & 0xFF;
			out += digits[v >> 4];
			out += digits[v & 15];
		}
		return out;
	}
} // namespace

struct WebServer::Request
{
	std::string method;
	std::string path; // decoded later per route
	std::string query;
	std::unordered_map<std::string, std::string> headers; // names in lower case
	std::string body_start; // body bytes read along with the head
	uint64_t content_length = 0;
	bool has_length = false;

	std::string Header(const char* name) const
	{
		const auto it = headers.find(name);
		return it == headers.end() ? std::string() : it->second;
	}
};

struct WebServer::Connection
{
	std::thread thread;
	int fd = -1;
	uint32_t peer = 0;
	std::atomic<bool> done{false};
};

WebServer::WebServer() = default;

WebServer::~WebServer()
{
	Stop();
}

bool WebServer::IsLocalAddress(uint32_t ip)
{
	return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1) || (ip >> 16) == ((192u << 8) | 168) ||
	       (ip >> 16) == ((169u << 8) | 254) || (ip >> 24) == 127;
}

bool WebServer::IsLocalHostName(std::string_view host_port)
{
	const std::string host = Lower(host_port.substr(0, host_port.rfind(':'))); // all of it without a port
	if (host.empty())
		return false;
	if (host == "localhost" || (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0))
		return true;
	in_addr addr;
	if (inet_pton(AF_INET, host.c_str(), &addr) == 1)
		return IsLocalAddress(ntohl(addr.s_addr));
	// A bare host name (no dots) is the machine's own name on the local network.
	char name[256] = {};
	return gethostname(name, sizeof(name) - 1) == 0 && host == Lower(name);
}

bool WebServer::IsValidFileName(std::string_view name)
{
	if (name.empty() || name.size() > 255 || name.front() == '.' || name.front() == ' ' || name.back() == ' ')
		return false;
	for (const char c : name)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20 || u == 0x7F || c == '/' || c == '\\')
			return false;
	}
	return true;
}

bool WebServer::Start(bool require_code)
{
	Stop();
	for (const Folder& folder : m_folders)
	{
		std::error_code ec;
		std::filesystem::create_directories(folder.path, ec);
	}

	m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_listen_fd < 0)
	{
		std::lock_guard lock(m_mutex);
		m_error = "Cannot open a network socket.";
		return false;
	}
	const int one = 1;
	setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	fcntl(m_listen_fd, F_SETFD, FD_CLOEXEC);
	m_port = 0;
	for (int port = FIRST_PORT; port < FIRST_PORT + PORT_TRIES && !m_port; port++)
	{
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY); // clients are filtered on accept
		addr.sin_port = htons(static_cast<uint16_t>(port));
		if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
			m_port = port;
	}
	if (!m_port || listen(m_listen_fd, 16) != 0)
	{
		close(m_listen_fd);
		m_listen_fd = -1;
		std::lock_guard lock(m_mutex);
		m_error = "No free network port (8080-8089).";
		return false;
	}

	{
		std::lock_guard lock(m_mutex);
		m_error.clear();
		m_address = LocalAddress();
		m_code = require_code ? NewCode() : std::string();
		m_sessions.clear();
		m_failures.clear();
	}
	m_stop = false;
	m_accept_thread = std::thread(&WebServer::AcceptLoop, this);
	Console.WriteLnFmt("Web transfer: listening on port {} ({}).", m_port, m_address.empty() ? "no network address" : m_address);
	return true;
}

void WebServer::Stop()
{
	if (!m_accept_thread.joinable())
		return;
	m_stop = true;
	m_accept_thread.join();
	close(m_listen_fd);
	m_listen_fd = -1;
	std::vector<std::unique_ptr<Connection>> connections;
	{
		std::lock_guard lock(m_mutex);
		// Wake connections blocked in recv / send; each closes its own socket.
		for (const auto& connection : m_connections)
		{
			if (connection->fd >= 0)
				shutdown(connection->fd, SHUT_RDWR);
		}
		connections = std::move(m_connections);
		m_connections.clear();
	}
	for (const auto& connection : connections)
		connection->thread.join();
	std::lock_guard lock(m_mutex);
	m_code.clear();
	m_sessions.clear();
	m_transfer.clear();
	Console.WriteLn("Web transfer: stopped.");
}

std::string WebServer::NewCode()
{
	std::random_device rd;
	std::uniform_int_distribution<int> digit(0, 9);
	std::string code;
	for (int i = 0; i < 6; i++)
		code += static_cast<char>('0' + digit(rd));
	return code;
}

void WebServer::SetRequireCode(bool require_code)
{
	std::lock_guard lock(m_mutex);
	m_code = require_code && m_accept_thread.joinable() ? NewCode() : std::string();
	m_sessions.clear();
	m_failures.clear();
}

void WebServer::SetProtectedFile(std::string path)
{
	std::lock_guard lock(m_mutex);
	m_protected_file = std::move(path);
}

WebServer::Status WebServer::GetStatus() const
{
	Status status;
	std::lock_guard lock(m_mutex);
	status.running = m_accept_thread.joinable();
	status.error = m_error;
	if (status.running)
	{
		if (!m_address.empty())
			status.url = "http://" + m_address + (m_port == 80 ? "" : ":" + std::to_string(m_port));
		status.code = m_code;
		status.transfer = m_transfer;
		status.progress = m_transfer_size ? static_cast<float>(static_cast<double>(m_transfer_done) /
		                                                       static_cast<double>(m_transfer_size)) :
		                                    0.0f;
	}
	return status;
}

void WebServer::AcceptLoop()
{
	int64_t next_address_check = 0;
	while (!m_stop)
	{
		// The Wi-Fi address can come and go while the server runs.
		if (const int64_t now = NowMs(); now >= next_address_check)
		{
			next_address_check = now + 3000;
			std::string address = LocalAddress();
			std::lock_guard lock(m_mutex);
			m_address = std::move(address);
		}

		pollfd pfd{m_listen_fd, POLLIN, 0};
		if (poll(&pfd, 1, 250) <= 0)
			continue;
		sockaddr_in addr{};
		socklen_t len = sizeof(addr);
		const int fd = accept(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		const uint32_t peer = ntohl(addr.sin_addr.s_addr);
		if (addr.sin_family != AF_INET || !IsLocalAddress(peer))
		{
			close(fd);
			continue;
		}
#ifdef SO_NOSIGPIPE
		const int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		timeval tv{IO_TIMEOUT_S, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		std::lock_guard lock(m_mutex);
		// Reap finished connections.
		for (auto it = m_connections.begin(); it != m_connections.end();)
		{
			if ((*it)->done)
			{
				(*it)->thread.join();
				it = m_connections.erase(it);
			}
			else
				++it;
		}
		if (m_connections.size() >= MAX_CONNECTIONS)
		{
			RespondError(fd, 503, "Too many connections; try again.");
			close(fd);
			continue;
		}
		auto connection = std::make_unique<Connection>();
		connection->fd = fd;
		connection->peer = peer;
		Connection* raw = connection.get();
		m_connections.push_back(std::move(connection));
		raw->thread = std::thread(&WebServer::Serve, this, raw);
	}
}

void WebServer::Serve(Connection* connection)
{
	const int fd = connection->fd;
	Request request;
	std::string head;
	char buf[4096];
	size_t end = std::string::npos;
	while (end == std::string::npos && head.size() < MAX_HEAD)
	{
		const ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		head.append(buf, static_cast<size_t>(n));
		end = head.find("\r\n\r\n");
	}
	if (end != std::string::npos)
	{
		request.body_start = head.substr(end + 4);
		head.resize(end);
		// Request line, then headers.
		size_t line_end = head.find("\r\n");
		const std::string line = head.substr(0, line_end);
		const size_t s1 = line.find(' '), s2 = line.rfind(' ');
		bool ok = s1 != std::string::npos && s2 > s1;
		if (ok)
		{
			request.method = line.substr(0, s1);
			const std::string target = line.substr(s1 + 1, s2 - s1 - 1);
			const size_t q = target.find('?');
			request.path = target.substr(0, q);
			if (q != std::string::npos)
				request.query = target.substr(q + 1);
		}
		while (ok && line_end != std::string::npos)
		{
			const size_t start = line_end + 2;
			line_end = head.find("\r\n", start);
			const std::string header = head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
			const size_t colon = header.find(':');
			if (colon == std::string::npos)
				continue;
			std::string value = header.substr(colon + 1);
			value.erase(0, value.find_first_not_of(" \t"));
			value.erase(value.find_last_not_of(" \t") + 1);
			request.headers[Lower(header.substr(0, colon))] = std::move(value);
		}
		if (const std::string length = request.Header("content-length"); ok && !length.empty())
		{
			char* stop = nullptr;
			request.content_length = std::strtoull(length.c_str(), &stop, 10);
			request.has_length = stop && *stop == '\0' && std::isdigit(static_cast<unsigned char>(length[0]));
			ok = request.has_length;
		}
		if (ok)
			Handle(fd, connection->peer, request);
		else
			RespondError(fd, 400, "Malformed request.");
	}
	else if (head.size() >= MAX_HEAD)
		RespondError(fd, 400, "Request too large.");

	std::lock_guard lock(m_mutex);
	close(fd);
	connection->fd = -1;
	connection->done = true;
}

const WebServer::Folder* WebServer::FindFolder(std::string_view id) const
{
	for (const Folder& folder : m_folders)
	{
		if (folder.id == id)
			return &folder;
	}
	return nullptr;
}

bool WebServer::Authorized(const Request& request) const
{
	std::lock_guard lock(m_mutex);
	if (m_code.empty())
		return true;
	const std::string cookies = request.Header("cookie");
	for (const std::string& session : m_sessions)
	{
		if (cookies.find("ps2v_session=" + session) != std::string::npos)
			return true;
	}
	return false;
}

void WebServer::Handle(int fd, uint32_t peer, Request& request)
{
	// DNS rebinding: a page from elsewhere reaching us through its own host name.
	const std::string host = request.Header("host");
	if (!IsLocalHostName(host))
	{
		RespondError(fd, 403, "Open the page with the headset's address.");
		return;
	}
	// Cross-site requests: a page from elsewhere cannot set this header without a CORS preflight,
	// which this server never allows.
	const bool safe = request.method == "GET";
	if (!safe)
	{
		const std::string origin = request.Header("origin");
		if (request.Header("x-requested-with") != "ps2v" || (!origin.empty() && Lower(origin) != "http://" + Lower(host)))
		{
			RespondError(fd, 403, "Cross-site request refused.");
			return;
		}
	}

	if (safe && (request.path == "/" || request.path == "/index.html"))
	{
		Respond(fd, 200, "text/html; charset=utf-8", s_web_page,
			"Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
			"img-src 'self' data:\r\n");
		return;
	}
	if (safe && request.path == "/api/status")
	{
		bool required;
		{
			std::lock_guard lock(m_mutex);
			required = !m_code.empty();
		}
		RespondJson(fd, 200,
			std::string("{\"code_required\":") + (required ? "true" : "false") +
				",\"authorized\":" + (Authorized(request) ? "true" : "false") + "}");
		return;
	}
	if (request.path == "/api/login")
	{
		if (request.method != "POST")
			RespondError(fd, 405, "Use POST.");
		else
			HandleLogin(fd, peer, request);
		return;
	}

	constexpr std::string_view files_prefix = "/api/files/";
	if (request.path.compare(0, files_prefix.size(), files_prefix) != 0)
	{
		RespondError(fd, 404, "Not found.");
		return;
	}
	if (!Authorized(request))
	{
		RespondError(fd, 401, "The access code is needed.");
		return;
	}
	const std::string rest = request.path.substr(files_prefix.size());
	const size_t slash = rest.find('/');
	const Folder* folder = FindFolder(rest.substr(0, slash));
	if (!folder)
	{
		RespondError(fd, 404, "No such folder.");
		return;
	}
	if (slash == std::string::npos)
	{
		if (safe)
			HandleList(fd, *folder);
		else
			RespondError(fd, 405, "Use GET.");
		return;
	}
	bool decoded = false;
	const std::string name = UrlDecode(rest.substr(slash + 1), &decoded);
	if (!decoded || !IsValidFileName(name))
	{
		RespondError(fd, 400, "That file name is not allowed.");
		return;
	}
	if (request.method == "PUT")
		HandleUpload(fd, *folder, name, request);
	else if (request.method == "DELETE")
		HandleDelete(fd, *folder, name);
	else
		RespondError(fd, 405, "Use PUT or DELETE.");
}

void WebServer::HandleLogin(int fd, uint32_t peer, Request& request)
{
	std::string body = request.body_start;
	if (!request.has_length || request.content_length > 256)
	{
		RespondError(fd, 400, "Bad login request.");
		return;
	}
	char buf[256];
	while (body.size() < request.content_length)
	{
		const ssize_t n = recv(fd, buf, std::min(sizeof(buf), static_cast<size_t>(request.content_length - body.size())), 0);
		if (n <= 0)
			return;
		body.append(buf, static_cast<size_t>(n));
	}
	std::string digits;
	for (const char c : body)
	{
		if (std::isdigit(static_cast<unsigned char>(c)))
			digits += c;
	}

	std::string token;
	int64_t wait_ms = 0;
	{
		std::lock_guard lock(m_mutex);
		Failures& failures = m_failures[peer];
		const int64_t now = NowMs();
		if (now < failures.blocked_until_ms)
			wait_ms = failures.blocked_until_ms - now;
		else if (m_code.empty())
			token = "-"; // no code to check: nothing to remember either
		else
		{
			// Compare without an early exit.
			unsigned diff = digits.size() != m_code.size();
			for (size_t i = 0; i < m_code.size(); i++)
				diff |= static_cast<unsigned>(i < digits.size() ? digits[i] ^ m_code[i] : 1);
			if (diff == 0)
			{
				token = RandomHex(16);
				m_sessions.push_back(token);
				if (m_sessions.size() > MAX_SESSIONS)
					m_sessions.erase(m_sessions.begin());
				failures = {};
			}
			else if (++failures.count >= CODE_TRIES)
			{
				// 30 s, then a little longer with each further miss.
				failures.blocked_until_ms = now + 30000 * (failures.count - CODE_TRIES + 1);
				wait_ms = failures.blocked_until_ms - now;
			}
		}
	}
	if (wait_ms > 0)
	{
		RespondJson(fd, 429,
			"{\"error\":\"Too many wrong codes. Wait a moment.\",\"retry\":" + std::to_string((wait_ms + 999) / 1000) + "}");
		return;
	}
	if (token.empty())
	{
		RespondError(fd, 401, "Wrong code.");
		return;
	}
	if (token == "-")
		RespondJson(fd, 200, "{\"ok\":true}");
	else
		RespondJson(fd, 200, "{\"ok\":true}", "Set-Cookie: ps2v_session=" + token + "; Path=/; HttpOnly; SameSite=Strict\r\n");
}

void WebServer::HandleList(int fd, const Folder& folder)
{
	struct Entry
	{
		std::string name;
		uint64_t size;
		int64_t mtime;
	};
	std::vector<Entry> entries;
	std::error_code ec;
	for (const auto& item : std::filesystem::directory_iterator(folder.path, ec))
	{
		const std::string name = item.path().filename().string();
		struct stat st;
		if (name.empty() || name[0] == '.' || stat(item.path().c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		entries.push_back({name, static_cast<uint64_t>(st.st_size), static_cast<int64_t>(st.st_mtime)});
	}
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return Lower(a.name) < Lower(b.name); });

	uint64_t free = 0, total = 0;
	DiskSpace(folder.path, &free, &total);
	std::string json = "{\"files\":[";
	for (size_t i = 0; i < entries.size(); i++)
	{
		json += i ? "," : "";
		json += "{\"name\":" + JsonString(entries[i].name) + ",\"size\":" + std::to_string(entries[i].size) +
		        ",\"mtime\":" + std::to_string(entries[i].mtime) + "}";
	}
	json += "],\"free\":" + std::to_string(free) + ",\"total\":" + std::to_string(total) + ",\"extensions\":[";
	for (size_t i = 0; i < folder.extensions.size(); i++)
		json += (i ? "," : "") + JsonString(folder.extensions[i]);
	json += "]}";
	RespondJson(fd, 200, json);
}

void WebServer::HandleUpload(int fd, const Folder& folder, const std::string& name, Request& request)
{
	const std::string lower = Lower(name);
	const bool accepted = std::any_of(folder.extensions.begin(), folder.extensions.end(), [&](const std::string& ext) {
		return lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0;
	});
	if (!accepted)
	{
		RespondError(fd, 400, "This kind of file does not belong here.");
		return;
	}
	if (!request.has_length)
	{
		RespondError(fd, 411, "The upload needs a Content-Length.");
		return;
	}
	if (!request.Header("transfer-encoding").empty())
	{
		RespondError(fd, 400, "Chunked uploads are not supported.");
		return;
	}
	uint64_t free = 0, total = 0;
	if (DiskSpace(folder.path, &free, &total) && request.content_length + SPACE_MARGIN > free)
	{
		RespondError(fd, 507, "Not enough space left on the headset.");
		return;
	}
	const std::string path = folder.path + "/" + name;
	if (std::filesystem::exists(path) && request.query.find("overwrite=1") == std::string::npos)
	{
		RespondError(fd, 409, "A file with this name is already there.");
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		if (!m_transfer.empty())
		{
			RespondError(fd, 503, "Another upload is in progress.");
			return;
		}
		m_transfer = name;
		m_transfer_done = 0;
		m_transfer_size = request.content_length;
	}

	const std::string part = folder.path + "/." + name + ".part";
	std::FILE* file = std::fopen(part.c_str(), "wb");
	bool ok = file != nullptr;
	uint64_t written = 0;
	if (ok && !request.body_start.empty())
	{
		const size_t n = static_cast<size_t>(std::min<uint64_t>(request.body_start.size(), request.content_length));
		ok = std::fwrite(request.body_start.data(), 1, n, file) == n;
		written = n;
	}
	std::vector<char> buf(1 << 20);
	while (ok && written < request.content_length && !m_stop)
	{
		const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), request.content_length - written));
		const ssize_t n = recv(fd, buf.data(), want, 0);
		if (n <= 0)
		{
			ok = false;
			break;
		}
		ok = std::fwrite(buf.data(), 1, static_cast<size_t>(n), file) == static_cast<size_t>(n);
		written += static_cast<uint64_t>(n);
		std::lock_guard lock(m_mutex);
		m_transfer_done = written;
	}
	if (file)
	{
		ok = ok && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
		ok = std::fclose(file) == 0 && ok;
	}
	ok = ok && written == request.content_length;
	std::error_code ec;
	if (ok)
		std::filesystem::rename(part, path, ec);
	if (!ok || ec)
		std::filesystem::remove(part, ec);
	{
		std::lock_guard lock(m_mutex);
		m_transfer.clear();
	}
	if (ok && !ec)
	{
		Console.WriteLnFmt("Web transfer: received {} ({} bytes).", path, written);
		RespondJson(fd, 200, "{\"ok\":true}");
	}
	else if (written == request.content_length)
		RespondError(fd, 500, "Could not write the file on the headset.");
	// Otherwise the connection broke off: nobody is left to answer.
}

void WebServer::HandleDelete(int fd, const Folder& folder, const std::string& name)
{
	const std::string path = folder.path + "/" + name;
	{
		std::lock_guard lock(m_mutex);
		std::error_code ec;
		if (!m_protected_file.empty() && std::filesystem::equivalent(path, m_protected_file, ec))
		{
			RespondError(fd, 409, "This game is running; it cannot be deleted now.");
			return;
		}
	}
	struct stat st;
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		RespondError(fd, 404, "No such file.");
		return;
	}
	if (std::remove(path.c_str()) != 0)
	{
		RespondError(fd, 500, "Could not delete the file.");
		return;
	}
	Console.WriteLnFmt("Web transfer: deleted {}.", path);
	RespondJson(fd, 200, "{\"ok\":true}");
}
