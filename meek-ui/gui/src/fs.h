#ifndef FS_H
#define FS_H

#include "types.h"
#include "gui_api.h"

//
//fs.h - small filesystem helpers built directly on Win32.
//paired with fs.c. extracted because three modules (parser_xml,
//parser_style, hot_reload) all needed the same "read a whole file
//into a heap buffer" routine and were maintaining identical copies.
//

/**
 * Read the entire contents of a file at the given UTF-8 path into a
 * heap-allocated buffer. The buffer is null-terminated for convenience
 * but the returned size is authoritative; binary data may contain
 * embedded null bytes.
 *
 * @function fs__read_entire_file
 * @param {char*} path - UTF-8 path. Converted to UTF-16 internally for the W-suffixed Win32 api.
 * @param {int64*} out_size - On success, receives the byte length of the read content (excluding the trailing null).
 * @return {char*} Heap buffer (caller must free), or NULL on any I/O error (file not found, read failure, file > 2GB, allocation failure).
 *
 * Open mode shares both read and write so editors that briefly hold
 * the file open mid-save don't block this read. Errors are silent;
 * callers that want logging should add their own.
 */
GUI_API char* fs__read_entire_file(char* path, int64* out_size);

//============================================================================
//memory-mapped files
//============================================================================
//
//fs__map_file maps a file read-only into the process's address space
//without copying. multiple processes that map the same file share the
//same physical pages via the kernel's page cache -- zero extra RAM
//per additional process. this is the idiomatic way to share
//read-only blobs (TTF fonts, shader bytecode, compiled .ui, etc.)
//across processes on every platform:
//
//   Windows:          CreateFileMapping + MapViewOfFile
//   POSIX / Android:  mmap(fd, PROT_READ, MAP_SHARED)
//   iOS / macOS:      same as POSIX
//
//semantics: the mapping is read-only. writes through `data` SIGSEGV /
//raise an access violation. the mapping stays alive until
//fs__unmap_file; keep the fs_mapped_file struct around while any code
//reads through `data`.
//
//on Windows, the API requires three handles (file, mapping object,
//view pointer). on POSIX one pointer + length is enough. the struct
//holds both shapes of platform state behind a single _platform array
//so callers don't have to know which OS they're on.
//

typedef struct fs_mapped_file
{
    const ubyte* data;    // pointer to mapped region (read-only).
    int64        size;    // byte length of the region.

    //
    //opaque platform handles. on Windows: [0] = file HANDLE,
    //[1] = mapping HANDLE, [2] = view pointer (same as data cast to
    //void*). on POSIX: unused -- `data` + `size` are passed to munmap
    //directly.
    //
    void* _platform[3];
} fs_mapped_file;

/**
 * Memory-map a file read-only. On all platforms this is shared-by-
 * default: if another process maps the same file, the kernel reuses
 * the same physical pages. Ideal for font files, shader blobs, and
 * other read-only assets that a host app wants to share across
 * instances (multiple windows in one app, or multiple co-operating
 * daemons).
 *
 * @function fs__map_file
 * @param {char*} path - UTF-8 path.
 * @param {fs_mapped_file*} out - Filled on success.
 * @return {boole} TRUE on success; FALSE on any I/O or mapping error.
 */
GUI_API boole fs__map_file(char* path, fs_mapped_file* out);

/**
 * Release a mapping obtained via fs__map_file. After this call the
 * pointer stored in `data` is invalid. Safe to call on a zero-filled
 * struct (no-op).
 *
 * @function fs__unmap_file
 * @param {fs_mapped_file*} f - Mapping to release.
 * @return {void}
 */
GUI_API void fs__unmap_file(fs_mapped_file* f);

//============================================================================
// Android: asset manager bridge.
//============================================================================
//
// On Android, the files a host app ships (main.ui, main.style, TTFs,
// etc.) live inside the APK under `assets/` and aren't reachable via
// plain open() -- they're entries in the APK's zip archive. the
// framework exposes them via AAssetManager, which needs to be plumbed
// into fs so the parser and font modules can stay path-based.
//
// The platform layer calls fs__set_asset_manager(app->activity->assetManager)
// after ANativeActivity_onCreate gives it an app pointer. After that,
// fs__read_entire_file on Android tries AAssetManager_open(path) FIRST
// and only falls back to plain open() if the asset-manager path misses.
// Windows builds ignore the setter entirely (compiled out).
//
// The parameter is typed `void*` to keep fs.h from having to include
// the NDK headers; fs.c casts it to `AAssetManager*` inside its
// #if !defined(_WIN32) branch.
//

/**
 * Install the AAssetManager used for APK asset reads on Android. Must
 * be called before any fs__read_entire_file / fs__map_file call that
 * should resolve against APK-packaged assets; otherwise those fall
 * back to plain filesystem lookups, which won't find APK-internal
 * files.
 *
 * @function fs__set_asset_manager
 * @param {void*} mgr - AAssetManager* on Android. Ignored on non-Android builds.
 * @return {void}
 */
GUI_API void fs__set_asset_manager(void* mgr);

//
// Sideload directory override. When set, fs__read_entire_file tries
// <sideload_dir>/<path> (with any leading `/` stripped from path)
// BEFORE the AAssetManager / POSIX fallback chain. Intended for dev
// builds on Android: `adb push main.ui <sideload_dir>/main.ui` drops
// a newer copy next to the APK-packaged version, and the next
// hot_reload poll picks it up. Passing NULL (or never calling this)
// turns the feature off.
//
// Typical wiring on Android:
//     fs__set_sideload_dir(app->activity->externalDataPath);
// which resolves to /storage/emulated/0/Android/data/<pkg>/files/ --
// readable by the app and writable by adb on every modern Android
// version without runtime permissions or root.
//
// Ignored on non-Android builds (Windows uses real paths already).
//

/**
 * Install a sideload directory that fs__read_entire_file checks
 * before AAssetManager / absolute-path fallbacks. The string is
 * borrowed, not copied; keep it alive for the process lifetime.
 *
 * @function fs__set_sideload_dir
 * @param {const char*} dir - UTF-8 directory path, no trailing slash. NULL to disable.
 * @return {void}
 */
GUI_API void fs__set_sideload_dir(const char* dir);

#endif
