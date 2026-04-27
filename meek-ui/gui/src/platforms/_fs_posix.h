#ifndef _FS_POSIX_H
#define _FS_POSIX_H

//
// _fs_posix.h - shared POSIX implementations of the fs.h slurp +
// mmap entry points. Used by fs_linux.c, fs_macos.c, and the
// POSIX fall-through in fs_android.c. Header-only because each
// platform TU still wants its own translation unit (per the
// per-platform-dir convention) but the bodies are identical
// modulo error-message phrasing.
//
// Inclusion shape:
//   - Define FS_POSIX_PLATFORM_TAG before including this header
//     ("linux", "macos", or "android-posix"); used in log messages
//     so a runtime error tells you which platform's path failed.
//   - Then call FS_POSIX_DEFINE_SLURP() and FS_POSIX_DEFINE_MMAP()
//     to emit the function bodies into the including TU.
//
// Why macros instead of plain functions: the per-platform TUs also
// own the `fs__set_asset_manager` / `fs__set_sideload_dir` stubs
// (which differ on Android), and we want a single-binary build
// (no static lib) so each platform contributes ONE .o with the
// full fs interface. Macro expansion fits that shape with zero
// link-time gymnastics.
//

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

#include "types.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

#ifndef FS_POSIX_PLATFORM_TAG
  #define FS_POSIX_PLATFORM_TAG "posix"
#endif

#define FS_POSIX_DEFINE_SLURP()                                                          \
char* fs__read_entire_file(char* path, int64* out_size)                                  \
{                                                                                        \
    if (path == NULL || out_size == NULL) { return NULL; }                               \
    *out_size = 0;                                                                       \
    int fd = open(path, O_RDONLY);                                                       \
    if (fd < 0) { return NULL; }                                                         \
    struct stat st;                                                                      \
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 0x7fffffff)                \
    {                                                                                    \
        close(fd);                                                                       \
        return NULL;                                                                     \
    }                                                                                    \
    char* buf = (char*)GUI_MALLOC_T((size_t)st.st_size + 1, MM_TYPE_FS);                 \
    if (buf == NULL) { close(fd); return NULL; }                                         \
    ssize_t total = 0;                                                                   \
    while (total < st.st_size)                                                           \
    {                                                                                    \
        ssize_t n = read(fd, buf + total, (size_t)(st.st_size - total));                 \
        if (n <= 0) { GUI_FREE(buf); close(fd); return NULL; }                           \
        total += n;                                                                      \
    }                                                                                    \
    buf[total] = 0;                                                                      \
    close(fd);                                                                           \
    *out_size = (int64)total;                                                            \
    return buf;                                                                          \
}

#define FS_POSIX_DEFINE_MMAP()                                                           \
boole fs__map_file(char* path, fs_mapped_file* out)                                      \
{                                                                                        \
    if (path == NULL || out == NULL) { return FALSE; }                                   \
    memset(out, 0, sizeof(*out));                                                        \
    int fd = open(path, O_RDONLY);                                                       \
    if (fd < 0)                                                                          \
    {                                                                                    \
        log_error("fs__map_file[" FS_POSIX_PLATFORM_TAG "]: open('%s') failed (%s)",     \
                  path, strerror(errno));                                                \
        return FALSE;                                                                    \
    }                                                                                    \
    struct stat st;                                                                      \
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 0x7fffffff)               \
    {                                                                                    \
        close(fd);                                                                       \
        return FALSE;                                                                    \
    }                                                                                    \
    void* mapped = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);         \
    if (mapped == MAP_FAILED)                                                            \
    {                                                                                    \
        log_error("fs__map_file[" FS_POSIX_PLATFORM_TAG "]: mmap('%s') failed (%s)",     \
                  path, strerror(errno));                                                \
        close(fd);                                                                       \
        return FALSE;                                                                    \
    }                                                                                    \
    close(fd);                                                                           \
    out->data         = (const ubyte*)mapped;                                            \
    out->size         = (int64)st.st_size;                                               \
    out->_platform[0] = NULL;                                                            \
    out->_platform[1] = NULL;                                                            \
    out->_platform[2] = mapped;                                                          \
    return TRUE;                                                                         \
}                                                                                        \
                                                                                         \
void fs__unmap_file(fs_mapped_file* f)                                                   \
{                                                                                        \
    if (f == NULL || f->data == NULL || f->size <= 0) { return; }                        \
    munmap((void*)f->data, (size_t)f->size);                                             \
    f->data = NULL;                                                                      \
    f->size = 0;                                                                         \
    memset(f->_platform, 0, sizeof(f->_platform));                                       \
}

#endif
