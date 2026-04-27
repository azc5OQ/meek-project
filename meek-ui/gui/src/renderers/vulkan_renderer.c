//============================================================================
// vulkan_renderer.c - Vulkan 1.1 backend.
//============================================================================
//
// Fourth renderer.h implementation. Status: SCAFFOLD (matches the
// existing d3d11 + d3d9 scaffold level -- solid-rect + scissor
// working, text + image + gradient + shadow stubbed).
//
// Target: portable -- works on Windows, Linux (X11 / Wayland via
// the matching VK_KHR_*_surface extensions), and Android. The
// platform layer creates the VkSurfaceKHR from its native window
// handle before calling renderer__init; the renderer owns the
// swapchain + everything downstream.
//
// VISUAL CONTRACT (from renderer.h): same SDF rounded-box formula,
// smoothstep range, scissor semantics, separate-alpha blend, and
// top-left-origin pixel coordinate space as opengl3 / gles3 /
// d3d11 / d3d9.
//
// DEPENDENCIES:
//   Vulkan 1.1 SDK (LunarG on Windows, vulkan-headers + vulkan-loader
//     on Linux).
//   libshaderc_shared for runtime GLSL->SPIR-V (avoids the hand-
//     maintaining-SPIR-V-bytecode trap; shaderc ships with the
//     Vulkan SDK). Can be swapped for precompiled SPIR-V arrays
//     later if we want to drop the dependency.
//
// INIT SEQUENCE:
//   renderer__init(native_window):
//     1. vkCreateInstance              (validation layers only in debug)
//     2. vkCreateXxxSurfaceKHR         (platform-specific; Win32 or XCB/XLib)
//     3. pick VkPhysicalDevice         (first one that presents to our surface
//                                       + has a graphics-capable queue family)
//     4. vkCreateDevice                (+ one graphics+present queue)
//     5. vkCreateSwapchainKHR          (BGRA8, FIFO = vsync)
//     6. render pass + framebuffers    (one FB per swapchain image)
//     7. command pool + buffers        (one CB per swapchain image)
//     8. semaphores + fences           (MAX_FRAMES_IN_FLIGHT = 2)
//     9. rect pipeline (VS + FS)       (shaderc: GLSL 450 -> SPIR-V)
//    10. vertex buffer                 (host-visible, mapped, per-frame)
//
// FRAME SEQUENCE (per renderer__begin_frame / submit_rect / end_frame):
//   acquire swapchain image -> reset CB -> begin CB -> begin render pass
//     -> bind pipeline + vertex buffer -> draw N rects as one draw
//     -> end render pass -> end CB -> submit -> present
//
// STUBBED:
//   renderer__create_atlas_r8 / destroy / set / submit_text_glyph  returns 0/no-op
//   renderer__create_texture_rgba / destroy / submit_image         returns NULL/no-op
//   renderer__submit_shadow / submit_rect_gradient                 fall back to submit_rect
//
// Scissor push/pop IS implemented via VkCommandBuffer scissor state
// (dynamic) + batch flush on state change, same pattern as every
// other backend.
//

//
// On Windows, vulkan_win32.h references HINSTANCE / HWND / HANDLE /
// DWORD / LPCWSTR / SECURITY_ATTRIBUTES without declaring them, so
// windows.h has to land first. Older Vulkan SDKs forward-declared
// the minimum subset themselves; 1.3+ relies on the system header
// being present. Order is brittle -- don't re-sort these includes.
//
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <vulkan/vulkan.h>

#if defined(_WIN32)
  #define VK_USE_PLATFORM_WIN32_KHR 1
  #include <vulkan/vulkan_win32.h>
#elif defined(__linux__) && !defined(__ANDROID__)
  //
  // On Linux we support both X11 and Wayland surfaces. Which one
  // is actually used depends on what the platform layer hands us
  // as `native_window`. For the current X11 backend we only need
  // VK_KHR_xlib_surface; Wayland lands when the Wayland platform
  // does.
  //
  #define VK_USE_PLATFORM_XLIB_KHR 1
  #include <vulkan/vulkan_xlib.h>
#elif defined(__ANDROID__)
  #define VK_USE_PLATFORM_ANDROID_KHR 1
  #include <vulkan/vulkan_android.h>
#endif

//
// libshaderc runtime GLSL -> SPIR-V. Ships with the Vulkan SDK as
// shaderc_shared.{dll,so}. Lets us keep shader sources inline in
// C strings instead of maintaining parallel .spv blobs.
//
#include <shaderc/shaderc.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "types.h"
#include "gui.h"
#include "renderer.h"
#include "_visual_contract.h"
#include "../clib/memory_manager.h"
#include "../third_party/log.h"

//============================================================================
// constants
//============================================================================

//
// Rect buffer sizes are bumped vs the d3d12 defaults because
// persistent-mapped UPLOAD buffers on Vulkan (host-visible + coherent)
// have the same mid-frame-flush stomp hazard: once a vkCmdDraw has
// been recorded against a vertex offset, subsequent CPU writes to
// those offsets pollute the still-pending draw. Fix is the same
// append-only cursor pattern used by d3d12. Buffers are sized for a
// whole frame's worth of verts at worst case.
//
#define _VULKAN_RENDERER_INTERNAL__MAX_QUADS         8192
#define _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD    6
#define _VULKAN_RENDERER_INTERNAL__MAX_FRAMES        2
#define _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG 8
#define _VULKAN_RENDERER_INTERNAL__MAX_SCISSORS      16
//
// Text pipeline limits: up to 16384 glyph quads (98K verts, ~3.1 MB
// per frame-in-flight) and 64 distinct text runs (one per atlas
// binding change). Matches the d3d12 profile.
//
#define _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS    16384
#define _VULKAN_RENDERER_INTERNAL__MAX_TEXT_RUNS     64
//
// Max concurrent atlas / image textures. font.c creates an atlas per
// (family, size); widget_image caches image textures. 128 covers
// reasonable asset libraries.
//
#define _VULKAN_RENDERER_INTERNAL__MAX_ATLASES       128

//============================================================================
// vertex / state types
//============================================================================

typedef struct _vulkan_renderer_internal__vertex
{
    float x, y;
    float r, g, b, a;
    float lx, ly;
    float rect_w, rect_h;
    float radius;
} _vulkan_renderer_internal__vertex;

//
// Text + image vertex: pos + color + uv = 8 floats. Smaller than the
// rect vertex (no SDF parameters needed — text samples an atlas, image
// samples an RGBA texture). Same layout the d3d12 text/image pipelines
// use so the visual contract stays uniform.
//
typedef struct _vulkan_renderer_internal__tx_vertex
{
    float x, y;
    float r, g, b, a;
    float u, v;
} _vulkan_renderer_internal__tx_vertex;

//
// Atlas / image record. One per R8 font atlas or RGBA image texture.
// The void* handle passed back to callers (font.c, widget_image.c)
// is a pointer to this struct.
//
typedef struct _vulkan_renderer_internal__atlas_entry
{
    VkImage         image;
    VkDeviceMemory  memory;
    VkImageView     view;
    VkDescriptorSet desc_set;   // binds `image` at set=0, binding=0.
    int             width;
    int             height;
    boole           in_use;
} _vulkan_renderer_internal__atlas_entry;

//
// One run of text quads sharing an atlas. end_frame / flush_text binds
// the atlas's descriptor set once, then issues a single vkCmdDraw
// covering all the run's verts.
//
typedef struct _vulkan_renderer_internal__text_run
{
    _vulkan_renderer_internal__atlas_entry* atlas;
    int64                                   vert_start;
    int64                                   vert_count;
} _vulkan_renderer_internal__text_run;

typedef struct _vulkan_renderer_internal__scissor_entry
{
    VkRect2D rect;
    boole    active;
} _vulkan_renderer_internal__scissor_entry;

//============================================================================
// global state
//============================================================================
//
// Mirrors opengl3_renderer's _g layout: one struct to rule them
// all, flushed on shutdown.
//

typedef struct _vulkan_renderer_internal__state
{
    // native window pass-through
    void* native_window;

    // instance / device / queue
    VkInstance       instance;
    VkPhysicalDevice phys_device;
    VkDevice         device;
    uint32_t         queue_family;
    VkQueue          queue;

    // surface + swapchain
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkFormat         swap_format;
    VkExtent2D       swap_extent;
    uint32_t         swap_image_count;
    VkImage          swap_images  [_VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG];
    VkImageView      swap_views   [_VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG];
    VkFramebuffer    framebuffers [_VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG];

    // render pass
    VkRenderPass     render_pass;

    // per-frame command buffers + sync
    VkCommandPool    command_pool;
    VkCommandBuffer  cmd_bufs    [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkSemaphore      sem_acquire [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkSemaphore      sem_present [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkFence          in_flight   [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    uint32_t         frame_index;          // 0..MAX_FRAMES-1
    uint32_t         swap_image_index;     // result of vkAcquireNextImageKHR

    // rect pipeline
    VkDescriptorSetLayout rect_dsl;        // unused today (no textures) but reserved
    VkPipelineLayout      rect_pipeline_layout;
    VkPipeline            rect_pipeline;
    VkShaderModule        rect_vs;
    VkShaderModule        rect_fs;

    //
    // Rect vertex buffer (host-visible, persistently mapped). Append-
    // only within a frame; `rect_draw_cursor` advances on flush so
    // mid-frame scissor flushes don't stomp the still-pending draw
    // (same discipline as d3d12).
    //
    VkBuffer              rect_vbo   [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkDeviceMemory        rect_vbo_mem[_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    _vulkan_renderer_internal__vertex* rect_vbo_ptr[_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    int64                 rect_vert_count;
    int64                 rect_draw_cursor;

    //
    // Text + image shared plumbing. Lazy-built on first atlas / image
    // request so a PoC that doesn't use text doesn't pay the shader-
    // compile cost.
    //
    VkSampler                 tx_sampler;           // linear, clamp.
    VkDescriptorSetLayout     tx_dsl;               // 1 combined-image-sampler at set=0, binding=0.
    VkDescriptorPool          tx_descriptor_pool;   // pool for per-atlas descriptor sets.
    VkPipelineLayout          text_pipeline_layout;
    VkPipeline                text_pipeline;        // R8 sample + gamma reshape.
    VkPipelineLayout          image_pipeline_layout;
    VkPipeline                image_pipeline;       // RGBA sample + tint.
    VkShaderModule            tx_vs;                // shared VS.
    VkShaderModule            text_fs;
    VkShaderModule            image_fs;
    boole                     tx_built;             // set true after first successful build.

    //
    // Text per-frame VBO + runs + current atlas. Append-only like rects.
    //
    VkBuffer              text_vbo     [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkDeviceMemory        text_vbo_mem [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    _vulkan_renderer_internal__tx_vertex* text_vbo_ptr[_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    int64                     text_vert_count;
    _vulkan_renderer_internal__text_run text_runs[_VULKAN_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int                       text_run_count;
    int                       text_first_undrawn_run;
    _vulkan_renderer_internal__atlas_entry* current_text_atlas;

    //
    // Atlas / image pool. Fixed-size; allocated lazily in
    // create_atlas_r8 / create_texture_rgba and released in destroy.
    //
    _vulkan_renderer_internal__atlas_entry atlas_pool[_VULKAN_RENDERER_INTERNAL__MAX_ATLASES];

    // frame state
    int64                 viewport_w;
    int64                 viewport_h;
    gui_color             clear_color;
    boole                 frame_active;

    // scissor stack
    _vulkan_renderer_internal__scissor_entry scissors[_VULKAN_RENDERER_INTERNAL__MAX_SCISSORS];
    int64                 scissor_depth;
} _vulkan_renderer_internal__state;

static _vulkan_renderer_internal__state _vulkan_renderer_internal__g;

//============================================================================
// shader sources (GLSL 450)
//============================================================================
//
// Same math as every other backend. GLSL 450 maps directly to
// Vulkan-flavored GLSL (push constants, explicit binding numbers,
// gl_VertexIndex instead of gl_VertexID). The SDF formula and AA
// band are identical to gles3_renderer.c.
//

static const char* _VULKAN_RENDERER_INTERNAL__RECT_VS_GLSL =
    "#version 450\n"
    "layout(location = 0) in vec2  a_pos_px;\n"
    "layout(location = 1) in vec4  a_color;\n"
    "layout(location = 2) in vec2  a_local;\n"
    "layout(location = 3) in vec2  a_rect_size;\n"
    "layout(location = 4) in float a_radius;\n"
    "layout(push_constant) uniform PC { vec2 u_viewport; } pc;\n"
    "layout(location = 0) out vec4  v_color;\n"
    "layout(location = 1) out vec2  v_local;\n"
    "layout(location = 2) out vec2  v_rect_size;\n"
    "layout(location = 3) out float v_radius;\n"
    "void main() {\n"
    "    vec2 ndc = vec2(\n"
    "        (a_pos_px.x / pc.u_viewport.x) * 2.0 - 1.0,\n"
    "        (a_pos_px.y / pc.u_viewport.y) * 2.0 - 1.0\n"
    "    );\n"
    //
    // Note: Vulkan's clip space is y-DOWN (top-left origin at
    // [-1,-1]) unlike GL (y-up bottom-left). So we do NOT flip y
    // here, unlike the gles3 / opengl3 shaders. The pixel-to-NDC
    // math is a straight mapping.
    //
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color     = a_color;\n"
    "    v_local     = a_local;\n"
    "    v_rect_size = a_rect_size;\n"
    "    v_radius    = a_radius;\n"
    "}\n";

static const char* _VULKAN_RENDERER_INTERNAL__RECT_FS_GLSL =
    "#version 450\n"
    "layout(location = 0) in  vec4  v_color;\n"
    "layout(location = 1) in  vec2  v_local;\n"
    "layout(location = 2) in  vec2  v_rect_size;\n"
    "layout(location = 3) in  float v_radius;\n"
    "layout(location = 0) out vec4  o_color;\n"
    RENDERER_SDF_ROUND_BOX_GLSL
    "void main() {\n"
    "    vec2 half_size = v_rect_size * 0.5;\n"
    "    vec2 p = v_local - half_size;\n"
    "    float d = sd_round_box(p, half_size, v_radius);\n"
    "    float aa = 1.0 - smoothstep(" RENDERER_SDF_AA_MIN ", " RENDERER_SDF_AA_MAX ", d);\n"
    "    if (aa <= 0.0) discard;\n"
    "    o_color = vec4(v_color.rgb, v_color.a * aa);\n"
    "}\n";

//
// Text + image shared VS. Takes pos + color + uv, emits NDC + passthrough.
// Same shape as the rect VS but no SDF attributes.
//
static const char* _VULKAN_RENDERER_INTERNAL__TX_VS_GLSL =
    "#version 450\n"
    "layout(location = 0) in  vec2 a_pos_px;\n"
    "layout(location = 1) in  vec4 a_color;\n"
    "layout(location = 2) in  vec2 a_uv;\n"
    "layout(push_constant) uniform PC { vec2 u_viewport; } pc;\n"
    "layout(location = 0) out vec4 v_color;\n"
    "layout(location = 1) out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 ndc = vec2(\n"
    "        (a_pos_px.x / pc.u_viewport.x) * 2.0 - 1.0,\n"
    "        (a_pos_px.y / pc.u_viewport.y) * 2.0 - 1.0\n"
    "    );\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color     = a_color;\n"
    "    v_uv        = a_uv;\n"
    "}\n";

//
// Text FS: sample single-channel atlas, gamma-reshape, alpha-multiply
// with vertex color. Matches the d3d12 / d3d11 / opengl3 text shaders.
//
static const char* _VULKAN_RENDERER_INTERNAL__TEXT_FS_GLSL =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_atlas;\n"
    "layout(location = 0) in  vec4 v_color;\n"
    "layout(location = 1) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "    float a = texture(u_atlas, v_uv).r;\n"
    "    if (a <= 0.0) discard;\n"
    //
    // Gamma reshape: swap chain is UNORM (non-sRGB), blending happens
    // in linear-encoded space, so partial-coverage text edges look
    // too thin without this correction. pow(a, 1/2.2) biases the
    // coverage curve so the half-transparent AA band lands at the
    // perceptual midpoint. Same rationale as opengl3 / d3d11.
    //
    "    a = pow(a, 1.0 / 2.2);\n"
    "    o_color = vec4(v_color.rgb, v_color.a * a);\n"
    "}\n";

//
// Image FS: sample RGBA texture and multiply by vertex tint.
//
static const char* _VULKAN_RENDERER_INTERNAL__IMAGE_FS_GLSL =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_tex;\n"
    "layout(location = 0) in  vec4 v_color;\n"
    "layout(location = 1) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "    vec4 s = texture(u_tex, v_uv);\n"
    "    o_color = s * v_color;\n"
    "}\n";

//============================================================================
// forward decls
//============================================================================

static boole _vulkan_renderer_internal__create_instance(void);
static boole _vulkan_renderer_internal__create_surface(void* native_window);
static boole _vulkan_renderer_internal__pick_physical_device(void);
static boole _vulkan_renderer_internal__create_device(void);
static boole _vulkan_renderer_internal__create_swapchain(void);
static void  _vulkan_renderer_internal__destroy_swapchain(void);
static boole _vulkan_renderer_internal__create_render_pass(void);
static boole _vulkan_renderer_internal__create_framebuffers(void);
static boole _vulkan_renderer_internal__create_command_pool_and_buffers(void);
static boole _vulkan_renderer_internal__create_sync_primitives(void);
static boole _vulkan_renderer_internal__compile_shader(shaderc_shader_kind kind, const char* src, const char* tag, VkShaderModule* out);
static boole _vulkan_renderer_internal__create_rect_pipeline(void);
static boole _vulkan_renderer_internal__create_rect_vertex_buffers(void);
static uint32_t _vulkan_renderer_internal__find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
static void  _vulkan_renderer_internal__flush_rects(void);
//
// text + image pipeline setup (lazy, on first atlas or texture request):
//
static boole _vulkan_renderer_internal__ensure_tx_plumbing(void);
static boole _vulkan_renderer_internal__create_tx_sampler(void);
static boole _vulkan_renderer_internal__create_tx_descriptor_layout(void);
static boole _vulkan_renderer_internal__create_tx_descriptor_pool(void);
static boole _vulkan_renderer_internal__create_text_pipeline(void);
static boole _vulkan_renderer_internal__create_image_pipeline(void);
static boole _vulkan_renderer_internal__create_tx_vertex_buffers(void);
//
// atlas pool + upload:
//
static _vulkan_renderer_internal__atlas_entry* _vulkan_renderer_internal__alloc_atlas_slot(void);
static void  _vulkan_renderer_internal__release_atlas(_vulkan_renderer_internal__atlas_entry* a);
static boole _vulkan_renderer_internal__upload_atlas(_vulkan_renderer_internal__atlas_entry* a, const ubyte* pixels, int w, int h, VkFormat format, int bytes_per_pixel);
static void  _vulkan_renderer_internal__flush_text(void);

//============================================================================
// PUBLIC: renderer__init
//============================================================================

boole renderer__init(void* native_window)
{
    memset(&_vulkan_renderer_internal__g, 0, sizeof(_vulkan_renderer_internal__g));
    _vulkan_renderer_internal__g.native_window = native_window;

    if (!_vulkan_renderer_internal__create_instance())          { return FALSE; }
    if (!_vulkan_renderer_internal__create_surface(native_window)) { return FALSE; }
    if (!_vulkan_renderer_internal__pick_physical_device())     { return FALSE; }
    if (!_vulkan_renderer_internal__create_device())            { return FALSE; }
    if (!_vulkan_renderer_internal__create_swapchain())         { return FALSE; }
    if (!_vulkan_renderer_internal__create_render_pass())       { return FALSE; }
    if (!_vulkan_renderer_internal__create_framebuffers())      { return FALSE; }
    if (!_vulkan_renderer_internal__create_command_pool_and_buffers()) { return FALSE; }
    if (!_vulkan_renderer_internal__create_sync_primitives())   { return FALSE; }
    if (!_vulkan_renderer_internal__create_rect_pipeline())     { return FALSE; }
    if (!_vulkan_renderer_internal__create_rect_vertex_buffers()) { return FALSE; }

    log_info("vulkan_renderer: up (%u swapchain images, %ux%u, format=%d)",
             (unsigned)_vulkan_renderer_internal__g.swap_image_count,
             (unsigned)_vulkan_renderer_internal__g.swap_extent.width,
             (unsigned)_vulkan_renderer_internal__g.swap_extent.height,
             (int)_vulkan_renderer_internal__g.swap_format);
    return TRUE;
}

//============================================================================
// PUBLIC: renderer__shutdown
//============================================================================

void renderer__shutdown(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    if (s->device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(s->device);
    }

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (s->rect_vbo_ptr[i] != NULL)
        {
            vkUnmapMemory(s->device, s->rect_vbo_mem[i]);
            s->rect_vbo_ptr[i] = NULL;
        }
        if (s->rect_vbo[i]     != VK_NULL_HANDLE) { vkDestroyBuffer(s->device, s->rect_vbo[i], NULL); }
        if (s->rect_vbo_mem[i] != VK_NULL_HANDLE) { vkFreeMemory(s->device, s->rect_vbo_mem[i], NULL); }
    }

    //
    // Text / image plumbing. Atlas pool entries own a VkImage + memory
    // + view + descriptor set; walk in_use==TRUE slots and free each.
    // Descriptor sets come from tx_descriptor_pool which is destroyed
    // right after, so the set handles don't need explicit vkFree --
    // they die with the pool. View + image + memory must be explicit.
    //
    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_ATLASES; i++)
    {
        _vulkan_renderer_internal__atlas_entry* a = &s->atlas_pool[i];
        if (!a->in_use) { continue; }
        if (a->view   != VK_NULL_HANDLE) { vkDestroyImageView(s->device, a->view,   NULL); }
        if (a->image  != VK_NULL_HANDLE) { vkDestroyImage    (s->device, a->image,  NULL); }
        if (a->memory != VK_NULL_HANDLE) { vkFreeMemory      (s->device, a->memory, NULL); }
    }

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (s->text_vbo_ptr[i] != NULL)
        {
            vkUnmapMemory(s->device, s->text_vbo_mem[i]);
            s->text_vbo_ptr[i] = NULL;
        }
        if (s->text_vbo[i]     != VK_NULL_HANDLE) { vkDestroyBuffer(s->device, s->text_vbo[i], NULL); }
        if (s->text_vbo_mem[i] != VK_NULL_HANDLE) { vkFreeMemory(s->device, s->text_vbo_mem[i], NULL); }
    }

    if (s->text_pipeline         != VK_NULL_HANDLE) { vkDestroyPipeline(s->device, s->text_pipeline, NULL); }
    if (s->text_pipeline_layout  != VK_NULL_HANDLE) { vkDestroyPipelineLayout(s->device, s->text_pipeline_layout, NULL); }
    if (s->image_pipeline        != VK_NULL_HANDLE) { vkDestroyPipeline(s->device, s->image_pipeline, NULL); }
    if (s->image_pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(s->device, s->image_pipeline_layout, NULL); }
    if (s->tx_descriptor_pool    != VK_NULL_HANDLE) { vkDestroyDescriptorPool(s->device, s->tx_descriptor_pool, NULL); }
    if (s->tx_dsl                != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(s->device, s->tx_dsl, NULL); }
    if (s->tx_sampler            != VK_NULL_HANDLE) { vkDestroySampler(s->device, s->tx_sampler, NULL); }
    if (s->tx_vs                 != VK_NULL_HANDLE) { vkDestroyShaderModule(s->device, s->tx_vs, NULL); }
    if (s->text_fs               != VK_NULL_HANDLE) { vkDestroyShaderModule(s->device, s->text_fs, NULL); }
    if (s->image_fs              != VK_NULL_HANDLE) { vkDestroyShaderModule(s->device, s->image_fs, NULL); }

    if (s->rect_pipeline         != VK_NULL_HANDLE) { vkDestroyPipeline(s->device, s->rect_pipeline, NULL); }
    if (s->rect_pipeline_layout  != VK_NULL_HANDLE) { vkDestroyPipelineLayout(s->device, s->rect_pipeline_layout, NULL); }
    if (s->rect_dsl              != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(s->device, s->rect_dsl, NULL); }
    if (s->rect_vs               != VK_NULL_HANDLE) { vkDestroyShaderModule(s->device, s->rect_vs, NULL); }
    if (s->rect_fs               != VK_NULL_HANDLE) { vkDestroyShaderModule(s->device, s->rect_fs, NULL); }

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (s->sem_acquire[i] != VK_NULL_HANDLE) { vkDestroySemaphore(s->device, s->sem_acquire[i], NULL); }
        if (s->sem_present[i] != VK_NULL_HANDLE) { vkDestroySemaphore(s->device, s->sem_present[i], NULL); }
        if (s->in_flight[i]   != VK_NULL_HANDLE) { vkDestroyFence(s->device, s->in_flight[i], NULL); }
    }

    if (s->command_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(s->device, s->command_pool, NULL); }

    _vulkan_renderer_internal__destroy_swapchain();

    if (s->render_pass != VK_NULL_HANDLE) { vkDestroyRenderPass(s->device, s->render_pass, NULL); }

    if (s->device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(s->device, NULL);
        s->device = VK_NULL_HANDLE;
    }

    if (s->surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(s->instance, s->surface, NULL);
        s->surface = VK_NULL_HANDLE;
    }

    if (s->instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(s->instance, NULL);
        s->instance = VK_NULL_HANDLE;
    }

    memset(s, 0, sizeof(*s));
}

//============================================================================
// PUBLIC: begin_frame / submit_rect / end_frame
//============================================================================

void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color clear)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    s->viewport_w             = viewport_w;
    s->viewport_h             = viewport_h;
    s->clear_color            = clear;
    s->rect_vert_count        = 0;
    s->rect_draw_cursor       = 0;
    s->text_vert_count        = 0;
    s->text_run_count         = 0;
    s->text_first_undrawn_run = 0;

    //
    // Wait for previous frame using this slot to finish.
    //
    vkWaitForFences(s->device, 1, &s->in_flight[s->frame_index], VK_TRUE, UINT64_MAX);

    //
    // Acquire the next swapchain image.
    //
    VkResult ar = vkAcquireNextImageKHR(
        s->device, s->swapchain, UINT64_MAX,
        s->sem_acquire[s->frame_index], VK_NULL_HANDLE,
        &s->swap_image_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR)
    {
        //
        // Swapchain got invalidated (window resized, display
        // reconnected). Recreating it mid-begin_frame would mean
        // rebuilding framebuffers + pipelines too; for this
        // scaffold we skip the frame and let the next tick try
        // again. Host platform typically re-layouts on resize so
        // the stale image isn't visible.
        //
        log_warn("vkAcquireNextImageKHR: OUT_OF_DATE; skipping frame (swapchain recreate not yet wired)");
        s->frame_active = FALSE;
        return;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
    {
        log_error("vkAcquireNextImageKHR failed: %d", (int)ar);
        s->frame_active = FALSE;
        return;
    }

    vkResetFences(s->device, 1, &s->in_flight[s->frame_index]);

    //
    // Record command buffer for this frame. vkResetCommandBuffer
    // keeps the pool happy (command buffers are transient and
    // re-recorded every frame).
    //
    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    //
    // One render pass wraps the entire frame's draws. Clear op
    // is baked into the first attachment load -- same semantic
    // as glClear / D3D11 ClearRenderTargetView. Convert gui_color
    // (linear 0..1) directly -- no sRGB conversion; we render to
    // a UNORM (non-sRGB) swapchain.
    //
    VkClearValue clear_val;
    clear_val.color.float32[0] = clear.r;
    clear_val.color.float32[1] = clear.g;
    clear_val.color.float32[2] = clear.b;
    clear_val.color.float32[3] = clear.a;

    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass             = s->render_pass;
    rpbi.framebuffer            = s->framebuffers[s->swap_image_index];
    rpbi.renderArea.offset.x    = 0;
    rpbi.renderArea.offset.y    = 0;
    rpbi.renderArea.extent      = s->swap_extent;
    rpbi.clearValueCount        = 1;
    rpbi.pClearValues           = &clear_val;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    //
    // Dynamic viewport + scissor (pipeline is created with these
    // states as DYNAMIC so we can update per-frame without
    // rebuilding the PSO on resize).
    //
    VkViewport vp = { 0 };
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = (float)s->swap_extent.width;
    vp.height   = (float)s->swap_extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = { { 0, 0 }, s->swap_extent };
    vkCmdSetScissor(cb, 0, 1, &sc);

    s->frame_active = TRUE;
}

//
// Push one vertex into the rect VBO. Factored out so submit_rect and
// submit_rect_gradient share it; the only difference is whether all
// six corners share the same color or four distinct ones.
//
static void _vulkan_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color c)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    _vulkan_renderer_internal__vertex* v = &s->rect_vbo_ptr[s->frame_index][s->rect_vert_count];
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
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (!s->frame_active) { return; }
    //
    // Buffer-full drop: the append-only cursor means a mid-frame
    // flush can't free space. MAX_QUADS (8192) is sized for a whole
    // frame; if a real UI hits the cap, bump the constant.
    //
    if (s->rect_vert_count + _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _VULKAN_RENDERER_INTERNAL__MAX_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    float half_min = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius > half_min) { radius = half_min; }
    if (radius < 0.0f)     { radius = 0.0f; }

    float x0 = r.x,         y0 = r.y;
    float x1 = r.x + r.w,   y1 = r.y + r.h;

    //
    // Two triangles: (tl, tr, bl) + (tr, br, bl). Matches every other
    // backend's winding per the VISUAL CONTRACT.
    //
    _vulkan_renderer_internal__push_vertex(x0, y0, 0.0f, 0.0f, r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x1, y1, r.w,  r.h,  r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
}

static void _vulkan_renderer_internal__flush_rects(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (s->rect_vert_count <= 0) { return; }
    if (!s->frame_active)        { return; }

    int64 pending = s->rect_vert_count - s->rect_draw_cursor;
    if (pending <= 0) { return; }

    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->rect_pipeline);

    VkBuffer     vbs[1] = { s->rect_vbo[s->frame_index] };
    VkDeviceSize ofs[1] = { 0 };
    vkCmdBindVertexBuffers(cb, 0, 1, vbs, ofs);

    //
    // Push constants deliver the viewport (2 floats) to the VS
    // for NDC conversion. Cheaper than a UBO for 8 bytes.
    //
    float viewport[2];
    viewport[0] = (float)s->viewport_w;
    viewport[1] = (float)s->viewport_h;
    vkCmdPushConstants(cb, s->rect_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewport),
                       viewport);

    //
    // Draw the append-only slice [rect_draw_cursor .. rect_vert_count).
    // DO NOT reset rect_vert_count here -- the command buffer still
    // references these verts by offset, and subsequent CPU writes
    // must go to HIGHER offsets to avoid stomping the pending draw.
    // Same hazard the d3d12 backend hit earlier; same fix.
    //
    vkCmdDraw(cb, (uint32_t)pending, 1, (uint32_t)s->rect_draw_cursor, 0);

    s->rect_draw_cursor = s->rect_vert_count;
}

void renderer__flush_pending_draws(void)
{
    // Public flush so scene_render can break the per-frame batch at
    // z-index sibling boundaries. See renderer.h for the full reason.
    // Rects first, text second -- same ordering as end_frame.
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (!s->frame_active) { return; }
    _vulkan_renderer_internal__flush_rects();
    _vulkan_renderer_internal__flush_text();
}

void renderer__end_frame(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (!s->frame_active)
    {
        //
        // begin_frame bailed out (acquire failure). Advance frame
        // index so the next tick uses the other slot.
        //
        s->frame_index = (s->frame_index + 1) % _VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
        return;
    }

    //
    // Flush rects first, text on top. Same ordering every backend
    // uses so text always draws OVER rect bg (matches submit-order
    // when the widget tree emits "bg rect, then text label").
    //
    _vulkan_renderer_internal__flush_rects();
    _vulkan_renderer_internal__flush_text();

    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    //
    // Submit: wait for the image to be acquired, signal the
    // "render done" semaphore when all color writes land.
    //
    VkPipelineStageFlags wait_stages[1] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &s->sem_acquire[s->frame_index];
    si.pWaitDstStageMask    = wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &s->sem_present[s->frame_index];
    vkQueueSubmit(s->queue, 1, &si, s->in_flight[s->frame_index]);

    //
    // Present after submission. FIFO present mode pins this to
    // vsync, same timing pacing as other backends.
    //
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &s->sem_present[s->frame_index];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &s->swapchain;
    pi.pImageIndices      = &s->swap_image_index;
    VkResult pr = vkQueuePresentKHR(s->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
    {
        //
        // Swapchain became invalid during / after the present.
        // Scaffold-level handling: log and continue; the next
        // begin_frame's acquire will also fail and skip a frame
        // until host platform rebuilds the swapchain. A full
        // impl would recreate here.
        //
        log_warn("vkQueuePresentKHR: OUT_OF_DATE / SUBOPTIMAL; swapchain rebuild pending");
    }
    else if (pr != VK_SUCCESS)
    {
        log_error("vkQueuePresentKHR failed: %d", (int)pr);
    }

    s->frame_active = FALSE;
    s->frame_index  = (s->frame_index + 1) % _VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
}

//============================================================================
// text / image / effects -- STUBS
//============================================================================
//
// Matches the d3d11 + d3d9 scaffold pattern: return 0 / NULL so
// Text pipeline now live. Image pipeline still a fallback (image
// uses the same DSL + sampler + VBO infrastructure; only the FS and
// the submit path differ -- mechanical extension on top of the text
// work below).
//

void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (pixels == NULL || width <= 0 || height <= 0) { return NULL; }
    //
    // Lazy-build the shared text plumbing (sampler, DSL, descriptor
    // pool, pipeline, VBO) on the first atlas request. That way a
    // PoC that never submits text doesn't pay for it.
    //
    if (!s->tx_built)
    {
        if (!_vulkan_renderer_internal__ensure_tx_plumbing()) { return NULL; }
        s->tx_built = TRUE;
    }
    _vulkan_renderer_internal__atlas_entry* a = _vulkan_renderer_internal__alloc_atlas_slot();
    if (a == NULL) { log_error("vulkan atlas: pool full"); return NULL; }
    if (!_vulkan_renderer_internal__upload_atlas(a, pixels, width, height, VK_FORMAT_R8_UNORM, 1))
    {
        _vulkan_renderer_internal__release_atlas(a);
        return NULL;
    }
    return (void*)a;
}

void renderer__destroy_atlas(void* atlas)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (atlas == NULL) { return; }
    _vulkan_renderer_internal__atlas_entry* a = (_vulkan_renderer_internal__atlas_entry*)atlas;
    //
    // Wait for all outstanding frames to finish rendering so we're
    // not freeing a VkImage the GPU is still reading from. Atlas
    // destruction happens at font__shutdown (process exit) so the
    // block is fine.
    //
    vkDeviceWaitIdle(s->device);
    if (s->current_text_atlas == a) { s->current_text_atlas = NULL; }
    _vulkan_renderer_internal__release_atlas(a);
}

void renderer__set_text_atlas(void* atlas)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    s->current_text_atlas = (_vulkan_renderer_internal__atlas_entry*)atlas;
}

void renderer__submit_text_glyph(gui_rect rect, gui_rect uv, gui_color color)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (!s->frame_active)              { return; }
    if (s->current_text_atlas == NULL) { return; }
    if (s->text_vert_count + _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    //
    // Open or extend the last run. Identical rule to d3d12: only
    // extend the tail if that run has NOT been flushed yet -- mid-
    // frame flushes (push_scissor) advance first_undrawn_run past
    // already-drawn runs, and extending one of those would slot new
    // glyphs into a vert range nothing re-draws.
    //
    _vulkan_renderer_internal__text_run* run = NULL;
    if (s->text_run_count > s->text_first_undrawn_run)
    {
        run = &s->text_runs[s->text_run_count - 1];
        if (run->atlas != s->current_text_atlas) { run = NULL; }
    }
    if (run == NULL)
    {
        if (s->text_run_count >= _VULKAN_RENDERER_INTERNAL__MAX_TEXT_RUNS) { return; }
        run = &s->text_runs[s->text_run_count++];
        run->atlas      = s->current_text_atlas;
        run->vert_start = s->text_vert_count;
        run->vert_count = 0;
    }

    float x0 = rect.x, y0 = rect.y;
    float x1 = rect.x + rect.w, y1 = rect.y + rect.h;
    float u0 = uv.x, v0 = uv.y;
    float u1 = uv.x + uv.w, v1 = uv.y + uv.h;

    _vulkan_renderer_internal__tx_vertex* v = &s->text_vbo_ptr[s->frame_index][s->text_vert_count];
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

    s->text_vert_count += _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;
    run->vert_count    += _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

void* renderer__create_texture_rgba(const ubyte* rgba, int w, int h)
{
    (void)rgba; (void)w; (void)h;
    //
    // Image pipeline port is a mechanical follow-up on top of the text
    // pipeline: same DSL + descriptor pool + VBO shape, different FS
    // (PSImage instead of PSText). Returning NULL here makes scene's
    // fitted-image path fall back to the placeholder gradient so the
    // layout still reserves the slot visually.
    //
    return NULL;
}
void renderer__destroy_texture(void* tex)                              { (void)tex; }
void renderer__submit_image(gui_rect r, void* tex, gui_color tint)
{
    (void)tex;
    //
    // Fall back to a tinted solid-color rect so image placeholders
    // are at least visible on this backend.
    //
    renderer__submit_rect(r, tint, 0.0f);
}

//
// Shadow: concentric layered rects with quadratic falloff. The rect
// pipeline's per-fragment SDF AA produces the soft edge; no separate
// shader needed. Same algorithm as d3d11 / d3d12 / opengl3 / gles3.
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
// Gradient: per-corner color over the existing SDF-rect pipeline.
// The fragment shader interpolates `col` across the triangle, so
// two-color lerps cost nothing extra -- just different corner colors
// fed to push_vertex. Direction maps onto corners the same way every
// other backend does it (see the comment in d3d11 / opengl3).
//
void renderer__submit_rect_gradient(gui_rect r, gui_color from, gui_color to, int direction, float radius)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (!s->frame_active) { return; }
    //
    // Same buffer-full drop rule as submit_rect; see comment there.
    //
    if (s->rect_vert_count + _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD >
        _VULKAN_RENDERER_INTERNAL__MAX_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD)
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

    _vulkan_renderer_internal__push_vertex(x0, y0, 0.0f, 0.0f, r.w, r.h, radius, c_tl);
    _vulkan_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c_tr);
    _vulkan_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c_bl);
    _vulkan_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c_tr);
    _vulkan_renderer_internal__push_vertex(x1, y1, r.w,  r.h,  r.w, r.h, radius, c_br);
    _vulkan_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c_bl);
}

//============================================================================
// scissor
//============================================================================
//
// Intersection stack + flush-on-change, same pattern as every
// other backend. Vulkan's vkCmdSetScissor is dynamic state on our
// pipeline, so pushing a new rect is cheap (no PSO rebuild).
//

void renderer__push_scissor(gui_rect r)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (s->scissor_depth >= _VULKAN_RENDERER_INTERNAL__MAX_SCISSORS) { return; }

    //
    // Flush BOTH rects and text before the scissor change so queued
    // draws land under the OLD clip. Text runs are only defined once
    // tx plumbing exists (lazy-built on first atlas upload); guard
    // accordingly.
    //
    _vulkan_renderer_internal__flush_rects();
    if (s->tx_built && s->text_run_count > s->text_first_undrawn_run)
    {
        _vulkan_renderer_internal__flush_text();
    }

    //
    // Clamp input rect to [0, viewport] and intersect with the
    // currently effective scissor (if any) to produce the new
    // effective rect.
    //
    int32_t x0 = (int32_t)r.x;
    int32_t y0 = (int32_t)r.y;
    int32_t x1 = (int32_t)(r.x + r.w);
    int32_t y1 = (int32_t)(r.y + r.h);
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (int32_t)s->swap_extent.width)  { x1 = (int32_t)s->swap_extent.width; }
    if (y1 > (int32_t)s->swap_extent.height) { y1 = (int32_t)s->swap_extent.height; }

    if (s->scissor_depth > 0)
    {
        VkRect2D prev = s->scissors[s->scissor_depth - 1].rect;
        int32_t px0 = prev.offset.x;
        int32_t py0 = prev.offset.y;
        int32_t px1 = prev.offset.x + (int32_t)prev.extent.width;
        int32_t py1 = prev.offset.y + (int32_t)prev.extent.height;
        if (x0 < px0) { x0 = px0; }
        if (y0 < py0) { y0 = py0; }
        if (x1 > px1) { x1 = px1; }
        if (y1 > py1) { y1 = py1; }
    }
    if (x1 < x0) { x1 = x0; }
    if (y1 < y0) { y1 = y0; }

    VkRect2D sc;
    sc.offset.x      = x0;
    sc.offset.y      = y0;
    sc.extent.width  = (uint32_t)(x1 - x0);
    sc.extent.height = (uint32_t)(y1 - y0);

    s->scissors[s->scissor_depth].rect   = sc;
    s->scissors[s->scissor_depth].active = TRUE;
    s->scissor_depth++;

    if (s->frame_active)
    {
        vkCmdSetScissor(s->cmd_bufs[s->frame_index], 0, 1, &sc);
    }
}

void renderer__pop_scissor(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (s->scissor_depth <= 0) { return; }

    //
    // Same dual-flush as push_scissor: queued draws land under the
    // inner (tighter) clip before we pop back out.
    //
    _vulkan_renderer_internal__flush_rects();
    if (s->tx_built && s->text_run_count > s->text_first_undrawn_run)
    {
        _vulkan_renderer_internal__flush_text();
    }

    s->scissor_depth--;

    if (!s->frame_active) { return; }

    VkRect2D sc;
    if (s->scissor_depth > 0)
    {
        sc = s->scissors[s->scissor_depth - 1].rect;
    }
    else
    {
        sc.offset.x      = 0;
        sc.offset.y      = 0;
        sc.extent.width  = s->swap_extent.width;
        sc.extent.height = s->swap_extent.height;
    }
    vkCmdSetScissor(s->cmd_bufs[s->frame_index], 0, 1, &sc);
}

void renderer__blur_region(gui_rect rect, float sigma_px)
{
    //
    // Placeholder: Vulkan doesn't yet wire up the capture + separable
    // Gaussian path for true backdrop blur. Translucent darken splat
    // as a stand-in so blur-styled regions at least visually differ
    // from the unblurred bg; the shape scene.c emitted before this
    // API existed.
    //
    (void)sigma_px;
    gui_color dim = { 0.0f, 0.0f, 0.0f, 0.15f };
    renderer__submit_rect(rect, dim, 0.0f);
}

//============================================================================
// IMPL: init helpers
//============================================================================
//
// From here down is straightforward Vulkan boilerplate: instance,
// surface, device pick, swapchain, render pass, framebuffers,
// command buffers, sync primitives, pipeline, vertex buffer.
// Every step is the minimum viable -- no validation layers enabled
// by default, no MSAA, no depth buffer. The shipped shape is
// "simplest possible Vulkan that renders one triangle" extended
// with double-buffered frame-in-flight + a real VBO.
//

static boole _vulkan_renderer_internal__create_instance(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName   = "gui";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "novyworkbench-gui";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_1;

    //
    // Minimal extension list: VK_KHR_surface + one platform-
    // specific surface extension. Validation layers intentionally
    // omitted; add -DGUI_VULKAN_VALIDATION to flip them on in a
    // future pass.
    //
    const char* exts[3] = { 0 };
    uint32_t    n_ext   = 0;

    exts[n_ext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    exts[n_ext++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    exts[n_ext++] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    exts[n_ext++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif

    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = n_ext;
    ici.ppEnabledExtensionNames = exts;

    VkResult r = vkCreateInstance(&ici, NULL, &s->instance);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateInstance failed: %d", (int)r);
        return FALSE;
    }
    return TRUE;
}

//
// Cross-platform surface dispatch table for native_window. The
// platform layer packs whichever native handle pair the active
// VK_KHR_*_surface extension expects:
//
//   Win32:    native_window = HWND. HINSTANCE comes from GetModuleHandleW.
//   X11:      native_window points at this struct so we get both
//             (Display*, Window) atomically; the platform_linux_x11
//             backend builds it on init and hands the address to
//             renderer__init.
//   Android:  native_window = ANativeWindow*.
//
// Other backends (DRM, Wayland, macOS) don't go through Vulkan
// today; if/when they do they add a branch here.
//
typedef struct vulkan_native_window_x11
{
    void*        display;   // Display* cast to void* to avoid the X header in the renderer
    unsigned long window;   // Window (XID, ulong on every libX11 build we target)
} vulkan_native_window_x11;

static boole _vulkan_renderer_internal__create_surface(void* native_window)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    if (native_window == NULL)
    {
        log_error("vulkan_renderer: native_window is NULL");
        return FALSE;
    }

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(NULL);
    sci.hwnd      = (HWND)native_window;
    VkResult r = vkCreateWin32SurfaceKHR(s->instance, &sci, NULL, &s->surface);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateWin32SurfaceKHR failed: %d", (int)r);
        return FALSE;
    }
    return TRUE;

#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    //
    // X11 platform packs (Display*, Window) into a small struct
    // because vkCreateXlibSurfaceKHR needs both. The Linux X11
    // platform layer constructs one on init and passes its address
    // through renderer__init's `native_window` parameter.
    //
    vulkan_native_window_x11* nw = (vulkan_native_window_x11*)native_window;
    VkXlibSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    sci.dpy    = (Display*)nw->display;
    sci.window = (Window)nw->window;
    VkResult r = vkCreateXlibSurfaceKHR(s->instance, &sci, NULL, &s->surface);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateXlibSurfaceKHR failed: %d", (int)r);
        return FALSE;
    }
    return TRUE;

#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    //
    // Android passes ANativeWindow* directly. The Android platform
    // layer's APP_CMD_INIT_WINDOW handler hands us app->window.
    //
    VkAndroidSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    sci.window = (ANativeWindow*)native_window;
    VkResult r = vkCreateAndroidSurfaceKHR(s->instance, &sci, NULL, &s->surface);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateAndroidSurfaceKHR failed: %d", (int)r);
        return FALSE;
    }
    return TRUE;

#else
    (void)native_window;
    log_error("vulkan_renderer: no surface extension compiled in for this platform");
    return FALSE;
#endif
}

static boole _vulkan_renderer_internal__pick_physical_device(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s->instance, &count, NULL);
    if (count == 0)
    {
        log_error("no VkPhysicalDevices available");
        return FALSE;
    }
    if (count > 8) { count = 8; }
    VkPhysicalDevice devs[8];
    vkEnumeratePhysicalDevices(s->instance, &count, devs);

    //
    // Walk the list, pick the first device that has a queue family
    // which supports both graphics AND present-to-our-surface.
    // Integrated GPU vs discrete preference is intentionally left
    // out for this scaffold.
    //
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        if (qn == 0) { continue; }
        if (qn > 16) { qn = 16; }
        VkQueueFamilyProperties qfp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qfp);
        for (uint32_t q = 0; q < qn; q++)
        {
            if ((qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) { continue; }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, s->surface, &present);
            if (present == VK_FALSE) { continue; }
            s->phys_device  = devs[i];
            s->queue_family = q;
            return TRUE;
        }
    }
    log_error("no VkPhysicalDevice with graphics+present queue on this surface");
    return FALSE;
}

static boole _vulkan_renderer_internal__create_device(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s->queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    VkResult r = vkCreateDevice(s->phys_device, &dci, NULL, &s->device);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateDevice failed: %d", (int)r);
        return FALSE;
    }
    vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);
    return TRUE;
}

static boole _vulkan_renderer_internal__create_swapchain(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->phys_device, s->surface, &caps);

    //
    // Pick an 8-bit UNORM BGRA/RGBA format. If the surface's
    // preferred is UNDEFINED, BGRA8 UNORM is the safe default.
    //
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys_device, s->surface, &fn, NULL);
    if (fn == 0)
    {
        log_error("no surface formats");
        return FALSE;
    }
    if (fn > 16) { fn = 16; }
    VkSurfaceFormatKHR formats[16];
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys_device, s->surface, &fn, formats);

    VkSurfaceFormatKHR pick = formats[0];
    for (uint32_t i = 0; i < fn; i++)
    {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
             formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            pick = formats[i];
            break;
        }
    }

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) { want = caps.maxImageCount; }
    if (want > _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG)
    {
        want = _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == (uint32_t)-1)
    {
        //
        // Surface is "size-follows-swapchain" (rare). Pick a
        // reasonable default; the host will soon set a real size
        // via the platform resize path.
        //
        extent.width  = 800;
        extent.height = 600;
    }

    VkSwapchainCreateInfoKHR scci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    scci.surface          = s->surface;
    scci.minImageCount    = want;
    scci.imageFormat      = pick.format;
    scci.imageColorSpace  = pick.colorSpace;
    scci.imageExtent      = extent;
    scci.imageArrayLayers = 1;
    scci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scci.preTransform     = caps.currentTransform;
    scci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;        // vsync
    scci.clipped          = VK_TRUE;

    VkResult r = vkCreateSwapchainKHR(s->device, &scci, NULL, &s->swapchain);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateSwapchainKHR failed: %d", (int)r);
        return FALSE;
    }
    s->swap_format = pick.format;
    s->swap_extent = extent;

    //
    // Retrieve image handles + create views for each.
    //
    uint32_t got = 0;
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &got, NULL);
    if (got > _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG)
    {
        got = _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG;
    }
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &got, s->swap_images);
    s->swap_image_count = got;

    for (uint32_t i = 0; i < got; i++)
    {
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image    = s->swap_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format   = s->swap_format;
        ivci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel   = 0;
        ivci.subresourceRange.levelCount     = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount     = 1;
        VkResult rr = vkCreateImageView(s->device, &ivci, NULL, &s->swap_views[i]);
        if (rr != VK_SUCCESS)
        {
            log_error("vkCreateImageView[%u] failed: %d", (unsigned)i, (int)rr);
            return FALSE;
        }
    }
    return TRUE;
}

static void _vulkan_renderer_internal__destroy_swapchain(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    for (uint32_t i = 0; i < s->swap_image_count; i++)
    {
        if (s->framebuffers[i] != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(s->device, s->framebuffers[i], NULL);
            s->framebuffers[i] = VK_NULL_HANDLE;
        }
        if (s->swap_views[i] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(s->device, s->swap_views[i], NULL);
            s->swap_views[i] = VK_NULL_HANDLE;
        }
    }
    if (s->swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        s->swapchain = VK_NULL_HANDLE;
    }
    s->swap_image_count = 0;
}

static boole _vulkan_renderer_internal__create_render_pass(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkAttachmentDescription attach = { 0 };
    attach.format         = s->swap_format;
    attach.samples        = VK_SAMPLE_COUNT_1_BIT;
    attach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attach.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref = { 0 };
    ref.attachment = 0;
    ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    VkSubpassDependency dep = { 0 };
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &attach;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;

    VkResult r = vkCreateRenderPass(s->device, &rpci, NULL, &s->render_pass);
    if (r != VK_SUCCESS)
    {
        log_error("vkCreateRenderPass failed: %d", (int)r);
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_framebuffers(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    for (uint32_t i = 0; i < s->swap_image_count; i++)
    {
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = s->render_pass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &s->swap_views[i];
        fci.width           = s->swap_extent.width;
        fci.height          = s->swap_extent.height;
        fci.layers          = 1;
        VkResult r = vkCreateFramebuffer(s->device, &fci, NULL, &s->framebuffers[i]);
        if (r != VK_SUCCESS)
        {
            log_error("vkCreateFramebuffer[%u] failed: %d", (unsigned)i, (int)r);
            return FALSE;
        }
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_command_pool_and_buffers(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = s->queue_family;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(s->device, &cpci, NULL, &s->command_pool) != VK_SUCCESS)
    {
        log_error("vkCreateCommandPool failed");
        return FALSE;
    }

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = s->command_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = _VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
    if (vkAllocateCommandBuffers(s->device, &cbai, s->cmd_bufs) != VK_SUCCESS)
    {
        log_error("vkAllocateCommandBuffers failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_sync_primitives(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;       // so the first frame doesn't wait on a never-signalled fence

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (vkCreateSemaphore(s->device, &sci, NULL, &s->sem_acquire[i]) != VK_SUCCESS ||
            vkCreateSemaphore(s->device, &sci, NULL, &s->sem_present[i]) != VK_SUCCESS ||
            vkCreateFence(s->device, &fci, NULL, &s->in_flight[i]) != VK_SUCCESS)
        {
            log_error("sync primitive create failed (frame %d)", i);
            return FALSE;
        }
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__compile_shader(shaderc_shader_kind kind, const char* src, const char* tag, VkShaderModule* out)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    shaderc_compiler_t        compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t opts     = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan,
                                            shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_performance);

    shaderc_compilation_result_t res = shaderc_compile_into_spv(
        compiler, src, strlen(src), kind, tag, "main", opts);

    shaderc_compilation_status status = shaderc_result_get_compilation_status(res);
    if (status != shaderc_compilation_status_success)
    {
        log_error("shaderc compile '%s' failed: %s", tag, shaderc_result_get_error_message(res));
        shaderc_result_release(res);
        shaderc_compile_options_release(opts);
        shaderc_compiler_release(compiler);
        return FALSE;
    }

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = shaderc_result_get_length(res);
    smci.pCode    = (const uint32_t*)shaderc_result_get_bytes(res);
    VkResult r = vkCreateShaderModule(s->device, &smci, NULL, out);

    shaderc_result_release(res);
    shaderc_compile_options_release(opts);
    shaderc_compiler_release(compiler);

    if (r != VK_SUCCESS)
    {
        log_error("vkCreateShaderModule '%s' failed: %d", tag, (int)r);
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_rect_pipeline(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    if (!_vulkan_renderer_internal__compile_shader(shaderc_vertex_shader,
            _VULKAN_RENDERER_INTERNAL__RECT_VS_GLSL, "rect.vs", &s->rect_vs)) { return FALSE; }
    if (!_vulkan_renderer_internal__compile_shader(shaderc_fragment_shader,
            _VULKAN_RENDERER_INTERNAL__RECT_FS_GLSL, "rect.fs", &s->rect_fs)) { return FALSE; }

    //
    // Vertex input: same 11-float layout gles3 uses, split into
    // five attributes matching the shader.
    //
    VkVertexInputBindingDescription vbind = { 0 };
    vbind.binding   = 0;
    vbind.stride    = sizeof(_vulkan_renderer_internal__vertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[5] = { 0 };
    vattr[0].location = 0; vattr[0].binding = 0; vattr[0].format = VK_FORMAT_R32G32_SFLOAT;       vattr[0].offset = offsetof(_vulkan_renderer_internal__vertex, x);
    vattr[1].location = 1; vattr[1].binding = 0; vattr[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[1].offset = offsetof(_vulkan_renderer_internal__vertex, r);
    vattr[2].location = 2; vattr[2].binding = 0; vattr[2].format = VK_FORMAT_R32G32_SFLOAT;       vattr[2].offset = offsetof(_vulkan_renderer_internal__vertex, lx);
    vattr[3].location = 3; vattr[3].binding = 0; vattr[3].format = VK_FORMAT_R32G32_SFLOAT;       vattr[3].offset = offsetof(_vulkan_renderer_internal__vertex, rect_w);
    vattr[4].location = 4; vattr[4].binding = 0; vattr[4].format = VK_FORMAT_R32_SFLOAT;          vattr[4].offset = offsetof(_vulkan_renderer_internal__vertex, radius);

    VkPipelineVertexInputStateCreateInfo visci = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    visci.vertexBindingDescriptionCount   = 1;
    visci.pVertexBindingDescriptions      = &vbind;
    visci.vertexAttributeDescriptionCount = 5;
    visci.pVertexAttributeDescriptions    = vattr;

    VkPipelineInputAssemblyStateCreateInfo iasci = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    //
    // Dynamic viewport + scissor so we don't bake screen size into
    // the PSO. Rebuilt at framebuffer resize without touching the
    // pipeline object.
    //
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsci = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsci.dynamicStateCount = 2;
    dsci.pDynamicStates    = dyn;

    VkPipelineViewportStateCreateInfo vsci = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vsci.viewportCount = 1;
    vsci.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsci = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.cullMode    = VK_CULL_MODE_NONE;
    rsci.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsci.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msci = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    //
    // Separate-alpha blend -- same contract as every other
    // backend: RGB uses SRC_ALPHA/ONE_MINUS_SRC_ALPHA, alpha uses
    // ONE/ONE_MINUS_SRC_ALPHA so the framebuffer stays opaque.
    //
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbci = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbci.attachmentCount = 1;
    cbci.pAttachments    = &cba;

    //
    // Push constants deliver the viewport size to the VS. One 8-
    // byte push range suffices.
    //
    VkPushConstantRange pcr = { 0 };
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(s->device, &plci, NULL, &s->rect_pipeline_layout) != VK_SUCCESS)
    {
        log_error("vkCreatePipelineLayout failed");
        return FALSE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = { { 0 } };
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->rect_vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->rect_fs;
    stages[1].pName  = "main";

    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState      = &vsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState   = &msci;
    gpci.pColorBlendState    = &cbci;
    gpci.pDynamicState       = &dsci;
    gpci.layout              = s->rect_pipeline_layout;
    gpci.renderPass          = s->render_pass;
    gpci.subpass             = 0;

    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gpci, NULL, &s->rect_pipeline) != VK_SUCCESS)
    {
        log_error("vkCreateGraphicsPipelines (rect) failed");
        return FALSE;
    }
    return TRUE;
}

static uint32_t _vulkan_renderer_internal__find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    {
        if ((type_filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static boole _vulkan_renderer_internal__create_rect_vertex_buffers(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkDeviceSize size = sizeof(_vulkan_renderer_internal__vertex) *
                        _VULKAN_RENDERER_INTERNAL__MAX_QUADS *
                        _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = size;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bci, NULL, &s->rect_vbo[i]) != VK_SUCCESS)
        {
            log_error("vkCreateBuffer (rect vbo %d) failed", i);
            return FALSE;
        }

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(s->device, s->rect_vbo[i], &mr);

        //
        // Host-visible + coherent: vertex writes land in memory
        // without an explicit flush. Fine for a 192 KB buffer
        // at 60 fps on modern GPUs.
        //
        uint32_t mt = _vulkan_renderer_internal__find_memory_type(
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX)
        {
            log_error("no host-visible+coherent memory type for rect vbo");
            return FALSE;
        }

        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(s->device, &mai, NULL, &s->rect_vbo_mem[i]) != VK_SUCCESS)
        {
            log_error("vkAllocateMemory (rect vbo %d) failed", i);
            return FALSE;
        }
        vkBindBufferMemory(s->device, s->rect_vbo[i], s->rect_vbo_mem[i], 0);

        //
        // Persistent map so submit_rect can stamp vertices without
        // re-mapping every frame.
        //
        if (vkMapMemory(s->device, s->rect_vbo_mem[i], 0, size, 0,
                        (void**)&s->rect_vbo_ptr[i]) != VK_SUCCESS)
        {
            log_error("vkMapMemory (rect vbo %d) failed", i);
            return FALSE;
        }
    }
    return TRUE;
}

//============================================================================
// text + image plumbing (lazy, built on first atlas / image request)
//============================================================================

static boole _vulkan_renderer_internal__ensure_tx_plumbing(void)
{
    if (!_vulkan_renderer_internal__create_tx_sampler())             { return FALSE; }
    if (!_vulkan_renderer_internal__create_tx_descriptor_layout())   { return FALSE; }
    if (!_vulkan_renderer_internal__create_tx_descriptor_pool())     { return FALSE; }
    if (!_vulkan_renderer_internal__create_text_pipeline())          { return FALSE; }
    if (!_vulkan_renderer_internal__create_tx_vertex_buffers())      { return FALSE; }
    //
    // image_pipeline is left unbuilt for now. The image FS compiles
    // fine but the submit path hasn't been wired; the header comment
    // on renderer__create_texture_rgba explains. Mechanical follow-up
    // once the text path is proven.
    //
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_sampler(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter        = VK_FILTER_LINEAR;
    sci.minFilter        = VK_FILTER_LINEAR;
    sci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy    = 1.0f;
    sci.compareEnable    = VK_FALSE;
    sci.compareOp        = VK_COMPARE_OP_NEVER;
    sci.mipLodBias       = 0.0f;
    sci.minLod           = 0.0f;
    sci.maxLod           = 0.0f;
    sci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(s->device, &sci, NULL, &s->tx_sampler) != VK_SUCCESS)
    {
        log_error("vkCreateSampler (text) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_descriptor_layout(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    //
    // One binding: combined image sampler at set=0, binding=0,
    // used by the fragment shader (`layout(set=0, binding=0)
    // uniform sampler2D u_atlas`). Matches the text FS shader source
    // at the top of this file.
    //
    VkDescriptorSetLayoutBinding b = { 0 };
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    b.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(s->device, &dslci, NULL, &s->tx_dsl) != VK_SUCCESS)
    {
        log_error("vkCreateDescriptorSetLayout (tx) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_descriptor_pool(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    //
    // One pool, sized for MAX_ATLASES combined-image-samplers + the
    // same number of sets. Descriptor sets are allocated on-demand
    // in upload_atlas.
    //
    VkDescriptorPoolSize ps = { 0 };
    ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = _VULKAN_RENDERER_INTERNAL__MAX_ATLASES;

    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets       = _VULKAN_RENDERER_INTERNAL__MAX_ATLASES;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(s->device, &pci, NULL, &s->tx_descriptor_pool) != VK_SUCCESS)
    {
        log_error("vkCreateDescriptorPool (tx) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_text_pipeline(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    if (!_vulkan_renderer_internal__compile_shader(
            shaderc_vertex_shader, _VULKAN_RENDERER_INTERNAL__TX_VS_GLSL,
            "tx.vert", &s->tx_vs)) { return FALSE; }
    if (!_vulkan_renderer_internal__compile_shader(
            shaderc_fragment_shader, _VULKAN_RENDERER_INTERNAL__TEXT_FS_GLSL,
            "text.frag", &s->text_fs)) { return FALSE; }

    //
    // Pipeline layout: the shared tx DSL + same push-constant range
    // the rect pipeline uses (viewport, 8 bytes vec2 at VS stage).
    //
    VkPushConstantRange pcr = { 0 };
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &s->tx_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(s->device, &plci, NULL, &s->text_pipeline_layout) != VK_SUCCESS)
    {
        log_error("vkCreatePipelineLayout (text) failed");
        return FALSE;
    }

    //
    // Vertex input: tx_vertex struct -- pos (vec2) + color (vec4) + uv (vec2).
    //
    VkVertexInputBindingDescription vib = { 0 };
    vib.binding   = 0;
    vib.stride    = sizeof(_vulkan_renderer_internal__tx_vertex);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription via[3] = { 0 };
    via[0].location = 0;
    via[0].binding  = 0;
    via[0].format   = VK_FORMAT_R32G32_SFLOAT;
    via[0].offset   = offsetof(_vulkan_renderer_internal__tx_vertex, x);
    via[1].location = 1;
    via[1].binding  = 0;
    via[1].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    via[1].offset   = offsetof(_vulkan_renderer_internal__tx_vertex, r);
    via[2].location = 2;
    via[2].binding  = 0;
    via[2].format   = VK_FORMAT_R32G32_SFLOAT;
    via[2].offset   = offsetof(_vulkan_renderer_internal__tx_vertex, u);

    VkPipelineVertexInputStateCreateInfo visci = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    visci.vertexBindingDescriptionCount   = 1;
    visci.pVertexBindingDescriptions      = &vib;
    visci.vertexAttributeDescriptionCount = 3;
    visci.pVertexAttributeDescriptions    = via;

    VkPipelineInputAssemblyStateCreateInfo iasci = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    //
    // Dynamic viewport + scissor (same as rect pipeline) so no re-
    // create on resize.
    //
    VkPipelineViewportStateCreateInfo vpsci = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpsci.viewportCount = 1;
    vpsci.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsci = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.cullMode    = VK_CULL_MODE_NONE;
    rsci.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsci.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msci = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    //
    // Separate-alpha blend -- same as every other backend per
    // VISUAL CONTRACT in renderer.h.
    //
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbsci = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbsci.attachmentCount = 1;
    cbsci.pAttachments    = &cba;

    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsci = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsci.dynamicStateCount = 2;
    dsci.pDynamicStates    = dyn_states;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->tx_vs;
    stages[0].pName  = "main";
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->text_fs;
    stages[1].pName  = "main";

    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState      = &vpsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState   = &msci;
    gpci.pColorBlendState    = &cbsci;
    gpci.pDynamicState       = &dsci;
    gpci.layout              = s->text_pipeline_layout;
    gpci.renderPass          = s->render_pass;
    gpci.subpass             = 0;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gpci, NULL, &s->text_pipeline) != VK_SUCCESS)
    {
        log_error("vkCreateGraphicsPipelines (text) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_vertex_buffers(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;

    VkDeviceSize size = (VkDeviceSize)sizeof(_vulkan_renderer_internal__tx_vertex) *
                        _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS *
                        _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = size;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bci, NULL, &s->text_vbo[i]) != VK_SUCCESS)
        {
            log_error("vkCreateBuffer (text vbo %d) failed", i);
            return FALSE;
        }

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(s->device, s->text_vbo[i], &req);

        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = _vulkan_renderer_internal__find_memory_type(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s->device, &mai, NULL, &s->text_vbo_mem[i]) != VK_SUCCESS)
        {
            log_error("vkAllocateMemory (text vbo %d) failed", i);
            return FALSE;
        }
        vkBindBufferMemory(s->device, s->text_vbo[i], s->text_vbo_mem[i], 0);
        if (vkMapMemory(s->device, s->text_vbo_mem[i], 0, size, 0, (void**)&s->text_vbo_ptr[i]) != VK_SUCCESS)
        {
            log_error("vkMapMemory (text vbo %d) failed", i);
            return FALSE;
        }
    }
    return TRUE;
}

//
// Atlas pool: next-free-slot scan. Fixed size (MAX_ATLASES); used
// entries mark in_use=TRUE. Release clears in_use and destroys the
// underlying Vulkan objects.
//
static _vulkan_renderer_internal__atlas_entry* _vulkan_renderer_internal__alloc_atlas_slot(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_ATLASES; i++)
    {
        _vulkan_renderer_internal__atlas_entry* a = &s->atlas_pool[i];
        if (!a->in_use)
        {
            memset(a, 0, sizeof(*a));
            a->in_use = TRUE;
            return a;
        }
    }
    return NULL;
}

static void _vulkan_renderer_internal__release_atlas(_vulkan_renderer_internal__atlas_entry* a)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (a == NULL) { return; }
    if (a->desc_set != VK_NULL_HANDLE)
    {
        vkFreeDescriptorSets(s->device, s->tx_descriptor_pool, 1, &a->desc_set);
    }
    if (a->view   != VK_NULL_HANDLE) { vkDestroyImageView(s->device, a->view,   NULL); }
    if (a->image  != VK_NULL_HANDLE) { vkDestroyImage    (s->device, a->image,  NULL); }
    if (a->memory != VK_NULL_HANDLE) { vkFreeMemory      (s->device, a->memory, NULL); }
    memset(a, 0, sizeof(*a));
}

//
// One-shot VkImage upload. Staging buffer -> VkImage copy via a
// dedicated command buffer; we block until the GPU finishes the
// copy before creating the SRV-equivalent (image view + descriptor
// set). Matches the "atlas loads are rare, simplicity > batching"
// trade-off the d3d12 path uses.
//
static boole _vulkan_renderer_internal__upload_atlas(_vulkan_renderer_internal__atlas_entry* a, const ubyte* pixels, int w, int h, VkFormat format, int bytes_per_pixel)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    VkDeviceSize byte_count = (VkDeviceSize)w * (VkDeviceSize)h * (VkDeviceSize)bytes_per_pixel;

    //
    // 1. Staging buffer (host-visible) + copy pixels.
    //
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = byte_count;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bci, NULL, &staging) != VK_SUCCESS) { log_error("staging vkCreateBuffer failed"); return FALSE; }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(s->device, staging, &req);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = _vulkan_renderer_internal__find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s->device, &mai, NULL, &staging_mem) != VK_SUCCESS) { vkDestroyBuffer(s->device, staging, NULL); log_error("staging vkAllocateMemory failed"); return FALSE; }
        vkBindBufferMemory(s->device, staging, staging_mem, 0);
        void* mapped = NULL;
        vkMapMemory(s->device, staging_mem, 0, byte_count, 0, &mapped);
        memcpy(mapped, pixels, (size_t)byte_count);
        vkUnmapMemory(s->device, staging_mem);
    }

    //
    // 2. VkImage + device memory.
    //
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = format;
        ici.extent.width  = (uint32_t)w;
        ici.extent.height = (uint32_t)h;
        ici.extent.depth  = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(s->device, &ici, NULL, &a->image) != VK_SUCCESS)
        {
            vkDestroyBuffer(s->device, staging, NULL);
            vkFreeMemory   (s->device, staging_mem, NULL);
            log_error("vkCreateImage (atlas) failed");
            return FALSE;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(s->device, a->image, &req);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = _vulkan_renderer_internal__find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(s->device, &mai, NULL, &a->memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(s->device, staging, NULL);
            vkFreeMemory   (s->device, staging_mem, NULL);
            vkDestroyImage (s->device, a->image, NULL); a->image = VK_NULL_HANDLE;
            log_error("vkAllocateMemory (atlas) failed");
            return FALSE;
        }
        vkBindImageMemory(s->device, a->image, a->memory, 0);
    }

    //
    // 3. One-shot command buffer: layout UNDEFINED -> TRANSFER_DST,
    //    copy staging -> image, layout TRANSFER_DST -> SHADER_READ.
    //    Allocated from the main command pool; submitted on the
    //    main queue; we block via vkQueueWaitIdle since atlas uploads
    //    are rare and this keeps the lifetimes simple.
    //
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = s->command_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer up = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbai, &up) != VK_SUCCESS)
    {
        vkDestroyBuffer(s->device, staging, NULL);
        vkFreeMemory   (s->device, staging_mem, NULL);
        log_error("atlas upload vkAllocateCommandBuffers failed");
        return FALSE;
    }
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(up, &cbbi);

    VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_dst.srcAccessMask       = 0;
    to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image               = a->image;
    to_dst.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.baseMipLevel   = 0;
    to_dst.subresourceRange.levelCount     = 1;
    to_dst.subresourceRange.baseArrayLayer = 0;
    to_dst.subresourceRange.layerCount     = 1;
    vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_dst);

    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width  = (uint32_t)w;
    region.imageExtent.height = (uint32_t)h;
    region.imageExtent.depth  = 1;
    vkCmdCopyBufferToImage(up, staging, a->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_read);

    vkEndCommandBuffer(up);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &up;
    vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(s->queue);

    vkFreeCommandBuffers(s->device, s->command_pool, 1, &up);
    vkDestroyBuffer(s->device, staging, NULL);
    vkFreeMemory(s->device, staging_mem, NULL);

    //
    // 4. ImageView + descriptor set.
    //
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = a->image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = format;
    ivci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s->device, &ivci, NULL, &a->view) != VK_SUCCESS)
    {
        log_error("vkCreateImageView (atlas) failed");
        return FALSE;
    }

    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = s->tx_descriptor_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &s->tx_dsl;
    if (vkAllocateDescriptorSets(s->device, &dsai, &a->desc_set) != VK_SUCCESS)
    {
        log_error("vkAllocateDescriptorSets (atlas) failed");
        return FALSE;
    }

    VkDescriptorImageInfo dii = { 0 };
    dii.sampler     = s->tx_sampler;
    dii.imageView   = a->view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet          = a->desc_set;
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo      = &dii;
    vkUpdateDescriptorSets(s->device, 1, &wds, 0, NULL);

    a->width  = w;
    a->height = h;
    return TRUE;
}

//
// Draw queued text runs. Mirrors the d3d12 flush_text:
//   - append-only semantics (no reset of vert_count or run_count),
//   - a single vkCmdDraw per run, binding that run's atlas descriptor
//     first so glyphs sample the right texture.
//
static void _vulkan_renderer_internal__flush_text(void)
{
    _vulkan_renderer_internal__state* s = &_vulkan_renderer_internal__g;
    if (!s->frame_active) { return; }
    if (s->text_first_undrawn_run >= s->text_run_count) { return; }

    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->text_pipeline);

    VkBuffer     vbs[1] = { s->text_vbo[s->frame_index] };
    VkDeviceSize ofs[1] = { 0 };
    vkCmdBindVertexBuffers(cb, 0, 1, vbs, ofs);

    float viewport[2];
    viewport[0] = (float)s->viewport_w;
    viewport[1] = (float)s->viewport_h;
    vkCmdPushConstants(cb, s->text_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewport),
                       viewport);

    for (int i = s->text_first_undrawn_run; i < s->text_run_count; i++)
    {
        _vulkan_renderer_internal__text_run* run = &s->text_runs[i];
        if (run->atlas == NULL || run->vert_count <= 0) { continue; }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                s->text_pipeline_layout, 0, 1, &run->atlas->desc_set,
                                0, NULL);
        vkCmdDraw(cb, (uint32_t)run->vert_count, 1, (uint32_t)run->vert_start, 0);
    }

    s->text_first_undrawn_run = s->text_run_count;
}
