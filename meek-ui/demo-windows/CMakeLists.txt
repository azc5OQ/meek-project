cmake_minimum_required(VERSION 3.21)

# demo-windows/CMakeLists.txt
# Example Windows host application. Lives outside gui/ entirely so
# it's clear that nothing in this directory is part of the library.
# The sibling demo-android/ is its Android counterpart (same role,
# different platform; built by demo-android/build.ps1 / build.sh
# rather than CMake).
#
# Builds gui_demo.exe and links it against the gui shared library
# (added at the top level via add_subdirectory(gui) before this
# subdirectory is added). cmake_minimum_required above is required
# by CMake 4.x even though the parent declares one.

# The Windows entry-point stub (real `int main()` that forwards to the
# host's renamed `app_main`) lives in the gui library's source tree but
# is compiled INTO THE EXECUTABLE, not into gui.dll. Reason: `app_main`
# is defined in this exe's main.c; if the stub lived in the DLL, the
# DLL link would fail with "undefined symbol: app_main".
#
# Every Windows host adds this same one-liner stub to its own target
# the same way.
add_executable(gui_demo
    main.c
    ${CMAKE_SOURCE_DIR}/gui/src/platforms/windows/main_entry_win32.c
)

# Linking gui resolves GUI_API symbols via the import lib at link
# time; at runtime the loader picks up gui.dll from the same
# directory (both targets share CMAKE_RUNTIME_OUTPUT_DIRECTORY).
target_link_libraries(gui_demo PRIVATE gui)

# DEMO_SOURCE_DIR is baked into the binary so hot_reload watches the
# .ui file in this directory directly. Dev-only convention; release
# builds will eventually embed the file content instead.
target_compile_definitions(gui_demo PRIVATE
    "DEMO_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
)

if(MSVC)
    target_compile_options(gui_demo PRIVATE /W4 /wd4100 /wd4505)
    # Console subsystem so printf() from click handlers shows up while
    # the PoC is under development. Switch to /SUBSYSTEM:WINDOWS for
    # shipping.
    set_target_properties(gui_demo PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(gui_demo PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unused-function
    )
endif()
