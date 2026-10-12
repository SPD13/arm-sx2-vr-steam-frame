// SPDX-FileCopyrightText: 2026 PS2 Volumetric contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Settings.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "pcsx2/INISettingsInterface.h"

#include <algorithm>
#include <array>

static constexpr std::array<VrSettingInfo, 11> s_settings = {{
	{"Screen", "screen_width", &VrSettings::screen_width, nullptr, 0.3f, 10.0f, false},
	{"Screen", "screen_distance", &VrSettings::screen_distance, nullptr, 0.3f, 10.0f, false},
	{"Mode", "flat", nullptr, &VrSettings::flat, 0.0f, 0.0f, true},
	{"Mode", "stereo", nullptr, &VrSettings::stereo, 0.0f, 0.0f, true},
	{"Window", "depth_range", &VrSettings::depth_range, nullptr, 0.0f, 5.0f, false},
	{"Window", "pop_out", &VrSettings::pop_out, nullptr, 0.0f, 1.0f, false},
	{"Window", "hud_overlay", nullptr, &VrSettings::hud_overlay, 0.0f, 0.0f, false},
	{"Window", "edge_fill", nullptr, &VrSettings::edge_fill, 0.0f, 0.0f, false},
	{"Stereo", "stereo_depth", &VrSettings::stereo_depth, nullptr, 0.0f, 20.0f, false},
	{"Stereo", "stereo_convergence", &VrSettings::stereo_convergence, nullptr, 0.001f, 100.0f, false},
	{"Stereo", "stereo_z_mode", nullptr, &VrSettings::stereo_z_mode, 0.0f, 0.0f, false},
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
	return info.number ? a.*info.number == b.*info.number : a.*info.toggle == b.*info.toggle;
}

VrSettingsStore::VrSettingsStore() = default;

VrSettingsStore::~VrSettingsStore() = default;

void VrSettingsStore::ReadIni(const INISettingsInterface& ini, VrSettings* settings) const
{
	for (const VrSettingInfo& info : s_settings)
	{
		if (info.number)
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
	if (info->number)
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
		if (info.number)
		{
			m_current.*info.number = std::clamp(settings.*info.number, info.min, info.max);
			m_ini->SetFloatValue(info.section, info.key, m_current.*info.number);
		}
		else
		{
			m_current.*info.toggle = settings.*info.toggle;
			m_ini->SetBoolValue(info.section, info.key, m_current.*info.toggle);
		}
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
					if (info.number)
					{
						m_current.*info.number = after.*info.number;
						m_ini->SetFloatValue(info.section, info.key, after.*info.number);
					}
					else
					{
						m_current.*info.toggle = after.*info.toggle;
						m_ini->SetBoolValue(info.section, info.key, after.*info.toggle);
					}
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
