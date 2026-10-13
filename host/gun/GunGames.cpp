// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunGames.h"

#include "Emu.h"
#include "Profile.h"

#include "common/Console.h"
#include "common/Error.h"

#include "pcsx2/INISettingsInterface.h"

namespace
{
	constexpr const char* SECTION = "Games";
}

GunGames::GunGames() = default;

GunGames::~GunGames() = default;

void GunGames::Load(std::string path)
{
	std::lock_guard lock(m_mutex);
	m_ini = std::make_unique<INISettingsInterface>(std::move(path));
	m_ini->Load();
}

bool GunGames::IsAuto(const std::string& serial)
{
	if (serial.empty())
		return false;
	std::lock_guard lock(m_mutex);
	const auto it = m_auto.find(serial);
	if (it != m_auto.end())
		return it->second;
	const bool on = Emu::IsLightgunGame(serial) || LoadGameProfile(serial).lightgun;
	m_auto.emplace(serial, on);
	return on;
}

bool GunGames::IsOn(const std::string& serial)
{
	if (serial.empty())
		return false;
	{
		std::lock_guard lock(m_mutex);
		bool on;
		if (m_ini && m_ini->GetBoolValue(SECTION, serial.c_str(), &on))
			return on;
	}
	return IsAuto(serial);
}

bool GunGames::IsChosen(const std::string& serial)
{
	std::lock_guard lock(m_mutex);
	return m_ini && !serial.empty() && m_ini->ContainsValue(SECTION, serial.c_str());
}

void GunGames::Set(const std::string& serial, bool on)
{
	if (serial.empty())
		return;
	std::lock_guard lock(m_mutex);
	if (!m_ini)
		return;
	m_ini->SetBoolValue(SECTION, serial.c_str(), on);
	Save();
}

void GunGames::Reset(const std::string& serial)
{
	std::lock_guard lock(m_mutex);
	if (!m_ini || serial.empty())
		return;
	m_ini->DeleteValue(SECTION, serial.c_str());
	Save();
}

void GunGames::Save()
{
	Error error;
	if (!m_ini->Save(&error))
		Console.ErrorFmt("Light gun: cannot save {}: {}", m_ini->GetFileName(), error.GetDescription());
}
