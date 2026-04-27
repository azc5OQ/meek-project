#ifndef CLIB_MEMORY_MANAGER_H
#define CLIB_MEMORY_MANAGER_H

//
// clib/memory_manager.h - allocation tracker.
//
// Wraps malloc / calloc / realloc / free so we can see, at any time,
// which blocks are live, how big they are, and what they're being
// used for (MM_TYPE_*). Allocation metadata is stored in an inline
// header immediately before the user pointer, so both alloc and free
// are O(1) -- the tracker doesn't scan any table, it just reaches
// one word behind the pointer and unlinks a node.
//
// USAGE
// -----
//     #include "clib/memory_manager.h"
//
//     void* p = GUI_MALLOC(128);
//     void* q = GUI_MALLOC_T(sizeof(thing), MM_TYPE_TEXT);
//     p = GUI_REALLOC(p, 256);
//     GUI_FREE(p);
//     GUI_FREE(q);
//
// TOGGLE
// ------
// The CMake option GUI_TRACK_ALLOCATIONS controls whether the macros
// go through the tracker or straight to the C stdlib. When off the
// tracker is not even linked; GUI_* expand to malloc/calloc/realloc/
// free with zero overhead.
//
// NAMING
// ------
// The GUI_ prefix sidesteps a Windows SDK collision: winnt.h defines
// MEM_FREE as a VirtualAlloc flag constant (0x00010000). Using our
// own prefix also matches the rest of the project (GUI_API, etc.).
//
// THREADING
// ---------
// Thread-safe. A single mutex (CRITICAL_SECTION on Windows,
// pthread_mutex_t elsewhere) guards the live-list head and stats
// across all public entry points. Contention is negligible because
// the only operation under the lock is O(1) pointer fixup + a
// handful of int64 counter updates.
//

#include "../types.h"
#include "../gui_api.h"

#include <stdlib.h>

//
// Allocation categories. Used purely for reporting -- every block has
// a type so `memory_manager__dump_live` can tell you "5 MB in textures,
// 800 KB in font atlases, 2 MB in parser nodes". Add new values at
// the end; order is not serialized.
//
typedef enum mm_type
{
    MM_TYPE_GENERIC = 0,
    MM_TYPE_TEXT,      // cstrings, parser token buffers.
    MM_TYPE_NODE,      // scene graph nodes + attribute arrays.
    MM_TYPE_STYLE,     // style rules + selector tables.
    MM_TYPE_FONT,      // font atlases, glyph arrays, ttf blobs.
    MM_TYPE_IMAGE,     // texture pixel buffers.
    MM_TYPE_RENDERER,  // backend-owned buffers (vb/ib staging, etc.).
    MM_TYPE_FS,        // file-read buffers.
    MM_TYPE_COUNT
} mm_type;

//
// Snapshot of the tracker state. Cheap to read; returned by value so
// callers don't have to lock or worry about the head pointer moving.
//
typedef struct memory_manager_stats
{
    int64 live_allocs;             // currently outstanding allocations.
    int64 total_allocs;             // cumulative count of alloc calls.
    int64 total_frees;              // cumulative count of free calls.
    int64 live_bytes;               // sum of sizes of live allocations.
    int64 peak_bytes;               // highest live_bytes ever reached.
    int64 live_bytes_by_type[MM_TYPE_COUNT];
} memory_manager_stats;

//
// Lifecycle. init zeros stats + head. shutdown logs any leaks (live_
// allocs != 0) and walks the live list dumping per-block type + size.
// Safe to call shutdown even if init was never called.
//
GUI_API void memory_manager__init(void);
GUI_API void memory_manager__shutdown(void);

//
// Core API. All three alloc flavors take a type so every live block
// carries its category. Pass MM_TYPE_GENERIC when nothing better fits.
// Realloc preserves the block's original type.
//
GUI_API void* memory_manager__alloc(size_t size, mm_type type);
GUI_API void* memory_manager__calloc(size_t count, size_t size, mm_type type);
GUI_API void* memory_manager__realloc(void* ptr, size_t new_size);
GUI_API void  memory_manager__free(void* ptr);

//
// Reporting.
//
GUI_API memory_manager_stats memory_manager__get_stats(void);
GUI_API void memory_manager__dump_live(void);
GUI_API const char* memory_manager__type_name(mm_type type);

//
// Macros that call sites use. Flipping GUI_TRACK_ALLOCATIONS off (or
// defining GUI_DISABLE_MEMORY_TRACKING directly) makes them expand
// to the plain stdlib calls -- no tracker, no overhead, type arg is
// silently discarded.
//
#if defined(GUI_TRACK_ALLOCATIONS) && !defined(GUI_DISABLE_MEMORY_TRACKING)
    #define GUI_MALLOC(size)              memory_manager__alloc((size), MM_TYPE_GENERIC)
    #define GUI_MALLOC_T(size, type)      memory_manager__alloc((size), (type))
    #define GUI_CALLOC(n, size)           memory_manager__calloc((n), (size), MM_TYPE_GENERIC)
    #define GUI_CALLOC_T(n, size, type)   memory_manager__calloc((n), (size), (type))
    #define GUI_REALLOC(ptr, size)        memory_manager__realloc((ptr), (size))
    #define GUI_FREE(ptr)                 memory_manager__free((ptr))
#else
    #define GUI_MALLOC(size)              malloc((size))
    #define GUI_MALLOC_T(size, type)      ((void)(type), malloc((size)))
    #define GUI_CALLOC(n, size)           calloc((n), (size))
    #define GUI_CALLOC_T(n, size, type)   ((void)(type), calloc((n), (size)))
    #define GUI_REALLOC(ptr, size)        realloc((ptr), (size))
    #define GUI_FREE(ptr)                 free((ptr))
#endif

#endif
