cmake_minimum_required(VERSION 3.21)

# demo-windows-code/CMakeLists.txt
# Second Windows host that builds its scene entirely in C -- no .ui, no
# .style, no hot reload. Sibling of demo-windows/. Same library target
# (gui.dll), same platform layer, same handler auto-discovery via
# UI_HANDLER + GetProcAddress. Only difference is how the scene is
# constructed: scene__node_new + direct writes to node->style[...].
#
# Proves the parser is optional. Useful as a template for apps that
# ship pre-baked UI without the runtime parsing step.

add_executable(gui_demo_code
    main.c
    ${CMAKE_SOURCE_DIR}/gui/src/platforms/windows/main_entry_win32.c
)

target_link_libraries(gui_demo_code PRIVATE gui)

if(MSVC)
    target_compile_options(gui_demo_code PRIVATE /W4 /wd4100 /wd4505)
    set_target_properties(gui_demo_code PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(gui_demo_code PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unused-function
    )
endif()
