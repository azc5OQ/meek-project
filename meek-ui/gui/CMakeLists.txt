cmake_minimum_required(VERSION 3.21)

# gui/CMakeLists.txt
# Library only. The toolkit ships as a shared library (gui.dll on
# Windows). The host application lives in ../demo/ and links to
# this target via add_subdirectory + target_link_libraries.
#
# Compiler standard, output dir, and project-wide preprocessor defs
# come from the top-level CMakeLists.txt. The cmake_minimum_required
# above is required by CMake 4.x even though the parent declares one.

# -----------------------------------------------------------------
# Renderer backend selection
# -----------------------------------------------------------------
#
# Exactly one renderer .c is compiled into the library at a time.
# All backends implement the same renderer.h interface.
#
#   opengl3  - OpenGL 3.3 core.          Working default (Windows).
#   d3d11    - Direct3D 11.              Scaffold: solid + text + scissor; image stubs.
#   d3d9     - Direct3D 9.               Scaffold: solid + text + scissor; image stubs.
#   d3d12    - Direct3D 12.              Scaffold: solid + scissor; text/image/effects stubbed.
#   vulkan   - Vulkan 1.1.               Scaffold: solid + scissor; text/image/effects stubbed.
#                                        Cross-platform -- usable on Linux too.
#
# Override from the CMake command line or a preset:
#   cmake -DGUI_RENDERER=vulkan ...
#
# Vulkan additionally needs the Vulkan SDK installed; it links
# vulkan-1 + shaderc_shared (for runtime GLSL->SPIR-V). Set the
# VULKAN_SDK environment variable before running cmake so the
# library paths resolve.
set(GUI_RENDERER "opengl3" CACHE STRING "Renderer backend: opengl3, d3d11, d3d9, d3d12, vulkan")
set_property(CACHE GUI_RENDERER PROPERTY STRINGS opengl3 d3d11 d3d9 d3d12 vulkan)

if(GUI_RENDERER STREQUAL "opengl3")
    set(_GUI_RENDERER_SRC "src/renderers/opengl3_renderer.c")
    set(_GUI_RENDERER_LIBS opengl32)
elseif(GUI_RENDERER STREQUAL "d3d11")
    set(_GUI_RENDERER_SRC "src/renderers/d3d11_renderer.c")
    # d3d11.lib: the core API.
    # dxgi.lib:  swapchain creation (DXGI is the presentation layer d3d11 sits on top of).
    # d3dcompiler.lib: runtime HLSL compile (D3DCompile). for release builds we'd
    #   want offline-compiled shader blobs to avoid shipping the compiler DLL,
    #   but for the poc runtime compile is simpler.
    # dxguid.lib: provides IID_ID3D11Texture2D and friends (REFIID constants used
    #   in QueryInterface-style calls). d3d11's C headers declare these as
    #   extern constants; without dxguid the linker reports "undefined symbol:
    #   IID_ID3D11Texture2D".
    set(_GUI_RENDERER_LIBS d3d11 dxgi d3dcompiler dxguid)
elseif(GUI_RENDERER STREQUAL "d3d9")
    set(_GUI_RENDERER_SRC "src/renderers/d3d9_renderer.c")
    # d3d9.lib: the core api.
    # d3dcompiler.lib: runtime HLSL compile (same as d3d11).
    set(_GUI_RENDERER_LIBS d3d9 d3dcompiler)
elseif(GUI_RENDERER STREQUAL "d3d12")
    set(_GUI_RENDERER_SRC "src/renderers/d3d12_renderer.c")
    # d3d12.lib: the core API.
    # dxgi.lib + dxguid.lib + d3dcompiler.lib: same rationale as d3d11.
    #   IDXGISwapChain3 + IID_ID3D12xxx constants + D3DCompile for HLSL.
    set(_GUI_RENDERER_LIBS d3d12 dxgi d3dcompiler dxguid)
elseif(GUI_RENDERER STREQUAL "vulkan")
    set(_GUI_RENDERER_SRC "src/renderers/vulkan_renderer.c")
    # Vulkan SDK: headers + vulkan-1.lib for the loader + shaderc_shared.lib
    # for runtime GLSL -> SPIR-V. Both ship with the LunarG SDK; set
    # VULKAN_SDK env var so CMake finds them.
    if(NOT DEFINED ENV{VULKAN_SDK})
        message(FATAL_ERROR
            "GUI_RENDERER=vulkan requires the Vulkan SDK. "
            "Install from https://vulkan.lunarg.com and set VULKAN_SDK "
            "in your environment before running cmake.")
    endif()
    set(_VK_SDK "$ENV{VULKAN_SDK}")
    message(STATUS "gui: Vulkan SDK at ${_VK_SDK}")
    if(WIN32)
        set(_GUI_RENDERER_LIBS
            "${_VK_SDK}/Lib/vulkan-1.lib"
            "${_VK_SDK}/Lib/shaderc_shared.lib")
        set(_GUI_RENDERER_INCS "${_VK_SDK}/Include")
    else()
        # Linux: SDK layout has lib/ instead of Lib/, and shaderc is
        # usually available as libshaderc_shared. Mesa's vulkan-1
        # is installed as libvulkan.so.
        set(_GUI_RENDERER_LIBS vulkan shaderc_shared)
        set(_GUI_RENDERER_INCS "${_VK_SDK}/include")
    endif()
else()
    message(FATAL_ERROR "GUI_RENDERER must be one of: opengl3, d3d11, d3d9, d3d12, vulkan (got '${GUI_RENDERER}')")
endif()

message(STATUS "gui: renderer backend = ${GUI_RENDERER}  (${_GUI_RENDERER_SRC})")

# -----------------------------------------------------------------
# Allocation tracking
# -----------------------------------------------------------------
#
# When ON, every malloc/calloc/realloc/free in the library goes
# through clib/memory_manager.c, which tags each block with an
# MM_TYPE_* category and threads it into a live-list for leak
# reporting. When OFF, the MEM_* macros in clib/memory_manager.h
# expand to plain stdlib calls with zero overhead -- the tracker
# .c is still compiled in (it's used directly at a few sites) but
# the wrapper indirection is gone.
#
# Default ON for dev builds; flip off for shipping builds where
# every nanosecond matters.
option(GUI_TRACK_ALLOCATIONS "Track all heap allocations (leak + type reporting)" ON)

# -----------------------------------------------------------------
# Library target
# -----------------------------------------------------------------

add_library(gui SHARED
    src/scene.c
    src/scene_style.c
    src/scene_layout.c
    src/scene_render.c
    src/scene_input.c
    src/platforms/windows/platform_win32.c
    src/platforms/windows/fs_win32.c
    src/parser_xml.c
    src/parser_style.c
    src/hot_reload.c
    src/font.c
    src/scroll.c
    src/animator.c
    src/widget_registry.c
    src/clib/memory_manager.c
    src/clib/stdlib.c
    src/widgets/widget_window.c
    src/widgets/widget_column.c
    src/widgets/widget_row.c
    src/widgets/widget_button.c
    src/widgets/widget_slider.c
    src/widgets/widget_div.c
    src/widgets/widget_text.c
    src/widgets/widget_input.c
    src/widgets/widget_checkbox.c
    src/widgets/widget_radio.c
    src/widgets/widget_select.c
    src/widgets/widget_option.c
    src/widgets/widget_image.c
    src/widgets/widget_collection.c
    src/widgets/widget_colorpicker.c
    src/widgets/widget_popup.c
    src/widgets/widget_textarea.c
    src/widgets/widget_canvas.c
    src/widgets/widget_keyboard.c
    src/widgets/widget_process_window.c
    src/third_party/log.c
    ${_GUI_RENDERER_SRC}
)

# src/ is on the include path PUBLIC so that any consumer linking
# this target (gui_demo, plus any future host app) can directly
# #include "scene.h" etc.
target_include_directories(gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GUI_BUILDING_DLL flips the GUI_API macro to dllexport while
# compiling the library. Consumers don't define it, so when their
# translation units include the same headers, GUI_API resolves to
# dllimport.
target_compile_definitions(gui PRIVATE GUI_BUILDING_DLL)

if(GUI_TRACK_ALLOCATIONS)
    target_compile_definitions(gui PUBLIC GUI_TRACK_ALLOCATIONS)
    message(STATUS "gui: allocation tracking ENABLED (GUI_TRACK_ALLOCATIONS)")
else()
    message(STATUS "gui: allocation tracking disabled")
endif()

# GUI_FONTS_SOURCE_DIR points at the directory holding the .ttf files
# shipped with the library (gui/src/fonts/). PRIVATE because it's a
# library-internal path -- font__init() scans this directory at startup
# and auto-registers every .ttf found there, so host apps just reference
# families by name in their .style files without knowing file paths.
# This is the on-disk mode; the alternative embed-in-DLL mode uses
# byte arrays generated at build time (deferred -- see STATUS.md).
#
# Quoting mirrors the proven DEMO_SOURCE_DIR pattern in demo-windows/CMakeLists.txt:
# outer CMake quotes group the argument, escaped inner quotes end up in
# the preprocessor define so font.c sees a proper string literal.
target_compile_definitions(gui PRIVATE
    "GUI_FONTS_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/src/fonts\""
)

# Compile flags. MSVC variable is TRUE for both cl.exe and clang-cl,
# which is the right discriminator for flag syntax. Pure Clang and
# GCC use the Unix-style driver flags.
if(MSVC)
    target_compile_options(gui PRIVATE
        /W4
        /wd4100  # unreferenced formal parameter — WndProc and handler sigs are fixed
        /wd4505  # unreferenced local function
    )
else()
    target_compile_options(gui PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unused-function
    )
endif()

# Always-linked Win32 system libs (window + GDI, independent of graphics API).
# Renderer-specific graphics libs come from _GUI_RENDERER_LIBS above.
target_link_libraries(gui PRIVATE user32 gdi32 ${_GUI_RENDERER_LIBS})

# Renderer-specific include path (currently only Vulkan needs one --
# the Vulkan SDK headers don't sit on the default include path the
# way opengl32.lib / d3d11.h do).
if(DEFINED _GUI_RENDERER_INCS)
    target_include_directories(gui PRIVATE ${_GUI_RENDERER_INCS})
endif()
