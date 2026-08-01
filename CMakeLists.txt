cmake_minimum_required(VERSION 3.24)
project(vcrop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# FFmpeg comes from the system (pkg-config). Everything else is vendored.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libswscale
  libswresample
  libavutil)

include(FetchContent)

# Vendored dependencies: pinned release tarballs, downloaded on first
# configure and built as part of the project. No system installs needed
# for SDL2 or Dear ImGui.
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.4.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(sdl2
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.32.10.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Single-header PNG decoder for the embedded window icon.
FetchContent_Declare(stb_image
  URL https://github.com/nothings/stb/archive/013ac3beddff3dbffafd5177e7972067cd2b5083.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# SDL2 builds itself via CMake; skip its tests to keep the build lean.
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sdl2)

# ImGui ships no build system; compile the core plus the SDL2/SDLRenderer2
# backends as a static library. Warnings are kept off this third-party target.
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
  target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_lib PUBLIC SDL2::SDL2)
endif()

FetchContent_GetProperties(stb_image)
if(NOT stb_image_POPULATED)
  FetchContent_Populate(stb_image)
endif()

# Embed the app icon (packaging/icons/vcrop.png) into the binary so the
# window icon works no matter where the binary lives. Swap that PNG to
# replace the icon; the header regenerates on reconfigure (CONFIGURE_DEPENDS).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/vcrop.png" _vcrop_icon_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _vcrop_icon_bytes "${_vcrop_icon_hex}")
file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/vcrop.png" _vcrop_icon_size)
set(VCROP_ICON_BYTES "${_vcrop_icon_bytes}")
set(VCROP_ICON_SIZE "${_vcrop_icon_size}")
configure_file(cmake/icon_data.h.in "${CMAKE_BINARY_DIR}/generated/icon_data.h"
  CONFIGURE_DEPENDS)

add_executable(vcrop
  src/main.cpp
  src/app.cpp
  src/video_decoder.cpp
  src/audio_decoder.cpp
  src/coords.cpp
  src/crop_command.cpp
  src/stb_image_impl.cpp)
target_compile_options(vcrop PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(vcrop PRIVATE
  "${CMAKE_BINARY_DIR}/generated"
  "${stb_image_SOURCE_DIR}")
target_link_libraries(vcrop PRIVATE
  imgui_lib
  SDL2::SDL2
  PkgConfig::FFMPEG)

install(TARGETS vcrop RUNTIME DESTINATION bin)
