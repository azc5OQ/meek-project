//
// platforms/macos/fs_macos.c - macOS fs.h implementation.
//
// Body lives in ../_fs_posix.h (shared with fs_linux.c). macOS is a
// POSIX system for our purposes (open / read / mmap behave identically
// on XNU and Linux), so this TU is a thin shim around the shared
// macros plus the Android-only stubs that every non-Android platform
// must accept-and-ignore.
//

#define FS_POSIX_PLATFORM_TAG "macos"
#include "../_fs_posix.h"

void fs__set_asset_manager(void* mgr)
{
    //
    // Android-only bridge. macOS hosts read the real filesystem
    // directly. No-op here.
    //
    (void)mgr;
}

void fs__set_sideload_dir(const char* dir)
{
    //
    // Android-only adb-push hot-reload mechanism. No-op on macOS.
    //
    (void)dir;
}

FS_POSIX_DEFINE_SLURP()
FS_POSIX_DEFINE_MMAP()
