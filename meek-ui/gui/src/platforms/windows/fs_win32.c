//
// platforms/windows/fs_win32.c - Windows implementation of fs.h.
//
// Compiled only into the Windows build (see gui/CMakeLists.txt).
// Pairs with:
//   - platforms/android/fs_android.c   Android + POSIX fallback
//   - platforms/linux/fs_linux.c       POSIX (uses _fs_posix.h shim)
//   - platforms/macos/fs_macos.c       POSIX (uses _fs_posix.h shim)
//
// Conventions:
//   - fs__read_entire_file heap-copies the entire file contents and
//     appends a trailing null, same shape as on every other platform.
//   - fs__map_file returns a read-only shared view backed by
//     CreateFileMapping + MapViewOfFile. The kernel page cache dedups
//     physical pages across every process that maps the same file,
//     which is how the font subsystem gets multi-process font sharing
//     for free.
//   - fs__set_asset_manager is a no-op on Windows: asset-manager paths
//     only exist inside an APK. host code calls this unconditionally
//     and this file accepts the call silently.
//
// The fs_mapped_file struct carries platform-private state in its
// _platform[] slots:
//   [0] = file   HANDLE  from CreateFileW
//   [1] = mapping HANDLE  from CreateFileMappingW
//   [2] = view   pointer from MapViewOfFile (also == data)
//

#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "types.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

void fs__set_asset_manager(void* mgr)
{
    //
    // Android-only bridge. On Windows the host has direct filesystem
    // access so there's nothing to install. Accepting the call keeps
    // the host API identical across platforms.
    //
    (void)mgr;
}

void fs__set_sideload_dir(const char* dir)
{
    //
    // Android-only. Windows hosts read .ui / .style directly from the
    // dev tree via DEMO_SOURCE_DIR, so there's no APK to override and
    // no sideload. Accept the call so host code can set it
    // unconditionally regardless of target.
    //
    (void)dir;
}

char* fs__read_entire_file(char* path, int64* out_size)
{
    //
    // UTF-8 -> UTF-16 wide path. Win32 filesystem APIs with non-ASCII
    // paths require the W variants; the *A variants go through the
    // system ANSI code page which varies by locale.
    //
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0)
    {
        return NULL;
    }

    wchar_t* wpath = (wchar_t*)GUI_MALLOC_T((size_t)wlen * sizeof(wchar_t), MM_TYPE_FS);
    if (wpath == NULL)
    {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    GUI_FREE(wpath);
    if (h == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size))
    {
        CloseHandle(h);
        return NULL;
    }
    if (size.QuadPart > 0x7fffffff)
    {
        //
        // Refuse files larger than ~2 GiB. .ui / .style / .ttf files
        // should be tiny; this guards against accidentally pointing
        // the loader at an unrelated giant file (a disk image, a DB).
        //
        CloseHandle(h);
        return NULL;
    }

    char* buf = (char*)GUI_MALLOC_T((size_t)size.QuadPart + 1, MM_TYPE_FS);
    if (buf == NULL)
    {
        CloseHandle(h);
        return NULL;
    }

    DWORD read = 0;
    if (!ReadFile(h, buf, (DWORD)size.QuadPart, &read, NULL))
    {
        GUI_FREE(buf);
        CloseHandle(h);
        return NULL;
    }
    buf[read] = 0;
    CloseHandle(h);

    *out_size = (int64)read;
    return buf;
}

//
// Memory-mapped read-only view. Three kernel objects to keep around
// for cleanup: file handle, mapping handle, view pointer.
//
// PAGE_READONLY matches the GENERIC_READ open -- attempts to write
// through the returned pointer will access-violate. The mapping is
// unnamed: sharing with other processes happens via the common backing
// file, not via a named section, so no explicit setup is needed.
//
boole fs__map_file(char* path, fs_mapped_file* out)
{
    if (path == NULL || out == NULL)
    {
        return FALSE;
    }
    memset(out, 0, sizeof(*out));

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0)
    {
        return FALSE;
    }
    wchar_t* wpath = (wchar_t*)GUI_MALLOC_T((size_t)wlen * sizeof(wchar_t), MM_TYPE_FS);
    if (wpath == NULL)
    {
        return FALSE;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    //
    // Broad share flags so other processes can also map the file, and
    // so editors holding the file open briefly don't block us.
    //
    HANDLE file = CreateFileW(
        wpath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    GUI_FREE(wpath);
    if (file == INVALID_HANDLE_VALUE)
    {
        log_error("fs__map_file: CreateFileW failed for '%s' (error %lu)", path, (unsigned long)GetLastError());
        return FALSE;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0)
    {
        log_error("fs__map_file: GetFileSizeEx failed or empty file: '%s'", path);
        CloseHandle(file);
        return FALSE;
    }
    if (size.QuadPart > 0x7fffffff)
    {
        log_error("fs__map_file: refusing file > 2 GiB: '%s'", path);
        CloseHandle(file);
        return FALSE;
    }

    HANDLE mapping = CreateFileMappingW(
        file,
        NULL,           // default security.
        PAGE_READONLY,
        0, 0,           // mapping size = file size.
        NULL            // unnamed mapping.
    );
    if (mapping == NULL)
    {
        log_error("fs__map_file: CreateFileMappingW failed for '%s' (error %lu)", path, (unsigned long)GetLastError());
        CloseHandle(file);
        return FALSE;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == NULL)
    {
        log_error("fs__map_file: MapViewOfFile failed for '%s' (error %lu)", path, (unsigned long)GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return FALSE;
    }

    out->data         = (const ubyte*)view;
    out->size         = (int64)size.QuadPart;
    out->_platform[0] = (void*)file;
    out->_platform[1] = (void*)mapping;
    out->_platform[2] = view;
    return TRUE;
}

void fs__unmap_file(fs_mapped_file* f)
{
    if (f == NULL || f->data == NULL)
    {
        return;
    }
    //
    // Tear down in reverse of construction. Each of these is a no-op
    // on NULL / invalid handles, but we null-check for resilience
    // against zero-initialized structs.
    //
    if (f->_platform[2] != NULL)
    {
        UnmapViewOfFile(f->_platform[2]);
    }
    if (f->_platform[1] != NULL)
    {
        CloseHandle((HANDLE)f->_platform[1]);
    }
    if (f->_platform[0] != NULL)
    {
        CloseHandle((HANDLE)f->_platform[0]);
    }
    memset(f, 0, sizeof(*f));
}
