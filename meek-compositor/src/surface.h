//
//surface.h -- wl_surface + wl_region + frame-callback subsystem.
//
//Split out of globals.c for readability (globals.c had grown to
//1700+ lines covering every compositor-wide protocol). This file
//owns:
//  * struct _globals_internal__surface + its lifecycle
//  * wl_region (used by surfaces for opaque/input region plumbing)
//  * the wl_surface -> scene-node bridging needed on commit
//  * wl_surface.frame deferred callbacks + vblank drain
//
//External callers still access this subsystem through globals.h's
//stable API (globals__wl_surface_set_role_hook,
//globals__fire_frame_callbacks). Helpers here with wider names
//(surface__*) are for cross-file use inside the compositor itself
//(notably wl_compositor.create_surface in globals.c and first-
//surface-for-client in seat.c).
//
#ifndef MEEK_COMPOSITOR_SURFACE_H
#define MEEK_COMPOSITOR_SURFACE_H

#include <stdint.h>

struct wl_client;
struct wl_display;
struct wl_resource;

//
//Create a wl_surface resource for `client` at protocol `id` with
//version `version`. Called from wl_compositor.create_surface in
//globals.c. Allocates the _globals_internal__surface struct,
//inserts it into the surfaces list, wires the surface_interface
//vtable. Posts no_memory on allocation failure.
//
void surface__create(struct wl_client* client, uint32_t version, uint32_t id);

//
//Create a wl_region resource. Same idea, for wl_compositor.create_region.
//
void surface__create_region(struct wl_client* client, uint32_t version, uint32_t id);

//
//First wl_surface we find whose owning wl_client matches `c`. Used
//by the seat subsystem to satisfy wl_touch.down's surface argument
//when it only has the client at hand. Returns NULL if none.
//
struct wl_resource* surface__first_resource_for_client(struct wl_client* c);

//
//Initialize the surfaces list. Must run before any wl_compositor
//bind can reach us. Called by globals__register_all.
//
void surface__init(void);

#endif
