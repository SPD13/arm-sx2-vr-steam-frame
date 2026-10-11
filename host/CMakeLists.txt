# PS2 Volumetric VR host. Added by ARMSX2's top-level CMakeLists through PS2V_HOST_DIR, so
# CMAKE_SOURCE_DIR is the ARMSX2 tree and the core targets (PCSX2, PCSX2_FLAGS) are in scope.

find_package(OpenXR CONFIG REQUIRED)

add_executable(ps2v
	main.cpp
	Emu.cpp
	Emu.h
	ScreenLayer.cpp
	ScreenLayer.h
	XrApp.cpp
	XrApp.h
)

target_include_directories(ps2v PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(ps2v PRIVATE
	PCSX2_FLAGS
	PCSX2
	libretro-headers
	OpenXR::openxr_loader
)

# resources/ (GameDB, shaders, fonts) next to the binary, as for the other frontends.
pcsx2_copy_runtime_resources(ps2v)
