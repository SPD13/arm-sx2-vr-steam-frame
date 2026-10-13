# PS2 Volumetric VR host. Added by ARMSX2's top-level CMakeLists through PS2V_HOST_DIR, so
# CMAKE_SOURCE_DIR is the ARMSX2 tree and the core targets (PCSX2, PCSX2_FLAGS) are in scope.

find_package(OpenXR CONFIG REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

add_executable(ps2v
	main.cpp
	Controls.cpp
	Controls.h
	Profile.cpp
	Profile.h
	DepthView.cpp
	DepthView.h
	VolumetricWindow.cpp
	VolumetricWindow.h
	Emu.cpp
	Emu.h
	ScreenLayer.cpp
	ScreenLayer.h
	Settings.cpp
	Settings.h
	XrApp.cpp
	XrApp.h
	XrMath.h
	ui/Canvas.cpp
	ui/Canvas.h
	ui/Font.cpp
	ui/Font.h
	ui/GameListMenu.cpp
	ui/GameListMenu.h
	ui/Gui.cpp
	ui/Gui.h
	ui/OverlayLayer.cpp
	ui/OverlayLayer.h
	ui/SettingsMenu.cpp
	ui/SettingsMenu.h
	ui/VrMenu.cpp
	ui/VrMenu.h
	web/WebServer.cpp
	web/WebServer.h
)

# The Wi-Fi transfer page, embedded in the binary.
set(PS2V_WEB_PAGE "${CMAKE_CURRENT_BINARY_DIR}/web/web_page.h")
add_custom_command(OUTPUT "${PS2V_WEB_PAGE}"
	COMMAND ${CMAKE_COMMAND} "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "-DOUTPUT=${PS2V_WEB_PAGE}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/web/embed.cmake"
	DEPENDS web/index.html web/embed.cmake
	COMMENT "Embedding web/index.html")
target_sources(ps2v PRIVATE "${PS2V_WEB_PAGE}")

# Shaders become SPIR-V arrays in generated headers: shaders/name.stage -> name.stage.h, array name_stage.
set(PS2V_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(PS2V_SHADERS
	depth_stats.comp depth_hist.comp depth_range.comp depth_normalize.comp depth_hud.comp depth_background.comp depth_view.comp
	window.vert window.frag)
file(GLOB PS2V_SHADER_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl")
foreach(shader ${PS2V_SHADERS})
	string(REPLACE "." "_" array_name "${shader}")
	set(output "${PS2V_SHADER_DIR}/${shader}.h")
	add_custom_command(OUTPUT "${output}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${PS2V_SHADER_DIR}"
		COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn ${array_name}
			-I${CMAKE_CURRENT_SOURCE_DIR}/shaders -o "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
		DEPENDS "shaders/${shader}" ${PS2V_SHADER_INCLUDES}
		COMMENT "Compiling ${shader}")
	target_sources(ps2v PRIVATE "${output}")
endforeach()

target_include_directories(ps2v PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${PS2V_SHADER_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}/web"
)

target_link_libraries(ps2v PRIVATE
	PCSX2_FLAGS
	PCSX2
	libretro-headers
	imgui # for its stb_truetype, see ui/Font.cpp
	OpenXR::openxr_loader
)

# resources/ (GameDB, shaders, fonts) next to the binary, as for the other frontends, and the game
# profiles.
pcsx2_copy_runtime_resources(ps2v)
add_custom_command(TARGET ps2v POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/profiles" "$<TARGET_FILE_DIR:ps2v>/profiles")
