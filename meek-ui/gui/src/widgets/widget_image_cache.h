#ifndef WIDGET_IMAGE_CACHE_H
#define WIDGET_IMAGE_CACHE_H

#include "types.h"
#include "gui_api.h"

//
// widget_image_cache.h - public API for the shared path-keyed image
// cache that lives in widget_image.c.
//
// Two callers outside widget_image.c need to talk to this cache:
//   - scene.c reaches in to load `background-image` textures lazily
//     during emit_default_bg.
//   - the platform layer needs to release the cache at shutdown so
//     GPU textures + path strings don't leak past renderer__shutdown
//     (the memory tracker logs them as leaks at process exit).
//
// Both prototypes used to live as `extern` declarations in their
// respective TUs; consolidating them here makes the API discoverable
// and lets us add documentation in one place.
//

/**
 * Look up a cached image by path. Loads + decodes + uploads on the
 * first request, returning the cached texture handle thereafter.
 * Bumps the LRU clock on hit so recently-used entries survive the
 * next byte-cap eviction.
 *
 * @function widget_image__cache_get_or_load
 * @param {char*} path - asset-relative path. Rejected if absolute or contains "..".
 * @param {int*} out_w - receives natural width in pixels (may be NULL).
 * @param {int*} out_h - receives natural height in pixels (may be NULL).
 * @return {void*} renderer texture handle, or NULL on any failure (file
 *   missing, decode failed, byte cap exceeded, renderer has no image
 *   pipeline). NULL is also valid for "no GPU texture but the cache
 *   knows the dimensions"; check out_w/out_h.
 */
GUI_API void* widget_image__cache_get_or_load(char* path, int* out_w, int* out_h);

/**
 * Release every cached entry: destroys the GPU texture, frees the
 * tracked-bytes accounting, clears the LRU clock. Safe to call
 * multiple times. Must be called BEFORE renderer__shutdown so the
 * texture-destroy callbacks reach a live backend.
 *
 * @function widget_image__cache_shutdown
 * @return {void}
 */
GUI_API void widget_image__cache_shutdown(void);

#endif
