#ifndef RENDERER_H
#define RENDERER_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//============================================================================
//renderer.h - abstract renderer backend interface.
//============================================================================
//
//the toolkit targets multiple GPU APIs via interchangeable backends, each
//implementing this contract:
//
//   renderers/opengl3_renderer.c   - OpenGL 3.3 core (working default).
//   renderers/d3d11_renderer.c     - Direct3D 11 (scaffold).
//   renderers/d3d9_renderer.c      - Direct3D 9 (scaffold).
//
//exactly one backend is compiled into gui.dll at a time, selected at
//configure time via the GUI_RENDERER CMake cache variable. platform_win32
//creates the window, then hands the native HWND to the chosen renderer's
//renderer__init() which creates whatever graphics context it needs.
//
//============================================================================
//VISUAL CONTRACT -- binding on every backend.
//============================================================================
//
//all backends MUST produce visually indistinguishable output (modulo at most
//one subpixel of anti-aliasing rounding) given the same sequence of
//renderer__submit_rect calls. swapping the backend must be invisible to the
//host app and the end user. the rules that enforce this:
//
//  COORDINATE SPACE
//    - input rects are in TOP-LEFT-ORIGIN pixel coordinates: (0,0) is the
//      top-left pixel of the window, x grows right, y grows down.
//    - backends convert to their native NDC inside their vertex shader.
//      OpenGL's NDC is y-up with bottom-left origin -> the GL vertex
//      shader does a y-flip. D3D10+'s NDC is y-up with top-left origin ->
//      different conversion, same visual result. D3D9 needs an additional
//      half-pixel adjustment for its integer-pixel-center rasterization
//      rules.
//
//  COLOR SPACE
//    - gui_color components are in LINEAR 0..1. no sRGB conversion done
//      by the library; we write linear values into a non-sRGB back buffer
//      so "what the .style file says" is "what lands on screen" byte-wise.
//      if we ever enable sRGB framebuffers, every backend enables it the
//      same way and we document the switch once.
//
//  ALPHA BLENDING
//    - every backend configures SEPARATE blend factors for color and alpha:
//
//         RGB:    final.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)
//         ALPHA:  final.a   = src.a   * 1     + dst.a   * (1 - src.a)
//
//      The RGB equation is classic alpha blending. The ALPHA equation
//      is what KEEPS the framebuffer's alpha channel at 1.0 across
//      overlapping draws -- a critical detail because:
//
//        * The Windows DWM compositor (opengl3) treats partial
//          framebuffer alpha as window transparency, letting the
//          desktop bleed through. Without separate-alpha blending,
//          our own alpha animations make the WHOLE WINDOW flicker.
//        * Some Android compositors multiply the swap chain by
//          framebuffer alpha at present time. Same flicker.
//        * D3D11/D3D9 swap chains default to ignoring framebuffer
//          alpha but the DXGI desktop window manager doesn't, so
//          we keep the same separate-alpha rule on those backends
//          for the VISUAL CONTRACT to hold byte-for-byte.
//
//      Per backend:
//        OpenGL 3.3 / GLES 3.0:
//          glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
//                              GL_ONE,       GL_ONE_MINUS_SRC_ALPHA)
//        D3D11: SrcBlend=SRC_ALPHA,  DestBlend=INV_SRC_ALPHA,
//               SrcBlendAlpha=ONE,   DestBlendAlpha=INV_SRC_ALPHA
//        D3D9:  D3DRS_SRCBLEND=SRCALPHA,        D3DRS_DESTBLEND=INVSRCALPHA,
//               D3DRS_SEPARATEALPHABLENDENABLE=TRUE,
//               D3DRS_SRCBLENDALPHA=ONE,        D3DRS_DESTBLENDALPHA=INVSRCALPHA
//
//      No color/alpha-channel write masking (RenderTargetWriteMask = ALL).
//
//  ROUNDED RECTANGLE SHAPE
//    - every backend uses the same signed-distance-function:
//
//         float sd_round_box(vec2 p, vec2 b, float r) {
//             vec2 q = abs(p) - b + vec2(r);
//             return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
//         }
//
//      with p = local_pixel - rect_size/2, b = rect_size/2, r = radius.
//    - edge antialiasing: alpha = 1 - smoothstep(-0.5, 0.5, d). that
//      specific range gives a 1-pixel-wide AA band centered on the
//      mathematical edge.
//    - pixels with alpha <= 0 MUST be discarded so the rect's rectangular
//      bounding box doesn't occlude what's behind the rounded corners.
//
//  RADIUS CLAMPING
//    - radius is clamped to [0, min(rect_w, rect_h) * 0.5] before being
//      passed to the SDF. the CPU-side clamp in each backend's
//      renderer__submit_rect must be identical.
//
//  VERTEX TOPOLOGY
//    - each rect is two triangles forming a quad: 6 vertices per rect,
//      no index buffer. ordering:
//         tri 1: (x0,y0) (x1,y0) (x0,y1)      -- top-left, top-right, bottom-left
//         tri 2: (x1,y0) (x1,y1) (x0,y1)      -- top-right, bottom-right, bottom-left
//    - backends with different winding-order defaults MUST disable face
//      culling to render both sides (or arrange winding themselves).
//
//a new backend passes validation when, given the same .ui + .style + scale
//and the same window size, its framebuffer matches the OpenGL reference to
//within one subpixel of anti-aliasing rounding. color readback + diff is
//the practical way to verify during development.
//

/**
 * Bring up the graphics context and initialize the per-frame pipeline.
 * Called once at startup AFTER the platform layer has created its
 * window. The renderer owns whatever GPU objects it needs (GL context,
 * D3D device+swapchain, shaders, vertex buffers) and stores them in
 * its own file-static state.
 *
 * @function renderer__init
 * @param {void*} native_window - Platform window handle. On Windows this is HWND (`struct HWND__*`). Backends cast as needed.
 * @return {boole} TRUE on success; FALSE if context creation, shader compilation, or resource allocation failed.
 */
GUI_API boole renderer__init(void* native_window);

/**
 * Tear down the graphics context and free every GPU/CPU resource
 * owned by the renderer. Symmetric with renderer__init; safe to call
 * even if init failed partway.
 *
 * @function renderer__shutdown
 * @return {void}
 */
GUI_API void  renderer__shutdown(void);

/**
 * Start a frame. Set the viewport, clear the back buffer to `clear`,
 * reset the per-frame vertex batch.
 *
 * @function renderer__begin_frame
 * @param {int64} viewport_w - Viewport width in pixels.
 * @param {int64} viewport_h - Viewport height in pixels.
 * @param {gui_color} clear - Back-buffer clear color.
 * @return {void}
 */
GUI_API void  renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear);

/**
 * Queue one rectangle for the current frame. Radius == 0 produces
 * square corners; positive values are clamped to half the smaller side.
 *
 * @function renderer__submit_rect
 * @param {gui_rect} r - Rectangle in top-left-origin pixel coords.
 * @param {gui_color} c - Fill color (linear RGBA, 0..1).
 * @param {float} radius - Corner radius in pixels.
 * @return {void}
 *
 * Silently drops the submission if the per-frame batch is full.
 */
GUI_API void  renderer__submit_rect(gui_rect r, gui_color c, float radius);

/**
 * Finalize the frame: upload the batched verts, issue the draw, then
 * present the back buffer to the window (swap buffers / swapchain
 * Present). After this call returns, the frame is on screen.
 *
 * @function renderer__end_frame
 * @return {void}
 *
 * End_frame draws the solid-rect pass first, then the text pass
 * (textured quads sampling the currently-bound atlas), then presents.
 * Text always renders on top of solid rects in the same frame --
 * intentional, because buttons/labels want their text over their bg.
 */
GUI_API void  renderer__end_frame(void);

/**
 * Drain the pending rect + text batches mid-frame in the correct order
 * (rects first, then text-on-top), WITHOUT changing scissor state or
 * presenting. After this call the GPU has painted everything submitted
 * so far, and subsequent submissions start a fresh batch that will
 * draw ON TOP of what's already on the back buffer.
 *
 * @function renderer__flush_pending_draws
 * @return {void}
 *
 * Why this exists: the renderer batches text in a separate late pass
 * so glyphs naturally land on top of their own widget's bg rect. That
 * batching breaks z-ordering across SIBLINGS with different z-index:
 * a higher-z sibling's rects would correctly overdraw a lower-z
 * sibling's rects in the rect pass, but the lower-z sibling's TEXT
 * (batched for the late text pass) would then paint on top of the
 * higher-z sibling's rects. scene_render calls this function between
 * children that cross a z-index boundary so each z layer's rects +
 * text land as a unit before the next layer starts.
 *
 * Safe to call multiple times per frame. No-op if both batches are
 * empty. Does NOT touch the scissor stack or GPU scissor state.
 */
GUI_API void  renderer__flush_pending_draws(void);

//============================================================================
//TEXT PIPELINE
//============================================================================
//
//the font module (font.c) owns TTF parsing + rasterization + atlas
//layout. the renderer just provides GPU storage for the atlas bitmap
//and a second draw pass for textured glyph quads.
//
//each backend implements four text entry points below + a shader that
//samples the bound atlas with LINEAR filtering (imgui-quality antialiased
//glyphs). the atlas is R8 (alpha-only); the shader multiplies the
//sampled alpha by the per-vertex color.
//
//each glyph's "color" is the text color (from the style) -- the atlas
//provides only alpha/coverage. the rect is in pixel coords (top-left
//origin, same as solid rects). uv is 0..1 into the atlas.
//

/**
 * Upload an alpha-only bitmap to the GPU as a texture. Called by
 * font.c when rasterizing a new (family, size) pair.
 *
 * @function renderer__create_atlas_r8
 * @param {const ubyte*} pixels - Row-major R8 bitmap (one byte per pixel, interpreted as alpha).
 * @param {int} width - Bitmap width in pixels.
 * @param {int} height - Bitmap height in pixels.
 * @return {void*} Opaque backend-specific texture handle (GLuint / ID3D11ShaderResourceView* / IDirect3DTexture9*), or NULL on failure.
 */
GUI_API void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height);

/**
 * Destroy an atlas created by renderer__create_atlas_r8.
 *
 * @function renderer__destroy_atlas
 * @param {void*} atlas - Handle returned by create_atlas_r8. NULL is a no-op.
 * @return {void}
 */
GUI_API void  renderer__destroy_atlas(void* atlas);

/**
 * Bind the atlas to be sampled by subsequent renderer__submit_text_glyph
 * calls (until either another atlas is bound or the frame ends). The
 * text draw pass uses the last-bound atlas -- so all text in one frame
 * that uses different fonts needs to be submitted in font-coherent runs.
 *
 * @function renderer__set_text_atlas
 * @param {void*} atlas - Handle from create_atlas_r8.
 * @return {void}
 */
GUI_API void  renderer__set_text_atlas(void* atlas);

/**
 * Queue one textured quad into the frame's text batch. Drawn after
 * all solid rects when end_frame fires.
 *
 * @function renderer__submit_text_glyph
 * @param {gui_rect} rect - Quad position + size in pixels (top-left origin).
 * @param {gui_rect} uv - UV rect into the atlas (0..1 coordinates; uv.w/h are sizes, not endpoints).
 * @param {gui_color} color - Text color; per-pixel alpha = color.a * atlas_sample.r.
 * @return {void}
 */
GUI_API void  renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color);

//============================================================================
//SHADOW + GRADIENT PIPELINE (extensions of the solid-rect pass)
//============================================================================
//
// Shadow: one SDF rounded-box splat, offset by (dx, dy) from the shape
// it's decorating, with a `blur`-wide soft edge. Submit BEFORE the bg
// rect so z-order is shadow-under-bg.
//
// Gradient: same rect + radius + SDF AA as submit_rect, but with two
// colors and a direction enum. Fragment shader lerps between the two
// colors along the requested axis.
//
// Backends without a full implementation: opengl3 + gles3 implement
// both; d3d11 / d3d9 fall back to submit_rect with a single color
// (no shadow, no gradient -- scenes that use either silently degrade).
//

/**
 * Queue a shadow splat for the current frame. Same rect convention as
 * submit_rect. `blur` is the feather width in pixels. Caller supplies
 * the final offset rect (rect.x already includes dx, rect.y includes
 * dy) so the renderer doesn't have to know about the originating shape.
 *
 * @function renderer__submit_shadow
 * @param {gui_rect} rect - Shadow bounding rect (already offset).
 * @param {gui_color} c - Shadow color (alpha controls strength).
 * @param {float} radius - Same corner radius as the decorated shape.
 * @param {float} blur - Soft-edge width in pixels. 0 = hard edge.
 * @return {void}
 */
GUI_API void renderer__submit_shadow(gui_rect rect, gui_color c, float radius, float blur);

/**
 * Two-color linear gradient rect. Drop-in replacement for submit_rect
 * when a gradient is requested. `direction` selects the axis via
 * gui_gradient_dir.
 *
 * @function renderer__submit_rect_gradient
 * @param {gui_rect} r - Rect in top-left-origin pixel coords.
 * @param {gui_color} from - Color at the start of the axis.
 * @param {gui_color} to - Color at the end of the axis.
 * @param {int} direction - gui_gradient_dir value. Cast to int for ABI stability across the DLL boundary.
 * @param {float} radius - Corner radius in pixels.
 * @return {void}
 */
GUI_API void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius);

//============================================================================
//IMAGE PIPELINE (RGBA textures)
//============================================================================
//
//Separate from the text pipeline because images sample ALL FOUR channels
//of their texture while text atlases are R8 (coverage only). Same quad-
//based draw shape, different shader.
//
//Usage pattern (widget_image):
//  1. decode file bytes (stb_image) to RGBA8.
//  2. void* tex = renderer__create_texture_rgba(rgba, w, h); -- once.
//  3. renderer__submit_image(rect, tex, tint); -- per frame.
//  4. renderer__destroy_texture(tex); -- on widget destroy.
//
//Images flush any pending rect/text batch on submit so draw order is
//preserved (image on top of a previously-submitted rect, underneath a
//subsequently-submitted text glyph, etc.). One draw call per image is
//fine -- UI scenes typically have <10 images, not hundreds of glyphs.
//
//Stubs on backends without an image implementation yet return NULL
//from create_texture_rgba; widget_image treats NULL as "skip draw",
//so a scene using <image> still runs on those backends (it just shows
//the node's background rect instead of the picture).
//

/**
 * Upload an RGBA8 image to the GPU. Bytes are consumed top-to-bottom,
 * left-to-right, 4 bytes per pixel, no row padding (tightly packed).
 * Caller retains ownership of the `rgba` buffer; the renderer copies
 * the data into a texture and no longer references the source pointer
 * after returning.
 *
 * @function renderer__create_texture_rgba
 * @param {const ubyte*} rgba - Tightly-packed RGBA8 pixel buffer, length = width*height*4.
 * @param {int} width - Image width in pixels.
 * @param {int} height - Image height in pixels.
 * @return {void*} Opaque texture handle, or NULL if upload failed or the backend doesn't yet implement this pipeline.
 */
GUI_API void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height);

/**
 * Release a texture returned by renderer__create_texture_rgba. NULL
 * is a no-op so widgets that never successfully uploaded can still
 * call this from their destroy hook without a guard.
 *
 * @function renderer__destroy_texture
 * @param {void*} tex - Handle from create_texture_rgba.
 * @return {void}
 */
GUI_API void  renderer__destroy_texture(void* tex);

/**
 * Draw one image quad immediately. Flushes any pending solid-rect or
 * text batch first so previously-submitted content stays underneath
 * and subsequent submissions stay on top. Full-texture draw (UV 0..1
 * on both axes); no subregion support in this pass (crop via node
 * bounds + scissor instead).
 *
 * @function renderer__submit_image
 * @param {gui_rect} rect - Destination rect in top-left-origin pixel coords.
 * @param {void*} tex - Texture handle from create_texture_rgba. NULL = skip draw.
 * @param {gui_color} tint - Multiplied against the sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 * @return {void}
 */
GUI_API void  renderer__submit_image(gui_rect rect, void* tex, gui_color tint);

//============================================================================
//SCISSOR (CLIPPING)
//============================================================================
//
//push_scissor / pop_scissor bracket a region of submissions whose draws
//are clipped to the given rect. Used by scrollable containers (widget_div,
//widget_window) to hide content that overflows their bounds.
//
//Calls nest. The current effective scissor is the INTERSECTION of every
//rect on the stack -- so a scrollable child div inside a scrollable
//parent div correctly clips to the smaller of the two regions.
//
//Implementation note: backends flush their batched submissions on every
//push/pop because state changes mid-frame can't be merged into a single
//draw call. Scrollable regions therefore add a few extra draw calls per
//frame -- fine for the dozens of regions a real UI has.
//

/**
 * Push a clip rect onto the scissor stack. Subsequent submissions are
 * clipped to the intersection of every rect currently on the stack.
 *
 * @function renderer__push_scissor
 * @param {gui_rect} rect - Clip region in pixel coords (top-left origin).
 * @return {void}
 *
 * Flushes the current batch before changing state, so the rects/text
 * already submitted this frame are NOT clipped to this new region.
 */
GUI_API void  renderer__push_scissor(gui_rect rect);

/**
 * Pop the most recent scissor rect off the stack. If the stack is now
 * empty, scissor testing is disabled entirely (everything draws).
 *
 * @function renderer__pop_scissor
 * @return {void}
 *
 * Flushes the current batch before changing state, so submissions made
 * BEFORE the pop are clipped to the popped rect, not the previous one.
 */
GUI_API void  renderer__pop_scissor(void);

/**
 * Capture the framebuffer region under `rect`, apply a separable
 * Gaussian blur with standard deviation `sigma_px`, and paint the
 * blurred result back over `rect`. Subsequent submissions draw on
 * top of the blurred backdrop, as the caller intends (e.g. text
 * rendered legibly above a frosted panel background).
 *
 * @function renderer__blur_region
 * @param {gui_rect} rect - Region to blur, in pixel coords.
 * @param {float} sigma_px - Gaussian standard deviation in pixels. Values
 *   under 1.0 produce negligible blur; 4-16 is the "frosted glass" range.
 * @return {void}
 *
 * The call flushes the current batch before reading from the framebuffer
 * (same reason push_scissor flushes: anything submitted AFTER this call
 * must land on top of the blur, not under it). Backends that don't yet
 * implement true blur fall back to a translucent darken splat over the
 * rect, which matches the prior placeholder behaviour.
 */
GUI_API void  renderer__blur_region(gui_rect rect, float sigma_px);

#endif
