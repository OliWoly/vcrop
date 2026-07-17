cmake_minimum_required(VERSION 3.24)
project(vcrop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libswscale
  libavutil)

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.4
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# ImGui ships no build system; compile the core plus the SDL2/SDLRenderer2
# backends as a static library. Warnings are kept off this third-party target.
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
target_link_libraries(imgui_lib PUBLIC PkgConfig::SDL2)

add_executable(vcrop
  src/main.cpp
  src/app.cpp
  src/video_decoder.cpp
  src/coords.cpp
  src/crop_command.cpp)
target_compile_options(vcrop PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(vcrop PRIVATE
  imgui_lib
  PkgConfig::SDL2
  PkgConfig::FFMPEG)

install(TARGETS vcrop RUNTIME DESTINATION bin)
