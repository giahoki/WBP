# Bloodborne.exe: the launcher (GLFW window, Direct3D 11 + Direct2D) and the game preparation
# that runs before every launch (successor of the Python launcher and scripts). MSYS2 CLANG64.
cmake_minimum_required(VERSION 3.20)
project(wbp_launcher LANGUAGES C CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
pkg_check_modules(DEFLATE REQUIRED IMPORTED_TARGET libdeflate)
find_package(pugixml REQUIRED)
find_package(nlohmann_json REQUIRED)
# miniz (update archives): the submodule of the GPU library.
add_subdirectory(../gpu/third_party/miniz ${CMAKE_BINARY_DIR}/miniz EXCLUDE_FROM_ALL)

add_executable(Bloodborne WIN32
    src/main.cpp src/util.cpp src/i18n.cpp src/settings.cpp src/net.cpp src/runner.cpp src/actions.cpp
    src/prep/image.cpp src/prep/patches.cpp src/prep/mods.cpp src/prep/key_icons.cpp
    src/ui/gfx.cpp src/ui/ui.cpp src/ui/launcher.cpp
    res/Bloodborne.rc)
target_compile_definitions(Bloodborne PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(Bloodborne PRIVATE -municode -Wall -Wno-missing-braces -Wno-unused-function)
# Fully static (libc++, winpthread, GLFW, libdeflate, pugixml): the launcher needs no DLLs.
target_link_options(Bloodborne PRIVATE -municode -static)
target_link_libraries(Bloodborne PRIVATE nlohmann_json::nlohmann_json miniz
    d3d11 d2d1 dwrite dxgi d3dcompiler dwmapi windowscodecs winhttp ws2_32 bcrypt ole32 shell32 shlwapi uuid
    advapi32 user32 gdi32)
# Static GLFW and libdeflate: the launcher needs no DLLs of its own.
target_include_directories(Bloodborne PRIVATE ${GLFW_INCLUDE_DIRS} ${DEFLATE_INCLUDE_DIRS})
find_library(GLFW_STATIC NAMES libglfw3.a glfw3 REQUIRED)
find_library(DEFLATE_STATIC NAMES libdeflate.a REQUIRED)
find_library(PUGIXML_STATIC NAMES libpugixml.a REQUIRED)
target_include_directories(Bloodborne PRIVATE $<TARGET_PROPERTY:pugixml::pugixml,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(Bloodborne PRIVATE ${GLFW_STATIC} ${DEFLATE_STATIC} ${PUGIXML_STATIC})
