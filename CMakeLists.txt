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

add_executable(vcrop
  src/main.cpp
  src/app.cpp
  src/video_decoder.cpp
  src/coords.cpp
  src/crop_command.cpp)
target_compile_options(vcrop PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(vcrop PRIVATE
  imgui_lib
  SDL2::SDL2
  PkgConfig::FFMPEG)

install(TARGETS vcrop RUNTIME DESTINATION bin)
