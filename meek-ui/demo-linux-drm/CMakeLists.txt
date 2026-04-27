cmake_minimum_required(VERSION 3.21)

# demo-linux-drm/CMakeLists.txt
#
# Linux DRM/KMS kiosk host. CMake mirror of build.sh -- picks up
# the same source list, same pkg-config deps, same baked-in
# DEMO_SOURCE_DIR / GUI_FONTS_SOURCE_DIR.
#
# Single-binary build. Unlike the Windows build we don't wrap the
# gui/ sources in a shared library; on Linux the demos compile
# everything into one ELF (matching how build.sh / the Android
# NDK scripts already work). Keeps deployment simple (one file +
# assets directory), and there's no second consumer to share a
# .so with right now.
#
# WSL2 NOTE: DRM does NOT work on WSL2. WSL2 has /dev/dxg
# (DirectX translation) but not /dev/dri/card*, so this backend
# cannot take DRM master. If you're on WSL2, use demo-linux-x11
# instead (WSLg surfaces it through XWayland automatically).

# --------------------------------------------------------------
# Dependencies: libdrm + gbm + EGL + GLESv2 via pkg-config.
# --------------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM_DEPS REQUIRED libdrm gbm egl glesv2)

# --------------------------------------------------------------
# Source list -- mirrors demo-linux-drm/build.sh's SOURCES array.
# Keep the two in sync when files are added / removed from gui/.
# --------------------------------------------------------------

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
    ${_GUI_SRC}/platforms/linux/platform_linux_drm.c
    ${_GUI_SRC}/clib/memory_manager.c
    ${_GUI_SRC}/clib/stdlib.c
    ${_GUI_SRC}/hot_reload.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)

# --------------------------------------------------------------
# Assets: stage the same files the shell script does, to
# ${CMAKE_CURRENT_SOURCE_DIR}/assets. Bake the absolute path
# into DEMO_SOURCE_DIR + GUI_FONTS_SOURCE_DIR so the binary
# loads them regardless of working directory.
# --------------------------------------------------------------

set(_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(MAKE_DIRECTORY "${_ASSET_DIR}/fonts")
file(MAKE_DIRECTORY "${_ASSET_DIR}/wallpapers")

# .ui / .style / loose images
file(GLOB _SHARED_UI
    "${CMAKE_SOURCE_DIR}/demo-windows/*.ui"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.style"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.png"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.jpg"
    "${CMAKE_SOURCE_DIR}/demo-windows/*.jpeg"
)
foreach(_f IN LISTS _SHARED_UI)
    get_filename_component(_bn "${_f}" NAME)
    # skip non-ASCII filenames (aapt-style guard, matches build.sh)
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

# --------------------------------------------------------------
# Executable
# --------------------------------------------------------------

add_executable(guidemo_linux_drm ${SOURCES})

target_include_directories(guidemo_linux_drm PRIVATE
    ${_GUI_SRC}
    ${DRM_DEPS_INCLUDE_DIRS}
)

target_compile_definitions(guidemo_linux_drm PRIVATE
    GUI_TRACK_ALLOCATIONS
    APPNAME="guidemo_linux_drm"
    "DEMO_SOURCE_DIR=\"${_ASSET_DIR}\""
    "GUI_FONTS_SOURCE_DIR=\"${_ASSET_DIR}/fonts\""
)

target_compile_options(guidemo_linux_drm PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -fvisibility=hidden
    ${DRM_DEPS_CFLAGS_OTHER}
)

target_link_libraries(guidemo_linux_drm PRIVATE
    ${DRM_DEPS_LIBRARIES}
    m dl pthread
)
target_link_directories(guidemo_linux_drm PRIVATE ${DRM_DEPS_LIBRARY_DIRS})
