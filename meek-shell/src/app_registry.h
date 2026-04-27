#ifndef MEEK_SHELL_APP_REGISTRY_H
#define MEEK_SHELL_APP_REGISTRY_H

//
// app_registry.h - installed-application discovery + launch.
//
// Reads XDG .desktop files from the standard `applications/`
// directories at startup, builds an in-memory list of "Type=
// Application" entries the user can launch from meek-shell's
// launcher tile grid, and exposes one launch entry point that
// fork+execs the entry's Exec command under /bin/sh.
//
// Spec reference: freedesktop Desktop Entry Specification 1.5
// (https://specifications.freedesktop.org/desktop-entry-spec/).
// We honour the minimum subset relevant to a launcher: Name,
// Exec, Icon, Type, Hidden, NoDisplay, TryExec.
//
// Lifecycle:
//
//   app_registry__scan()  -- walk the directories once. Idempotent
//                            on repeat call (prior entries are
//                            replaced). Cheap, ~50-200 files on
//                            a typical phone.
//   app_registry__count() -- number of entries currently loaded.
//   app_registry__get(i)  -- read-only handle to entry i. Returns
//                            NULL if i is out of range.
//   app_registry__launch(i) -- fork + setsid + exec the i-th
//                            entry's Exec command via /bin/sh -c.
//                            Returns 0 on fork success, -1 on
//                            bad index or fork failure. The
//                            launched process inherits the
//                            shell's WAYLAND_DISPLAY +
//                            XDG_RUNTIME_DIR environment.
//
// Thread safety: not thread-safe. Single-threaded shell only.
//

#include <stdint.h>

#define APP_REGISTRY_MAX 128
#define APP_REGISTRY_NAME_LEN 96
#define APP_REGISTRY_EXEC_LEN 256
#define APP_REGISTRY_ICON_LEN 96

typedef struct app_entry
{
	char name[APP_REGISTRY_NAME_LEN]; // display name, from `Name=`.
	char exec[APP_REGISTRY_EXEC_LEN]; // command to launch, with %X format codes
	// stripped.
	char icon[APP_REGISTRY_ICON_LEN]; // icon name from `Icon=`. NOT a path; theme
	// resolution is a future pass.
} app_entry;

void app_registry__scan(void);
int app_registry__count(void);
const app_entry *app_registry__get(int index);
int app_registry__launch(int index);

#endif
