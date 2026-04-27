cmake_minimum_required(VERSION 3.21)

# demo-linux-x11/CMakeLists.txt
#
# Linux X11 desktop host. CMake mirror of build.sh. Single-binary
# build that lists the gui/ sources directly (same shape as the
# DRM demo next door).
#
# Runs on any X session, and on WSL2 (Windows 11) via WSLg -- the
# Microsoft-shipped XWayland layer routes the app to a Windows 11
# window automatically. You do NOT need to start gnome / xfce4 /
# anything else under WSL2; WSLg runs its own tiny compositor.

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11_DEPS REQUIRED x11 egl glesv2)

set(_GUI_SRC "${CMAKE_SOURCE_DIR}/gui/src")

set(SOURCES
    ${_GUI_SRC}/scene.c
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
    ${_GUI_SRC}/platforms/linux/platform_linux_x11.c
    ${_GUI_SRC}/clib/memory_manager.c
    ${_GUI_SRC}/clib/stdlib.c
    ${_GUI_SRC}/hot_reload.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
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

add_executable(guidemo_linux_x11 ${SOURCES})

target_include_directories(guidemo_linux_x11 PRIVATE
    ${_GUI_SRC}
    ${X11_DEPS_INCLUDE_DIRS}
)

target_compile_definitions(guidemo_linux_x11 PRIVATE
    GUI_TRACK_ALLOCATIONS
    APPNAME="guidemo_linux_x11"
    "DEMO_SOURCE_DIR=\"${_ASSET_DIR}\""
    "GUI_FONTS_SOURCE_DIR=\"${_ASSET_DIR}/fonts\""
)

target_compile_options(guidemo_linux_x11 PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -fvisibility=hidden
    ${X11_DEPS_CFLAGS_OTHER}
)

target_link_libraries(guidemo_linux_x11 PRIVATE
    ${X11_DEPS_LIBRARIES}
    m dl pthread
)
target_link_directories(guidemo_linux_x11 PRIVATE ${X11_DEPS_LIBRARY_DIRS})
