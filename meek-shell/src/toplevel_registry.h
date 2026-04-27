#ifndef MEEK_SHELL_TOPLEVEL_REGISTRY_H
#define MEEK_SHELL_TOPLEVEL_REGISTRY_H

//
// toplevel_registry.h - bookkeeping for non-shell app windows the
// compositor forwards to us via meek_shell_v1.toplevel_*.
//
// One entry per live non-shell toplevel. Populated by
// meek_shell_v1_client.c event handlers (add/title_changed/
// buffer/removed). Consumed by the eventual <client-window> widget
// (Phase 3) which asks the registry for the GL texture to sample
// for each visible app tile.
//
// Storage: fixed-size array, linear scan. No free app will be
// running 32+ windows on a phone, and linear-scan of a 32-slot
// array is cheaper than any alternative at this size.
//

#include "types.h"

#define TOPLEVEL_REGISTRY_MAX 32
#define TOPLEVEL_REGISTRY_APPID_LEN 128
#define TOPLEVEL_REGISTRY_TITLE_LEN 256

struct toplevel_entry
{
	uint handle; // 0 = unused slot; otherwise monotonic from compositor.
	char app_id[TOPLEVEL_REGISTRY_APPID_LEN];
	char title[TOPLEVEL_REGISTRY_TITLE_LEN];
	uint gl_texture; // 0 = no buffer received yet.
	void *egl_image; // EGLImage (opaque); 0 = none.
	int64 width;
	int64 height;
	uint fourcc; // last committed format (DRM fourcc).
	//
	// Scene node that renders this toplevel's texture. Allocated by
	// meek_shell_v1_client on toplevel_added, destroyed on
	// toplevel_removed. Stored as void* so this header doesn't need
	// to pull in meek-ui's gui.h -- shell code casts to gui_node*.
	//
	void *scene_node;

	//
	// CLOCK_MONOTONIC ms timestamp of the most recent
	// platform_wayland__request_render() wake fired for this
	// handle. Used by meek_shell_v1_client.c to honour the
	// settings.preview_max_fps rate limit. 0 = never woken yet
	// (memset zero from add). Not part of the persisted state;
	// strictly a runtime throttling cursor.
	//
	uint64 last_preview_wake_ms;
};

//
// Insert a fresh entry for `handle` (or return the existing one if
// already registered -- rebind replay after shell restart). `app_id`
// and `title` may be NULL; stored as empty strings then. Returns
// pointer into the registry valid until the next remove; do not
// keep across event boundaries.
//
struct toplevel_entry *toplevel_registry__add(uint handle, const char *app_id, const char *title);

//
// Lookup by handle. Returns NULL if not found.
//
struct toplevel_entry *toplevel_registry__find(uint handle);

//
// Free the entry's texture + EGLImage if any and mark the slot
// unused. No-op if handle not found. The caller is responsible for
// passing the right EGLDisplay so we can eglDestroyImage; we take
// it as a void* to keep this header EGL-type-free.
//
void toplevel_registry__remove(uint handle, void *egl_display);

//
// Iterate every in-use entry. `cb` returns non-zero to stop early.
// Used by the widget layer to lay out tiles.
//
void toplevel_registry__foreach(int (*cb)(struct toplevel_entry *, void *userdata), void *userdata);

//
// Count of in-use entries. Used by UI for "how many tiles do I
// have to render" sizing.
//
int toplevel_registry__count(void);

#endif
