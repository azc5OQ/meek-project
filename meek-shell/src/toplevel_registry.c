//
// toplevel_registry.c - see header for scope.
//
// The EGL/GL object teardown on `remove` uses eglDestroyImage +
// glDeleteTextures. We resolve eglDestroyImage via eglGetProcAddress
// on first use (same entry-point resolution pattern the compositor
// uses in output_drm.c / linux_dmabuf.c).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#include "third_party/log.h"
#include "toplevel_registry.h"

//
// Fixed-size table. Slot state: handle == 0 means unused.
// Allocating this as a file-static keeps the footprint predictable
// (a few hundred bytes total) and avoids a heap allocation every
// shell startup.
//
static struct toplevel_entry _toplevel_registry_internal__slots[TOPLEVEL_REGISTRY_MAX];

//
// Entry-point cache for EGL/GLES extension functions we use at
// teardown time.
//
typedef EGLBoolean (*_fncp_eglDestroyImage)(EGLDisplay, EGLImage);
static _fncp_eglDestroyImage _toplevel_registry_internal__destroy_image = NULL;
static int _toplevel_registry_internal__resolved = 0;

static void _toplevel_registry_internal__resolve_egl_ext(void)
{
	if (_toplevel_registry_internal__resolved)
	{
		return;
	}
	_toplevel_registry_internal__resolved = 1;
	_toplevel_registry_internal__destroy_image = (_fncp_eglDestroyImage)eglGetProcAddress("eglDestroyImage");
	if (_toplevel_registry_internal__destroy_image == NULL)
	{
		log_warn("toplevel_registry: eglDestroyImage not found; EGLImages will "
				 "leak on remove");
	}
}

//
// Copy `src` into a caller-owned fixed-size buffer of `cap` bytes
// with null termination. NULL src is treated as empty string.
// Truncates silently if `src` is longer than cap-1; we'd rather
// have the first N chars of a long title than refuse to display
// the entry.
//
static void _toplevel_registry_internal__copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0)
	{
		return;
	}
	if (src == NULL)
	{
		dst[0] = 0;
		return;
	}
	size_t n = strlen(src);
	if (n >= cap)
	{
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

//
// Find the slot for `handle`, or NULL. Handle 0 never matches (it's
// our sentinel for "unused slot"), so callers can't accidentally
// look up a zeroed entry.
//
struct toplevel_entry *toplevel_registry__find(uint handle)
{
	if (handle == 0)
	{
		return NULL;
	}
	for (int i = 0; i < TOPLEVEL_REGISTRY_MAX; ++i)
	{
		if (_toplevel_registry_internal__slots[i].handle == handle)
		{
			return &_toplevel_registry_internal__slots[i];
		}
	}
	return NULL;
}

struct toplevel_entry *toplevel_registry__add(uint handle, const char *app_id, const char *title)
{
	if (handle == 0)
	{
		log_warn("toplevel_registry: refuse to add handle 0 (sentinel)");
		return NULL;
	}
	//
	// Idempotent on duplicate add (happens during announce_ready
	// replay after a shell rebind): update strings, return the
	// existing entry. Callers can treat add() as "ensure exists".
	//
	struct toplevel_entry *existing = toplevel_registry__find(handle);
	if (existing != NULL)
	{
		_toplevel_registry_internal__copy_str(existing->app_id, sizeof(existing->app_id), app_id);
		_toplevel_registry_internal__copy_str(existing->title, sizeof(existing->title), title);
		return existing;
	}

	for (int i = 0; i < TOPLEVEL_REGISTRY_MAX; ++i)
	{
		struct toplevel_entry *e = &_toplevel_registry_internal__slots[i];
		if (e->handle == 0)
		{
			memset(e, 0, sizeof(*e));
			e->handle = handle;
			_toplevel_registry_internal__copy_str(e->app_id, sizeof(e->app_id), app_id);
			_toplevel_registry_internal__copy_str(e->title, sizeof(e->title), title);
			return e;
		}
	}
	log_error("toplevel_registry: full (max=%d); toplevel handle=%u dropped", TOPLEVEL_REGISTRY_MAX, handle);
	return NULL;
}

void toplevel_registry__remove(uint handle, void *egl_display)
{
	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e == NULL)
	{
		return;
	}

	_toplevel_registry_internal__resolve_egl_ext();

	//
	// NOTE: as of the per-buffer-id EGLImage cache (critical_fixes
	// entry #5), the GL texture + EGLImage lifetime lives in the
	// meek_shell_v1_client cache, not here. The registry's gl_texture
	// field is a borrowed pointer; do NOT destroy it here or we'd
	// free something the cache still owns. The cache releases when
	// the compositor sends `buffer_forgotten` for the underlying
	// wl_buffer.
	//
	(void)egl_display;
	e->gl_texture = 0;
	e->egl_image = NULL;
	e->handle = 0; // mark slot unused
}

void toplevel_registry__foreach(int (*cb)(struct toplevel_entry *, void *), void *userdata)
{
	if (cb == NULL)
	{
		return;
	}
	for (int i = 0; i < TOPLEVEL_REGISTRY_MAX; ++i)
	{
		struct toplevel_entry *e = &_toplevel_registry_internal__slots[i];
		if (e->handle != 0)
		{
			if (cb(e, userdata) != 0)
			{
				return;
			}
		}
	}
}

int toplevel_registry__count(void)
{
	int n = 0;
	for (int i = 0; i < TOPLEVEL_REGISTRY_MAX; ++i)
	{
		if (_toplevel_registry_internal__slots[i].handle != 0)
		{
			n++;
		}
	}
	return n;
}
