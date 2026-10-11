# PS2 Volumetric VR host. Added by ARMSX2's top-level CMakeLists through PS2V_HOST_DIR, so
# CMAKE_SOURCE_DIR is the ARMSX2 tree and the core targets (PCSX2, PCSX2_FLAGS) are in scope.

find_package(OpenXR CONFIG REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

add_executable(ps2v
	main.cpp
	DepthView.cpp
	DepthView.h
	Emu.cpp
	Emu.h
	ScreenLayer.cpp
	ScreenLayer.h
	XrApp.cpp
	XrApp.h
)

# Compute shaders become SPIR-V arrays (<name>_comp) in generated headers.
set(PS2V_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader depth_stats depth_view)
	set(output "${PS2V_SHADER_DIR}/${shader}.comp.h")
	add_custom_command(OUTPUT "${output}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${PS2V_SHADER_DIR}"
		COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn ${shader}_comp
			-I${CMAKE_CURRENT_SOURCE_DIR}/shaders -o "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.comp"
		DEPENDS "shaders/${shader}.comp" "shaders/depth_common.glsl"
		COMMENT "Compiling ${shader}.comp")
	target_sources(ps2v PRIVATE "${output}")
endforeach()

target_include_directories(ps2v PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${PS2V_SHADER_DIR}"
)

target_link_libraries(ps2v PRIVATE
	PCSX2_FLAGS
	PCSX2
	libretro-headers
	OpenXR::openxr_loader
)

# resources/ (GameDB, shaders, fonts) next to the binary, as for the other frontends.
pcsx2_copy_runtime_resources(ps2v)
