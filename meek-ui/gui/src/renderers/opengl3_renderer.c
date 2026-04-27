//============================================================================
//opengl3_renderer.c - opengl 3.3 core backend.
//============================================================================
//
//WHAT THIS FILE DOES (the elevator pitch):
//
//  scene.c walks the widget tree and tells us "draw this colored rectangle
//  here" by calling renderer__submit_rect(). this file packages those calls
//  into work the GPU understands and ships it across to the graphics card
//  using the opengl 3.3 api. the GPU then turns that into pixels on screen.
//
//  this backend also owns every piece of the opengl integration stack:
//  picking a compatible pixel format for the window, building a 3.3 core
//  context, loading the function pointers for every GL entry point we use,
//  and calling SwapBuffers at the end of each frame. platform_win32 just
//  hands us an HWND at init and stays out of our way.
//
//WHY OPENGL?
//
//  the GPU doesn't speak C. it speaks a small set of binary commands like
//  "draw triangles using these vertices", "run this little program for every
//  pixel inside them", "blend the result with what's already on screen".
//  opengl is one of several apis that gives us a portable way to issue those
//  commands. d3d9, d3d11, vulkan, metal are alternatives -- our renderers/
//  directory has stubs for d3d9 and d3d11 alongside this one. exactly one
//  is compiled into the gui library at a time.
//
//WHY BATCHING?
//
//  each call from CPU to GPU costs hundreds of microseconds of overhead --
//  context switching, driver bookkeeping, command queueing. drawing 100 rects
//  with 100 separate "draw" calls is dramatically slower than gathering them
//  into a list of 600 vertices (100 rects * 6 verts per quad) and issuing
//  one "draw 600 vertices" call. so:
//
//      begin_frame:    clear screen, reset our cpu-side vertex list.
//      submit_rect *N: append 6 vertices to the list per rectangle.
//      end_frame:      copy the whole list to the GPU, issue ONE draw call,
//                      then SwapBuffers to make the frame visible.
//
//  this gets us 60+ FPS even with thousands of rects per frame.
//
//ROUNDED CORNERS WITHOUT EXTRA TRIANGLES (SDF):
//
//  the obvious way to draw a rounded rect is to tessellate the corners with
//  many tiny triangles (like 8 per corner) approximating the curve. that
//  works but it's wasteful and aliases badly when zoomed.
//
//  instead, every rect is just two triangles forming a square (6 vertices).
//  the fragment shader -- a tiny program the GPU runs FOR EVERY PIXEL inside
//  those triangles -- asks for each pixel: "am i inside the rounded shape,
//  outside, or right on the edge?" using a Signed Distance Function (SDF).
//
//  pixels inside: full color. pixels outside: discarded (transparent).
//  pixels on the edge: partial alpha for free anti-aliasing.
//
//  result: pixel-perfect rounded corners at any size, with one shader, no
//  per-corner geometry. when radius is zero the math collapses to a regular
//  rectangle and you just get sharp corners.
//
//VISUAL CONTRACT:
//
//  this backend must produce the same pixels as every other backend
//  (d3d11, d3d9, ...) given the same input -- see renderer.h's "VISUAL
//  CONTRACT" section. the SDF function, blend mode, smoothstep range,
//  and radius clamping are all specified; don't diverge from them.
//

#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>

#include <math.h>     // powf for sRGB -> linear clear-color conversion.
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   // uintptr_t for the GLuint <-> void* atlas-handle round trip.

#include "types.h"
#include "gui.h"
#include "renderer.h"
#include "_visual_contract.h"
#include "../clib/stdlib.h"
//
//gl_funcs.h lives next to this file (inside renderers/) because its
//contents are opengl-specific. include path is relative to this file.
//
#include "gl_funcs.h"
#include "../clib/memory_manager.h"
#include "../third_party/log.h"

//============================================================================
//GL FUNCTION POINTER STORAGE
//============================================================================
//
//gl_funcs.h declares these as extern. we define them (allocate the storage)
//here because this backend is the sole loader + user of them. on non-GL
//backend builds this file isn't compiled, so the globals don't exist and
//nothing else references them -- the linker is happy.
//

fncp_glCreateShader            p_glCreateShader;
fncp_glDeleteShader            p_glDeleteShader;
fncp_glShaderSource            p_glShaderSource;
fncp_glCompileShader           p_glCompileShader;
fncp_glGetShaderiv             p_glGetShaderiv;
fncp_glGetShaderInfoLog        p_glGetShaderInfoLog;

fncp_glCreateProgram           p_glCreateProgram;
fncp_glDeleteProgram           p_glDeleteProgram;
fncp_glAttachShader            p_glAttachShader;
fncp_glLinkProgram             p_glLinkProgram;
fncp_glGetProgramiv            p_glGetProgramiv;
fncp_glGetProgramInfoLog       p_glGetProgramInfoLog;
fncp_glUseProgram              p_glUseProgram;

fncp_glGetUniformLocation      p_glGetUniformLocation;
fncp_glUniform2f               p_glUniform2f;
fncp_glUniform1i               p_glUniform1i;
fncp_glActiveTexture           p_glActiveTexture;

fncp_glGenBuffers              p_glGenBuffers;
fncp_glDeleteBuffers           p_glDeleteBuffers;
fncp_glBindBuffer              p_glBindBuffer;
fncp_glBufferData              p_glBufferData;
fncp_glBufferSubData           p_glBufferSubData;

fncp_glGenVertexArrays         p_glGenVertexArrays;
fncp_glBindVertexArray         p_glBindVertexArray;
fncp_glDeleteVertexArrays      p_glDeleteVertexArrays;
fncp_glVertexAttribPointer     p_glVertexAttribPointer;
fncp_glEnableVertexAttribArray p_glEnableVertexAttribArray;

fncp_glBlendFuncSeparate       p_glBlendFuncSeparate;
fncp_glGenerateMipmap          p_glGenerateMipmap;

//============================================================================
//WGL EXTENSION BITS FOR 3.3 CORE CONTEXT CREATION
//============================================================================
//
//core 3.3 contexts are created via wglCreateContextAttribsARB, which is an
//extension. to call it we first need a GL context (any context) to resolve
//the function pointer. the classic chicken-and-egg dance:
//
//   1. make a "legacy" (2.x) context so we can call wglGetProcAddress.
//   2. resolve wglCreateContextAttribsARB via that context.
//   3. call it to create the real 3.3 core context.
//   4. throw away the legacy context.
//

typedef HGLRC (WINAPI* fncp_wglCreateContextAttribsARB)(HDC hDC, HGLRC hShareContext, const int* attribList);

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

//============================================================================
//VERTEX FORMAT
//============================================================================
//
//a "vertex" is just a struct of data the GPU shader program receives one
//copy of, per corner of every triangle we draw. you can think of it as
//"the input row to the vertex shader".
//
//each rect = 6 vertices (2 triangles = 6 corners), and each vertex carries:
//  - WHERE on screen this corner is        (x, y)         in pixels
//  - WHAT color this corner should be      (r, g, b, a)
//  - WHERE inside the rect this corner is  (lx, ly)       in 0..rect_w/h
//  - HOW BIG the parent rect is            (rect_w, rect_h)
//  - HOW ROUND the corners are             (radius)
//
//the last three are the same value for all 6 vertices of one rect. they're
//"varyings" the fragment shader needs to compute the SDF math. we duplicate
//them per-vertex (instead of using uniforms or instancing) because it keeps
//the batching trivial -- one pass over the buffer, no per-rect bookkeeping
//on the GPU side.
//

/**
 *one vertex in the cpu-side staging buffer. layout MUST match the shader
 *attribute setup in renderer__init below (same field order, same types).
 *if you change this struct, also change those attribute pointers.
 */
typedef struct _opengl3_renderer_internal__vertex
{
    float x;        // pixel x of this corner (top-left origin: 0 = left edge of window).
    float y;        // pixel y of this corner (0 = top edge of window).
    float r;        // red component of this corner, 0..1.
    float g;        // green component, 0..1.
    float b;        // blue component, 0..1.
    float a;        // alpha (opacity), 0..1. 0 = fully transparent, 1 = opaque.
    float lx;       // local x INSIDE the rect this vertex belongs to: 0 at the left edge, rect_w at the right edge.
    float ly;       // local y INSIDE the rect: 0 at the top, rect_h at the bottom.
    float rect_w;   // full width of the rect this vertex belongs to. fragment shader needs it for the SDF math.
    float rect_h;   // full height of the rect.
    float radius;   // corner radius in pixels. 0 = sharp rectangle. positive = rounded; clamped to half the smaller side.
} _opengl3_renderer_internal__vertex;

//
//how many rects we can hold in our cpu-side batch buffer in one frame.
//going past this drops draw calls silently. 2048 is plenty for the poc.
//once we add scrolling lists of thousands of items, we'll either grow this
//or flush mid-frame.
//
#define _OPENGL3_RENDERER_INTERNAL__MAX_QUADS       2048
#define _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD  6

//
//text batch capacity: one textured quad per glyph. 4096 glyphs per frame
//is plenty for a typical UI (a screen of 14px code at 40 lines * 80 cols
//is ~3200 glyphs); anything larger should be paged with scroll clipping
//anyway. kept conservative because this toolkit is intended to render on
//ARM embedded devices and phones where VBO memory matters -- 4096 * 6
//verts * 32 bytes = 768 KiB, well under mobile GPU working-set limits.
//each glyph adds 6 verts of the smaller text vertex format declared below.
//
#define _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096

//
//text run: a contiguous slice of the text vertex buffer that samples
//from ONE atlas. the frame can intermix multiple atlases (different
//fonts + sizes); we split the batch into runs so each run binds its
//own texture. set_text_atlas closes the current run and opens a new
//one when the atlas changes; each (font__draw call for a fresh font)
//typically produces one run.
//
//MAX_TEXT_RUNS caps how many distinct atlases can be used per frame.
//64 covers 16 fonts at 4 sizes each, well beyond any realistic UI.
//
#define _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_RUNS 64

typedef struct _opengl3_renderer_internal__text_run
{
    GLuint atlas;       // GL texture handle for this run.
    int64  vert_start;  // offset into text_cpu_verts.
    int64  vert_count;  // number of verts belonging to this run.
} _opengl3_renderer_internal__text_run;

//
//a text vertex is simpler than the solid-rect vertex: no SDF bookkeeping,
//just position + color + atlas uv. one per corner of each glyph quad, 6
//per glyph.
//
typedef struct _opengl3_renderer_internal__text_vertex
{
    float x;   // pixel x of this corner (top-left origin: 0 = left edge of window).
    float y;   // pixel y.
    float r;   // color channels, 0..1. per-vertex so each draw carries its own tint.
    float g;
    float b;
    float a;
    float u;   // atlas UV in 0..1.
    float v;
} _opengl3_renderer_internal__text_vertex;

//============================================================================
//RENDERER STATE
//============================================================================
//
//all the GPU object handles, win32 handles, and CPU-side staging we hold
//onto across frames. exactly one instance of this struct exists for the
//process lifetime; it's allocated as a file-static global below.
//

/**
 *renderer state. file-local; one instance per process.
 */
typedef struct _opengl3_renderer_internal__state
{
    HWND   hwnd;           // host window. supplied by platform_win32 in renderer__init.
    HDC    hdc;            // device context for the window. we derive this ourselves; we hold onto it for SwapBuffers.
    HGLRC  hglrc;          // gl 3.3 core context.

    //
    //solid-rect (SDF rounded-box) pipeline.
    //
    GLuint program;        // handle to the linked vertex+fragment shader program. set by renderer__init.
    GLint  u_viewport_loc; // cached "location" (slot) for the u_viewport uniform inside the program. -1 if not found.
    GLuint vao;            // Vertex Array Object: a recipe describing how the VBO's bytes map to shader attributes.
    GLuint vbo;            // Vertex Buffer Object: a chunk of GPU memory holding our vertices.
    _opengl3_renderer_internal__vertex* cpu_verts; // cpu-side scratch buffer; we fill this during the frame, then upload to the VBO once.
    int64                       vert_count;        // how many vertices we've written into cpu_verts this frame so far.
    int64                       vert_cap;          // capacity of cpu_verts (and the GPU VBO) in vertices.
    int64 viewport_w;      // window width in pixels, captured at begin_frame. used by the shader to convert pixel coords to GPU coords.
    int64 viewport_h;      // window height in pixels.

    //
    //text pipeline. separate shader (no SDF; just textured alpha),
    //separate VAO/VBO, separate cpu staging buffer. drawn after the
    //solid pass in end_frame so glyphs land on top of button bg rects.
    //
    //per-atlas "runs": the frame can mix text from multiple fonts
    //(different family/size pairs => different atlases). each run is
    //a contiguous slice of the vertex buffer that binds one atlas.
    //end_frame does one glDrawArrays per run.
    //
    GLuint text_program;         // shader program for the textured glyph quads.
    GLint  text_u_viewport_loc;  // u_viewport slot in text_program.
    GLint  text_u_atlas_loc;     // u_atlas (sampler2D) slot in text_program.
    GLuint text_vao;             // vao for the text vertex layout.
    GLuint text_vbo;             // vbo for text vertices.
    _opengl3_renderer_internal__text_vertex* text_cpu_verts; // cpu staging for text verts.
    int64  text_vert_count;      // text verts written this frame.
    int64  text_vert_cap;        // capacity of text_cpu_verts.
    GLuint current_text_atlas;   // GLuint of the atlas most recently set by set_text_atlas.

    _opengl3_renderer_internal__text_run text_runs[_OPENGL3_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int64                                text_run_count;

    //
    //image pipeline. lazy-initialized on first renderer__submit_image
    //call; zero means "not built yet". Separate shader (samples RGBA
    //not just .r). Separate 6-vert VBO used immediate-mode (one quad
    //per draw call, no batching -- images are 0..N-few per frame).
    //
    GLuint image_program;
    GLint  image_u_viewport_loc;
    GLint  image_u_tex_loc;
    GLuint image_vao;
    GLuint image_vbo;

    //
    //scissor stack. push_scissor records the new clip rect at the top
    //and applies the intersection of all stacked rects via glScissor.
    //pop pops one. depth 0 means "no scissor; everything draws".
    //32 deep is way more than any sane UI nesting needs.
    //
    gui_rect scissor_stack[32];
    int64    scissor_depth;

    //
    // sRGB framebuffer in play. Set by set_pixel_format when
    // wglChoosePixelFormatARB successfully returns a pixel format
    // with WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB = TRUE. When set:
    //   - glEnable(GL_FRAMEBUFFER_SRGB) runs after context creation.
    //   - Clear color is linearized before glClearColor (the default
    //     framebuffer treats clear values as linear and gamma-encodes
    //     on store).
    //   - build_pipeline compiles the sRGB-aware fragment shaders
    //     which convert vertex-color RGB from sRGB to linear, so the
    //     hardware's linear->sRGB write produces the correct color.
    // When NOT set (driver lacks WGL_ARB_pixel_format, or it failed
    // to find a matching format), we stay on the legacy non-sRGB
    // framebuffer and compile the older shaders that keep colors in
    // sRGB space and use the pow(a, 1/2.2) coverage workaround for
    // text. Same visual output as before this refactor.
    //
    boole srgb_fb;
} _opengl3_renderer_internal__state;

static _opengl3_renderer_internal__state _opengl3_renderer_internal__g;

//============================================================================
//SHADER SOURCES
//============================================================================
//
//"shader" = a tiny program that runs on the GPU. opengl 3.3 uses GLSL, a
//C-like language. we have two programs forming one pipeline:
//
//  VERTEX SHADER:    runs once per vertex (so 6 times per rect).
//                    receives the vertex's attributes as inputs (in vec2/vec4).
//                    its job: write gl_Position (where on screen this vertex
//                    lands, in "Normalized Device Coordinates" -- see below).
//                    can also produce "varyings" (out variables) that get
//                    automatically interpolated across the triangle and
//                    delivered to the fragment shader.
//
//  FRAGMENT SHADER:  runs once per pixel inside the triangle (could be
//                    millions of times per frame). receives the interpolated
//                    varyings from the vertex shader. writes the final pixel
//                    color (or "discards" the pixel to leave it transparent).
//
//between them: the GPU's rasterizer figures out which pixels are inside the
//triangle and runs the fragment shader for each one, interpolating the
//varyings linearly (or perspective-correct, but for 2D it's just linear).
//
//NORMALIZED DEVICE COORDINATES (NDC):
//  the GPU's native coordinate system for gl_Position. it's a cube from
//  (-1, -1, -1) to (+1, +1, +1) where (-1, -1) is bottom-left of the
//  screen and (+1, +1) is top-right. we work in pixels (top-left = 0,0)
//  because that's what humans understand. the vertex shader does the
//  conversion: pixel_x / viewport_width * 2 - 1 -> NDC x, and so on.
//  note the y-axis flip: pixel y grows downward, NDC y grows upward.
//

//
//VERTEX SHADER:
//  inputs:  the 5 per-vertex attributes (matching our vertex struct layout).
//  uniform: u_viewport (constant for all vertices in one draw call -- the
//           current window size in pixels).
//  outputs: gl_Position (built-in: where this vertex lives in NDC),
//           plus varyings v_color/v_local/v_rect_size/v_radius forwarded
//           to the fragment shader.
//
static char* _opengl3_renderer_internal__vs_src =
    "#version 330 core\n"
    "layout(location = 0) in vec2  a_pos_px;\n"
    "layout(location = 1) in vec4  a_color;\n"
    "layout(location = 2) in vec2  a_local;\n"
    "layout(location = 3) in vec2  a_rect_size;\n"
    "layout(location = 4) in float a_radius;\n"
    "uniform vec2 u_viewport;\n"
    "out vec4  v_color;\n"
    "out vec2  v_local;\n"
    "out vec2  v_rect_size;\n"
    "out float v_radius;\n"
    "void main()\n"
    "{\n"
    "    // convert pixel coords (top-left origin) to NDC ([-1,+1], y-up).\n"
    "    vec2 ndc = vec2(\n"
    "        (a_pos_px.x / u_viewport.x) * 2.0 - 1.0,\n"
    "        1.0 - (a_pos_px.y / u_viewport.y) * 2.0\n"
    "    );\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color     = a_color;\n"
    "    v_local     = a_local;\n"
    "    v_rect_size = a_rect_size;\n"
    "    v_radius    = a_radius;\n"
    "}\n";

//
//FRAGMENT SHADER: SDF rounded-box + 1px smoothstep AA. see renderer.h's
//VISUAL CONTRACT section -- the SDF formula and smoothstep range are
//binding on all backends, not just this one.
//
static char* _opengl3_renderer_internal__fs_src =
    "#version 330 core\n"
    "in vec4  v_color;\n"
    "in vec2  v_local;\n"
    "in vec2  v_rect_size;\n"
    "in float v_radius;\n"
    "out vec4 o_color;\n"
    "\n"
    RENDERER_SDF_ROUND_BOX_GLSL
    "\n"
    "void main()\n"
    "{\n"
    "    vec2 half_size = v_rect_size * 0.5;\n"
    "    vec2 p = v_local - half_size;\n"
    "    float d = sd_round_box(p, half_size, v_radius);\n"
    "    float aa = 1.0 - smoothstep(" RENDERER_SDF_AA_MIN ", " RENDERER_SDF_AA_MAX ", d);\n"
    "    if (aa <= 0.0) discard;\n"
    "    o_color = vec4(v_color.rgb, v_color.a * aa);\n"
    "}\n";

//
//TEXT VERTEX SHADER:
//  simpler than the solid-rect one -- just (position, color, uv). the
//  same pixel-coords -> NDC conversion is used. varying outputs the uv
//  so the fragment shader can sample the atlas.
//
static char* _opengl3_renderer_internal__text_vs_src =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos_px;\n"
    "layout(location = 1) in vec4 a_color;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "uniform vec2 u_viewport;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "void main()\n"
    "{\n"
    "    vec2 ndc = vec2(\n"
    "        (a_pos_px.x / u_viewport.x) * 2.0 - 1.0,\n"
    "        1.0 - (a_pos_px.y / u_viewport.y) * 2.0\n"
    "    );\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "    v_uv    = a_uv;\n"
    "}\n";

//
//TEXT FRAGMENT SHADER:
//  samples the atlas (R8 alpha-only glyph coverage) with LINEAR
//  filtering and multiplies it by the vertex color's alpha to produce
//  smooth anti-aliased text. discards when alpha is zero so we don't
//  create needless blend work for transparent pixels (and so quad
//  edges between glyphs don't accumulate "zero + zero" color that
//  could be picked up by downstream effects).
//
//  GAMMA-CORRECT EDGE COVERAGE. We don't render to an sRGB framebuffer
//  (that would require WGL_ARB_pixel_format's sRGB-capable attribute,
//  which needs a whole dummy-context dance to resolve before we can
//  call it -- a bigger refactor than we wanted for the first sharpness
//  pass). Instead we apply the gamma curve to the atlas coverage
//  BEFORE multiplying by vertex alpha: pow(a, 1/2.2).
//
//  Why this helps: standard SRC_ALPHA/INV_SRC_ALPHA blending treats
//  8-bit color values as if they were linear intensities, but monitors
//  interpret them as sRGB. That means a pixel with coverage=0.5 blends
//  50%/50% between text color and background in code, but the human
//  eye sees it as ~22% brightness relative to full text color
//  (because sRGB curves the lower half hard). Edges of glyphs end up
//  looking dim and soft -- the "washed-out grayscale AA" look that
//  makes our text feel blurry next to DirectWrite's output in WSL
//  Settings or similar native UIs.
//
//  pow(coverage, 1/2.2) reshapes the coverage so that a linear ramp
//  in input produces a visually-linear ramp out. At coverage=0.5 this
//  gives ~0.73, i.e. 73% blend toward text color, which lands at the
//  perceptual midpoint. Edges appear solid and crisp, not smeared.
//
//  This is an approximation of proper sRGB-framebuffer blending --
//  accurate to within a percent or two for the common case of light
//  text on a dark background, which is essentially all of the UI.
//  For a fully physically-correct pipeline we would eventually want
//  an sRGB swap chain (or an intermediate sRGB FBO we blit from) on
//  all four backends; until then this keeps the rest of the pipeline
//  untouched.
//
static char* _opengl3_renderer_internal__text_fs_src =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main()\n"
    "{\n"
    "    float a = texture(u_atlas, v_uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    "    // gamma-correct the edge coverage so glyph boundaries\n"
    "    // don't get darkened by sRGB-space alpha blending.\n"
    "    a = pow(a, 1.0 / 2.2);\n"
    "    o_color = vec4(v_color.rgb, v_color.a * a);\n"
    "}\n";

//
// ===== sRGB-framebuffer shader variants ====================================
//
// Used when the pixel format is sRGB-capable (wglChoosePixelFormatARB +
// WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB) and GL_FRAMEBUFFER_SRGB is enabled.
// In that mode, the hardware converts shader output values from linear
// to sRGB on write, and alpha blending happens in linear space --
// exactly what we want for physically-correct edge compositing.
//
// Two changes from the legacy shaders above:
//   1. vec3 linear_rgb = pow(v_color.rgb, vec3(2.2));
//      Vertex colors are sourced from sRGB hex values in .style
//      (e.g. #6366f1). The hardware writes linear->sRGB, so we
//      must feed it linear values; converting in the fragment
//      shader is the simplest place.
//   2. Text shader drops the pow(a, 1/2.2) coverage workaround.
//      That hack existed to approximate gamma-correct blending on
//      a non-sRGB FB by brightening edge coverage; with a real
//      sRGB FB the math is already correct. Applying it here
//      would DOUBLE-gamma the alpha and produce thick/chunky text.
//
// pow(x, 2.2) is a fast approximation of the full piecewise sRGB
// transfer function. Error is < 0.01 across the 0..1 range, which is
// well below 8-bit quantization. Worth the two saved instructions.
//
static char* _opengl3_renderer_internal__fs_src_srgb =
    "#version 330 core\n"
    "in vec4  v_color;\n"
    "in vec2  v_local;\n"
    "in vec2  v_rect_size;\n"
    "in float v_radius;\n"
    "out vec4 o_color;\n"
    "\n"
    RENDERER_SDF_ROUND_BOX_GLSL
    "\n"
    "void main()\n"
    "{\n"
    "    vec2 half_size = v_rect_size * 0.5;\n"
    "    vec2 p = v_local - half_size;\n"
    "    float d = sd_round_box(p, half_size, v_radius);\n"
    "    float aa = 1.0 - smoothstep(" RENDERER_SDF_AA_MIN ", " RENDERER_SDF_AA_MAX ", d);\n"
    "    if (aa <= 0.0) discard;\n"
    "    vec3 linear_rgb = pow(v_color.rgb, vec3(2.2));\n"
    "    o_color = vec4(linear_rgb, v_color.a * aa);\n"
    "}\n";

static char* _opengl3_renderer_internal__text_fs_src_srgb =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main()\n"
    "{\n"
    "    float a = texture(u_atlas, v_uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    "    vec3 linear_rgb = pow(v_color.rgb, vec3(2.2));\n"
    "    o_color = vec4(linear_rgb, v_color.a * a);\n"
    "}\n";

//
//forward declarations of file-local helpers. defined further down.
//
static boole  _opengl3_renderer_internal__set_pixel_format(HDC hdc);
static boole  _opengl3_renderer_internal__create_core_context(HDC hdc, HGLRC* out_ctx);
static boole  _opengl3_renderer_internal__load_gl_functions(void);
static boole  _opengl3_renderer_internal__build_pipeline(void);
static boole  _opengl3_renderer_internal__build_text_pipeline(void);
static GLuint _opengl3_renderer_internal__compile_shader(GLenum kind, char* src);
static GLuint _opengl3_renderer_internal__link_program(GLuint vs, GLuint fs);
static void   _opengl3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c);
static void   _opengl3_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c);
static void   _opengl3_renderer_internal__log_win32_error(char* where);

//============================================================================
//PUBLIC: renderer__init
//============================================================================
//
//bring up the GL context against the window the platform layer just
//created, then build our rendering pipeline on top.
//
//steps:
//  1. grab an HDC for the window.
//  2. pick + set a pixel format compatible with opengl.
//  3. create a dummy legacy GL context so we can resolve the 3.3 core
//     context-creation extension.
//  4. create the real 3.3 core context.
//  5. make it current on this thread.
//  6. load every GL 3.x function pointer we need.
//  7. build shaders + VAO + VBO + cpu staging buffer (the "pipeline").
//  8. enable alpha blending.
//

boole renderer__init(void* native_window)
{
    memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));

    _opengl3_renderer_internal__g.hwnd = (HWND)native_window;
    if (_opengl3_renderer_internal__g.hwnd == NULL)
    {
        log_error("renderer__init got a NULL window handle");
        return FALSE;
    }

    //
    //step 1: get an HDC. required for every subsequent GL+wgl call.
    //we hold onto it for the life of the renderer -- SwapBuffers uses
    //it at end_frame.
    //
    _opengl3_renderer_internal__g.hdc = GetDC(_opengl3_renderer_internal__g.hwnd);
    if (_opengl3_renderer_internal__g.hdc == NULL)
    {
        _opengl3_renderer_internal__log_win32_error("GetDC");
        return FALSE;
    }

    //
    //step 2: pixel format. describes the back buffer's bit depth,
    //double-buffering, stencil/depth availability, etc. CAN ONLY BE
    //SET ONCE per window. if we ever destroy and recreate the GL
    //context for the same window we'd be stuck.
    //
    if (!_opengl3_renderer_internal__set_pixel_format(_opengl3_renderer_internal__g.hdc))
    {
        return FALSE;
    }

    //
    //steps 3+4: create the real core context via the dummy-context dance.
    //
    if (!_opengl3_renderer_internal__create_core_context(_opengl3_renderer_internal__g.hdc, &_opengl3_renderer_internal__g.hglrc))
    {
        return FALSE;
    }

    //
    //step 5: bind the context to this thread. subsequent gl* calls go
    //through it.
    //
    if (!wglMakeCurrent(_opengl3_renderer_internal__g.hdc, _opengl3_renderer_internal__g.hglrc))
    {
        _opengl3_renderer_internal__log_win32_error("wglMakeCurrent");
        return FALSE;
    }

    //
    //step 6: load every GL 3.x function pointer we use. fails loudly
    //if any one is missing -- means the driver lied about supporting 3.3.
    //
    if (!_opengl3_renderer_internal__load_gl_functions())
    {
        log_error("failed to load gl 3.3 functions");
        return FALSE;
    }

    //
    //step 7+8: build the draw pipeline (shaders, VAO, VBO, cpu buffer,
    //blend state).
    //
    return _opengl3_renderer_internal__build_pipeline();
}

//============================================================================
//PUBLIC: renderer__shutdown
//============================================================================
//
//symmetric with init. tear down the pipeline, then the context, then
//release the HDC. safe to call even if init bailed partway, because
//every handle starts at 0 / NULL and we null-check each one.
//

void renderer__shutdown(void)
{
    //
    //solid-rect pipeline objects.
    //
    if (_opengl3_renderer_internal__g.vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.vbo);
    }
    if (_opengl3_renderer_internal__g.vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.vao);
    }
    if (_opengl3_renderer_internal__g.program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.program);
    }
    if (_opengl3_renderer_internal__g.cpu_verts != NULL)
    {
        GUI_FREE(_opengl3_renderer_internal__g.cpu_verts);
    }

    //
    //text pipeline objects. atlas textures are owned by font.c; it
    //destroys them via renderer__destroy_atlas before we get here.
    //
    if (_opengl3_renderer_internal__g.text_vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.text_vbo);
    }
    if (_opengl3_renderer_internal__g.text_vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.text_vao);
    }
    if (_opengl3_renderer_internal__g.text_program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.text_program);
    }
    if (_opengl3_renderer_internal__g.text_cpu_verts != NULL)
    {
        GUI_FREE(_opengl3_renderer_internal__g.text_cpu_verts);
    }

    //
    //image pipeline (lazy-built; guards against "never used" case).
    //RGBA textures created via renderer__create_texture_rgba are owned
    //by whichever widget called it; widget_image::on_destroy calls
    //renderer__destroy_texture. No teardown loop here.
    //
    if (_opengl3_renderer_internal__g.image_vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.image_vbo);
    }
    if (_opengl3_renderer_internal__g.image_vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.image_vao);
    }
    if (_opengl3_renderer_internal__g.image_program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.image_program);
    }

    //
    //tear down GL context. wglMakeCurrent(NULL, NULL) unbinds it from
    //this thread first -- required before deletion or the driver may
    //complain / leak.
    //
    if (_opengl3_renderer_internal__g.hglrc != NULL)
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(_opengl3_renderer_internal__g.hglrc);
    }
    if (_opengl3_renderer_internal__g.hdc != NULL)
    {
        ReleaseDC(_opengl3_renderer_internal__g.hwnd, _opengl3_renderer_internal__g.hdc);
    }

    memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));
}

//============================================================================
//PUBLIC: renderer__begin_frame
//============================================================================

void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear)
{
    _opengl3_renderer_internal__g.viewport_w      = viewport_w;
    _opengl3_renderer_internal__g.viewport_h      = viewport_h;
    _opengl3_renderer_internal__g.vert_count      = 0;
    _opengl3_renderer_internal__g.text_vert_count = 0;
    _opengl3_renderer_internal__g.text_run_count  = 0;
    //
    //current_text_atlas is intentionally preserved across frames -- the
    //first font__draw call each frame rebinds it anyway. runs, though,
    //reset every frame: a run belongs to the frame that produced it.
    //

    //
    //the viewport is the rectangle (in pixels, bottom-left origin here)
    //that gl_Position values in NDC get mapped into.
    //
    glViewport(0, 0, (GLsizei)viewport_w, (GLsizei)viewport_h);

    //
    // Set the color glClear should fill the back buffer with.
    //
    // sRGB framebuffer subtlety: with GL_FRAMEBUFFER_SRGB enabled, the
    // hardware treats glClearColor values as LINEAR and encodes them
    // to sRGB on store. Our gui_color values come from scene__hex /
    // scene__rgba, which produce sRGB-space channel values (0.039 for
    // hex 0x0a, etc.). We linearize them first so the cleared FB
    // actually lands at the intended sRGB color rather than at a much
    // brighter value.
    //
    // pow(x, 2.2) is a fast approximation of the sRGB transfer curve.
    // Error < 0.01 across 0..1, well below 8-bit quantization.
    // Alpha is unchanged; sRGB encoding only applies to color.
    //
    if (_opengl3_renderer_internal__g.srgb_fb)
    {
        float lr = powf(clear.r, 2.2f);
        float lg = powf(clear.g, 2.2f);
        float lb = powf(clear.b, 2.2f);
        glClearColor(lr, lg, lb, clear.a);
    }
    else
    {
        glClearColor(clear.r, clear.g, clear.b, clear.a);
    }

    //
    //do the actual clear.
    //
    glClear(GL_COLOR_BUFFER_BIT);
}

//============================================================================
//PUBLIC: renderer__submit_rect
//============================================================================
//
//appends 6 vertices (one quad, two triangles) to the cpu-side staging
//buffer. does NOT touch the GPU -- that happens once at end_frame.
//

//
// Shadow: cheap multi-pass approximation. Emit `layers` concentric
// rects, each larger than the last by `blur / layers` pixels and
// with alpha = shadow.a * (1 - t)^2 where t = layer/(layers). The
// SDF rounded-box in the fragment shader smooths each layer's own
// edge; stacking them with decreasing alpha produces a visually
// acceptable feather without needing a separate blur shader.
//
// Not mathematically accurate (a real Gaussian would be smoother),
// but good enough for UI shadows and doesn't require any shader
// changes. Layer count scales with blur radius; 1px blur = 2 layers,
// 20px blur = 8 layers. Capped at 8 for performance.
//
void renderer__submit_shadow(gui_rect rect, gui_color c, float radius, float blur)
{
    if (c.a <= 0.0f)
    {
        return;
    }
    int layers = (int)(blur * 0.4f) + 2;
    if (layers < 2) { layers = 2; }
    if (layers > 8) { layers = 8; }
    float step = blur / (float)layers;
    for (int i = 0; i < layers; i++)
    {
        float grow = step * (float)i;
        float t = (float)i / (float)(layers - 1);
        //
        // quadratic falloff: inner layer opaque, outer layer faded.
        // produces a softer tail than linear.
        //
        float falloff = (1.0f - t) * (1.0f - t);
        gui_color layer_c = c;
        layer_c.a = c.a * falloff / (float)layers;
        gui_rect lr;
        lr.x = rect.x - grow;
        lr.y = rect.y - grow;
        lr.w = rect.w + 2.0f * grow;
        lr.h = rect.h + 2.0f * grow;
        renderer__submit_rect(lr, layer_c, radius + grow);
    }
}

//
// Two-color linear gradient rect. The existing solid-rect vertex
// format already carries per-vertex color; the fragment shader
// interpolates it across the triangle (`v_color`). So a gradient
// is just a submit_rect call with different color per corner,
// picked by direction.
//
void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius)
{
    if (_opengl3_renderer_internal__g.vert_count + _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _opengl3_renderer_internal__g.vert_cap)
    {
        return;
    }

    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f) { radius = 0.0f; }
    if (radius > max_r) { radius = max_r; }

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    //
    // Pick corner colors from direction.
    //   VERTICAL:    top row = from,    bottom row = to.
    //   HORIZONTAL:  left col = from,   right col  = to.
    //   DIAGONAL_TL: top-left = from,   bottom-right = to; midpoints interpolated halfway.
    //   DIAGONAL_TR: top-right = from,  bottom-left  = to.
    //
    gui_color c_tl = from, c_tr = from, c_bl = to, c_br = to;
    if (direction == (int)GUI_GRADIENT_HORIZONTAL)
    {
        c_tl = from; c_bl = from;
        c_tr = to;   c_br = to;
    }
    else if (direction == (int)GUI_GRADIENT_DIAGONAL_TL)
    {
        c_tl = from;
        c_br = to;
        //
        // Mid-corners at 0.5 lerp.
        //
        c_tr.r = (from.r + to.r) * 0.5f;
        c_tr.g = (from.g + to.g) * 0.5f;
        c_tr.b = (from.b + to.b) * 0.5f;
        c_tr.a = (from.a + to.a) * 0.5f;
        c_bl = c_tr;
    }
    else if (direction == (int)GUI_GRADIENT_DIAGONAL_TR)
    {
        c_tr = from;
        c_bl = to;
        c_tl.r = (from.r + to.r) * 0.5f;
        c_tl.g = (from.g + to.g) * 0.5f;
        c_tl.b = (from.b + to.b) * 0.5f;
        c_tl.a = (from.a + to.a) * 0.5f;
        c_br = c_tl;
    }
    //
    // VERTICAL is the default case already set above.
    //

    _opengl3_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c_tl);
    _opengl3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _opengl3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
    _opengl3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _opengl3_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c_br);
    _opengl3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
}

void renderer__submit_rect(gui_rect r, gui_color c, float radius)
{
    if (_opengl3_renderer_internal__g.vert_count + _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _opengl3_renderer_internal__g.vert_cap)
    {
        return;
    }

    //
    //clamp radius. the SDF math goes nuts if radius > min(w,h)/2 (the
    //corners would overlap and produce garbage). this clamp MUST match
    //every other backend (renderer.h VISUAL CONTRACT).
    //
    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f)
    {
        radius = 0.0f;
    }
    if (radius > max_r)
    {
        radius = max_r;
    }

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    //
    //triangle 1: (x0,y0) (x1,y0) (x0,y1)
    //triangle 2: (x1,y0) (x1,y1) (x0,y1)
    //
    //                       x   y   lx    ly    rect_w  rect_h  radius  color
    _opengl3_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w,    r.h,    radius, c);  // top-left
    _opengl3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w,    r.h,    radius, c);  // top-right
    _opengl3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w,    r.h,    radius, c);  // bottom-left
    _opengl3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w,    r.h,    radius, c);  // top-right
    _opengl3_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w,    r.h,    radius, c);  // bottom-right
    _opengl3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w,    r.h,    radius, c);  // bottom-left
}

//============================================================================
//INTERNAL: flush_batches
//============================================================================
//
//Draw whatever is currently buffered (solid rects + text glyphs), then
//reset the buffer counts so subsequent submissions start a fresh batch.
//Used in two places: (1) end_frame, to draw the final batch before
//SwapBuffers; (2) push_scissor / pop_scissor, to make sure the rects
//submitted before the scissor change get drawn under the OLD scissor
//state, not the new one.
//
//Does NOT call SwapBuffers -- that's end_frame's job.
//

static void _opengl3_renderer_internal__flush_batches(void)
{
    //
    //PASS 1: solid rects (SDF rounded boxes). drawn first so text lands
    //on top of backgrounds in the same flush.
    //
    if (_opengl3_renderer_internal__g.vert_count > 0)
    {
        glUseProgram(_opengl3_renderer_internal__g.program);
        glUniform2f(_opengl3_renderer_internal__g.u_viewport_loc,
                    (float)_opengl3_renderer_internal__g.viewport_w,
                    (float)_opengl3_renderer_internal__g.viewport_h);

        glBindVertexArray(_opengl3_renderer_internal__g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.vbo);

        //
        // Buffer ORPHANING (critical for mid-frame multi-flush correctness):
        // We call glBufferData with NULL data first, which tells the driver
        // "throw away the previous contents -- I don't need them anymore,
        // give me fresh GPU memory". Then glBufferSubData uploads our new
        // verts into that fresh memory.
        //
        // Without this, when push_scissor / pop_scissor causes multiple
        // flushes per frame, the SECOND glBufferSubData would have to wait
        // for the FIRST glDrawArrays to finish reading from the buffer
        // before it could safely overwrite -- or worse, on some drivers
        // it would NOT wait properly and produce flicker as the second
        // upload races the first draw. (D3D11's MAP_WRITE_DISCARD and
        // D3D9's D3DLOCK_DISCARD are the equivalent "orphan" hints --
        // the d3d backends never had this bug.)
        //
        // The "orphan" cost is negligible: drivers internally pool old
        // memory and recycle it once the GPU is done. A typical driver
        // handles thousands of orphans per second without breaking a sweat.
        //
        GLsizeiptr bytes = (GLsizeiptr)_opengl3_renderer_internal__g.vert_count * (GLsizeiptr)sizeof(_opengl3_renderer_internal__vertex);
        GLsizeiptr cap_bytes = (GLsizeiptr)_opengl3_renderer_internal__g.vert_cap * (GLsizeiptr)sizeof(_opengl3_renderer_internal__vertex);
        glBufferData(GL_ARRAY_BUFFER, cap_bytes, NULL, GL_DYNAMIC_DRAW);   // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, _opengl3_renderer_internal__g.cpu_verts);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_opengl3_renderer_internal__g.vert_count);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);

        _opengl3_renderer_internal__g.vert_count = 0;
    }

    //
    //PASS 2: text. one draw per atlas-run so a flush can mix fonts of
    //different sizes (each (family, size) rasterizes into its own
    //atlas texture). all runs share the same VAO/VBO/program; only
    //the bound texture changes between runs.
    //
    if (_opengl3_renderer_internal__g.text_vert_count > 0 &&
        _opengl3_renderer_internal__g.text_run_count > 0)
    {
        glUseProgram(_opengl3_renderer_internal__g.text_program);
        glUniform2f(_opengl3_renderer_internal__g.text_u_viewport_loc,
                    (float)_opengl3_renderer_internal__g.viewport_w,
                    (float)_opengl3_renderer_internal__g.viewport_h);

        glActiveTexture(GL_TEXTURE0);
        if (_opengl3_renderer_internal__g.text_u_atlas_loc >= 0)
        {
            glUniform1i(_opengl3_renderer_internal__g.text_u_atlas_loc, 0);
        }

        glBindVertexArray(_opengl3_renderer_internal__g.text_vao);
        glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.text_vbo);

        //
        // Same orphan trick as the solid-rect pass above.
        //
        GLsizeiptr tbytes = (GLsizeiptr)_opengl3_renderer_internal__g.text_vert_count * (GLsizeiptr)sizeof(_opengl3_renderer_internal__text_vertex);
        GLsizeiptr tcap_bytes = (GLsizeiptr)_opengl3_renderer_internal__g.text_vert_cap * (GLsizeiptr)sizeof(_opengl3_renderer_internal__text_vertex);
        glBufferData(GL_ARRAY_BUFFER, tcap_bytes, NULL, GL_DYNAMIC_DRAW);   // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, tbytes, _opengl3_renderer_internal__g.text_cpu_verts);

        for (int64 i = 0; i < _opengl3_renderer_internal__g.text_run_count; i++)
        {
            _opengl3_renderer_internal__text_run* run = &_opengl3_renderer_internal__g.text_runs[i];
            if (run->vert_count <= 0)
            {
                continue;
            }
            glBindTexture(GL_TEXTURE_2D, run->atlas);
            glDrawArrays(GL_TRIANGLES, (GLint)run->vert_start, (GLsizei)run->vert_count);
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        _opengl3_renderer_internal__g.text_vert_count = 0;
        _opengl3_renderer_internal__g.text_run_count  = 0;
        _opengl3_renderer_internal__g.current_text_atlas = 0;
    }
}

//============================================================================
//PUBLIC: renderer__push_scissor / renderer__pop_scissor
//============================================================================
//
//Bracket a region of submissions whose draws are clipped to the given
//rect. Each push/pop forces a flush of whatever is currently batched,
//then changes GL scissor state. Subsequent submissions accumulate into
//a fresh batch that will be drawn under the new state.
//
//OpenGL's glScissor uses BOTTOM-LEFT-origin coordinates, so we flip Y
//from our top-left convention: gl_y = viewport_h - top_y - height.
//
//Nesting takes the INTERSECTION of all stacked rects. A scrollable
//child div inside a scrollable parent div correctly clips to the
//smaller of the two regions.
//

static gui_rect _opengl3_renderer_internal__intersect(gui_rect a, gui_rect b)
{
    float x0 = (a.x > b.x) ? a.x : b.x;
    float y0 = (a.y > b.y) ? a.y : b.y;
    float x1a = a.x + a.w;
    float y1a = a.y + a.h;
    float x1b = b.x + b.w;
    float y1b = b.y + b.h;
    float x1 = (x1a < x1b) ? x1a : x1b;
    float y1 = (y1a < y1b) ? y1a : y1b;
    gui_rect r;
    r.x = x0;
    r.y = y0;
    r.w = (x1 > x0) ? (x1 - x0) : 0.0f;
    r.h = (y1 > y0) ? (y1 - y0) : 0.0f;
    return r;
}

static void _opengl3_renderer_internal__apply_scissor_top(void)
{
    //
    //Compute the intersection of every rect on the stack. The result
    //is what the GPU's scissor test will use until the next change.
    //
    if (_opengl3_renderer_internal__g.scissor_depth <= 0)
    {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    gui_rect r = _opengl3_renderer_internal__g.scissor_stack[0];
    for (int64 i = 1; i < _opengl3_renderer_internal__g.scissor_depth; i++)
    {
        r = _opengl3_renderer_internal__intersect(r, _opengl3_renderer_internal__g.scissor_stack[i]);
    }
    GLint   gl_x = (GLint)r.x;
    GLint   gl_y = (GLint)((float)_opengl3_renderer_internal__g.viewport_h - r.y - r.h);
    GLsizei gl_w = (GLsizei)r.w;
    GLsizei gl_h = (GLsizei)r.h;
    if (gl_w < 0) { gl_w = 0; }
    if (gl_h < 0) { gl_h = 0; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(gl_x, gl_y, gl_w, gl_h);
}

void renderer__push_scissor(gui_rect rect)
{
    _opengl3_renderer_internal__flush_batches();
    if (_opengl3_renderer_internal__g.scissor_depth >= (int64)(sizeof(_opengl3_renderer_internal__g.scissor_stack) / sizeof(_opengl3_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    _opengl3_renderer_internal__g.scissor_stack[_opengl3_renderer_internal__g.scissor_depth++] = rect;
    _opengl3_renderer_internal__apply_scissor_top();
}

void renderer__pop_scissor(void)
{
    _opengl3_renderer_internal__flush_batches();
    if (_opengl3_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("renderer__pop_scissor: stack already empty");
        return;
    }
    _opengl3_renderer_internal__g.scissor_depth--;
    _opengl3_renderer_internal__apply_scissor_top();
}

//
// Real backdrop blur for opengl3. glReadPixels the backbuffer region,
// run a CPU-side separable Gaussian over the captured pixels, upload
// the result as a temp RGBA8 texture, draw it back at the same rect.
// Not the fastest possible implementation (GPU-side FBO ping-pong
// would beat it), but this path has no new shader code and drops in
// on any GL context that can glReadPixels -- which is every desktop
// GL 3.x profile. Rect sizes typical for UI panels (200-600 px wide,
// 200-400 px tall) keep the per-frame cost low enough to not register
// on a frame timer at sigma <= 16.
//
// The other backends keep the translucent-darken placeholder until
// they gain a comparable path (each has its own readback + temp-
// texture machinery to wire up).
//
void renderer__blur_region(gui_rect rect, float sigma_px)
{
    if (sigma_px <= 0.0f)           { return; }
    if (rect.w <= 0.0f || rect.h <= 0.0f) { return; }

    //
    // Flush anything batched so the backbuffer reflects every
    // submission that happened BEFORE this blur call. Otherwise the
    // pixels we read back would be stale ("before the bg of the
    // div that the blur is inside" instead of "including it").
    //
    _opengl3_renderer_internal__flush_batches();

    int vw = (int)_opengl3_renderer_internal__g.viewport_w;
    int vh = (int)_opengl3_renderer_internal__g.viewport_h;

    //
    // Clamp the blur rect to the viewport. Going past the edges
    // would feed glReadPixels coordinates outside the framebuffer
    // (undefined pixel values, often black) and would waste work on
    // regions that won't be composited.
    //
    int x = (int)rect.x;
    int y = (int)rect.y;
    int w = (int)rect.w;
    int h = (int)rect.h;
    if (x < 0)       { w += x; x = 0; }
    if (y < 0)       { h += y; y = 0; }
    if (x + w > vw)  { w = vw - x; }
    if (y + h > vh)  { h = vh - y; }
    if (w <= 0 || h <= 0) { return; }

    //
    // GL's framebuffer origin is bottom-left, ours is top-left, so the
    // readback Y is mirrored. glReadPixels returns rows starting at the
    // requested y and going UP; that means the FIRST row corresponds
    // to the BOTTOM of our screen rect. We'll flip the pixel buffer
    // back to top-first before uploading so the regular image UV
    // convention (V=0 at top) lines up.
    //
    int gl_y = vh - (y + h);

    size_t pxbytes = (size_t)w * (size_t)h * 4;
    ubyte* pixels = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE_RENDERER);
    ubyte* tmp    = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE_RENDERER);
    if (pixels == NULL || tmp == NULL)
    {
        if (pixels) { GUI_FREE(pixels); }
        if (tmp)    { GUI_FREE(tmp); }
        //
        // OOM for the blur buffers: fall back to the darken placeholder
        // so at least something renders.
        //
        gui_color dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, gl_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    //
    // Build the Gaussian kernel. Radius = ceil(3 * sigma) covers
    // ~99.7 % of the distribution's weight; bigger adds cost with no
    // visible benefit. Capped at 32 so extreme sigma values don't
    // blow up kernel width beyond reason.
    //
    int ksize = (int)(sigma_px * 3.0f);
    if (ksize < 1)  { ksize = 1; }
    if (ksize > 32) { ksize = 32; }
    int kw = ksize * 2 + 1;

    float kernel_stack[65]; // enough for ksize = 32 (kw = 65).
    float ksum = 0.0f;
    float two_sigma_sq = 2.0f * sigma_px * sigma_px;
    for (int i = 0; i < kw; i++)
    {
        float d = (float)(i - ksize);
        kernel_stack[i] = stdlib__expf(-(d * d) / two_sigma_sq);
        ksum += kernel_stack[i];
    }
    for (int i = 0; i < kw; i++) { kernel_stack[i] /= ksum; }

    //
    // Horizontal pass: pixels -> tmp. Edge pixels clamp to the nearest
    // valid column (CLAMP_TO_EDGE behaviour). Cheaper than mirrored
    // or wrapped edges and produces a stable-looking edge for UI
    // rects against the viewport boundary.
    //
    for (int py = 0; py < h; py++)
    {
        ubyte* dst_row = tmp    + (size_t)py * (size_t)w * 4;
        ubyte* src_row = pixels + (size_t)py * (size_t)w * 4;
        for (int px = 0; px < w; px++)
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int k = 0; k < kw; k++)
            {
                int sx = px + (k - ksize);
                if (sx < 0)   { sx = 0;     }
                if (sx >= w)  { sx = w - 1; }
                ubyte* s = src_row + (size_t)sx * 4;
                float kv = kernel_stack[k];
                r += (float)s[0] * kv;
                g += (float)s[1] * kv;
                b += (float)s[2] * kv;
                a += (float)s[3] * kv;
            }
            ubyte* d = dst_row + (size_t)px * 4;
            d[0] = (ubyte)r;
            d[1] = (ubyte)g;
            d[2] = (ubyte)b;
            d[3] = (ubyte)a;
        }
    }

    //
    // Vertical pass: tmp -> pixels. Same kernel, stride the OTHER way.
    //
    for (int py = 0; py < h; py++)
    {
        for (int px = 0; px < w; px++)
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int k = 0; k < kw; k++)
            {
                int sy = py + (k - ksize);
                if (sy < 0)  { sy = 0;     }
                if (sy >= h) { sy = h - 1; }
                ubyte* s = tmp + ((size_t)sy * (size_t)w + (size_t)px) * 4;
                float kv = kernel_stack[k];
                r += (float)s[0] * kv;
                g += (float)s[1] * kv;
                b += (float)s[2] * kv;
                a += (float)s[3] * kv;
            }
            ubyte* d = pixels + ((size_t)py * (size_t)w + (size_t)px) * 4;
            d[0] = (ubyte)r;
            d[1] = (ubyte)g;
            d[2] = (ubyte)b;
            d[3] = (ubyte)a;
        }
    }

    //
    // Flip rows top-for-bottom so the buffer becomes "top row first"
    // (screen order). tmp is free to reuse as the swap scratch row.
    //
    for (int py = 0; py < h / 2; py++)
    {
        ubyte* row_a = pixels + (size_t)py         * (size_t)w * 4;
        ubyte* row_b = pixels + (size_t)(h-1-py)   * (size_t)w * 4;
        stdlib__memcpy(tmp,   row_a, (size_t)w * 4);
        stdlib__memcpy(row_a, row_b, (size_t)w * 4);
        stdlib__memcpy(row_b, tmp,   (size_t)w * 4);
    }

    //
    // Upload as a throw-away RGBA8 texture and draw it at the blur
    // rect using the existing image pipeline. Force no mipmapping
    // (single level) since we're rendering 1:1 and mipmap generation
    // would be wasted cost. GL refcount-defers the deletion until
    // the draw finishes, so destroying immediately after the draw
    // call is safe per the GL spec.
    //
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    //
    // Draw at the (possibly clamped) rect. Tint = white so the image
    // displays at full opacity. submit_image flushes any pending
    // batches itself, then draws the textured quad.
    //
    gui_rect draw_rect;
    draw_rect.x = (float)x;
    draw_rect.y = (float)y;
    draw_rect.w = (float)w;
    draw_rect.h = (float)h;
    gui_color white = { 1.0f, 1.0f, 1.0f, 1.0f };
    renderer__submit_image(draw_rect, (void*)(uintptr_t)tex, white);

    glDeleteTextures(1, &tex);
    GUI_FREE(tmp);
    GUI_FREE(pixels);
}

//============================================================================
//PUBLIC: renderer__end_frame
//============================================================================
//
//Flushes any remaining batched submissions, then SwapBuffers to promote
//the back buffer to front (= to screen). Also defensively resets the
//scissor stack -- a missing pop_scissor in widget code would otherwise
//leak into the next frame.
//

void renderer__flush_pending_draws(void)
{
    // Public thin wrapper over the internal flush so scene_render can
    // break the per-frame batch at z-index sibling boundaries. See
    // renderer.h for why this matters for text+z interleaving.
    _opengl3_renderer_internal__flush_batches();
}

void renderer__end_frame(void)
{
    _opengl3_renderer_internal__flush_batches();

    //
    //defensive scissor cleanup: if a widget pushed but failed to pop,
    //the next frame would inherit the stale scissor. clear and disable.
    //
    if (_opengl3_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("renderer__end_frame: scissor stack non-empty (%lld); resetting", (long long)_opengl3_renderer_internal__g.scissor_depth);
        _opengl3_renderer_internal__g.scissor_depth = 0;
    }
    glDisable(GL_SCISSOR_TEST);

    //
    //present. SwapBuffers takes the just-rendered back buffer and makes
    //it the visible front buffer. this is what actually puts pixels on
    //screen; everything above this line only wrote to an offscreen
    //buffer. even when both passes were skipped (empty frame), we still
    //present so the clear color reaches the display.
    //
    SwapBuffers(_opengl3_renderer_internal__g.hdc);
}

//============================================================================
//INTERNAL: context creation
//============================================================================

//
//pick a pixel format and set it on the HDC. this establishes the back
//buffer's properties (RGBA8, 24-bit depth + 8-bit stencil, double-buffered).
//must happen before any context creation. irreversible -- the window is
//locked into this format after SetPixelFormat.
//
//
// Build the legacy PIXELFORMATDESCRIPTOR we use both for the sRGB
// path's DescribePixelFormat fill-in and for the non-sRGB fallback's
// ChoosePixelFormat call. Single source of truth for the fields.
//
static void _opengl3_renderer_internal__fill_legacy_pfd(PIXELFORMATDESCRIPTOR* pfd)
{
    ZeroMemory(pfd, sizeof(*pfd));
    pfd->nSize        = sizeof(*pfd);
    pfd->nVersion     = 1;
    pfd->dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType   = PFD_TYPE_RGBA;
    pfd->cColorBits   = 32;
    //
    // cAlphaBits = 0 (NOT 8). The Windows DWM compositor treats a
    // non-zero alpha channel in the framebuffer as window transparency
    // and samples it during composition. Even though our blend func
    // (glBlendFuncSeparate with GL_ONE / GL_ONE_MINUS_SRC_ALPHA on the
    // alpha channel) keeps dst.a pinned to the cleared value of 1.0
    // in steady state, intermediate draws within a frame can briefly
    // produce dst.a values that DWM samples mid-composition, producing
    // visible flicker as our :appear animation modulates per-frame
    // alpha. Setting cAlphaBits = 0 makes DWM completely ignore the
    // framebuffer alpha and treat the surface as opaque -- which is
    // what we want, since the toolkit doesn't render to a transparent
    // window. The d3d11 / d3d9 backends use DXGI_FORMAT_R8G8B8A8_UNORM /
    // D3DFMT_A8R8G8B8 (both have alpha) but their swap chains don't
    // hand the alpha to DWM the same way -- which is why those
    // backends don't show this bug.
    //
    pfd->cAlphaBits   = 0;
    pfd->cDepthBits   = 24;
    pfd->cStencilBits = 8;
    pfd->iLayerType   = PFD_MAIN_PLANE;
}

//
// Resolve wglChoosePixelFormatARB. The extension is only queryable
// while a GL context is current, and a GL context requires a pixel
// format to already be set on its HDC -- chicken and egg. The
// idiomatic workaround: stand up a COMPLETELY SEPARATE throwaway
// window with a legacy pixel format + legacy 2.x context just long
// enough to call wglGetProcAddress, then tear the whole thing down.
// The main window's HDC is untouched, so SetPixelFormat can be called
// on it with whatever format wglChoosePixelFormatARB returned.
//
// Returns NULL if the extension isn't available (very old drivers).
// Caller falls back to the plain ChoosePixelFormat path in that case.
//
static fncp_wglChoosePixelFormatARB _opengl3_renderer_internal__resolve_wgl_pixel_format_arb(void)
{
    //
    // Register the dummy class exactly once per process. Two reasons:
    //   1. If renderer__init is called twice (re-init after shutdown),
    //      a fresh RegisterClassW on a previously-registered class
    //      fails with ERROR_CLASS_ALREADY_EXISTS and we'd bail early.
    //   2. UnregisterClassW can fail if ANY window of that class is
    //      still alive, which isn't a guarantee we can make cleanly
    //      while the GL context is being stood up. Keeping the class
    //      alive for the process lifetime avoids the race.
    // The leak is ~one WNDCLASS entry per process -- negligible.
    //
    static ATOM          s_dummy_class_atom   = 0;
    static const wchar_t s_dummy_class_name[] = L"GuiOpenGL3DummyClass";

    if (s_dummy_class_atom == 0)
    {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.lpszClassName = s_dummy_class_name;
        s_dummy_class_atom = RegisterClassW(&wc);
        if (s_dummy_class_atom == 0)
        {
            //
            // Non-fatal; just means we can't resolve the ARB extension.
            // Caller falls back to the legacy ChoosePixelFormat path.
            //
            return NULL;
        }
    }

    fncp_wglChoosePixelFormatARB result = NULL;

    HWND dummy_hwnd = CreateWindowW(
        s_dummy_class_name, L"",
        WS_OVERLAPPEDWINDOW,
        0, 0, 1, 1,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (dummy_hwnd == NULL)
    {
        return NULL;
    }

    HDC dummy_hdc = GetDC(dummy_hwnd);
    if (dummy_hdc != NULL)
    {
        PIXELFORMATDESCRIPTOR pfd;
        _opengl3_renderer_internal__fill_legacy_pfd(&pfd);
        int pf = ChoosePixelFormat(dummy_hdc, &pfd);
        if (pf != 0 && SetPixelFormat(dummy_hdc, pf, &pfd))
        {
            HGLRC dummy_ctx = wglCreateContext(dummy_hdc);
            if (dummy_ctx != NULL)
            {
                if (wglMakeCurrent(dummy_hdc, dummy_ctx))
                {
                    result = (fncp_wglChoosePixelFormatARB)wglGetProcAddress("wglChoosePixelFormatARB");
                    wglMakeCurrent(NULL, NULL);
                }
                wglDeleteContext(dummy_ctx);
            }
        }
        ReleaseDC(dummy_hwnd, dummy_hdc);
    }

    DestroyWindow(dummy_hwnd);
    return result;
}

//
// Select a pixel format for the main HDC. Preferred path is
// wglChoosePixelFormatARB with WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB = TRUE,
// which gives us an sRGB default framebuffer -- the hardware then
// does linear->sRGB on every pixel write and blending happens in
// physically-correct linear space. Fallback path is the legacy
// ChoosePixelFormat with the same PFD as before this refactor; the
// renderer then stays on non-sRGB blending (same pixel output as
// pre-refactor builds).
//
// _opengl3_renderer_internal__g.srgb_fb is set to TRUE only when the
// ARB path succeeded AND DescribePixelFormat + SetPixelFormat also
// succeeded. Everywhere else in the renderer reads that flag to
// decide whether to linearize clear colors, enable GL_FRAMEBUFFER_SRGB,
// and which shader variant to compile.
//
static boole _opengl3_renderer_internal__set_pixel_format(HDC hdc)
{
    fncp_wglChoosePixelFormatARB wglChoosePixelFormatARB_p = _opengl3_renderer_internal__resolve_wgl_pixel_format_arb();

    if (wglChoosePixelFormatARB_p != NULL)
    {
        //
        // Attribute list mirrors the legacy PFD's fields:
        // accelerated + double-buffered + RGBA + 24 color / 0 alpha /
        // 24 depth / 8 stencil + sRGB-capable.
        //
        // Ending in 0 (not two 0s; wgl terminator is a single int
        // entry) stops the driver from reading past our array.
        //
        int attribs[] = {
            WGL_DRAW_TO_WINDOW_ARB,           1,
            WGL_SUPPORT_OPENGL_ARB,           1,
            WGL_DOUBLE_BUFFER_ARB,            1,
            WGL_ACCELERATION_ARB,             WGL_FULL_ACCELERATION_ARB,
            WGL_PIXEL_TYPE_ARB,               WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB,               24,
            WGL_ALPHA_BITS_ARB,               0,
            WGL_DEPTH_BITS_ARB,               24,
            WGL_STENCIL_BITS_ARB,             8,
            WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, 1,
            0
        };

        int  pf  = 0;
        UINT num = 0;
        if (wglChoosePixelFormatARB_p(hdc, attribs, NULL, 1, &pf, &num) && num > 0)
        {
            //
            // DescribePixelFormat fills the PFD fields SetPixelFormat
            // actually reads (most are advisory; the pixel-format index
            // is what binds). Required by the SetPixelFormat API even
            // when the format was picked by the ARB extension.
            //
            PIXELFORMATDESCRIPTOR pfd;
            ZeroMemory(&pfd, sizeof(pfd));
            pfd.nSize    = sizeof(pfd);
            pfd.nVersion = 1;
            DescribePixelFormat(hdc, pf, sizeof(pfd), &pfd);

            if (SetPixelFormat(hdc, pf, &pfd))
            {
                _opengl3_renderer_internal__g.srgb_fb = TRUE;
                log_info("opengl3: sRGB-capable pixel format selected (index %d)", pf);
                return TRUE;
            }
            _opengl3_renderer_internal__log_win32_error("SetPixelFormat (sRGB)");
            //
            // fall through to legacy path; don't bail -- maybe the
            // legacy ChoosePixelFormat can still give us something.
            //
        }
        else
        {
            log_warn("opengl3: wglChoosePixelFormatARB returned no sRGB format; falling back to legacy");
        }
    }

    //
    // Legacy fallback: no sRGB. Same PFD + ChoosePixelFormat path as
    // before this refactor. srgb_fb stays FALSE.
    //
    PIXELFORMATDESCRIPTOR pfd;
    _opengl3_renderer_internal__fill_legacy_pfd(&pfd);
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (pf == 0)
    {
        _opengl3_renderer_internal__log_win32_error("ChoosePixelFormat");
        return FALSE;
    }
    if (!SetPixelFormat(hdc, pf, &pfd))
    {
        _opengl3_renderer_internal__log_win32_error("SetPixelFormat");
        return FALSE;
    }
    log_info("opengl3: legacy (non-sRGB) pixel format selected (index %d); text will use pow-coverage gamma approximation", pf);
    return TRUE;
}

//
//the dummy-context dance. a "dummy" 2.x context is created first,
//made current, used to resolve wglCreateContextAttribsARB, then
//thrown away once we have the real 3.3 core context. gross but
//that's how wgl works.
//
static boole _opengl3_renderer_internal__create_core_context(HDC hdc, HGLRC* out_ctx)
{
    HGLRC dummy = wglCreateContext(hdc);
    if (dummy == NULL)
    {
        _opengl3_renderer_internal__log_win32_error("wglCreateContext (dummy)");
        return FALSE;
    }
    if (!wglMakeCurrent(hdc, dummy))
    {
        _opengl3_renderer_internal__log_win32_error("wglMakeCurrent (dummy)");
        wglDeleteContext(dummy);
        return FALSE;
    }

    fncp_wglCreateContextAttribsARB wglCreateContextAttribsARB = (fncp_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB == NULL)
    {
        //
        //driver doesn't expose the arb extension. fall back to the
        //legacy context -- still usable, though some 3.3-only calls
        //may not work.
        //
        log_warn("wglCreateContextAttribsARB not available; using legacy context");
        *out_ctx = dummy;
        wglMakeCurrent(NULL, NULL);
        return TRUE;
    }

    const int attribs[] =
    {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    HGLRC real = wglCreateContextAttribsARB(hdc, NULL, attribs);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(dummy);

    if (real == NULL)
    {
        _opengl3_renderer_internal__log_win32_error("wglCreateContextAttribsARB");
        return FALSE;
    }
    *out_ctx = real;
    return TRUE;
}

//
//resolve every GL 3.x function pointer via wglGetProcAddress. gl 1.1
//functions (glClear, glViewport etc.) are linked statically against
//opengl32.lib and don't need this. returns FALSE on any missing entry
//point -- that indicates a broken driver.
//
static boole _opengl3_renderer_internal__load_gl_functions(void)
{
    boole ok = TRUE;

    //
    //helper macro: given an identifier `name`, stringify it for
    //wglGetProcAddress, and build the matching extern pointer /
    //typedef names via ## token-paste.
    //
    #define LOAD(name)                                                           \
        do                                                                       \
        {                                                                        \
            p_##name = (fncp_##name)wglGetProcAddress(#name);                    \
            if (p_##name == NULL)                                                \
            {                                                                    \
                log_error("wglGetProcAddress failed for %s", #name);                     \
                ok = FALSE;                                                      \
            }                                                                    \
        } while (0)

    LOAD(glCreateShader);
    LOAD(glDeleteShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);

    LOAD(glCreateProgram);
    LOAD(glDeleteProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram);

    LOAD(glGetUniformLocation);
    LOAD(glUniform2f);
    LOAD(glUniform1i);
    LOAD(glActiveTexture);

    LOAD(glGenBuffers);
    LOAD(glDeleteBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glBufferSubData);

    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glDeleteVertexArrays);
    LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray);

    LOAD(glBlendFuncSeparate);
    LOAD(glGenerateMipmap);

    #undef LOAD

    return ok;
}

//============================================================================
//INTERNAL: pipeline (shader + VAO + VBO + cpu buffer)
//============================================================================
//
//once the GL context exists and function pointers are loaded, build the
//draw pipeline: compile + link shaders, create + configure the VAO/VBO,
//allocate the cpu staging buffer, enable alpha blending.
//

static boole _opengl3_renderer_internal__build_pipeline(void)
{
    //
    //compile each shader stage independently. Fragment shader source
    //depends on whether we negotiated an sRGB-capable framebuffer --
    //see shader source comments above for the two variants.
    //
    char* fs_src = _opengl3_renderer_internal__g.srgb_fb
                 ? _opengl3_renderer_internal__fs_src_srgb
                 : _opengl3_renderer_internal__fs_src;
    GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER, _opengl3_renderer_internal__vs_src);
    GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    //
    //link the two stages into a program. delete the shader objects
    //afterwards; the program has copied what it needs from them.
    //
    _opengl3_renderer_internal__g.program = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_opengl3_renderer_internal__g.program == 0)
    {
        return FALSE;
    }

    //
    //cache the u_viewport uniform's slot so we don't re-query each frame.
    //
    _opengl3_renderer_internal__g.u_viewport_loc =
        glGetUniformLocation(_opengl3_renderer_internal__g.program, "u_viewport");

    //
    //create + bind VAO and VBO. gl 3.3 core requires a VAO bound any
    //time you draw.
    //
    glGenVertexArrays(1, &_opengl3_renderer_internal__g.vao);
    glGenBuffers(1, &_opengl3_renderer_internal__g.vbo);

    glBindVertexArray(_opengl3_renderer_internal__g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.vbo);

    //
    //reserve the max buffer size up front. GL_DYNAMIC_DRAW tells the
    //driver we'll update this buffer often.
    //
    GLsizeiptr max_bytes = (GLsizeiptr)(_OPENGL3_RENDERER_INTERNAL__MAX_QUADS * _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_opengl3_renderer_internal__vertex);
    glBufferData(GL_ARRAY_BUFFER, max_bytes, NULL, GL_DYNAMIC_DRAW);

    //
    //describe the vertex layout to the GPU. one glVertexAttribPointer
    //per attribute in our vertex struct; must match the shader's
    //layout(location=N) declarations.
    //
    GLsizei stride = (GLsizei)sizeof(_opengl3_renderer_internal__vertex);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex, lx));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex, rect_w));

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex, radius));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    //
    //allocate the cpu-side staging buffer that mirrors the VBO.
    //
    _opengl3_renderer_internal__g.vert_cap = _OPENGL3_RENDERER_INTERNAL__MAX_QUADS * _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _opengl3_renderer_internal__g.cpu_verts = (_opengl3_renderer_internal__vertex*)GUI_MALLOC_T((size_t)_opengl3_renderer_internal__g.vert_cap * sizeof(_opengl3_renderer_internal__vertex), MM_TYPE_RENDERER);
    if (_opengl3_renderer_internal__g.cpu_verts == NULL)
    {
        log_error("out of memory for cpu vertex buffer");
        return FALSE;
    }

    //
    //alpha blending. required for our anti-aliased rounded corners
    //to fade cleanly into the background. this blend mode is the
    //VISUAL CONTRACT for all backends -- see renderer.h.
    //
    //
    // Alpha blending. We use glBlendFuncSeparate (not the simpler
    // glBlendFunc) so that the framebuffer's ALPHA channel stays at
    // 1.0 across overlapping draws -- if it dropped below 1.0, the
    // Windows DWM compositor would treat the OpenGL window as
    // semi-transparent and let the desktop bleed through, producing
    // visible flicker as our own alpha animations modulate dst.a.
    //
    //   RGB:    src.a   * src.rgb +  (1 - src.a) * dst.rgb    (classic alpha blend)
    //   Alpha:  1       * src.a   +  (1 - src.a) * dst.a       (preserves dst.a = 1)
    //
    // Matches d3d11's separate-alpha blend state. Part of the VISUAL
    // CONTRACT in renderer.h.
    //
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    //
    // Turn on hardware sRGB encoding for the default framebuffer when
    // we got an sRGB-capable pixel format. With this enabled, shader
    // fragment outputs are interpreted as LINEAR color values and the
    // GPU does the linear->sRGB conversion on write -- which means
    // alpha blending runs in linear space (physically correct) and
    // partial-coverage edges land at the perceptual midpoint instead
    // of the sRGB-space midpoint.
    //
    // Has no effect if the default FB isn't sRGB-capable (spec:
    // silently becomes a no-op), so the glEnable call alone wouldn't
    // hurt even in the fallback path -- we still guard it behind
    // srgb_fb to make the pairing with the shader variant explicit.
    //
    if (_opengl3_renderer_internal__g.srgb_fb)
    {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    //
    //the second pipeline: textured glyph quads. same blend state,
    //separate shader + VAO + VBO + cpu buffer.
    //
    if (!_opengl3_renderer_internal__build_text_pipeline())
    {
        return FALSE;
    }

    return TRUE;
}

//
//build_text_pipeline: compile the text shaders, link them, create the
//text VAO/VBO, and allocate the cpu-side staging buffer. mirrors the
//solid-rect pipeline structure exactly; only the shader sources and
//vertex layout differ.
//
static boole _opengl3_renderer_internal__build_text_pipeline(void)
{
    //
    // Text fragment shader variant depends on sRGB FB availability
    // just like the rect shader. Non-sRGB path keeps the legacy
    // pow(a, 1/2.2) coverage hack; sRGB path does sRGB->linear on
    // the vertex color and relies on the hardware for gamma.
    //
    char* fs_src = _opengl3_renderer_internal__g.srgb_fb
                 ? _opengl3_renderer_internal__text_fs_src_srgb
                 : _opengl3_renderer_internal__text_fs_src;
    GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   _opengl3_renderer_internal__text_vs_src);
    GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    _opengl3_renderer_internal__g.text_program = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_opengl3_renderer_internal__g.text_program == 0)
    {
        return FALSE;
    }

    _opengl3_renderer_internal__g.text_u_viewport_loc =
        glGetUniformLocation(_opengl3_renderer_internal__g.text_program, "u_viewport");
    _opengl3_renderer_internal__g.text_u_atlas_loc =
        glGetUniformLocation(_opengl3_renderer_internal__g.text_program, "u_atlas");

    glGenVertexArrays(1, &_opengl3_renderer_internal__g.text_vao);
    glGenBuffers(1,      &_opengl3_renderer_internal__g.text_vbo);

    glBindVertexArray(_opengl3_renderer_internal__g.text_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.text_vbo);

    GLsizeiptr max_bytes = (GLsizeiptr)(_OPENGL3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_opengl3_renderer_internal__text_vertex);
    glBufferData(GL_ARRAY_BUFFER, max_bytes, NULL, GL_DYNAMIC_DRAW);

    //
    //attribute layout: matches the text vertex shader's location=N
    //declarations and the _opengl3_renderer_internal__text_vertex
    //struct. one attribute per field group (position, color, uv).
    //
    GLsizei stride = (GLsizei)sizeof(_opengl3_renderer_internal__text_vertex);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__text_vertex, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__text_vertex, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__text_vertex, u));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    _opengl3_renderer_internal__g.text_vert_cap =
        _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _opengl3_renderer_internal__g.text_cpu_verts =
        (_opengl3_renderer_internal__text_vertex*)GUI_MALLOC_T((size_t)_opengl3_renderer_internal__g.text_vert_cap * sizeof(_opengl3_renderer_internal__text_vertex), MM_TYPE_RENDERER);
    if (_opengl3_renderer_internal__g.text_cpu_verts == NULL)
    {
        log_error("out of memory for cpu text vertex buffer");
        return FALSE;
    }

    return TRUE;
}

//============================================================================
//INTERNAL: shader compile / link helpers
//============================================================================

static GLuint _opengl3_renderer_internal__compile_shader(GLenum kind, char* src)
{
    GLuint sh = glCreateShader(kind);
    if (sh == 0)
    {
        log_error("glCreateShader returned 0");
        return 0;
    }

    const GLchar* srcs[1] = { (const GLchar*)src };
    glShaderSource(sh, 1, srcs, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 0)
        {
            char* log = (char*)GUI_MALLOC(len);
            if (log != NULL)
            {
                glGetShaderInfoLog(sh, len, NULL, log);
                log_error("shader compile error:\n%s", log);
                GUI_FREE(log);
            }
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint _opengl3_renderer_internal__link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    if (p == 0)
    {
        log_error("glCreateProgram returned 0");
        return 0;
    }
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        if (len > 0)
        {
            char* log = (char*)GUI_MALLOC(len);
            if (log != NULL)
            {
                glGetProgramInfoLog(p, len, NULL, log);
                log_error("program link error:\n%s", log);
                GUI_FREE(log);
            }
        }
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

//============================================================================
//PUBLIC: text pipeline
//============================================================================
//
//the font module rasterizes glyphs into a 1024x1024 R8 bitmap and hands
//it to us here for uploading. we keep the GLuint as the opaque atlas
//handle; void* round-trips through font.c until it comes back in
//set_text_atlas/destroy_atlas.
//

void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    if (pixels == NULL || width <= 0 || height <= 0)
    {
        return NULL;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        return NULL;
    }

    //
    //row alignment defaults to 4 bytes. our R8 atlas rows are width*1
    //bytes; widths that aren't multiples of 4 would land on the wrong
    //byte offset per row without this. atlases are normally 1024-wide
    //so we're already aligned, but setting this makes arbitrary widths
    //safe -- zero cost at texture upload time.
    //
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindTexture(GL_TEXTURE_2D, tex);

    //
    //GL_R8 internal format (single-channel 8-bit unsigned, reads as
    //0..1 in the shader's .r channel). GL_RED is the format of the
    //data we're uploading. core in 3.0; widely supported.
    //
    glTexImage2D(
        GL_TEXTURE_2D, 0,
        GL_R8,
        width, height, 0,
        GL_RED, GL_UNSIGNED_BYTE,
        pixels
    );

    //
    //LINEAR filtering so the 3x-oversampled glyphs sample smoothly
    //between texels. CLAMP_TO_EDGE so we never bleed neighbor glyphs
    //(stbtt_pack leaves 1px padding, but bilinear reaches ~0.5px
    //either way from the quad edge).
    //
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    //
    //round-trip the GLuint through a void* (via uintptr_t to dodge
    //sign-extension warnings on 64-bit compilers).
    //
    return (void*)(uintptr_t)tex;
}

void renderer__destroy_atlas(void* atlas)
{
    if (atlas == NULL)
    {
        return;
    }
    GLuint tex = (GLuint)(uintptr_t)atlas;
    //
    //if we're about to delete the currently-bound atlas, clear the
    //cached handle so end_frame won't try to sample a dangling name.
    //
    if (_opengl3_renderer_internal__g.current_text_atlas == tex)
    {
        _opengl3_renderer_internal__g.current_text_atlas = 0;
    }
    glDeleteTextures(1, &tex);
}

void renderer__set_text_atlas(void* atlas)
{
    _opengl3_renderer_internal__g.current_text_atlas = (GLuint)(uintptr_t)atlas;
}

void renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color)
{
    GLuint atlas = _opengl3_renderer_internal__g.current_text_atlas;
    if (atlas == 0)
    {
        //
        //no atlas bound -- font.c didn't call set_text_atlas before
        //submitting, or the current font's atlas was destroyed. drop.
        //
        return;
    }
    if (_opengl3_renderer_internal__g.text_vert_count + _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _opengl3_renderer_internal__g.text_vert_cap)
    {
        return;
    }

    //
    //resolve the current run. if the last run targets this atlas, we
    //keep appending to it. otherwise we open a new run pointing at the
    //next vertex slot. this is the heart of multi-font support: the
    //frame's text VBO is laid out as [run_0_verts | run_1_verts | ...],
    //and end_frame issues one draw per run with that run's atlas bound.
    //
    _opengl3_renderer_internal__text_run* run = NULL;
    if (_opengl3_renderer_internal__g.text_run_count > 0)
    {
        run = &_opengl3_renderer_internal__g.text_runs[_opengl3_renderer_internal__g.text_run_count - 1];
        if (run->atlas != atlas)
        {
            run = NULL; // different atlas -- need a fresh run.
        }
    }
    if (run == NULL)
    {
        if (_opengl3_renderer_internal__g.text_run_count >= _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_RUNS)
        {
            //
            //runs exhausted. rare in practice (64 distinct fonts per
            //frame), but drop silently rather than render to the wrong
            //atlas.
            //
            return;
        }
        run = &_opengl3_renderer_internal__g.text_runs[_opengl3_renderer_internal__g.text_run_count++];
        run->atlas      = atlas;
        run->vert_start = _opengl3_renderer_internal__g.text_vert_count;
        run->vert_count = 0;
    }

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    float u0 = uv.x;
    float v0 = uv.y;
    float u1 = uv.x + uv.w;
    float v1 = uv.y + uv.h;

    //
    //same vertex ordering as solid rects (VISUAL CONTRACT):
    //  tri 1: TL, TR, BL
    //  tri 2: TR, BR, BL
    //
    _opengl3_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _opengl3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _opengl3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _opengl3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _opengl3_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _opengl3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    run->vert_count += _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

//============================================================================
//INTERNAL: misc helpers
//============================================================================

static void _opengl3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c)
{
    _opengl3_renderer_internal__vertex* v = &_opengl3_renderer_internal__g.cpu_verts[_opengl3_renderer_internal__g.vert_count++];
    v->x      = x;
    v->y      = y;
    v->r      = c.r;
    v->g      = c.g;
    v->b      = c.b;
    v->a      = c.a;
    v->lx     = lx;
    v->ly     = ly;
    v->rect_w = rw;
    v->rect_h = rh;
    v->radius = radius;
}

static void _opengl3_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c)
{
    _opengl3_renderer_internal__text_vertex* tv = &_opengl3_renderer_internal__g.text_cpu_verts[_opengl3_renderer_internal__g.text_vert_count++];
    tv->x = x;
    tv->y = y;
    tv->r = c.r;
    tv->g = c.g;
    tv->b = c.b;
    tv->a = c.a;
    tv->u = u;
    tv->v = v;
}

static void _opengl3_renderer_internal__log_win32_error(char* where)
{
    DWORD err = GetLastError();
    log_error("%s failed (error %lu)", where, (unsigned long)err);
}

//============================================================================
//IMAGE PIPELINE (RGBA textures, one-quad-per-submit, lazy-initialized)
//============================================================================
//
//Kept self-contained down here so the rest of the file doesn't have to
//thread image-specific state through init. First call to submit_image
//lazily builds the program / VAO / VBO; shutdown above releases them.
//

static const char* _OPENGL3_RENDERER_INTERNAL__IMAGE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 in_pos;\n"
    "layout(location=1) in vec2 in_uv;\n"
    "layout(location=2) in vec4 in_color;\n"
    "uniform vec2 u_viewport;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    vec2 ndc = (in_pos / u_viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);\n"
    "    v_uv = in_uv;\n"
    "    v_color = in_color;\n"
    "}\n";

static const char* _OPENGL3_RENDERER_INTERNAL__IMAGE_FS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "    vec4 s = texture(u_tex, v_uv);\n"
    "    out_color = s * v_color;\n"
    "}\n";

static boole _opengl3_renderer_internal__ensure_image_pipeline(void)
{
    if (_opengl3_renderer_internal__g.image_program != 0)
    {
        return TRUE;
    }

    GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   (char*)_OPENGL3_RENDERER_INTERNAL__IMAGE_VS);
    GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_OPENGL3_RENDERER_INTERNAL__IMAGE_FS);
    if (vs == 0 || fs == 0)
    {
        if (vs != 0) { glDeleteShader(vs); }
        if (fs != 0) { glDeleteShader(fs); }
        return FALSE;
    }
    GLuint prog = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0)
    {
        return FALSE;
    }
    _opengl3_renderer_internal__g.image_program         = prog;
    _opengl3_renderer_internal__g.image_u_viewport_loc  = glGetUniformLocation(prog, "u_viewport");
    _opengl3_renderer_internal__g.image_u_tex_loc       = glGetUniformLocation(prog, "u_tex");

    //
    //one-quad scratch VBO. reused across every submit_image call;
    //glBufferData with NULL first (orphan) before each upload so the
    //driver doesn't stall the GPU waiting for the previous draw to
    //finish reading the buffer.
    //
    glGenVertexArrays(1, &_opengl3_renderer_internal__g.image_vao);
    glGenBuffers(1, &_opengl3_renderer_internal__g.image_vbo);
    glBindVertexArray(_opengl3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.image_vbo);

    //
    //Same layout as text_vertex: x,y, r,g,b,a, u,v. Reusing the struct
    //would be viable but keeping it inline here (as a 32-byte stride)
    //avoids a forward dependency on that struct's memory layout.
    //
    GLsizei stride = (GLsizei)(8 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return TRUE;
}

void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height)
{
    if (rgba == NULL || width <= 0 || height <= 0)
    {
        return NULL;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        return NULL;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    //
    // Generate a full mipmap chain and use trilinear for minification.
    // Photo-sized sources (6000x4000) rendered as 48 px launcher tiles
    // twinkle badly without mipmaps: each output pixel samples only
    // one 2x2 texel neighborhood in a 125x-bigger source, so
    // subpixel motion during scrolling / animation grabs different
    // source pixels each frame. Trilinear samples the pre-reduced
    // mip level closest to the shown scale and smooths across it.
    //
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return (void*)(uintptr_t)tex;
}

void renderer__destroy_texture(void* tex)
{
    if (tex == NULL)
    {
        return;
    }
    GLuint name = (GLuint)(uintptr_t)tex;
    glDeleteTextures(1, &name);
}

void renderer__submit_image(gui_rect rect, void* tex, gui_color tint)
{
    if (tex == NULL)
    {
        return;
    }
    if (!_opengl3_renderer_internal__ensure_image_pipeline())
    {
        return;
    }

    //
    //flush any pending rects + text so their draws land UNDERNEATH the
    //image (correct order relative to submit_rect / submit_text_glyph
    //calls made before this one).
    //
    _opengl3_renderer_internal__flush_batches();

    float x0 = (float)rect.x;
    float y0 = (float)rect.y;
    float x1 = x0 + (float)rect.w;
    float y1 = y0 + (float)rect.h;

    float verts[6 * 8] = {
        // x,  y,    r,g,b,a,                   u, v
        x0, y0,  tint.r, tint.g, tint.b, tint.a,  0.0f, 0.0f,
        x1, y0,  tint.r, tint.g, tint.b, tint.a,  1.0f, 0.0f,
        x0, y1,  tint.r, tint.g, tint.b, tint.a,  0.0f, 1.0f,
        x1, y0,  tint.r, tint.g, tint.b, tint.a,  1.0f, 0.0f,
        x1, y1,  tint.r, tint.g, tint.b, tint.a,  1.0f, 1.0f,
        x0, y1,  tint.r, tint.g, tint.b, tint.a,  0.0f, 1.0f,
    };

    glUseProgram(_opengl3_renderer_internal__g.image_program);
    if (_opengl3_renderer_internal__g.image_u_viewport_loc >= 0)
    {
        glUniform2f(_opengl3_renderer_internal__g.image_u_viewport_loc, (float)_opengl3_renderer_internal__g.viewport_w, (float)_opengl3_renderer_internal__g.viewport_h);
    }
    if (_opengl3_renderer_internal__g.image_u_tex_loc >= 0)
    {
        glUniform1i(_opengl3_renderer_internal__g.image_u_tex_loc, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)tex);

    glBindVertexArray(_opengl3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.image_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), NULL,  GL_DYNAMIC_DRAW);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}
