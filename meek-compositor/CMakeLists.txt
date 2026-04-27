cmake_minimum_required(VERSION 3.21)

# meek-compositor/CMakeLists.txt
#
# Wayland server skeleton. Single-binary build, mirrors the shape of
# meek-ui/demo-linux-drm/CMakeLists.txt (same pkg-config pattern,
# same compile defs, same visibility flags) so the two stay cousin-
# compatible. No add_subdirectory(../meek-ui/gui) -- we only borrow
# meek-ui's types.h via an include path in this pass; no meek-ui .c
# files are compiled yet. That hookup lands in the next pass once we
# have something to render.
#
# Builds ONLY on Linux. libwayland-server has no Windows/macOS port.
# Dev host is Windows; deploy/build happens on a Linux SSH target
# via deploy.bat + deploy.sh -> build.sh.
#
# Decision: NOT using wlroots. See session/DESIGN.md.

project(meek_compositor C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "meek-compositor is Linux-only. Current CMAKE_SYSTEM_NAME is "
        "'${CMAKE_SYSTEM_NAME}'. Build on the SSH target (see deploy.bat).")
endif()

# --------------------------------------------------------------
# Dependencies: libwayland-server + wayland-scanner + wayland-protocols
# all via pkg-config / find_program.
# --------------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER    REQUIRED wayland-server)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)

#
# EGL + GLES3 + GBM -- needed starting at pass A3 for client-buffer
# import. EGL is "how we connect OpenGL-family APIs to a native
# display"; GBM (Generic Buffer Manager) is the Linux-DRM-side
# companion that lets a GPU buffer be shared between processes as
# an fd (that's what dmabuf is -- a fd referencing a GPU buffer).
#
pkg_check_modules(EGL      REQUIRED egl)
pkg_check_modules(GLES3    REQUIRED glesv2)
pkg_check_modules(GBM      REQUIRED gbm)
#
# libdrm gives us drm_fourcc.h (DRM_FORMAT_ARGB8888 etc. + the
# MOD_INVALID sentinel). Also the right header path for client
# buffer import.
#
pkg_check_modules(LIBDRM   REQUIRED libdrm)
#
# libX11 -- only used by the nested-X11 output backend
# (output_x11.c). The DRM/KMS output backend (output_drm.c) runs
# alongside it; selection is runtime (MEEK_COMPOSITOR_OUTPUT env),
# so both depend sets are linked in.
#
pkg_check_modules(X11      REQUIRED x11)

#
# wayland-scanner. Prefer the pkg-config variable (points at the
# exact scanner binary for the wayland-server version we're linking
# against); fall back to whatever wayland-scanner is on PATH.
#
pkg_get_variable(WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER_BIN)
    find_program(WAYLAND_SCANNER_BIN NAMES wayland-scanner)
endif()
if(NOT WAYLAND_SCANNER_BIN)
    message(FATAL_ERROR
        "wayland-scanner not found. Install 'wayland-dev' (Alpine), "
        "'libwayland-dev' (Debian/Ubuntu), 'wayland-devel' (Fedora), "
        "or 'wayland' (Arch).")
endif()

#
# wayland-protocols pkgdatadir gives us /usr/share/wayland-protocols
# or equivalent. xdg-shell.xml + linux-dmabuf are under there.
#
pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PROTOCOLS_DATADIR)
    message(FATAL_ERROR
        "could not resolve wayland-protocols pkgdatadir via pkg-config")
endif()

# --------------------------------------------------------------
# wl_scanner_generate(<xml path relative to pkgdatadir> <stem>)
#
# Emits custom commands to produce:
#   ${GEN_DIR}/<stem>-protocol.h   (server-header)
#   ${GEN_DIR}/<stem>-protocol.c   (private-code)
#
# Appends both to WL_PROTOCOL_SOURCES for inclusion in the build.
# Use `server-header` for server-side bindings; `private-code` is
# the modern wayland-scanner (>=1.15) name for what used to be
# `code`, producing a compilation unit with the wl_interface tables
# and marshaling glue.
# --------------------------------------------------------------

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${GEN_DIR}")

set(WL_PROTOCOL_SOURCES "")

function(wl_scanner_generate xml_rel stem)
    #
    # If xml_rel is an absolute path, use as-is (for our custom
    # protocols/meek-shell-v1.xml). Otherwise prepend the standard
    # wayland-protocols pkgdatadir (for xdg-shell, linux-dmabuf,
    # etc.).
    #
    if(IS_ABSOLUTE "${xml_rel}")
        set(xml "${xml_rel}")
    else()
        set(xml "${WAYLAND_PROTOCOLS_DATADIR}/${xml_rel}")
    endif()
    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR
            "wayland-protocols XML not found: ${xml}\n"
            "wayland-protocols install dir: ${WAYLAND_PROTOCOLS_DATADIR}")
    endif()

    set(hdr "${GEN_DIR}/${stem}-protocol.h")
    set(src "${GEN_DIR}/${stem}-protocol.c")

    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND "${WAYLAND_SCANNER_BIN}" server-header "${xml}" "${hdr}"
        DEPENDS "${xml}"
        COMMENT "wayland-scanner server-header ${stem}"
        VERBATIM)

    add_custom_command(
        OUTPUT "${src}"
        COMMAND "${WAYLAND_SCANNER_BIN}" private-code "${xml}" "${src}"
        DEPENDS "${xml}" "${hdr}"
        COMMENT "wayland-scanner private-code ${stem}"
        VERBATIM)

    set(WL_PROTOCOL_SOURCES ${WL_PROTOCOL_SOURCES} "${hdr}" "${src}" PARENT_SCOPE)
endfunction()

#
# Extension protocols we need. Core protocols (wl_compositor,
# wl_shm, wl_output, wl_seat) already ship with libwayland-server
# via <wayland-server-protocol.h>, no scanner run needed for them.
#
wl_scanner_generate("stable/xdg-shell/xdg-shell.xml" xdg-shell)
wl_scanner_generate("staging/linux-dmabuf/linux-dmabuf-v1.xml" linux-dmabuf-v1)
#
# Our private extension (C5 pass). Lives in the repo under
# protocols/meek-shell-v1.xml; scanner generates server-header +
# private-code into the same build/protocols/ dir as the standard
# extensions. Only meek-shell is allowed to bind this -- the
# gating logic lives in src/meek_shell_v1.c.
#
wl_scanner_generate(
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/meek-shell-v1.xml"
    meek-shell-v1)

# --------------------------------------------------------------
# Source list -- mirrors build.sh's SOURCES array. Keep the two
# in sync when files are added / removed.
# --------------------------------------------------------------

set(_MEEK_UI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../meek-ui/gui/src")

#
# Shared meek-ui utility sources pulled into meek-compositor's
# binary. Per project guideline, prefer these over raw libc:
#
#   * third_party/log.c       -> log_info / log_warn / log_error
#                                instead of fprintf.
#   * clib/memory_manager.c   -> GUI_MALLOC / GUI_FREE instead of
#                                malloc/free; tracked for leaks when
#                                GUI_TRACK_ALLOCATIONS is on.
#   * clib/stdlib.c           -> stdlib__* wrappers.
#
# The src/clib/ directory still exists but its contents (user pre-
# staged stdlib.c et al.) are superseded by pulling meek-ui's
# canonical versions from ../meek-ui/gui/src/clib/.
#
set(_MEEK_UI_SHARED_SOURCES
    ${_MEEK_UI_SRC}/third_party/log.c
    ${_MEEK_UI_SRC}/clib/memory_manager.c
    ${_MEEK_UI_SRC}/clib/stdlib.c
)

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/globals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xdg_shell.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/egl_ctx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/linux_dmabuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/output_x11.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/output_drm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/meek_shell_v1.c
    ${_MEEK_UI_SHARED_SOURCES}
    ${WL_PROTOCOL_SOURCES}
)

# --------------------------------------------------------------
# Executable
# --------------------------------------------------------------

add_executable(meek_compositor ${SOURCES})

target_include_directories(meek_compositor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${_MEEK_UI_SRC}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES3_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    #
    # Scanner-generated headers under build/protocols/ are #include'd
    # by globals.c (and future .c files) via "xdg-shell-protocol.h"
    # / "linux-dmabuf-v1-protocol.h". Exposing GEN_DIR lets those
    # resolve without path qualifiers.
    #
    ${GEN_DIR}
)

target_compile_definitions(meek_compositor PRIVATE
    APPNAME="meek_compositor"
    #
    # Turn on memory_manager's tracked-alloc path. Without this the
    # GUI_MALLOC / GUI_FREE macros degrade to raw malloc/free (zero
    # overhead) but also zero leak reporting.
    #
    GUI_TRACK_ALLOCATIONS
)

target_compile_options(meek_compositor PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -fvisibility=hidden
    ${WAYLAND_SERVER_CFLAGS_OTHER}
)

target_link_libraries(meek_compositor PRIVATE
    ${WAYLAND_SERVER_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES3_LIBRARIES}
    ${GBM_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${X11_LIBRARIES}
    #
    # memory_manager.c uses pthread_mutex_t on Linux.
    #
    pthread
)

target_link_directories(meek_compositor PRIVATE
    ${WAYLAND_SERVER_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GLES3_LIBRARY_DIRS}
    ${GBM_LIBRARY_DIRS}
    ${LIBDRM_LIBRARY_DIRS}
    ${X11_LIBRARY_DIRS}
)
