//
// platforms/android/fs_android.c - Android implementation of fs.h.
//
// Compiled only into the Android build (see demo-android/build.ps1 /
// build.sh source lists). Pairs with:
//   - platforms/windows/fs_win32.c     Win32 file mapping
//   - platforms/linux/fs_linux.c       POSIX (uses _fs_posix.h shim)
//   - platforms/macos/fs_macos.c       POSIX (uses _fs_posix.h shim)
//
// Two paths inside each fs__* function:
//
//   1. AAssetManager path -- resolves names against the APK's assets/
//      directory. This is what lets parser_xml__load_ui("main.ui") and
//      parser_style__load_styles("main.style") work transparently on
//      Android. platform_android__init installs the asset manager
//      pointer via fs__set_asset_manager before any asset read.
//
//   2. POSIX fallback -- open + read / mmap against the device
//      filesystem. Used when the asset manager isn't set yet, or when
//      the asset path resolves to nothing (lets a sideloaded file at
//      /sdcard/<name> override the packaged asset for debug).
//
// fs_mapped_file carries platform-private state the same way as on
// Windows but with POSIX values:
//   [0] = fd cast to (void*)(intptr_t) -- 0 is a valid fd (stdin), so
//         f->size > 0 is the "mapped?" sentinel, not NULL-check.
//   [1] = unused
//   [2] = view pointer from mmap (also == data)
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

#include <android/asset_manager.h>

#include "types.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

//
// Asset manager bridge. platform_android__init installs this after
// ANativeActivity_onCreate hands us the struct android_app.
// Typed as void* in fs.h so the header doesn't have to pull in the
// NDK's <android/asset_manager.h>; the cast happens here.
//
static AAssetManager* _fs_android_internal__asset_manager = NULL;

//
// Sideload directory for adb-push hot reload. When non-NULL every
// fs__read_entire_file call first tries <sideload_dir>/<name> via
// POSIX open; only if that misses do we fall through to the
// AAssetManager + absolute-path paths. Installed by
// platform_android__init once the NDK hands us an
// ANativeActivity->externalDataPath. Borrowed pointer -- lives as
// long as the android_app struct does, i.e. the process.
//
static const char* _fs_android_internal__sideload_dir = NULL;

void fs__set_asset_manager(void* mgr)
{
    _fs_android_internal__asset_manager = (AAssetManager*)mgr;
}

void fs__set_sideload_dir(const char* dir)
{
    _fs_android_internal__sideload_dir = dir;
    if (dir != NULL)
    {
        log_info("fs: sideload dir = %s  (adb push <file> here for hot reload)", dir);
    }
}

//
// Strip any leading `/` from a path. AAssetManager and our sideload
// join both want a relative name; the convention for unified main.c
// code is `DEMO_SOURCE_DIR "/main.ui"` which produces `"/main.ui"`
// when the Android build sets DEMO_SOURCE_DIR to the empty string.
// Without this strip AAssetManager_open treats `/main.ui` as missing.
//
static const char* _fs_android_internal__strip_leading_slash(const char* path)
{
    while (path != NULL && *path == '/')
    {
        path++;
    }
    return path;
}

//
// Open a POSIX file at an absolute path and slurp its entire contents
// into a newly-allocated heap buffer. Returns NULL if the file is
// missing, too big (>2 GiB), or any read step fails. Shared between
// the sideload + POSIX-fallback branches so we don't duplicate the
// fstat/read loop twice.
//
static char* _fs_android_internal__read_posix(const char* absolute_path, int64* out_size)
{
    int fd = open(absolute_path, O_RDONLY);
    if (fd < 0)
    {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 0x7fffffff)
    {
        close(fd);
        return NULL;
    }
    char* buf = (char*)GUI_MALLOC_T((size_t)st.st_size + 1, MM_TYPE_FS);
    if (buf == NULL)
    {
        close(fd);
        return NULL;
    }
    ssize_t total = 0;
    while (total < st.st_size)
    {
        ssize_t n = read(fd, buf + total, (size_t)(st.st_size - total));
        if (n <= 0)
        {
            GUI_FREE(buf);
            close(fd);
            return NULL;
        }
        total += n;
    }
    buf[total] = 0;
    close(fd);
    *out_size = (int64)total;
    return buf;
}

char* fs__read_entire_file(char* path, int64* out_size)
{
    const char* name = _fs_android_internal__strip_leading_slash(path);

    //
    // 1. SIDELOAD. If the host has installed a sideload dir, check
    //    <sideload>/<name> first -- wins over APK-packaged assets so
    //    adb-pushed files immediately override. No log on miss; the
    //    expected steady state is "no sideloaded file, fall through".
    //
    if (_fs_android_internal__sideload_dir != NULL && name != NULL && *name != 0)
    {
        char joined[1024];
        int written = snprintf(joined, sizeof(joined), "%s/%s", _fs_android_internal__sideload_dir, name);
        if (written > 0 && (size_t)written < sizeof(joined))
        {
            char* buf = _fs_android_internal__read_posix(joined, out_size);
            if (buf != NULL)
            {
                log_info("fs: sideload hit '%s' (%lld bytes)", joined, (long long)*out_size);
                return buf;
            }
        }
    }

    //
    // 2. AAssetManager. Parser callers pass paths like "main.ui" that
    //    live inside the APK's assets/ directory; plain open() can't
    //    reach into the APK zip. Leading `/` was already stripped so
    //    `"/main.ui"` and `"main.ui"` both land on `"main.ui"`.
    //
    if (_fs_android_internal__asset_manager != NULL && name != NULL && *name != 0)
    {
        AAsset* asset = AAssetManager_open(_fs_android_internal__asset_manager, name, AASSET_MODE_BUFFER);
        if (asset != NULL)
        {
            off_t asset_len = AAsset_getLength(asset);
            if (asset_len < 0 || asset_len > 0x7fffffff)
            {
                log_error("fs: asset length out of range for '%s'", name);
                AAsset_close(asset);
                return NULL;
            }
            char* buf = (char*)GUI_MALLOC_T((size_t)asset_len + 1, MM_TYPE_FS);
            if (buf == NULL)
            {
                log_error("fs: alloc(%lld) failed for asset '%s'", (long long)(asset_len + 1), name);
                AAsset_close(asset);
                return NULL;
            }
            int read_n = AAsset_read(asset, buf, (size_t)asset_len);
            AAsset_close(asset);
            if (read_n < 0 || read_n != (int)asset_len)
            {
                log_error("fs: AAsset_read short/failed (got %d of %lld) for '%s'", read_n, (long long)asset_len, name);
                GUI_FREE(buf);
                return NULL;
            }
            buf[read_n] = 0;
            *out_size = (int64)read_n;
            return buf;
        }
    }

    //
    // 3. POSIX fallback with the ORIGINAL path. Lets callers reach
    //    absolute paths like /sdcard/foo.ui when they want them. Same
    //    2 GiB guard as the Windows branch.
    //
    return _fs_android_internal__read_posix(path, out_size);
}

//
// POSIX mmap(MAP_SHARED, PROT_READ). The kernel's page cache
// transparently shares physical pages across every process that
// maps the same file -- same deduplication semantics as Windows'
// CreateFileMapping. We stash the fd in _platform[0] (cast through
// intptr_t) so unmap can close it.
//
// NOT used for APK-packaged assets: AAssetManager assets are typically
// compressed in the zip, so they have no addressable fd + offset the
// caller could mmap directly. Asset bytes go through the heap-copy
// fs__read_entire_file path above. mmap is for /sdcard paths, external
// files, debug sideloads, etc.
//
boole fs__map_file(char* path, fs_mapped_file* out)
{
    if (path == NULL || out == NULL)
    {
        return FALSE;
    }
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        log_error("fs__map_file: open failed for '%s' (errno %d)", path, errno);
        return FALSE;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        log_error("fs__map_file: fstat failed or empty file: '%s'", path);
        close(fd);
        return FALSE;
    }

    void* view = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED)
    {
        log_error("fs__map_file: mmap failed for '%s' (errno %d)", path, errno);
        close(fd);
        return FALSE;
    }

    out->data         = (const ubyte*)view;
    out->size         = (int64)st.st_size;
    out->_platform[0] = (void*)(intptr_t)fd;
    out->_platform[1] = NULL;
    out->_platform[2] = view;
    return TRUE;
}

void fs__unmap_file(fs_mapped_file* f)
{
    if (f == NULL || f->data == NULL)
    {
        return;
    }
    if (f->_platform[2] != NULL && f->size > 0)
    {
        munmap(f->_platform[2], (size_t)f->size);
    }
    //
    // fd stashed as (void*)(intptr_t)fd. 0 is a valid fd (stdin), so
    // we can't use NULL as the sentinel -- check f->size > 0 instead
    // as the "was mapped" signal.
    //
    if (f->size > 0)
    {
        close((int)(intptr_t)f->_platform[0]);
    }
    memset(f, 0, sizeof(*f));
}
