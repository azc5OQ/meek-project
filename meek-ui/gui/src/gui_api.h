#ifndef GUI_API_H
#define GUI_API_H

//
//gui_api.h - dll export / import macros.
//
//GUI_API     prefixes every public function declaration in the
//            library headers. when the library is being compiled
//            (CMake defines GUI_BUILDING_DLL), it expands to
//            __declspec(dllexport) so the function lands in
//            gui.dll's export table. when a host app includes the
//            same header, it expands to __declspec(dllimport) so
//            calls go through the import-address-table thunk.
//
//UI_HANDLER  prefixes event-handler functions defined in host apps.
//            it always expands to __declspec(dllexport) (even from
//            an .exe, which makes the symbol visible to
//            GetProcAddress(GetModuleHandleW(NULL), name)). the
//            library's symbol resolver uses that to look up
//            on_click="foo" handlers without explicit registration.
//

#if defined(_WIN32)
    #ifdef GUI_BUILDING_DLL
        #define GUI_API __declspec(dllexport)
    #else
        #define GUI_API __declspec(dllimport)
    #endif
    #define UI_HANDLER __declspec(dllexport)
#else
    //
    //posix (Android / future Linux / macOS) builds combine
    //-fvisibility=hidden at the command line with
    //__attribute__((visibility("default"))) on the exported symbols.
    //
    //UI_HANDLER additionally carries __attribute__((used)) so the
    //linker's --gc-sections pass can't collect the handler as
    //"unused" just because every reference to it is by-name at
    //runtime (scene's dispatcher calls dlsym(RTLD_DEFAULT, "foo")
    //to find it). Without `used`, a release build would silently
    //drop all UI handlers and every on_click= / on_change= ref
    //in the .ui would log "handler not found".
    //
    #define GUI_API     __attribute__((visibility("default")))
    #define UI_HANDLER  __attribute__((visibility("default"), used))
#endif

#endif
