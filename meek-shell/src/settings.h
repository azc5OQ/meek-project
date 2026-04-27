#ifndef MEEK_SHELL_SETTINGS_H
#define MEEK_SHELL_SETTINGS_H

//
// settings.h - runtime-mutable shell preferences with on-disk
// persistence.
//
// Lifecycle:
//
//   meek_shell_settings__init()     -- set defaults. Call once,
//                                       BEFORE any code reads from
//                                       the struct.
//   meek_shell_settings__load()     -- override defaults from file.
//                                       Safe to call when the file
//                                       doesn't exist (no-op + log
//                                       line).
//   meek_shell_settings__save()     -- persist current values to
//                                       file. Call when the user
//                                       confirms a change in the
//                                       (future) settings UI, or on
//                                       shutdown.
//   meek_shell_settings__get()      -- read-only accessor. Pointer
//                                       is stable for the process
//                                       lifetime.
//
// File format: plain text, one line per setting:
//
//   <variable_name> <value>\n
//
// Unknown lines + lines without a space are skipped at load. Per-
// field setters clamp out-of-range values + log a warning.
//
// Storage location: $MEEK_SHELL_SETTINGS_PATH if set, otherwise
// "meek_shell_settings.cfg" in the working directory.
//

#include "types.h"  // int64, uint, boole.

typedef struct meek_shell_settings
{
	//
	// Cap on how often a foreign app's buffer commit wakes the
	// render gate when its tile is visible. The texture cache is
	// still updated on every commit so a tile that wasn't woken
	// still shows the latest pixels the moment a render happens
	// for some other reason.
	//
	//   0  -> no rate limit. Every commit on a visible handle
	//         wakes the gate. Equivalent to pre-setting behaviour.
	//   1+ -> at most N wakes per second per handle. Useful when
	//         several foreign apps commit at vblank rate and you
	//         want the shell to idle even though their tiles are
	//         technically visible.
	//
	int64 preview_max_fps;

	//
	// Phase 2 idle-gate tunables. Defaults match the constants
	// hard-coded in platform_linux_wayland_client.c
	// (ACTIVITY_TIMEOUT_MS / IDLE_TIMEOUT_MS).
	//
	int64 activity_window_ms;
	int64 idle_poll_ms;

	//
	// Phase 2 master kill switch. 0 = gating disabled, 1 = on.
	// MEEK_RENDER_GATING env var still wins at startup.
	//
	int64 render_gating_enabled;

	//
	// Launcher-tile fill mode for app icons.
	//
	//   1 -> Opaque tile behind every PNG icon. Color is sampled
	//        from the icon's pixels by icon_color_sampler so each
	//        app gets a frame whose hue matches its own art --
	//        cheap stand-in for the pre-baked tile colors that
	//        commercial app stores require designers to ship.
	//        Default. Looks like a modern phone home screen.
	//
	//   0 -> Transparent icons. No frame; icons float on the
	//        wallpaper. Lighter visual weight; relies on the
	//        wallpaper having enough contrast under every icon.
	//
	boole opaque_icon_tiles;

} meek_shell_settings;

//
// Read-only accessor. Returned pointer is to a file-static struct;
// stable for the process lifetime; do NOT cache field values
// across event-loop boundaries because the load/setter layer may
// rewrite them between ticks.
//
const meek_shell_settings *meek_shell_settings__get(void);

//
// One-time init. Sets defaults; idempotent.
//
void meek_shell_settings__init(void);

//
// Reset every field to compile-time defaults. Idempotent.
//
void meek_shell_settings__reset(void);

//
// Load persisted values from disk and apply via the per-field
// setters (so range-clamping + logging still happens). No-op +
// log line when the file doesn't exist; that's the first-run path.
//
void meek_shell_settings__load(void);

//
// Persist the current struct contents to disk. Overwrites the
// file. Logs at info on success, warn on I/O failure.
//
void meek_shell_settings__save(void);

//
// Per-field setters. Each clamps to a documented range and logs
// at warn level on out-of-range input.
//
void meek_shell_settings__set_preview_max_fps(int64 fps);
void meek_shell_settings__set_activity_window_ms(int64 ms);
void meek_shell_settings__set_idle_poll_ms(int64 ms);
void meek_shell_settings__set_render_gating_enabled(int64 on);
void meek_shell_settings__set_opaque_icon_tiles(boole on);

#endif
