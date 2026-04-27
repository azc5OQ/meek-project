#ifndef FONT_H
#define FONT_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
//font.h - TTF font loading, rasterization, and text drawing.
//
//built on stb_truetype (in gui/src/third_party/). cross-platform:
//the same code runs on windows today and will run on linux when we
//port. the only platform-specific piece is where TTF bytes come from
//(a path on disk via fs__read_entire_file, or a byte buffer already
//in memory), which we abstract with two registration entry points.
//
//USAGE:
//
//  // once at startup:
//  font__init();
//  font__register_from_file("Inter", "fonts/Inter.ttf");
//  // or, when fonts are embedded into the binary at build time:
//  // font__register_from_memory("Inter", inter_bytes, inter_size);
//
//  // whenever we need to draw text:
//  gui_font* f = font__at("Inter", 14.0f);
//  font__draw(f, x, y, color, "Hello");
//
//  // at shutdown:
//  font__shutdown();
//
//font__at lazily rasterizes + atlas-packs the (family, size) pair the
//first time it's requested, then caches. the atlas gets uploaded to
//the renderer via renderer__create_atlas_r8 and released via
//renderer__destroy_atlas on shutdown.
//
//QUALITY NOTES (imgui-style):
//
//  we use stb_truetype's packer API with 3x horizontal oversampling.
//  that means each glyph is rasterized 3x wider than final size and
//  sampled with LINEAR filtering at draw time -- gives crisp subpixel
//  positioning without having to pre-render multiple versions per
//  glyph. stb_rect_pack does the atlas packing itself.
//

//
//opaque font handle returned by font__at. holds everything needed to
//measure and draw text at a specific (family, size) -- the rasterized
//atlas pixels, the per-codepoint glyph table, the renderer texture
//handle.
//
typedef struct gui_font gui_font;

//
//per-codepoint glyph info returned by font__glyph. coordinates are
//in pixels at the font's rasterization size. UVs are 0..1 into the
//atlas texture.
//
typedef struct gui_glyph
{
    float advance;   // how far to advance the pen after drawing this glyph.
    float offset_x;  // bearing left: x offset from pen to the glyph's left edge.
    float offset_y;  // bearing top: y offset from pen's baseline to the glyph's top edge (typically negative).
    float width;     // glyph quad width in pixels.
    float height;    // glyph quad height in pixels.
    float uv_x0;     // atlas UV of top-left corner.
    float uv_y0;
    float uv_x1;     // atlas UV of bottom-right corner.
    float uv_y1;
} gui_glyph;

//============================================================================
//lifecycle
//============================================================================

/**
 * Initialize the font subsystem. Must be called once after the renderer
 * is up (font atlases become GPU textures, so the renderer must exist).
 *
 * @function font__init
 * @return {boole} TRUE on success; FALSE if something went wrong.
 */
GUI_API boole font__init(void);

/**
 * Release every registered family + cached rasterization + renderer
 * atlas texture. Safe to call if font__init wasn't successful.
 *
 * @function font__shutdown
 * @return {void}
 */
GUI_API void  font__shutdown(void);

/**
 * Drop every (family, size) rasterization from the cache, including
 * its GPU atlas texture. Registered TTF families are untouched, so
 * the next font__at call re-rasterizes + re-uploads against whichever
 * renderer context is current. Used on Android to recover from
 * EGL-context loss across APP_CMD_TERM_WINDOW / APP_CMD_INIT_WINDOW;
 * must be called while the OLD context is still current (so
 * renderer__destroy_atlas can safely release the GPU handle).
 *
 * @function font__drop_gpu_cache
 * @return {void}
 */
GUI_API void  font__drop_gpu_cache(void);

//============================================================================
//family registration
//============================================================================

/**
 * Register a font family from TTF bytes already in memory. The bytes
 * are NOT copied -- caller guarantees they remain valid for the life
 * of the font subsystem.
 *
 * @function font__register_from_memory
 * @param {char*} family - The name to refer to this font by (e.g. "Inter").
 * @param {const ubyte*} ttf_bytes - Pointer to the TTF file contents.
 * @param {int64} ttf_size - Size of the TTF file in bytes.
 * @return {boole} TRUE on success.
 */
GUI_API boole font__register_from_memory(char* family, const ubyte* ttf_bytes, int64 ttf_size);

/**
 * Register a font family by reading a TTF file from disk. The bytes
 * are owned by the font subsystem and freed at shutdown.
 *
 * @function font__register_from_file
 * @param {char*} family - The name to refer to this font by.
 * @param {char*} path - UTF-8 path to the .ttf file.
 * @return {boole} TRUE if the file was read and the TTF validated.
 */
GUI_API boole font__register_from_file(char* family, char* path);

//============================================================================
//query
//============================================================================

/**
 * Get (or lazily build) a font at a specific pixel size. First call for
 * a given (family, size) rasterizes the full ASCII+Latin-1 range into
 * an atlas and uploads the texture; subsequent calls return the cache.
 *
 * @function font__at
 * @param {char*} family - Registered family name. NULL or "" = default (first registered).
 * @param {float} size_px - Requested size in pixels. <= 0 falls back to a 14 px default.
 * @return {gui_font*} Cached font handle, or NULL if no families are registered.
 */
GUI_API gui_font* font__at(char* family, float size_px);

/**
 * Get per-codepoint glyph info. Returns FALSE for codepoints outside
 * the rasterized range (currently 32..255).
 *
 * @function font__glyph
 * @param {gui_font*} f - Font handle from font__at.
 * @param {uint} codepoint - Unicode codepoint.
 * @param {gui_glyph*} out - Filled on success.
 * @return {boole} TRUE if the glyph exists in the atlas.
 */
GUI_API boole font__glyph(gui_font* f, uint codepoint, gui_glyph* out);

/**
 * Measure the pixel width of a utf-8 string at this font's size.
 * Ignores codepoints outside the rasterized range.
 *
 * @function font__measure
 * @param {gui_font*} f - Font handle.
 * @param {char*} utf8 - Null-terminated string.
 * @return {float} Total advance width in pixels.
 */
GUI_API float font__measure(gui_font* f, char* utf8);

/**
 * Measure the pixel width of the first `n` bytes of a utf-8 string.
 * Used by word-wrap logic (widget_text) to probe candidate line breaks
 * without having to null-terminate substrings. Ignores codepoints
 * outside the rasterized range.
 *
 * @function font__measure_n
 * @param {gui_font*} f - Font handle.
 * @param {char*} utf8 - String (not necessarily null-terminated).
 * @param {int64} n - Number of bytes to measure.
 * @return {float} Total advance width of those n bytes in pixels.
 */
GUI_API float font__measure_n(gui_font* f, char* utf8, int64 n);

/**
 * Line height in pixels (ascent + descent + line gap).
 *
 * @function font__line_height
 * @param {gui_font*} f - Font handle.
 * @return {float} Line height.
 */
GUI_API float font__line_height(gui_font* f);

/**
 * Ascent in pixels (baseline to top of cap height, roughly).
 *
 * @function font__ascent
 * @param {gui_font*} f - Font handle.
 * @return {float} Ascent.
 */
GUI_API float font__ascent(gui_font* f);

/**
 * Opaque atlas texture handle (void* cast from each backend's native
 * texture type). The renderer uses this with renderer__set_text_atlas.
 *
 * @function font__atlas_tex
 * @param {gui_font*} f - Font handle.
 * @return {void*} Renderer-specific texture handle (GLuint, ID3D11SRV*, IDirect3DTexture9*).
 */
GUI_API void* font__atlas_tex(gui_font* f);

//============================================================================
//drawing
//============================================================================

/**
 * Walk a utf-8 string and emit one textured quad per glyph via
 * renderer__submit_text_glyph. Also binds the font's atlas texture
 * via renderer__set_text_atlas before emitting.
 *
 * @function font__draw
 * @param {gui_font*} f - Font to draw with.
 * @param {float} x - Pen x position (left edge of first glyph, in pixels).
 * @param {float} y - Pen y position on the baseline.
 * @param {gui_color} color - Text color (multiplied against the glyph's alpha).
 * @param {char*} utf8 - Null-terminated string.
 * @return {void}
 */
GUI_API void font__draw(gui_font* f, float x, float y, gui_color color, char* utf8);

/**
 * Length-bounded variant of font__draw. Draws utf8[0..n) instead of
 * reading until a null terminator. Used by widget_text's word-wrap
 * path to draw one line-slice at a time without having to build
 * null-terminated substrings.
 *
 * @function font__draw_n
 * @param {gui_font*} f - Font to draw with.
 * @param {float} x - Pen x position.
 * @param {float} y - Pen y position (baseline).
 * @param {gui_color} color - Text color.
 * @param {char*} utf8 - String (not necessarily null-terminated).
 * @param {int64} n - Number of bytes to draw.
 * @return {void}
 */
GUI_API void font__draw_n(gui_font* f, float x, float y, gui_color color, char* utf8, int64 n);

#endif
