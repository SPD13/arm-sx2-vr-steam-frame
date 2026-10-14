// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// HD texture packs, kept the way ARMSX2 keeps them: the replacement textures of the game with
// serial S are in <textures>/S/replacements and its sub-folders, where the core looks for them
// (EmuFolders::Textures, GSTextureReplacements). A pack is paired with a game by that serial.
//
// Archives are unpacked with the rules of ARMSX2's Android and iOS apps (TexturePackPaths.h, shared
// with the iOS app): the wrappers a pack comes in (S/, S/replacements/, a GitHub root folder) are
// dropped, only the PNG, DDS, ASTC and KTX files the core loads are kept, an unsafe entry refuses the
// whole archive, and the files merge over the game's pack, so an add-on keeps its base pack.

#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace TexturePacks
{
	struct Pack
	{
		std::string serial; // its folder's name
		uint64_t textures = 0; // files the core can load
		uint64_t bytes = 0;
		std::time_t modified = 0; // when files last arrived
	};
	// The packs in the textures folder root, by serial.
	std::vector<Pack> List(const std::string& root);
	// The serials whose pack holds at least one texture (quicker than List: it stops at the first).
	std::set<std::string> Serials(const std::string& root);
	// Deletes the game's pack (its replacements folder; texture dumps beside it stay).
	bool Remove(const std::string& root, const std::string& serial, std::string* error);
	// Finishes deleting what an interrupted Remove left.
	void Sweep(const std::string& root);

	// The serial a pack or file name holds, as ARMSX2 reads it: "slus_212.87", "SLUS 21287" and
	// "slus-21287" are SLUS-21287.
	std::optional<std::string> FindSerial(std::string_view text);

	// An archive's textures unpacked into a staging folder, before they go to a game's pack.
	struct Staged
	{
		std::string dir;
		std::vector<std::string> paths; // relative to dir and to the pack's replacements folder
		std::optional<std::string> serial; // from a serial folder in the archive
	};
	bool Unpack(const std::string& archive, const std::string& dir, uint64_t max_bytes,
		const std::function<void(float)>& progress, const std::atomic<bool>* stop, Staged* staged, std::string* error);
	// Moves the staged textures into the pack of the game with this serial (merging), then deletes
	// the staging folder.
	bool Install(const std::string& root, const Staged& staged, const std::string& serial, std::string* error);
} // namespace TexturePacks
