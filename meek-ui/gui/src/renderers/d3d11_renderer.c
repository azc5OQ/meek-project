// ============================================================================
// d3d11_renderer.c - Direct3D 11 backend.
// ============================================================================
//
// WHAT THIS FILE DOES (the elevator pitch):
//
//   scene.c walks the widget tree and tells us "draw this colored rectangle
//   here" by calling renderer__submit_rect(). this file packages those calls
//   into work the GPU understands and ships it across to the graphics card
//   using the Direct3D 11 api -- microsoft's "modern" graphics api on
//   Windows (replaced by D3D12 conceptually, but D3D11 ships on every
//   Windows 7+ box and is dramatically simpler to use).
//
//   this backend is interchangeable with opengl3_renderer.c and
//   d3d9_renderer.c -- exactly one is compiled into gui.dll at a time
//   (selected via the GUI_RENDERER CMake cache variable). all three
//   produce visually-identical output (see renderer.h "VISUAL CONTRACT").
//
// D3D11 IN PLAIN C (not C++):
//
//   d3d11 is a COM api and the headers are C++-friendly by default. we
//   define COBJMACROS before including <d3d11.h> which enables the C-style
//   macros like ID3D11Device_CreateBuffer(device, ...) instead of
//   device->lpVtbl->CreateBuffer(device, ...). the rest of the code looks
//   mostly like regular C.
//
//   every COM pointer we get back must be Release()'d when we're done. we
//   use the _SAFE_RELEASE macro below to null-check + release in one go,
//   so the shutdown path is resilient to init bailing halfway.
//
// THE PIPELINE (the rocket-stage analogy):
//
//   1. INPUT-ASSEMBLY (IA): tell D3D where the vertex bytes are and what
//      shape they have (vertex format, primitive topology). like loading
//      raw materials into the rocket -- "here's the data, here's its
//      schema".
//
//   2. VERTEX SHADER (VS): a tiny program that runs ONCE PER VERTEX. our
//      VS reads pixel coords from each vertex, converts to D3D's NDC
//      (-1..1 with y-up), and passes through color + the SDF "varyings"
//      that the pixel shader needs. running this on the GPU instead of
//      doing the math CPU-side avoids constant CPU-GPU round trips.
//
//   3. RASTERIZER (RS): the GPU's hardware fixed function. takes the
//      transformed vertices, figures out which pixels are inside the
//      triangles, and runs the pixel shader once per pixel. ScissorEnable
//      lives here -- when on, pixels outside the scissor RECT are
//      discarded before the pixel shader runs.
//
//   4. PIXEL SHADER (PS): runs ONCE PER PIXEL inside our triangles. ours
//      computes the SDF (signed distance function) for a rounded box,
//      antialiases the edge with a 1-pixel smoothstep, and writes the
//      final color (or `discard`s if outside the rounded shape -- that's
//      what makes the rounded corners actually rounded instead of
//      square-with-a-shaded-corner).
//
//   5. OUTPUT-MERGER (OM): the final stage. takes the pixel shader's
//      output and blends it with what's already in the render target
//      (this is where alpha blending happens). then writes the result
//      to the back buffer.
//
//   6. PRESENT: hand the back buffer to the OS to display on screen.
//      the back buffer becomes the front buffer; the previous front
//      becomes the new back. this is the "swap" in "swap chain".
//
// WHY BATCHING?
//
//   each call from CPU to GPU costs hundreds of microseconds of overhead --
//   driver bookkeeping, command queue latency. drawing 100 rects with 100
//   separate draw calls is dramatically slower than gathering them into a
//   list of 600 vertices and issuing ONE draw call:
//
//       begin_frame:    Map the back buffer for clearing, reset our
//                       cpu-side vertex list.
//       submit_rect *N: append 6 vertices to the list per rectangle.
//       end_frame:      flush_batches uploads the WHOLE vertex list to
//                       the GPU vertex buffer (one Map+memcpy+Unmap),
//                       sets pipeline state once, issues ONE Draw, then
//                       Present.
//
//   the per-batch flush only fragments when state changes mid-frame
//   (push_scissor or set_text_atlas during the recursion). each flush
//   draws what's accumulated so far before changing GPU state.
//
// RESIZE HANDLING:
//
//   unlike opengl (where the swapchain auto-tracks the window), d3d11
//   requires an explicit call to IDXGISwapChain::ResizeBuffers plus
//   recreating the render-target view after a window resize. we detect
//   size changes at the start of each frame by comparing the viewport
//   passed to begin_frame against what we saw last frame.
//
// VISUAL CONTRACT:
//
//   this backend must produce the same pixels as every other backend
//   given the same input -- see renderer.h's "VISUAL CONTRACT" section.
//   the SDF function, blend mode, smoothstep range, scissor semantics,
//   and radius clamping are all specified; don't diverge from them.
//

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "renderer.h"
#include "_visual_contract.h"
#include "../clib/memory_manager.h"
#include "../third_party/log.h"

//
//convenience: release a COM ptr and null it out. the "(IUnknown*)" cast
//is to satisfy strict compilers; IUnknown_Release works on any COM iface.
//
#define _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(p)           \
    do                                                      \
    {                                                       \
        if ((p) != NULL)                                    \
        {                                                   \
            IUnknown_Release((IUnknown*)(p));               \
            (p) = NULL;                                     \
        }                                                   \
    } while (0)

//============================================================================
//VERTEX FORMAT
//============================================================================
//
//identical to the OpenGL backend's vertex. the 11 floats map to the same
//shader inputs (POSITION, COLOR, TEXCOORD0/1/2 for local/rect_size/radius).
//

typedef struct _d3d11_renderer_internal__vertex
{
    float x, y;             // pixel position, top-left origin.
    float r, g, b, a;       // rgba, linear 0..1.
    float lx, ly;           // local position inside the rect, 0..rect_w/h.
    float rect_w, rect_h;   // rect size, passed through as varying for the fragment SDF.
    float radius;           // corner radius in pixels.
} _d3d11_renderer_internal__vertex;

//
//text vertex: simpler layout (position + color + uv). one per corner of
//each glyph quad. 6 per glyph.
//
typedef struct _d3d11_renderer_internal__text_vertex
{
    float x, y;           // pixel position, top-left origin.
    float r, g, b, a;     // text color (atlas supplies alpha/coverage).
    float u, v;           // UV in 0..1 into the atlas texture.
} _d3d11_renderer_internal__text_vertex;

#define _D3D11_RENDERER_INTERNAL__MAX_QUADS       2048
#define _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD  6

//
//text batch cap. matches the opengl3 backend (4096 glyphs) for the same
//ARM/mobile-conscious reason -- small enough to stay under embedded-GPU
//working-set limits.
//
#define _D3D11_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096

//
//text runs: one per (atlas SRV) used in the frame. see opengl3_renderer
//comments -- same pattern. each run maps to one Draw call with the
//matching SRV bound to PS register t0.
//
#define _D3D11_RENDERER_INTERNAL__MAX_TEXT_RUNS 64

typedef struct _d3d11_renderer_internal__text_run
{
    ID3D11ShaderResourceView* atlas;       // SRV bound for this run's draw.
    int64                     vert_start;  // offset into text_cpu_verts.
    int64                     vert_count;  // verts in this run.
} _d3d11_renderer_internal__text_run;

//============================================================================
//CONSTANT BUFFER LAYOUT
//============================================================================
//
//d3d11 constant buffers are 16-byte-aligned. we only need a float2 but
//pad to float4 (16 bytes).
//

typedef struct _d3d11_renderer_internal__cb_globals
{
    float viewport_w;
    float viewport_h;
    float _padding_0;
    float _padding_1;
} _d3d11_renderer_internal__cb_globals;

//============================================================================
//HLSL SHADER SOURCES
//============================================================================
//
//direct translation of the GLSL shaders in opengl3_renderer.c. differences:
//  - HLSL uses "float2/float3/float4" instead of "vec2/vec3/vec4".
//  - semantics (POSITION, COLOR, TEXCOORDn, SV_POSITION, SV_TARGET) link
//    shader inputs/outputs to the input-layout slots we create below.
//  - the NDC conversion is identical -- d3d11 NDC is y-up with top-left
//    origin (same as opengl after our vertex shader's y-flip), so the
//    same math produces the same result.
//

static char* _d3d11_renderer_internal__vs_src =
    "cbuffer Globals : register(b0) {\n"
    "    float4 u_viewport_and_pad;\n" // .xy = viewport in pixels, .zw = padding.
    "};\n"
    "\n"
    "struct VSIn {\n"
    "    float2 pos : POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 lc  : TEXCOORD0;\n"
    "    float2 rs  : TEXCOORD1;\n"
    "    float  rad : TEXCOORD2;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 lc  : TEXCOORD0;\n"
    "    float2 rs  : TEXCOORD1;\n"
    "    float  rad : TEXCOORD2;\n"
    "};\n"
    "\n"
    "VSOut vs_main(VSIn i) {\n"
    "    VSOut o;\n"
    "    float2 vp = u_viewport_and_pad.xy;\n"
    "    float2 ndc = float2(\n"
    "        (i.pos.x / vp.x) * 2.0 - 1.0,\n"
    "        1.0 - (i.pos.y / vp.y) * 2.0\n"
    "    );\n"
    "    o.pos = float4(ndc, 0.0, 1.0);\n"
    "    o.col = i.col;\n"
    "    o.lc  = i.lc;\n"
    "    o.rs  = i.rs;\n"
    "    o.rad = i.rad;\n"
    "    return o;\n"
    "}\n";

static char* _d3d11_renderer_internal__ps_src =
    "struct PSIn {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 lc  : TEXCOORD0;\n"
    "    float2 rs  : TEXCOORD1;\n"
    "    float  rad : TEXCOORD2;\n"
    "};\n"
    "\n"
    RENDERER_SDF_ROUND_BOX_HLSL
    "\n"
    "float4 ps_main(PSIn i) : SV_TARGET {\n"
    "    float2 half_size = i.rs * 0.5;\n"
    "    float2 p = i.lc - half_size;\n"
    "    float d = sd_round_box(p, half_size, i.rad);\n"
    "    float aa = 1.0 - smoothstep(" RENDERER_SDF_AA_MIN ", " RENDERER_SDF_AA_MAX ", d);\n"
    "    if (aa <= 0.0) discard;\n"
    "    return float4(i.col.rgb, i.col.a * aa);\n"
    "}\n";

//
//TEXT SHADERS (HLSL SM5). direct port of the GL text shaders.
//  - single-channel atlas texture bound to t0.
//  - LINEAR-filtering sampler bound to s0.
//  - same pixel -> NDC conversion as the solid vs.
//  - output color = vertex_color.rgb + (vertex_color.a * atlas.r) alpha.
//
static char* _d3d11_renderer_internal__text_vs_src =
    "cbuffer Globals : register(b0) {\n"
    "    float4 u_viewport_and_pad;\n"
    "};\n"
    "\n"
    "struct VSIn {\n"
    "    float2 pos : POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VSOut vs_main(VSIn i) {\n"
    "    VSOut o;\n"
    "    float2 vp = u_viewport_and_pad.xy;\n"
    "    float2 ndc = float2(\n"
    "        (i.pos.x / vp.x) * 2.0 - 1.0,\n"
    "        1.0 - (i.pos.y / vp.y) * 2.0\n"
    "    );\n"
    "    o.pos = float4(ndc, 0.0, 1.0);\n"
    "    o.col = i.col;\n"
    "    o.uv  = i.uv;\n"
    "    return o;\n"
    "}\n";

//
// See opengl3_renderer.c text_fs_src for the full rationale on the
// pow(a, 1/2.2) step. Short version: we blend in sRGB space (the
// swap chain is DXGI_FORMAT_R8G8B8A8_UNORM, not _SRGB), which
// darkens partial-coverage edges. Reshaping the atlas coverage
// through the gamma curve produces edges that land at the
// perceptual midpoint instead of the sRGB-linear midpoint, so
// text stops looking washed-out on dark backgrounds.
//
static char* _d3d11_renderer_internal__text_ps_src =
    "Texture2D    u_atlas : register(t0);\n"
    "SamplerState u_samp  : register(s0);\n"
    "\n"
    "struct PSIn {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "float4 ps_main(PSIn i) : SV_TARGET {\n"
    "    float a = u_atlas.Sample(u_samp, i.uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    "    a = pow(a, 1.0 / 2.2);\n"
    "    return float4(i.col.rgb, i.col.a * a);\n"
    "}\n";

//============================================================================
//STATE
//============================================================================

typedef struct _d3d11_renderer_internal__state
{
    HWND hwnd;

    // core objects
    ID3D11Device*           device;
    ID3D11DeviceContext*    context;
    IDXGISwapChain*         swapchain;
    ID3D11RenderTargetView* rtv;

    // solid-rect pipeline
    ID3D11VertexShader* vs;
    ID3D11PixelShader*  ps;
    ID3D11InputLayout*  input_layout;
    ID3D11Buffer*       vbo;       // dynamic vertex buffer
    ID3D11Buffer*       cbuffer;   // per-frame globals (viewport)
    ID3D11BlendState*   blend_state;

    // cpu side
    _d3d11_renderer_internal__vertex* cpu_verts;
    int64                             vert_count;
    int64                             vert_cap;

    int64 viewport_w;
    int64 viewport_h;

    //
    //text pipeline. mirrors the solid-rect set but has its own shaders,
    //input layout, and VBO. also owns a sampler state for the atlas and
    //a cached "currently bound atlas" SRV pointer.
    //
    //per-atlas run batching: multiple fonts/sizes per frame land in
    //the same VBO as a sequence of runs, each tagged with its SRV.
    //end_frame binds each SRV in turn and issues one Draw per run.
    //
    ID3D11VertexShader*       text_vs;
    ID3D11PixelShader*        text_ps;
    ID3D11InputLayout*        text_input_layout;
    ID3D11Buffer*             text_vbo;
    ID3D11SamplerState*       text_sampler;
    ID3D11ShaderResourceView* current_text_atlas; // not owned; set by renderer__set_text_atlas.

    _d3d11_renderer_internal__text_vertex* text_cpu_verts;
    int64                                  text_vert_count;
    int64                                  text_vert_cap;

    _d3d11_renderer_internal__text_run text_runs[_D3D11_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int64                              text_run_count;

    //
    // Image pipeline. Lazy-built on first renderer__submit_image call
    // so scenes that don't use <image> / background-image don't pay
    // for the shader compile + VBO alloc. Reuses the text sampler
    // (same LINEAR / CLAMP knobs) and shares the same vertex layout
    // (pos + color + uv = 32 bytes), just a separate PS that samples
    // the texture's full RGBA instead of text's alpha-only .r channel.
    //
    ID3D11VertexShader* image_vs;
    ID3D11PixelShader*  image_ps;
    ID3D11InputLayout*  image_input_layout;
    ID3D11Buffer*       image_vbo;    // 6-vert scratch, reused per submit.

    //
    // Scissor stack + rasterizer state. The rasterizer state has
    // ScissorEnable=TRUE so the GPU clips to whatever rect was last
    // set via RSSetScissorRects. When the stack is empty the scissor
    // rect is set to the full viewport (which is a no-op clip),
    // so we don't have to swap rasterizer states on every push/pop.
    //
    ID3D11RasterizerState* rasterizer_state;
    gui_rect               scissor_stack[32];
    int64                  scissor_depth;
} _d3d11_renderer_internal__state;

static _d3d11_renderer_internal__state _d3d11_renderer_internal__g;

//============================================================================
//FORWARD DECLS
//============================================================================

static boole    _d3d11_renderer_internal__create_device_and_swapchain(HWND hwnd);
static boole    _d3d11_renderer_internal__create_rtv_from_backbuffer(void);
static boole    _d3d11_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob);
static boole    _d3d11_renderer_internal__build_pipeline(void);
static boole    _d3d11_renderer_internal__build_text_pipeline(void);
static boole    _d3d11_renderer_internal__handle_resize(int64 new_w, int64 new_h);
static void     _d3d11_renderer_internal__upload_constant_buffer(void);
static void     _d3d11_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c);
static void     _d3d11_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c);
static void     _d3d11_renderer_internal__flush_batches(void);
static void     _d3d11_renderer_internal__apply_scissor_top(void);

//============================================================================
//PUBLIC: renderer__init
//============================================================================

boole renderer__init(void* native_window)
{
    memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));

    _d3d11_renderer_internal__g.hwnd = (HWND)native_window;
    if (_d3d11_renderer_internal__g.hwnd == NULL)
    {
        log_error("renderer__init got a NULL window handle");
        return FALSE;
    }

    //
    //look up the initial client-area size from the window. d3d needs to
    //know it up front to size the back buffer. resize is handled later.
    //
    RECT rc;
    GetClientRect(_d3d11_renderer_internal__g.hwnd, &rc);
    _d3d11_renderer_internal__g.viewport_w = rc.right - rc.left;
    _d3d11_renderer_internal__g.viewport_h = rc.bottom - rc.top;
    if (_d3d11_renderer_internal__g.viewport_w <= 0) _d3d11_renderer_internal__g.viewport_w = 1;
    if (_d3d11_renderer_internal__g.viewport_h <= 0) _d3d11_renderer_internal__g.viewport_h = 1;

    if (!_d3d11_renderer_internal__create_device_and_swapchain(_d3d11_renderer_internal__g.hwnd))
    {
        return FALSE;
    }
    if (!_d3d11_renderer_internal__create_rtv_from_backbuffer())
    {
        return FALSE;
    }
    if (!_d3d11_renderer_internal__build_pipeline())
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
    //text pipeline. atlas textures/SRVs are owned by font.c (destroyed
    //via renderer__destroy_atlas before we get here).
    //
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_sampler);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_vs);

    //
    // Image pipeline. texture SRVs are owned by callers
    // (widget_image via user_data, image cache globally); caller
    // releases via renderer__destroy_texture.
    //
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_vs);
    //
    //current_text_atlas is a borrowed pointer (not ours to release).
    //
    _d3d11_renderer_internal__g.current_text_atlas = NULL;

    //
    //solid-rect pipeline.
    //
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rasterizer_state);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blend_state);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.cbuffer);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.vs);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rtv);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.swapchain);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.context);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.device);

    if (_d3d11_renderer_internal__g.cpu_verts != NULL)
    {
        GUI_FREE(_d3d11_renderer_internal__g.cpu_verts);
    }
    if (_d3d11_renderer_internal__g.text_cpu_verts != NULL)
    {
        GUI_FREE(_d3d11_renderer_internal__g.text_cpu_verts);
    }
    memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));
}

//============================================================================
//PUBLIC: renderer__begin_frame
//============================================================================

void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear)
{
    //
    //handle window resize. if the platform says the viewport is now a
    //different size than our swapchain was created for, we have to
    //ResizeBuffers the swapchain and recreate the RTV against the new
    //back buffer.
    //
    if (viewport_w != _d3d11_renderer_internal__g.viewport_w ||
        viewport_h != _d3d11_renderer_internal__g.viewport_h)
    {
        _d3d11_renderer_internal__handle_resize(viewport_w, viewport_h);
    }

    _d3d11_renderer_internal__g.vert_count      = 0;
    _d3d11_renderer_internal__g.text_vert_count = 0;
    _d3d11_renderer_internal__g.text_run_count  = 0;
    //
    //current_text_atlas is intentionally preserved across frames; the
    //first font__draw of each frame rebinds it anyway. runs reset per
    //frame (each run belongs to the frame that produced it).
    //

    //
    //bind the render target, clear it. OMSetRenderTargets sets where
    //pixel-shader output lands; ClearRenderTargetView fills that
    //target with the clear color.
    //
    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 1, &_d3d11_renderer_internal__g.rtv, NULL);

    FLOAT clear_rgba[4] = { clear.r, clear.g, clear.b, clear.a };
    ID3D11DeviceContext_ClearRenderTargetView(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rtv, clear_rgba);

    //
    //set the viewport (the area of the RTV we'll draw into). d3d11
    //viewports use top-left origin with y-down, same as our pixel coords.
    //
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = (FLOAT)viewport_w;
    vp.Height   = (FLOAT)viewport_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(_d3d11_renderer_internal__g.context, 1, &vp);

    //
    // Initial scissor: full viewport. The rasterizer state has
    // ScissorEnable=TRUE permanently (so we don't have to swap states
    // on every push/pop), which means there ALWAYS needs to be a sane
    // scissor rect bound. Each push/pop updates it; here we set the
    // no-op (full viewport) starting state so the first batch isn't
    // accidentally clipped to nothing.
    //
    _d3d11_renderer_internal__apply_scissor_top();
}

//============================================================================
//PUBLIC: renderer__submit_rect
//============================================================================
//
//identical cpu-side logic to opengl3_renderer. vertex layout + clamp +
//triangle winding are all part of the VISUAL CONTRACT.
//

void renderer__submit_rect(gui_rect r, gui_color c, float radius)
{
    if (_d3d11_renderer_internal__g.vert_count + _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _d3d11_renderer_internal__g.vert_cap)
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

    _d3d11_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
}

//============================================================================
//PUBLIC: renderer__end_frame
//============================================================================
//
//1. upload this frame's vertices to the dynamic vertex buffer.
//2. upload viewport size to the constant buffer.
//3. set the pipeline state (shaders, input layout, VB, CB, blend, topology).
//4. Draw.
//5. Present.
//
//Map uses D3D11_MAP_WRITE_DISCARD which tells the driver "throw away the
//old contents of this buffer, give me fresh gpu memory to write into". the
//driver can reuse the old memory or allocate new -- either way, it
//avoids a GPU/CPU sync and is the standard pattern for per-frame buffers.
//

//============================================================================
//INTERNAL: flush_batches
//============================================================================
//
//Draw whatever is currently buffered (solid rects + text glyphs), then
//reset the buffer counts so subsequent submissions start a fresh batch.
//Called from end_frame and from push/pop_scissor (any state change that
//can't be merged into a single Draw forces a flush).
//

static void _d3d11_renderer_internal__flush_batches(void)
{
    //
    //PASS 1: solid rects.
    //
    if (_d3d11_renderer_internal__g.vert_count > 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            size_t bytes = (size_t)_d3d11_renderer_internal__g.vert_count * sizeof(_d3d11_renderer_internal__vertex);
            memcpy(mapped.pData, _d3d11_renderer_internal__g.cpu_verts, bytes);
            ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.vbo, 0);
        }

        ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.input_layout);

        UINT stride = (UINT)sizeof(_d3d11_renderer_internal__vertex);
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.vbo, &stride, &offset);

        ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.ps, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.cbuffer);

        FLOAT blend_factor[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_OMSetBlendState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.blend_state, blend_factor, 0xFFFFFFFF);
        ID3D11DeviceContext_RSSetState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rasterizer_state);

        ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, (UINT)_d3d11_renderer_internal__g.vert_count, 0);

        _d3d11_renderer_internal__g.vert_count = 0;
    }

    //
    //PASS 2: text. one Draw per atlas-run (multi-font support).
    //
    if (_d3d11_renderer_internal__g.text_vert_count > 0 &&
        _d3d11_renderer_internal__g.text_run_count > 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.text_vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            size_t bytes = (size_t)_d3d11_renderer_internal__g.text_vert_count * sizeof(_d3d11_renderer_internal__text_vertex);
            memcpy(mapped.pData, _d3d11_renderer_internal__g.text_cpu_verts, bytes);
            ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.text_vbo, 0);
        }

        ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.text_input_layout);

        UINT tstride = (UINT)sizeof(_d3d11_renderer_internal__text_vertex);
        UINT toffset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.text_vbo, &tstride, &toffset);

        ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.text_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.text_ps, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.cbuffer);
        ID3D11DeviceContext_PSSetSamplers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.text_sampler);

        FLOAT blend_factor[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_OMSetBlendState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.blend_state, blend_factor, 0xFFFFFFFF);
        ID3D11DeviceContext_RSSetState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rasterizer_state);

        for (int64 i = 0; i < _d3d11_renderer_internal__g.text_run_count; i++)
        {
            _d3d11_renderer_internal__text_run* run = &_d3d11_renderer_internal__g.text_runs[i];
            if (run->vert_count <= 0 || run->atlas == NULL)
            {
                continue;
            }
            ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &run->atlas);
            ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, (UINT)run->vert_count, (UINT)run->vert_start);
        }

        ID3D11ShaderResourceView* null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &null_srv);

        _d3d11_renderer_internal__g.text_vert_count    = 0;
        _d3d11_renderer_internal__g.text_run_count     = 0;
        _d3d11_renderer_internal__g.current_text_atlas = NULL;
    }
}

//============================================================================
//PUBLIC: renderer__push_scissor / renderer__pop_scissor
//============================================================================
//
//Bracket a region of submissions whose draws are clipped to the given
//rect. Each push/pop forces a flush of whatever is currently batched,
//then changes the GPU scissor rect via RSSetScissorRects. The
//rasterizer state already has ScissorEnable=TRUE (set in init), so the
//rect is always honored.
//
//Nesting takes the INTERSECTION of all stacked rects -- a scrollable
//child div inside a scrollable parent div correctly clips to the
//smaller of the two regions.
//
//When the stack becomes empty (or when no scissor was ever pushed),
//we still call RSSetScissorRects with the full viewport rect so the
//"always-on" rasterizer state has a sensible no-op clip.
//

static gui_rect _d3d11_renderer_internal__intersect(gui_rect a, gui_rect b)
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

static void _d3d11_renderer_internal__apply_scissor_top(void)
{
    D3D11_RECT sr;
    if (_d3d11_renderer_internal__g.scissor_depth <= 0)
    {
        //
        //empty stack: scissor covers the full viewport so the
        //"always-on" rasterizer state behaves as if disabled.
        //
        sr.left   = 0;
        sr.top    = 0;
        sr.right  = (LONG)_d3d11_renderer_internal__g.viewport_w;
        sr.bottom = (LONG)_d3d11_renderer_internal__g.viewport_h;
    }
    else
    {
        gui_rect r = _d3d11_renderer_internal__g.scissor_stack[0];
        for (int64 i = 1; i < _d3d11_renderer_internal__g.scissor_depth; i++)
        {
            r = _d3d11_renderer_internal__intersect(r, _d3d11_renderer_internal__g.scissor_stack[i]);
        }
        sr.left   = (LONG)r.x;
        sr.top    = (LONG)r.y;
        sr.right  = (LONG)(r.x + r.w);
        sr.bottom = (LONG)(r.y + r.h);
        if (sr.right  < sr.left) { sr.right  = sr.left; }
        if (sr.bottom < sr.top)  { sr.bottom = sr.top;  }
    }
    ID3D11DeviceContext_RSSetScissorRects(_d3d11_renderer_internal__g.context, 1, &sr);
}

void renderer__push_scissor(gui_rect rect)
{
    _d3d11_renderer_internal__flush_batches();
    if (_d3d11_renderer_internal__g.scissor_depth >= (int64)(sizeof(_d3d11_renderer_internal__g.scissor_stack) / sizeof(_d3d11_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    _d3d11_renderer_internal__g.scissor_stack[_d3d11_renderer_internal__g.scissor_depth++] = rect;
    _d3d11_renderer_internal__apply_scissor_top();
}

void renderer__pop_scissor(void)
{
    _d3d11_renderer_internal__flush_batches();
    if (_d3d11_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("renderer__pop_scissor: stack already empty");
        return;
    }
    _d3d11_renderer_internal__g.scissor_depth--;
    _d3d11_renderer_internal__apply_scissor_top();
}

void renderer__blur_region(gui_rect rect, float sigma_px)
{
    //
    // Placeholder: D3D11 doesn't yet wire up the FBO + separable Gaussian
    // pipeline needed for true backdrop blur. Submit a translucent darken
    // splat so blur-styled regions at least read as "muted backdrop" --
    // the visual intent comes through even if the mathematical blur is
    // absent. Matches the shape scene.c emitted before this API existed.
    //
    (void)sigma_px;
    gui_color dim = { 0.0f, 0.0f, 0.0f, 0.15f };
    renderer__submit_rect(rect, dim, 0.0f);
}

//============================================================================
//PUBLIC: renderer__flush_pending_draws / renderer__end_frame
//============================================================================

void renderer__flush_pending_draws(void)
{
    // Public thin wrapper over the internal flush so scene_render can
    // break the per-frame batch at z-index sibling boundaries. See
    // renderer.h for why this matters for text+z interleaving.
    _d3d11_renderer_internal__flush_batches();
}

void renderer__end_frame(void)
{
    //
    //always upload the constant buffer (the viewport may have changed
    //due to resize even if no rects were submitted).
    //
    _d3d11_renderer_internal__upload_constant_buffer();

    //
    //flush any remaining batched submissions.
    //
    _d3d11_renderer_internal__flush_batches();

    //
    //defensive scissor reset: if a widget pushed but failed to pop,
    //reset to the full viewport so the next frame starts clean.
    //
    if (_d3d11_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("renderer__end_frame: scissor stack non-empty (%lld); resetting", (long long)_d3d11_renderer_internal__g.scissor_depth);
        _d3d11_renderer_internal__g.scissor_depth = 0;
        _d3d11_renderer_internal__apply_scissor_top();
    }

    //
    //present. 1 = vsync (wait for next vblank). use 0 to uncap the
    //frame rate if you want unlimited fps.
    //
    IDXGISwapChain_Present(_d3d11_renderer_internal__g.swapchain, 1, 0);
}

//============================================================================
//INTERNAL: device + swapchain creation
//============================================================================

static boole _d3d11_renderer_internal__create_device_and_swapchain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC scd;
    memset(&scd, 0, sizeof(scd));
    scd.BufferDesc.Width                   = (UINT)_d3d11_renderer_internal__g.viewport_w;
    scd.BufferDesc.Height                  = (UINT)_d3d11_renderer_internal__g.viewport_h;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count                   = 1;
    scd.SampleDesc.Quality                 = 0;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount                        = 2;
    scd.OutputWindow                       = hwnd;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags                              = 0;

    //
    //feature level array: ask for 11.0 first, fall back to 10.x if the
    //driver says "not supported". D3D11CreateDeviceAndSwapChain tries
    //them in order and returns the highest it could create.
    //
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT flags = 0;
    //
    //in a dev build we'd also OR D3D11_CREATE_DEVICE_DEBUG so that the
    //d3d11 debug layer prints validation messages to the debugger. that
    //requires the Windows SDK graphics tools installed on the machine.
    //

    D3D_FEATURE_LEVEL got_fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL,                          // adapter: default
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,                          // software rasterizer: not used
        flags,
        feature_levels, (UINT)(sizeof(feature_levels) / sizeof(feature_levels[0])),
        D3D11_SDK_VERSION,
        &scd,
        &_d3d11_renderer_internal__g.swapchain,
        &_d3d11_renderer_internal__g.device,
        &got_fl,
        &_d3d11_renderer_internal__g.context
    );
    if (FAILED(hr))
    {
        log_error("D3D11CreateDeviceAndSwapChain failed (HRESULT 0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    return TRUE;
}

//
//get the current back buffer from the swapchain, wrap it in a
//render-target view, and release our local ref on the texture (the
//RTV holds its own). called once at init and again after every resize.
//
static boole _d3d11_renderer_internal__create_rtv_from_backbuffer(void)
{
    ID3D11Texture2D* back_buffer = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(_d3d11_renderer_internal__g.swapchain, 0, &IID_ID3D11Texture2D, (void**)&back_buffer);
    if (FAILED(hr))
    {
        log_error("IDXGISwapChain::GetBuffer failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    hr = ID3D11Device_CreateRenderTargetView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)back_buffer, NULL, &_d3d11_renderer_internal__g.rtv);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(back_buffer);
    if (FAILED(hr))
    {
        log_error("CreateRenderTargetView failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    return TRUE;
}

//============================================================================
//INTERNAL: shader compilation
//============================================================================
//
//compile an HLSL source string to bytecode via D3DCompile. `target` is
//the shader profile: "vs_5_0" for vertex shaders, "ps_5_0" for pixel
//shaders (the "5_0" is the shader model, available on feature level 11).
//
//errors come back in a separate ID3DBlob whose contents are a printable
//compile log. we log it and return FALSE.
//

static boole _d3d11_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob)
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
            log_error("shader compile error (%s):\n%s", entry, (char*)ID3D10Blob_GetBufferPointer(err_blob));
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
        }
        else
        {
            log_error("D3DCompile failed (0x%08lX)", (unsigned long)hr);
        }
        return FALSE;
    }
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
    return TRUE;
}

//============================================================================
//INTERNAL: pipeline setup
//============================================================================

static boole _d3d11_renderer_internal__build_pipeline(void)
{
    //
    //compile the two shader stages.
    //
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;

    if (!_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__vs_src, "vs_main", "vs_5_0", &vs_blob))
    {
        return FALSE;
    }
    if (!_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__ps_src, "ps_main", "ps_5_0", &ps_blob))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    //
    //create shader objects from the bytecode blobs.
    //
    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &_d3d11_renderer_internal__g.vs);
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("CreateVertexShader failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    hr = ID3D11Device_CreatePixelShader(
        _d3d11_renderer_internal__g.device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob),
        NULL,
        &_d3d11_renderer_internal__g.ps
    );
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("CreatePixelShader failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //input layout: the d3d11 equivalent of opengl's VAO attribute pointers.
    //semantics (POSITION, COLOR, TEXCOORD0/1/2) must match the shader's
    //input struct (VSIn). AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT
    //lets d3d compute the stride automatically, but we spell out the
    //offsets for clarity since they have to match our C struct layout.
    //
    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, (UINT)(sizeof(layout_desc) / sizeof(layout_desc[0])), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.input_layout);

    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    if (FAILED(hr))
    {
        log_error("CreateInputLayout failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //dynamic vertex buffer, sized for the max batch.
    //
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth      = (UINT)(_D3D11_RENDERER_INTERNAL__MAX_QUADS * _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d11_renderer_internal__vertex));
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vbd, NULL, &_d3d11_renderer_internal__g.vbo);
    if (FAILED(hr))
    {
        log_error("CreateBuffer (vbo) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //constant buffer for the globals struct.
    //
    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth      = (UINT)sizeof(_d3d11_renderer_internal__cb_globals);
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &cbd, NULL, &_d3d11_renderer_internal__g.cbuffer);
    if (FAILED(hr))
    {
        log_error("CreateBuffer (cbuffer) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //blend state matching opengl's SRC_ALPHA/INV_SRC_ALPHA (= the
    //"classic" alpha blend). this is part of the VISUAL CONTRACT.
    //
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = ID3D11Device_CreateBlendState(_d3d11_renderer_internal__g.device, &bd, &_d3d11_renderer_internal__g.blend_state);
    if (FAILED(hr))
    {
        log_error("CreateBlendState failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    // Rasterizer state. Two non-default knobs:
    //   CullMode = NONE         the toolkit's quads are 2D (no winding
    //                            convention enforced); disable culling
    //                            so triangles draw regardless of order.
    //   ScissorEnable = TRUE    enable the GPU scissor test always; the
    //                            scissor RECT is updated as widgets push/
    //                            pop. When the scissor stack is empty
    //                            we set the rect to cover the full
    //                            viewport, so it acts as a no-op clip.
    //
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode              = D3D11_FILL_SOLID;
    rd.CullMode              = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable       = TRUE;
    rd.ScissorEnable         = TRUE;
    rd.MultisampleEnable     = FALSE;
    rd.AntialiasedLineEnable = FALSE;

    hr = ID3D11Device_CreateRasterizerState(_d3d11_renderer_internal__g.device, &rd, &_d3d11_renderer_internal__g.rasterizer_state);
    if (FAILED(hr))
    {
        log_error("CreateRasterizerState failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //cpu-side vertex staging buffer.
    //
    _d3d11_renderer_internal__g.vert_cap  = _D3D11_RENDERER_INTERNAL__MAX_QUADS * _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d11_renderer_internal__g.cpu_verts = (_d3d11_renderer_internal__vertex*)GUI_MALLOC_T((size_t)_d3d11_renderer_internal__g.vert_cap * sizeof(_d3d11_renderer_internal__vertex), MM_TYPE_RENDERER);
    if (_d3d11_renderer_internal__g.cpu_verts == NULL)
    {
        log_error("out of memory for cpu vertex buffer");
        return FALSE;
    }

    //
    //text pipeline: separate shaders + input layout + VBO + sampler.
    //
    if (!_d3d11_renderer_internal__build_text_pipeline())
    {
        return FALSE;
    }

    return TRUE;
}

//
//build the text-specific half of the D3D11 pipeline: text VS/PS compile,
//text input layout, dynamic text vertex buffer, and a LINEAR/CLAMP
//sampler state bound at draw time.
//
static boole _d3d11_renderer_internal__build_text_pipeline(void)
{
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;

    if (!_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__text_vs_src, "vs_main", "vs_5_0", &vs_blob))
    {
        return FALSE;
    }
    if (!_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__text_ps_src, "ps_main", "ps_5_0", &ps_blob))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &_d3d11_renderer_internal__g.text_vs);
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("CreateVertexShader (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    hr = ID3D11Device_CreatePixelShader(
        _d3d11_renderer_internal__g.device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob),
        NULL,
        &_d3d11_renderer_internal__g.text_ps
    );
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("CreatePixelShader (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //text input layout: 3 attributes, matching the text_vertex struct.
    //  POSITION @ 0    (float2)
    //  COLOR    @ 8    (float4)
    //  TEXCOORD0 @ 24  (float2)
    //
    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, (UINT)(sizeof(layout_desc) / sizeof(layout_desc[0])), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.text_input_layout);

    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    if (FAILED(hr))
    {
        log_error("CreateInputLayout (text) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //dynamic text vertex buffer. sized for MAX_TEXT_GLYPHS quads.
    //
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth      = (UINT)(_D3D11_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d11_renderer_internal__text_vertex));
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vbd, NULL, &_d3d11_renderer_internal__g.text_vbo);
    if (FAILED(hr))
    {
        log_error("CreateBuffer (text vbo) failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //sampler: LINEAR filter + CLAMP address on both axes. mirrors the
    //GL backend's glTexParameteri settings, so the visual output
    //matches across backends.
    //
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(_d3d11_renderer_internal__g.device, &sd, &_d3d11_renderer_internal__g.text_sampler);
    if (FAILED(hr))
    {
        log_error("CreateSamplerState failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    //cpu-side text staging buffer.
    //
    _d3d11_renderer_internal__g.text_vert_cap  = _D3D11_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d11_renderer_internal__g.text_cpu_verts = (_d3d11_renderer_internal__text_vertex*)GUI_MALLOC_T((size_t)_d3d11_renderer_internal__g.text_vert_cap * sizeof(_d3d11_renderer_internal__text_vertex), MM_TYPE_RENDERER);
    if (_d3d11_renderer_internal__g.text_cpu_verts == NULL)
    {
        log_error("out of memory for cpu text vertex buffer");
        return FALSE;
    }

    return TRUE;
}

//============================================================================
//INTERNAL: resize
//============================================================================
//
//when the window changes size, the swapchain's back buffer is still
//the old size. we need to:
//  1. release our RTV (it holds a ref to the old back buffer).
//  2. call IDXGISwapChain::ResizeBuffers to reallocate the back buffer.
//  3. get the new back buffer and create a fresh RTV against it.
//

static boole _d3d11_renderer_internal__handle_resize(int64 new_w, int64 new_h)
{
    if (new_w <= 0) new_w = 1;
    if (new_h <= 0) new_h = 1;

    _d3d11_renderer_internal__g.viewport_w = new_w;
    _d3d11_renderer_internal__g.viewport_h = new_h;

    //
    //unbind and release the old RTV before ResizeBuffers (d3d11
    //requires no outstanding references to the back buffer).
    //
    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 0, NULL, NULL);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rtv);

    //
    //BufferCount=0 means "keep the same as before", Format=UNKNOWN
    //means "keep the same format". passing 0 for Flags = no changes.
    //
    HRESULT hr = IDXGISwapChain_ResizeBuffers(_d3d11_renderer_internal__g.swapchain, 0, (UINT)new_w, (UINT)new_h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        log_error("IDXGISwapChain::ResizeBuffers failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    return _d3d11_renderer_internal__create_rtv_from_backbuffer();
}

//============================================================================
//INTERNAL: constant buffer upload
//============================================================================

static void _d3d11_renderer_internal__upload_constant_buffer(void)
{
    _d3d11_renderer_internal__cb_globals cb;
    cb.viewport_w = (float)_d3d11_renderer_internal__g.viewport_w;
    cb.viewport_h = (float)_d3d11_renderer_internal__g.viewport_h;
    cb._padding_0 = 0.0f;
    cb._padding_1 = 0.0f;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        memcpy(mapped.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.cbuffer, 0);
    }
}

//============================================================================
//INTERNAL: vertex append
//============================================================================

static void _d3d11_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c)
{
    _d3d11_renderer_internal__vertex* v = &_d3d11_renderer_internal__g.cpu_verts[_d3d11_renderer_internal__g.vert_count++];
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

static void _d3d11_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color c)
{
    _d3d11_renderer_internal__text_vertex* tv = &_d3d11_renderer_internal__g.text_cpu_verts[_d3d11_renderer_internal__g.text_vert_count++];
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
//font.c rasterizes the atlas into an R8 bitmap and hands us the bytes.
//we wrap those in an immutable ID3D11Texture2D and an SRV, which we own
//until renderer__destroy_atlas releases them. the opaque void* handle
//we hand back to font.c is the SRV pointer -- that's what ends up bound
//to pixel-shader register t0 during the text pass. the underlying
//Texture2D is held implicitly via the SRV's internal ref, so we release
//our own ref on the texture right after creating the view.
//

void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    if (pixels == NULL || width <= 0 || height <= 0 || _d3d11_renderer_internal__g.device == NULL)
    {
        return NULL;
    }

    //
    //immutable texture: we never update it after creation (if we need
    //a new atlas size we just create a new one).
    //
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width              = (UINT)width;
    td.Height             = (UINT)height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count   = 1;
    td.SampleDesc.Quality = 0;
    td.Usage              = D3D11_USAGE_IMMUTABLE;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd;
    memset(&srd, 0, sizeof(srd));
    srd.pSysMem     = pixels;
    srd.SysMemPitch = (UINT)width; // R8 = 1 byte per pixel, so pitch = width.

    ID3D11Texture2D* tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &td, &srd, &tex);
    if (FAILED(hr))
    {
        log_error("CreateTexture2D (atlas) failed (0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    //
    //wrap the texture in a shader resource view, which is what the PS
    //samples from. NULL desc = the whole texture as a 2D SRV.
    //
    ID3D11ShaderResourceView* srv = NULL;
    hr = ID3D11Device_CreateShaderResourceView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)tex, NULL, &srv);
    //
    //release our own ref on the texture. the SRV holds the last ref;
    //when the SRV is released in renderer__destroy_atlas, the texture
    //goes with it.
    //
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(tex);

    if (FAILED(hr))
    {
        log_error("CreateShaderResourceView (atlas) failed (0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    return (void*)srv;
}

void renderer__destroy_atlas(void* atlas)
{
    if (atlas == NULL)
    {
        return;
    }
    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)atlas;
    //
    //if it's the currently-bound atlas, clear our cached borrow so we
    //don't feed a dangling SRV to the next frame's text pass.
    //
    if (_d3d11_renderer_internal__g.current_text_atlas == srv)
    {
        _d3d11_renderer_internal__g.current_text_atlas = NULL;
    }
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(srv);
}

void renderer__set_text_atlas(void* atlas)
{
    _d3d11_renderer_internal__g.current_text_atlas = (ID3D11ShaderResourceView*)atlas;
}

void renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color)
{
    ID3D11ShaderResourceView* atlas = _d3d11_renderer_internal__g.current_text_atlas;
    if (atlas == NULL)
    {
        return;
    }
    if (_d3d11_renderer_internal__g.text_vert_count + _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD > _d3d11_renderer_internal__g.text_vert_cap)
    {
        return;
    }

    //
    //open or extend a run keyed by the current atlas SRV. when the
    //atlas changes between submissions, this function starts a new
    //run so end_frame can bind the right SRV per Draw.
    //
    _d3d11_renderer_internal__text_run* run = NULL;
    if (_d3d11_renderer_internal__g.text_run_count > 0)
    {
        run = &_d3d11_renderer_internal__g.text_runs[_d3d11_renderer_internal__g.text_run_count - 1];
        if (run->atlas != atlas)
        {
            run = NULL;
        }
    }
    if (run == NULL)
    {
        if (_d3d11_renderer_internal__g.text_run_count >= _D3D11_RENDERER_INTERNAL__MAX_TEXT_RUNS)
        {
            return;
        }
        run = &_d3d11_renderer_internal__g.text_runs[_d3d11_renderer_internal__g.text_run_count++];
        run->atlas      = atlas;
        run->vert_start = _d3d11_renderer_internal__g.text_vert_count;
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
    _d3d11_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _d3d11_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d11_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _d3d11_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d11_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _d3d11_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    run->vert_count += _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

//============================================================================
//IMAGE PIPELINE (RGBA8 textures via D3D11)
//============================================================================
//
// Mirrors the text pipeline's shape: same vertex format (pos + color
// + uv = 32 bytes), same sampler (reused from the text pipeline),
// different PS. Lazy-built on first submit_image.
//
// Texture creation: ID3D11Device::CreateTexture2D with
// DXGI_FORMAT_R8G8B8A8_UNORM and USAGE_IMMUTABLE. One
// D3D11_SUBRESOURCE_DATA fill with the caller's RGBA bytes at
// SysMemPitch = width*4 (no padding). Wrap an
// ID3D11ShaderResourceView around it and hand that out as the
// opaque texture handle -- the SRV keeps the Texture2D alive, so
// callers never need to know about the texture object directly.
//

static const char* _D3D11_RENDERER_INTERNAL__IMAGE_PS_SRC =
    "Texture2D    u_tex  : register(t0);\n"
    "SamplerState u_samp : register(s0);\n"
    "\n"
    "struct PSIn {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "float4 ps_main(PSIn i) : SV_TARGET {\n"
    "    float4 s = u_tex.Sample(u_samp, i.uv);\n"
    "    return s * i.col;\n"
    "}\n";

static boole _d3d11_renderer_internal__ensure_image_pipeline(void)
{
    if (_d3d11_renderer_internal__g.image_ps != NULL)
    {
        return TRUE;
    }

    //
    // VS: reuse the text vertex shader source verbatim -- both take
    // pos + color + uv, both emit SV_POSITION via the u_viewport
    // cbuffer. No reason to duplicate the compile.
    //
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    if (!_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__text_vs_src, "vs_main", "vs_5_0", &vs_blob)) { return FALSE; }
    if (!_d3d11_renderer_internal__compile_shader((char*)_D3D11_RENDERER_INTERNAL__IMAGE_PS_SRC, "ps_main", "ps_5_0", &ps_blob))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &_d3d11_renderer_internal__g.image_vs);
    if (FAILED(hr))
    {
        log_error("image_vs CreateVertexShader failed (0x%08lX)", (unsigned long)hr);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        return FALSE;
    }
    hr = ID3D11Device_CreatePixelShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL, &_d3d11_renderer_internal__g.image_ps);
    if (FAILED(hr))
    {
        log_error("image_ps CreatePixelShader failed (0x%08lX)", (unsigned long)hr);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        return FALSE;
    }

    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, (UINT)(sizeof(layout_desc) / sizeof(layout_desc[0])), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.image_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    if (FAILED(hr))
    {
        log_error("image input layout failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }

    //
    // 6-vertex dynamic VBO. Same D3D11_USAGE_DYNAMIC + MAP_WRITE_DISCARD
    // pattern the other pipelines use so each submit_image starts from
    // a fresh CPU-writable buffer on the driver's ring.
    //
    D3D11_BUFFER_DESC vb_desc;
    memset(&vb_desc, 0, sizeof(vb_desc));
    vb_desc.Usage          = D3D11_USAGE_DYNAMIC;
    vb_desc.ByteWidth      = (UINT)(6 * 8 * sizeof(float));
    vb_desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vb_desc, NULL, &_d3d11_renderer_internal__g.image_vbo);
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
    if (_d3d11_renderer_internal__g.device == NULL) { return NULL; }

    //
    // Mip chain. MipLevels=0 asks D3D11 to auto-pick log2(max(w,h))+1.
    // We create the texture without initial data (D3D11 won't let us
    // pair initial data with autogen-mipmaps), upload the top mip via
    // UpdateSubresource, then call GenerateMips on the SRV. That path
    // needs D3D11_BIND_RENDER_TARGET + D3D11_RESOURCE_MISC_GENERATE_MIPS
    // and USAGE_DEFAULT (not IMMUTABLE).
    //
    // Why do this at all: photo sources (6000x4000) rendered as 48 px
    // launcher tiles shimmer badly under pure bilinear -- minification
    // aliasing. Trilinear + mipmaps samples the pre-reduced level
    // closest to the displayed scale, which eats the aliasing.
    //
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = 0;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ID3D11Texture2D* tex2d = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &td, NULL, &tex2d);
    if (FAILED(hr))
    {
        log_error("image CreateTexture2D failed (0x%08lX) for %dx%d", (unsigned long)hr, width, height);
        return NULL;
    }

    //
    // Upload mip 0. Subresource index 0 = (ArraySlice 0, MipSlice 0).
    // UpdateSubresource is the simplest path for a DEFAULT-usage
    // texture; ~15 MB for a 6000x4000 image, cheap once at load.
    //
    ID3D11DeviceContext_UpdateSubresource(_d3d11_renderer_internal__g.context, (ID3D11Resource*)tex2d, 0, NULL, rgba, (UINT)width * 4u, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels       = (UINT)-1; // -1 = all generated mips

    ID3D11ShaderResourceView* srv = NULL;
    hr = ID3D11Device_CreateShaderResourceView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)tex2d, &srv_desc, &srv);
    ID3D11Texture2D_Release(tex2d);
    if (FAILED(hr))
    {
        log_error("image CreateShaderResourceView failed (0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    //
    // Fill the mip chain from the top level we just uploaded. The
    // driver downsamples each level from the one above via a box
    // filter. Runs on the GPU; returns immediately.
    //
    ID3D11DeviceContext_GenerateMips(_d3d11_renderer_internal__g.context, srv);
    return (void*)srv;
}

void renderer__destroy_texture(void* tex)
{
    if (tex == NULL) { return; }
    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)tex;
    ID3D11ShaderResourceView_Release(srv);
}

void renderer__submit_image(gui_rect rect, void* tex, gui_color tint)
{
    if (tex == NULL) { return; }
    if (!_d3d11_renderer_internal__ensure_image_pipeline()) { return; }

    //
    // Flush any pending rect / text batches FIRST so their draw
    // calls land underneath this image (correct submit-order =
    // render-order). scissor push/pop would do the same but this
    // is the direct way.
    //
    _d3d11_renderer_internal__flush_batches();

    //
    // cbuffer may not have been uploaded yet for this frame if the
    // tree submitted nothing else before the image. Upload now
    // (cheap; DYNAMIC + MAP_WRITE_DISCARD, a few bytes).
    //
    _d3d11_renderer_internal__upload_constant_buffer();

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    //
    // 6 verts: two tris covering the quad. Layout: pos(2) col(4) uv(2)
    // = 8 floats per vertex.
    //
    float verts[6 * 8] = {
        x0, y0, tint.r, tint.g, tint.b, tint.a, 0.0f, 0.0f,
        x1, y0, tint.r, tint.g, tint.b, tint.a, 1.0f, 0.0f,
        x0, y1, tint.r, tint.g, tint.b, tint.a, 0.0f, 1.0f,
        x1, y0, tint.r, tint.g, tint.b, tint.a, 1.0f, 0.0f,
        x1, y1, tint.r, tint.g, tint.b, tint.a, 1.0f, 1.0f,
        x0, y1, tint.r, tint.g, tint.b, tint.a, 0.0f, 1.0f,
    };

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.image_vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { return; }
    memcpy(mapped.pData, verts, sizeof(verts));
    ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.image_vbo, 0);

    //
    // Pipeline state: image shaders + our input layout + the shared
    // text sampler + the caller's SRV at slot 0.
    //
    ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.image_input_layout);

    UINT stride = (UINT)(8 * sizeof(float));
    UINT offset = 0;
    ID3D11Buffer* vbo_ptr = _d3d11_renderer_internal__g.image_vbo;
    ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &vbo_ptr, &stride, &offset);

    ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.image_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.image_ps, NULL, 0);

    //
    // cbuffer (viewport) was written by flush_batches' rect pass; it
    // holds the current frame's viewport. Bind it to b0 for the VS.
    //
    ID3D11Buffer* cb = _d3d11_renderer_internal__g.cbuffer;
    ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &cb);

    ID3D11SamplerState* samp = _d3d11_renderer_internal__g.text_sampler;
    ID3D11DeviceContext_PSSetSamplers(_d3d11_renderer_internal__g.context, 0, 1, &samp);

    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)tex;
    ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &srv);

    ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, 6, 0);

    //
    // Leave SRV bound to t0 through end of frame; flush_batches on
    // next rect/text draw will overwrite what it needs. No explicit
    // clear of the SRV slot required.
    //
}

//
// Shadow: multi-layer concentric rects with quadratic falloff.
// Matches the opengl3/gles3 implementation -- uses submit_rect, which
// this backend already supports, so the same stacking trick works.
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
// Gradient: per-corner color via the existing SDF-rect vertex format.
// The fragment shader interpolates v_color across the triangle, so
// two-color lerps cost nothing extra on the GPU side -- just different
// corner colors fed to the same push_vertex call that solid-rect uses.
//
void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius)
{
    if (_d3d11_renderer_internal__g.vert_count + _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _d3d11_renderer_internal__g.vert_cap)
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
    _d3d11_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c_tl);
    _d3d11_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _d3d11_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
    _d3d11_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c_tr);
    _d3d11_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c_br);
    _d3d11_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c_bl);
}
