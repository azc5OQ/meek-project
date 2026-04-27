cmake_minimum_required(VERSION 3.21)

# meek-shell/CMakeLists.txt
#
# The privileged Wayland CLIENT that will eventually be the phone
# shell. D2 pass: pulls meek-ui sources into the binary (same
# explicit-list pattern as meek-ui's demo-linux-wayland) so
# meek-shell can render through meek-ui's Wayland-client platform
# backend. meek_shell_v1 extension binding lives in D3 (later
# pass, needs meek-compositor C5 to exist first).
#
# Three-project tree:
#   meek-ui/          -- pure UI library, no Wayland knowledge
#   meek-compositor/  -- pure Wayland server, no UI library
#   meek-shell/       -- THIS; middle tier. Depends on both at
#                        runtime; depends on meek-ui at build time.
#
# Builds ONLY on Linux. libwayland-client has no Windows/macOS port.
# Dev host is Windows; deploy/build happens on a Linux SSH target
# via deploy.bat + deploy.sh -> build.sh.
#
# Architecture on record: see session/DESIGN.md.

project(meek_shell C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "meek-shell is Linux-only. Current CMAKE_SYSTEM_NAME is "
        "'${CMAKE_SYSTEM_NAME}'. Build on the SSH target (see deploy.bat).")
endif()

# --------------------------------------------------------------
# Dependencies. Same surface as meek-ui/demo-linux-wayland:
#   * libwayland-client + libwayland-egl  (protocol + EGL bridge)
#   * egl + glesv2                        (rendering)
#   * wayland-protocols                   (xdg-shell.xml lives here)
#   * wayland-scanner (+ pkg-config var to find it)
# --------------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT    REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL       REQUIRED wayland-egl)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(EGL               REQUIRED egl)
pkg_check_modules(GLES3             REQUIRED glesv2)

pkg_get_variable(WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER_BIN)
    find_program(WAYLAND_SCANNER_BIN NAMES wayland-scanner)
endif()
if(NOT WAYLAND_SCANNER_BIN)
    message(FATAL_ERROR
        "wayland-scanner not found. Install wayland-dev / libwayland-dev.")
endif()

pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PROTOCOLS_DATADIR)
    message(FATAL_ERROR
        "could not resolve wayland-protocols pkgdatadir via pkg-config")
endif()

# --------------------------------------------------------------
# Scanner: generate client-side bindings for xdg-shell into
# ${GEN_DIR}. Plus meek_shell_v1 later (D3) once the extension
# XML is designed.
# --------------------------------------------------------------

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${GEN_DIR}")

set(_XDG_XML    "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml")
set(_XDG_HEADER "${GEN_DIR}/xdg-shell-client-protocol.h")
set(_XDG_SRC    "${GEN_DIR}/xdg-shell-protocol.c")

add_custom_command(
    OUTPUT "${_XDG_HEADER}"
    COMMAND "${WAYLAND_SCANNER_BIN}" client-header "${_XDG_XML}" "${_XDG_HEADER}"
    DEPENDS "${_XDG_XML}"
    COMMENT "wayland-scanner client-header xdg-shell"
    VERBATIM)
add_custom_command(
    OUTPUT "${_XDG_SRC}"
    COMMAND "${WAYLAND_SCANNER_BIN}" private-code "${_XDG_XML}" "${_XDG_SRC}"
    DEPENDS "${_XDG_XML}" "${_XDG_HEADER}"
    COMMENT "wayland-scanner private-code xdg-shell"
    VERBATIM)

#
# meek_shell_v1 bindings (client side). XML lives in the sibling
# meek-compositor project; both sides of the extension read the
# same file so scanner output stays in lockstep.
#
set(_MSV1_XML
    "${CMAKE_CURRENT_SOURCE_DIR}/../meek-compositor/protocols/meek-shell-v1.xml")
set(_MSV1_HEADER "${GEN_DIR}/meek-shell-v1-client-protocol.h")
set(_MSV1_SRC    "${GEN_DIR}/meek-shell-v1-protocol.c")

if(NOT EXISTS "${_MSV1_XML}")
    message(FATAL_ERROR
        "meek-shell-v1.xml not found at ${_MSV1_XML}. "
        "Expected layout:\n"
        "  <parent>/meek-compositor/protocols/meek-shell-v1.xml\n"
        "  <parent>/meek-shell/CMakeLists.txt")
endif()

add_custom_command(
    OUTPUT "${_MSV1_HEADER}"
    COMMAND "${WAYLAND_SCANNER_BIN}" client-header "${_MSV1_XML}" "${_MSV1_HEADER}"
    DEPENDS "${_MSV1_XML}"
    COMMENT "wayland-scanner client-header meek-shell-v1"
    VERBATIM)
add_custom_command(
    OUTPUT "${_MSV1_SRC}"
    COMMAND "${WAYLAND_SCANNER_BIN}" private-code "${_MSV1_XML}" "${_MSV1_SRC}"
    DEPENDS "${_MSV1_XML}" "${_MSV1_HEADER}"
    COMMENT "wayland-scanner private-code meek-shell-v1"
    VERBATIM)

# --------------------------------------------------------------
# meek-ui sources. Same explicit list as meek-ui/demo-linux-wayland/
# CMakeLists.txt; keep them in sync when new widgets or subsystems
# are added on the meek-ui side.
# --------------------------------------------------------------

set(_MEEK_UI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../meek-ui/gui/src")

set(_MEEK_UI_SOURCES
    ${_MEEK_UI_SRC}/scene.c
    ${_MEEK_UI_SRC}/scene_style.c
    ${_MEEK_UI_SRC}/scene_layout.c
    ${_MEEK_UI_SRC}/scene_render.c
    ${_MEEK_UI_SRC}/scene_input.c
    ${_MEEK_UI_SRC}/platforms/linux/fs_linux.c
    ${_MEEK_UI_SRC}/font.c
    ${_MEEK_UI_SRC}/scroll.c
    ${_MEEK_UI_SRC}/animator.c
    ${_MEEK_UI_SRC}/parser_xml.c
    ${_MEEK_UI_SRC}/parser_style.c
    ${_MEEK_UI_SRC}/widget_registry.c
    ${_MEEK_UI_SRC}/widgets/widget_window.c
    ${_MEEK_UI_SRC}/widgets/widget_column.c
    ${_MEEK_UI_SRC}/widgets/widget_row.c
    ${_MEEK_UI_SRC}/widgets/widget_button.c
    ${_MEEK_UI_SRC}/widgets/widget_slider.c
    ${_MEEK_UI_SRC}/widgets/widget_div.c
    ${_MEEK_UI_SRC}/widgets/widget_text.c
    ${_MEEK_UI_SRC}/widgets/widget_input.c
    ${_MEEK_UI_SRC}/widgets/widget_checkbox.c
    ${_MEEK_UI_SRC}/widgets/widget_radio.c
    ${_MEEK_UI_SRC}/widgets/widget_select.c
    ${_MEEK_UI_SRC}/widgets/widget_option.c
    ${_MEEK_UI_SRC}/widgets/widget_image.c
    ${_MEEK_UI_SRC}/widgets/widget_collection.c
    ${_MEEK_UI_SRC}/widgets/widget_colorpicker.c
    ${_MEEK_UI_SRC}/widgets/widget_popup.c
    ${_MEEK_UI_SRC}/widgets/widget_textarea.c
    ${_MEEK_UI_SRC}/widgets/widget_canvas.c
    ${_MEEK_UI_SRC}/widgets/widget_keyboard.c
    ${_MEEK_UI_SRC}/third_party/log.c
    ${_MEEK_UI_SRC}/renderers/gles3_renderer.c
    ${_MEEK_UI_SRC}/platforms/linux/platform_linux_wayland_client.c
    ${_MEEK_UI_SRC}/clib/memory_manager.c
    ${_MEEK_UI_SRC}/clib/stdlib.c
    ${_MEEK_UI_SRC}/hot_reload.c
)

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/meek_shell_v1_client.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toplevel_registry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gesture_recognizer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app_registry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/icon_resolver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/icon_color_sampler.c
    ${_MEEK_UI_SOURCES}
    ${_XDG_HEADER}
    ${_XDG_SRC}
    ${_MSV1_HEADER}
    ${_MSV1_SRC}
)

# --------------------------------------------------------------
# Asset staging. meek-shell has its own views/ directory; we copy
# .ui + .style + fonts into assets/ next to the binary so main.c's
# hot_reload__watch_* paths resolve.
# --------------------------------------------------------------

set(_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(MAKE_DIRECTORY "${_ASSET_DIR}/fonts")

file(GLOB _SHELL_VIEWS
    "${CMAKE_CURRENT_SOURCE_DIR}/views/*.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/views/*.style"
)
foreach(_f IN LISTS _SHELL_VIEWS)
    get_filename_component(_bn "${_f}" NAME)
    configure_file("${_f}" "${_ASSET_DIR}/${_bn}" COPYONLY)
endforeach()

file(GLOB _FONTS "${_MEEK_UI_SRC}/fonts/*.ttf")
foreach(_f IN LISTS _FONTS)
    get_filename_component(_bn "${_f}" NAME)
    configure_file("${_f}" "${_ASSET_DIR}/fonts/${_bn}" COPYONLY)
endforeach()

# --------------------------------------------------------------
# Executable
# --------------------------------------------------------------

add_executable(meek_shell ${SOURCES})

target_include_directories(meek_shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${_MEEK_UI_SRC}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES3_INCLUDE_DIRS}
    ${GEN_DIR}
)

target_compile_definitions(meek_shell PRIVATE
    GUI_TRACK_ALLOCATIONS
    APPNAME="meek_shell"
    "SHELL_ASSET_DIR=\"${_ASSET_DIR}\""
    "GUI_FONTS_SOURCE_DIR=\"${_ASSET_DIR}/fonts\""
)

target_compile_options(meek_shell PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -fvisibility=hidden
    ${WAYLAND_CLIENT_CFLAGS_OTHER}
    ${WAYLAND_EGL_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES3_CFLAGS_OTHER}
)

target_link_libraries(meek_shell PRIVATE
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES3_LIBRARIES}
    m dl pthread
)

target_link_directories(meek_shell PRIVATE
    ${WAYLAND_CLIENT_LIBRARY_DIRS}
    ${WAYLAND_EGL_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GLES3_LIBRARY_DIRS}
)
