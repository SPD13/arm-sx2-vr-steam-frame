# ARMSX2 VR VR host. Added by ARMSX2's top-level CMakeLists through PS2V_HOST_DIR, so
# CMAKE_SOURCE_DIR is the ARMSX2 tree and the core targets (PCSX2, PCSX2_FLAGS) are in scope.

find_package(OpenXR CONFIG REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
# Zip, 7z and RAR archives sent over Wi-Fi: libarchive, static (tools/build-frame.sh builds it into
# the deps; the Steam Runtime has none), with the runtime's own zlib, liblzma, libbz2 and libzstd.
find_package(LibArchive 3.8 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(BZip2 REQUIRED)

add_executable(ps2v
	main.cpp
	ControllerModel.cpp
	ControllerModel.h
	Unarchive.cpp
	Unarchive.h
	Controls.cpp
	Controls.h
	Profile.cpp
	Profile.h
	Emu.cpp
	Emu.h
	ScreenLayer.cpp
	ScreenLayer.h
	Settings.cpp
	Settings.h
	TexturePacks.cpp
	TexturePacks.h
	XrApp.cpp
	XrApp.h
	XrMath.h
	gun/GunAim.cpp
	gun/GunAim.h
	gun/GunDepth.cpp
	gun/GunDepth.h
	gun/GunGames.cpp
	gun/GunGames.h
	gun/GunLight.cpp
	gun/GunLight.h
	gun/GunModel.cpp
	gun/GunModel.h
	gun/GunRenderer.cpp
	gun/GunRenderer.h
	gun/GunView.cpp
	gun/GunView.h
	third_party/cgltf.h
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
	ui/PerfWindow.cpp
	ui/PerfWindow.h
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
	gun_depth.comp gun_depth_layered.comp gun_light.comp gun.vert gun.frag gun_beam.vert gun_beam.frag controller.vert
	controller.frag)
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
	# ARMSX2's texture pack path rules (IOS/TexturePackPaths.h), shared with its iOS app.
	"${CMAKE_SOURCE_DIR}/platforms/ios/app/src/main/cpp"
)

target_link_libraries(ps2v PRIVATE
	PCSX2_FLAGS
	PCSX2
	libretro-headers
	imgui # for its stb_truetype, see ui/Font.cpp
	OpenXR::openxr_loader
	LibArchive::LibArchive
	LibLZMA::LibLZMA
	BZip2::BZip2
	Zstd::Zstd
	ZLIB::ZLIB
)

# resources/ (GameDB, shaders, fonts) next to the binary, as for the other frontends, the game
# profiles and the light gun's model.
pcsx2_copy_runtime_resources(ps2v)
add_custom_command(TARGET ps2v POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/profiles" "$<TARGET_FILE_DIR:ps2v>/profiles")
# The light gun's model (tools/lightgun builds it).
add_custom_command(TARGET ps2v POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ps2v>/assets"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../assets/lightgun.glb"
		"$<TARGET_FILE_DIR:ps2v>/assets/lightgun.glb")
