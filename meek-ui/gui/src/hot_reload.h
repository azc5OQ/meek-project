#ifndef HOT_RELOAD_H
#define HOT_RELOAD_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
//hot_reload.h - poll-based hot reload for .ui and .style files.
//paired with hot_reload.c.
//
//mechanism: each watched file has its content hashed (fnv-1a 32-bit
//over the bytes) on every tick. when the hash changes, the file is
//re-read and re-applied:
//
//  .ui files    -> parser_xml__load_ui produces a new tree;
//                  the previous tree is freed and the new one
//                  becomes the scene root.
//  .style files -> scene__clear_styles wipes all registered styles
//                  and parser_style__load_styles re-registers from
//                  the file.
//
//file-open / parse failures are silent so editors that briefly hold
//the file open for write don't spam the log.
//
//the polling implementation is the fallback. ReadDirectoryChangesW +
//IOCP would replace it without changing this header.
//
//multiple watches are supported (one .ui + one .style is the typical
//host-app case, but the table holds up to 8 watches total).
//

/**
 * Begin watching a .ui file. Performs the initial load synchronously,
 * sets the parsed tree as the scene root, and stores its content hash.
 *
 * @function hot_reload__watch_ui
 * @param {char*} path - Absolute path to the .ui file.
 * @return {boole} TRUE if the initial load succeeded; FALSE otherwise.
 */
GUI_API boole hot_reload__watch_ui(char* path);

/**
 * Begin watching a .style file. Performs the initial load + style
 * registration synchronously and stores the file's content hash.
 *
 * @function hot_reload__watch_style
 * @param {char*} path - Absolute path to the .style file.
 * @return {boole} TRUE if the initial load succeeded; FALSE otherwise.
 *
 * Subsequent reloads call scene__clear_styles before reparsing, so
 * removing a rule from the file actually un-registers it.
 */
GUI_API boole hot_reload__watch_style(char* path);

/**
 * Poll every watched file once. For each file whose content hash
 * differs from the previous tick, run the appropriate reload. Cheap
 * when nothing changed; safe to call every frame.
 *
 * Returns the number of reloads that landed during this tick (0 on
 * a quiet tick, >=1 if any .ui or .style was reparsed). Hosts whose
 * platform backend uses render-gating (see Phase 2 in
 * platform_linux_wayland_client.c) use the return to call
 * platform_wayland__request_render() so the reloaded content
 * becomes visible without waiting for the next input event.
 *
 * Existing call sites that don't care about the count can ignore
 * the return -- it's a strict superset of the previous void
 * signature.
 *
 * @function hot_reload__tick
 * @return {int} Count of reloads landed this tick (0 = nothing changed).
 *
 * Transient open / parse failures are silently swallowed.
 */
GUI_API int hot_reload__tick(void);

/**
 * Free any owned trees (from .ui watches) and clear the watch table.
 *
 * @function hot_reload__shutdown
 * @return {void}
 */
GUI_API void hot_reload__shutdown(void);

#endif
