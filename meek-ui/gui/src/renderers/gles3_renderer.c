// ============================================================================
// gles3_renderer.c - OpenGL ES 3.0 backend.
// ============================================================================
//
// WHAT THIS FILE DOES (the elevator pitch):
//
//   scene.c walks the widget tree and tells us "draw this colored rectangle
//   here" by calling renderer__submit_rect(). this file packages those calls
//   into work the GPU understands and ships it across using OpenGL ES 3.0 --
//   the embedded variant of OpenGL that ships on essentially every Android
//   device (and iOS, and most embedded Linux SoCs).
//
//   this backend is interchangeable with opengl3_renderer.c (desktop OpenGL),
//   d3d11_renderer.c, and d3d9_renderer.c -- exactly one is compiled into
//   the gui library at a time. on Android the build script picks gles3.
//
// PLATFORM / RENDERER SPLIT (different from the Windows backends):
//
//   UNLIKE opengl3_renderer.c, this file does NOT create the GL context.
//   EGL setup lives in platform_android.c (EGLDisplay + EGLConfig +
//   EGLSurface + EGLContext tied to the ANativeWindow from
//   android_native_app_glue). the renderer expects the platform to have
//   made an EGL context current BEFORE renderer__init runs and to keep
//   it current during every renderer__* call.
//
//   this is a cleaner split than the Windows backends, which each own
//   their context creation. eventually platform_win32 should pull the
//   wgl* dance out of opengl3_renderer.c and own the WGL context the
//   same way platform_android owns the EGL context.
//
// THE PIPELINE:
//
//   1. INPUT-ASSEMBLY: glBindVertexArray + glBindBuffer set the source
//      of vertex data. the VAO recipe was built once at init.
//
//   2. VERTEX SHADER: written in GLSL ES 3.00. converts pixel coords
//      to NDC (-1..1, y-up) so we can work in pixel coords on the CPU
//      side. passes color and SDF varyings through to the PS.
//
//   3. RASTERIZER: hardware fixed function. ScissorEnable lives here
//      (glEnable(GL_SCISSOR_TEST) + glScissor(rect) -- BL-origin Y
//      coords).
//
//   4. PIXEL SHADER: GLSL ES 3.00. computes SDF for a rounded box,
//      antialiases the edge with a 1-pixel smoothstep, discards
//      pixels outside the rounded shape so corners actually round.
//
//   5. OUTPUT-MERGER: alpha blending (glBlendFuncSeparate so the
//      framebuffer's alpha stays at 1.0 across overlapping draws --
//      see renderer.h VISUAL CONTRACT).
//
//   6. PRESENT: NOT done here. platform_android calls eglSwapBuffers
//      on its EGLSurface. our end_frame just flushes the batches.
//
// SCOPE FOR THE FIRST PASS:
//
//   SOLID RECT PASS: full SDF rounded-box renderer, identical math to
//     opengl3_renderer.c. this gives us window + widgets + buttons.
//
//   SCISSOR: implemented (mirrors opengl3). scrollable divs and Window
//     get proper clipping on Android.
//
//   TEXT PASS: stub entry points (create_atlas_r8 returns NULL, submit
//     is a no-op). font__at will fail to create atlases on Android for
//     now; widgets gracefully skip text rendering. adding the textured
//     glyph pipeline is a small follow-up once the rect path is verified
//     on-device.
//
// VISUAL CONTRACT: identical SDF formula, smoothstep range, scissor
// semantics, separate-alpha blend mode, and vertex ordering as the
// opengl3 / d3d11 / d3d9 backends.
//

//
// GL header: Android + Linux + DRM use <GLES3/gl3.h>. macOS has no
// GLES headers and instead ships desktop GL 3.3+ core under
// <OpenGL/gl3.h>. The token set the backend uses is the intersection
// of GLES 3.0 and desktop GL 3.3 core, so picking either header at
// compile time yields the same callable surface.
//
#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION 1
  #include <OpenGL/gl3.h>
#else
  #include <GLES3/gl3.h>
#endif

#include <stdint.h>     // uintptr_t for GLuint <-> void* round-trip through font.c's opaque atlas_tex handle.
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "renderer.h"
#include "_visual_contract.h"
#include "../clib/memory_manager.h"
#include "../third_party/log.h"

//
// Shader version preamble. GLES 3.0 uses "#version 300 es" +
// requires precision qualifiers. Desktop GL on macOS (we ask
// NSOpenGLPixelFormat for a 4.1 Core Profile context, which gives
// GLSL 410) uses "#version 410"; precision qualifiers are allowed
// but ignored. Everything else in the shader bodies (layout(location
// =N), in/out, texture(), discard) works identically in both
// versions, so the only per-platform difference is this preamble
// string. Kept as a macro rather than a const char* so string
// concatenation at the call sites stays compile-time.
//
#if defined(__APPLE__)
  #define _GLES3_RENDERER_INTERNAL__GLSL_VERSION "#version 410\n"
#else
  #define _GLES3_RENDERER_INTERNAL__GLSL_VERSION "#version 300 es\n"
#endif

//============================================================================
//VERTEX FORMAT -- identical to opengl3_renderer.c
//============================================================================

typedef struct _gles3_renderer_internal__vertex
{
    float x, y;
    float r, g, b, a;
    float lx, ly;
    float rect_w, rect_h;
    float radius;
} _gles3_renderer_internal__vertex;

#define _GLES3_RENDERER_INTERNAL__MAX_QUADS       2048
#define _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD  6

//
//TEXT VERTEX: simpler than the solid-rect vertex -- no SDF bookkeeping,
//just pixel position + per-vertex color + atlas UV. six per glyph quad.
//mirrors opengl3_renderer.c's layout exactly so the font.c path works
//unchanged.
//
typedef struct _gles3_renderer_internal__text_vertex
{
    float x, y;
    float r, g, b, a;
    float u, v;
} _gles3_renderer_internal__text_vertex;

#define _GLES3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096
#define _GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS    64

//
//per-atlas "run" record. a frame may emit glyphs from multiple
//(family, size) pairs -- each combination lives in its own atlas
//texture, so we partition the frame's text VBO into runs and issue
//one glDrawArrays per run with the matching texture bound.
//
typedef struct _gles3_renderer_internal__text_run
{
    GLuint atlas;
    int64  vert_start;
    int64  vert_count;
} _gles3_renderer_internal__text_run;

//============================================================================
//SHADER SOURCES (GLSL ES 3.00)
//============================================================================
//
//differences from the desktop GL 3.30 shaders:
//  - "#version 300 es" instead of "#version 330 core"
//  - explicit "precision" qualifier required for floats in the fragment
//    shader (ES has no implicit default for floats). we use highp for
//    consistent subpixel precision with the desktop backend; mediump
//    would be fine for most phones but drops ~2-3 bits on low-end.
//  - vertex shader's precision defaults to highp, fragment shader's
//    defaults to undefined-in-GLSL-ES-3.0 -> must be specified.
//

static char* _gles3_renderer_internal__vs_src =
    _GLES3_RENDERER_INTERNAL__GLSL_VERSION
    "precision highp float;\n"
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

static char* _gles3_renderer_internal__fs_src =
    _GLES3_RENDERER_INTERNAL__GLSL_VERSION
    "precision highp float;\n"
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
//TEXT VS/FS -- GLES 3.0 port of opengl3's text shaders. same math as
//the desktop legacy (non-sRGB) variant: sample the atlas (R8 alpha
//coverage), apply the pow(a, 1/2.2) gamma approximation, multiply by
//vertex alpha. matches the Windows backends' text look.
//
//precision qualifier is required in the FS on GLES (there's no
//implicit default for floats); we use highp to stay consistent with
//the rect pipeline so subpixel glyph positions don't snap to the
//mediump grid on low-end phones.
//
static char* _gles3_renderer_internal__text_vs_src =
    _GLES3_RENDERER_INTERNAL__GLSL_VERSION
    "precision highp float;\n"
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

static char* _gles3_renderer_internal__text_fs_src =
    _GLES3_RENDERER_INTERNAL__GLSL_VERSION
    "precision highp float;\n"
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main()\n"
    "{\n"
    "    float a = texture(u_atlas, v_uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    "    a = pow(a, 1.0 / 2.2);\n"
    "    o_color = vec4(v_color.rgb, v_color.a * a);\n"
    "}\n";

//============================================================================
//STATE
//============================================================================

typedef struct _gles3_renderer_internal__state
{
    GLuint program;
    GLint  u_viewport_loc;
    GLuint vao;
    GLuint vbo;

    _gles3_renderer_internal__vertex* cpu_verts;
    int64                             vert_count;
    int64                             vert_cap;

    int64 viewport_w;
    int64 viewport_h;

    //
    //text pipeline (separate shader + VAO + VBO + cpu staging).
    //drawn after the solid-rect pass in end_frame so glyphs land
    //on top of button background rects. see the opengl3 backend
    //for the full commentary on per-atlas run batching.
    //
    GLuint text_program;
    GLint  text_u_viewport_loc;
    GLint  text_u_atlas_loc;
    GLuint text_vao;
    GLuint text_vbo;

    _gles3_renderer_internal__text_vertex* text_cpu_verts;
    int64                                  text_vert_count;
    int64                                  text_vert_cap;

    GLuint current_text_atlas;

    _gles3_renderer_internal__text_run text_runs[_GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int64                              text_run_count;

    //
    //Image pipeline (RGBA textures, one-quad-per-submit). Lazy-built
    //on first submit_image call. Matches opengl3_renderer.c.
    //
    GLuint image_program;
    GLint  image_u_viewport_loc;
    GLint  image_u_tex_loc;
    GLuint image_vao;
    GLuint image_vbo;

    //
    // Scissor stack. Same shape as opengl3_renderer.c: push records the
    // new clip rect, the GL scissor is set to the intersection of all
    // stacked rects, and pop drops the top of the stack.
    //
    gui_rect scissor_stack[32];
    int64    scissor_depth;
    //
    // Last effective scissor rect we applied to the GPU. push/pop
    // computes the new effective rect and skips both the batch
    // flush and the glScissor call when it matches -- nested
    // containers that share the same intersected clip (e.g. a
    // scrollable div whose child <text> nodes each push the same
    // padded inner rect) thus avoid one draw-call per nested push.
    //
    gui_rect scissor_last_applied;
    boole    scissor_last_valid;
} _gles3_renderer_internal__state;

static _gles3_renderer_internal__state _gles3_renderer_internal__g;

//============================================================================
//FORWARD DECLS
//============================================================================

static boole  _gles3_renderer_internal__build_pipeline(void);
static boole  _gles3_renderer_internal__build_text_pipeline(void);
static GLuint _gles3_renderer_internal__compile_shader(GLenum kind, char* src);
static GLuint _gles3_renderer_internal__link_program(GLuint vs, GLuint fs);
static void   _gles3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c);
static void   _gles3_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c);
static void   _gles3_renderer_internal__flush_batches(void);

//============================================================================
//PUBLIC: lifecycle
//============================================================================
//
//renderer__init assumes the platform layer already made an EGL context
//current. native_window is unused on Android -- the platform already
//consumed the ANativeWindow when creating the EGL surface.
//

boole renderer__init(void* native_window)
{
    (void)native_window;
    memset(&_gles3_renderer_internal__g, 0, sizeof(_gles3_renderer_internal__g));
    return _gles3_renderer_internal__build_pipeline();
}

void renderer__shutdown(void)
{
    //solid-rect pipeline objects
    if (_gles3_renderer_internal__g.vbo != 0)
    {
        glDeleteBuffers(1, &_gles3_renderer_internal__g.vbo);
    }
    if (_gles3_renderer_internal__g.vao != 0)
    {
        glDeleteVertexArrays(1, &_gles3_renderer_internal__g.vao);
    }
    if (_gles3_renderer_internal__g.program != 0)
    {
        glDeleteProgram(_gles3_renderer_internal__g.program);
    }
    if (_gles3_renderer_internal__g.cpu_verts != NULL)
    {
        GUI_FREE(_gles3_renderer_internal__g.cpu_verts);
    }

    //text pipeline objects
    if (_gles3_renderer_internal__g.text_vbo != 0)
    {
        glDeleteBuffers(1, &_gles3_renderer_internal__g.text_vbo);
    }
    if (_gles3_renderer_internal__g.text_vao != 0)
    {
        glDeleteVertexArrays(1, &_gles3_renderer_internal__g.text_vao);
    }
    if (_gles3_renderer_internal__g.text_program != 0)
    {
        glDeleteProgram(_gles3_renderer_internal__g.text_program);
    }
    if (_gles3_renderer_internal__g.text_cpu_verts != NULL)
    {
        GUI_FREE(_gles3_renderer_internal__g.text_cpu_verts);
    }

    //
    //image pipeline (lazy-built; guarded against "never used" case).
    //Textures created via renderer__create_texture_rgba are owned by
    //widget_image and released through renderer__destroy_texture.
    //
    if (_gles3_renderer_internal__g.image_vbo != 0)
    {
        glDeleteBuffers(1, &_gles3_renderer_internal__g.image_vbo);
    }
    if (_gles3_renderer_internal__g.image_vao != 0)
    {
        glDeleteVertexArrays(1, &_gles3_renderer_internal__g.image_vao);
    }
    if (_gles3_renderer_internal__g.image_program != 0)
    {
        glDeleteProgram(_gles3_renderer_internal__g.image_program);
    }

    memset(&_gles3_renderer_internal__g, 0, sizeof(_gles3_renderer_internal__g));
}

void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear)
{
    _gles3_renderer_internal__g.viewport_w      = viewport_w;
    _gles3_renderer_internal__g.viewport_h      = viewport_h;
    _gles3_renderer_internal__g.vert_count      = 0;
    _gles3_renderer_internal__g.text_vert_count = 0;
    _gles3_renderer_internal__g.text_run_count  = 0;
    //
    //current_text_atlas is intentionally preserved across frames;
    //the first font__draw call each frame rebinds it anyway. runs,
    //though, reset every frame: a run belongs to the frame that
    //produced it.
    //

    glViewport(0, 0, (GLsizei)viewport_w, (GLsizei)viewport_h);
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void renderer__submit_rect(gui_rect r, gui_color c, float radius)
{
    if (_gles3_renderer_internal__g.vert_count + _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _gles3_renderer_internal__g.vert_cap)
    {
        return;
    }

    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f)   radius = 0.0f;
    if (radius > max_r)  radius = max_r;

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    _gles3_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
}

//
// Shadow: multi-layer concentric rects with quadratic falloff.
// See the opengl3 twin for the rationale.
//
void renderer__submit_shadow(gui_rect rect, gui_color c, float radius, float blur)
{
    if (c.a <= 0.0f) { return; }
    int layers = (int)(blur * 0.4f) + 2;
    if (layers < 2) { layers = 2; }
    if (layers > 8) { layers = 8; }
    float step = blur / (float)layers;
    for (int i = 0; i < layers; i++)
    {
        float grow = step * (float)i;
        float t = (float)i / (float)(layers - 1);
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

void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius)
{
    if (_gles3_renderer_internal__g.vert_count + _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _gles3_renderer_internal__g.vert_cap)
    {
        return;
    }
    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f)   radius = 0.0f;
    if (radius > max_r)  radius = max_r;
    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

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
        c_tr.r = (from.r + to.r) * 0.5f; c_tr.g = (from.g + to.g) * 0.5f;
        c_tr.b = (from.b + to.b) * 0.5f; c_tr.a = (from.a + to.a) * 0.5f;
        c_bl = c_tr;
    }
    else if (direction == (int)GUI_GRADIENT_DIAGONAL_TR)
    {
        c_tr = from;
        c_bl = to;
        c_tl.r = (from.r + to.r) * 0.5f; c_tl.g = (from.g + to.g) * 0.5f;
        c_tl.b = (from.b + to.b) * 0.5f; c_tl.a = (from.a + to.a) * 0.5f;
        c_br = c_tl;
    }
    _gles3_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c_tl);
    _gles3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _gles3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
    _gles3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _gles3_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c_br);
    _gles3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
}

//============================================================================
//INTERNAL: flush_batches
//============================================================================
//
//Draws whatever solid rects are currently buffered, then resets the
//count so subsequent submissions start a fresh batch. Called from
//end_frame and from push/pop_scissor (a state change can't be merged
//into a single Draw, so we flush before the change).
//

static void _gles3_renderer_internal__flush_batches(void)
{
    //
    // PASS 1: solid rects. Conditional -- if nothing's queued we
    // skip straight to the text pass rather than early-return for
    // the whole function. This is the bug the select popup was
    // hitting: if the caller pushed scissor with ONLY text queued
    // (not rects), the old early-return skipped the text flush,
    // so tree labels leaked out of the scissor bracket and drew
    // on top of the popup panel at end_frame.
    //
    if (_gles3_renderer_internal__g.vert_count > 0)
    {
        glUseProgram(_gles3_renderer_internal__g.program);
        glUniform2f(_gles3_renderer_internal__g.u_viewport_loc,
                    (float)_gles3_renderer_internal__g.viewport_w,
                    (float)_gles3_renderer_internal__g.viewport_h);

        glBindVertexArray(_gles3_renderer_internal__g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.vbo);

        //
        // Buffer ORPHAN before SubData -- see opengl3_renderer.c for the
        // detailed rationale. Critical for mid-frame multi-flush correctness:
        // without it, the second SubData this frame races the first Draw
        // and produces flicker as alpha animations play out.
        //
        GLsizeiptr bytes     = (GLsizeiptr)_gles3_renderer_internal__g.vert_count * (GLsizeiptr)sizeof(_gles3_renderer_internal__vertex);
        GLsizeiptr cap_bytes = (GLsizeiptr)_gles3_renderer_internal__g.vert_cap   * (GLsizeiptr)sizeof(_gles3_renderer_internal__vertex);
        glBufferData(GL_ARRAY_BUFFER, cap_bytes, NULL, GL_DYNAMIC_DRAW);   // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, _gles3_renderer_internal__g.cpu_verts);

        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_gles3_renderer_internal__g.vert_count);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);

        _gles3_renderer_internal__g.vert_count = 0;
    }

    //
    // PASS 2: text glyphs, one draw per atlas run. Runs after the
    // solid-rect pass so glyphs land on top of button bg rects in
    // the same flush. Conditional on its own batch state.
    //
    if (_gles3_renderer_internal__g.text_vert_count > 0 &&
        _gles3_renderer_internal__g.text_run_count  > 0)
    {
        glUseProgram(_gles3_renderer_internal__g.text_program);
        glUniform2f(_gles3_renderer_internal__g.text_u_viewport_loc,
                    (float)_gles3_renderer_internal__g.viewport_w,
                    (float)_gles3_renderer_internal__g.viewport_h);
        glUniform1i(_gles3_renderer_internal__g.text_u_atlas_loc, 0);
        glActiveTexture(GL_TEXTURE0);

        glBindVertexArray(_gles3_renderer_internal__g.text_vao);
        glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.text_vbo);

        GLsizeiptr tbytes     = (GLsizeiptr)_gles3_renderer_internal__g.text_vert_count * (GLsizeiptr)sizeof(_gles3_renderer_internal__text_vertex);
        GLsizeiptr tcap_bytes = (GLsizeiptr)_gles3_renderer_internal__g.text_vert_cap   * (GLsizeiptr)sizeof(_gles3_renderer_internal__text_vertex);
        glBufferData(GL_ARRAY_BUFFER, tcap_bytes, NULL, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, tbytes, _gles3_renderer_internal__g.text_cpu_verts);

        for (int64 i = 0; i < _gles3_renderer_internal__g.text_run_count; i++)
        {
            _gles3_renderer_internal__text_run* run = &_gles3_renderer_internal__g.text_runs[i];
            glBindTexture(GL_TEXTURE_2D, run->atlas);
            glDrawArrays(GL_TRIANGLES, (GLint)run->vert_start, (GLsizei)run->vert_count);
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);

        _gles3_renderer_internal__g.text_vert_count = 0;
        _gles3_renderer_internal__g.text_run_count  = 0;
    }
}

//============================================================================
//PUBLIC: renderer__push_scissor / renderer__pop_scissor
//============================================================================
//
//GLES has the same glScissor / glEnable(GL_SCISSOR_TEST) shape as
//desktop GL. Each push/pop forces a flush of the current batch, then
//updates GPU scissor state. Stack-nested rects intersect (smaller of
//the two clips wins).
//
//GL's scissor uses BOTTOM-LEFT-origin coordinates; we flip Y from our
//top-left convention.
//

static gui_rect _gles3_renderer_internal__intersect(gui_rect a, gui_rect b)
{
    float x0  = (a.x > b.x) ? a.x : b.x;
    float y0  = (a.y > b.y) ? a.y : b.y;
    float x1a = a.x + a.w, x1b = b.x + b.w;
    float y1a = a.y + a.h, y1b = b.y + b.h;
    float x1 = (x1a < x1b) ? x1a : x1b;
    float y1 = (y1a < y1b) ? y1a : y1b;
    gui_rect r;
    r.x = x0;
    r.y = y0;
    r.w = (x1 > x0) ? (x1 - x0) : 0.0f;
    r.h = (y1 > y0) ? (y1 - y0) : 0.0f;
    return r;
}

//
// Compute the effective rect (intersection of every rect on the
// stack) without touching GPU state. Returns a w=h=0 rect when
// the stack is empty, signalling "no scissor active".
//
static gui_rect _gles3_renderer_internal__effective_scissor(void)
{
    gui_rect zero = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (_gles3_renderer_internal__g.scissor_depth <= 0) { return zero; }
    gui_rect r = _gles3_renderer_internal__g.scissor_stack[0];
    for (int64 i = 1; i < _gles3_renderer_internal__g.scissor_depth; i++)
    {
        r = _gles3_renderer_internal__intersect(r, _gles3_renderer_internal__g.scissor_stack[i]);
    }
    return r;
}

static void _gles3_renderer_internal__apply_scissor_top(void)
{
    if (_gles3_renderer_internal__g.scissor_depth <= 0)
    {
        glDisable(GL_SCISSOR_TEST);
        _gles3_renderer_internal__g.scissor_last_valid = FALSE;
        return;
    }
    gui_rect r = _gles3_renderer_internal__effective_scissor();
    GLint   gl_x = (GLint)r.x;
    GLint   gl_y = (GLint)((float)_gles3_renderer_internal__g.viewport_h - r.y - r.h);
    GLsizei gl_w = (GLsizei)r.w;
    GLsizei gl_h = (GLsizei)r.h;
    if (gl_w < 0) { gl_w = 0; }
    if (gl_h < 0) { gl_h = 0; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(gl_x, gl_y, gl_w, gl_h);
    _gles3_renderer_internal__g.scissor_last_applied = r;
    _gles3_renderer_internal__g.scissor_last_valid   = TRUE;
}

//
// Same-rect optimization: if the new effective rect equals what
// we last applied, skip both the batch flush (expensive) and the
// glScissor call. Nested containers that resolve to the same
// intersected clip thus produce one draw call instead of N.
//
static boole _gles3_renderer_internal__rects_equal(gui_rect a, gui_rect b)
{
    return (boole)(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
}

void renderer__push_scissor(gui_rect rect)
{
    if (_gles3_renderer_internal__g.scissor_depth >= (int64)(sizeof(_gles3_renderer_internal__g.scissor_stack) / sizeof(_gles3_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    //
    // Tentatively push so effective_scissor sees the new rect, then
    // compare against last_applied. If unchanged, leave the batch
    // in place (no flush, no GPU state update). If changed, flush
    // and apply.
    //
    _gles3_renderer_internal__g.scissor_stack[_gles3_renderer_internal__g.scissor_depth++] = rect;
    gui_rect new_eff = _gles3_renderer_internal__effective_scissor();
    if (_gles3_renderer_internal__g.scissor_last_valid &&
        _gles3_renderer_internal__rects_equal(new_eff, _gles3_renderer_internal__g.scissor_last_applied))
    {
        return;
    }
    _gles3_renderer_internal__flush_batches();
    _gles3_renderer_internal__apply_scissor_top();
}

void renderer__pop_scissor(void)
{
    if (_gles3_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("renderer__pop_scissor: stack already empty");
        return;
    }
    _gles3_renderer_internal__g.scissor_depth--;
    gui_rect new_eff = _gles3_renderer_internal__effective_scissor();
    boole becoming_disabled = (_gles3_renderer_internal__g.scissor_depth == 0);
    if (!becoming_disabled &&
        _gles3_renderer_internal__g.scissor_last_valid &&
        _gles3_renderer_internal__rects_equal(new_eff, _gles3_renderer_internal__g.scissor_last_applied))
    {
        return;
    }
    _gles3_renderer_internal__flush_batches();
    _gles3_renderer_internal__apply_scissor_top();
}

void renderer__blur_region(gui_rect rect, float sigma_px)
{
    //
    // Placeholder: GLES3 doesn't yet implement true backdrop blur
    // (would need a helper FBO + separable Gaussian fragment shader).
    // Translucent darken splat as a stand-in.
    //
    (void)sigma_px;
    gui_color dim = { 0.0f, 0.0f, 0.0f, 0.15f };
    renderer__submit_rect(rect, dim, 0.0f);
}

void renderer__flush_pending_draws(void)
{
    // Public thin wrapper over the internal flush so scene_render can
    // break the per-frame batch at z-index sibling boundaries. See
    // renderer.h for why this matters for text+z interleaving.
    _gles3_renderer_internal__flush_batches();
}

void renderer__end_frame(void)
{
    //
    //note: eglSwapBuffers is called by platform_android, not here. on
    //Android the platform owns the EGLSurface and therefore the
    //present. same split as what a future refactor would do on Windows.
    //
    // flush_batches drains BOTH pending rect and text batches in the
    // right order (rects first, then text-on-top), so one call is
    // enough here. Scissor push/pop during the frame may have drained
    // partial queues already; the final call flushes whatever's left.
    //
    _gles3_renderer_internal__flush_batches();

    //
    //defensive scissor cleanup: if a widget pushed but failed to pop,
    //the next frame would inherit the stale scissor. clear and disable.
    //
    if (_gles3_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("renderer__end_frame: scissor stack non-empty (%lld); resetting", (long long)_gles3_renderer_internal__g.scissor_depth);
        _gles3_renderer_internal__g.scissor_depth = 0;
    }
    glDisable(GL_SCISSOR_TEST);
}

//============================================================================
//PUBLIC: text pipeline -- STUBS for the MVP Android port
//============================================================================
//
//font.c calls these during font rasterization. returning NULL from
//create_atlas_r8 cascades through: font__at returns NULL, font__draw
//does nothing, widgets with text render only their bg rect. this lets
//the solid-rect path stand alone on Android until we add the textured
//glyph pipeline here (mirror of the opengl3 backend's text pass).
//

//
//Upload an R8 coverage bitmap to a fresh texture. font.c calls us
//once per (family, size) pair; we return the GLuint cast to a void*
//and font.c holds onto it across frames. LINEAR filtering +
//CLAMP_TO_EDGE matches the opengl3 backend (same VISUAL CONTRACT).
//
//GLES 3.0 supports GL_R8 as a sized internal format + GL_RED as a
//transfer format, so the upload shape matches opengl3 exactly.
//GL_UNPACK_ALIGNMENT is set to 1 before the upload because an R8
//bitmap's row stride is one byte per pixel; leaving alignment at
//the GL default of 4 corrupts rows whose width isn't a multiple of
//4 (our 1024/2048/4096 power-of-two atlases happen to be safe but
//we set alignment defensively).
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
        log_error("gles3: glGenTextures failed for text atlas");
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, tex);

    GLint prev_align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)width, (GLsizei)height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, pixels);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    //
    //cast GLuint -> uintptr_t -> void* so the handle round-trips
    //through font.c's void* storage without tripping -Wint-to-pointer
    //warnings (GLuint is 32-bit, void* is 64-bit on arm64).
    //
    return (void*)(uintptr_t)tex;
}

void renderer__destroy_atlas(void* atlas)
{
    if (atlas == NULL) { return; }
    GLuint tex = (GLuint)(uintptr_t)atlas;
    glDeleteTextures(1, &tex);
}

//
//select which atlas the NEXT submit_text_glyph calls will use.
//if the atlas differs from the one active in the current run,
//we open a new run (sealing off the previous one).
//
void renderer__set_text_atlas(void* atlas)
{
    GLuint tex = (GLuint)(uintptr_t)atlas;
    if (tex == _gles3_renderer_internal__g.current_text_atlas)
    {
        return;
    }
    _gles3_renderer_internal__g.current_text_atlas = tex;

    //
    //force the next submit_text_glyph to open a new run by
    //marking "the in-progress run (if any) is closed". we leave
    //text_run_count alone -- a new run is opened lazily when the
    //next vertex actually arrives.
    //
    if (_gles3_renderer_internal__g.text_run_count > 0)
    {
        _gles3_renderer_internal__text_run* last = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1];
        if (last->atlas != tex)
        {
            //
            //mark it closed by ensuring the next vertex goes to a
            //fresh run. submit_text_glyph detects "top run's atlas
            //!= current_text_atlas" and opens a new entry.
            //
        }
    }
}

//
//append six vertices for one glyph quad. extends the current run
//if its atlas matches current_text_atlas; otherwise opens a new run.
//no-op if we'd exceed the per-frame glyph cap.
//
void renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color)
{
    if (_gles3_renderer_internal__g.current_text_atlas == 0)
    {
        return; // no atlas set yet; caller should set_text_atlas first
    }
    if (_gles3_renderer_internal__g.text_vert_count + _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _gles3_renderer_internal__g.text_vert_cap)
    {
        return;
    }

    //
    //decide whether to extend the current run or open a new one.
    //new-run conditions: no runs yet, OR top run's atlas differs.
    //
    boole need_new_run = TRUE;
    if (_gles3_renderer_internal__g.text_run_count > 0)
    {
        _gles3_renderer_internal__text_run* last = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1];
        if (last->atlas == _gles3_renderer_internal__g.current_text_atlas)
        {
            need_new_run = FALSE;
        }
    }

    if (need_new_run)
    {
        if (_gles3_renderer_internal__g.text_run_count >= _GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS)
        {
            //
            //too many distinct atlases this frame; drop the glyph.
            //64 is the cap -- about 16 fonts x 4 sizes, so hitting
            //this means the host app is rendering an unusually
            //diverse typography stack in a single frame.
            //
            return;
        }
        _gles3_renderer_internal__text_run* run = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count++];
        run->atlas      = _gles3_renderer_internal__g.current_text_atlas;
        run->vert_start = _gles3_renderer_internal__g.text_vert_count;
        run->vert_count = 0;
    }

    //
    //push the six vertices for this glyph. quad winding matches the
    //solid-rect pipeline: TL, TR, BL, TR, BR, BL -> two triangles.
    //
    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;
    float u0 = uv.x;
    float v0 = uv.y;
    float u1 = uv.x + uv.w;
    float v1 = uv.y + uv.h;

    _gles3_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _gles3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _gles3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _gles3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _gles3_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _gles3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);

    //
    //extend the current run by the six verts we just pushed.
    //
    _gles3_renderer_internal__text_run* run = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1];
    run->vert_count += _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

//============================================================================
//INTERNAL: pipeline
//============================================================================

static boole _gles3_renderer_internal__build_pipeline(void)
{
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   _gles3_renderer_internal__vs_src);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, _gles3_renderer_internal__fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.program = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_gles3_renderer_internal__g.program == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.u_viewport_loc =
        glGetUniformLocation(_gles3_renderer_internal__g.program, "u_viewport");

    glGenVertexArrays(1, &_gles3_renderer_internal__g.vao);
    glGenBuffers(1,      &_gles3_renderer_internal__g.vbo);

    glBindVertexArray(_gles3_renderer_internal__g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.vbo);

    GLsizeiptr max_bytes = (GLsizeiptr)(_GLES3_RENDERER_INTERNAL__MAX_QUADS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_gles3_renderer_internal__vertex);
    glBufferData(GL_ARRAY_BUFFER, max_bytes, NULL, GL_DYNAMIC_DRAW);

    GLsizei stride = (GLsizei)sizeof(_gles3_renderer_internal__vertex);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex, lx));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex, rect_w));

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex, radius));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    _gles3_renderer_internal__g.vert_cap = _GLES3_RENDERER_INTERNAL__MAX_QUADS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _gles3_renderer_internal__g.cpu_verts = (_gles3_renderer_internal__vertex*)GUI_MALLOC_T((size_t)_gles3_renderer_internal__g.vert_cap * sizeof(_gles3_renderer_internal__vertex), MM_TYPE_RENDERER);
    if (_gles3_renderer_internal__g.cpu_verts == NULL)
    {
        log_error("gles3: out of memory for cpu vertex buffer");
        return FALSE;
    }

    //
    // Alpha blending. Same separate-alpha setup as opengl3 -- preserves
    // dst.a = 1 across overlapping draws so the framebuffer's alpha
    // channel doesn't decay during animations. Some Android compositors
    // multiply by framebuffer alpha at present time; we want fully
    // opaque output regardless of how many translucent rects we draw.
    // Part of the VISUAL CONTRACT in renderer.h.
    //
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    //
    //Text pipeline (second shader + VAO + VBO + cpu staging). Must
    //succeed or the whole renderer__init fails -- host apps can
    //gracefully handle that, but silently having no text pipeline
    //after init would produce invisible text with no error signal.
    //
    if (!_gles3_renderer_internal__build_text_pipeline())
    {
        return FALSE;
    }

    return TRUE;
}

static GLuint _gles3_renderer_internal__compile_shader(GLenum kind, char* src)
{
    GLuint sh = glCreateShader(kind);
    if (sh == 0)
    {
        log_error("gles3: glCreateShader returned 0");
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
            char* log_buf = (char*)GUI_MALLOC(len);
            if (log_buf != NULL)
            {
                glGetShaderInfoLog(sh, len, NULL, log_buf);
                log_error("gles3: shader compile error:\n%s", log_buf);
                GUI_FREE(log_buf);
            }
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint _gles3_renderer_internal__link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    if (p == 0)
    {
        log_error("gles3: glCreateProgram returned 0");
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
            char* log_buf = (char*)GUI_MALLOC(len);
            if (log_buf != NULL)
            {
                glGetProgramInfoLog(p, len, NULL, log_buf);
                log_error("gles3: program link error:\n%s", log_buf);
                GUI_FREE(log_buf);
            }
        }
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void _gles3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c)
{
    _gles3_renderer_internal__vertex* v = &_gles3_renderer_internal__g.cpu_verts[_gles3_renderer_internal__g.vert_count++];
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

static void _gles3_renderer_internal__push_text_vertex(float x, float y, float u, float v_uv, gui_color c)
{
    _gles3_renderer_internal__text_vertex* tv =
        &_gles3_renderer_internal__g.text_cpu_verts[_gles3_renderer_internal__g.text_vert_count++];
    tv->x = x;
    tv->y = y;
    tv->r = c.r;
    tv->g = c.g;
    tv->b = c.b;
    tv->a = c.a;
    tv->u = u;
    tv->v = v_uv;
}

//
//build the text pipeline's shaders, VAO, VBO, cpu staging buffer.
//mirrors the solid-rect setup but with the simpler vertex format
//(pos + color + uv; no SDF bookkeeping). called at the end of
//build_pipeline so a single init failure is noticed before we
//return TRUE from renderer__init.
//
static boole _gles3_renderer_internal__build_text_pipeline(void)
{
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   _gles3_renderer_internal__text_vs_src);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, _gles3_renderer_internal__text_fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.text_program = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_gles3_renderer_internal__g.text_program == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.text_u_viewport_loc =
        glGetUniformLocation(_gles3_renderer_internal__g.text_program, "u_viewport");
    _gles3_renderer_internal__g.text_u_atlas_loc =
        glGetUniformLocation(_gles3_renderer_internal__g.text_program, "u_atlas");

    glGenVertexArrays(1, &_gles3_renderer_internal__g.text_vao);
    glGenBuffers(1,      &_gles3_renderer_internal__g.text_vbo);

    glBindVertexArray(_gles3_renderer_internal__g.text_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.text_vbo);

    GLsizeiptr max_text_bytes = (GLsizeiptr)(_GLES3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_gles3_renderer_internal__text_vertex);
    glBufferData(GL_ARRAY_BUFFER, max_text_bytes, NULL, GL_DYNAMIC_DRAW);

    GLsizei stride = (GLsizei)sizeof(_gles3_renderer_internal__text_vertex);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__text_vertex, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__text_vertex, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__text_vertex, u));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    _gles3_renderer_internal__g.text_vert_cap  = _GLES3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _gles3_renderer_internal__g.text_cpu_verts = (_gles3_renderer_internal__text_vertex*)GUI_MALLOC_T((size_t)_gles3_renderer_internal__g.text_vert_cap * sizeof(_gles3_renderer_internal__text_vertex), MM_TYPE_RENDERER);
    if (_gles3_renderer_internal__g.text_cpu_verts == NULL)
    {
        log_error("gles3: out of memory for text cpu vertex buffer");
        return FALSE;
    }

    return TRUE;
}

//============================================================================
//IMAGE PIPELINE (RGBA textures, one-quad-per-submit, lazy-initialized)
//============================================================================

static const char* _GLES3_RENDERER_INTERNAL__IMAGE_VS =
    _GLES3_RENDERER_INTERNAL__GLSL_VERSION
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

static const char* _GLES3_RENDERER_INTERNAL__IMAGE_FS =
    _GLES3_RENDERER_INTERNAL__GLSL_VERSION
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "    vec4 s = texture(u_tex, v_uv);\n"
    "    out_color = s * v_color;\n"
    "}\n";

static boole _gles3_renderer_internal__ensure_image_pipeline(void)
{
    if (_gles3_renderer_internal__g.image_program != 0)
    {
        return TRUE;
    }
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   (char*)_GLES3_RENDERER_INTERNAL__IMAGE_VS);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_GLES3_RENDERER_INTERNAL__IMAGE_FS);
    if (vs == 0 || fs == 0)
    {
        if (vs != 0) { glDeleteShader(vs); }
        if (fs != 0) { glDeleteShader(fs); }
        return FALSE;
    }
    GLuint prog = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0)
    {
        return FALSE;
    }
    _gles3_renderer_internal__g.image_program        = prog;
    _gles3_renderer_internal__g.image_u_viewport_loc = glGetUniformLocation(prog, "u_viewport");
    _gles3_renderer_internal__g.image_u_tex_loc      = glGetUniformLocation(prog, "u_tex");

    glGenVertexArrays(1, &_gles3_renderer_internal__g.image_vao);
    glGenBuffers(1, &_gles3_renderer_internal__g.image_vbo);
    glBindVertexArray(_gles3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.image_vbo);

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
    // Mipmap chain + trilinear min filter -- same rationale as the
    // opengl3 twin. Kills the shimmer on heavily-downscaled photos.
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
    if (!_gles3_renderer_internal__ensure_image_pipeline())
    {
        return;
    }
    _gles3_renderer_internal__flush_batches();

    float x0 = (float)rect.x;
    float y0 = (float)rect.y;
    float x1 = x0 + (float)rect.w;
    float y1 = y0 + (float)rect.h;

    float verts[6 * 8] = {
        x0, y0, tint.r, tint.g, tint.b, tint.a, 0.0f, 0.0f,
        x1, y0, tint.r, tint.g, tint.b, tint.a, 1.0f, 0.0f,
        x0, y1, tint.r, tint.g, tint.b, tint.a, 0.0f, 1.0f,
        x1, y0, tint.r, tint.g, tint.b, tint.a, 1.0f, 0.0f,
        x1, y1, tint.r, tint.g, tint.b, tint.a, 1.0f, 1.0f,
        x0, y1, tint.r, tint.g, tint.b, tint.a, 0.0f, 1.0f,
    };

    glUseProgram(_gles3_renderer_internal__g.image_program);
    if (_gles3_renderer_internal__g.image_u_viewport_loc >= 0)
    {
        glUniform2f(_gles3_renderer_internal__g.image_u_viewport_loc, (float)_gles3_renderer_internal__g.viewport_w, (float)_gles3_renderer_internal__g.viewport_h);
    }
    if (_gles3_renderer_internal__g.image_u_tex_loc >= 0)
    {
        glUniform1i(_gles3_renderer_internal__g.image_u_tex_loc, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)tex);

    glBindVertexArray(_gles3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.image_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), NULL,  GL_DYNAMIC_DRAW);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}
