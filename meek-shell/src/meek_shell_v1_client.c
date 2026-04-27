//
// meek_shell_v1_client.c - meek-shell's client side of the
// privileged extension (D3 pass).
//
// See meek_shell_v1_client.h for the public surface + fallback
// semantics.
//
// IMPLEMENTATION NOTES:
//
// We install our OWN wl_registry listener on the shared display,
// separate from the one meek-ui's platform backend already has.
// libwayland supports any number of listeners per client; each
// listener sees every `global` event. We ignore everything except
// meek_shell_v1 since meek-ui already handled the standard
// protocols.
//
// wl_display_roundtrip is safe to call here too, even though
// meek-ui already did one during its init -- roundtrip just
// blocks until a wl_display.sync response comes back, which
// fires our listener for any queued globals.
//
// ONE wl_display, TWO registries. This matters for cleanup: we
// own our wl_registry and must destroy it in shutdown; we do NOT
// own the wl_display (meek-ui's backend does). Never call
// wl_display_disconnect here.
//

#include <errno.h>
#include <linux/input-event-codes.h> //KEY_A / KEY_B / ... evdev codes for route_keyboard_key.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> //mmap() for shm pixel pulls.
#include <time.h> //clock_gettime for key event timestamps.
#include <unistd.h> //close() for dmabuf fds received via SCM_RIGHTS.

#include <wayland-client.h>

// clang-format off
//
// GLES3/gl3.h MUST come before GLES2/gl2ext.h: gl2ext.h uses the
// GL_APIENTRYP / GL_APIENTRY macros that are defined via
// gl2platform.h (pulled in transitively by gl3.h). An auto-
// formatter that sorts these alphabetically swaps the order and
// breaks the build with "expected ')' before '*' token" on every
// PFNGL*PROC typedef. The clang-format off / on guards keep the
// order stable across formatter runs.
//
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
// clang-format on

#include "meek-shell-v1-client-protocol.h"

#include "debug_definitions.h"
#include "third_party/log.h"

#include "gesture_recognizer.h" //edge-swipe recognizer (Phase 5).
#include "gui.h" //gui_node_type + scene graph types.
#include "meek_shell_v1_client.h"
#include "platforms/linux/platform_linux_wayland_client.h" //EGL handle getters.
#include "scene.h" //scene__node_new / scene__add_child / scene__find_by_id.
#include "settings.h" //preview_max_fps rate-limit + Phase 2 tunables.
#include "toplevel_registry.h"
#include "widgets/widget_process_window.h" //set_texture / set_handle.

//
// module-level state
//
static struct wl_registry *_meek_shell_v1_client_internal__registry = NULL;
static struct meek_shell_v1 *_meek_shell_v1_client_internal__handle = NULL;
static struct wl_display *_meek_shell_v1_client_internal__display = NULL;
static int _meek_shell_v1_client_internal__live = 0;

//
// Currently-focused app handle for wl_keyboard routing. Set by
// main.c on fullscreen-enter, cleared to 0 on exit. When non-zero,
// every char from the on-screen keyboard is translated to an evdev
// keycode and forwarded via meek_shell_v1.route_keyboard_key so
// foot / any non-text_input_v3 consumer receives real key events.
//
// Why we track it here instead of reaching into main.c: the scene
// char-redirect callback runs inside meek-ui's event dispatch;
// passing the handle through that callback chain would require a
// scene API addition. A module-local cache avoids the plumbing.
//
static uint32_t _meek_shell_v1_client_internal__focused_kbd_handle = 0;

//
// forward decls for file-local statics
//
static void _meek_shell_v1_client_internal__on_registry_global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version);
static void _meek_shell_v1_client_internal__on_registry_global_remove(void *data, struct wl_registry *r, uint32_t name);

static void _meek_shell_v1_client_internal__on_toplevel_added(void *data, struct meek_shell_v1 *s, uint32_t handle, const char *app_id, const char *title);
static void _meek_shell_v1_client_internal__on_toplevel_title_changed(void *data, struct meek_shell_v1 *s, uint32_t handle, const char *title);
static void _meek_shell_v1_client_internal__on_toplevel_removed(void *data, struct meek_shell_v1 *s, uint32_t handle);
static void _meek_shell_v1_client_internal__on_toplevel_buffer(void *data, struct meek_shell_v1 *s, uint32_t handle, uint32_t buffer_id, int32_t fd, int32_t w, int32_t h, int32_t stride, uint32_t fourcc, uint32_t modifier_hi, uint32_t modifier_lo);
static void _meek_shell_v1_client_internal__on_buffer_forgotten(void *data, struct meek_shell_v1 *s, uint32_t buffer_id);
static void _meek_shell_v1_client_internal__on_toplevel_buffer_shm(void *data, struct meek_shell_v1 *s, uint32_t handle, uint32_t buffer_id, int32_t fd, int32_t w, int32_t h, int32_t stride, uint32_t format);
static void _meek_shell_v1_client_internal__on_ime_request_on(void *data, struct meek_shell_v1 *s, uint32_t app_handle);
static void _meek_shell_v1_client_internal__on_ime_request_off(void *data, struct meek_shell_v1 *s, uint32_t app_handle);
static void _meek_shell_v1_client_internal__on_pointer_motion_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t x, int32_t y);
static void _meek_shell_v1_client_internal__on_pointer_button_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, uint32_t button, uint32_t state);
static void _meek_shell_v1_client_internal__on_key_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, uint32_t keycode, uint32_t state);
static void _meek_shell_v1_client_internal__on_touch_down_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t id, int32_t x, int32_t y);
static void _meek_shell_v1_client_internal__on_touch_motion_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t id, int32_t x, int32_t y);
static void _meek_shell_v1_client_internal__on_touch_up_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t id);
static void _meek_shell_v1_client_internal__on_frame_presented(void *data, struct meek_shell_v1 *s, uint32_t time_ms);

static int _meek_shell_v1_client_internal__should_wake_for_handle(uint32_t handle);

//============================================================================
// listener tables
//============================================================================

static const struct wl_registry_listener _meek_shell_v1_client_internal__registry_listener = {
	.global = _meek_shell_v1_client_internal__on_registry_global,
	.global_remove = _meek_shell_v1_client_internal__on_registry_global_remove,
};

static const struct meek_shell_v1_listener _meek_shell_v1_client_internal__listener = {
	.toplevel_added = _meek_shell_v1_client_internal__on_toplevel_added,
	.toplevel_title_changed = _meek_shell_v1_client_internal__on_toplevel_title_changed,
	.toplevel_removed = _meek_shell_v1_client_internal__on_toplevel_removed,
	.toplevel_buffer = _meek_shell_v1_client_internal__on_toplevel_buffer,
	.buffer_forgotten = _meek_shell_v1_client_internal__on_buffer_forgotten,
	.toplevel_buffer_shm = _meek_shell_v1_client_internal__on_toplevel_buffer_shm,
	.ime_request_on = _meek_shell_v1_client_internal__on_ime_request_on,
	.ime_request_off = _meek_shell_v1_client_internal__on_ime_request_off,
	.pointer_motion_raw = _meek_shell_v1_client_internal__on_pointer_motion_raw,
	.pointer_button_raw = _meek_shell_v1_client_internal__on_pointer_button_raw,
	.key_raw = _meek_shell_v1_client_internal__on_key_raw,
	.touch_down_raw = _meek_shell_v1_client_internal__on_touch_down_raw,
	.touch_motion_raw = _meek_shell_v1_client_internal__on_touch_motion_raw,
	.touch_up_raw = _meek_shell_v1_client_internal__on_touch_up_raw,
	.frame_presented = _meek_shell_v1_client_internal__on_frame_presented,
};

//============================================================================
// registry: look for meek_shell_v1 and bind it
//============================================================================

static void _meek_shell_v1_client_internal__on_registry_global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
	(void)data;
	if (strcmp(interface, meek_shell_v1_interface.name) != 0)
	{
		//
		// Ignore everything else. meek-ui's platform backend is
		// already handling wl_compositor / xdg_wm_base / etc. on
		// its own registry listener.
		//
		return;
	}
	if (_meek_shell_v1_client_internal__handle != NULL)
	{
		log_warn("meek_shell_v1_client: duplicate global advertisement (name=%u)", name);
		return;
	}

	//
	// We advertise/support v1 only. Clamp the bind version to
	// what we understand so the compositor knows not to send
	// events from newer versions.
	//
	uint32_t v = version < 1 ? version : 1;
	_meek_shell_v1_client_internal__handle = wl_registry_bind(r, name, &meek_shell_v1_interface, v);
	if (_meek_shell_v1_client_internal__handle == NULL)
	{
		log_error("meek_shell_v1_client: wl_registry_bind returned NULL "
				  "(name=%u v=%u -- was bind gated?)",
			name, v);
		return;
	}

	meek_shell_v1_add_listener(_meek_shell_v1_client_internal__handle, &_meek_shell_v1_client_internal__listener, NULL);
	meek_shell_v1_announce_ready(_meek_shell_v1_client_internal__handle);

	_meek_shell_v1_client_internal__live = 1;
	log_info("meek_shell_v1_client: bound v=%u, announce_ready sent", v);
}

static void _meek_shell_v1_client_internal__on_registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data;
	(void)r;
	(void)name;
	//
	// Compositor globals don't normally go away. If ours does,
	// the compositor is probably shutting down; not much we can
	// do usefully here.
	//
}

//============================================================================
// EGL extension entry points for dmabuf import.
//
// Resolved on first use via eglGetProcAddress. NULL after resolve
// means the EGL display we got from the platform doesn't support
// dmabuf import -- we log-warn and skip imports (the protocol
// event still arrives; we just can't turn it into a texture).
//============================================================================

typedef EGLImage (*_fncp_eglCreateImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib *);
typedef EGLBoolean (*_fncp_eglDestroyImage)(EGLDisplay, EGLImage);
typedef void (*_fncp_glEGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);

static _fncp_eglCreateImage _meek_shell_v1_client_internal__create_image = NULL;
static _fncp_eglDestroyImage _meek_shell_v1_client_internal__destroy_image = NULL;
static _fncp_glEGLImageTargetTexture2DOES _meek_shell_v1_client_internal__tex_from_image = NULL;
static int _meek_shell_v1_client_internal__egl_resolved = 0;

//
// Buffer cache keyed by the compositor-assigned `buffer_id`. First
// time we see an id we import the dmabuf into a new GL texture +
// EGLImage and store them here. Subsequent commits of the same
// underlying wl_buffer (triple-buffer rotation) arrive with the
// same id and we just reuse. Entries are evicted on the
// `buffer_forgotten` event, which the compositor sends when the
// source wl_buffer is destroyed.
//
// Fixed-size array. 64 slots comfortably covers triple-buffered
// clients × several live apps. If we overrun, we evict the oldest
// entry (which may cause a fresh import but won't leak).
//
#define _MEEK_SHELL_V1_CLIENT__BUFFER_CACHE_CAP 64
typedef struct _meek_shell_v1_client_internal__buffer_cache_entry
{
	uint32_t id; // 0 = free slot
	GLuint gl_texture;
	EGLImage egl_image;
	int width;
	int height;
	uint64_t last_used_seq; // eviction timestamp
} _meek_shell_v1_client_internal__buffer_cache_entry;

static _meek_shell_v1_client_internal__buffer_cache_entry _meek_shell_v1_client_internal__buffer_cache[_MEEK_SHELL_V1_CLIENT__BUFFER_CACHE_CAP];
static uint64_t _meek_shell_v1_client_internal__buffer_cache_seq = 0;

static _meek_shell_v1_client_internal__buffer_cache_entry *_meek_shell_v1_client_internal__cache_find(uint32_t id)
{
	if (id == 0)
	{
		return NULL;
	}
	for (int i = 0; i < _MEEK_SHELL_V1_CLIENT__BUFFER_CACHE_CAP; i++)
	{
		if (_meek_shell_v1_client_internal__buffer_cache[i].id == id)
		{
			return &_meek_shell_v1_client_internal__buffer_cache[i];
		}
	}
	return NULL;
}

static _meek_shell_v1_client_internal__buffer_cache_entry *_meek_shell_v1_client_internal__cache_alloc(EGLDisplay dpy)
{
	//
	// First pass: free slot.
	//
	for (int i = 0; i < _MEEK_SHELL_V1_CLIENT__BUFFER_CACHE_CAP; i++)
	{
		if (_meek_shell_v1_client_internal__buffer_cache[i].id == 0)
		{
			return &_meek_shell_v1_client_internal__buffer_cache[i];
		}
	}
	//
	// Second pass: LRU evict. Shouldn't normally happen (64 > typical
	// live-buffer working set) but keeps us from leaking if a
	// misbehaving compositor never fires buffer_forgotten.
	//
	int victim = 0;
	for (int i = 1; i < _MEEK_SHELL_V1_CLIENT__BUFFER_CACHE_CAP; i++)
	{
		if (_meek_shell_v1_client_internal__buffer_cache[i].last_used_seq < _meek_shell_v1_client_internal__buffer_cache[victim].last_used_seq)
		{
			victim = i;
		}
	}
	_meek_shell_v1_client_internal__buffer_cache_entry *e = &_meek_shell_v1_client_internal__buffer_cache[victim];
	if (e->gl_texture != 0)
	{
		glDeleteTextures(1, &e->gl_texture);
	}
	if (e->egl_image != EGL_NO_IMAGE && _meek_shell_v1_client_internal__destroy_image != NULL)
	{
		_meek_shell_v1_client_internal__destroy_image(dpy, e->egl_image);
	}
	log_warn("[meek-shell] buffer cache evicting id=%u (LRU, cache full)", e->id);
	e->id = 0;
	e->gl_texture = 0;
	e->egl_image = EGL_NO_IMAGE;
	return e;
}

static void _meek_shell_v1_client_internal__resolve_egl_ext(void)
{
	if (_meek_shell_v1_client_internal__egl_resolved)
	{
		return;
	}
	_meek_shell_v1_client_internal__egl_resolved = 1;
	_meek_shell_v1_client_internal__create_image = (_fncp_eglCreateImage)eglGetProcAddress("eglCreateImage");
	_meek_shell_v1_client_internal__destroy_image = (_fncp_eglDestroyImage)eglGetProcAddress("eglDestroyImage");
	_meek_shell_v1_client_internal__tex_from_image = (_fncp_glEGLImageTargetTexture2DOES)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (_meek_shell_v1_client_internal__create_image == NULL || _meek_shell_v1_client_internal__destroy_image == NULL || _meek_shell_v1_client_internal__tex_from_image == NULL)
	{
		log_warn("meek_shell_v1_client: EGL dmabuf-import ext entry points missing; "
				 "scanout imports will be skipped");
	}
}

//============================================================================
// dmabuf import helper. Takes the plane-0 info forwarded via
// meek_shell_v1.toplevel_buffer, returns a GL texture (and the
// EGLImage that backs it so the registry can release it later).
//
// Runs with the meek-ui renderer's EGL context current. That's the
// context that'll sample the resulting texture at scene render
// time, so the import has to happen in that same context --
// EGL textures don't cross contexts on different displays, and we
// deliberately share the single EGL display the meek-ui platform
// owns.
//============================================================================

//
// Rebinds an existing GL texture onto a newly created EGLImage.
// `*in_out_texture` MUST be a valid GL texture name on entry (0 is
// allowed only on the first import; we'll glGenTextures in that
// case). On exit, the same texture name is kept and now samples
// from the new dmabuf.
//
// Rationale: per-buffer glGenTextures was accumulating GL objects
// at the framerate (~60/sec) and Mesa/Zink kept their backing
// VkImages live as deferred cleanup. After a couple minutes the
// driver ran out of host memory. Reusing the texture name
// sidesteps the issue: Mesa reassigns the backing VkImage when we
// rebind, and the old one is reclaimed synchronously.
//
static int _meek_shell_v1_client_internal__import_dmabuf(int fd, int32_t w, int32_t h, int32_t stride, uint32_t fourcc, uint64_t modifier, EGLDisplay dpy, GLuint *in_out_texture, EGLImage *out_image)
{
	if (_meek_shell_v1_client_internal__create_image == NULL || _meek_shell_v1_client_internal__tex_from_image == NULL)
	{
		return -1;
	}
	if (dpy == EGL_NO_DISPLAY)
	{
		log_error("meek_shell_v1_client: no EGL display available for import");
		return -1;
	}

	//
	// EGLAttrib list for single-plane dmabuf. v1 of our protocol
	// is dmabuf-single-plane only; multi-plane YUV deferred.
	//
	EGLAttrib attrs[32];
	int ai = 0;
	attrs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;
	attrs[ai++] = (EGLAttrib)fourcc;
	attrs[ai++] = EGL_WIDTH;
	attrs[ai++] = (EGLAttrib)w;
	attrs[ai++] = EGL_HEIGHT;
	attrs[ai++] = (EGLAttrib)h;
	attrs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attrs[ai++] = (EGLAttrib)fd;
	attrs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attrs[ai++] = 0;
	attrs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attrs[ai++] = (EGLAttrib)stride;
	if (modifier != 0 && modifier != ((uint64_t)-1))
	{
		attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attrs[ai++] = (EGLAttrib)(modifier & 0xFFFFFFFFu);
		attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attrs[ai++] = (EGLAttrib)(modifier >> 32);
	}
	attrs[ai++] = EGL_NONE;

	EGLImage img = _meek_shell_v1_client_internal__create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	if (img == EGL_NO_IMAGE)
	{
		log_warn("meek_shell_v1_client: eglCreateImage(DMA_BUF) failed: 0x%x", eglGetError());
		return -1;
	}

	//
	// First-import path: allocate a GL texture the caller will keep
	// for the life of the toplevel. Subsequent imports for the same
	// toplevel pass the same texture back in and we rebind in place.
	//
	GLuint tex = *in_out_texture;
	if (tex == 0)
	{
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, tex);
	}
	_meek_shell_v1_client_internal__tex_from_image(GL_TEXTURE_2D, (GLeglImageOES)img);

	//
	// Check for GL errors from the target-image bind. Adreno + some
	// Mesa drivers produce GL_INVALID_OPERATION for modifier
	// combinations they don't actually support even if
	// eglCreateImage succeeded; catch here so we don't sample
	// garbage later.
	//
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		log_warn("meek_shell_v1_client: glEGLImageTargetTexture2DOES GL error "
				 "0x%04x; discarding image",
			err);
		//
		// Keep the texture alive on failure (caller can retry on
		// next frame); drop just the newly-allocated EGLImage.
		//
		if (_meek_shell_v1_client_internal__destroy_image != NULL)
		{
			_meek_shell_v1_client_internal__destroy_image(dpy, img);
		}
		return -1;
	}

	*in_out_texture = tex;
	*out_image = img;
	return 0;
}

//============================================================================
// Scene helpers for task-switcher card management (Phase 4).
//
// Each toplevel gets a <process-window> node inserted into the
// shell's scene under whatever parent has id="cards-deck". The
// widget is created empty (no texture yet) and populated on the
// first toplevel_buffer import.
//
// A "cards-deck" container is expected in shell.ui; if it's not
// present we skip the scene integration (log warn once) so
// non-task-switcher shells can still use this protocol layer.
//============================================================================

//
// Unlink a node from its parent's child chain. meek-ui doesn't
// expose scene__remove_child; we walk the linked list and patch
// pointers ourselves. Does NOT free the node -- caller must do
// scene__node_free afterwards. Idempotent on already-detached
// nodes (NULL parent).
//
static void _meek_shell_v1_client_internal__detach_node(gui_node *n)
{
	if (n == NULL || n->parent == NULL)
	{
		return;
	}
	gui_node *parent = n->parent;

	//
	// Find n in the parent's sibling list, patch predecessor.
	//
	if (parent->first_child == n)
	{
		parent->first_child = n->next_sibling;
	}
	else
	{
		for (gui_node *c = parent->first_child; c != NULL; c = c->next_sibling)
		{
			if (c->next_sibling == n)
			{
				c->next_sibling = n->next_sibling;
				break;
			}
		}
	}
	if (parent->last_child == n)
	{
		//
		// Re-scan to find the new tail. O(child_count) but
		// toplevel lists are small (N apps = N children).
		//
		gui_node *tail = NULL;
		for (gui_node *c = parent->first_child; c != NULL; c = c->next_sibling)
		{
			tail = c;
		}
		parent->last_child = tail;
	}
	if (parent->child_count > 0)
	{
		parent->child_count--;
	}
	n->parent = NULL;
	n->next_sibling = NULL;
}

//
// Look up the deck container in shell.ui. First call caches the
// pointer; later lookups reuse it. Returns NULL (logged once) if
// the shell.ui doesn't define id="cards-deck".
//
static gui_node *_meek_shell_v1_client_internal__find_cards_deck(void)
{
	static gui_node *cached = NULL;
	static boole warned = FALSE;
	if (cached != NULL)
	{
		return cached;
	}
	cached = scene__find_by_id("cards-deck");
	if (cached == NULL && !warned)
	{
		warned = TRUE;
		log_warn("[meek-shell] no id=\"cards-deck\" in shell.ui; app-tile "
				 "insertion will be skipped");
	}
	return cached;
}

//============================================================================
// event handlers
//============================================================================

static void _meek_shell_v1_client_internal__on_toplevel_added(void *data, struct meek_shell_v1 *s, uint32_t handle, const char *app_id, const char *title)
{
	(void)data;
	(void)s;
	//
	// Register in bookkeeping first. If this fails (registry full)
	// we still log the event but skip scene-node creation.
	//
	struct toplevel_entry *e = toplevel_registry__add(handle, app_id, title);
	if (e == NULL)
	{
		log_warn("[meek-shell] toplevel_added handle=%u: registry full; skipping "
				 "scene insertion",
			handle);
		return;
	}

	//
	// Spawn a tile in the cards-deck. Each tile is a wrapper
	// <column class="tile-card"> containing the live process-window
	// texture on top + a label with the app title below. The wrapper
	// is what we store in toplevel_registry's scene_node slot so
	// cleanup on toplevel_removed frees the whole subtree.
	// on_click is wired on every node (wrapper, process-window,
	// label) so taps land regardless of which inner node the
	// hit-tester reports as deepest; main.c's tap handler walks
	// the subtree to find the process-window for the handle.
	//
	gui_node *deck = _meek_shell_v1_client_internal__find_cards_deck();
	if (deck != NULL)
	{
		gui_node *card = scene__node_new(GUI_NODE_COLUMN);
		if (card != NULL)
		{
			strncpy(card->klass, "tile-card", sizeof(card->klass) - 1);
			card->klass[sizeof(card->klass) - 1] = '\0';
			scene__set_on_click(card, "__process_window_tile_tap");

			gui_node *pw = scene__node_new(GUI_NODE_PROCESS_WINDOW);
			if (pw != NULL)
			{
				widget_process_window__set_handle(pw, handle);
				strncpy(pw->klass, "tile-process-window", sizeof(pw->klass) - 1);
				pw->klass[sizeof(pw->klass) - 1] = '\0';
				scene__set_on_click(pw, "__process_window_tile_tap");
				scene__add_child(card, pw);
			}

			gui_node *lbl = scene__node_new(GUI_NODE_TEXT);
			if (lbl != NULL)
			{
				strncpy(lbl->klass, "tile-label", sizeof(lbl->klass) - 1);
				lbl->klass[sizeof(lbl->klass) - 1] = '\0';
				//
				// Title preference: title -> app_id -> "Untitled".
				// Many Wayland clients set title late (after the
				// first commit), so toplevel_title_changed arrives
				// shortly and we refresh the label there.
				//
				const char *tag = (title != NULL && title[0] != 0) ? title : (app_id != NULL && app_id[0] != 0) ? app_id :
																												  "Untitled";
				size_t tlen = strlen(tag);
				if (tlen >= sizeof(lbl->text))
				{
					tlen = sizeof(lbl->text) - 1;
				}
				memcpy(lbl->text, tag, tlen);
				lbl->text[tlen] = 0;
				lbl->text_len = (int)tlen;
				scene__set_on_click(lbl, "__process_window_tile_tap");
				scene__add_child(card, lbl);
			}

			scene__add_child(deck, card);
			e->scene_node = (void *)card;
		}
	}

	log_info("[meek-shell] toplevel_added handle=%u app_id='%s' title='%s' "
			 "(registry count=%d)",
		handle, app_id ? app_id : "", title ? title : "", toplevel_registry__count());

	//
	// Container-transform launch: if the user just tapped a
	// launcher tile and we're inside the pending-launch window,
	// promote this new toplevel directly to fullscreen instead of
	// letting it sit silently in the cards-deck. The shell-side
	// launch animation in main.c (icon -> full panel) has either
	// already finished, or is finishing right around now -- the
	// texture set by _show_fullscreen will appear as a normal
	// foreground window.
	//
	extern boole meek_shell__try_consume_pending_launch(uint32_t handle);
	extern void  meek_shell__show_fullscreen(uint32_t handle);
	if (meek_shell__try_consume_pending_launch(handle))
	{
		log_info("[meek-shell] auto-fullscreen handle=%u (consumed pending launch)", handle);
		meek_shell__show_fullscreen(handle);
	}

	//
	// Phase 2 wake: a new tile node landed in the scene, but
	// platform_linux_wayland_client.c's render gate doesn't know
	// about meek_shell_v1 events. Tell it to redraw the next tick.
	//
	platform_wayland__request_render();
}

static void _meek_shell_v1_client_internal__on_toplevel_title_changed(void *data, struct meek_shell_v1 *s, uint32_t handle, const char *title)
{
	(void)data;
	(void)s;
	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e != NULL)
	{
		//
		// Copy-into-fixed-buffer truncates if title too long; that's
		// fine for display, and any listener code downstream already
		// must handle partial-title-on-truncation.
		//
		size_t n = title ? strlen(title) : 0;
		if (n >= sizeof(e->title))
		{
			n = sizeof(e->title) - 1;
		}
		memcpy(e->title, title ? title : "", n);
		e->title[n] = 0;

		//
		// Refresh the tile-label text in the scene. scene_node points
		// at the wrapper <column class="tile-card">; the label is the
		// child whose class is "tile-label". Walk children to find it
		// (small N, ~3 children per tile) and rewrite its text. If
		// the title is empty, fall back to app_id, then "Untitled".
		//
		gui_node *card = (gui_node *)e->scene_node;
		if (card != NULL)
		{
			const char *tag = (title != NULL && title[0] != 0) ? title : (e->app_id[0] != 0) ? e->app_id :
																							   "Untitled";
			for (gui_node *c = card->first_child; c != NULL; c = c->next_sibling)
			{
				if (strcmp(c->klass, "tile-label") == 0)
				{
					size_t tlen = strlen(tag);
					if (tlen >= sizeof(c->text))
					{
						tlen = sizeof(c->text) - 1;
					}
					memcpy(c->text, tag, tlen);
					c->text[tlen] = 0;
					c->text_len = (int)tlen;
					break;
				}
			}
		}
	}
	log_info("[meek-shell] toplevel_title_changed handle=%u title='%s'", handle, title ? title : "");
	//
	// Visibility + preview rate limit; see
	// _meek_shell_v1_client_internal__should_wake_for_handle.
	// Title text shows up in the tile, so this is treated as a
	// tile-content update for gating purposes.
	//
	if (_meek_shell_v1_client_internal__should_wake_for_handle(handle))
	{
		platform_wayland__request_render();
	}
}

static void _meek_shell_v1_client_internal__on_toplevel_removed(void *data, struct meek_shell_v1 *s, uint32_t handle)
{
	(void)data;
	(void)s;
	//
	// If the removed handle is the one the shell is currently
	// fullscreened on, exit fullscreen BEFORE tearing down the
	// scene node / texture. Otherwise the fullscreen-view keeps
	// pointing at a dead handle, draws only the bg color, and
	// leaves the user with a black screen they can't recover
	// from (every tap routes to the dead handle and does nothing).
	// See session/bugs_to_investigate.md entry #1.
	//
	extern uint32_t meek_shell__focused_handle(void);
	extern void meek_shell__show_switcher(void);
	if (meek_shell__focused_handle() == handle)
	{
		log_info("[meek-shell] toplevel_removed handle=%u was currently focused; "
				 "exiting fullscreen",
			handle);
		meek_shell__show_switcher();
	}

	//
	// Detach + free the scene node BEFORE dropping the registry
	// entry (we need the scene_node pointer stored there).
	// scene__node_free walks the widget's on_destroy hooks and
	// releases user_data; the <process-window> widget's destroy
	// hook is a no-op for the texture (we own that in the
	// registry), so ordering here is fine.
	//
	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e != NULL && e->scene_node != NULL)
	{
		gui_node *node = (gui_node *)e->scene_node;
		_meek_shell_v1_client_internal__detach_node(node);
		scene__node_free(node);
		e->scene_node = NULL;
	}

	//
	// Release texture+EGLImage via the registry. Needs EGL display
	// from the platform so eglDestroyImage has something to bind.
	//
	void *dpy = platform_wayland__get_egl_display();
	toplevel_registry__remove(handle, dpy);
	log_info("[meek-shell] toplevel_removed handle=%u (registry count=%d)", handle, toplevel_registry__count());
	platform_wayland__request_render();
}

//
// Phase 3 + preview rate-limit gate. Returns 1 if a buffer / title
// update for this handle should wake the render loop right now,
// 0 if the wake should be suppressed.
//
// Two filters apply in order:
//   1. Visibility (meek_shell__handle_is_visible from main.c).
//      Suppress wakes for handles whose pixels aren't on screen.
//   2. Rate limit (settings.preview_max_fps). When > 0, cap wakes
//      per visible handle to at most N per second using the
//      per-entry last_preview_wake_ms cursor in toplevel_registry.
//
// The texture cache + scene-node binding work happens unconditionally
// in the callers; this function only governs whether we ask the
// render loop to repaint NOW.
//
static int _meek_shell_v1_client_internal__should_wake_for_handle(uint32_t handle)
{
	extern boole meek_shell__handle_is_visible(uint32_t);
	if (!meek_shell__handle_is_visible(handle))
	{
		return 0;
	}

	const meek_shell_settings *st = meek_shell_settings__get();
	if (st->preview_max_fps <= 0)
	{
		return 1;
	}

	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e == NULL)
	{
		return 1;
	}

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
	uint64_t interval_ms = 1000ULL / (uint64_t)st->preview_max_fps;
	if (now_ms - e->last_preview_wake_ms < interval_ms)
	{
		return 0;
	}
	e->last_preview_wake_ms = now_ms;
	return 1;
}

static void _meek_shell_v1_client_internal__on_toplevel_buffer(void *data, struct meek_shell_v1 *s, uint32_t handle, uint32_t buffer_id, int32_t fd, int32_t w, int32_t h, int32_t stride, uint32_t fourcc, uint32_t modifier_hi, uint32_t modifier_lo)
{
	(void)data;
	(void)s;
	uint64_t modifier = ((uint64_t)modifier_hi << 32) | (uint64_t)modifier_lo;
	DBG_TEX log_trace("[meek-shell] toplevel_buffer handle=%u buffer_id=%u fd=%d "
					  "%dx%d stride=%d "
					  "fourcc=0x%x mod=0x%016lx",
		handle, buffer_id, fd, w, h, stride, fourcc, (unsigned long)modifier);

	_meek_shell_v1_client_internal__resolve_egl_ext();

	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e == NULL)
	{
		//
		// Shouldn't happen: compositor is supposed to send
		// toplevel_added before toplevel_buffer. If it does, create
		// a registry entry on the fly (defensive) so we don't drop
		// the buffer silently.
		//
		log_warn("[meek-shell] toplevel_buffer for unregistered handle=%u; auto-adding", handle);
		e = toplevel_registry__add(handle, NULL, NULL);
		if (e == NULL)
		{
			close(fd);
			return;
		}
	}

	EGLDisplay dpy = (EGLDisplay)platform_wayland__get_egl_display();

	//
	// Per-buffer EGLImage cache. If we've seen this buffer_id
	// before, the same wl_buffer is being re-attached (triple-buffer
	// rotation) and we already have its texture + EGLImage.  In that
	// case we close the fresh dup (EGL kept its own), bump the LRU
	// timestamp, and point the scene node at the cached texture.
	//
	_meek_shell_v1_client_internal__buffer_cache_entry *cached = _meek_shell_v1_client_internal__cache_find(buffer_id);
	GLuint cached_tex = 0;
	EGLImage cached_img = EGL_NO_IMAGE;

	if (cached != NULL)
	{
		close(fd); // EGL already owns the underlying dmabuf for this buffer_id.
		cached->last_used_seq = ++_meek_shell_v1_client_internal__buffer_cache_seq;
		cached_tex = cached->gl_texture;
		cached_img = cached->egl_image;
		DBG_TEX log_info("[dbg-tex] cache HIT buffer_id=%u handle=%u tex=%u", buffer_id, handle, cached_tex);
	}
	else
	{
		//
		// First time we've seen this buffer_id; do the import and
		// store in the cache. new_tex=0 tells _import_dmabuf to
		// glGenTextures a fresh one.
		//
		GLuint new_tex = 0;
		EGLImage new_img = EGL_NO_IMAGE;
		int import_rc = _meek_shell_v1_client_internal__import_dmabuf(fd, w, h, stride, fourcc, modifier, dpy, &new_tex, &new_img);
		close(fd);
		if (import_rc != 0)
		{
			//
			// Leave the scene's current texture alone so the tile
			// keeps showing the last-good frame rather than
			// flickering to blank.
			//
			return;
		}
		cached = _meek_shell_v1_client_internal__cache_alloc(dpy);
		cached->id = buffer_id;
		cached->gl_texture = new_tex;
		cached->egl_image = new_img;
		cached->width = w;
		cached->height = h;
		cached->last_used_seq = ++_meek_shell_v1_client_internal__buffer_cache_seq;
		cached_tex = new_tex;
		cached_img = new_img;
		DBG_TEX log_info("[dbg-tex] cache MISS import buffer_id=%u handle=%u tex=%u %dx%d", buffer_id, handle, new_tex, w, h);
	}

	//
	// Point the toplevel registry entry at the cached texture. We
	// no longer own the texture lifetime on the registry side --
	// the cache owns it and releases only on buffer_forgotten.
	// Registry still stores the current tex for widgets to sample.
	//
	e->gl_texture = cached_tex;
	e->egl_image = (void *)cached_img;
	e->width = w;
	e->height = h;
	e->fourcc = fourcc;

	//
	// Point the scene's <process-window> node at the new texture.
	// If the node wasn't created (no cards-deck in shell.ui), this
	// is a no-op and the texture still exists in the registry for
	// future consumers.
	//
	if (e->scene_node != NULL)
	{
		//
		// scene_node is the wrapper <column class="tile-card">; the
		// process-window child is what actually carries the texture.
		// Walk children once to find it. Small N (the wrapper has at
		// most ~3 children today: process-window + label + future
		// icon).
		//
		gui_node *card = (gui_node *)e->scene_node;
		gui_node *pw = NULL;
		if (card->type == GUI_NODE_PROCESS_WINDOW)
		{
			pw = card;
		}
		else
		{
			for (gui_node *c = card->first_child; c != NULL; c = c->next_sibling)
			{
				if (c->type == GUI_NODE_PROCESS_WINDOW)
				{
					pw = c;
					break;
				}
			}
		}
		if (pw != NULL)
		{
			DBG_TEX log_info("[dbg-tex] set on tile handle=%u tex=%u %dx%d", handle, cached_tex, w, h);
			widget_process_window__set_texture(pw, cached_tex, w, h);
		}
	}

	//
	// Fullscreen mirror. If this handle is the currently focused app,
	// also forward the texture to the fullscreen-view node so the
	// large surface stays in sync with live updates. No-op when no
	// app is focused or the fullscreen-view node doesn't exist.
	//
	extern uint32_t meek_shell__focused_handle(void);
	if (meek_shell__focused_handle() == handle)
	{
		//
		// Forward texture to the inner #fullscreen-app process-window.
		// #fullscreen-view is a column wrapping back-bar + app area;
		// the texture only makes sense on the inner node.
		//
		gui_node *app = scene__find_by_id("fullscreen-app");
		if (app != NULL)
		{
			DBG_TEX log_info("[dbg-tex] mirror to fullscreen-app handle=%u tex=%u %dx%d", handle, cached_tex, w, h);
			widget_process_window__set_texture(app, cached_tex, w, h);
		}
	}

	DBG_TEX log_info("[meek-shell] toplevel handle=%u imported %dx%d fourcc=0x%x tex=%u", handle, w, h, fourcc, cached_tex);

	//
	// Visibility + preview rate limit. Cache + scene-node binding
	// above are unconditional so an off-screen tile shows the
	// latest pixels as soon as it becomes visible again. See
	// _meek_shell_v1_client_internal__should_wake_for_handle for
	// the gate semantics.
	//
	if (_meek_shell_v1_client_internal__should_wake_for_handle(handle))
	{
		platform_wayland__request_render();
	}
}

static void _meek_shell_v1_client_internal__on_buffer_forgotten(void *data, struct meek_shell_v1 *s, uint32_t buffer_id)
{
	(void)data;
	(void)s;
	_meek_shell_v1_client_internal__buffer_cache_entry *entry = _meek_shell_v1_client_internal__cache_find(buffer_id);
	if (entry == NULL)
	{
		log_trace("[meek-shell] buffer_forgotten buffer_id=%u (not in cache)", buffer_id);
		return;
	}
	EGLDisplay dpy = (EGLDisplay)platform_wayland__get_egl_display();
	if (entry->gl_texture != 0)
	{
		glDeleteTextures(1, &entry->gl_texture);
	}
	if (entry->egl_image != EGL_NO_IMAGE && _meek_shell_v1_client_internal__destroy_image != NULL)
	{
		_meek_shell_v1_client_internal__destroy_image(dpy, entry->egl_image);
	}
	DBG_TEX log_info("[dbg-tex] buffer_forgotten id=%u freed tex=%u", buffer_id, entry->gl_texture);
	entry->id = 0;
	entry->gl_texture = 0;
	entry->egl_image = EGL_NO_IMAGE;
	entry->width = 0;
	entry->height = 0;
	entry->last_used_seq = 0;

	//
	// Phase 2 wake: a tile that was being displayed had its texture
	// freed; a redraw is needed to drop the stale image (or fall
	// back to a placeholder).
	//
	platform_wayland__request_render();
}

//
// toplevel_buffer_shm handler. Compositor has mmapped the client's
// shm pool + copied the pixel bytes into a fresh memfd it passes
// to us. We mmap read-only, upload to a GL texture via
// glTexImage2D (first time) or glTexSubImage2D (reuse), and point
// the scene nodes at the texture.
//
// Caching model differs from dmabuf:
//   * dmabuf: bytes are shared memory; same buffer_id = same GPU
//     memory. Cache avoids repeated eglCreateImage.
//   * shm: bytes are a fresh copy per commit. Same buffer_id may
//     carry different pixels on a re-attach. Cache stores only
//     the GL texture name (so we can glTexSubImage2D into it);
//     pixels re-upload every commit.
//
// wl_shm format codes (NOT DRM fourcc):
//   WL_SHM_FORMAT_ARGB8888 = 0  -> in memory [B, G, R, A]
//   WL_SHM_FORMAT_XRGB8888 = 1  -> in memory [B, G, R, X]
// Both map to GL_BGRA_EXT + GL_UNSIGNED_BYTE for the upload (the
// GL_EXT_texture_format_BGRA8888 extension, which every driver we
// target has).
//
static void _meek_shell_v1_client_internal__on_toplevel_buffer_shm(void *data, struct meek_shell_v1 *s, uint32_t handle, uint32_t buffer_id, int32_t fd, int32_t w, int32_t h, int32_t stride, uint32_t format)
{
	(void)data;
	(void)s;
	(void)format;

	if (w <= 0 || h <= 0 || stride < w * 4)
	{
		log_warn("[meek-shell] toplevel_buffer_shm bad dims %dx%d stride=%d; dropping", w, h, stride);
		close(fd);
		return;
	}

	size_t bytes = (size_t)stride * (size_t)h;

	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e == NULL)
	{
		log_warn("[meek-shell] toplevel_buffer_shm for unregistered handle=%u; "
				 "auto-adding",
			handle);
		e = toplevel_registry__add(handle, NULL, NULL);
		if (e == NULL)
		{
			close(fd);
			return;
		}
	}

	//
	// mmap the compositor-supplied memfd read-only. MAP_PRIVATE so
	// we don't accidentally write back. Compositor closes its fd
	// after wl_client_flush; ours is independent.
	//
	void *pixels = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	if (pixels == MAP_FAILED)
	{
		log_error("[meek-shell] toplevel_buffer_shm mmap(%zu) failed: %s", bytes, strerror(errno));
		close(fd);
		return;
	}

	//
	// Look up cache entry for buffer_id. Allocates a new texture on
	// miss; reuses on hit (glTexSubImage2D path).
	//
	_meek_shell_v1_client_internal__buffer_cache_entry *cached = _meek_shell_v1_client_internal__cache_find(buffer_id);
	GLuint tex = 0;
	boole first_upload = FALSE;
	if (cached != NULL && cached->gl_texture != 0 && cached->width == w && cached->height == h)
	{
		tex = cached->gl_texture;
		cached->last_used_seq = ++_meek_shell_v1_client_internal__buffer_cache_seq;
	}
	else
	{
		//
		// Either never seen this id, or dimensions changed (client
		// resize). Drop any stale texture + allocate fresh.
		//
		if (cached != NULL && cached->gl_texture != 0)
		{
			glDeleteTextures(1, &cached->gl_texture);
			cached->gl_texture = 0;
		}
		if (cached == NULL)
		{
			EGLDisplay dpy = (EGLDisplay)platform_wayland__get_egl_display();
			cached = _meek_shell_v1_client_internal__cache_alloc(dpy);
		}
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		cached->id = buffer_id;
		cached->gl_texture = tex;
		cached->egl_image = EGL_NO_IMAGE; // shm path has no EGLImage.
		cached->width = w;
		cached->height = h;
		cached->last_used_seq = ++_meek_shell_v1_client_internal__buffer_cache_seq;
		first_upload = TRUE;
	}

	//
	// Upload. Row length is stride/4 (bytes per row / bytes per
	// pixel) so GL reads the client's row padding correctly.
	//
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
	if (first_upload)
	{
		//
		// GL_BGRA is the right format for WL_SHM_FORMAT_ARGB8888 /
		// XRGB8888 on little-endian. On GLES3 the enum lives in
		// GL_EXT_texture_format_BGRA8888; we just use its numeric
		// value (0x80E1) via the GLES2 header include.
		//
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
	}
	else
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	munmap(pixels, bytes);
	close(fd);

	e->gl_texture = tex;
	e->egl_image = NULL;
	e->width = w;
	e->height = h;
	e->fourcc = format; // shm format, not DRM fourcc; harmless either way for the
	// registry.

	if (e->scene_node != NULL)
	{
		//
		// scene_node = wrapper column; resolve to the process-window
		// child. Same pattern as the dmabuf path above.
		//
		gui_node *card = (gui_node *)e->scene_node;
		gui_node *pw = NULL;
		if (card->type == GUI_NODE_PROCESS_WINDOW)
		{
			pw = card;
		}
		else
		{
			for (gui_node *c = card->first_child; c != NULL; c = c->next_sibling)
			{
				if (c->type == GUI_NODE_PROCESS_WINDOW)
				{
					pw = c;
					break;
				}
			}
		}
		if (pw != NULL)
		{
			widget_process_window__set_texture(pw, tex, w, h);
		}
	}

	//
	// Fullscreen mirror, same pattern as dmabuf path.
	//
	extern uint32_t meek_shell__focused_handle(void);
	if (meek_shell__focused_handle() == handle)
	{
		gui_node *app = scene__find_by_id("fullscreen-app");
		if (app != NULL)
		{
			widget_process_window__set_texture(app, tex, w, h);
		}
	}

	DBG_TEX log_info("[dbg-tex] shm upload handle=%u buffer_id=%u tex=%u %dx%d stride=%d %s", handle, buffer_id, tex, w, h, stride, first_upload ? "NEW" : "reuse");

	//
	// Visibility + preview rate limit; see
	// _meek_shell_v1_client_internal__should_wake_for_handle.
	//
	if (_meek_shell_v1_client_internal__should_wake_for_handle(handle))
	{
		platform_wayland__request_render();
	}
}

//
// char -> (evdev_keycode, shift?) translation for the on-screen
// keyboard. When the user taps a letter in widget_keyboard, meek-ui
// fires a char event with the unicode codepoint; we translate it
// here to the kernel's evdev keycode (from
// <linux/input-event-codes.h>) so foot / gtk / qt can decode it via
// their libxkbcommon using the us-qwerty keymap the compositor
// sent at wl_seat.get_keyboard.
//
// Why this table instead of reverse-looking-up via xkbcommon: we'd
// need to carry our own xkb_state on the shell side to do that, and
// shell-internal shortcuts aren't active yet. A small literal table
// covers ASCII printable + enter + backspace, which is enough to
// type "ls -la" in a terminal. Extending to non-ASCII lives in a
// later pass once we need non-Latin input for shell shortcuts.
//
struct _meek_shell_v1_client_internal__char_key_entry
{
	uint codepoint;
	uint32_t keycode;
	int needs_shift;
};

static const struct _meek_shell_v1_client_internal__char_key_entry _meek_shell_v1_client_internal__char_key_table[] = {
	//
	// Letters: lowercase = plain; uppercase = same keycode + shift.
	//
	{ 'a', KEY_A, 0 },
	{ 'b', KEY_B, 0 },
	{ 'c', KEY_C, 0 },
	{ 'd', KEY_D, 0 },
	{ 'e', KEY_E, 0 },
	{ 'f', KEY_F, 0 },
	{ 'g', KEY_G, 0 },
	{ 'h', KEY_H, 0 },
	{ 'i', KEY_I, 0 },
	{ 'j', KEY_J, 0 },
	{ 'k', KEY_K, 0 },
	{ 'l', KEY_L, 0 },
	{ 'm', KEY_M, 0 },
	{ 'n', KEY_N, 0 },
	{ 'o', KEY_O, 0 },
	{ 'p', KEY_P, 0 },
	{ 'q', KEY_Q, 0 },
	{ 'r', KEY_R, 0 },
	{ 's', KEY_S, 0 },
	{ 't', KEY_T, 0 },
	{ 'u', KEY_U, 0 },
	{ 'v', KEY_V, 0 },
	{ 'w', KEY_W, 0 },
	{ 'x', KEY_X, 0 },
	{ 'y', KEY_Y, 0 },
	{ 'z', KEY_Z, 0 },

	{ 'A', KEY_A, 1 },
	{ 'B', KEY_B, 1 },
	{ 'C', KEY_C, 1 },
	{ 'D', KEY_D, 1 },
	{ 'E', KEY_E, 1 },
	{ 'F', KEY_F, 1 },
	{ 'G', KEY_G, 1 },
	{ 'H', KEY_H, 1 },
	{ 'I', KEY_I, 1 },
	{ 'J', KEY_J, 1 },
	{ 'K', KEY_K, 1 },
	{ 'L', KEY_L, 1 },
	{ 'M', KEY_M, 1 },
	{ 'N', KEY_N, 1 },
	{ 'O', KEY_O, 1 },
	{ 'P', KEY_P, 1 },
	{ 'Q', KEY_Q, 1 },
	{ 'R', KEY_R, 1 },
	{ 'S', KEY_S, 1 },
	{ 'T', KEY_T, 1 },
	{ 'U', KEY_U, 1 },
	{ 'V', KEY_V, 1 },
	{ 'W', KEY_W, 1 },
	{ 'X', KEY_X, 1 },
	{ 'Y', KEY_Y, 1 },
	{ 'Z', KEY_Z, 1 },

	//
	// Digits + shifted US-QWERTY top-row punctuation.
	//
	{ '0', KEY_0, 0 },
	{ '1', KEY_1, 0 },
	{ '2', KEY_2, 0 },
	{ '3', KEY_3, 0 },
	{ '4', KEY_4, 0 },
	{ '5', KEY_5, 0 },
	{ '6', KEY_6, 0 },
	{ '7', KEY_7, 0 },
	{ '8', KEY_8, 0 },
	{ '9', KEY_9, 0 },

	{ ')', KEY_0, 1 },
	{ '!', KEY_1, 1 },
	{ '@', KEY_2, 1 },
	{ '#', KEY_3, 1 },
	{ '$', KEY_4, 1 },
	{ '%', KEY_5, 1 },
	{ '^', KEY_6, 1 },
	{ '&', KEY_7, 1 },
	{ '*', KEY_8, 1 },
	{ '(', KEY_9, 1 },

	//
	// Common punctuation + whitespace. Shifted variants on the same
	// physical key get needs_shift=1.
	//
	{ ' ', KEY_SPACE, 0 },
	{ '\n', KEY_ENTER, 0 },
	{ '\t', KEY_TAB, 0 },
	{ '\b', KEY_BACKSPACE, 0 },

	{ '-', KEY_MINUS, 0 },
	{ '_', KEY_MINUS, 1 },
	{ '=', KEY_EQUAL, 0 },
	{ '+', KEY_EQUAL, 1 },
	{ '[', KEY_LEFTBRACE, 0 },
	{ '{', KEY_LEFTBRACE, 1 },
	{ ']', KEY_RIGHTBRACE, 0 },
	{ '}', KEY_RIGHTBRACE, 1 },
	{ '\\', KEY_BACKSLASH, 0 },
	{ '|', KEY_BACKSLASH, 1 },
	{ ';', KEY_SEMICOLON, 0 },
	{ ':', KEY_SEMICOLON, 1 },
	{ '\'', KEY_APOSTROPHE, 0 },
	{ '"', KEY_APOSTROPHE, 1 },
	{ '`', KEY_GRAVE, 0 },
	{ '~', KEY_GRAVE, 1 },
	{ ',', KEY_COMMA, 0 },
	{ '<', KEY_COMMA, 1 },
	{ '.', KEY_DOT, 0 },
	{ '>', KEY_DOT, 1 },
	{ '/', KEY_SLASH, 0 },
	{ '?', KEY_SLASH, 1 },
};

//
// wl_keyboard.modifiers bit layout used by xkbcommon's default
// keymap. We only drive shift from the shell's char mapping; caps /
// ctrl / alt would come from dedicated modifier keys on a larger
// keyboard widget.
//
#define _MEEK_SHELL_V1_CLIENT_INTERNAL__MOD_SHIFT (1u << 0)

static boole _meek_shell_v1_client_internal__lookup_keycode(uint codepoint, uint32_t *out_keycode, int *out_needs_shift)
{
	int64 n = (int64)(sizeof(_meek_shell_v1_client_internal__char_key_table) / sizeof(_meek_shell_v1_client_internal__char_key_table[0]));
	for (int64 i = 0; i < n; ++i)
	{
		const struct _meek_shell_v1_client_internal__char_key_entry *e = &_meek_shell_v1_client_internal__char_key_table[i];
		if (e->codepoint == codepoint)
		{
			*out_keycode = e->keycode;
			*out_needs_shift = e->needs_shift;
			return TRUE;
		}
	}
	return FALSE;
}

//
// Monotonic millisecond timestamp for wl_keyboard.key events.
// Wayland says the timebase is "arbitrary, shared by all events
// from a given seat"; we pick CLOCK_MONOTONIC to match libinput.
//
static uint32_t _meek_shell_v1_client_internal__now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

//
// IME bridge (layer 2 of zwp_text_input_v3 integration).
//
// Compositor fires ime_request_on when a foreign Wayland client
// enables its text_input (e.g. foot when cursor is in the
// terminal). We show the on-screen keyboard so the user can type.
// Each key tap delivers a codepoint through scene's char-redirect,
// and we forward via ime_commit_string back to the compositor,
// which emits commit_string + done on the original text_input.
//
static boole _meek_shell_v1_client_internal__ime_char_redirect(uint codepoint)
{
	//
	// UTF-8 encode the codepoint. zwp_text_input_v3.commit_string
	// takes a string, so even a single character gets sent as a
	// 1..4-byte UTF-8 sequence. ASCII fast path: 1 byte + NUL.
	//
	char buf[8];
	int len = 0;
	if (codepoint < 0x80)
	{
		buf[len++] = (char)codepoint;
	}
	else if (codepoint < 0x800)
	{
		buf[len++] = (char)(0xC0 | (codepoint >> 6));
		buf[len++] = (char)(0x80 | (codepoint & 0x3F));
	}
	else if (codepoint < 0x10000)
	{
		buf[len++] = (char)(0xE0 | (codepoint >> 12));
		buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
		buf[len++] = (char)(0x80 | (codepoint & 0x3F));
	}
	else
	{
		buf[len++] = (char)(0xF0 | (codepoint >> 18));
		buf[len++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
		buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
		buf[len++] = (char)(0x80 | (codepoint & 0x3F));
	}
	buf[len] = '\0';

	if (_meek_shell_v1_client_internal__handle != NULL)
	{
		//
		// PATH 1: text_input_v3 IME (GTK / Qt / most toolkits).
		// The compositor forwards this as commit_string + done on
		// whichever app enabled text_input. Harmless no-op if no
		// app has enabled it (text_input_v3.c drops with a trace
		// log).
		//
		meek_shell_v1_ime_commit_string(_meek_shell_v1_client_internal__handle, buf);

		//
		// PATH 2: wl_keyboard (terminals). foot / xterm and similar
		// terminals don't use text_input_v3 for Latin input;
		// they read wl_keyboard.key and translate through their own
		// xkbcommon. Map the codepoint to an evdev keycode, fire
		// press + release (with shift modifiers bracketing the pair
		// if needed) on the currently-focused app handle.
		//
		uint32_t keycode;
		int table_needs_shift;
		if (_meek_shell_v1_client_internal__focused_kbd_handle != 0 && _meek_shell_v1_client_internal__lookup_keycode(codepoint, &keycode, &table_needs_shift))
		{
			uint32_t h = _meek_shell_v1_client_internal__focused_kbd_handle;
			uint32_t t = _meek_shell_v1_client_internal__now_ms();

			//
			// Build the full modifier bitmask. Two sources:
			//
			//   (A) char-table's implicit "needs_shift" column --
			//       for punctuation that's a shifted digit or
			//       shifted key on us-qwerty (e.g. `@` = shift+2).
			//       The user doesn't tap SHIFT for these; the
			//       table encodes it.
			//
			//   (B) scene__get_active_modifiers() -- what
			//       widget_keyboard pushed just before calling
			//       scene__on_char. Carries user-latched shift (for
			//       typing uppercase letters) and user-latched ctrl
			//       (for terminal combos like ctrl-C / ctrl-D).
			//
			// Without (B), tapping CTRL + a letter on the on-screen
			// keyboard would look identical to tapping just the
			// letter, so foot would receive plain chars and ctrl
			// wouldn't work.
			//
			uint32_t mods = scene__get_active_modifiers();
			if (table_needs_shift)
			{
				mods |= _MEEK_SHELL_V1_CLIENT_INTERNAL__MOD_SHIFT;
			}

			if (mods != 0)
			{
				meek_shell_v1_route_keyboard_modifiers(_meek_shell_v1_client_internal__handle, h, mods, 0, 0, 0);
			}
			//
			// Press and release on the same (simulated) tap. The +1ms
			// on release is cosmetic -- keeps the two events from
			// sharing a timestamp, matches the shape libinput produces
			// for a quick keypress.
			//
			meek_shell_v1_route_keyboard_key(_meek_shell_v1_client_internal__handle, h, t, keycode, 1);
			meek_shell_v1_route_keyboard_key(_meek_shell_v1_client_internal__handle, h, t + 1, keycode, 0);
			if (mods != 0)
			{
				meek_shell_v1_route_keyboard_modifiers(_meek_shell_v1_client_internal__handle, h, 0, 0, 0, 0);
			}
		}

		wl_display_flush(_meek_shell_v1_client_internal__display);
	}
	return TRUE; // we handled it; skip normal widget-char delivery.
}

//
// Focus setter called by main.c on fullscreen enter/exit. Passing
// 0 disables wl_keyboard routing (taps then only fire the
// ime_commit_string path, which is a safe no-op when no app has
// text_input_v3 enabled).
//
void meek_shell_v1_client__set_keyboard_focus(uint app_handle)
{
	_meek_shell_v1_client_internal__focused_kbd_handle = app_handle;
	log_info("[meek-shell] set_keyboard_focus app_handle=%u", app_handle);
}

static void _meek_shell_v1_client_internal__on_ime_request_on(void *data, struct meek_shell_v1 *s, uint32_t app_handle)
{
	(void)data;
	(void)s;
	(void)app_handle;
	log_info("[meek-shell] ime_request_on app_handle=%u -> show keyboard", app_handle);
	scene__show_keyboard();
	scene__set_char_redirect(_meek_shell_v1_client_internal__ime_char_redirect);
	platform_wayland__request_render();
}

static void _meek_shell_v1_client_internal__on_ime_request_off(void *data, struct meek_shell_v1 *s, uint32_t app_handle)
{
	(void)data;
	(void)s;
	(void)app_handle;
	log_info("[meek-shell] ime_request_off app_handle=%u -> hide keyboard", app_handle);
	scene__set_char_redirect(NULL);
	scene__hide_keyboard();
	platform_wayland__request_render();
}

//
// Public handles for main.c's show/hide fullscreen code. We want
// to route keyboard taps to the foreign app whenever the shell is
// in fullscreen mode, regardless of whether the app requested IME.
// Covers foot / any non-text_input client.
//
void meek_shell_v1_client__install_char_redirect(void)
{
	scene__set_char_redirect(_meek_shell_v1_client_internal__ime_char_redirect);
}

void meek_shell_v1_client__clear_char_redirect(void)
{
	scene__set_char_redirect(NULL);
}

//
// Input events. D5 replaces these logs with hit-test + route-back
// calls. D3 just observes.
//
static void _meek_shell_v1_client_internal__on_pointer_motion_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t x, int32_t y)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] pointer_motion_raw t=%u (%d,%d)", time_ms, x, y);
}

static void _meek_shell_v1_client_internal__on_pointer_button_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, uint32_t button, uint32_t state)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] pointer_button_raw t=%u button=%u state=%u", time_ms, button, state);
}

static void _meek_shell_v1_client_internal__on_key_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, uint32_t keycode, uint32_t state)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] key_raw t=%u keycode=%u state=%u", time_ms, keycode, state);
}

extern void meek_shell__card_drag_on_touch_down(int32_t id, int32_t x, int32_t y);
extern void meek_shell__card_drag_on_touch_motion(int32_t id, int32_t x, int32_t y);
extern void meek_shell__card_drag_on_touch_up(int32_t id);

static void _meek_shell_v1_client_internal__on_touch_down_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t id, int32_t x, int32_t y)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] touch_down_raw t=%u id=%d (%d,%d)", time_ms, id, x, y);
	gesture_recognizer__on_touch_down(time_ms, id, x, y);
	meek_shell__card_drag_on_touch_down(id, x, y);
}

static void _meek_shell_v1_client_internal__on_touch_motion_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t id, int32_t x, int32_t y)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] touch_motion_raw t=%u id=%d (%d,%d)", time_ms, id, x, y);
	gesture_recognizer__on_touch_motion(time_ms, id, x, y);
	meek_shell__card_drag_on_touch_motion(id, x, y);
}

static void _meek_shell_v1_client_internal__on_touch_up_raw(void *data, struct meek_shell_v1 *s, uint32_t time_ms, int32_t id)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] touch_up_raw t=%u id=%d", time_ms, id);
	gesture_recognizer__on_touch_up(time_ms, id);
	meek_shell__card_drag_on_touch_up(id);
	//
	// Phase 2 wake: gesture_recognizer may have fired a UI handler
	// (on_swipe_up_bottom etc.) that mutated scene state. Conservative
	// approach -- always wake on touch_up. Cost is at most 1 extra
	// render per touch which is negligible.
	//
	platform_wayland__request_render();
}

static void _meek_shell_v1_client_internal__on_frame_presented(void *data, struct meek_shell_v1 *s, uint32_t time_ms)
{
	(void)data;
	(void)s;
	DBG_INPUT log_info("[dbg-input] frame_presented t=%u", time_ms);
}

//============================================================================
// public
//============================================================================

int meek_shell_v1_client__init(struct wl_display *display)
{
	if (display == NULL)
	{
		log_error("meek_shell_v1_client__init: NULL display "
				  "(platform not initialized yet?)");
		return -1;
	}

	_meek_shell_v1_client_internal__display = display;
	_meek_shell_v1_client_internal__registry = wl_display_get_registry(display);
	if (_meek_shell_v1_client_internal__registry == NULL)
	{
		log_error("meek_shell_v1_client__init: wl_display_get_registry failed");
		return -1;
	}
	wl_registry_add_listener(_meek_shell_v1_client_internal__registry, &_meek_shell_v1_client_internal__registry_listener, NULL);

	//
	// Two roundtrips:
	//   * first -- compositor dispatches all its `global` events;
	//     our listener fires (bind() if we find meek_shell_v1).
	//   * second -- any events our bind triggered (e.g. initial
	//     toplevel_added for already-alive apps) get drained.
	//
	// On roundtrip failure, clean up what we've set up so far.
	// Without this the wl_registry we just created leaks.
	//
	if (wl_display_roundtrip(display) < 0)
	{
		log_error("meek_shell_v1_client__init: roundtrip 1 failed (errno=%d)", errno);
		meek_shell_v1_client__shutdown();
		return -1;
	}
	if (wl_display_roundtrip(display) < 0)
	{
		log_error("meek_shell_v1_client__init: roundtrip 2 failed (errno=%d)", errno);
		meek_shell_v1_client__shutdown();
		return -1;
	}

	if (!_meek_shell_v1_client_internal__live)
	{
		log_warn("meek_shell_v1_client: compositor did not advertise "
				 "meek_shell_v1 -- running in shell-chrome-only mode "
				 "(app windows won't be composited). Is this meek-compositor? "
				 "Was MEEK_SHELL_DEV=1 set, or is the exe path in the allowlist?");
	}

	return 0;
}

void meek_shell_v1_client__shutdown(void)
{
	if (_meek_shell_v1_client_internal__handle != NULL)
	{
		meek_shell_v1_destroy(_meek_shell_v1_client_internal__handle);
		_meek_shell_v1_client_internal__handle = NULL;
	}
	if (_meek_shell_v1_client_internal__registry != NULL)
	{
		wl_registry_destroy(_meek_shell_v1_client_internal__registry);
		_meek_shell_v1_client_internal__registry = NULL;
	}
	_meek_shell_v1_client_internal__live = 0;
}

int meek_shell_v1_client__is_live(void)
{
	return _meek_shell_v1_client_internal__live;
}

//
// Phase 6 — route_touch_* thin wrappers. These just call the
// scanner-generated request functions against the bound handle.
// Noop when not live so shell-only mode doesn't crash.
//
void meek_shell_v1_client__route_touch_down(uint handle, uint time_ms, int64 id, int64 sx, int64 sy)
{
	if (!_meek_shell_v1_client_internal__live)
	{
		return;
	}
	meek_shell_v1_route_touch_down(_meek_shell_v1_client_internal__handle, handle, time_ms, (int32_t)id, (int32_t)sx, (int32_t)sy);
}

void meek_shell_v1_client__route_touch_motion(uint handle, uint time_ms, int64 id, int64 sx, int64 sy)
{
	if (!_meek_shell_v1_client_internal__live)
	{
		return;
	}
	meek_shell_v1_route_touch_motion(_meek_shell_v1_client_internal__handle, handle, time_ms, (int32_t)id, (int32_t)sx, (int32_t)sy);
}

void meek_shell_v1_client__route_touch_up(uint handle, uint time_ms, int64 id)
{
	if (!_meek_shell_v1_client_internal__live)
	{
		return;
	}
	meek_shell_v1_route_touch_up(_meek_shell_v1_client_internal__handle, handle, time_ms, (int32_t)id);
}

//
// Polite-close request: tells the compositor to send xdg_toplevel.close
// to the target client. The target may show a save-prompt or just
// exit; this is NOT a hard kill. Kept around for non-dismiss flows
// (e.g. a future "X" button that respects the app's clean-exit path).
//
void meek_shell_v1_client__close_toplevel(uint handle)
{
	if (!_meek_shell_v1_client_internal__live)
	{
		return;
	}
	if (handle == 0)
	{
		return;
	}
	meek_shell_v1_close_toplevel(_meek_shell_v1_client_internal__handle, handle);
}

//
// Force-quit request: compositor sends xdg_toplevel.close immediately
// (so well-behaved apps get a 2-second grace window to flush state),
// then SIGKILLs the client process unconditionally 2 seconds later.
// Used by the shell's swipe-up-to-dismiss gesture in the task
// switcher -- user expects the app GONE, not "asked to close, may
// stay running".
//
void meek_shell_v1_client__kill_toplevel(uint handle)
{
	if (!_meek_shell_v1_client_internal__live)
	{
		return;
	}
	if (handle == 0)
	{
		return;
	}
	meek_shell_v1_kill_toplevel(_meek_shell_v1_client_internal__handle, handle);
}
