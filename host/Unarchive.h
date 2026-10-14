// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Unpacks the archives the Wi-Fi transfer receives: zip, 7z and RAR (RAR4 and RAR5), with
// libarchive. Encrypted and multi-volume archives are not supported.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Unarchive
{
	// .zip, .7z, .rar
	const std::vector<std::string>& Extensions();
	// Whether the file name ends with one of them (any case).
	bool IsArchive(std::string_view name);

	struct Entry
	{
		std::string name; // its path in the archive, as stored ('/' or '\' separated)
		uint64_t size = 0; // as the archive says, 0 when it does not
		bool regular = false; // a plain file (not a folder, link or device)
		bool link = false; // a symbolic or hard link
	};

	// What to do with an entry.
	struct Decision
	{
		enum class Action
		{
			Skip,
			Extract,
			Fail, // the whole archive is refused
		};
		Action action = Action::Skip;
		std::string path; // Extract: where, relative to the output folder ('/' separated)
		std::string error; // Fail: why
		uint64_t max_size = 0; // Extract: the most bytes the file may have, 0 for no limit

		static Decision Skip() { return {}; }
		static Decision Extract(std::string path, uint64_t max_size = 0) { return {Action::Extract, std::move(path), {}, max_size}; }
		static Decision Fail(std::string error) { return {Action::Fail, {}, std::move(error), 0}; }
	};
	using Filter = std::function<Decision(const Entry& entry)>;

	// Extracts the entries the filter takes into out_dir, made if needed. Writes at most max_bytes
	// in all (the space there is). progress gets 0..1 now and then; a stop set to true aborts.
	// On failure error says why, and what was written so far stays (the caller removes out_dir).
	bool Extract(const std::string& archive, const std::string& out_dir, const Filter& filter, uint64_t max_bytes,
		const std::function<void(float)>& progress, const std::atomic<bool>* stop, std::string* error);
} // namespace Unarchive
