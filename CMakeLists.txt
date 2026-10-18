cmake_minimum_required(VERSION 3.21)
project(vcrop VERSION 0.1.0 DESCRIPTION "Crop and trim videos from the file manager" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Multimedia)
find_package(PkgConfig REQUIRED)
# FFmpeg 7.1 or newer (avcodec_get_supported_config).
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec>=61.19
    libavfilter
    libavutil)

qt_add_executable(vcrop MACOSX_BUNDLE
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/VideoView.cpp
    src/VideoView.h
    src/TrimSlider.cpp
    src/TrimSlider.h
    src/Cuts.cpp
    src/Cuts.h
    src/Exporter.cpp
    src/Exporter.h
    src/TimeFormat.h)

target_link_libraries(vcrop PRIVATE Qt6::Widgets Qt6::Multimedia PkgConfig::FFMPEG)
target_compile_definitions(vcrop PRIVATE VCROP_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vcrop PRIVATE -Wall -Wextra)
endif()

if(APPLE)
    # Declares the video document types, which is what puts VCrop into
    # Finder's "Open With" context menu.
    set_target_properties(vcrop PROPERTIES
        OUTPUT_NAME "VCrop"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in")
    install(TARGETS vcrop BUNDLE DESTINATION .)
else()
    include(GNUInstallDirs)
    install(TARGETS vcrop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Absolute Exec= paths so Dolphin finds the binary even when the install
    # prefix (e.g. ~/.local) is not on the session's PATH.
    set(VCROP_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/vcrop")
    configure_file(data/vcrop.desktop.in vcrop.desktop @ONLY)
    configure_file(data/vcrop-servicemenu.desktop.in vcrop-servicemenu.desktop @ONLY)

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vcrop.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    # Dolphin only honours user-local service menus that are executable.
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vcrop-servicemenu.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus
        RENAME vcrop.desktop
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    install(FILES data/vcrop.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
