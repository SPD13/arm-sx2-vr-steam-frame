# SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
# SPDX-License-Identifier: GPL-3.0+

# Turns web/index.html into a C++ header holding it as s_web_page.
# Usage: cmake -DINPUT=index.html -DOUTPUT=web_page.h -P embed.cmake
file(READ "${INPUT}" content)
file(WRITE "${OUTPUT}" "// Generated from web/index.html by web/embed.cmake.\n#pragma once\nstatic constexpr char s_web_page[] = R\"PS2V_HTML(${content})PS2V_HTML\";\n")
