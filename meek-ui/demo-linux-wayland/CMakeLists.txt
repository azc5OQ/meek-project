cmake_minimum_required(VERSION 3.21)

# demo-linux-wayland/CMakeLists.txt
#
# Wayland-client host for the gui library. Third Linux demo
# alongside demo-linux-x11 (nested X11 client) and demo-linux-drm
# (DRM/KMS kiosk). This one runs as a regular Wayland client under
# whatever compositor is already up.
#
# SCANNER INTEGRATION:
#   xdg-shell is the standard Wayland extension for application
#   windows (titles, min/maximize, fullscreen). It's defined in
#   /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
#   and lowered to C headers + stubs by wayland-scanner. We run
#   scanner at configure time so the generated .h / .c files are
#   under ${CMAKE_CURRENT_BINARY_DIR}/protocols/ and the include
#   path exposes them to the platform backend.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT    REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL       REQUIRED wayland-egl)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(EGL               REQUIRED egl)
pkg_check_modules(GLES3             REQUIRED glesv2)

#
# wayland-scanner binary. Same discovery pattern as
# meek-compositor: prefer the pkg-config variable, fall back to
# PATH. Same fatal error on miss.
#
pkg_get_variable(WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER_BIN)
    find_program(WAYLAND_SCANNER_BIN NAMES wayland-scanner)
endif()
if(NOT WAYLAND_SCANNER_BIN)
    message(FATAL_ERROR
        "wayland-scanner not found. Install 'wayland-dev' (Alpine) "
        "or 'libwayland-dev' (Debian / Ubuntu) or 'wayland-devel' "
        "(Fedora) or 'wayland' (Arch).")
endif()

pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PROTOCOLS_DATADIR)
    message(FATAL_ERROR
        "could not resolve wayland-protocols pkgdatadir via pkg-config")
endif()

#
# Run wayland-scanner on xdg-shell.xml, emitting client-side
# bindings:
#   ${GEN_DIR}/xdg-shell-client-protocol.h  (client-header)
#   ${GEN_DIR}/xdg-shell-protocol.c         (private-code)
# NOTE: client-header produces `*-client-protocol.h` (with the
# "client" infix); private-code produces plain `*-protocol.c`.
#
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

set(_GUI_SRC "${CMAKE_SOURCE_DIR}/gui/src")

set(SOURCES
    ${_GUI_SRC}/scene.c
    ${_GUI_SRC}/scene_style.c
    ${_GUI_SRC}/scene_layout.c
    ${_GUI_SRC}/scene_render.c
    ${_GUI_SRC}/scene_input.c
    ${_GUI_SRC}/platforms/linux/fs_linux.c
    ${_GUI_SRC}/font.c
    ${_GUI_SRC}/scroll.c
    ${_GUI_SRC}/animator.c
    ${_GUI_SRC}/parser_xml.c
    ${_GUI_SRC}/parser_style.c
    ${_GUI_SRC}/widget_registry.c
    ${_GUI_SRC}/widgets/widget_window.c
    ${_GUI_SRC}/widgets/widget_column.c
    ${_GUI_SRC}/widgets/widget_row.c
    ${_GUI_SRC}/widgets/widget_button.c
    ${_GUI_SRC}/widgets/widget_slider.c
    ${_GUI_SRC}/widgets/widget_div.c
    ${_GUI_SRC}/widgets/widget_text.c
    ${_GUI_SRC}/widgets/widget_input.c
    ${_GUI_SRC}/widgets/widget_checkbox.c
    ${_GUI_SRC}/widgets/widget_radio.c
    ${_GUI_SRC}/widgets/widget_select.c
    ${_GUI_SRC}/widgets/widget_option.c
    ${_GUI_SRC}/widgets/widget_image.c
    ${_GUI_SRC}/widgets/widget_collection.c
    ${_GUI_SRC}/widgets/widget_colorpicker.c
    ${_GUI_SRC}/widgets/widget_popup.c
    ${_GUI_SRC}/widgets/widget_textarea.c
    ${_GUI_SRC}/widgets/widget_canvas.c
    ${_GUI_SRC}/widgets/widget_keyboard.c
    ${_GUI_SRC}/third_party/log.c
    ${_GUI_SRC}/renderers/gles3_renderer.c
    ${_GUI_SRC}/platforms/linux/platform_linux_wayland_client.c
    ${_GUI_SRC}/clib/memory_manager.c
    ${_GUI_SRC}/clib/stdlib.c
    ${_GUI_SRC}/hot_reload.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${_XDG_HEADER}
    ${_XDG_SRC}
)

set(_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(MAKE_DIRECTORY "${_ASSET_DIR}/fonts")
file(MAKE_DIRECTORY "${_ASSET_DIR}/wallpapers")

file(GLOB _SHARED_UI
    "${CMAKE_SOURCE_DIR}/demo-windows/*.ui"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.style"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.png"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.jpg"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.jpeg"
)
foreach(_f IN LISTS _SHARED_UI)
    get_filename_component(_bn "${_f}" NAME)
    string(REGEX MATCH "[^ -~]" _nonascii "${_bn}")
    if(NOT _nonascii)
        configure_file("${_f}" "${_ASSET_DIR}/${_bn}" COPYONLY)
    endif()
endforeach()

file(GLOB _WALLPAPERS
    "${CMAKE_SOURCE_DIR}/demo-windows/wallpapers/*.png"
    "${CMAKE_SOURCE_DIR}/demo-windows/wallpapers/*.jpg"
    "${CMAKE_SOURCE_DIR}/demo-windows/wallpapers/*.jpeg"
)
foreach(_f IN LISTS _WALLPAPERS)
    get_filename_component(_bn "${_f}" NAME)
    string(REGEX MATCH "[^ -~]" _nonascii "${_bn}")
    if(NOT _nonascii)
        configure_file("${_f}" "${_ASSET_DIR}/wallpapers/${_bn}" COPYONLY)
    endif()
endforeach()

file(GLOB _FONTS "${CMAKE_SOURCE_DIR}/gui/src/fonts/*.ttf")
foreach(_f IN LISTS _FONTS)
    get_filename_component(_bn "${_f}" NAME)
    configure_file("${_f}" "${_ASSET_DIR}/fonts/${_bn}" COPYONLY)
endforeach()

add_executable(guidemo_linux_wayland ${SOURCES})

target_include_directories(guidemo_linux_wayland PRIVATE
    ${_GUI_SRC}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES3_INCLUDE_DIRS}
    ${GEN_DIR}
)

target_compile_definitions(guidemo_linux_wayland PRIVATE
    GUI_TRACK_ALLOCATIONS
    APPNAME="guidemo_linux_wayland"
    "DEMO_SOURCE_DIR=\"${_ASSET_DIR}\""
    "GUI_FONTS_SOURCE_DIR=\"${_ASSET_DIR}/fonts\""
)

target_compile_options(guidemo_linux_wayland PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -fvisibility=hidden
    ${WAYLAND_CLIENT_CFLAGS_OTHER}
    ${WAYLAND_EGL_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES3_CFLAGS_OTHER}
)

target_link_libraries(guidemo_linux_wayland PRIVATE
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES3_LIBRARIES}
    m dl pthread
)
target_link_directories(guidemo_linux_wayland PRIVATE
    ${WAYLAND_CLIENT_LIBRARY_DIRS}
    ${WAYLAND_EGL_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GLES3_LIBRARY_DIRS}
)
