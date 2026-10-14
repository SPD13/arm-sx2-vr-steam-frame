// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "TexturePacks.h"

#include "Unarchive.h"

// ARMSX2's archive path rules, from its iOS app (a port of the Android app's TextureArchivePath).
#include "IOS/TexturePackPaths.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
	// The core's folder name (GSTextureReplacements.cpp).
	constexpr const char* REPLACEMENTS = "replacements";
	// A removed pack while its files are deleted: beside the game folders, so moving it there is
	// one rename, and the dot keeps it from reading as a serial (as ARMSX2 Android does).
	constexpr std::string_view REMOVED_PREFIX = ".deleting-";
	// ARMSX2's limits: a bigger file is malformed rather than large, and so is a pack of more entries.
	constexpr uint64_t MAX_TEXTURE_BYTES = 512ull << 20;
	constexpr uint64_t MAX_ENTRIES = 100000;

	bool IsTextureFile(const std::string& name)
	{
		const std::string lower = TexturePackPaths::Lower(name);
		for (const char* ext : {".png", ".dds", ".astc", ".ktx"})
		{
			const size_t n = std::char_traits<char>::length(ext);
			if (lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0)
				return true;
		}
		return false;
	}

	// A rename, or a copy when the two are on different file systems.
	bool MoveFile(const fs::path& from, const fs::path& to)
	{
		std::error_code ec;
		fs::rename(from, to, ec);
		if (!ec)
			return true;
		if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec))
			return false;
		fs::remove(from, ec);
		return true;
	}
} // namespace

std::vector<TexturePacks::Pack> TexturePacks::List(const std::string& root)
{
	std::vector<Pack> packs;
	std::error_code ec;
	for (const fs::directory_entry& game : fs::directory_iterator(root, ec))
	{
		const std::string serial = game.path().filename().string();
		const fs::path replacements = game.path() / REPLACEMENTS;
		if (serial.empty() || serial[0] == '.' || !fs::is_directory(replacements, ec))
			continue;
		Pack pack;
		pack.serial = serial;
		for (auto it = fs::recursive_directory_iterator(replacements, ec); !ec && it != fs::recursive_directory_iterator();
			 it.increment(ec))
		{
			struct stat st;
			if (lstat(it->path().c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			pack.bytes += static_cast<uint64_t>(st.st_size);
			pack.textures += IsTextureFile(it->path().filename().string()) ? 1 : 0;
			pack.modified = std::max<std::time_t>(pack.modified, st.st_ctime);
		}
		ec.clear();
		packs.push_back(std::move(pack));
	}
	std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.serial < b.serial; });
	return packs;
}

bool TexturePacks::Remove(const std::string& root, const std::string& serial, std::string* error)
{
	const fs::path game = fs::path(root) / serial;
	const fs::path replacements = game / REPLACEMENTS;
	std::error_code ec;
	if (!fs::is_directory(replacements, ec))
	{
		*error = "No texture pack for " + serial + ".";
		return false;
	}
	// Out of the core's way in one step; the files go after.
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path removed = fs::path(root) / (std::string(REMOVED_PREFIX) + serial + "-" + std::to_string(now));
	fs::rename(replacements, removed, ec);
	fs::remove_all(ec ? replacements : removed, ec);
	if (fs::exists(replacements))
	{
		*error = "Could not delete the texture pack for " + serial + ".";
		return false;
	}
	if (fs::is_empty(game, ec))
		fs::remove(game, ec);
	return true;
}

void TexturePacks::Sweep(const std::string& root)
{
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::directory_iterator(root, ec))
	{
		if (entry.path().filename().string().starts_with(REMOVED_PREFIX))
			fs::remove_all(entry.path(), ec);
	}
}

std::optional<std::string> TexturePacks::FindSerial(std::string_view text)
{
	return TexturePackPaths::FindSerial(std::string(text));
}

bool TexturePacks::Unpack(const std::string& archive, const std::string& dir, uint64_t max_bytes,
	const std::function<void(float)>& progress, const std::atomic<bool>* stop, Staged* staged, std::string* error)
{
	staged->dir = dir;
	staged->paths.clear();
	staged->serial.reset();
	uint64_t entries = 0;
	std::unordered_set<std::string> destinations;
	const auto filter = [&](const Unarchive::Entry& entry) {
		using Decision = Unarchive::Decision;
		if (++entries > MAX_ENTRIES)
			return Decision::Fail("The archive has more than 100000 entries.");
		const TexturePackPaths::Entry classified = TexturePackPaths::Classify(entry.name);
		if (classified.kind == TexturePackPaths::Kind::Unsafe || entry.link)
			return Decision::Fail("The archive contains an unsafe entry: " + entry.name);
		if (!staged->serial)
			staged->serial = TexturePackPaths::SerialFolderIn(entry.name);
		if (classified.kind != TexturePackPaths::Kind::Texture || !entry.regular)
			return Decision::Skip();
		// Two entries for one file (by case too: the pack may come from a case-blind file system).
		if (!destinations.insert(TexturePackPaths::Lower(classified.path)).second)
			return Decision::Fail("The archive holds two files for " + classified.path + ".");
		staged->paths.push_back(classified.path);
		return Decision::Extract(classified.path, MAX_TEXTURE_BYTES);
	};
	if (!Unarchive::Extract(archive, dir, filter, max_bytes, progress, stop, error))
		return false;
	if (staged->paths.empty())
	{
		*error = "The archive has no PNG, DDS, ASTC or KTX textures.";
		return false;
	}
	return true;
}

bool TexturePacks::Install(const std::string& root, const Staged& staged, const std::string& serial, std::string* error)
{
	const fs::path target = fs::path(root) / serial / REPLACEMENTS;
	std::error_code ec;
	bool ok = true;
	for (const std::string& path : staged.paths)
	{
		const fs::path to = target / path;
		fs::create_directories(to.parent_path(), ec);
		if (!MoveFile(fs::path(staged.dir) / path, to))
		{
			*error = "Could not install " + path + ".";
			ok = false;
			break;
		}
	}
	fs::remove_all(staged.dir, ec);
	return ok;
}
