#ifndef GUI_API_H
#define GUI_API_H

//
// gui_api.h -- visibility macros, mirrored from meek-ui.
//
// Meek-shell links meek-ui sources directly into a single ELF with
// no DLL boundary. GUI_API therefore collapses to visibility("default")
// on POSIX (harmless with -fvisibility=default) and dllexport on
// Windows (unused path here; meek-shell is Linux-only).
//
// UI_HANDLER must keep __attribute__((used)) so --gc-sections doesn't
// drop handler symbols that are only referenced at runtime via dlsym
// from the scene dispatcher. Without this, on_click="..." lookups
// would silently fail in release builds.
//

#if defined(_WIN32)
#define GUI_API __declspec(dllexport)
#define UI_HANDLER __declspec(dllexport)
#else
#define GUI_API __attribute__((visibility("default")))
#define UI_HANDLER __attribute__((visibility("default"), used))
#endif

#endif
