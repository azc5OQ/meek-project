//============================================================================
//font.c - TTF loading + rasterization + atlas + text drawing.
//============================================================================
//
//PIPELINE:
//
//  1. register a family (TTF bytes stored, not yet rasterized).
//  2. on first request for (family, size): use stb_truetype's packer
//     API to rasterize codepoints 32..255 into a 1024x1024 R8 atlas
//     bitmap with 3x horizontal oversampling.
//  3. call renderer__create_atlas_r8 to upload the bitmap and get
//     a backend-specific texture handle.
//  4. cache the whole rasterization under its (family, size) key.
//  5. each glyph's uv rect + advance + bearing stored in a per-font
//     stbtt_packedchar array; looked up by codepoint at draw time.
//
//  font__draw walks a utf-8 string, asks stb_truetype for each glyph's
//  quad coordinates, and submits one textured quad per glyph via
//  renderer__submit_text_glyph. the font's atlas texture is bound
//  once before the glyph batch via renderer__set_text_atlas.
//
//IMGUI-LEVEL QUALITY:
//
//  - 3x horizontal oversampling (stbtt_PackSetOversampling(3, 1)).
//  - stbtt_PackFontRange handles the packer + rasterizer in one call
//    (uses stb_rect_pack internally).
//  - atlas sampled with LINEAR filtering in the shader (set up by
//    each renderer backend).
//  - advances are stored as floats; pen positions stay fractional,
//    rounded only at final pixel write. this preserves subpixel
//    positioning across a line.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
//Win32 for directory enumeration (FindFirstFileW / FindNextFileW) --
//only used by the auto-scan helper, which is itself guarded by _WIN32.
//on Android the host app registers TTF bytes explicitly via
//font__register_from_memory (e.g. pulled from APK assets), so no
//directory walk is needed on that platform.
//
#if defined(_WIN32)
#include <windows.h>
#else
//
// POSIX scan (Linux DRM/X11, macOS) reads the fonts directory via
// opendir / readdir. Android skips this path entirely -- its fonts
// ship inside the APK and platform_android.c walks them through
// AAssetManager_openDir instead.
//
#if !defined(__ANDROID__)
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#endif
#endif

#include "types.h"
#include "gui.h"
#include "fs.h"
#include "renderer.h"
#include "font.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

//
//stb_truetype requires us to #define the implementation in exactly
//one translation unit. this is that one. we also pull in its malloc
//hooks, asserts, and a few basic stdlib headers it wants.
//
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

//
//rasterized-range bounds. codepoints outside this are not in the
//atlas; font__measure and font__draw silently skip them for now.
//a later iteration can page in codepoint ranges on demand.
//
#define _FONT_INTERNAL__FIRST_CODEPOINT 32
#define _FONT_INTERNAL__LAST_CODEPOINT  255
#define _FONT_INTERNAL__NUM_CODEPOINTS  (_FONT_INTERNAL__LAST_CODEPOINT - _FONT_INTERNAL__FIRST_CODEPOINT + 1)

//
//default atlas size for small fonts. 1024x1024 R8 = 1 MiB. fits a
//full Latin-1 range comfortably up to ~40 px. larger sizes scale the
//atlas dynamically via _font_internal__pick_atlas_dim below; the
//literal _ATLAS_W / _ATLAS_H constants below are kept only for the
//very-initial default. actual dimensions live on each gui_font entry
//(f->atlas_w / f->atlas_h) since they differ per (family, size).
//
//we cap atlas size at 4096x4096 -- any bigger and we're risking GPU
//texture limits on mobile and a single rasterization costs 16 MiB of
//transient CPU bitmap + 16 MiB of permanent GPU storage. a host app
//that wants to render 200 px text needs to either raise this cap or
//switch to an MSDF pipeline (deferred; see STATUS.md's font notes).
//
#define _FONT_INTERNAL__ATLAS_MIN_DIM   1024
#define _FONT_INTERNAL__ATLAS_MAX_DIM   4096

//
//pick the smallest power-of-two atlas dimension that can safely hold
//the full codepoint range at `size_px`. heuristic: each glyph needs
//roughly (size_px * oversample_x + padding) by (size_px + padding)
//pixels; 224 glyphs at that area, multiplied by 1.5 for packing
//overhead, gives the required total area; take sqrt to get the square
//side, round up to next power of two.
//
static int _font_internal__pick_atlas_dim(float size_px);

//
//size-adaptive oversample factors, matching imgui 1.92's
//ImFontAtlasBuildGetOversampleFactors:
//  - below 36 px: 2x horizontal (subpixel positioning helps)
//  - 36 px and above: 1x horizontal (atlas samples already big
//    enough that GPU LINEAR filtering does the job; the 2x pays
//    for no visible benefit and just wastes atlas area)
//  - vertical always 1x (unless a caller overrides in the future)
//  - pixel_snap_h families force 1x regardless of size so glyphs
//    sit flush on integer columns, matching what bitmap fonts need
//
//previously we used a fixed (3, 1) factor regardless of size, which
//made large-atlas rasterizations of big display text ~50 % larger
//than necessary without any measurable sharpness benefit past
//roughly 20 px. the new picker mirrors imgui's approach.
//
static void _font_internal__pick_oversample(float size_px, boole pixel_snap_h, int* out_h, int* out_v)
{
    if (pixel_snap_h || size_px > 36.0f)
    {
        *out_h = 1;
    }
    else
    {
        *out_h = 2;
    }
    *out_v = 1;
}

//
//caps for the in-process tables. each registered family takes one
//slot; each (family, size) request takes one slot in the per-font
//cache.
//
//
// Bumped from 16 to 64. Projects shipping a reasonable font
// library (Roboto family + Karla + Droid + a few decoratives)
// easily hit 16, and because the auto-scan in platform init adds
// in alphabetical order, families late in the alphabet (Roboto,
// TimesRoman, Verdana) would be SILENTLY dropped. Lookups for
// those family names then fell back to whatever was registered
// first, giving "my text renders in the wrong font" surprises.
// The visual-test harness caught one of these during a popover
// scene showing a script font instead of Roboto. Atlas memory
// per family is tiny (just the tables); 64 is safe.
//
#define _FONT_INTERNAL__MAX_FAMILIES 64
//
//cache size. each (family, integer-pixel-size) pair rasterizes into
//one entry. the scale slider drags font sizes through every integer
//from ~min_size*0.5 to ~max_size*2.5, so a typical UI with 3 style
//rules (label, body, heading) and 2 fonts can land on:
//  3 style_sizes * 2 families * ~30 scale-driven sizes = 180 entries.
//256 gives comfortable headroom; bumping further costs 96 bytes of
//table space per slot (just a pointer + two ints), so it's cheap.
//
#define _FONT_INTERNAL__MAX_FONTS    256

//
//family: a registered TTF file (or TTF bytes). rasterizations at
//various sizes hang off it lazily.
//
//the TTF bytes for a family can come from three sources:
//
//  FROM_MAPPED_FILE (default path for font__register_from_file):
//    the bytes are backed by fs__map_file -- a read-only memory-mapped
//    view of the .ttf on disk. any other process that maps the same
//    file shares the same physical pages via the kernel page cache,
//    so you pay for each font exactly once across the whole device.
//    this is the path that makes multi-process font sharing work on
//    mobile (Android + iOS) for free. `mapping` holds the handles to
//    unmap on shutdown.
//
//  FROM_HEAP (fallback when fs__map_file fails):
//    register_from_file malloc+ReadFile-copies the bytes. kept as a
//    fallback if the filesystem doesn't support mapping. source ==
//    _FONT_BYTES_HEAP => we free() the buffer on shutdown.
//
//  FROM_EXTERNAL_MEMORY (font__register_from_memory):
//    the caller hands us a pointer + size and guarantees the bytes
//    live longer than the font subsystem. we don't free, unmap, or
//    touch the backing storage. this is the embed-in-DLL mode.
//
typedef enum
{
    _FONT_BYTES_EXTERNAL = 0, // caller owns; do nothing on shutdown.
    _FONT_BYTES_HEAP     = 1, // we malloc'd; free on shutdown.
    _FONT_BYTES_MAPPED   = 2, // we mmap'd via fs__map_file; unmap on shutdown.
} _font_bytes_source;

typedef struct _font_internal__family
{
    char               name[64];
    const ubyte*       ttf_bytes;
    int64              ttf_size;
    _font_bytes_source source;   // drives cleanup behavior.
    fs_mapped_file     mapping;  // populated only when source == _FONT_BYTES_MAPPED.
    boole              active;
    //
    //pixel_snap_h: force integer x-advance and 1x horizontal
    //oversampling for this family. auto-set for bitmap-designed
    //fonts (ProggyClean / ProggyTiny / kenvector_future) whose
    //glyph outlines land on an integer pixel grid at their design
    //size; pushing them through a subpixel positioning pipeline
    //blurs the very-deliberate pixel alignment. matches imgui's
    //ImFontConfig::PixelSnapH field + auto-assignment to bitmap
    //defaults.
    //
    boole              pixel_snap_h;
} _font_internal__family;

//
//gui_font: a rasterization of one family at one size. holds the
//atlas pixels (freed after upload), the renderer texture handle,
//the packer output (one stbtt_packedchar per codepoint), and cached
//metrics.
//
struct gui_font
{
    int64  family_index;  // index into g_families. -1 = unused slot.
    float  size_px;

    void*  atlas_tex;     // renderer-specific handle (cast from GLuint / ID3D11SRV* / IDirect3DTexture9*).
    ubyte* atlas_pixels;  // kept null after upload -- renderer owns GPU copy.

    //
    //per-entry atlas dimensions. picked at rasterize time by
    //_font_internal__pick_atlas_dim based on size_px, so a small
    //font still uses a 1024^2 sheet while a 73 px one gets 4096^2.
    //stbtt_GetPackedQuad needs these to compute correct UVs, so
    //every call site that used to pass _FONT_INTERNAL__ATLAS_W/H
    //now passes f->atlas_w / f->atlas_h.
    //
    int    atlas_w;
    int    atlas_h;

    //
    //cache sentinel: TRUE when rasterization failed permanently for
    //this (family, size). font__at returns NULL for cached entries
    //in this state so callers fall back to their "no font" path
    //without us re-attempting a ~MiB allocation and a full pack run
    //every frame. without it, a scene that asks for a too-big font
    //size each frame rasterizes (and fails) once per text widget per
    //frame -- very visibly laggy once a few widgets are in play.
    //
    boole  rasterize_failed;

    stbtt_packedchar packed[_FONT_INTERNAL__NUM_CODEPOINTS]; // uv + metrics per glyph.

    //
    //vertical metrics, scaled to size_px. populated at pack time.
    //
    float ascent;
    float descent;
    float line_gap;
    float line_height; // ascent + descent + line_gap (typical line advance).

    //
    // LRU eviction tick. Bumped on every cache hit + at rasterize
    // time. When the cache hits MAX_FONTS, the entry with the
    // smallest tick gets its atlas dropped + the slot reclaimed
    // for the new (family, size). Prevents unbounded GPU memory
    // growth from prolonged scale-slider scrubbing without
    // bouncing back through font__shutdown.
    //
    uint64 lru_tick;

    //
    // Single-slot measure cache. Most widgets call font__measure
    // twice per frame on the same string: once during layout to
    // size the node, once during draw to center / align text.
    // measure_last_sig is a small content fingerprint (string
    // length + first/middle/last byte) so we don't return stale
    // widths when:
    //   - hot reload allocates a new node at the same heap address
    //     as a freed one and the content differs;
    //   - widget_input mutates text in place via on_char / on_key
    //     between layout and draw calls.
    // The fingerprint is cheap to compute (~3 byte loads + a few
    // shifts) and catches the realistic mutation patterns.
    //
    void*  measure_last_ptr;
    int64  measure_last_n;
    uint   measure_last_sig;
    float  measure_last_width;
};

//
//file-local state.
//
static _font_internal__family _font_internal__families[_FONT_INTERNAL__MAX_FAMILIES];
static int64                  _font_internal__family_count = 0;

static gui_font               _font_internal__fonts[_FONT_INTERNAL__MAX_FONTS];
static int64                  _font_internal__font_count = 0;

static boole                  _font_internal__initialized = FALSE;

//
// Monotonic counter feeding gui_font::lru_tick. Incremented on
// every cache hit and on rasterize, so older entries have lower
// values and are first to be evicted when the cache is full.
//
static uint64 _font_internal__lru_clock = 0;

//============================================================================
//forward declarations
//============================================================================

static _font_internal__family* _font_internal__find_family(char* name);
static gui_font*               _font_internal__find_cached(int64 family_index, float size_px);
static gui_font*               _font_internal__rasterize(int64 family_index, float size_px);
static void                    _font_internal__scan_and_register_dir(char* dir_path);

//============================================================================
//lifecycle
//============================================================================

boole font__init(void)
{
    if (_font_internal__initialized)
    {
        return TRUE;
    }
    memset(_font_internal__families, 0, sizeof(_font_internal__families));
    memset(_font_internal__fonts,    0, sizeof(_font_internal__fonts));
    _font_internal__family_count = 0;
    _font_internal__font_count   = 0;
    _font_internal__initialized  = TRUE;

    //
    //auto-discover and register every .ttf in the library's shipped
    //fonts directory. GUI_FONTS_SOURCE_DIR is a compile-time define
    //(absolute path, baked by gui/CMakeLists.txt) so host apps don't
    //have to know any paths -- they just reference families by name
    //in their .style files (e.g. "Roboto-Regular", "Cousine-Regular").
    //
    //the on-demand switch the user asked for: in the embedded-in-DLL
    //build mode (deferred), this call is replaced by iterating a
    //compiled-in byte-array table and calling font__register_from_memory
    //for each entry. the public API is identical either way.
    //
#ifdef GUI_FONTS_SOURCE_DIR
    _font_internal__scan_and_register_dir(GUI_FONTS_SOURCE_DIR);
#endif

    return TRUE;
}

void font__shutdown(void)
{
    //
    //release each rasterized font's GPU atlas and (if still present)
    //its cpu bitmap.
    //
    for (int64 i = 0; i < _font_internal__font_count; i++)
    {
        gui_font* f = &_font_internal__fonts[i];
        if (f->atlas_tex != NULL)
        {
            renderer__destroy_atlas(f->atlas_tex);
            f->atlas_tex = NULL;
        }
        if (f->atlas_pixels != NULL)
        {
            GUI_FREE(f->atlas_pixels);
            f->atlas_pixels = NULL;
        }
    }

    //
    //release family TTF buffers we own, based on where the bytes came
    //from. mapped files go back through fs__unmap_file so the kernel
    //can drop the shared-page refcount; heap-owned bytes get free()'d;
    //external bytes (embed-in-DLL) we leave alone.
    //
    for (int64 i = 0; i < _font_internal__family_count; i++)
    {
        _font_internal__family* fam = &_font_internal__families[i];
        switch (fam->source)
        {
            case _FONT_BYTES_MAPPED:
            {
                fs__unmap_file(&fam->mapping);
                break;
            }
            case _FONT_BYTES_HEAP:
            {
                if (fam->ttf_bytes != NULL)
                {
                    GUI_FREE((void*)fam->ttf_bytes);
                }
                break;
            }
            case _FONT_BYTES_EXTERNAL:
            default:
            {
                // caller-owned; do nothing.
                break;
            }
        }
    }

    memset(_font_internal__families, 0, sizeof(_font_internal__families));
    memset(_font_internal__fonts,    0, sizeof(_font_internal__fonts));
    _font_internal__family_count = 0;
    _font_internal__font_count   = 0;
    _font_internal__initialized  = FALSE;
}

void font__drop_gpu_cache(void)
{
    //
    //GL-context-loss recovery. On Android, APP_CMD_TERM_WINDOW
    //destroys the EGL context and every GPU handle with it. The
    //registered font FAMILIES (TTF bytes) are pure CPU state and
    //survive -- but every cached (family, size) rasterization holds
    //an atlas_tex GPU handle that's about to become garbage. Drop
    //them here so the next font__at call after INIT_WINDOW builds
    //a fresh entry against the new context. Families are untouched.
    //
    //Must be called while the OLD context is still current so
    //renderer__destroy_atlas's glDeleteTextures (or D3D Release) is
    //valid. platform_android__on_app_cmd orders it before
    //renderer__shutdown for that reason.
    //
    for (int64 i = 0; i < _font_internal__font_count; i++)
    {
        gui_font* f = &_font_internal__fonts[i];
        if (f->atlas_tex != NULL)
        {
            renderer__destroy_atlas(f->atlas_tex);
            f->atlas_tex = NULL;
        }
        if (f->atlas_pixels != NULL)
        {
            //
            //Shouldn't happen in steady state (atlas_pixels is freed
            //right after upload) but covers the in-flight rasterize
            //case where the context died mid-pack.
            //
            GUI_FREE(f->atlas_pixels);
            f->atlas_pixels = NULL;
        }
    }
    memset(_font_internal__fonts, 0, sizeof(_font_internal__fonts));
    _font_internal__font_count = 0;
}

//============================================================================
//family registration
//============================================================================

boole font__register_from_memory(char* family, const ubyte* ttf_bytes, int64 ttf_size)
{
    if (!_font_internal__initialized || family == NULL || ttf_bytes == NULL || ttf_size <= 0)
    {
        return FALSE;
    }

    //
    //sanity-check the TTF header via stb_truetype. fails early on
    //bogus bytes before they can cause weirdness later.
    //
    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(ttf_bytes, 0);
    if (offset < 0 || !stbtt_InitFont(&info, ttf_bytes, offset))
    {
        log_error("stbtt_InitFont failed for family '%s'", family);
        return FALSE;
    }

    //
    //already registered? release the old backing storage first (mapped
    //or heap; external bytes are caller-owned), then reuse the slot.
    //
    _font_internal__family* fam = _font_internal__find_family(family);
    if (fam == NULL)
    {
        if (_font_internal__family_count >= _FONT_INTERNAL__MAX_FAMILIES)
        {
            log_error("family table full (max %d)", _FONT_INTERNAL__MAX_FAMILIES);
            return FALSE;
        }
        fam = &_font_internal__families[_font_internal__family_count++];
    }
    else
    {
        if (fam->source == _FONT_BYTES_MAPPED)
        {
            fs__unmap_file(&fam->mapping);
        }
        else if (fam->source == _FONT_BYTES_HEAP && fam->ttf_bytes != NULL)
        {
            GUI_FREE((void*)fam->ttf_bytes);
        }
    }

    size_t n = strlen(family);
    if (n >= sizeof(fam->name))
    {
        n = sizeof(fam->name) - 1;
    }
    memcpy(fam->name, family, n);
    fam->name[n]   = 0;
    fam->ttf_bytes = ttf_bytes;
    fam->ttf_size  = ttf_size;
    fam->source    = _FONT_BYTES_EXTERNAL;
    memset(&fam->mapping, 0, sizeof(fam->mapping));
    fam->active    = TRUE;

    //
    //auto-detect bitmap-designed fonts and turn on pixel_snap_h.
    //ProggyClean / ProggyTiny / kenvector_future / kenvector_future_thin
    //are the bitmap-grid faces bundled with the toolkit; at their
    //native sizes every stem lands on an integer pixel column, so
    //we want no subpixel positioning (oversample=1) and we want each
    //glyph's advance rounded to integer so consecutive chars stay
    //column-aligned. heuristic match beats plumbing a flag through
    //every registration call site; host apps with their own bitmap
    //faces can extend the check here.
    //
    fam->pixel_snap_h = FALSE;
    {
        const char* nm = fam->name;
        if (strncmp(nm, "Proggy", 6) == 0 || strstr(nm, "kenvector") != NULL)
        {
            fam->pixel_snap_h = TRUE;
        }
    }

    return TRUE;
}

boole font__register_from_file(char* family, char* path)
{
    if (!_font_internal__initialized || family == NULL || path == NULL)
    {
        return FALSE;
    }

    //
    //try the memory-mapped path first. fs__map_file produces a
    //read-only view that every process mapping the same file shares
    //via the kernel page cache -- a phone with N app instances pays
    //for the fonts exactly once regardless of N. see fs.h for the
    //cross-platform story.
    //
    //fall back to fs__read_entire_file (heap copy) only if mapping
    //fails, which on Windows typically means the file is on a weird
    //filesystem that doesn't support mappings.
    //
    fs_mapped_file mapping;
    if (fs__map_file(path, &mapping))
    {
        //
        //plug the mapped view into register_from_memory (it treats the
        //bytes as external). then upgrade the family record so we know
        //to unmap on shutdown instead of free().
        //
        if (!font__register_from_memory(family, mapping.data, mapping.size))
        {
            fs__unmap_file(&mapping);
            return FALSE;
        }
        _font_internal__family* fam = _font_internal__find_family(family);
        if (fam != NULL)
        {
            fam->source  = _FONT_BYTES_MAPPED;
            fam->mapping = mapping;
        }
        return TRUE;
    }

    //
    //mapping failed; try the heap-copy fallback. if that also fails,
    //we bail.
    //
    int64 size = 0;
    char* buf  = fs__read_entire_file(path, &size);
    if (buf == NULL)
    {
        log_error("failed to read '%s'", path);
        return FALSE;
    }

    if (!font__register_from_memory(family, (const ubyte*)buf, size))
    {
        GUI_FREE(buf);
        return FALSE;
    }

    //
    //mark the family as heap-owned so shutdown frees the buffer.
    //
    _font_internal__family* fam = _font_internal__find_family(family);
    if (fam != NULL)
    {
        fam->source = _FONT_BYTES_HEAP;
    }
    return TRUE;
}

//============================================================================
//query
//============================================================================

gui_font* font__at(char* family, float size_px)
{
    if (!_font_internal__initialized || _font_internal__family_count == 0)
    {
        return NULL;
    }
    if (size_px <= 0.0f)
    {
        size_px = 14.0f;
    }

    //
    //snap to an integer pixel size before caching.
    //
    //why: callers routinely pass (style_size * scene_scale), which from
    //the demo's scale slider lands on endless fractional values (14.02,
    //14.07, 14.13, ...). without snapping, every tiny slider wiggle
    //rasterizes a brand-new atlas and the cache fills in seconds.
    //humans can't tell 14 from 14.3 px of text anyway, so integer snap
    //costs nothing visually and bounds cache growth to ~one entry per
    //(family, integer-pixel-size) pair.
    //
    //clamp to [1, 256] as a safety rail -- stbtt's packer can't render
    //glyphs below ~6 px usefully, and anything over ~128 would overflow
    //the atlas. host apps that need extreme sizes can revisit.
    //
    int rounded = (int)(size_px + 0.5f);
    if (rounded < 1)   rounded = 1;
    if (rounded > 256) rounded = 256;
    size_px = (float)rounded;

    int64 family_index = -1;
    if (family != NULL && family[0] != 0)
    {
        for (int64 i = 0; i < _font_internal__family_count; i++)
        {
            if (strcmp(_font_internal__families[i].name, family) == 0)
            {
                family_index = i;
                break;
            }
        }
    }
    if (family_index < 0)
    {
        //
        //fallback to the first registered family. matches the "no font
        //family specified" or "requested family not found" cases.
        //
        family_index = 0;
    }

    gui_font* cached = _font_internal__find_cached(family_index, size_px);
    if (cached != NULL)
    {
        //
        //cached failure -- return NULL so callers fall back to their
        //"no font" path instead of our rasterize attempting a huge
        //allocation + PackFontRange call every frame.
        //
        if (cached->rasterize_failed)
        {
            return NULL;
        }
        return cached;
    }
    return _font_internal__rasterize(family_index, size_px);
}

boole font__glyph(gui_font* f, uint codepoint, gui_glyph* out)
{
    if (f == NULL || out == NULL)
    {
        return FALSE;
    }
    if (codepoint < _FONT_INTERNAL__FIRST_CODEPOINT || codepoint > _FONT_INTERNAL__LAST_CODEPOINT)
    {
        return FALSE;
    }

    //
    //stbtt_GetPackedQuad computes the quad (screen rect + uv rect)
    //given a pen position. we ask at (0, 0) and pick off the raw
    //metrics; caller positions with its own pen.
    //
    stbtt_aligned_quad q;
    float x = 0.0f;
    float y = 0.0f;
    int   index = (int)codepoint - _FONT_INTERNAL__FIRST_CODEPOINT;
    stbtt_GetPackedQuad(f->packed, f->atlas_w, f->atlas_h, index, &x, &y, &q, 1);

    out->advance  = f->packed[index].xadvance;
    out->offset_x = q.x0;            // bearing left relative to the pen origin we passed (0,0).
    out->offset_y = q.y0;            // bearing top (negative for most glyphs, going "up" from baseline).
    out->width    = q.x1 - q.x0;
    out->height   = q.y1 - q.y0;
    out->uv_x0    = q.s0;
    out->uv_y0    = q.t0;
    out->uv_x1    = q.s1;
    out->uv_y1    = q.t1;
    return TRUE;
}

//
// Cheap content fingerprint: byte length + a few sampled bytes.
// Used by the measure cache to detect when the string at a given
// pointer has changed in place (widget_input typing) or is a
// different string at a recycled heap address (post-hot-reload).
// Not collision-proof but the payoff for a 4-load fingerprint is
// catching the only realistic mutation patterns.
//
static uint _font_internal__measure_sig(const char* utf8, int64 len_or_neg1)
{
    int64 n = len_or_neg1;
    if (n < 0)
    {
        const char* p = utf8;
        while (*p != 0) { p++; }
        n = (int64)(p - utf8);
    }
    uint sig = (uint)n * 2654435761u;
    if (n > 0) { sig ^= (uint)(unsigned char)utf8[0]; }
    if (n > 1) { sig ^= (uint)(unsigned char)utf8[n - 1] << 8; }
    if (n > 2) { sig ^= (uint)(unsigned char)utf8[n / 2] << 16; }
    return sig;
}

float font__measure(gui_font* f, char* utf8)
{
    if (f == NULL || utf8 == NULL)
    {
        return 0.0f;
    }
    //
    // Single-slot cache: most widgets call font__measure twice per
    // frame on the same string (layout + draw). The fingerprint
    // catches the realistic mutation cases (in-place text edits,
    // hot-reload heap recycling). n=-1 sentinel distinguishes the
    // null-terminated cache slot from the length-prefixed one used
    // by font__measure_n.
    //
    uint sig = _font_internal__measure_sig(utf8, -1);
    if (f->measure_last_ptr == utf8 && f->measure_last_n == -1 &&
        f->measure_last_sig == sig)
    {
        return f->measure_last_width;
    }
    float x = 0.0f;
    for (const ubyte* p = (const ubyte*)utf8; *p != 0; p++)
    {
        if (*p < _FONT_INTERNAL__FIRST_CODEPOINT || *p > _FONT_INTERNAL__LAST_CODEPOINT)
        {
            continue;
        }
        int index = (int)(*p) - _FONT_INTERNAL__FIRST_CODEPOINT;
        x += f->packed[index].xadvance;
    }
    f->measure_last_ptr   = utf8;
    f->measure_last_n     = -1;
    f->measure_last_sig   = sig;
    f->measure_last_width = x;
    return x;
}

//
//fixed-length variant for word-wrap probing: measures utf8[0..n) in
//pixels without needing a trailing null byte. widget_text.c calls
//this with every candidate "word fits on this line?" test, which
//avoids mutating the source string just to terminate a substring.
//
float font__measure_n(gui_font* f, char* utf8, int64 n)
{
    if (f == NULL || utf8 == NULL || n <= 0)
    {
        return 0.0f;
    }
    uint sig = _font_internal__measure_sig(utf8, n);
    if (f->measure_last_ptr == utf8 && f->measure_last_n == n &&
        f->measure_last_sig == sig)
    {
        return f->measure_last_width;
    }
    float x = 0.0f;
    const ubyte* p   = (const ubyte*)utf8;
    const ubyte* end = p + n;
    for (; p < end; p++)
    {
        if (*p < _FONT_INTERNAL__FIRST_CODEPOINT || *p > _FONT_INTERNAL__LAST_CODEPOINT)
        {
            continue;
        }
        int index = (int)(*p) - _FONT_INTERNAL__FIRST_CODEPOINT;
        x += f->packed[index].xadvance;
    }
    f->measure_last_ptr   = utf8;
    f->measure_last_n     = n;
    f->measure_last_sig   = sig;
    f->measure_last_width = x;
    return x;
}

float font__line_height(gui_font* f)
{
    return f != NULL ? f->line_height : 0.0f;
}

float font__ascent(gui_font* f)
{
    return f != NULL ? f->ascent : 0.0f;
}

void* font__atlas_tex(gui_font* f)
{
    return f != NULL ? f->atlas_tex : NULL;
}

//============================================================================
//drawing
//============================================================================

void font__draw(gui_font* f, float x, float y, gui_color color, char* utf8)
{
    if (utf8 == NULL)
    {
        return;
    }
    //
    //delegate to the length-bounded variant so the two functions share
    //one glyph-emit implementation. measuring strlen here costs a loop
    //we'd be doing implicitly in font__draw_n anyway.
    //
    int64 n = 0;
    while (utf8[n] != 0) { n++; }
    font__draw_n(f, x, y, color, utf8, n);
}

void font__draw_n(gui_font* f, float x, float y, gui_color color, char* utf8, int64 n)
{
    if (f == NULL || utf8 == NULL || n <= 0)
    {
        return;
    }

    //
    //bind the atlas so subsequent submit_text_glyph calls sample from
    //it. different fonts have different textures; the renderer's text
    //batch only handles one atlas at a time, so if the caller mixes
    //fonts within a frame they need to call font__draw per font run.
    //
    renderer__set_text_atlas(f->atlas_tex);

    //
    //pixel-truncate the text origin before emitting glyphs. callers
    //pass bounds.x + bearing + ascent and such, which are routinely
    //fractional once the scene's scale slider is at something other
    //than 1.0 -- and every glyph's stbtt_GetPackedQuad align_to_integer
    //step anchors relative to this origin, so a fractional pen_y
    //slides horizontal stems across two adjacent rows of pixels,
    //producing the classic "baseline drift softness" even though
    //each individual glyph is pixel-aligned. matches imgui's
    //ImFont::RenderText prologue:
    //    // Align to be pixel perfect
    //    float x = IM_TRUNC(pos.x);
    //    float y = IM_TRUNC(pos.y);
    //truncating (floor toward zero) instead of rounding matches
    //imgui's IM_TRUNC so the behavior is the same.
    //
    float pen_x = (float)(int)x;
    float pen_y = (float)(int)y;

    const ubyte* p   = (const ubyte*)utf8;
    const ubyte* end = p + n;
    for (; p < end; p++)
    {
        uint cp = *p;
        if (cp < _FONT_INTERNAL__FIRST_CODEPOINT || cp > _FONT_INTERNAL__LAST_CODEPOINT)
        {
            continue;
        }

        //
        //stbtt_GetPackedQuad bakes the oversampling adjustment into the
        //returned quad and advances the pen position by xadvance. this
        //is the "right" way to consume PackedChars -- doing it manually
        //is error-prone (easy to be off by half a pixel).
        //
        stbtt_aligned_quad q;
        int index = (int)cp - _FONT_INTERNAL__FIRST_CODEPOINT;
        stbtt_GetPackedQuad(f->packed, f->atlas_w, f->atlas_h, index, &pen_x, &pen_y, &q, 1);

        gui_rect rect;
        rect.x = q.x0;
        rect.y = q.y0;
        rect.w = q.x1 - q.x0;
        rect.h = q.y1 - q.y0;

        gui_rect uv;
        uv.x = q.s0;
        uv.y = q.t0;
        uv.w = q.s1 - q.s0;
        uv.h = q.t1 - q.t0;

        renderer__submit_text_glyph(rect, uv, color);
    }
}

//============================================================================
//internal: cache lookup
//============================================================================

static _font_internal__family* _font_internal__find_family(char* name)
{
    for (int64 i = 0; i < _font_internal__family_count; i++)
    {
        if (_font_internal__families[i].active && strcmp(_font_internal__families[i].name, name) == 0)
        {
            return &_font_internal__families[i];
        }
    }
    return NULL;
}

static gui_font* _font_internal__find_cached(int64 family_index, float size_px)
{
    for (int64 i = 0; i < _font_internal__font_count; i++)
    {
        gui_font* f = &_font_internal__fonts[i];
        if (f->family_index == family_index && f->size_px == size_px)
        {
            //
            // Touch the LRU clock so this entry sticks around. Cheap
            // counter increment, called once per font__at hit.
            //
            f->lru_tick = ++_font_internal__lru_clock;
            return f;
        }
    }
    return NULL;
}

//
// Evict the least-recently-used cache entry to make room for a new
// rasterization when the cache is full. Returns the freed slot
// index (which the caller will overwrite). Releases the GPU atlas
// + any held atlas pixels first so the GPU memory comes back.
//
static int64 _font_internal__evict_lru(void)
{
    int64  oldest_idx  = 0;
    uint64 oldest_tick = _font_internal__fonts[0].lru_tick;
    for (int64 i = 1; i < _font_internal__font_count; i++)
    {
        if (_font_internal__fonts[i].lru_tick < oldest_tick)
        {
            oldest_tick = _font_internal__fonts[i].lru_tick;
            oldest_idx  = i;
        }
    }
    gui_font* victim = &_font_internal__fonts[oldest_idx];
    if (victim->atlas_tex != NULL)
    {
        renderer__destroy_atlas(victim->atlas_tex);
        victim->atlas_tex = NULL;
    }
    if (victim->atlas_pixels != NULL)
    {
        GUI_FREE(victim->atlas_pixels);
        victim->atlas_pixels = NULL;
    }
    log_info("font cache: evicted LRU slot %lld (family=%lld size=%.1f)",
             (long long)oldest_idx, (long long)victim->family_index, (double)victim->size_px);
    memset(victim, 0, sizeof(*victim));
    victim->family_index = -1;
    return oldest_idx;
}

//============================================================================
//internal: rasterize + atlas build
//============================================================================
//
//called lazily the first time we see a (family, size) combo. picks an
//atlas size that's large enough to hold the full codepoint range at
//this `size_px`, runs stb_truetype's packer to rasterize every glyph
//into it, uploads the result to the renderer, frees the cpu buffer,
//caches the font. if rasterization fails (typically because we've
//maxed out at 4096^2 and the font is still too big), the cache entry
//is still written but with rasterize_failed=TRUE -- subsequent
//font__at calls for that size return NULL from the cached-failure
//branch without attempting another allocation.
//

static int _font_internal__pick_atlas_dim(float size_px)
{
    //
    //rough area estimate: each glyph occupies (size_px * oversample_x
    //+ padding) by (size_px + padding) pixels, times 224 glyphs,
    //times 1.5 for rect-pack overhead. take sqrt for a square side,
    //round UP to the next power of two so we land on sampler-friendly
    //dimensions.
    //
    //we ask _pick_oversample for the factor with pixel_snap_h=FALSE
    //(the non-bitmap path) so the budget is sized for the worst-case
    //the rasterizer might actually use at this size. a bitmap family
    //at the same size will pick a smaller oversample and just leave
    //some atlas unused -- which is fine, beats under-budgeting and
    //failing to pack.
    //
    int ovs_h;
    int ovs_v;
    _font_internal__pick_oversample(size_px, FALSE, &ovs_h, &ovs_v);

    float w_per = size_px * (float)ovs_h + 2.0f;
    float h_per = size_px * (float)ovs_v + 2.0f;
    float area  = (float)_FONT_INTERNAL__NUM_CODEPOINTS * w_per * h_per * 1.5f;
    float side  = 1.0f;
    while (side * side < area)
    {
        side *= 2.0f;
    }

    int dim = (int)side;
    if (dim < _FONT_INTERNAL__ATLAS_MIN_DIM)
    {
        dim = _FONT_INTERNAL__ATLAS_MIN_DIM;
    }
    if (dim > _FONT_INTERNAL__ATLAS_MAX_DIM)
    {
        dim = _FONT_INTERNAL__ATLAS_MAX_DIM;
    }
    return dim;
}

static gui_font* _font_internal__rasterize(int64 family_index, float size_px)
{
    _font_internal__family* fam = &_font_internal__families[family_index];
    if (!fam->active || fam->ttf_bytes == NULL)
    {
        return NULL;
    }

    //
    // Pick a slot. If there's room at the end, append; otherwise
    // evict the least-recently-used entry and reuse its slot. The
    // `slot_is_reuse` flag suppresses the unconditional font_count++
    // at the end of rasterize when we're recycling an existing
    // slot -- previously a decrement-here / increment-there dance
    // hid valid entries from find_cached during the gap.
    //
    int64 slot;
    boole slot_is_reuse;
    if (_font_internal__font_count < _FONT_INTERNAL__MAX_FONTS)
    {
        slot          = _font_internal__font_count;
        slot_is_reuse = FALSE;
    }
    else
    {
        slot          = _font_internal__evict_lru();
        slot_is_reuse = TRUE;
    }
    gui_font* f = &_font_internal__fonts[slot];
    memset(f, 0, sizeof(*f));
    f->family_index = family_index;
    f->size_px      = size_px;
    f->lru_tick     = ++_font_internal__lru_clock;

    //
    //pick an atlas size that should comfortably fit the full glyph
    //range for this font size. scaled dynamically rather than a fixed
    //constant so a 14 px body font keeps using 1024x1024 while a 73 px
    //heading pulls up to 4096x4096 transparently. stbtt_GetPackedQuad
    //reads these back via f->atlas_w / f->atlas_h when drawing.
    //
    f->atlas_w = _font_internal__pick_atlas_dim(size_px);
    f->atlas_h = f->atlas_w;

    //
    //record the failure path once up front: if anything below fails,
    //we jump to `fail_cache`, mark the entry rasterize_failed, and
    //increment font_count so subsequent font__at calls see the cached
    //failure and bail fast instead of re-rasterizing.
    //
    #define _FONT_INTERNAL__RASTER_FAIL_CACHE() \
        do { \
            if (f->atlas_pixels != NULL) \
            { \
                GUI_FREE(f->atlas_pixels); \
                f->atlas_pixels = NULL; \
            } \
            f->rasterize_failed = TRUE; \
            if (!slot_is_reuse) { _font_internal__font_count++; } \
            return NULL; \
        } while (0)

    //
    //allocate the atlas bitmap. one byte per pixel (R8 alpha-only).
    //at 4096x4096 that's 16 MiB of transient CPU memory -- freed
    //right after upload to the renderer.
    //
    int atlas_bytes = f->atlas_w * f->atlas_h;
    f->atlas_pixels = (ubyte*)GUI_CALLOC_T((size_t)atlas_bytes, 1, MM_TYPE_FONT);
    if (f->atlas_pixels == NULL)
    {
        log_error("out of memory for atlas (%d x %d)", f->atlas_w, f->atlas_h);
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    //
    //stbtt packer handles rect-packing + rasterization in one pass.
    //padding = 1 keeps adjacent glyphs from bleeding into each other
    //under bilinear sampling.
    //
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, f->atlas_pixels, f->atlas_w, f->atlas_h, 0, 1, NULL))
    {
        log_error("stbtt_PackBegin failed for size %.1f (atlas %d x %d)", size_px, f->atlas_w, f->atlas_h);
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    //
    //size-adaptive oversample. imgui's formula: 2x horizontal below
    //36 px, 1x at or above; 1x for pixel_snap_h families regardless
    //of size. reason big sizes get 1x: at >36 px the atlas samples
    //are already large enough that the GPU's LINEAR filter resolves
    //subpixel positioning on its own, so the extra horizontal
    //resolution just wastes atlas area. see
    //_font_internal__pick_oversample comment for the full story.
    //
    int ovs_h;
    int ovs_v;
    _font_internal__pick_oversample(size_px, fam->pixel_snap_h, &ovs_h, &ovs_v);
    stbtt_PackSetOversampling(&pc, (unsigned int)ovs_h, (unsigned int)ovs_v);

    //
    //pack the full codepoint range. stbtt_PackFontRange fills in the
    //stbtt_packedchar array: one entry per codepoint with its uv rect,
    //bearing, and advance (all at our requested pixel size).
    //
    int ok = stbtt_PackFontRange(
        &pc,
        fam->ttf_bytes, 0,
        size_px,
        _FONT_INTERNAL__FIRST_CODEPOINT, _FONT_INTERNAL__NUM_CODEPOINTS,
        f->packed
    );
    stbtt_PackEnd(&pc);

    if (!ok)
    {
        //
        //atlas still couldn't hold the range (we already capped at
        //4096x4096 via pick_atlas_dim). cache the failure so we don't
        //retry every frame -- a noisy text widget would otherwise
        //allocate + log + free 16 MiB per frame.
        //
        log_error("stbtt_PackFontRange failed for size %.1f at %d x %d; caching as failed", size_px, f->atlas_w, f->atlas_h);
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    //
    //pixel_snap_h families: round every glyph's x-advance to the
    //nearest integer so consecutive glyphs always land on integer
    //pixel columns. without this, a string of 13 px ProggyClean
    //glyphs drifts half-pixel by half-pixel across a line and the
    //pristine bitmap design starts to smear. matches imgui's
    //`if (src->PixelSnapH) advance_x = IM_ROUND(advance_x)` step.
    //
    if (fam->pixel_snap_h)
    {
        for (int gi = 0; gi < _FONT_INTERNAL__NUM_CODEPOINTS; gi++)
        {
            float a = f->packed[gi].xadvance;
            f->packed[gi].xadvance = (float)((int)(a + 0.5f));
        }
    }

    //
    //grab vertical metrics directly from the TTF so line spacing
    //matches what the font designer intended.
    //
    stbtt_fontinfo info;
    stbtt_InitFont(&info, fam->ttf_bytes, stbtt_GetFontOffsetForIndex(fam->ttf_bytes, 0));
    float scale = stbtt_ScaleForPixelHeight(&info, size_px);
    int   ascent_i;
    int   descent_i;
    int   line_gap_i;
    stbtt_GetFontVMetrics(&info, &ascent_i, &descent_i, &line_gap_i);
    f->ascent      = (float)ascent_i   * scale;
    f->descent     = (float)descent_i  * scale; // typically negative.
    f->line_gap    = (float)line_gap_i * scale;
    f->line_height = f->ascent - f->descent + f->line_gap;

    //
    //upload the bitmap to the renderer. we ask for R8 (single channel,
    //which the fragment shader interprets as alpha). the renderer
    //returns an opaque handle that we store + free at shutdown.
    //
    f->atlas_tex = renderer__create_atlas_r8(f->atlas_pixels, f->atlas_w, f->atlas_h);

    //
    //done with the cpu-side bitmap. the GPU has its own copy now.
    //
    GUI_FREE(f->atlas_pixels);
    f->atlas_pixels = NULL;

    if (f->atlas_tex == NULL)
    {
        log_error("renderer__create_atlas_r8 returned NULL (size %.1f, atlas %d x %d)", size_px, f->atlas_w, f->atlas_h);
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    if (!slot_is_reuse) { _font_internal__font_count++; }
    return f;
    #undef _FONT_INTERNAL__RASTER_FAIL_CACHE
}

//============================================================================
//internal: directory scan + auto-register
//============================================================================
//
//enumerate *.ttf in `dir_path` and call font__register_from_file for each,
//using the file's basename (without the .ttf extension) as the family
//name. so "gui/src/fonts/Roboto-Regular.ttf" becomes family "Roboto-Regular",
//referenced from .style as `font_family: Roboto-Regular;`.
//
//called from font__init with GUI_FONTS_SOURCE_DIR. missing directory is
//not fatal -- we log and return. failing to register an individual file
//is also not fatal; other fonts still load.
//
//uses Win32 FindFirstFileW / FindNextFileW. needs UTF-16 paths, which we
//build by concatenating dir_path + "\\*.ttf" in UTF-8 and converting.
//

static void _font_internal__scan_and_register_dir(char* dir_path)
{
#if !defined(_WIN32)
    //
    // Non-Windows scan. Android routes font loading through its
    // own AAssetManager path inside platform_android.c (APK assets
    // aren't on a POSIX filesystem), so we skip the opendir walk
    // there. Everywhere else -- Linux DRM/X11, macOS Cocoa, etc. --
    // we scan the directory for *.ttf files the same way the Win32
    // branch does, just via opendir/readdir instead of FindFirstFile.
    //
  #if defined(__ANDROID__)
    (void)dir_path;
    return;
  #else
    if (dir_path == NULL || dir_path[0] == 0) { return; }
    DIR* d = opendir(dir_path);
    if (d == NULL)
    {
        log_warn("fonts: scan '%s' failed (opendir: %s)", dir_path, strerror(errno));
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        //
        // Suffix check: must end in ".ttf" (case-insensitive). Skip
        // "." / ".." and anything that isn't a regular file (symlinks
        // to a real .ttf still resolve through the subsequent open).
        //
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5) { continue; }
        const char* suf = ent->d_name + nlen - 4;
        if (!(suf[0] == '.' &&
              (suf[1] == 't' || suf[1] == 'T') &&
              (suf[2] == 't' || suf[2] == 'T') &&
              (suf[3] == 'f' || suf[3] == 'F')))
        {
            continue;
        }
        //
        // Family name = filename stem; build absolute path from
        // dir_path + '/' + filename for font__register_from_file.
        //
        char family[64];
        size_t stem_len = nlen - 4;
        if (stem_len >= sizeof(family)) { stem_len = sizeof(family) - 1; }
        memcpy(family, ent->d_name, stem_len);
        family[stem_len] = 0;

        char path[1024];
        size_t dlen = strlen(dir_path);
        if (dlen + 1 + nlen + 1 > sizeof(path)) { continue; }
        memcpy(path, dir_path, dlen);
        path[dlen] = '/';
        memcpy(path + dlen + 1, ent->d_name, nlen + 1);

        if (!font__register_from_file(family, path))
        {
            log_warn("fonts: failed to register '%s'", path);
        }
    }
    closedir(d);
  #endif
    return;
#else
    if (dir_path == NULL || dir_path[0] == 0)
    {
        return;
    }

    //
    //build the search pattern: "<dir_path>\*.ttf". 512 chars is enough
    //for any reasonable absolute path on windows (MAX_PATH = 260).
    //
    char pattern[1024];
    size_t n = strlen(dir_path);
    if (n + 8 >= sizeof(pattern))
    {
        log_error("fonts dir path too long: '%s'", dir_path);
        return;
    }
    memcpy(pattern, dir_path, n);
    //
    //append "\*.ttf". forward or backslashes both work on windows, but
    //we use backslash for consistency with the other Win32 code in this
    //project.
    //
    pattern[n++] = '\\';
    pattern[n++] = '*';
    pattern[n++] = '.';
    pattern[n++] = 't';
    pattern[n++] = 't';
    pattern[n++] = 'f';
    pattern[n]   = 0;

    //
    //convert to UTF-16 for FindFirstFileW.
    //
    WCHAR wpattern[1024];
    int wn = MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, (int)(sizeof(wpattern) / sizeof(wpattern[0])));
    if (wn == 0)
    {
        log_error("MultiByteToWideChar failed for pattern '%s'", pattern);
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        //
        //directory empty / missing. that's allowed -- the host app can
        //still register fonts manually via font__register_from_file.
        //
        return;
    }

    do
    {
        //
        //skip subdirectories; we only want .ttf files at this level.
        //
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        //
        //convert WIN32_FIND_DATAW::cFileName (UTF-16) to UTF-8 filename
        //and absolute path.
        //
        char filename_utf8[512];
        int  fn = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, filename_utf8, (int)sizeof(filename_utf8), NULL, NULL);
        if (fn == 0)
        {
            continue;
        }

        //
        //strip the ".ttf" extension to derive the family name.
        //
        char family[128];
        size_t fnl = strlen(filename_utf8);
        if (fnl < 5)
        {
            continue; // shorter than "x.ttf" -- shouldn't happen but skip defensively.
        }
        size_t stem_len = fnl - 4; // drop ".ttf".
        if (stem_len >= sizeof(family))
        {
            stem_len = sizeof(family) - 1;
        }
        memcpy(family, filename_utf8, stem_len);
        family[stem_len] = 0;

        //
        //build the full path: "<dir_path>\<filename>".
        //
        char full_path[1024];
        size_t dl = strlen(dir_path);
        if (dl + 1 + fnl >= sizeof(full_path))
        {
            log_error("full path too long for '%s'", filename_utf8);
            continue;
        }
        memcpy(full_path, dir_path, dl);
        full_path[dl] = '\\';
        memcpy(full_path + dl + 1, filename_utf8, fnl + 1); // include NUL.

        if (!font__register_from_file(family, full_path))
        {
            log_error("auto-register failed for '%s'", full_path);
            continue;
        }
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);
#endif   // _WIN32
}
