//
// icon_color_sampler.c - dominant-color extraction for app icons.
//
// See header for scope.
//
// Algorithm: hybrid edge-then-body sampling.
//
//   1. Decode the PNG via stb_image at native resolution. RGBA8.
//   2. Walk an outer ring (15% inset on each side). Accumulate
//      RGB sums for opaque pixels (alpha > 32) only.
//   3. If the ring had enough opaque samples (e.g. > 64), use its
//      averaged color: this catches icons with a designed opaque
//      background (foot, htop, mpv-style "the whole tile is the
//      icon").
//   4. Else fall back to whole-image opaque-pixel averaging: works
//      for icons that are mostly-transparent with a colored logo
//      in the middle (Firefox, Lollypop, GTK-style "transparent
//      bg + centered logo").
//   5. Darken the result by 30% so the tile reads as background
//      behind the icon, not as part of it.
//   6. Cache by path so subsequent lookups are O(1).
//
// Cache: bounded fixed-size table keyed by full path. ~50 apps
// today; cap at 128 entries with a tiny linear scan -- no eviction
// pressure in practice.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icon_color_sampler.h"
#include "third_party/log.h"

//
// stb_image is already vendored + #defined in widget_image.c with
// our memory hooks. Pull the header here too WITHOUT
// STB_IMAGE_IMPLEMENTATION so we re-use the symbols compiled
// elsewhere. Same memory hooks (the macros from widget_image apply
// only at IMPLEMENTATION include); we just need the prototypes.
//
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

//
// nanosvg headers (declarations only -- _IMPLEMENTATION is in
// widget_image.c). Lets us decode .svg icons here too so the
// edge-ring / body-average sampling has real RGBA pixels to work
// with for SVG-only apps.
//
#include "third_party/nanosvg.h"
#include "third_party/nanosvgrast.h"

//
// ===== forward decls =====================================================
//

static boole _icon_color_sampler_internal__cache_lookup(const char *path, uint *out_rgba);
static void _icon_color_sampler_internal__cache_insert(const char *path, uint rgba);

//
// ===== cache =============================================================
//

#define _ICON_COLOR_SAMPLER_INTERNAL__CACHE_MAX 128
#define _ICON_COLOR_SAMPLER_INTERNAL__PATH_LEN  256

typedef struct _icon_color_sampler_internal__entry {
    char     path[_ICON_COLOR_SAMPLER_INTERNAL__PATH_LEN];
    uint rgba;
    int      used;
} _icon_color_sampler_internal__entry;

static _icon_color_sampler_internal__entry _icon_color_sampler_internal__cache[_ICON_COLOR_SAMPLER_INTERNAL__CACHE_MAX];
static int _icon_color_sampler_internal__cache_count = 0;

//
// ===== public api ========================================================
//

boole icon_color_sampler__sample(const char *path, uint *out_rgba)
{
    if (path == NULL || path[0] == 0 || out_rgba == NULL) {
        return 0;
    }

    if (_icon_color_sampler_internal__cache_lookup(path, out_rgba)) {
        return 1;
    }

    //
    // Read the file into a heap buffer. stbi_load_from_memory needs
    // bytes + length; we use stdio to keep the dep surface small.
    //
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > (32 * 1024 * 1024)) {
        fclose(f);
        return 0;
    }
    unsigned char *bytes = (unsigned char *)malloc((size_t)fsize);
    if (bytes == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(bytes, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(bytes);
        fclose(f);
        return 0;
    }
    fclose(f);

    //
    // Detect SVG by extension. For .svg, parse + rasterize via
    // nanosvg into our own RGBA8 buffer. For everything else
    // (PNG, JPEG, etc.) hand off to stb_image. Both paths produce
    // a `px` buffer with the same byte layout so the sampling code
    // below works unchanged.
    //
    int w = 0, h = 0;
    unsigned char *px = NULL;
    boole owned_by_stb = FALSE;
    {
        size_t plen = strlen(path);
        boole is_svg = (plen >= 4
            && (path[plen - 4] == '.')
            && (path[plen - 3] == 's' || path[plen - 3] == 'S')
            && (path[plen - 2] == 'v' || path[plen - 2] == 'V')
            && (path[plen - 1] == 'g' || path[plen - 1] == 'G'));

        if (is_svg) {
            //
            // nsvgParse mutates its input. Need a +1 nul-terminated
            // copy because the file buffer isn't guaranteed to be
            // null-terminated.
            //
            char *svg_text = (char *)malloc((size_t)fsize + 1);
            if (svg_text == NULL) {
                free(bytes);
                return 0;
            }
            memcpy(svg_text, bytes, (size_t)fsize);
            svg_text[fsize] = 0;
            free(bytes);

            NSVGimage *svg = nsvgParse(svg_text, "px", 96.0f);
            free(svg_text);
            if (svg == NULL) { return 0; }

            //
            // Rasterize at a small fixed resolution -- we only need
            // pixels for color sampling, not display. 64x64 is
            // plenty: the edge ring still has ~1500 sample pixels
            // and the body has 4096 max; both well over the count
            // thresholds the sampler uses.
            //
            int rw = 64, rh = 64;
            unsigned char *buf = (unsigned char *)malloc((size_t)rw * rh * 4);
            if (buf == NULL) { nsvgDelete(svg); return 0; }

            NSVGrasterizer *rast = nsvgCreateRasterizer();
            if (rast == NULL) { free(buf); nsvgDelete(svg); return 0; }

            float sx = (svg->width  > 0.0f) ? (float)rw / svg->width  : 1.0f;
            float sy = (svg->height > 0.0f) ? (float)rh / svg->height : 1.0f;
            float s_uniform = (sx < sy) ? sx : sy;
            nsvgRasterize(rast, svg, 0.0f, 0.0f, s_uniform, buf, rw, rh, rw * 4);
            nsvgDeleteRasterizer(rast);
            nsvgDelete(svg);

            px = buf;
            w = rw;
            h = rh;
            owned_by_stb = FALSE;
        }
        else {
            int comp = 0;
            px = stbi_load_from_memory(bytes, (int)fsize, &w, &h, &comp, 4);
            free(bytes);
            owned_by_stb = TRUE;
        }
    }
    if (px == NULL || w <= 0 || h <= 0) {
        if (px != NULL) {
            if (owned_by_stb) { stbi_image_free(px); } else { free(px); }
        }
        return 0;
    }

    //
    // Step 1: edge ring. Inset 15% on each axis; outer band
    // = "everything outside the inner rectangle" gets sampled.
    //
    int inset_x = w * 15 / 100;
    int inset_y = h * 15 / 100;
    if (inset_x < 1) { inset_x = 1; }
    if (inset_y < 1) { inset_y = 1; }

    uint64 edge_sum_r = 0, edge_sum_g = 0, edge_sum_b = 0;
    int      edge_count = 0;

    for (int y = 0; y < h; y++) {
        int in_y_band = (y < inset_y) || (y >= h - inset_y);
        for (int x = 0; x < w; x++) {
            int in_x_band = (x < inset_x) || (x >= w - inset_x);
            if (!in_y_band && !in_x_band) {
                continue;
            }
            unsigned char a = px[(y * w + x) * 4 + 3];
            if (a < 32) {
                continue;
            }
            edge_sum_r += px[(y * w + x) * 4 + 0];
            edge_sum_g += px[(y * w + x) * 4 + 1];
            edge_sum_b += px[(y * w + x) * 4 + 2];
            edge_count++;
        }
    }

    int avg_r = 0, avg_g = 0, avg_b = 0;

    if (edge_count > 64) {
        //
        // Edge ring had real content -- use it. This is the
        // "designed-tile" path: the icon ships its own colored
        // background (foot's black frame, htop's dark bg).
        //
        avg_r = (int)(edge_sum_r / (uint64)edge_count);
        avg_g = (int)(edge_sum_g / (uint64)edge_count);
        avg_b = (int)(edge_sum_b / (uint64)edge_count);
    } else {
        //
        // Fall back to whole-image opaque-pixel averaging. This
        // handles icons with transparent backgrounds + a centered
        // colored logo (firefox-esr, lollypop). The body color
        // becomes the tile color.
        //
        uint64 body_sum_r = 0, body_sum_g = 0, body_sum_b = 0;
        int      body_count = 0;
        int total = w * h;
        for (int i = 0; i < total; i++) {
            unsigned char a = px[i * 4 + 3];
            if (a < 32) {
                continue;
            }
            body_sum_r += px[i * 4 + 0];
            body_sum_g += px[i * 4 + 1];
            body_sum_b += px[i * 4 + 2];
            body_count++;
        }
        if (body_count == 0) {
            if (owned_by_stb) { stbi_image_free(px); } else { free(px); }
            return 0;
        }
        avg_r = (int)(body_sum_r / (uint64)body_count);
        avg_g = (int)(body_sum_g / (uint64)body_count);
        avg_b = (int)(body_sum_b / (uint64)body_count);
    }

    if (owned_by_stb) { stbi_image_free(px); } else { free(px); }

    //
    // Darken by 30% so the tile sits visually behind the icon.
    // Without this, a bright orange Firefox logo on a bright
    // orange tile loses contrast.
    //
    avg_r = (avg_r * 70) / 100;
    avg_g = (avg_g * 70) / 100;
    avg_b = (avg_b * 70) / 100;

    if (avg_r > 255) { avg_r = 255; }
    if (avg_g > 255) { avg_g = 255; }
    if (avg_b > 255) { avg_b = 255; }

    uint rgba = ((uint)avg_r) | ((uint)avg_g << 8) | ((uint)avg_b << 16) | ((uint)0xff << 24);
    *out_rgba = rgba;

    _icon_color_sampler_internal__cache_insert(path, rgba);
    log_trace("icon_color_sampler: '%s' -> #%02x%02x%02x", path, avg_r, avg_g, avg_b);
    return 1;
}

//
// ===== cache helpers =====================================================
//

static boole _icon_color_sampler_internal__cache_lookup(const char *path, uint *out_rgba)
{
    for (int i = 0; i < _icon_color_sampler_internal__cache_count; i++) {
        if (_icon_color_sampler_internal__cache[i].used &&
            strcmp(_icon_color_sampler_internal__cache[i].path, path) == 0)
        {
            *out_rgba = _icon_color_sampler_internal__cache[i].rgba;
            return 1;
        }
    }
    return 0;
}

static void _icon_color_sampler_internal__cache_insert(const char *path, uint rgba)
{
    if (_icon_color_sampler_internal__cache_count >= _ICON_COLOR_SAMPLER_INTERNAL__CACHE_MAX) {
        //
        // Cap reached. ~128 apps is well above any realistic phone
        // launcher; just drop subsequent inserts. We still return
        // correctly via the recompute path; it's just slightly
        // slower for the 129th+ apps.
        //
        return;
    }
    int slot = _icon_color_sampler_internal__cache_count++;
    size_t plen = strlen(path);
    if (plen >= _ICON_COLOR_SAMPLER_INTERNAL__PATH_LEN) {
        plen = _ICON_COLOR_SAMPLER_INTERNAL__PATH_LEN - 1;
    }
    memcpy(_icon_color_sampler_internal__cache[slot].path, path, plen);
    _icon_color_sampler_internal__cache[slot].path[plen] = 0;
    _icon_color_sampler_internal__cache[slot].rgba = rgba;
    _icon_color_sampler_internal__cache[slot].used = 1;
}
