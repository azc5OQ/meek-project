//
// platforms/linux/fs_linux.c - Linux fs.h implementation.
//
// Body lives in ../_fs_posix.h (shared with fs_macos.c). This TU
// owns the platform-specific stubs (set_asset_manager + set_sideload_dir,
// both no-ops on Linux -- those are Android-only bridges) and emits
// the slurp + mmap implementations via the FS_POSIX_DEFINE_* macros.
//

#define FS_POSIX_PLATFORM_TAG "linux"
#include "../_fs_posix.h"

void fs__set_asset_manager(void* mgr)
{
    //
    // Android-only bridge. Linux hosts read the real filesystem
    // directly. Accepted silently so host init code stays
    // platform-neutral.
    //
    (void)mgr;
}

void fs__set_sideload_dir(const char* dir)
{
    //
    // Android-only override mechanism for adb-push hot reload.
    // Same accept-and-ignore pattern as fs__set_asset_manager.
    //
    (void)dir;
}

FS_POSIX_DEFINE_SLURP()
FS_POSIX_DEFINE_MMAP()
