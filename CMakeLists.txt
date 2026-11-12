cmake_minimum_required(VERSION 4.0)

# MSVC runtime library flags are selected by an abstraction.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(stardesk CXX)

# ---- Where the dui source tree is -------------------------------------------------
# StarDesk is built against a dui source checkout: this project adds it as a
# subdirectory (library only) and links dui::app, exactly as an in-tree dui example
# does. Point DUI_SRC_ROOT_DIR at that checkout; without it the default keeps this
# repository next to dui in projects-main.
if(NOT DEFINED DUI_SRC_ROOT_DIR)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../cmake/dui_app.cmake")
        # Copied into <dui>/examples/stardesk and configured from the dui tree.
        set(_dui_default_src "${CMAKE_CURRENT_LIST_DIR}/../..")
    else()
        # Standalone checkout next to dui.
        set(_dui_default_src "${CMAKE_CURRENT_LIST_DIR}/../dui")
    endif()
    set(DUI_SRC_ROOT_DIR "${_dui_default_src}" CACHE PATH
        "Path to the dui source tree (the directory that contains cmake/dui_app.cmake)")
endif()
get_filename_component(DUI_SRC_ROOT_DIR "${DUI_SRC_ROOT_DIR}" ABSOLUTE)

if(NOT EXISTS "${DUI_SRC_ROOT_DIR}/cmake/dui_app.cmake")
    message(FATAL_ERROR
        "DUI_SRC_ROOT_DIR does not point at a dui source tree: '${DUI_SRC_ROOT_DIR}'\n"
        "Configure with -DDUI_SRC_ROOT_DIR=<path to dui>")
endif()

# Build the dui library as part of this project (library only: no examples, no tests,
# no CEF). A parent dui tree that already defines the target is used as-is.
if(NOT TARGET dui)
    set(DUI_BUILD_EXAMPLES OFF CACHE BOOL "Build the example programs" FORCE)
    set(DUI_BUILD_TESTS OFF CACHE BOOL "Build dui unit tests" FORCE)
    set(DUI_ENABLE_CEF OFF CACHE BOOL "CEF support" FORCE)
    set(DUI_BUILD_CEF_EXAMPLES OFF CACHE BOOL "Build the CEF examples" FORCE)
    add_subdirectory("${DUI_SRC_ROOT_DIR}" "${CMAKE_BINARY_DIR}/dui")
endif()

include("${DUI_SRC_ROOT_DIR}/cmake/dui_app.cmake")

# Sources are collected per directory (aux_source_directory); platform-specific
# implementations live in src/platform/<os> and are added to the build only on the
# matching OS (.mm files must not be compiled outside macOS).
set(STARDESK_SRC_DIRS src src/app src/net src/session src/codec src/platform src/filetx src/ui)
if(APPLE)
    list(APPEND STARDESK_SRC_DIRS src/platform/apple)
elseif(WIN32)
    list(APPEND STARDESK_SRC_DIRS src/platform/win)
elseif(DUI_ENABLE_WAYLAND)
    # Wayland: screen capture via the xdg-desktop-portal ScreenCast API
    list(APPEND STARDESK_SRC_DIRS src/platform/wayland)
else()
    list(APPEND STARDESK_SRC_DIRS src/platform/x11)
endif()

set(STARDESK_SOURCES)
foreach(_dir IN LISTS STARDESK_SRC_DIRS)
    aux_source_directory("${CMAKE_CURRENT_SOURCE_DIR}/${_dir}" _dir_sources)
    list(APPEND STARDESK_SOURCES ${_dir_sources})
endforeach()

add_executable(stardesk ${STARDESK_SOURCES})
target_link_libraries(stardesk PRIVATE dui::app)

# ---- StarDesk-specific additions ----

# libpng headers for screen-frame encoding/decoding (libdui-png is already linked
# by dui::app; only the headers need to be exposed).
target_include_directories(stardesk PRIVATE "${DUI_SRC_ROOT_DIR}/third_party/skia")
target_include_directories(stardesk PRIVATE "${DUI_SRC_ROOT_DIR}/third_party/libpng")
# stb_image_resize2.h for receive-thread frame pre-scaling (ClientSession.cpp)
target_include_directories(stardesk PRIVATE "${DUI_SRC_ROOT_DIR}/third_party/stb_image")

find_package(PkgConfig QUIET)

if(APPLE)
    # ScreenCaptureKit (macOS 14+) + CoreMedia (CMTime) for screen capture,
    # CoreVideo (CVPixelBuffer) + QuartzCore (CACurrentMediaTime) for the SCStream
    # frame delivery path, VideoToolbox for H.264 client decode, AudioToolbox for
    # audio playback.
    target_link_libraries(stardesk PRIVATE
        "-framework ScreenCaptureKit" "-framework CoreMedia"
        "-framework CoreVideo" "-framework QuartzCore"
        "-framework VideoToolbox" "-framework AudioToolbox")

    # libvpx (VP9) / libaom (AV1) software decode fallback for the client
    # (VideoToolbox lacks VP9/AV1 hardware decode on some Macs)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(VPX QUIET vpx)
        pkg_check_modules(AOM QUIET aom)
    endif()
    if(VPX_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_VPX=1)
        target_include_directories(stardesk PRIVATE ${VPX_INCLUDE_DIRS})
        target_link_directories(stardesk PRIVATE ${VPX_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${VPX_LIBRARIES})
        message(STATUS "stardesk: VP9 software decode enabled (libvpx ${VPX_VERSION})")
    else()
        message(WARNING "libvpx not found - VP9 client decode unavailable")
    endif()
    if(AOM_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_AOM=1)
        target_include_directories(stardesk PRIVATE ${AOM_INCLUDE_DIRS})
        target_link_directories(stardesk PRIVATE ${AOM_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${AOM_LIBRARIES})
        message(STATUS "stardesk: AV1 software decode enabled (libaom ${AOM_VERSION})")
    else()
        message(WARNING "libaom not found - AV1 software decode unavailable")
    endif()

    # Opus for client audio decode (homebrew installs opus.pc alongside vpx/aom;
    # AudioPlayback_apple.mm + OpusAudio.cpp)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(OPUS QUIET opus)
    endif()
    if(OPUS_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_OPUS=1)
        target_include_directories(stardesk PRIVATE ${OPUS_INCLUDE_DIRS} ${OPUS_INCLUDEDIR})
        target_link_directories(stardesk PRIVATE ${OPUS_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${OPUS_LIBRARIES})
        message(STATUS "stardesk: Opus audio enabled (libopus ${OPUS_VERSION})")
    else()
        message(WARNING "libopus not found - audio playback unavailable")
    endif()
elseif(WIN32)
    target_link_libraries(stardesk PRIVATE ws2_32)
    # UI strings contain CJK/UTF-8 literals (8 built-in languages)
    target_compile_options(stardesk PRIVATE /utf-8)
elseif(UNIX AND NOT APPLE AND DUI_ENABLE_WAYLAND)
    # Wayland: portal ScreenCast + PipeWire capture (Capturer_wayland.cpp);
    # the dui Wayland backend provides the window/input plumbing.
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(DBUS1 QUIET dbus-1)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
    endif()
    if(DBUS1_FOUND)
        target_include_directories(stardesk PRIVATE ${DBUS1_INCLUDE_DIRS})
        target_link_libraries(stardesk PRIVATE ${DBUS1_LIBRARIES})
        message(STATUS "stardesk: Wayland capture enabled (dbus-1 ${DBUS1_VERSION})")
    else()
        message(WARNING "dbus-1 not found - Wayland screen capture unavailable")
    endif()
    if(PIPEWIRE_FOUND)
        target_include_directories(stardesk PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(stardesk PRIVATE ${PIPEWIRE_LIBRARIES})
        message(STATUS "stardesk: PipeWire ScreenCast capture enabled (${PIPEWIRE_VERSION})")
    else()
        message(WARNING "libpipewire not found - Wayland screen capture unavailable")
    endif()
    # EGL/GLES for the DMA-BUF readback (the dui Wayland backend already links the
    # same libs; needed here explicitly for the capturer)
    target_link_libraries(stardesk PRIVATE EGL GLESv2)
    # libei (EIS) for the official Wayland input-injection path
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(EI QUIET libei-1.0)
        pkg_check_modules(OPUS QUIET opus)
    endif()
    if(EI_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_LIBEI=1)
        target_include_directories(stardesk PRIVATE ${EI_INCLUDE_DIRS})
        target_link_libraries(stardesk PRIVATE ${EI_LIBRARIES})
        message(STATUS "stardesk: libei EIS input injection enabled (${EI_VERSION})")
    else()
        message(WARNING "libei not found - Wayland input falls back to uinput")
    endif()
    if(OPUS_FOUND AND PIPEWIRE_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_OPUS=1)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_AUDIO=1)
        target_include_directories(stardesk PRIVATE ${OPUS_INCLUDE_DIRS} ${OPUS_INCLUDEDIR})
        target_link_libraries(stardesk PRIVATE ${OPUS_LIBRARIES})
        message(STATUS "stardesk: desktop audio capture enabled (libopus ${OPUS_VERSION})")
    else()
        message(WARNING "libopus not found - desktop audio capture unavailable")
    endif()
elseif(UNIX AND NOT APPLE)
    # X11 screen capture (XShm) + XTest input injection (X11 session only)
    find_package(X11 REQUIRED)
    find_library(XTST_LIB Xtst)
    find_library(XEXT_LIB Xext)
    if(X11_FOUND AND XTST_LIB)
        target_include_directories(stardesk PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(stardesk PRIVATE ${X11_LIBRARIES} ${XTST_LIB} ${XEXT_LIB})
    else()
        message(WARNING "X11/Xtst not found - screen capture and input injection will be unavailable on this system")
    endif()
endif()

# Software codecs (pure libraries - used by both the X11 and the Wayland host)
if(UNIX AND NOT APPLE)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(X264 QUIET x264)
        pkg_check_modules(VPX QUIET vpx)
        pkg_check_modules(AOM QUIET aom)
        pkg_check_modules(OPENH264 QUIET openh264)
    endif()
    if(X264_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_X264=1)
        target_include_directories(stardesk PRIVATE ${X264_INCLUDE_DIRS})
        target_link_directories(stardesk PRIVATE ${X264_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${X264_LIBRARIES})
        message(STATUS "stardesk: H.264 host encoding enabled (libx264 ${X264_VERSION})")
    else()
        message(WARNING "libx264 not found - host will use the tile/PNG stream only")
    endif()
    if(VPX_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_VPX=1)
        target_include_directories(stardesk PRIVATE ${VPX_INCLUDE_DIRS})
        target_link_directories(stardesk PRIVATE ${VPX_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${VPX_LIBRARIES})
        message(STATUS "stardesk: VP9 host encoding enabled (libvpx ${VPX_VERSION})")
    else()
        message(WARNING "libvpx not found - VP9 unavailable")
    endif()
    if(AOM_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_AOM=1)
        target_include_directories(stardesk PRIVATE ${AOM_INCLUDE_DIRS})
        target_link_directories(stardesk PRIVATE ${AOM_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${AOM_LIBRARIES})
        message(STATUS "stardesk: AV1 host encoding enabled (libaom ${AOM_VERSION})")
    else()
        message(WARNING "libaom not found - AV1 unavailable")
    endif()
    if(OPENH264_FOUND)
        target_compile_definitions(stardesk PRIVATE STARDESK_HAS_OPENH264=1)
        target_include_directories(stardesk PRIVATE ${OPENH264_INCLUDE_DIRS})
        target_link_directories(stardesk PRIVATE ${OPENH264_LIBRARY_DIRS})
        target_link_libraries(stardesk PRIVATE ${OPENH264_LIBRARIES})
        message(STATUS "stardesk: OpenH264 host encoding enabled (${OPENH264_VERSION})")
    else()
        message(WARNING "libopenh264 not found - OpenH264 codec unavailable")
    endif()
endif()

# Output directory, Windows entry point, the macOS .app bundle and its code
# signature, and build ordering for the Skia archive.
dui_finalize_app(stardesk)
