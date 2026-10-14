// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Unarchive.h"

#include <archive.h>
#include <archive_entry.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace
{
	struct ArchiveDeleter
	{
		void operator()(archive* a) const { archive_read_free(a); }
	};

	// Relative, '/' separated, and staying inside the folder.
	bool IsSafeRelativePath(std::string_view path)
	{
		if (path.empty() || path.front() == '/' || path.back() == '/')
			return false;
		size_t start = 0;
		while (start <= path.size())
		{
			const size_t end = std::min(path.find('/', start), path.size());
			const std::string_view part = path.substr(start, end - start);
			if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string_view::npos)
				return false;
			start = end + 1;
		}
		return true;
	}

	std::string EntryName(archive_entry* entry)
	{
		// UTF-8 when the archive says how its names are encoded, else the bytes as stored.
		if (const char* utf8 = archive_entry_pathname_utf8(entry))
			return utf8;
		const char* raw = archive_entry_pathname(entry);
		return raw ? raw : std::string();
	}
} // namespace

const std::vector<std::string>& Unarchive::Extensions()
{
	static const std::vector<std::string> extensions = {".zip", ".7z", ".rar"};
	return extensions;
}

bool Unarchive::IsArchive(std::string_view name)
{
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	return std::any_of(Extensions().begin(), Extensions().end(), [&](const std::string& ext) {
		return lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0;
	});
}

bool Unarchive::Extract(const std::string& path, const std::string& out_dir, const Filter& filter, uint64_t max_bytes,
	const std::function<void(float)>& progress, const std::atomic<bool>* stop, std::string* error)
{
	std::unique_ptr<archive, ArchiveDeleter> a(archive_read_new());
	// By content, whatever the extension says.
	archive_read_support_format_zip(a.get());
	archive_read_support_format_7zip(a.get());
	archive_read_support_format_rar(a.get());
	archive_read_support_format_rar5(a.get());
	if (archive_read_open_filename(a.get(), path.c_str(), 1 << 20) != ARCHIVE_OK)
	{
		const char* why = archive_error_string(a.get());
		*error = std::string("Not a zip, 7z or RAR archive the headset can read") + (why ? std::string(" (") + why + ")" : "") + ".";
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(out_dir, ec);
	const uint64_t archive_size = std::max<uint64_t>(std::filesystem::file_size(path, ec), 1);
	std::vector<char> buf(1 << 20);
	uint64_t written = 0;
	archive_entry* header = nullptr;
	for (;;)
	{
		const int r = archive_read_next_header(a.get(), &header);
		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK && r != ARCHIVE_WARN)
		{
			const char* why = archive_error_string(a.get());
			*error = std::string("The archive is damaged") + (why ? std::string(": ") + why : ".");
			return false;
		}
		if (stop && stop->load())
		{
			*error = "Cancelled.";
			return false;
		}

		Entry entry;
		entry.name = EntryName(header);
		entry.size = archive_entry_size_is_set(header) ? static_cast<uint64_t>(std::max<la_int64_t>(archive_entry_size(header), 0)) : 0;
		entry.link = archive_entry_hardlink(header) != nullptr || archive_entry_filetype(header) == AE_IFLNK;
		entry.regular = !entry.link && archive_entry_filetype(header) == AE_IFREG;
		const Decision decision = filter(entry);
		if (decision.action == Decision::Action::Fail)
		{
			*error = decision.error;
			return false;
		}
		if (decision.action == Decision::Action::Skip)
			continue; // the next header skips its data
		if (!IsSafeRelativePath(decision.path))
		{
			*error = "The archive contains an unsafe path: " + entry.name;
			return false;
		}
		if (archive_entry_is_encrypted(header))
		{
			*error = "Password-protected archives are not supported.";
			return false;
		}

		const std::filesystem::path out = std::filesystem::path(out_dir) / decision.path;
		std::filesystem::create_directories(out.parent_path(), ec);
		std::FILE* file = std::fopen(out.c_str(), "wb");
		if (!file)
		{
			*error = "Could not write " + decision.path + " on the headset.";
			return false;
		}
		uint64_t size = 0;
		bool ok = true;
		for (;;)
		{
			const la_ssize_t n = archive_read_data(a.get(), buf.data(), buf.size());
			if (n == 0)
				break;
			if (n < 0)
			{
				const char* why = archive_error_string(a.get());
				*error = "Could not unpack " + entry.name + (why ? std::string(": ") + why : std::string("."));
				ok = false;
				break;
			}
			size += static_cast<uint64_t>(n);
			written += static_cast<uint64_t>(n);
			if (decision.max_size && size > decision.max_size)
			{
				*error = entry.name + " is too large.";
				ok = false;
				break;
			}
			if (written > max_bytes)
			{
				*error = "Not enough space left on the headset to unpack the archive.";
				ok = false;
				break;
			}
			if (std::fwrite(buf.data(), 1, static_cast<size_t>(n), file) != static_cast<size_t>(n))
			{
				*error = "Could not write " + decision.path + " on the headset.";
				ok = false;
				break;
			}
			if (stop && stop->load())
			{
				*error = "Cancelled.";
				ok = false;
				break;
			}
			if (progress)
				progress(std::min(static_cast<float>(static_cast<double>(archive_filter_bytes(a.get(), -1)) /
				                                     static_cast<double>(archive_size)),
					1.0f));
		}
		ok = std::fclose(file) == 0 && ok;
		if (!ok)
		{
			if (error->empty())
				*error = "Could not write " + decision.path + " on the headset.";
			return false;
		}
	}
#ifdef __linux__
	// One flush for all the files rather than one per file (a texture pack has thousands).
	if (const int dir = open(out_dir.c_str(), O_RDONLY | O_DIRECTORY); dir >= 0)
	{
		syncfs(dir);
		close(dir);
	}
#endif
	return true;
}
