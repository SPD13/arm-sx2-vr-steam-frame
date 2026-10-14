// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Settings.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "pcsx2/INISettingsInterface.h"

#include <algorithm>
#include <array>
#include <cstdio>

static constexpr std::array<VrSettingInfo, 18> s_settings = {{
	{"Screen", "screen_width", &VrSettings::screen_width, nullptr, 0.3f, 10.0f, false},
	{"Screen", "screen_distance", &VrSettings::screen_distance, nullptr, 0.3f, 10.0f, false},
	{"Mode", "stereo", nullptr, &VrSettings::stereo, 0.0f, 0.0f, false},
	{"Mode", "passthrough", nullptr, &VrSettings::passthrough, 0.0f, 0.0f, false},
	{"Stereo", "stereo_depth", &VrSettings::stereo_depth, nullptr, 0.0f, 20.0f, false},
	{"Stereo", "stereo_convergence", &VrSettings::stereo_convergence, nullptr, 0.01f, 1.0f, false},
	{"Web", "web_enabled", nullptr, &VrSettings::web_enabled, 0.0f, 0.0f, false},
	{"Web", "web_require_code", nullptr, &VrSettings::web_require_code, 0.0f, 0.0f, false},
	{"Controllers", "show_controllers", nullptr, &VrSettings::show_controllers, 0.0f, 0.0f, false},
	{"LightGun", "gun_players", &VrSettings::gun_players, nullptr, 1.0f, 2.0f, false},
	{"LightGun", "gun_left_hand", nullptr, &VrSettings::gun_left_hand, 0.0f, 0.0f, false},
	{"LightGun", "gun_beam", &VrSettings::gun_beam, nullptr, 0.0f, 2.0f, false},
	{"LightGun", "gun_pedal", &VrSettings::gun_pedal, nullptr, 0.0f, 2.0f, false},
	{"LightGun", "gun_haptics", &VrSettings::gun_haptics, nullptr, 0.0f, 1.0f, false},
	{"LightGun", "gun_pitch", &VrSettings::gun_pitch, nullptr, -45.0f, 45.0f, false},
	{"LightGun", "gun_color", nullptr, nullptr, 0.0f, 0.0f, false, &VrSettings::gun_color},
	{"LightGun", "gun_color_2p", nullptr, nullptr, 0.0f, 0.0f, false, &VrSettings::gun_color_2p},
	{"LightGun", "gun2_color", nullptr, nullptr, 0.0f, 0.0f, false, &VrSettings::gun2_color},
}};

std::span<const VrSettingInfo> VrSettingList()
{
	return s_settings;
}

const VrSettingInfo* FindVrSetting(std::string_view key)
{
	for (const VrSettingInfo& info : s_settings)
	{
		if (key == info.key)
			return &info;
	}
	return nullptr;
}

bool SameVrSetting(const VrSettingInfo& info, const VrSettings& a, const VrSettings& b)
{
	if (info.color)
		return a.*info.color == b.*info.color;
	return info.number ? a.*info.number == b.*info.number : a.*info.toggle == b.*info.toggle;
}

std::optional<uint32_t> ParseColor(std::string_view text)
{
	if (!text.empty() && text.front() == '#')
		text.remove_prefix(1);
	if (text.size() != 6)
		return std::nullopt;
	const std::optional<uint32_t> rgb = StringUtil::FromChars<uint32_t>(text, 16);
	return rgb && *rgb <= 0xFFFFFFu ? rgb : std::nullopt;
}

std::string FormatColor(uint32_t rgb)
{
	char text[8];
	std::snprintf(text, sizeof(text), "#%06x", rgb & 0xFFFFFFu);
	return text;
}

// Copies one setting from `from` to `to`, and into the ini.
static void StoreSetting(const VrSettingInfo& info, const VrSettings& from, VrSettings* to, INISettingsInterface* ini)
{
	if (info.color)
	{
		to->*info.color = from.*info.color;
		ini->SetStringValue(info.section, info.key, FormatColor(to->*info.color).c_str());
	}
	else if (info.number)
	{
		to->*info.number = std::clamp(from.*info.number, info.min, info.max);
		ini->SetFloatValue(info.section, info.key, to->*info.number);
	}
	else
	{
		to->*info.toggle = from.*info.toggle;
		ini->SetBoolValue(info.section, info.key, to->*info.toggle);
	}
}

VrSettingsStore::VrSettingsStore() = default;

VrSettingsStore::~VrSettingsStore() = default;

void VrSettingsStore::ReadIni(const INISettingsInterface& ini, VrSettings* settings) const
{
	for (const VrSettingInfo& info : s_settings)
	{
		if (info.color)
		{
			std::string value;
			if (ini.GetStringValue(info.section, info.key, &value))
			{
				if (const std::optional<uint32_t> rgb = ParseColor(value))
					settings->*info.color = *rgb;
			}
		}
		else if (info.number)
		{
			float value;
			if (ini.GetFloatValue(info.section, info.key, &value))
				settings->*info.number = std::clamp(value, info.min, info.max);
		}
		else
			ini.GetBoolValue(info.section, info.key, &(settings->*info.toggle));
	}
}

bool VrSettingsStore::StatIni(std::time_t* mtime, long long* size) const
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(m_ini->GetFileName().c_str(), &sd))
		return false;
	*mtime = sd.ModificationTime;
	*size = sd.Size;
	return true;
}

void VrSettingsStore::Load(std::string path)
{
	m_ini = std::make_unique<INISettingsInterface>(std::move(path));
	if (m_ini->Load())
	{
		ReadIni(*m_ini, &m_current);
		StatIni(&m_file_mtime, &m_file_size);
		Console.WriteLnFmt("VR settings from {}.", m_ini->GetFileName());
	}
	m_version++;
}

bool VrSettingsStore::Override(std::string_view key, std::string_view value)
{
	const VrSettingInfo* info = FindVrSetting(key);
	if (!info)
		return false;
	if (info->color)
	{
		const std::optional<uint32_t> rgb = ParseColor(value);
		if (!rgb)
			return false;
		m_current.*info->color = *rgb;
	}
	else if (info->number)
	{
		const std::optional<float> number = StringUtil::FromChars<float>(value);
		if (!number)
			return false;
		m_current.*info->number = std::clamp(*number, info->min, info->max);
	}
	else
	{
		const std::optional<bool> toggle = StringUtil::FromChars<bool>(value, 10);
		if (!toggle)
			return false;
		m_current.*info->toggle = *toggle;
	}
	m_version++;
	return true;
}

void VrSettingsStore::Set(const VrSettings& settings)
{
	bool changed = false;
	for (const VrSettingInfo& info : s_settings)
	{
		if (SameVrSetting(info, settings, m_current))
			continue;
		StoreSetting(info, settings, &m_current, m_ini.get());
		changed = true;
	}
	if (!changed)
		return;
	m_version++;
	m_dirty = true;
	m_save_at = Clock::now() + std::chrono::seconds(1);
}

void VrSettingsStore::Update(bool force)
{
	if (!m_ini)
		return;
	const Clock::time_point now = Clock::now();

	// Edited by hand: take the settings whose value in the file changed.
	if (now >= m_next_poll)
	{
		m_next_poll = now + std::chrono::seconds(1);
		std::time_t mtime;
		long long size;
		if (StatIni(&mtime, &size) && (mtime != m_file_mtime || size != m_file_size))
		{
			m_file_mtime = mtime;
			m_file_size = size;
			INISettingsInterface edited(m_ini->GetFileName());
			if (edited.Load())
			{
				VrSettings before, after;
				ReadIni(*m_ini, &before);
				ReadIni(edited, &after);
				bool changed = false;
				for (const VrSettingInfo& info : s_settings)
				{
					if (SameVrSetting(info, before, after))
						continue;
					Console.WriteLnFmt("VR settings: {} changed in {}", info.key, m_ini->GetFileName());
					StoreSetting(info, after, &m_current, m_ini.get());
					changed = true;
				}
				if (changed)
					m_version++;
			}
		}
	}

	if (m_dirty && (force || now >= m_save_at))
	{
		m_dirty = false;
		Error error;
		if (m_ini->Save(&error))
			StatIni(&m_file_mtime, &m_file_size);
		else
			Console.ErrorFmt("Failed to save {}: {}", m_ini->GetFileName(), error.GetDescription());
	}
}
