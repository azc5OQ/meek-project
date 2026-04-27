// ============================================================================
// d3d9_renderer.c - Direct3D 9 backend.
// ============================================================================
//
// WHAT THIS FILE DOES (the elevator pitch):
//
//   scene.c walks the widget tree and tells us "draw this colored rectangle
//   here" by calling renderer__submit_rect(). this file packages those calls
//   into work the GPU understands and ships them across using Direct3D 9 --
//   microsoft's previous-generation graphics api that's still ubiquitous on
//   Windows XP/Vista/7 and any GPU that predates D3D10's "feature levels".
//
//   this backend is interchangeable with opengl3_renderer.c and
//   d3d11_renderer.c -- exactly one is compiled into gui.dll at a time
//   (selected via the GUI_RENDERER CMake cache variable). all three
//   produce visually-identical output (see renderer.h "VISUAL CONTRACT").
//
// WHY KEEP D3D9 AROUND?
//
//   it's the lowest-common-denominator api on Windows. works on:
//     - Windows XP (last mainstream OS without D3D10+)
//     - integrated graphics from before ~2008
//     - virtual machines / remote desktop scenarios where modern api
//       support is patchy
//     - users who specifically install older drivers for stability
//
//   if the toolkit ever needs to ship to a wider audience than "modern
//   gaming GPUs", the D3D9 path keeps it accessible.
//
// D3D9 IN PLAIN C (not C++):
//
//   like d3d11, d3d9 is a COM api with C++-friendly headers. unlike d3d11,
//   d3d9 doesn't have a COBJMACROS option -- we have to write
//   `device->lpVtbl->Method(device, args...)` everywhere. it's noisy but
//   mechanical; the alternative (#define a per-method macro) would just be
//   reinventing what d3d11 ships in its header.
//
//   COM Release()/AddRef() rules apply: every pointer we get back must be
//   Release()'d when we're done. shutdown() walks every pointer in the
//   state struct and releases what's non-NULL.
//
// THE PIPELINE (very similar to d3d11's, with the differences below):
//
//   1. INPUT-ASSEMBLY: SetVertexDeclaration + SetStreamSource. d3d9
//      pre-dates d3d10's "input layout" concept, so we describe the
//      vertex format with a vertex declaration object instead.
//
//   2. VERTEX SHADER: shader model 3 (vs_3_0). compiled at runtime via
//      D3DCompile (same compiler as d3d11). constants are set via
//      SetVertexShaderConstantF (one float4 per call -- no constant
//      buffer object).
//
//   3. RASTERIZER: state set via SetRenderState calls (no rasterizer
//      state object). scissor enable lives here.
//
//   4. PIXEL SHADER: shader model 3 (ps_3_0). same SDF math as the other
//      backends. samplers via SetSamplerState (per-stage).
//
//   5. OUTPUT-MERGER: alpha blending via D3DRS_SRCBLEND / DESTBLEND
//      render state. no separate "blend state object" -- it's all a
//      bag of flags.
//
//   6. PRESENT: IDirect3DDevice9::Present, but the frame must be
//      bracketed by BeginScene / EndScene (a d3d9-ism that d3d10+
//      removed).
//
// D3D9 VS D3D11 / OPENGL -- DIFFERENCES THAT MATTER FOR THIS CODE:
//
//   1. NO "CONTEXT" OBJECT. IDirect3DDevice9 does both what d3d11's
//      Device and DeviceContext do.
//   2. VERTEX DECLARATION replaces d3d11's input layout / opengl's VAO.
//   3. CONSTANTS via SetVertexShaderConstantF (one float4 per slot),
//      not via cbuffers.
//   4. RENDER STATE is a bag of flags set with SetRenderState; no
//      "blend state object" / "rasterizer state object" the way d3d11
//      has. To swap blend modes or scissor enable, you set individual
//      flags.
//   5. PRESENT is IDirect3DDevice9::Present, and frames are bracketed
//      by BeginScene / EndScene -- forgetting either silently drops
//      every draw call. this caught us once.
//   6. PIXEL CENTER OFFSET. d3d9's rasterizer puts pixel centers at
//      INTEGER coordinates, unlike d3d10+/gl which put them at
//      HALF-INTEGER (n.5). to land on the same pixels, we subtract
//      0.5 from the vertex position in the vertex shader BEFORE the
//      NDC conversion. visible in the VS source string below as
//      `adj = i.pos - float2(0.5, 0.5)`. without this every rect
//      drawn at integer coordinates would be off by half a pixel
//      compared to the d3d11/opengl renderings.
//      ref: docs.microsoft.com -- "directly mapping texels to pixels"
//   7. LOST DEVICE. on certain events (user alt-tab in fullscreen,
//      some driver updates, display mode changes) the device becomes
//      "lost" and all POOL_DEFAULT resources must be released, then
//      Reset() called on the device, then resources recreated. for
//      the poc we handle it minimally: if Present returns
//      D3DERR_DEVICELOST we set device_lost=TRUE and stop drawing.
//      production code would release POOL_DEFAULT resources +
//      TestCooperativeLevel + Reset + recreate.
//
// VISUAL CONTRACT:
//
//   this backend must produce the same pixels as every other backend
//   given the same input -- see renderer.h's "VISUAL CONTRACT" section.
//   the SDF function, blend mode (note: SEPARATE alpha blend), smoothstep
//   range, scissor semantics, vertex ordering, and the half-pixel offset
//   are all specified; don't diverge from them.
//

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "renderer.h"
#include "_visual_contract.h"
#include "../clib/memory_manager.h"
#include "../third_party/log.h"

#define _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(p)      \
    do                                                \
    {                                                 \
        if ((p) != NULL)                              \
        {                                             \
            (p)->lpVtbl->Release(p);                  \
            (p) = NULL;                               \
        }                                             \
    } while (0)

//============================================================================
//VERTEX FORMAT
//============================================================================
//
//identical to the other backends: 11 floats (pos, color, local, rect
//size, radius). in d3d9 the vertex declaration binds the offsets to
//HLSL semantics.
//

typedef struct _d3d9_renderer_internal__vertex
{
    float x, y;
    float r, g, b, a;
    float lx, ly;
    float rect_w, rect_h;
    float radius;
} _d3d9_renderer_internal__vertex;

//
//text vertex: position + color + atlas UV. 6 per glyph quad.
//
typedef struct _d3d9_renderer_internal__text_vertex
{
    float x, y;
    float r, g, b, a;
    float u, v;
} _d3d9_renderer_internal__text_vertex;

#define _D3D9_RENDERER_INTERNAL__MAX_QUADS       2048
#define _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD  6

//
//text batch cap. matches the other backends (4096 glyphs) — sized for
//ARM/mobile-conscious working-set limits in spirit, even though this
//backend itself only runs on windows.
//
#define _D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096

//
//per-atlas runs. same pattern as the other backends: each run in the
//text VBO binds a different IDirect3DTexture9 before DrawPrimitive, so
//a frame can mix multiple fonts / sizes.
//
#define _D3D9_RENDERER_INTERNAL__MAX_TEXT_RUNS 64

typedef struct _d3d9_renderer_internal__text_run
{
    IDirect3DTexture9* atlas;
    int64              vert_start;
    int64              vert_count;
} _d3d9_renderer_internal__text_run;

//============================================================================
//HLSL SHADERS (shader model 3)
//============================================================================
//
//direct translation of the GLSL, with three d3d9-specific adjustments:
//
//  - VS output position semantic is "POSITION" (SM3) instead of the
//    d3d10+ "SV_POSITION".
//  - PS output semantic is "COLOR" instead of "SV_TARGET".
//  - vertex shader subtracts (0.5, 0.5) from input pixel coord before
//    the NDC conversion, to account for d3d9's integer-pixel-center
//    rasterization rule. WITHOUT this, every rect renders half a pixel
//    off and looks blurry.
//  - uniforms come from SetVertexShaderConstantF / SetPixelShaderConstantF,
//    not cbuffers. vs constant c0 holds the viewport (float2 + 2 unused).
//

static char* _d3d9_renderer_internal__vs_src =
    "float4 u_viewport_and_pad : register(c0);\n"
    "\n"
    "struct VSIn {\n"
    "    float2 pos : POSITION;\n"
    "    float4 col : COLOR0;\n"
    "    float2 lc  : TEXCOORD0;\n"
    "    float2 rs  : TEXCOORD1;\n"
    "    float  rad : TEXCOORD2;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : POSITION;\n"
    "    float4 col : COLOR0;\n"
    "    float2 lc  : TEXCOORD0;\n"
    "    float2 rs  : TEXCOORD1;\n"
    "    float  rad : TEXCOORD2;\n"
    "};\n"
    "\n"
    "VSOut vs_main(VSIn i) {\n"
    "    VSOut o;\n"
    "    float2 vp = u_viewport_and_pad.xy;\n"
    "    // d3d9 pixel-center correction: shift by -0.5px.\n"
    "    float2 adj = i.pos - float2(0.5, 0.5);\n"
    "    float2 ndc = float2(\n"
    "        (adj.x / vp.x) * 2.0 - 1.0,\n"
    "        1.0 - (adj.y / vp.y) * 2.0\n"
    "    );\n"
    "    o.pos = float4(ndc, 0.0, 1.0);\n"
    "    o.col = i.col;\n"
    "    o.lc  = i.lc;\n"
    "    o.rs  = i.rs;\n"
    "    o.rad = i.rad;\n"
    "    return o;\n"
    "}\n";

static char* _d3d9_renderer_internal__ps_src =
    "struct PSIn {\n"
    "    float4 col : COLOR0;\n"
    "    float2 lc  : TEXCOORD0;\n"
    "    float2 rs  : TEXCOORD1;\n"
    "    float  rad : TEXCOORD2;\n"
    "};\n"
    "\n"
    RENDERER_SDF_ROUND_BOX_HLSL
    "\n"
    "float4 ps_main(PSIn i) : COLOR {\n"
    "    float2 half_size = i.rs * 0.5;\n"
    "    float2 p = i.lc - half_size;\n"
    "    float d = sd_round_box(p, half_size, i.rad);\n"
    "    float aa = 1.0 - smoothstep(" RENDERER_SDF_AA_MIN ", " RENDERER_SDF_AA_MAX ", d);\n"
    "    if (aa <= 0.0) discard;\n"
    "    return float4(i.col.rgb, i.col.a * aa);\n"
    "}\n";

//
//TEXT SHADERS (HLSL SM3).
//  - vertex shader does the same -0.5 pixel-center adjustment as the
//    solid rect VS so glyph quads land on the same integer pixels as
//    the rects they're drawn on top of.
//  - pixel shader samples sampler s0. with D3DFMT_L8 the texel's .r is
//    the luminance (our atlas coverage byte); .rrrr is the same value
//    in all channels, so reading .r is correct.
//
static char* _d3d9_renderer_internal__text_vs_src =
    "float4 u_viewport_and_pad : register(c0);\n"
    "\n"
    "struct VSIn {\n"
    "    float2 pos : POSITION;\n"
    "    float4 col : COLOR0;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : POSITION;\n"
    "    float4 col : COLOR0;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VSOut vs_main(VSIn i) {\n"
    "    VSOut o;\n"
    "    float2 vp = u_viewport_and_pad.xy;\n"
    "    float2 adj = i.pos - float2(0.5, 0.5);\n"
    "    float2 ndc = float2(\n"
    "        (adj.x / vp.x) * 2.0 - 1.0,\n"
    "        1.0 - (adj.y / vp.y) * 2.0\n"
    "    );\n"
    "    o.pos = float4(ndc, 0.0, 1.0);\n"
    "    o.col = i.col;\n"
    "    o.uv  = i.uv;\n"
    "    return o;\n"
    "}\n";

//
// See opengl3_renderer.c text_fs_src for the full rationale on the
// pow(a, 1/2.2) step. Short version: we blend in sRGB space (the
// backbuffer is D3DFMT_A8R8G8B8, not gamma-corrected), which
// darkens partial-coverage edges. Reshaping the atlas coverage
// through the gamma curve produces edges that land at the
// perceptual midpoint instead of the sRGB-linear midpoint, so
// text stops looking washed-out on dark backgrounds.
//
static char* _d3d9_renderer_internal__text_ps_src =
    "sampler2D u_atlas : register(s0);\n"
    "\n"
    "struct PSIn {\n"
    "    float4 col : COLOR0;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "float4 ps_main(PSIn i) : COLOR {\n"
    "    float a = tex2D(u_atlas, i.uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    "    a = pow(a, 1.0 / 2.2);\n"
    "    return float4(i.col.rgb, i.col.a * a);\n"
    "}\n";

//============================================================================
//STATE
//============================================================================

typedef struct _d3d9_renderer_internal__state
{
    HWND hwnd;

    IDirect3D9*               d3d9;
    IDirect3DDevice9*         device;

    //
    //solid-rect pipeline.
    //
    IDirect3DVertexShader9*      vs;
    IDirect3DPixelShader9*       ps;
    IDirect3DVertexDeclaration9* decl;
    IDirect3DVertexBuffer9*      vbo;

    _d3d9_renderer_internal__vertex* cpu_verts;
    int64                            vert_count;
    int64                            vert_cap;

    int64 viewport_w;
    int64 viewport_h;

    //
    //last viewport dimensions we actually applied to the swap chain
    //via Device::Reset. begin_frame compares the incoming
    //viewport_w / viewport_h against these; if they differ, the
    //backbuffer is resized so content doesn't stretch. initialized
    //to 0/0 in renderer__init so the first frame's Reset kicks in
    //via the usual mismatch path.
    //
    int64 last_applied_w;
    int64 last_applied_h;

    boole device_lost; // stuck-lost flag: stops draws and Present attempts.

    //
    //text pipeline. atlas textures are D3DFMT_L8 in D3DPOOL_MANAGED
    //(survives device lost without explicit recovery).
    //
    //per-atlas run batching: the frame's text verts are partitioned
    //into runs, each tagged with its atlas texture; end_frame calls
    //SetTexture + DrawPrimitive once per run.
    //
    IDirect3DVertexShader9*      text_vs;
    IDirect3DPixelShader9*       text_ps;
    IDirect3DVertexDeclaration9* text_decl;
    IDirect3DVertexBuffer9*      text_vbo;
    IDirect3DTexture9*           current_text_atlas; // borrowed; set by set_text_atlas.

    _d3d9_renderer_internal__text_vertex* text_cpu_verts;
    int64                                 text_vert_count;
    int64                                 text_vert_cap;

    _d3d9_renderer_internal__text_run text_runs[_D3D9_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int64                             text_run_count;

    //
    // Image pipeline. Lazy-built on first renderer__submit_image.
    // Vertex format (pos + color + uv = 32 bytes) matches the text
    // pipeline -- same VS fits both. A separate vertex declaration
    // is needed because the image VS only declares 3 attributes,
    // not the 5 the text VS declares (no lx/ly/rect_w/rect_h/radius).
    //
    IDirect3DVertexShader9*      image_vs;
    IDirect3DPixelShader9*       image_ps;
    IDirect3DVertexDeclaration9* image_decl;
    IDirect3DVertexBuffer9*      image_vbo;   // 6-vert scratch.

    //
    // Scissor stack. D3D9 toggles scissor via D3DRS_SCISSORTESTENABLE
    // and SetScissorRect. We keep the render state TRUE permanently
    // (set in init) so we don't have to flip it on every push/pop;
    // the rect itself is what changes. When the stack is empty we
    // set the rect to the full viewport (no-op clip).
    //
    gui_rect scissor_stack[32];
    int64    scissor_depth;
} _d3d9_renderer_internal__state;

static _d3d9_renderer_internal__state _d3d9_renderer_internal__g;

//============================================================================
//FORWARD DECLS
//============================================================================

static boole _d3d9_renderer_internal__create_device(HWND hwnd);
static boole _d3d9_renderer_internal__build_pipeline(void);
static boole _d3d9_renderer_internal__build_text_pipeline(void);
static boole _d3d9_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob);
static void  _d3d9_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c);
static void  _d3d9_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c);
static void  _d3d9_renderer_internal__flush_batches(void);
static void  _d3d9_renderer_internal__apply_scissor_top(void);
static void  _d3d9_renderer_internal__apply_render_state(void);
static boole _d3d9_renderer_internal__reset_swapchain(int64 w, int64 h);

//============================================================================
//PUBLIC: renderer__init
//============================================================================

boole renderer__init(void* native_window)
{
    memset(&_d3d9_renderer_internal__g, 0, sizeof(_d3d9_renderer_internal__g));

    _d3d9_renderer_internal__g.hwnd = (HWND)native_window;
    if (_d3d9_renderer_internal__g.hwnd == NULL)
    {
        log_error("renderer__init got a NULL window handle");
        return FALSE;
    }

    RECT rc;
    GetClientRect(_d3d9_renderer_internal__g.hwnd, &rc);
    _d3d9_renderer_internal__g.viewport_w = rc.right - rc.left;
    _d3d9_renderer_internal__g.viewport_h = rc.bottom - rc.top;
    if (_d3d9_renderer_internal__g.viewport_w <= 0) _d3d9_renderer_internal__g.viewport_w = 1;
    if (_d3d9_renderer_internal__g.viewport_h <= 0) _d3d9_renderer_internal__g.viewport_h = 1;

    if (!_d3d9_renderer_internal__create_device(_d3d9_renderer_internal__g.hwnd))
    {
        return FALSE;
    }
    if (!_d3d9_renderer_internal__build_pipeline())
    {
        return FALSE;
    }

    return TRUE;
}

//============================================================================
//PUBLIC: renderer__shutdown
//============================================================================

void renderer__shutdown(void)
{
    //
    //text pipeline first (atlas textures are owned by font.c; it
    //releases them via renderer__destroy_atlas before we get here).
    //
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_vs);

    //
    // Image pipeline. Textures from renderer__create_texture_rgba
    // are owned by callers (released via renderer__destroy_texture).
    //
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_vs);
    //
    //current_text_atlas is a borrowed pointer -- not ours to release.
    //
    _d3d9_renderer_internal__g.current_text_atlas = NULL;

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vs);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.device);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.d3d9);

    if (_d3d9_renderer_internal__g.cpu_verts != NULL)
    {
        GUI_FREE(_d3d9_renderer_internal__g.cpu_verts);
    }
    if (_d3d9_renderer_internal__g.text_cpu_verts != NULL)
    {
        GUI_FREE(_d3d9_renderer_internal__g.text_cpu_verts);
    }
    memset(&_d3d9_renderer_internal__g, 0, sizeof(_d3d9_renderer_internal__g));
}

//============================================================================
//PUBLIC: renderer__begin_frame
//============================================================================

void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear)
{
    if (_d3d9_renderer_internal__g.device == NULL || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }

    //
    //resize the swap chain's backbuffer if the host window changed
    //size since the last frame. without this, the backbuffer stays
    //at its original dimensions and d3d9's Present stretches /
    //letterboxes it onto the new client area -- visibly bad on a
    //window drag that grows or shrinks the client rect. full Reset
    //flow lives in _reset_swapchain below (releases POOL_DEFAULT
    //VBs, calls Device::Reset with new pp, recreates VBs, reapplies
    //render state). mismatch against last_applied_* triggers it.
    //
    _d3d9_renderer_internal__g.viewport_w = viewport_w;
    _d3d9_renderer_internal__g.viewport_h = viewport_h;
    if (viewport_w != _d3d9_renderer_internal__g.last_applied_w ||
        viewport_h != _d3d9_renderer_internal__g.last_applied_h)
    {
        if (!_d3d9_renderer_internal__reset_swapchain(viewport_w, viewport_h))
        {
            //
            //reset_swapchain already logged + flagged device_lost;
            //nothing useful we can do here beyond bail.
            //
            return;
        }
    }

    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    //
    //d3d9's Clear takes an ARGB packed uint, not a float array.
    //convert the gui_color to 0xAARRGGBB.
    //
    DWORD cr = (DWORD)(clear.r * 255.0f + 0.5f) & 0xFF;
    DWORD cg = (DWORD)(clear.g * 255.0f + 0.5f) & 0xFF;
    DWORD cb = (DWORD)(clear.b * 255.0f + 0.5f) & 0xFF;
    DWORD ca = (DWORD)(clear.a * 255.0f + 0.5f) & 0xFF;
    D3DCOLOR packed = (ca << 24) | (cr << 16) | (cg << 8) | cb;

    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, packed, 1.0f, 0);

    //
    //viewport. d3d9 viewports use top-left origin (y-down).
    //
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width  = (DWORD)viewport_w;
    vp.Height = (DWORD)viewport_h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->lpVtbl->SetViewport(dev, &vp);

    //
    //d3d9 requires every frame's drawing to be bracketed by
    //BeginScene / EndScene. failing to do so silently drops draws.
    //
    dev->lpVtbl->BeginScene(dev);

    _d3d9_renderer_internal__g.vert_count      = 0;
    _d3d9_renderer_internal__g.text_vert_count = 0;
    _d3d9_renderer_internal__g.text_run_count  = 0;
    //
    //current_text_atlas is intentionally preserved across frames; the
    //first font__draw call each frame rebinds it anyway. text_runs is
    //cleared per-frame (each run belongs to the frame that produced it).
    //

    //
    // Initial scissor: full viewport (no-op clip). Required since
    // D3DRS_SCISSORTESTENABLE is permanently TRUE -- without a sane
    // rect, the GPU clips to whatever leftover scissor was last set.
    //
    _d3d9_renderer_internal__apply_scissor_top();
}

//============================================================================
//PUBLIC: renderer__submit_rect
//============================================================================

void renderer__submit_rect(gui_rect r, gui_color c, float radius)
{
    if (_d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    if (_d3d9_renderer_internal__g.vert_count + _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _d3d9_renderer_internal__g.vert_cap)
    {
        return;
    }

    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f) radius = 0.0f;
    if (radius > max_r) radius = max_r;

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    _d3d9_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
}

//============================================================================
//PUBLIC: renderer__end_frame
//============================================================================

//============================================================================
//INTERNAL: flush_batches
//============================================================================
//
//Draw whatever is currently buffered (solid rects + text glyphs), then
//reset the buffer counts so subsequent submissions start a fresh batch.
//Called from end_frame and from push/pop_scissor (any state change that
//can't be merged into a single DrawPrimitive forces a flush).
//

static void _d3d9_renderer_internal__flush_batches(void)
{
    if (_d3d9_renderer_internal__g.device == NULL || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    //
    //PASS 1: solid rects.
    //
    if (_d3d9_renderer_internal__g.vert_count > 0)
    {
        void*  mapped = NULL;
        UINT   bytes  = (UINT)(_d3d9_renderer_internal__g.vert_count * sizeof(_d3d9_renderer_internal__vertex));
        HRESULT hr = _d3d9_renderer_internal__g.vbo->lpVtbl->Lock(_d3d9_renderer_internal__g.vbo, 0, bytes, &mapped, D3DLOCK_DISCARD);
        if (SUCCEEDED(hr) && mapped != NULL)
        {
            memcpy(mapped, _d3d9_renderer_internal__g.cpu_verts, bytes);
            _d3d9_renderer_internal__g.vbo->lpVtbl->Unlock(_d3d9_renderer_internal__g.vbo);
        }

        dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.decl);
        dev->lpVtbl->SetStreamSource(dev, 0, _d3d9_renderer_internal__g.vbo, 0, (UINT)sizeof(_d3d9_renderer_internal__vertex));
        dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.vs);
        dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.ps);

        UINT primitive_count = (UINT)(_d3d9_renderer_internal__g.vert_count / 3);
        dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, primitive_count);

        _d3d9_renderer_internal__g.vert_count = 0;
    }

    //
    //PASS 2: text. one DrawPrimitive per atlas-run (multi-font support).
    //
    if (_d3d9_renderer_internal__g.text_vert_count > 0 &&
        _d3d9_renderer_internal__g.text_run_count > 0)
    {
        void*  tmapped = NULL;
        UINT   tbytes  = (UINT)(_d3d9_renderer_internal__g.text_vert_count * sizeof(_d3d9_renderer_internal__text_vertex));
        HRESULT thr = _d3d9_renderer_internal__g.text_vbo->lpVtbl->Lock(_d3d9_renderer_internal__g.text_vbo, 0, tbytes, &tmapped, D3DLOCK_DISCARD);
        if (SUCCEEDED(thr) && tmapped != NULL)
        {
            memcpy(tmapped, _d3d9_renderer_internal__g.text_cpu_verts, tbytes);
            _d3d9_renderer_internal__g.text_vbo->lpVtbl->Unlock(_d3d9_renderer_internal__g.text_vbo);
        }

        dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.text_decl);
        dev->lpVtbl->SetStreamSource(dev, 0, _d3d9_renderer_internal__g.text_vbo, 0, (UINT)sizeof(_d3d9_renderer_internal__text_vertex));
        dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.text_vs);
        dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.text_ps);

        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

        for (int64 i = 0; i < _d3d9_renderer_internal__g.text_run_count; i++)
        {
            _d3d9_renderer_internal__text_run* run = &_d3d9_renderer_internal__g.text_runs[i];
            if (run->vert_count <= 0 || run->atlas == NULL)
            {
                continue;
            }
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9*)run->atlas);
            UINT prim_count = (UINT)(run->vert_count / 3);
            dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, (UINT)run->vert_start, prim_count);
        }

        dev->lpVtbl->SetTexture(dev, 0, NULL);

        _d3d9_renderer_internal__g.text_vert_count    = 0;
        _d3d9_renderer_internal__g.text_run_count     = 0;
        _d3d9_renderer_internal__g.current_text_atlas = NULL;
    }
}

//============================================================================
//PUBLIC: renderer__push_scissor / renderer__pop_scissor
//============================================================================
//
//D3D9 toggles scissor via D3DRS_SCISSORTESTENABLE + SetScissorRect.
//We leave the render state TRUE permanently (set in init) so we don't
//have to flip it on every push/pop; the rect itself is what changes.
//When the stack becomes empty we set the rect to the full viewport
//(no-op clip).
//

static gui_rect _d3d9_renderer_internal__intersect(gui_rect a, gui_rect b)
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

static void _d3d9_renderer_internal__apply_scissor_top(void)
{
    if (_d3d9_renderer_internal__g.device == NULL || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    RECT sr;
    if (_d3d9_renderer_internal__g.scissor_depth <= 0)
    {
        sr.left   = 0;
        sr.top    = 0;
        sr.right  = (LONG)_d3d9_renderer_internal__g.viewport_w;
        sr.bottom = (LONG)_d3d9_renderer_internal__g.viewport_h;
    }
    else
    {
        gui_rect r = _d3d9_renderer_internal__g.scissor_stack[0];
        for (int64 i = 1; i < _d3d9_renderer_internal__g.scissor_depth; i++)
        {
            r = _d3d9_renderer_internal__intersect(r, _d3d9_renderer_internal__g.scissor_stack[i]);
        }
        sr.left   = (LONG)r.x;
        sr.top    = (LONG)r.y;
        sr.right  = (LONG)(r.x + r.w);
        sr.bottom = (LONG)(r.y + r.h);
        if (sr.right  < sr.left) { sr.right  = sr.left; }
        if (sr.bottom < sr.top)  { sr.bottom = sr.top;  }
    }
    dev->lpVtbl->SetScissorRect(dev, &sr);
}

void renderer__push_scissor(gui_rect rect)
{
    _d3d9_renderer_internal__flush_batches();
    if (_d3d9_renderer_internal__g.scissor_depth >= (int64)(sizeof(_d3d9_renderer_internal__g.scissor_stack) / sizeof(_d3d9_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    _d3d9_renderer_internal__g.scissor_stack[_d3d9_renderer_internal__g.scissor_depth++] = rect;
    _d3d9_renderer_internal__apply_scissor_top();
}

void renderer__pop_scissor(void)
{
    _d3d9_renderer_internal__flush_batches();
    if (_d3d9_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("renderer__pop_scissor: stack already empty");
        return;
    }
    _d3d9_renderer_internal__g.scissor_depth--;
    _d3d9_renderer_internal__apply_scissor_top();
}

void renderer__blur_region(gui_rect rect, float sigma_px)
{
    //
    // Placeholder: D3D9 doesn't wire up the capture + Gaussian pipeline
    // for true backdrop blur. Translucent darken splat as a stand-in so
    // the blur-styled region reads as "muted backdrop".
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
    if (_d3d9_renderer_internal__g.device == NULL || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    _d3d9_renderer_internal__flush_batches();
}

void renderer__end_frame(void)
{
    if (_d3d9_renderer_internal__g.device == NULL || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    //
    //upload the viewport constant. SetVertexShaderConstantF writes
    //`Vector4fCount` vec4s starting at register c0. we use one vec4:
    //(viewport_w, viewport_h, 0, 0).
    //
    float cb[4] = {
        (float)_d3d9_renderer_internal__g.viewport_w,
        (float)_d3d9_renderer_internal__g.viewport_h,
        0.0f,
        0.0f,
    };
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, cb, 1);

    //
    //flush any remaining batched submissions.
    //
    _d3d9_renderer_internal__flush_batches();

    //
    //defensive scissor reset: if a widget pushed but failed to pop,
    //reset to the full viewport so the next frame starts clean.
    //
    if (_d3d9_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("renderer__end_frame: scissor stack non-empty (%lld); resetting", (long long)_d3d9_renderer_internal__g.scissor_depth);
        _d3d9_renderer_internal__g.scissor_depth = 0;
        _d3d9_renderer_internal__apply_scissor_top();
    }

    dev->lpVtbl->EndScene(dev);

    //
    //present. on D3DERR_DEVICELOST, flip our flag -- subsequent frames
    //won't try to draw. production code would call TestCooperativeLevel
    //periodically and Reset() when the device is recoverable, but that
    //requires releasing all POOL_DEFAULT resources (our vbo) and
    //recreating them; we skip that for the poc.
    //
    HRESULT hr = dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET)
    {
        log_error("device lost; rendering halted (poc doesn't recover)");
        _d3d9_renderer_internal__g.device_lost = TRUE;
    }
}

//============================================================================
//INTERNAL: device creation
//============================================================================

static boole _d3d9_renderer_internal__create_device(HWND hwnd)
{
    _d3d9_renderer_internal__g.d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (_d3d9_renderer_internal__g.d3d9 == NULL)
    {
        log_error("Direct3DCreate9 failed");
        return FALSE;
    }

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth        = (UINT)_d3d9_renderer_internal__g.viewport_w;
    pp.BackBufferHeight       = (UINT)_d3d9_renderer_internal__g.viewport_h;
    pp.hDeviceWindow          = hwnd;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE; // vsync on.
    pp.EnableAutoDepthStencil = FALSE;                   // we don't use depth for 2d.

    HRESULT hr = _d3d9_renderer_internal__g.d3d9->lpVtbl->CreateDevice(
        _d3d9_renderer_internal__g.d3d9,
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &pp,
        &_d3d9_renderer_internal__g.device
    );
    if (FAILED(hr))
    {
        log_error("CreateDevice failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //seed last_applied_* to match the pp values we just created the
    //device with, so begin_frame's resize detection only trips when
    //the window actually changes size (not on the very first frame).
    //
    _d3d9_renderer_internal__g.last_applied_w = _d3d9_renderer_internal__g.viewport_w;
    _d3d9_renderer_internal__g.last_applied_h = _d3d9_renderer_internal__g.viewport_h;

    _d3d9_renderer_internal__apply_render_state();

    return TRUE;
}

//
//apply the full render-state bag the VISUAL CONTRACT requires.
//factored out of create_device so reset_swapchain can re-apply it
//after IDirect3DDevice9::Reset (Reset wipes render state back to
//defaults).
//
static void _d3d9_renderer_internal__apply_render_state(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL)
    {
        return;
    }

    //
    //alpha blending matching the VISUAL CONTRACT:
    //  - alpha blending: src=SRCALPHA, dst=INVSRCALPHA.
    //  - no culling (we don't bother with winding order).
    //  - fixed-function lighting off (irrelevant for shader path but
    //    some drivers warn if you leave it on).
    //
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP,   D3DBLENDOP_ADD);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE,  D3DCULL_NONE);

    //
    // Separate alpha blend so framebuffer alpha stays at 1.0 across
    // overlapping draws -- matches d3d11 and (post-fix) opengl3/gles3.
    // Without this, dst.a decays and DWM/compositor may treat the
    // window as semi-transparent during alpha animations.
    //
    dev->lpVtbl->SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLENDALPHA,  D3DBLEND_ONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOPALPHA,   D3DBLENDOP_ADD);

    //
    // Scissor: enable permanently. Each push/pop_scissor updates the
    // RECT itself via SetScissorRect; when the stack is empty, the
    // rect is set to the full viewport (no-op clip). Keeps the render
    // state stable across the frame -- no need to flip TRUE/FALSE on
    // each scissor change.
    //
    dev->lpVtbl->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING,  FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE,   D3DZB_FALSE);
}

//
//resize the swap chain's backbuffer to (w, h) via Device::Reset.
//without this, a resized window stretches / letterboxes the old
//backbuffer onto the new client area -- what the user observed as
//"d3d9 stretches when resizing, opengl3/d3d11 don't".
//
//Reset is a cranky API: every POOL_DEFAULT resource (both vertex
//buffers in our case) must be released first or Reset returns
//D3DERR_INVALIDCALL. render state also gets wiped and must be
//re-applied. POOL_MANAGED resources (our atlas textures) and the
//shaders + vertex declarations survive untouched.
//
static boole _d3d9_renderer_internal__reset_swapchain(int64 w, int64 h)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL)
    {
        return FALSE;
    }
    if (w <= 0) { w = 1; }
    if (h <= 0) { h = 1; }

    //
    //release POOL_DEFAULT resources. VERY IMPORTANT to do this BEFORE
    //Reset -- a single outstanding POOL_DEFAULT handle makes Reset
    //fail with D3DERR_INVALIDCALL and the device stays in a broken
    //state. we null the pointers so recreate can safely overwrite.
    //
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_vbo);

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth        = (UINT)w;
    pp.BackBufferHeight       = (UINT)h;
    pp.hDeviceWindow          = _d3d9_renderer_internal__g.hwnd;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
    pp.EnableAutoDepthStencil = FALSE;

    HRESULT hr = dev->lpVtbl->Reset(dev, &pp);
    if (FAILED(hr))
    {
        //
        //this is recoverable in theory -- another Reset might succeed
        //once the user stops dragging the window edge -- but for now
        //we flag the device lost and give up. the next successful
        //renderer__init after an app restart will rebuild everything.
        //
        log_error("IDirect3DDevice9::Reset failed (0x%08lX)", (unsigned long)hr);
        _d3d9_renderer_internal__g.device_lost = TRUE;
        return FALSE;
    }

    //
    //recreate the POOL_DEFAULT vertex buffers at their original caps.
    //same create-flags as the original init path.
    //
    UINT vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_QUADS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__vertex));
    hr = dev->lpVtbl->CreateVertexBuffer(
        dev,
        vbo_bytes,
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        0,
        D3DPOOL_DEFAULT,
        &_d3d9_renderer_internal__g.vbo,
        NULL
    );
    if (FAILED(hr))
    {
        log_error("CreateVertexBuffer (solid) post-Reset failed (0x%08lX)", (unsigned long)hr);
        _d3d9_renderer_internal__g.device_lost = TRUE;
        return FALSE;
    }

    UINT text_vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__text_vertex));
    hr = dev->lpVtbl->CreateVertexBuffer(
        dev,
        text_vbo_bytes,
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        0,
        D3DPOOL_DEFAULT,
        &_d3d9_renderer_internal__g.text_vbo,
        NULL
    );
    if (FAILED(hr))
    {
        log_error("CreateVertexBuffer (text) post-Reset failed (0x%08lX)", (unsigned long)hr);
        _d3d9_renderer_internal__g.device_lost = TRUE;
        return FALSE;
    }

    //
    //Reset clears render state, sampler state, and texture stage
    //state. re-apply the bag we rely on.
    //
    _d3d9_renderer_internal__apply_render_state();

    _d3d9_renderer_internal__g.last_applied_w = w;
    _d3d9_renderer_internal__g.last_applied_h = h;
    return TRUE;
}

//============================================================================
//INTERNAL: shader compilation
//============================================================================

static boole _d3d9_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob)
{
    ID3DBlob* err_blob = NULL;
    HRESULT   hr       = D3DCompile(
        src, strlen(src),
        NULL, NULL, NULL,
        entry, target,
        0, 0,
        out_blob, &err_blob
    );
    if (FAILED(hr))
    {
        if (err_blob != NULL)
        {
            log_error("shader compile error (%s):\n%s", entry, (char*)err_blob->lpVtbl->GetBufferPointer(err_blob));
            _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
        }
        else
        {
            log_error("D3DCompile failed (0x%08lX)", (unsigned long)hr);
        }
        return FALSE;
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
    return TRUE;
}

//============================================================================
//INTERNAL: pipeline setup
//============================================================================

static boole _d3d9_renderer_internal__build_pipeline(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    //
    //compile HLSL to shader model 3 bytecode.
    //
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    if (!_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__vs_src, "vs_main", "vs_3_0", &vs_blob))
    {
        return FALSE;
    }
    if (!_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__ps_src, "ps_main", "ps_3_0", &ps_blob))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    //
    //create shader objects from bytecode.
    //
    HRESULT hr;
    hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs_blob->lpVtbl->GetBufferPointer(vs_blob), &_d3d9_renderer_internal__g.vs);
    if (FAILED(hr))
    {
        log_error("CreateVertexShader failed (0x%08lX)", (unsigned long)hr);
        goto fail;
    }
    hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps_blob->lpVtbl->GetBufferPointer(ps_blob), &_d3d9_renderer_internal__g.ps);
    if (FAILED(hr))
    {
        log_error("CreatePixelShader failed (0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    //
    //vertex declaration. d3d9 matches elements to shader inputs by
    //semantic name + index, same as d3d11's input layout.
    //element stream=0 (the only stream we use), offsets in bytes,
    //type = FLOAT2/3/4/FLOAT1.
    //
    D3DVERTEXELEMENT9 elems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0,  8, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 40, D3DDECLTYPE_FLOAT1,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
        D3DDECL_END()
    };
    hr = dev->lpVtbl->CreateVertexDeclaration(dev, elems, &_d3d9_renderer_internal__g.decl);
    if (FAILED(hr))
    {
        log_error("CreateVertexDeclaration failed (0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    //
    //dynamic vertex buffer in POOL_DEFAULT. POOL_DEFAULT is the fast
    //GPU-memory pool but it's what gets lost on device lost.
    //POOL_MANAGED would survive Reset but is slower for dynamic data.
    //
    UINT vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_QUADS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__vertex));
    hr = dev->lpVtbl->CreateVertexBuffer(
        dev,
        vbo_bytes,
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        0,                       // FVF = 0 (we use a vertex declaration instead of fixed vertex format).
        D3DPOOL_DEFAULT,
        &_d3d9_renderer_internal__g.vbo,
        NULL
    );
    if (FAILED(hr))
    {
        log_error("CreateVertexBuffer failed (0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    //
    //cpu-side staging buffer.
    //
    _d3d9_renderer_internal__g.vert_cap  = _D3D9_RENDERER_INTERNAL__MAX_QUADS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d9_renderer_internal__g.cpu_verts = (_d3d9_renderer_internal__vertex*)GUI_MALLOC_T((size_t)_d3d9_renderer_internal__g.vert_cap * sizeof(_d3d9_renderer_internal__vertex), MM_TYPE_RENDERER);
    if (_d3d9_renderer_internal__g.cpu_verts == NULL)
    {
        log_error("out of memory for cpu vertex buffer");
        goto fail;
    }

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    //
    //text pipeline: separate shaders, VBO, vertex declaration.
    //
    if (!_d3d9_renderer_internal__build_text_pipeline())
    {
        return FALSE;
    }
    return TRUE;

fail:
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    return FALSE;
}

//============================================================================
//INTERNAL: text pipeline
//============================================================================
//
//compiles the text VS+PS, builds a vertex declaration for the 3-attribute
//text vertex format, allocates a dynamic text VBO + cpu staging buffer.
//sampler state for the atlas is set per draw in end_frame (d3d9 doesn't
//have a "sampler state object" -- state is scalar per-stage).
//

static boole _d3d9_renderer_internal__build_text_pipeline(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    if (!_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__text_vs_src, "vs_main", "vs_3_0", &vs_blob))
    {
        return FALSE;
    }
    if (!_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__text_ps_src, "ps_main", "ps_3_0", &ps_blob))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs_blob->lpVtbl->GetBufferPointer(vs_blob), &_d3d9_renderer_internal__g.text_vs);
    if (FAILED(hr))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("CreateVertexShader (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps_blob->lpVtbl->GetBufferPointer(ps_blob), &_d3d9_renderer_internal__g.text_ps);
    if (FAILED(hr))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("CreatePixelShader (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    //
    //text vertex declaration:
    //  POSITION @ 0   (float2)
    //  COLOR0   @ 8   (float4)
    //  TEXCOORD0 @ 24 (float2)
    //matches the HLSL VSIn struct and the _d3d9_renderer_internal__text_vertex layout.
    //
    D3DVERTEXELEMENT9 elems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0,  8, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    hr = dev->lpVtbl->CreateVertexDeclaration(dev, elems, &_d3d9_renderer_internal__g.text_decl);
    if (FAILED(hr))
    {
        log_error("CreateVertexDeclaration (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //dynamic text vertex buffer (POOL_DEFAULT, same tradeoff as the
    //solid VBO: fast but lost on device lost; recovery isn't wired up).
    //
    UINT vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__text_vertex));
    hr = dev->lpVtbl->CreateVertexBuffer(
        dev,
        vbo_bytes,
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        0,
        D3DPOOL_DEFAULT,
        &_d3d9_renderer_internal__g.text_vbo,
        NULL
    );
    if (FAILED(hr))
    {
        log_error("CreateVertexBuffer (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //cpu-side text staging buffer.
    //
    _d3d9_renderer_internal__g.text_vert_cap  = _D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d9_renderer_internal__g.text_cpu_verts = (_d3d9_renderer_internal__text_vertex*)GUI_MALLOC_T((size_t)_d3d9_renderer_internal__g.text_vert_cap * sizeof(_d3d9_renderer_internal__text_vertex), MM_TYPE_RENDERER);
    if (_d3d9_renderer_internal__g.text_cpu_verts == NULL)
    {
        log_error("out of memory for cpu text vertex buffer");
        return FALSE;
    }

    return TRUE;
}

//============================================================================
//INTERNAL: vertex append
//============================================================================

static void _d3d9_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c)
{
    _d3d9_renderer_internal__vertex* v = &_d3d9_renderer_internal__g.cpu_verts[_d3d9_renderer_internal__g.vert_count++];
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

static void _d3d9_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c)
{
    _d3d9_renderer_internal__text_vertex* tv = &_d3d9_renderer_internal__g.text_cpu_verts[_d3d9_renderer_internal__g.text_vert_count++];
    tv->x = x;
    tv->y = y;
    tv->r = c.r;
    tv->g = c.g;
    tv->b = c.b;
    tv->a = c.a;
    tv->u = u;
    tv->v = v;
}

//============================================================================
//PUBLIC: text pipeline
//============================================================================
//
//font.c produces an R8 atlas; d3d9 has no straight "one-channel 8-bit"
//format, but D3DFMT_L8 is functionally equivalent (luminance 8 -- the
//sampler returns .rrrr, and reading .r in the pixel shader gives us our
//coverage byte). we use D3DPOOL_MANAGED so the texture survives device
//lost without explicit recovery; the small upload cost is fine for
//one-time atlas creation.
//

void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    if (pixels == NULL || width <= 0 || height <= 0 || _d3d9_renderer_internal__g.device == NULL)
    {
        return NULL;
    }

    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    IDirect3DTexture9* tex = NULL;
    HRESULT hr = dev->lpVtbl->CreateTexture(
        dev,
        (UINT)width, (UINT)height,
        1,                  // levels (no mips).
        0,                  // usage.
        D3DFMT_L8,
        D3DPOOL_MANAGED,
        &tex,
        NULL
    );
    if (FAILED(hr) || tex == NULL)
    {
        log_error("CreateTexture (atlas) failed (0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    //
    //upload the bitmap. LockRect gives us a pointer + pitch; the driver
    //may have padded each row to a different stride than our source, so
    //we memcpy per row.
    //
    D3DLOCKED_RECT lr;
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (FAILED(hr))
    {
        log_error("LockRect (atlas) failed (0x%08lX)", (unsigned long)hr);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(tex);
        return NULL;
    }

    ubyte*       dst     = (ubyte*)lr.pBits;
    const ubyte* src     = pixels;
    size_t       row_len = (size_t)width; // R8 = 1 byte per pixel.
    for (int y = 0; y < height; y++)
    {
        memcpy(dst, src, row_len);
        dst += lr.Pitch;
        src += row_len;
    }
    tex->lpVtbl->UnlockRect(tex, 0);

    return (void*)tex;
}

void renderer__destroy_atlas(void* atlas)
{
    if (atlas == NULL)
    {
        return;
    }
    IDirect3DTexture9* tex = (IDirect3DTexture9*)atlas;
    //
    //clear our borrowed pointer if it matches, so the next frame's text
    //pass doesn't dereference a dangling texture.
    //
    if (_d3d9_renderer_internal__g.current_text_atlas == tex)
    {
        _d3d9_renderer_internal__g.current_text_atlas = NULL;
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(tex);
}

void renderer__set_text_atlas(void* atlas)
{
    _d3d9_renderer_internal__g.current_text_atlas = (IDirect3DTexture9*)atlas;
}

void renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color)
{
    if (_d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DTexture9* atlas = _d3d9_renderer_internal__g.current_text_atlas;
    if (atlas == NULL)
    {
        return;
    }
    if (_d3d9_renderer_internal__g.text_vert_count + _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _d3d9_renderer_internal__g.text_vert_cap)
    {
        return;
    }

    //
    //open or extend a run keyed by the current atlas texture.
    //
    _d3d9_renderer_internal__text_run* run = NULL;
    if (_d3d9_renderer_internal__g.text_run_count > 0)
    {
        run = &_d3d9_renderer_internal__g.text_runs[_d3d9_renderer_internal__g.text_run_count - 1];
        if (run->atlas != atlas)
        {
            run = NULL;
        }
    }
    if (run == NULL)
    {
        if (_d3d9_renderer_internal__g.text_run_count >= _D3D9_RENDERER_INTERNAL__MAX_TEXT_RUNS)
        {
            return;
        }
        run = &_d3d9_renderer_internal__g.text_runs[_d3d9_renderer_internal__g.text_run_count++];
        run->atlas      = atlas;
        run->vert_start = _d3d9_renderer_internal__g.text_vert_count;
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
    //same vertex ordering as solid rects (VISUAL CONTRACT).
    //
    _d3d9_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _d3d9_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d9_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _d3d9_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d9_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _d3d9_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    run->vert_count += _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

//============================================================================
//IMAGE PIPELINE (RGBA8 textures via D3D9)
//============================================================================
//
// Textures are IDirect3DTexture9 in D3DFMT_A8R8G8B8 (universally
// supported on D3D9 hardware), D3DPOOL_MANAGED so they survive a
// device reset without explicit recovery -- matches the text atlas
// pool choice. Upload via LockRect / memcpy-with-BGRA-swizzle /
// UnlockRect.
//
// Shader pair: VS is identical to the text pipeline's (pos + col +
// uv, viewport constant at c0 for screen-to-NDC). PS just does a
// full RGBA sample * color instead of the text path's .r * color.
// A dedicated vertex declaration declares only the 3 attributes
// the image VS uses (pos, color, uv) rather than the text pipeline's
// 5-attribute layout.
//

static char* _D3D9_RENDERER_INTERNAL__IMAGE_PS_SRC =
    "sampler2D u_tex : register(s0);\n"
    "\n"
    "struct PSIn {\n"
    "    float4 col : COLOR0;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "float4 ps_main(PSIn i) : COLOR {\n"
    "    float4 s = tex2D(u_tex, i.uv);\n"
    "    return s * i.col;\n"
    "}\n";

static boole _d3d9_renderer_internal__ensure_image_pipeline(void)
{
    if (_d3d9_renderer_internal__g.image_ps != NULL)
    {
        return TRUE;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL) { return FALSE; }

    //
    // Reuse the text VS source. Both pipelines share the same
    // screen-to-NDC transform + vertex format (pos + col + uv).
    //
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    if (!_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__text_vs_src, "vs_main", "vs_3_0", &vs_blob)) { return FALSE; }
    if (!_d3d9_renderer_internal__compile_shader(_D3D9_RENDERER_INTERNAL__IMAGE_PS_SRC, "ps_main", "ps_3_0", &ps_blob))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs_blob->lpVtbl->GetBufferPointer(vs_blob), &_d3d9_renderer_internal__g.image_vs);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    if (FAILED(hr))
    {
        log_error("image_vs CreateVertexShader failed (0x%08lX)", (unsigned long)hr);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        return FALSE;
    }
    hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps_blob->lpVtbl->GetBufferPointer(ps_blob), &_d3d9_renderer_internal__g.image_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    if (FAILED(hr))
    {
        log_error("image_ps CreatePixelShader failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    D3DVERTEXELEMENT9 elems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0,  8, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    hr = dev->lpVtbl->CreateVertexDeclaration(dev, elems, &_d3d9_renderer_internal__g.image_decl);
    if (FAILED(hr))
    {
        log_error("image decl failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    UINT vb_bytes = (UINT)(6 * 8 * sizeof(float));
    hr = dev->lpVtbl->CreateVertexBuffer(dev, vb_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.image_vbo, NULL);
    if (FAILED(hr))
    {
        log_error("image vbo failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    return TRUE;
}

void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height)
{
    if (rgba == NULL || width <= 0 || height <= 0) { return NULL; }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL) { return NULL; }

    //
    // Mip chain. Levels=0 asks D3D9 to auto-pick log2(max(w,h))+1.
    // D3DUSAGE_AUTOGENMIPMAP needs D3DPOOL_DEFAULT (not MANAGED),
    // with the side cost that a device reset (rare on our path)
    // would invalidate the texture. On the bright side AUTOGEN
    // rebuilds mips on the GPU each time the top level is touched,
    // so we don't have to call GenerateMips by hand.
    //
    // Without mipmaps, 6000x4000 photos downscaled to 48 px tiles
    // twinkle under D3DTEXF_LINEAR (each output pixel samples a
    // different 2x2 texel neighborhood each frame as scroll shifts).
    // LINEAR min + LINEAR mip = trilinear, which samples the nearest
    // pre-reduced level and kills the aliasing.
    //
    //
    // Two-attempt CreateTexture path. The first attempt uses
    // D3DPOOL_DEFAULT + D3DUSAGE_AUTOGENMIPMAP for the proper
    // mipmap chain, but D3DPOOL_DEFAULT textures can't be
    // LockRect'd directly (returns D3DERR_INVALIDCALL = 0x8876086C
    // on every modern driver -- they live in video memory and
    // need D3DUSAGE_DYNAMIC or a SYSTEMMEM staging texture +
    // UpdateTexture to upload to). Without dynamic usage the
    // upload path is "create temporary SYSTEMMEM, lock that,
    // copy from CPU, then call IDirect3DDevice9::UpdateTexture
    // to copy SYSTEMMEM -> DEFAULT" -- not yet wired here, so we
    // detect the LockRect failure on DEFAULT and reset to
    // MANAGED below.
    //
    // MANAGED pool is single-level (no autogen) but lockable,
    // and the runtime keeps a system-memory backing for restore
    // after device-lost. Sampling is pure bilinear -- some
    // shimmer on tiny downscales -- but the image at least
    // renders. This is the path we end up on for D3D9 in
    // practice.
    //
    IDirect3DTexture9* tex = NULL;
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, (UINT)width, (UINT)height, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    boole using_autogen = FALSE;
    if (SUCCEEDED(hr))
    {
        //
        // Filter used when rebuilding the mip chain. D3DTEXF_LINEAR
        // = box-filter downsample; the default is D3DTEXF_POINT
        // which would look blocky. Must be called after CreateTexture
        // but before LockRect/UnlockRect -- unlock triggers the
        // regen.
        //
        tex->lpVtbl->SetAutoGenFilterType(tex, D3DTEXF_LINEAR);
        using_autogen = TRUE;
    }
    else
    {
        log_warn("image CreateTexture(DEFAULT+AUTOGEN) failed (0x%08lX); trying MANAGED", (unsigned long)hr);
        hr = dev->lpVtbl->CreateTexture(dev, (UINT)width, (UINT)height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
        if (FAILED(hr))
        {
            log_error("image CreateTexture(MANAGED) failed (0x%08lX) for %dx%d", (unsigned long)hr, width, height);
            return NULL;
        }
    }

    D3DLOCKED_RECT locked;
    hr = tex->lpVtbl->LockRect(tex, 0, &locked, NULL, 0);
    if (FAILED(hr))
    {
        if (using_autogen)
        {
            //
            // Default-pool textures aren't lockable without
            // D3DUSAGE_DYNAMIC. Drop this one and retry with a
            // MANAGED-pool single-level texture which IS lockable.
            // No more spam: subsequent images of any size go
            // through the MANAGED branch on the first
            // CreateTexture attempt now (we still try DEFAULT
            // first per call -- this is the per-image fallback,
            // not a global "give up on autogen" flag).
            //
            tex->lpVtbl->Release(tex);
            tex = NULL;
            hr = dev->lpVtbl->CreateTexture(dev, (UINT)width, (UINT)height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
            if (FAILED(hr))
            {
                log_error("image MANAGED-fallback CreateTexture failed (0x%08lX) for %dx%d", (unsigned long)hr, width, height);
                return NULL;
            }
            hr = tex->lpVtbl->LockRect(tex, 0, &locked, NULL, 0);
        }
        if (FAILED(hr))
        {
            log_error("image LockRect failed (0x%08lX)", (unsigned long)hr);
            tex->lpVtbl->Release(tex);
            return NULL;
        }
    }

    //
    // Byte swizzle: stb_image hands us R,G,B,A byte order in memory.
    // D3DFMT_A8R8G8B8 on little-endian expects B,G,R,A. So per pixel
    // we rewrite (r, g, b, a) as (b, g, r, a). Plain loop; this
    // runs once per image at upload time, not per frame.
    //
    ubyte* dst_base = (ubyte*)locked.pBits;
    int stride = locked.Pitch;
    for (int y = 0; y < height; y++)
    {
        const ubyte* src = rgba + (size_t)y * (size_t)width * 4u;
        ubyte*       dst = dst_base + (size_t)y * (size_t)stride;
        for (int x = 0; x < width; x++)
        {
            dst[0] = src[2]; // B <- R_src is wrong... wait, source is RGBA, dst wants BGRA
            //
            // src layout: [R][G][B][A]. dst layout: [B][G][R][A].
            // so dst[0]=src[2], dst[1]=src[1], dst[2]=src[0], dst[3]=src[3].
            //
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            src += 4;
            dst += 4;
        }
    }
    tex->lpVtbl->UnlockRect(tex, 0);
    return (void*)tex;
}

void renderer__destroy_texture(void* tex)
{
    if (tex == NULL) { return; }
    IDirect3DTexture9* t = (IDirect3DTexture9*)tex;
    t->lpVtbl->Release(t);
}

void renderer__submit_image(gui_rect rect, void* tex, gui_color tint)
{
    if (tex == NULL) { return; }
    if (!_d3d9_renderer_internal__ensure_image_pipeline()) { return; }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL) { return; }

    //
    // Flush pending rect / text batches so their Draws land under
    // this image in submit order.
    //
    _d3d9_renderer_internal__flush_batches();

    //
    // Viewport constant into c0. flush_batches normally does this
    // before each draw, but text / rect paths may not have run this
    // frame; ensure the VS gets its viewport either way.
    //
    float cb[4] = {
        (float)_d3d9_renderer_internal__g.viewport_w,
        (float)_d3d9_renderer_internal__g.viewport_h,
        0.0f, 0.0f,
    };
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, cb, 1);

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    //
    // 6 verts, pos(2) col(4) uv(2) = 8 floats each.
    //
    float verts[6 * 8] = {
        x0, y0, tint.r, tint.g, tint.b, tint.a, 0.0f, 0.0f,
        x1, y0, tint.r, tint.g, tint.b, tint.a, 1.0f, 0.0f,
        x0, y1, tint.r, tint.g, tint.b, tint.a, 0.0f, 1.0f,
        x1, y0, tint.r, tint.g, tint.b, tint.a, 1.0f, 0.0f,
        x1, y1, tint.r, tint.g, tint.b, tint.a, 1.0f, 1.0f,
        x0, y1, tint.r, tint.g, tint.b, tint.a, 0.0f, 1.0f,
    };

    void* mapped = NULL;
    HRESULT hr = _d3d9_renderer_internal__g.image_vbo->lpVtbl->Lock(_d3d9_renderer_internal__g.image_vbo, 0, sizeof(verts), &mapped, D3DLOCK_DISCARD);
    if (FAILED(hr) || mapped == NULL) { return; }
    memcpy(mapped, verts, sizeof(verts));
    _d3d9_renderer_internal__g.image_vbo->lpVtbl->Unlock(_d3d9_renderer_internal__g.image_vbo);

    dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.image_decl);
    dev->lpVtbl->SetStreamSource(dev, 0, _d3d9_renderer_internal__g.image_vbo, 0, (UINT)(8 * sizeof(float)));
    dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.image_vs);
    dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.image_ps);

    //
    // Sampler state on stage 0. D3D9 has no sampler-state objects,
    // so we set each knob individually. MIPFILTER=LINEAR completes
    // the trilinear triangle (MIN + MAG + MIP all LINEAR) so the
    // mipmap chain we built in create_texture_rgba actually gets
    // sampled -- without it MIPFILTER_NONE ignores the mips and we
    // get the same bilinear-only shimmer the pre-mipmap build had.
    //
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
    dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    dev->lpVtbl->SetTexture(dev, 0, NULL);
}

//
// Shadow: multi-layer concentric rects with quadratic falloff.
// Same approach as opengl3/gles3/d3d11 -- stacks of submit_rect calls
// with increasing size and decreasing alpha approximate a Gaussian
// feather without needing a separate blur shader.
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

//
// Gradient: per-corner colors via the existing SDF-rect vertex format.
// The PS interpolates v_color across the triangle.
//
void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius)
{
    if (_d3d9_renderer_internal__g.vert_count + _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _d3d9_renderer_internal__g.vert_cap)
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
    _d3d9_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c_tl);
    _d3d9_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _d3d9_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
    _d3d9_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _d3d9_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c_br);
    _d3d9_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
}
