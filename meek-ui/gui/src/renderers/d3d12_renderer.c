//============================================================================
// d3d12_renderer.c - Direct3D 12 backend.
//============================================================================
//
// Fifth renderer.h implementation. Status: SCAFFOLD (solid-rect +
// scissor working, text + image + gradient + shadow stubbed).
// Matches the completeness level of d3d11 + d3d9 + vulkan scaffolds.
//
// Platform: Windows only (D3D12 is Win32). The platform_win32 layer
// hands its HWND to renderer__init; we create a DXGI swap chain
// against it.
//
// VISUAL CONTRACT (from renderer.h): same SDF rounded-box formula,
// smoothstep range, scissor semantics, separate-alpha blend, top-
// left-origin pixel coordinate space as every other backend.
//
// DEPENDENCIES:
//   d3d12.lib d3d12.dll       the core API.
//   dxgi.lib dxgi1_4+.dll     swapchain + factory.
//   d3dcompiler.lib           runtime HLSL compile, same as d3d11.
//   dxguid.lib                IID_* constants for the COM-style
//                             ID3D12xxx interfaces.
//
// LIFECYCLE (renderer__init):
//   1. D3D12CreateDevice(D3D_FEATURE_LEVEL_11_0)
//   2. create direct command queue
//   3. CreateDXGIFactory -> CreateSwapChainForHwnd
//   4. descriptor heaps: RTV (for swapchain buffers)
//   5. command allocator + command list (one per frame-in-flight)
//   6. fence + event for CPU/GPU sync
//   7. root signature (one root constant for viewport)
//   8. rect PSO: HLSL via D3DCompile, IA layout, blend, raster
//   9. upload vertex buffer (committed resource in UPLOAD heap,
//      persistently mapped)
//
// FRAME LOOP (begin_frame -> submit_rect* -> end_frame):
//   - wait fence for this frame slot
//   - reset allocator + list
//   - barrier backbuffer PRESENT -> RENDER_TARGET
//   - OMSetRenderTargets, ClearRenderTargetView, SetPipelineState,
//     SetGraphicsRootSignature, RSSetViewports, RSSetScissorRects,
//     IASetVertexBuffers, SetGraphicsRoot32BitConstants, DrawInstanced
//   - barrier RENDER_TARGET -> PRESENT
//   - Close + ExecuteCommandLists
//   - Present(1,0) (vsync)
//   - Signal fence, advance frame slot
//

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "types.h"
#include "gui.h"
#include "renderer.h"
#include "_visual_contract.h"
#include "../clib/memory_manager.h"
#include "../third_party/log.h"

//
// C-mode COM helpers. D3D12 headers only expose the method-table
// form in C (no operator overloading); the `vtbl_call(p, M, ...)`
// macro keeps call sites readable. Same pattern the d3d11_renderer
// + d3d9_renderer use.
//
#define VCALL(obj, method, ...) ((obj)->lpVtbl->method((obj), __VA_ARGS__))
#define VCALL0(obj, method)     ((obj)->lpVtbl->method((obj)))

//============================================================================
// constants
//============================================================================

//
// MAX_QUADS is sized for a WHOLE FRAME's worth of rects, not a
// mid-frame flush batch -- we can't safely reuse lower offsets in
// the persistent-mapped UPLOAD buffer after a flush has recorded a
// DrawInstanced against them. 8192 rect quads = 48K verts = ~2.1 MB
// per FIF slot; easily fits even heavy UIs. Same reasoning for
// MAX_TEXT_QUADS: 16384 glyph quads covers large text pages.
//
#define _D3D12_RENDERER_INTERNAL__MAX_QUADS         8192
#define _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD    6
#define _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT  2
#define _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT 2
#define _D3D12_RENDERER_INTERNAL__MAX_SCISSORS      16
#define _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS    16384
#define _D3D12_RENDERER_INTERNAL__MAX_TEXT_RUNS     64
//
// Shader-visible CBV_SRV_UAV heap capacity. Every loaded atlas +
// every uploaded image texture reserves one slot. Widget_image's
// cache caps at 64; font.c creates at most a few atlases per frame.
// 256 leaves generous headroom.
//
#define _D3D12_RENDERER_INTERNAL__MAX_SRVS          256

//============================================================================
// vertex layout -- matches every other backend
//============================================================================

typedef struct _d3d12_renderer_internal__vertex
{
    float x, y;
    float r, g, b, a;
    float lx, ly;
    float rect_w, rect_h;
    float radius;
} _d3d12_renderer_internal__vertex;

//
// Text + image vertex: pos + color + uv = 8 floats. Same layout as
// the d3d11 text/image pipelines. Both use the SAME vertex shader
// (view->NDC + passthrough color/uv) but different pixel shaders
// because R8 atlas sampling and RGBA texture sampling want different
// math.
//
typedef struct _d3d12_renderer_internal__tx_vertex
{
    float x, y;
    float r, g, b, a;
    float u, v;
} _d3d12_renderer_internal__tx_vertex;

typedef struct _d3d12_renderer_internal__scissor_entry
{
    D3D12_RECT rect;
    boole      active;
} _d3d12_renderer_internal__scissor_entry;

//
// Atlas / texture record. One per loaded atlas or image. Holds the
// GPU texture + its shader-visible SRV slot. The void* handle we
// hand back to font.c / widget_image.c is a pointer to this struct.
//
typedef struct _d3d12_renderer_internal__tex_entry
{
    ID3D12Resource*              resource;
    int                          srv_slot;     // index into srv_heap
    D3D12_GPU_DESCRIPTOR_HANDLE  srv_gpu;      // cached so glyph path skips offset-math
    int                          width;
    int                          height;
    boole                        in_use;
} _d3d12_renderer_internal__tex_entry;

//
// One run of text quads sharing an atlas. end_frame binds the run's
// atlas SRV once, then draws the run's vertex range with a single
// DrawInstanced. Scene typically issues a small number of runs per
// frame (one per font size / family combination).
//
typedef struct _d3d12_renderer_internal__text_run
{
    _d3d12_renderer_internal__tex_entry* atlas;
    int64                                 vert_start;
    int64                                 vert_count;
} _d3d12_renderer_internal__text_run;

//============================================================================
// state
//============================================================================

typedef struct _d3d12_renderer_internal__state
{
    HWND hwnd;

    ID3D12Device*               device;
    ID3D12CommandQueue*         queue;
    IDXGISwapChain3*            swapchain;

    // RTV descriptor heap (one per swap buffer)
    ID3D12DescriptorHeap*       rtv_heap;
    UINT                        rtv_stride;
    ID3D12Resource*             swap_buffers[_D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT];
    D3D12_CPU_DESCRIPTOR_HANDLE swap_rtvs   [_D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT];

    // per-frame command recording
    ID3D12CommandAllocator*     allocators[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    ID3D12GraphicsCommandList*  cmd_lists[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];

    // fence + event
    ID3D12Fence*                fence;
    UINT64                      fence_values[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    UINT64                      next_fence_value;
    HANDLE                      fence_event;

    // rect pipeline
    ID3D12RootSignature*        root_sig;
    ID3D12PipelineState*        rect_pso;

    //
    // Rect vertex buffer (one per frame-in-flight). Append-only
    // within a frame: CPU writes at offset rect_vert_count, flush
    // draws [rect_draw_cursor..rect_vert_count) then advances
    // rect_draw_cursor to rect_vert_count. We do NOT reset
    // rect_vert_count mid-frame because the command list may have
    // recorded a DrawInstanced that will read those verts later,
    // and persistent-mapped UPLOAD memory means CPU writes to the
    // same offsets would stomp the still-pending draw's data. Both
    // cursors reset to 0 at begin_frame (previous frame's work has
    // completed per the fence wait).
    //
    ID3D12Resource*             rect_vbo[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    _d3d12_renderer_internal__vertex* rect_vbo_ptr[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    int64                       rect_vert_count;
    int64                       rect_draw_cursor;

    // current frame slot / backbuffer index
    UINT                        frame_slot;         // 0..FRAMES_IN_FLIGHT-1
    UINT                        backbuffer_index;   // 0..SWAP_BUFFER_COUNT-1

    // viewport
    int64                       viewport_w;
    int64                       viewport_h;
    gui_color                   clear_color;
    boole                       frame_active;

    // scissor
    _d3d12_renderer_internal__scissor_entry scissors[_D3D12_RENDERER_INTERNAL__MAX_SCISSORS];
    int64                       scissor_depth;

    // text + image: shared shader-visible SRV heap + pool of tex entries
    ID3D12DescriptorHeap*       srv_heap;
    UINT                        srv_stride;
    _d3d12_renderer_internal__tex_entry tex_pool[_D3D12_RENDERER_INTERNAL__MAX_SRVS];

    // text + image: shared root signature (+ static sampler) and pipelines
    ID3D12RootSignature*        tx_root_sig;
    ID3D12PipelineState*        text_pso;
    ID3D12PipelineState*        image_pso;
    boole                       tx_built;   // lazy-built on first atlas/image

    //
    // Text per-frame VBO. Same append-only discipline as the rect
    // VBO: text_vert_count is where NEW glyph verts get written;
    // flush_text draws runs [text_first_undrawn_run..text_run_count)
    // then advances text_first_undrawn_run. Don't reset either
    // counter mid-frame for the same CPU-writes-stomp-recorded-GPU-reads
    // reason described on the rect VBO above.
    //
    ID3D12Resource*             text_vbo[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    _d3d12_renderer_internal__tx_vertex* text_vbo_ptr[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    int64                       text_vert_count;
    _d3d12_renderer_internal__text_run text_runs[_D3D12_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int                         text_run_count;
    int                         text_first_undrawn_run;
    _d3d12_renderer_internal__tex_entry* current_text_atlas;

    //
    // Image per-frame VBO. Image draws aren't batched into runs --
    // each submit_image call flushes rects + text first, writes 6
    // verts into this buffer, and issues its own draw. So we only
    // need ONE quad's worth of verts in this buffer per frame, but
    // since calls are sequential we reuse the 6-vertex slot for
    // each one's bounds update (the vertex position for one call
    // stays valid long enough for its own draw). Simplest: size it
    // for max N queued images per frame.
    //
    ID3D12Resource*             image_vbo[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    _d3d12_renderer_internal__tx_vertex* image_vbo_ptr[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    int64                       image_vert_count;    // reset each frame
} _d3d12_renderer_internal__state;

static _d3d12_renderer_internal__state _d3d12_renderer_internal__g;

//============================================================================
// HLSL shaders
//============================================================================
//
// HLSL 5.1 (D3D12 requires at least 5.0; 5.1 is fine everywhere).
// Same math as the opengl3 / gles3 shaders, SM5.1 flavored.
// Viewport delivered via root 32-bit constants (cheaper than a
// CBV for 8 bytes; same role push constants play in the Vulkan
// backend).
//

static const char* _D3D12_RENDERER_INTERNAL__HLSL_SRC =
    "cbuffer RootCB : register(b0) { float2 u_viewport; float2 _pad; };\n"
    "struct VSIn {\n"
    "    float2 pos      : POSITION;\n"
    "    float4 color    : COLOR0;\n"
    "    float2 local    : TEXCOORD0;\n"
    "    float2 rect_sz  : TEXCOORD1;\n"
    "    float  radius   : TEXCOORD2;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos      : SV_POSITION;\n"
    "    float4 color    : COLOR0;\n"
    "    float2 local    : TEXCOORD0;\n"
    "    float2 rect_sz  : TEXCOORD1;\n"
    "    float  radius   : TEXCOORD2;\n"
    "};\n"
    "\n"
    "VSOut VSMain(VSIn v)\n"
    "{\n"
    "    VSOut o;\n"
    "    float2 ndc = float2(\n"
    "        (v.pos.x / u_viewport.x) * 2.0 - 1.0,\n"
    "        1.0 - (v.pos.y / u_viewport.y) * 2.0\n"
    "    );\n"
    //
    // D3D's clip space is y-up (bottom-left origin at (-1,-1), top
    // at (+1,+1)) so we flip y here, same as the GL shaders.
    //
    "    o.pos     = float4(ndc, 0.0, 1.0);\n"
    "    o.color   = v.color;\n"
    "    o.local   = v.local;\n"
    "    o.rect_sz = v.rect_sz;\n"
    "    o.radius  = v.radius;\n"
    "    return o;\n"
    "}\n"
    "\n"
    RENDERER_SDF_ROUND_BOX_HLSL
    "\n"
    "float4 PSMain(VSOut i) : SV_Target\n"
    "{\n"
    "    float2 half_size = i.rect_sz * 0.5;\n"
    "    float2 p = i.local - half_size;\n"
    "    float d = sd_round_box(p, half_size, i.radius);\n"
    "    float aa = 1.0 - smoothstep(" RENDERER_SDF_AA_MIN ", " RENDERER_SDF_AA_MAX ", d);\n"
    "    if (aa <= 0.0) discard;\n"
    "    return float4(i.color.rgb, i.color.a * aa);\n"
    "}\n";

//
// Text + image share one VS (pos + color + uv -> NDC + passthrough).
// Different pixel shaders: text samples R8 with gamma-correct
// reshape; image samples RGBA and multiplies by vertex color for
// tint. Root signature reserves slot 0 for viewport constants,
// slot 1 for a 1-SRV descriptor table, with one static sampler
// at s0 (linear, clamp).
//
static const char* _D3D12_RENDERER_INTERNAL__TX_HLSL_SRC =
    "cbuffer RootCB : register(b0) { float2 u_viewport; float2 _pad; };\n"
    "Texture2D    u_tex  : register(t0);\n"
    "SamplerState u_samp : register(s0);\n"
    "\n"
    "struct VSIn {\n"
    "    float2 pos : POSITION;\n"
    "    float4 col : COLOR0;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR0;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VSOut VSMain(VSIn v)\n"
    "{\n"
    "    VSOut o;\n"
    "    float2 ndc = float2(\n"
    "        (v.pos.x / u_viewport.x) * 2.0 - 1.0,\n"
    "        1.0 - (v.pos.y / u_viewport.y) * 2.0\n"
    "    );\n"
    "    o.pos = float4(ndc, 0.0, 1.0);\n"
    "    o.col = v.col;\n"
    "    o.uv  = v.uv;\n"
    "    return o;\n"
    "}\n"
    "\n"
    //
    // Text PS: R8 coverage sampled from atlas, gamma-reshaped so
    // partial-coverage edges don't wash out on dark backgrounds when
    // the backbuffer is linear-UNORM. Same rationale as the d3d11
    // text_ps. See opengl3_renderer.c text_fs_src for the full
    // gamma explanation.
    //
    "float4 PSText(VSOut i) : SV_Target\n"
    "{\n"
    "    float a = u_tex.Sample(u_samp, i.uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    "    a = pow(a, 1.0 / 2.2);\n"
    "    return float4(i.col.rgb, i.col.a * a);\n"
    "}\n"
    "\n"
    //
    // Image PS: full RGBA sampled + tinted. `col` coming from the VS
    // carries the tint.
    //
    "float4 PSImage(VSOut i) : SV_Target\n"
    "{\n"
    "    float4 s = u_tex.Sample(u_samp, i.uv);\n"
    "    return s * i.col;\n"
    "}\n";

//============================================================================
// forward decls
//============================================================================

static boole _d3d12_renderer_internal__create_device_and_queue(void);
static boole _d3d12_renderer_internal__create_swapchain(HWND hwnd);
static boole _d3d12_renderer_internal__create_rtvs(void);
static boole _d3d12_renderer_internal__create_command_objects(void);
static boole _d3d12_renderer_internal__create_fence(void);
static boole _d3d12_renderer_internal__create_root_signature(void);
static boole _d3d12_renderer_internal__create_rect_pso(void);
static boole _d3d12_renderer_internal__create_rect_vertex_buffers(void);
static boole _d3d12_renderer_internal__create_srv_heap(void);
static boole _d3d12_renderer_internal__create_tx_root_and_pipelines(void);
static boole _d3d12_renderer_internal__create_tx_vertex_buffers(void);
static int   _d3d12_renderer_internal__alloc_srv_slot(void);
static void  _d3d12_renderer_internal__free_srv_slot(int slot);
static _d3d12_renderer_internal__tex_entry* _d3d12_renderer_internal__alloc_tex_entry(void);
static void  _d3d12_renderer_internal__release_tex_entry(_d3d12_renderer_internal__tex_entry* e);
static boole _d3d12_renderer_internal__upload_texture(_d3d12_renderer_internal__tex_entry* e, const ubyte* pixels, int w, int h, DXGI_FORMAT fmt, int bytes_per_pixel);
static void  _d3d12_renderer_internal__wait_for_fence(UINT64 value);
static void  _d3d12_renderer_internal__wait_for_gpu(void);
static void  _d3d12_renderer_internal__flush_rects(void);
static void  _d3d12_renderer_internal__flush_text(void);
static void  _d3d12_renderer_internal__bind_rect_pipeline(ID3D12GraphicsCommandList* list);

//============================================================================
// PUBLIC: renderer__init
//============================================================================

boole renderer__init(void* native_window)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    memset(s, 0, sizeof(*s));

    if (native_window == NULL)
    {
        log_error("d3d12_renderer: NULL native_window");
        return FALSE;
    }
    s->hwnd = (HWND)native_window;

    if (!_d3d12_renderer_internal__create_device_and_queue())           { return FALSE; }
    if (!_d3d12_renderer_internal__create_swapchain(s->hwnd))           { return FALSE; }
    if (!_d3d12_renderer_internal__create_rtvs())                       { return FALSE; }
    if (!_d3d12_renderer_internal__create_command_objects())            { return FALSE; }
    if (!_d3d12_renderer_internal__create_fence())                      { return FALSE; }
    if (!_d3d12_renderer_internal__create_root_signature())             { return FALSE; }
    if (!_d3d12_renderer_internal__create_rect_pso())                   { return FALSE; }
    if (!_d3d12_renderer_internal__create_rect_vertex_buffers())        { return FALSE; }
    if (!_d3d12_renderer_internal__create_srv_heap())                   { return FALSE; }
    if (!_d3d12_renderer_internal__create_tx_vertex_buffers())          { return FALSE; }
    //
    // Text + image root sig + pipelines are lazy-built on first use so
    // a PoC that never submits text (unit-test style) doesn't pay for
    // them. Flag is reset here so shutdown can tell if we need to
    // release them.
    //
    s->tx_built = FALSE;

    s->backbuffer_index = VCALL0(s->swapchain, GetCurrentBackBufferIndex);

    log_info("d3d12_renderer: up (%d buffers, %lldx%lld)",
             _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT,
             (long long)s->viewport_w, (long long)s->viewport_h);
    return TRUE;
}

//============================================================================
// PUBLIC: renderer__shutdown
//============================================================================

void renderer__shutdown(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    if (s->queue != NULL && s->fence != NULL)
    {
        //
        // Make sure the GPU is done touching everything before we
        // start releasing. D3D12 resources are refcounted but
        // releasing an in-use resource blows up the debug layer.
        //
        _d3d12_renderer_internal__wait_for_gpu();
    }

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        if (s->rect_vbo_ptr[i] != NULL && s->rect_vbo[i] != NULL)
        {
            VCALL(s->rect_vbo[i], Unmap, 0, NULL);
            s->rect_vbo_ptr[i] = NULL;
        }
        if (s->rect_vbo[i]    != NULL) { VCALL0(s->rect_vbo[i], Release);    s->rect_vbo[i]    = NULL; }
        if (s->text_vbo_ptr[i] != NULL && s->text_vbo[i] != NULL)
        {
            VCALL(s->text_vbo[i], Unmap, 0, NULL);
            s->text_vbo_ptr[i] = NULL;
        }
        if (s->text_vbo[i]    != NULL) { VCALL0(s->text_vbo[i], Release);    s->text_vbo[i]    = NULL; }
        if (s->image_vbo_ptr[i] != NULL && s->image_vbo[i] != NULL)
        {
            VCALL(s->image_vbo[i], Unmap, 0, NULL);
            s->image_vbo_ptr[i] = NULL;
        }
        if (s->image_vbo[i]   != NULL) { VCALL0(s->image_vbo[i], Release);   s->image_vbo[i]   = NULL; }
        if (s->cmd_lists[i]   != NULL) { VCALL0(s->cmd_lists[i], Release);   s->cmd_lists[i]   = NULL; }
        if (s->allocators[i]  != NULL) { VCALL0(s->allocators[i], Release);  s->allocators[i]  = NULL; }
    }

    //
    // Release any still-live atlas / image textures. font__shutdown and
    // widget_image__cache_shutdown run BEFORE renderer__shutdown per
    // the platform contract, so the pool should normally be empty
    // here -- this loop is defense against host code that didn't
    // play by the rules.
    //
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__MAX_SRVS; i++)
    {
        _d3d12_renderer_internal__tex_entry* e = &s->tex_pool[i];
        if (!e->in_use) { continue; }
        if (e->resource != NULL) { VCALL0(e->resource, Release); e->resource = NULL; }
        e->in_use = FALSE;
    }

    if (s->text_pso    != NULL) { VCALL0(s->text_pso, Release);    s->text_pso    = NULL; }
    if (s->image_pso   != NULL) { VCALL0(s->image_pso, Release);   s->image_pso   = NULL; }
    if (s->tx_root_sig != NULL) { VCALL0(s->tx_root_sig, Release); s->tx_root_sig = NULL; }
    if (s->srv_heap    != NULL) { VCALL0(s->srv_heap, Release);    s->srv_heap    = NULL; }

    if (s->rect_pso   != NULL) { VCALL0(s->rect_pso, Release);   s->rect_pso   = NULL; }
    if (s->root_sig   != NULL) { VCALL0(s->root_sig, Release);   s->root_sig   = NULL; }

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT; i++)
    {
        if (s->swap_buffers[i] != NULL) { VCALL0(s->swap_buffers[i], Release); s->swap_buffers[i] = NULL; }
    }

    if (s->rtv_heap   != NULL) { VCALL0(s->rtv_heap, Release);   s->rtv_heap   = NULL; }
    if (s->swapchain  != NULL) { VCALL0(s->swapchain, Release);  s->swapchain  = NULL; }
    if (s->queue      != NULL) { VCALL0(s->queue, Release);      s->queue      = NULL; }
    if (s->device     != NULL) { VCALL0(s->device, Release);     s->device     = NULL; }
    if (s->fence      != NULL) { VCALL0(s->fence, Release);      s->fence      = NULL; }
    if (s->fence_event != NULL) { CloseHandle(s->fence_event);   s->fence_event = NULL; }

    memset(s, 0, sizeof(*s));
}

//============================================================================
// PUBLIC: begin_frame / submit_rect / end_frame
//============================================================================

void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    s->viewport_w             = viewport_w;
    s->viewport_h             = viewport_h;
    s->clear_color            = clear;
    s->rect_vert_count        = 0;
    s->rect_draw_cursor       = 0;
    s->text_vert_count        = 0;
    s->text_run_count         = 0;
    s->text_first_undrawn_run = 0;
    s->image_vert_count       = 0;

    //
    // Wait for the GPU to be done with the slot we're about to
    // reuse. fence_values[slot] was set in end_frame when that
    // slot's cmd list was submitted; we block here if the GPU
    // hasn't reached it yet.
    //
    UINT slot = s->frame_slot;
    _d3d12_renderer_internal__wait_for_fence(s->fence_values[slot]);

    ID3D12CommandAllocator*    alloc = s->allocators[slot];
    ID3D12GraphicsCommandList* list  = s->cmd_lists[slot];

    //
    // Reset allocator + list before recording new commands for
    // this frame. Both must be idle (GPU done) before Reset.
    //
    VCALL0(alloc, Reset);
    VCALL(list, Reset, alloc, s->rect_pso);

    s->backbuffer_index = VCALL0(s->swapchain, GetCurrentBackBufferIndex);

    //
    // Transition: PRESENT -> RENDER_TARGET for the current backbuffer.
    //
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = s->swap_buffers[s->backbuffer_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    VCALL(list, ResourceBarrier, 1, &barrier);

    //
    // Bind the current backbuffer RTV and clear it.
    //
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->swap_rtvs[s->backbuffer_index];
    VCALL(list, OMSetRenderTargets, 1, &rtv, FALSE, NULL);

    float clear_col[4] = { clear.r, clear.g, clear.b, clear.a };
    VCALL(list, ClearRenderTargetView, rtv, clear_col, 0, NULL);

    //
    // Static pipeline state for this frame: root sig + rect PSO
    // bound (already bound via Reset(pso)), viewport + default
    // scissor set to the full backbuffer.
    //
    VCALL(list, SetGraphicsRootSignature, s->root_sig);

    D3D12_VIEWPORT vp = { 0 };
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = (float)viewport_w;
    vp.Height   = (float)viewport_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    VCALL(list, RSSetViewports, 1, &vp);

    D3D12_RECT sc = { 0 };
    sc.left   = 0;
    sc.top    = 0;
    sc.right  = (LONG)viewport_w;
    sc.bottom = (LONG)viewport_h;
    VCALL(list, RSSetScissorRects, 1, &sc);

    //
    // Push the viewport into the root 32-bit constants for the VS.
    //
    float pc[4] = { (float)viewport_w, (float)viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);

    s->frame_active = TRUE;
}

//
// Push one vertex into the rect VBO. Factored out so submit_rect and
// submit_rect_gradient can share it -- the only thing they disagree on
// is whether all six corners get the same color or four distinct ones.
//
static void _d3d12_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    _d3d12_renderer_internal__vertex* v = &s->rect_vbo_ptr[s->frame_slot][s->rect_vert_count];
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
    s->rect_vert_count++;
}

void renderer__submit_rect(gui_rect r, gui_color c, float radius)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }
    //
    // Buffer-full check: drop the rect and log once per frame. We
    // can't flush-and-reuse because the UPLOAD buffer is persistent-
    // mapped and the already-recorded DrawInstanced commands would
    // re-read the lower offsets as we overwrite them. Bump
    // MAX_QUADS if a real UI hits this.
    //
    if (s->rect_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _D3D12_RENDERER_INTERNAL__MAX_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    float half_min = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius > half_min) { radius = half_min; }
    if (radius < 0.0f)     { radius = 0.0f; }

    float x0 = r.x,         y0 = r.y;
    float x1 = r.x + r.w,   y1 = r.y + r.h;

    //
    // Two triangles: (tl, tr, bl) + (tr, br, bl). Vertex ordering
    // matches every other backend's per-VISUAL-CONTRACT layout.
    //
    _d3d12_renderer_internal__push_vertex(x0, y0, 0.0f, 0.0f, r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x1, y1, r.w,  r.h,  r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
}

static void _d3d12_renderer_internal__flush_rects(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }

    int64 pending = s->rect_vert_count - s->rect_draw_cursor;
    if (pending <= 0) { return; }

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    //
    // Draw the pending slice [rect_draw_cursor .. rect_vert_count).
    // VBV points at the start of the buffer; StartVertexLocation in
    // DrawInstanced offsets the draw inside it. Critically, we do
    // NOT reset rect_vert_count here -- the command list still
    // references these verts by offset, so subsequent CPU writes
    // must go to a HIGHER offset to avoid stomping. Cursor advances
    // to vert_count; next flush will pick up from there.
    //
    D3D12_VERTEX_BUFFER_VIEW vbv = { 0 };
    vbv.BufferLocation = VCALL0(s->rect_vbo[s->frame_slot], GetGPUVirtualAddress);
    vbv.SizeInBytes    = (UINT)(sizeof(_d3d12_renderer_internal__vertex) * s->rect_vert_count);
    vbv.StrideInBytes  = sizeof(_d3d12_renderer_internal__vertex);
    VCALL(list, IASetVertexBuffers, 0, 1, &vbv);
    VCALL(list, IASetPrimitiveTopology, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    VCALL(list, DrawInstanced, (UINT)pending, 1, (UINT)s->rect_draw_cursor, 0);

    s->rect_draw_cursor = s->rect_vert_count;
}

void renderer__flush_pending_draws(void)
{
    // Public flush so scene_render can break the per-frame batch at
    // z-index sibling boundaries. See renderer.h for the full reason.
    // Rects first, text second -- same ordering as end_frame.
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }
    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();
}

void renderer__end_frame(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }

    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    //
    // Transition: RENDER_TARGET -> PRESENT before Close.
    //
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = s->swap_buffers[s->backbuffer_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    VCALL(list, ResourceBarrier, 1, &barrier);

    VCALL0(list, Close);

    ID3D12CommandList* submit_lists[1] = { (ID3D12CommandList*)list };
    VCALL(s->queue, ExecuteCommandLists, 1, submit_lists);

    //
    // Present -- interval 1 = vsync, same semantics as d3d11 /
    // eglSwapBuffers / SwapBuffers.
    //
    HRESULT pr = VCALL(s->swapchain, Present, 1, 0);
    if (FAILED(pr))
    {
        log_warn("IDXGISwapChain3::Present failed: 0x%08x", (unsigned)pr);
    }

    //
    // Signal the fence after present so the next wait_for_fence
    // for this slot (on begin_frame in two frames' time) can
    // guarantee the GPU has completed all its work.
    //
    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    s->fence_values[s->frame_slot] = s->next_fence_value;

    s->frame_active = FALSE;
    s->frame_slot   = (s->frame_slot + 1) % _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT;
}

//============================================================================
// text / image pipelines -- atlas upload, glyph submit, texture draw
//============================================================================

void* renderer__create_atlas_r8(const ubyte* pixels, int w, int h)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (pixels == NULL || w <= 0 || h <= 0) { return NULL; }
    //
    // Text + image root sig / pipelines are built lazily on the first
    // atlas or texture request. Doing it here (rather than in init)
    // means a PoC that never uses text avoids the D3DCompile cost.
    //
    if (!s->tx_built)
    {
        if (!_d3d12_renderer_internal__create_tx_root_and_pipelines()) { return NULL; }
        s->tx_built = TRUE;
    }
    _d3d12_renderer_internal__tex_entry* e = _d3d12_renderer_internal__alloc_tex_entry();
    if (e == NULL) { log_error("d3d12 atlas: pool full"); return NULL; }
    if (!_d3d12_renderer_internal__upload_texture(e, pixels, w, h, DXGI_FORMAT_R8_UNORM, 1))
    {
        _d3d12_renderer_internal__release_tex_entry(e);
        return NULL;
    }
    return (void*)e;
}

void renderer__destroy_atlas(void* atlas)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (atlas == NULL) { return; }
    _d3d12_renderer_internal__tex_entry* e = (_d3d12_renderer_internal__tex_entry*)atlas;
    //
    // Wait for any in-flight frame that might still reference this
    // atlas via a recorded command list. Textures in this renderer
    // live for the whole process under normal use (font__shutdown is
    // the one caller), so blocking here is cheap -- better than
    // risking a use-after-free mid-flight.
    //
    _d3d12_renderer_internal__wait_for_gpu();
    if (s->current_text_atlas == e) { s->current_text_atlas = NULL; }
    _d3d12_renderer_internal__release_tex_entry(e);
}

void renderer__set_text_atlas(void* atlas)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    s->current_text_atlas = (_d3d12_renderer_internal__tex_entry*)atlas;
}

void renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active)            { return; }
    if (s->current_text_atlas == NULL) { return; }
    if (s->text_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    //
    // Open or extend a run keyed by the current atlas. end_frame walks
    // the run list, binds each run's SRV once, draws that run's vert
    // range. Matches d3d11's text-run batching.
    //
    // Critical: we may only extend the LAST run if it has NOT been
    // flushed yet. Mid-frame flushes (push_scissor, submit_image)
    // advance text_first_undrawn_run past already-drawn runs; if we
    // extended one of those, the new glyphs would live inside its
    // recorded [vert_start..vert_start+vert_count) range but no
    // subsequent DrawInstanced would cover them -- first_undrawn is
    // already past this run, so flush_text wouldn't re-draw it.
    // Bug surfaced as "popup text invisible" because the popup paints
    // after the main tree's scroll pop_scissor flushed text, sharing
    // the same atlas with the main tree.
    //
    _d3d12_renderer_internal__text_run* run = NULL;
    if (s->text_run_count > s->text_first_undrawn_run)
    {
        run = &s->text_runs[s->text_run_count - 1];
        if (run->atlas != s->current_text_atlas) { run = NULL; }
    }
    if (run == NULL)
    {
        if (s->text_run_count >= _D3D12_RENDERER_INTERNAL__MAX_TEXT_RUNS) { return; }
        run = &s->text_runs[s->text_run_count++];
        run->atlas      = s->current_text_atlas;
        run->vert_start = s->text_vert_count;
        run->vert_count = 0;
    }

    float x0 = rect.x, y0 = rect.y;
    float x1 = rect.x + rect.w, y1 = rect.y + rect.h;
    float u0 = uv.x, v0 = uv.y;
    float u1 = uv.x + uv.w, v1 = uv.y + uv.h;

    _d3d12_renderer_internal__tx_vertex* v = &s->text_vbo_ptr[s->frame_slot][s->text_vert_count];
    #define _PUSH_TX(IDX, X, Y, U, V) do { \
        v[IDX].x = (X); v[IDX].y = (Y); \
        v[IDX].r = color.r; v[IDX].g = color.g; v[IDX].b = color.b; v[IDX].a = color.a; \
        v[IDX].u = (U); v[IDX].v = (V); \
    } while (0)
    _PUSH_TX(0, x0, y0, u0, v0);
    _PUSH_TX(1, x1, y0, u1, v0);
    _PUSH_TX(2, x0, y1, u0, v1);
    _PUSH_TX(3, x1, y0, u1, v0);
    _PUSH_TX(4, x1, y1, u1, v1);
    _PUSH_TX(5, x0, y1, u0, v1);
    #undef _PUSH_TX

    s->text_vert_count += _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD;
    run->vert_count    += _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

void* renderer__create_texture_rgba(const ubyte* rgba, int w, int h)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (rgba == NULL || w <= 0 || h <= 0) { return NULL; }
    if (!s->tx_built)
    {
        if (!_d3d12_renderer_internal__create_tx_root_and_pipelines()) { return NULL; }
        s->tx_built = TRUE;
    }
    _d3d12_renderer_internal__tex_entry* e = _d3d12_renderer_internal__alloc_tex_entry();
    if (e == NULL) { log_error("d3d12 image: pool full"); return NULL; }
    if (!_d3d12_renderer_internal__upload_texture(e, rgba, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 4))
    {
        _d3d12_renderer_internal__release_tex_entry(e);
        return NULL;
    }
    return (void*)e;
}

void renderer__destroy_texture(void* tex)
{
    if (tex == NULL) { return; }
    _d3d12_renderer_internal__tex_entry* e = (_d3d12_renderer_internal__tex_entry*)tex;
    _d3d12_renderer_internal__wait_for_gpu();
    _d3d12_renderer_internal__release_tex_entry(e);
}

void renderer__submit_image(gui_rect r, void* tex, gui_color tint)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }
    if (tex == NULL)
    {
        //
        // Placeholder: let scene's fitted-image fallback handle it.
        // Caller passes a tinted gradient rect in that case.
        //
        return;
    }
    _d3d12_renderer_internal__tex_entry* e = (_d3d12_renderer_internal__tex_entry*)tex;

    //
    // Flush pending rects + text first so this image draws on top
    // of anything queued before it but under anything queued after.
    // Matches the submit-order = render-order invariant.
    //
    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();

    //
    // Write 6 verts into the per-frame image VBO.
    //
    if (s->image_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }
    int64 vstart = s->image_vert_count;
    _d3d12_renderer_internal__tx_vertex* v = &s->image_vbo_ptr[s->frame_slot][vstart];
    float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    #define _PUSH_IMG(IDX, X, Y, U, V) do { \
        v[IDX].x = (X); v[IDX].y = (Y); \
        v[IDX].r = tint.r; v[IDX].g = tint.g; v[IDX].b = tint.b; v[IDX].a = tint.a; \
        v[IDX].u = (U); v[IDX].v = (V); \
    } while (0)
    _PUSH_IMG(0, x0, y0, 0.0f, 0.0f);
    _PUSH_IMG(1, x1, y0, 1.0f, 0.0f);
    _PUSH_IMG(2, x0, y1, 0.0f, 1.0f);
    _PUSH_IMG(3, x1, y0, 1.0f, 0.0f);
    _PUSH_IMG(4, x1, y1, 1.0f, 1.0f);
    _PUSH_IMG(5, x0, y1, 0.0f, 1.0f);
    #undef _PUSH_IMG
    s->image_vert_count += _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD;

    //
    // Bind image pipeline + root sig + descriptor heap + SRV slot,
    // push viewport, issue draw for these 6 verts. Leaves state
    // such that the next flush_rects re-binds the rect pipeline.
    //
    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    VCALL(list, SetGraphicsRootSignature, s->tx_root_sig);
    ID3D12DescriptorHeap* heaps[1] = { s->srv_heap };
    VCALL(list, SetDescriptorHeaps, 1, heaps);
    VCALL(list, SetGraphicsRootDescriptorTable, 1, e->srv_gpu);

    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);

    VCALL(list, SetPipelineState, s->image_pso);

    D3D12_VERTEX_BUFFER_VIEW vbv = { 0 };
    vbv.BufferLocation = VCALL0(s->image_vbo[s->frame_slot], GetGPUVirtualAddress);
    vbv.BufferLocation += (UINT64)(sizeof(_d3d12_renderer_internal__tx_vertex) * vstart);
    vbv.SizeInBytes    = (UINT)(sizeof(_d3d12_renderer_internal__tx_vertex) * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD);
    vbv.StrideInBytes  = sizeof(_d3d12_renderer_internal__tx_vertex);
    VCALL(list, IASetVertexBuffers, 0, 1, &vbv);
    VCALL(list, IASetPrimitiveTopology, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    VCALL(list, DrawInstanced, _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD, 1, 0, 0);

    //
    // Re-bind the rect root sig + PSO so subsequent submit_rect calls
    // don't need to do any work besides appending verts. Mirrors how
    // the d3d11 path leaves image state such that the next rect
    // flush rebinds cleanly via flush_batches.
    //
    _d3d12_renderer_internal__bind_rect_pipeline(list);
}

//
// Shadow: concentric layered rects with quadratic falloff. The rect
// pipeline's per-fragment SDF AA handles the soft edge; no special
// shader needed. Layer count scales with blur radius. Same algorithm
// as d3d11 / opengl3 / gles3.
//
void renderer__submit_shadow(gui_rect r, gui_color c, float radius, float blur)
{
    if (c.a <= 0.0f) { return; }
    int layers = (int)(blur * 0.4f) + 2;
    if (layers < 2) { layers = 2; }
    if (layers > 8) { layers = 8; }
    float step = blur / (float)layers;
    for (int i = 0; i < layers; i++)
    {
        float grow    = step * (float)i;
        float t       = (float)i / (float)(layers - 1);
        float falloff = (1.0f - t) * (1.0f - t);
        gui_color layer_c = c;
        layer_c.a = c.a * falloff / (float)layers;
        gui_rect lr;
        lr.x = r.x - grow;
        lr.y = r.y - grow;
        lr.w = r.w + 2.0f * grow;
        lr.h = r.h + 2.0f * grow;
        renderer__submit_rect(lr, layer_c, radius + grow);
    }
}

//
// Gradient: per-corner color on the existing SDF-rect pipeline. The
// VS passes `color` through and the PS interpolates across the triangle,
// so two-color lerps cost nothing extra on the GPU -- just different
// corner colors fed through push_vertex. Direction maps onto which
// corners get `from` vs `to`:
//
//   VERTICAL       from at top row, to at bottom row.
//   HORIZONTAL     from at left column, to at right column.
//   DIAGONAL_TL    from at top-left corner, to at bottom-right; the
//                  other two corners get the midpoint so the interp
//                  follows the correct axis rather than a triangle-
//                  induced diagonal.
//   DIAGONAL_TR    mirror of the above.
//
void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }
    //
    // Same buffer-full drop rule as submit_rect; see comment there.
    //
    if (s->rect_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _D3D12_RENDERER_INTERNAL__MAX_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    float half_min = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius > half_min) { radius = half_min; }
    if (radius < 0.0f)     { radius = 0.0f; }

    float x0 = r.x,         y0 = r.y;
    float x1 = r.x + r.w,   y1 = r.y + r.h;

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

    _d3d12_renderer_internal__push_vertex(x0, y0, 0.0f, 0.0f, r.w, r.h, radius, c_tl);
    _d3d12_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c_tr);
    _d3d12_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c_bl);
    _d3d12_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c_tr);
    _d3d12_renderer_internal__push_vertex(x1, y1, r.w,  r.h,  r.w, r.h, radius, c_br);
    _d3d12_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c_bl);
}

//============================================================================
// scissor
//============================================================================

void renderer__push_scissor(gui_rect r)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (s->scissor_depth >= _D3D12_RENDERER_INTERNAL__MAX_SCISSORS) { return; }

    //
    // Flush both rect and text batches before the scissor state
    // change so anything already queued draws under the OLD scissor.
    // flush_text restores text state; we re-bind rect afterwards so
    // subsequent submit_rect flushes don't see stale tx pipeline.
    //
    _d3d12_renderer_internal__flush_rects();
    if (s->text_run_count > 0)
    {
        _d3d12_renderer_internal__flush_text();
        if (s->frame_active)
        {
            _d3d12_renderer_internal__bind_rect_pipeline(s->cmd_lists[s->frame_slot]);
        }
    }

    LONG x0 = (LONG)r.x;
    LONG y0 = (LONG)r.y;
    LONG x1 = (LONG)(r.x + r.w);
    LONG y1 = (LONG)(r.y + r.h);
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (LONG)s->viewport_w) { x1 = (LONG)s->viewport_w; }
    if (y1 > (LONG)s->viewport_h) { y1 = (LONG)s->viewport_h; }

    if (s->scissor_depth > 0)
    {
        D3D12_RECT prev = s->scissors[s->scissor_depth - 1].rect;
        if (x0 < prev.left)   { x0 = prev.left; }
        if (y0 < prev.top)    { y0 = prev.top; }
        if (x1 > prev.right)  { x1 = prev.right; }
        if (y1 > prev.bottom) { y1 = prev.bottom; }
    }
    if (x1 < x0) { x1 = x0; }
    if (y1 < y0) { y1 = y0; }

    D3D12_RECT sc = { x0, y0, x1, y1 };
    s->scissors[s->scissor_depth].rect   = sc;
    s->scissors[s->scissor_depth].active = TRUE;
    s->scissor_depth++;

    if (s->frame_active)
    {
        VCALL(s->cmd_lists[s->frame_slot], RSSetScissorRects, 1, &sc);
    }
}

void renderer__pop_scissor(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (s->scissor_depth <= 0) { return; }

    //
    // Mirror push_scissor: both batches flush before the scissor
    // pop so queued draws land under the inner scissor.
    //
    _d3d12_renderer_internal__flush_rects();
    if (s->text_run_count > 0)
    {
        _d3d12_renderer_internal__flush_text();
        if (s->frame_active)
        {
            _d3d12_renderer_internal__bind_rect_pipeline(s->cmd_lists[s->frame_slot]);
        }
    }

    s->scissor_depth--;

    if (!s->frame_active) { return; }

    D3D12_RECT sc;
    if (s->scissor_depth > 0)
    {
        sc = s->scissors[s->scissor_depth - 1].rect;
    }
    else
    {
        sc.left   = 0;
        sc.top    = 0;
        sc.right  = (LONG)s->viewport_w;
        sc.bottom = (LONG)s->viewport_h;
    }
    VCALL(s->cmd_lists[s->frame_slot], RSSetScissorRects, 1, &sc);
}

void renderer__blur_region(gui_rect rect, float sigma_px)
{
    //
    // Placeholder: D3D12 doesn't yet implement real backdrop blur (would
    // need capture to a staging RTV, separable Gaussian via two compute
    // or graphics passes, composite back). Translucent darken splat
    // stands in so the blur-styled region reads as "muted backdrop".
    //
    (void)sigma_px;
    gui_color dim = { 0.0f, 0.0f, 0.0f, 0.15f };
    renderer__submit_rect(rect, dim, 0.0f);
}

//============================================================================
// IMPL: device / swap / RTVs / command / fence / root sig / PSO / VBO
//============================================================================

static boole _d3d12_renderer_internal__create_device_and_queue(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

#if !defined(NDEBUG)
    //
    // Debug layer: catches invalid API usage (wrong resource states,
    // unbound descriptors, mismatched root signatures, etc.) and
    // logs to OutputDebugString. Only available when the Windows
    // Graphics Tools feature is installed. Silently continues if
    // not available (release builds, or Windows SKUs without the
    // Graphics Tools optional feature).
    //
    {
        ID3D12Debug* dbg = NULL;
        HRESULT hr_dbg = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg);
        if (SUCCEEDED(hr_dbg) && dbg != NULL)
        {
            VCALL0(dbg, EnableDebugLayer);
            VCALL0(dbg, Release);
            log_info("d3d12 debug layer ON");
        }
    }
#endif

    //
    // Default adapter (NULL) at feature level 11.0. D3D12 picks
    // the best available driver.
    //
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                                   &IID_ID3D12Device, (void**)&s->device);
    if (FAILED(hr))
    {
        log_error("D3D12CreateDevice failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }

#if !defined(NDEBUG)
    //
    // Route the debug layer's messages into log_warn so validation
    // errors show up in the same console the host app uses for all
    // other logging, not just the IDE's Debug Output window. Also
    // enables "break on error" in the InfoQueue so the debugger
    // stops on the offending call (useful under VS; harmless when
    // running standalone).
    //
    {
        ID3D12InfoQueue* iq = NULL;
        HRESULT hr_iq = VCALL(s->device, QueryInterface, &IID_ID3D12InfoQueue, (void**)&iq);
        if (SUCCEEDED(hr_iq) && iq != NULL)
        {
            VCALL(iq, SetBreakOnSeverity, D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
            VCALL(iq, SetBreakOnSeverity, D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            VCALL0(iq, Release);
            log_info("d3d12 info queue: break-on-error armed");
        }
    }
#endif

    D3D12_COMMAND_QUEUE_DESC cqd = { 0 };
    cqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    cqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = VCALL(s->device, CreateCommandQueue, &cqd, &IID_ID3D12CommandQueue, (void**)&s->queue);
    if (FAILED(hr))
    {
        log_error("ID3D12Device::CreateCommandQueue failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_swapchain(HWND hwnd)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    IDXGIFactory4* factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr))
    {
        log_error("CreateDXGIFactory1 failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0) { w = 800; }
    if (h <= 0) { h = 600; }
    s->viewport_w = w;
    s->viewport_h = h;

    DXGI_SWAP_CHAIN_DESC1 scd = { 0 };
    scd.Width       = (UINT)w;
    scd.Height      = (UINT)h;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferCount = _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;

    IDXGISwapChain1* sc1 = NULL;
    hr = VCALL(factory, CreateSwapChainForHwnd,
               (IUnknown*)s->queue, hwnd, &scd, NULL, NULL, &sc1);
    if (FAILED(hr))
    {
        log_error("CreateSwapChainForHwnd failed: 0x%08x", (unsigned)hr);
        VCALL0(factory, Release);
        return FALSE;
    }

    //
    // Disable DXGI's alt-enter fullscreen -- we handle window
    // state ourselves.
    //
    VCALL(factory, MakeWindowAssociation, hwnd, DXGI_MWA_NO_ALT_ENTER);

    //
    // Upgrade to IDXGISwapChain3 for GetCurrentBackBufferIndex.
    //
    hr = VCALL(sc1, QueryInterface, &IID_IDXGISwapChain3, (void**)&s->swapchain);
    VCALL0(sc1, Release);
    VCALL0(factory, Release);
    if (FAILED(hr))
    {
        log_error("QueryInterface IDXGISwapChain3 failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_rtvs(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    D3D12_DESCRIPTOR_HEAP_DESC hd = { 0 };
    hd.NumDescriptors = _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = VCALL(s->device, CreateDescriptorHeap, &hd,
                       &IID_ID3D12DescriptorHeap, (void**)&s->rtv_heap);
    if (FAILED(hr))
    {
        log_error("CreateDescriptorHeap (RTV) failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }

    s->rtv_stride = VCALL(s->device, GetDescriptorHandleIncrementSize, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    VCALL(s->rtv_heap, GetCPUDescriptorHandleForHeapStart, &handle);

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT; i++)
    {
        hr = VCALL(s->swapchain, GetBuffer, (UINT)i,
                   &IID_ID3D12Resource, (void**)&s->swap_buffers[i]);
        if (FAILED(hr))
        {
            log_error("IDXGISwapChain3::GetBuffer[%d] failed: 0x%08x", i, (unsigned)hr);
            return FALSE;
        }
        VCALL(s->device, CreateRenderTargetView, s->swap_buffers[i], NULL, handle);
        s->swap_rtvs[i] = handle;
        handle.ptr += s->rtv_stride;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_command_objects(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = VCALL(s->device, CreateCommandAllocator,
                           D3D12_COMMAND_LIST_TYPE_DIRECT,
                           &IID_ID3D12CommandAllocator,
                           (void**)&s->allocators[i]);
        if (FAILED(hr))
        {
            log_error("CreateCommandAllocator[%d] failed: 0x%08x", i, (unsigned)hr);
            return FALSE;
        }

        hr = VCALL(s->device, CreateCommandList, 0,
                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                   s->allocators[i], NULL,
                   &IID_ID3D12GraphicsCommandList,
                   (void**)&s->cmd_lists[i]);
        if (FAILED(hr))
        {
            log_error("CreateCommandList[%d] failed: 0x%08x", i, (unsigned)hr);
            return FALSE;
        }
        //
        // Lists are created in the recording state; close them
        // so begin_frame's Reset call is symmetric.
        //
        VCALL0(s->cmd_lists[i], Close);
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_fence(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    HRESULT hr = VCALL(s->device, CreateFence, 0, D3D12_FENCE_FLAG_NONE,
                       &IID_ID3D12Fence, (void**)&s->fence);
    if (FAILED(hr))
    {
        log_error("ID3D12Device::CreateFence failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }
    s->next_fence_value = 0;
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        s->fence_values[i] = 0;
    }

    s->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (s->fence_event == NULL)
    {
        log_error("CreateEventW failed: %lu", (unsigned long)GetLastError());
        return FALSE;
    }
    return TRUE;
}

static void _d3d12_renderer_internal__wait_for_fence(UINT64 value)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    UINT64 completed = VCALL0(s->fence, GetCompletedValue);
    if (completed < value)
    {
        VCALL(s->fence, SetEventOnCompletion, value, s->fence_event);
        WaitForSingleObject(s->fence_event, INFINITE);
    }
}

static void _d3d12_renderer_internal__wait_for_gpu(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    _d3d12_renderer_internal__wait_for_fence(s->next_fence_value);
}

static boole _d3d12_renderer_internal__create_root_signature(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    //
    // One root parameter: 4 32-bit constants (viewport.x,
    // viewport.y, _pad.x, _pad.y). Mapped to b0 in the HLSL as
    // `cbuffer RootCB`. Cheaper than a CBV for 16 bytes.
    //
    D3D12_ROOT_PARAMETER rp = { 0 };
    rp.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 4;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.RegisterSpace  = 0;
    rp.ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    rsd.NumParameters = 1;
    rsd.pParameters   = &rp;
    rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err != NULL)
        {
            log_error("D3D12SerializeRootSignature: %s", (const char*)VCALL0(err, GetBufferPointer));
            VCALL0(err, Release);
        }
        return FALSE;
    }
    if (err != NULL) { VCALL0(err, Release); }

    hr = VCALL(s->device, CreateRootSignature, 0,
               VCALL0(sig, GetBufferPointer), VCALL0(sig, GetBufferSize),
               &IID_ID3D12RootSignature, (void**)&s->root_sig);
    VCALL0(sig, Release);

    if (FAILED(hr))
    {
        log_error("ID3D12Device::CreateRootSignature failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_rect_pso(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    ID3DBlob* vs = NULL;
    ID3DBlob* ps = NULL;
    ID3DBlob* err = NULL;

    HRESULT hr = D3DCompile(
        _D3D12_RENDERER_INTERNAL__HLSL_SRC, strlen(_D3D12_RENDERER_INTERNAL__HLSL_SRC),
        "rect.hlsl", NULL, NULL, "VSMain", "vs_5_0",
        0, 0, &vs, &err);
    if (FAILED(hr))
    {
        if (err != NULL) { log_error("D3DCompile VS: %s", (const char*)VCALL0(err, GetBufferPointer)); VCALL0(err, Release); }
        return FALSE;
    }
    if (err != NULL) { VCALL0(err, Release); err = NULL; }

    hr = D3DCompile(
        _D3D12_RENDERER_INTERNAL__HLSL_SRC, strlen(_D3D12_RENDERER_INTERNAL__HLSL_SRC),
        "rect.hlsl", NULL, NULL, "PSMain", "ps_5_0",
        0, 0, &ps, &err);
    if (FAILED(hr))
    {
        if (err != NULL) { log_error("D3DCompile PS: %s", (const char*)VCALL0(err, GetBufferPointer)); VCALL0(err, Release); }
        VCALL0(vs, Release);
        return FALSE;
    }
    if (err != NULL) { VCALL0(err, Release); }

    //
    // IA layout: same 11-float vertex as every other backend,
    // split into five named elements matching the HLSL struct.
    //
    //
    // DXGI uses *_FLOAT for IEEE-float formats (no "S" prefix --
    // that's the Vulkan VK_FORMAT_*_SFLOAT spelling). Got crossed
    // initially because both APIs ship side-by-side in this repo.
    //
    D3D12_INPUT_ELEMENT_DESC ied[5] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__vertex, x),      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(_d3d12_renderer_internal__vertex, r),      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__vertex, lx),     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__vertex, rect_w), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, offsetof(_d3d12_renderer_internal__vertex, radius), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = { 0 };
    pso.pRootSignature                     = s->root_sig;
    pso.VS.pShaderBytecode                 = VCALL0(vs, GetBufferPointer);
    pso.VS.BytecodeLength                  = VCALL0(vs, GetBufferSize);
    pso.PS.pShaderBytecode                 = VCALL0(ps, GetBufferPointer);
    pso.PS.BytecodeLength                  = VCALL0(ps, GetBufferSize);
    pso.InputLayout.pInputElementDescs     = ied;
    pso.InputLayout.NumElements            = 5;
    pso.PrimitiveTopologyType              = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets                   = 1;
    pso.RTVFormats[0]                      = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat                          = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count                   = 1;
    pso.SampleMask                         = UINT_MAX;

    // Rasterizer: no cull, no depth, no scissor default-disabled (we enable dynamically).
    pso.RasterizerState.FillMode           = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode           = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable    = TRUE;

    // Depth-stencil: disabled. We don't use a DS view.
    pso.DepthStencilState.DepthEnable      = FALSE;
    pso.DepthStencilState.StencilEnable    = FALSE;

    // Blend: separate-alpha per the visual contract.
    pso.BlendState.AlphaToCoverageEnable   = FALSE;
    pso.BlendState.IndependentBlendEnable  = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable            = TRUE;
    pso.BlendState.RenderTarget[0].LogicOpEnable          = FALSE;
    pso.BlendState.RenderTarget[0].SrcBlend               = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend              = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp                = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha          = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha         = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha           = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr2 = VCALL(s->device, CreateGraphicsPipelineState, &pso,
                        &IID_ID3D12PipelineState, (void**)&s->rect_pso);
    VCALL0(vs, Release);
    VCALL0(ps, Release);

    if (FAILED(hr2))
    {
        log_error("CreateGraphicsPipelineState (rect) failed: 0x%08x", (unsigned)hr2);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_rect_vertex_buffers(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    UINT64 size = (UINT64)(sizeof(_d3d12_renderer_internal__vertex) *
                          _D3D12_RENDERER_INTERNAL__MAX_QUADS *
                          _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD);

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask     = 1;
    hp.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment           = 0;
    rd.Width               = size;
    rd.Height              = 1;
    rd.DepthOrArraySize    = 1;
    rd.MipLevels           = 1;
    rd.Format              = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count    = 1;
    rd.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags               = D3D12_RESOURCE_FLAG_NONE;

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = VCALL(s->device, CreateCommittedResource,
                           &hp, D3D12_HEAP_FLAG_NONE, &rd,
                           D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                           &IID_ID3D12Resource, (void**)&s->rect_vbo[i]);
        if (FAILED(hr))
        {
            log_error("CreateCommittedResource (rect vbo %d) failed: 0x%08x", i, (unsigned)hr);
            return FALSE;
        }

        //
        // Persistent map -- same strategy as the Vulkan backend.
        // UPLOAD heap is CPU-write-combined, fast enough for a
        // 192 KB buffer streaming per frame.
        //
        D3D12_RANGE read_range = { 0, 0 };
        hr = VCALL(s->rect_vbo[i], Map, 0, &read_range, (void**)&s->rect_vbo_ptr[i]);
        if (FAILED(hr))
        {
            log_error("Map (rect vbo %d) failed: 0x%08x", i, (unsigned)hr);
            return FALSE;
        }
    }
    return TRUE;
}

//============================================================================
// text + image support
//============================================================================

static boole _d3d12_renderer_internal__create_srv_heap(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    //
    // Shader-visible CBV_SRV_UAV heap. `SHADER_VISIBLE` is required so
    // `SetGraphicsRootDescriptorTable` can reference slots in it.
    // Capacity = _MAX_SRVS is sized for worst-case atlases + images.
    //
    D3D12_DESCRIPTOR_HEAP_DESC hd = { 0 };
    hd.NumDescriptors = _D3D12_RENDERER_INTERNAL__MAX_SRVS;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = VCALL(s->device, CreateDescriptorHeap, &hd, &IID_ID3D12DescriptorHeap, (void**)&s->srv_heap);
    if (FAILED(hr))
    {
        log_error("CreateDescriptorHeap (SRV) failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }
    s->srv_stride = VCALL(s->device, GetDescriptorHandleIncrementSize, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return TRUE;
}

//
// Text + image VBOs. Upload heap, persistent mapped. Streaming write
// from the CPU each frame, GPU reads once per draw. Same pattern as
// the rect VBO.
//
static boole _d3d12_renderer_internal__create_tx_vertex_buffers(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type             = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    UINT64 size = (UINT64)(sizeof(_d3d12_renderer_internal__tx_vertex) *
                           _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS *
                           _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD);

    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension         = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width             = size;
    rd.Height            = 1;
    rd.DepthOrArraySize  = 1;
    rd.MipLevels         = 1;
    rd.Format            = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count  = 1;
    rd.Layout            = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags             = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RANGE read_range = { 0, 0 };

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = VCALL(s->device, CreateCommittedResource,
                           &hp, D3D12_HEAP_FLAG_NONE, &rd,
                           D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                           &IID_ID3D12Resource, (void**)&s->text_vbo[i]);
        if (FAILED(hr)) { log_error("CreateCommittedResource (text vbo %d) 0x%08x", i, (unsigned)hr); return FALSE; }
        hr = VCALL(s->text_vbo[i], Map, 0, &read_range, (void**)&s->text_vbo_ptr[i]);
        if (FAILED(hr)) { log_error("Map text vbo %d: 0x%08x", i, (unsigned)hr); return FALSE; }

        hr = VCALL(s->device, CreateCommittedResource,
                   &hp, D3D12_HEAP_FLAG_NONE, &rd,
                   D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                   &IID_ID3D12Resource, (void**)&s->image_vbo[i]);
        if (FAILED(hr)) { log_error("CreateCommittedResource (image vbo %d) 0x%08x", i, (unsigned)hr); return FALSE; }
        hr = VCALL(s->image_vbo[i], Map, 0, &read_range, (void**)&s->image_vbo_ptr[i]);
        if (FAILED(hr)) { log_error("Map image vbo %d: 0x%08x", i, (unsigned)hr); return FALSE; }
    }
    return TRUE;
}

//
// Build the text + image root signature (shared) and compile both
// pixel shaders against one shared VS. Root sig layout:
//   slot 0: 4x 32-bit constants (viewport xy + pad)
//   slot 1: 1-SRV descriptor table  (shader register t0)
//   static sampler at s0: LINEAR min/mag, CLAMP uv -- matches every
//   other backend's atlas + image sampling.
//
static boole _d3d12_renderer_internal__create_tx_root_and_pipelines(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    //
    // Descriptor range: 1 SRV starting at t0. The shader reads the
    // current table's first descriptor.
    //
    D3D12_DESCRIPTOR_RANGE range = { 0 };
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors     = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace      = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp[2] = { 0 };
    rp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.RegisterSpace  = 0;
    rp[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges   = &range;
    rp[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC ss = { 0 };
    ss.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxAnisotropy  = 1;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.BorderColor    = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    ss.MinLOD         = 0;
    ss.MaxLOD         = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.RegisterSpace  = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    rsd.NumParameters     = 2;
    rsd.pParameters       = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &ss;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err != NULL) { log_error("tx root sig serialize: %s", (const char*)VCALL0(err, GetBufferPointer)); VCALL0(err, Release); }
        return FALSE;
    }
    if (err != NULL) { VCALL0(err, Release); }

    hr = VCALL(s->device, CreateRootSignature, 0,
               VCALL0(sig, GetBufferPointer), VCALL0(sig, GetBufferSize),
               &IID_ID3D12RootSignature, (void**)&s->tx_root_sig);
    VCALL0(sig, Release);
    if (FAILED(hr))
    {
        log_error("tx CreateRootSignature failed: 0x%08x", (unsigned)hr);
        return FALSE;
    }

    //
    // Compile VS + both PSes from the shared TX HLSL source.
    //
    ID3DBlob* vs = NULL;
    ID3DBlob* ps_text = NULL;
    ID3DBlob* ps_img  = NULL;

    hr = D3DCompile(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC, strlen(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC),
                    "tx.hlsl", NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { if (err != NULL) { log_error("tx VS: %s", (const char*)VCALL0(err, GetBufferPointer)); VCALL0(err, Release); } return FALSE; }
    if (err != NULL) { VCALL0(err, Release); err = NULL; }

    hr = D3DCompile(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC, strlen(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC),
                    "tx.hlsl", NULL, NULL, "PSText", "ps_5_0", 0, 0, &ps_text, &err);
    if (FAILED(hr)) { if (err != NULL) { log_error("tx PSText: %s", (const char*)VCALL0(err, GetBufferPointer)); VCALL0(err, Release); } VCALL0(vs, Release); return FALSE; }
    if (err != NULL) { VCALL0(err, Release); err = NULL; }

    hr = D3DCompile(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC, strlen(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC),
                    "tx.hlsl", NULL, NULL, "PSImage", "ps_5_0", 0, 0, &ps_img, &err);
    if (FAILED(hr)) { if (err != NULL) { log_error("tx PSImage: %s", (const char*)VCALL0(err, GetBufferPointer)); VCALL0(err, Release); } VCALL0(vs, Release); VCALL0(ps_text, Release); return FALSE; }
    if (err != NULL) { VCALL0(err, Release); }

    //
    // IA layout: 2 floats pos + 4 floats color + 2 floats uv.
    //
    D3D12_INPUT_ELEMENT_DESC ied[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__tx_vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(_d3d12_renderer_internal__tx_vertex, r), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__tx_vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = { 0 };
    pso.pRootSignature                    = s->tx_root_sig;
    pso.VS.pShaderBytecode                = VCALL0(vs, GetBufferPointer);
    pso.VS.BytecodeLength                 = VCALL0(vs, GetBufferSize);
    pso.InputLayout.pInputElementDescs    = ied;
    pso.InputLayout.NumElements           = 3;
    pso.PrimitiveTopologyType             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets                  = 1;
    pso.RTVFormats[0]                     = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat                         = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count                  = 1;
    pso.SampleMask                        = UINT_MAX;

    pso.RasterizerState.FillMode          = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode          = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable   = TRUE;

    pso.DepthStencilState.DepthEnable     = FALSE;
    pso.DepthStencilState.StencilEnable   = FALSE;

    // Separate-alpha blend per the visual contract (same as the rect pipeline).
    pso.BlendState.AlphaToCoverageEnable  = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable            = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend               = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend              = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp                = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha          = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha         = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha           = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.PS.pShaderBytecode = VCALL0(ps_text, GetBufferPointer);
    pso.PS.BytecodeLength  = VCALL0(ps_text, GetBufferSize);
    hr = VCALL(s->device, CreateGraphicsPipelineState, &pso, &IID_ID3D12PipelineState, (void**)&s->text_pso);
    if (FAILED(hr)) { log_error("text PSO failed: 0x%08x", (unsigned)hr); VCALL0(vs, Release); VCALL0(ps_text, Release); VCALL0(ps_img, Release); return FALSE; }

    pso.PS.pShaderBytecode = VCALL0(ps_img, GetBufferPointer);
    pso.PS.BytecodeLength  = VCALL0(ps_img, GetBufferSize);
    hr = VCALL(s->device, CreateGraphicsPipelineState, &pso, &IID_ID3D12PipelineState, (void**)&s->image_pso);
    VCALL0(vs, Release);
    VCALL0(ps_text, Release);
    VCALL0(ps_img, Release);
    if (FAILED(hr)) { log_error("image PSO failed: 0x%08x", (unsigned)hr); return FALSE; }

    return TRUE;
}

//
// Next-free-slot allocator over the fixed tex_pool. O(n) scan; pool
// is bounded at MAX_SRVS (256) so this is cheap. Frees put the slot
// back in the pool by flipping in_use.
//
static int _d3d12_renderer_internal__alloc_srv_slot(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__MAX_SRVS; i++)
    {
        if (!s->tex_pool[i].in_use) { return i; }
    }
    return -1;
}

static void _d3d12_renderer_internal__free_srv_slot(int slot)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (slot < 0 || slot >= _D3D12_RENDERER_INTERNAL__MAX_SRVS) { return; }
    s->tex_pool[slot].in_use = FALSE;
}

static _d3d12_renderer_internal__tex_entry* _d3d12_renderer_internal__alloc_tex_entry(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    int slot = _d3d12_renderer_internal__alloc_srv_slot();
    if (slot < 0) { return NULL; }
    _d3d12_renderer_internal__tex_entry* e = &s->tex_pool[slot];
    memset(e, 0, sizeof(*e));
    e->in_use   = TRUE;
    e->srv_slot = slot;
    return e;
}

static void _d3d12_renderer_internal__release_tex_entry(_d3d12_renderer_internal__tex_entry* e)
{
    if (e == NULL) { return; }
    if (e->resource != NULL) { VCALL0(e->resource, Release); e->resource = NULL; }
    _d3d12_renderer_internal__free_srv_slot(e->srv_slot);
}

//
// One-shot synchronous upload helper. Atlas and image loads are rare
// (once per font / image file) so the simple "record + submit + block"
// path beats a queue + batching design. Steps:
//
//   1. Create DEFAULT-heap texture (GPU-only) for the final resource.
//   2. Create UPLOAD-heap staging buffer sized by GetCopyableFootprints
//      (accounts for the 256-byte row-pitch alignment requirement).
//   3. Map the upload buffer and copy pixels row-by-row (respecting
//      RowPitch, which may exceed `w * bytes_per_pixel`).
//   4. Use a dedicated one-shot allocator + command list to record
//      the CopyTextureRegion + transition to PIXEL_SHADER_RESOURCE.
//   5. Execute, signal the fence, block until GPU done.
//   6. Create the SRV in the shader-visible heap at the entry's slot.
//   7. Release staging buffer + command objects.
//
static boole _d3d12_renderer_internal__upload_texture(_d3d12_renderer_internal__tex_entry* e, const ubyte* pixels, int w, int h, DXGI_FORMAT fmt, int bytes_per_pixel)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;

    D3D12_HEAP_PROPERTIES hp_default = { 0 };
    hp_default.Type             = D3D12_HEAP_TYPE_DEFAULT;
    hp_default.CreationNodeMask = 1;
    hp_default.VisibleNodeMask  = 1;

    D3D12_HEAP_PROPERTIES hp_upload = { 0 };
    hp_upload.Type             = D3D12_HEAP_TYPE_UPLOAD;
    hp_upload.CreationNodeMask = 1;
    hp_upload.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC td = { 0 };
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = (UINT64)w;
    td.Height           = (UINT)h;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = VCALL(s->device, CreateCommittedResource,
                       &hp_default, D3D12_HEAP_FLAG_NONE, &td,
                       D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                       &IID_ID3D12Resource, (void**)&e->resource);
    if (FAILED(hr)) { log_error("tex CreateCommittedResource (default) failed: 0x%08x", (unsigned)hr); return FALSE; }

    //
    // Query footprint to learn the UPLOAD buffer size + RowPitch the
    // driver expects. RowPitch is rounded up to 256 bytes per D3D12's
    // ReadRange requirement (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
    //
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = { 0 };
    UINT64 total_bytes = 0;
    UINT   num_rows    = 0;
    UINT64 row_bytes   = 0;
    VCALL(s->device, GetCopyableFootprints, &td, 0, 1, 0, &fp, &num_rows, &row_bytes, &total_bytes);

    D3D12_RESOURCE_DESC bd = { 0 };
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total_bytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* upload = NULL;
    hr = VCALL(s->device, CreateCommittedResource,
               &hp_upload, D3D12_HEAP_FLAG_NONE, &bd,
               D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
               &IID_ID3D12Resource, (void**)&upload);
    if (FAILED(hr)) { log_error("tex upload CreateCommittedResource failed: 0x%08x", (unsigned)hr); VCALL0(e->resource, Release); e->resource = NULL; return FALSE; }

    //
    // Map + row-by-row copy. Row padding (fp.Footprint.RowPitch) may
    // exceed `w * bpp` on some GPUs; copy each logical row into its
    // aligned slot. Upload buffer's offset into its own allocation
    // is fp.Offset, but for a freshly-allocated buffer it's 0.
    //
    D3D12_RANGE read_range = { 0, 0 };
    ubyte* mapped = NULL;
    hr = VCALL(upload, Map, 0, &read_range, (void**)&mapped);
    if (FAILED(hr)) { log_error("tex upload Map failed: 0x%08x", (unsigned)hr); VCALL0(upload, Release); VCALL0(e->resource, Release); e->resource = NULL; return FALSE; }
    UINT row_bpp = (UINT)w * (UINT)bytes_per_pixel;
    for (UINT row = 0; row < num_rows; row++)
    {
        memcpy(mapped + fp.Offset + (size_t)row * fp.Footprint.RowPitch,
               pixels + (size_t)row * row_bpp,
               row_bpp);
    }
    VCALL(upload, Unmap, 0, NULL);

    //
    // One-shot command list. Create dedicated allocator + list so we
    // don't interfere with the per-frame command objects. Record the
    // copy + transition + close + execute + fence + block.
    //
    ID3D12CommandAllocator*    upload_alloc = NULL;
    ID3D12GraphicsCommandList* upload_list  = NULL;
    hr = VCALL(s->device, CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT,
               &IID_ID3D12CommandAllocator, (void**)&upload_alloc);
    if (FAILED(hr)) { log_error("tex upload CreateCommandAllocator: 0x%08x", (unsigned)hr); VCALL0(upload, Release); VCALL0(e->resource, Release); e->resource = NULL; return FALSE; }
    hr = VCALL(s->device, CreateCommandList, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
               upload_alloc, NULL, &IID_ID3D12GraphicsCommandList, (void**)&upload_list);
    if (FAILED(hr)) { log_error("tex upload CreateCommandList: 0x%08x", (unsigned)hr); VCALL0(upload_alloc, Release); VCALL0(upload, Release); VCALL0(e->resource, Release); e->resource = NULL; return FALSE; }

    D3D12_TEXTURE_COPY_LOCATION src = { 0 };
    src.pResource       = upload;
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;

    D3D12_TEXTURE_COPY_LOCATION dst = { 0 };
    dst.pResource        = e->resource;
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    VCALL(upload_list, CopyTextureRegion, &dst, 0, 0, 0, &src, NULL);

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = e->resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    VCALL(upload_list, ResourceBarrier, 1, &barrier);

    VCALL0(upload_list, Close);

    ID3D12CommandList* submit_lists[1] = { (ID3D12CommandList*)upload_list };
    VCALL(s->queue, ExecuteCommandLists, 1, submit_lists);

    //
    // Block until the copy completes. Use the same fence as the main
    // frame loop -- ticks monotonically, no conflict.
    //
    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    _d3d12_renderer_internal__wait_for_fence(s->next_fence_value);

    VCALL0(upload_list, Release);
    VCALL0(upload_alloc, Release);
    VCALL0(upload, Release);

    //
    // Create the SRV in the heap slot the entry already reserved.
    //
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format                    = fmt;
    sv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels       = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    VCALL(s->srv_heap, GetCPUDescriptorHandleForHeapStart, &cpu);
    cpu.ptr += (SIZE_T)e->srv_slot * s->srv_stride;
    VCALL(s->device, CreateShaderResourceView, e->resource, &sv, cpu);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    VCALL(s->srv_heap, GetGPUDescriptorHandleForHeapStart, &gpu);
    gpu.ptr += (UINT64)e->srv_slot * s->srv_stride;
    e->srv_gpu = gpu;

    e->width  = w;
    e->height = h;
    return TRUE;
}

//
// Draw queued text runs. Called from end_frame after the rect batch
// flush. Binds the text pipeline + descriptor heap once, then walks
// the runs array -- each run gets its own SRV binding + draw call.
//
static void _d3d12_renderer_internal__flush_text(void)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    if (!s->frame_active) { return; }
    if (s->text_first_undrawn_run >= s->text_run_count) { return; }

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    VCALL(list, SetGraphicsRootSignature, s->tx_root_sig);
    ID3D12DescriptorHeap* heaps[1] = { s->srv_heap };
    VCALL(list, SetDescriptorHeaps, 1, heaps);
    VCALL(list, SetPipelineState, s->text_pso);

    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);

    //
    // Append-only buffer: each run already records its own
    // vert_start offset into text_vbo, so a single VBV over the
    // whole buffer lets each run's DrawInstanced pick its slice
    // via StartVertexLocation. Don't reset vert_count or run_count
    // here -- same reason as flush_rects; subsequent glyph writes
    // must go to HIGHER offsets to avoid stomping these still-
    // pending draws.
    //
    D3D12_VERTEX_BUFFER_VIEW vbv = { 0 };
    vbv.BufferLocation = VCALL0(s->text_vbo[s->frame_slot], GetGPUVirtualAddress);
    vbv.SizeInBytes    = (UINT)(sizeof(_d3d12_renderer_internal__tx_vertex) * s->text_vert_count);
    vbv.StrideInBytes  = sizeof(_d3d12_renderer_internal__tx_vertex);
    VCALL(list, IASetVertexBuffers, 0, 1, &vbv);
    VCALL(list, IASetPrimitiveTopology, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (int i = s->text_first_undrawn_run; i < s->text_run_count; i++)
    {
        _d3d12_renderer_internal__text_run* run = &s->text_runs[i];
        if (run->atlas == NULL || run->vert_count <= 0) { continue; }
        VCALL(list, SetGraphicsRootDescriptorTable, 1, run->atlas->srv_gpu);
        VCALL(list, DrawInstanced, (UINT)run->vert_count, 1, (UINT)run->vert_start, 0);
    }

    s->text_first_undrawn_run = s->text_run_count;
}

//
// Restore rect pipeline + root sig binding. Called after a text or
// image draw swapped in the tx root sig; subsequent submit_rect
// flushes expect the rect pipeline active.
//
static void _d3d12_renderer_internal__bind_rect_pipeline(ID3D12GraphicsCommandList* list)
{
    _d3d12_renderer_internal__state* s = &_d3d12_renderer_internal__g;
    VCALL(list, SetGraphicsRootSignature, s->root_sig);
    VCALL(list, SetPipelineState, s->rect_pso);
    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);
}
