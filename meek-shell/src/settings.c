//
// settings.c - implementation of meek-shell's runtime preferences
// + plain-text persistence.
//
// Persistence layer is modelled on the per-type save/load helper
// pattern from the user's reference settings module:
//
//   _save_bool / _save_int / _save_float / _save_char append a
//   "<name> <value>\n" line to a buffer.
//
//   _load_bool / _load_int / _load_float / _load_char walk every
//   line in a file-static buffer, split on the first space, and
//   return the parsed value when the name matches. Each lookup is
//   O(N_lines); fine for ~tens of settings, would need an index
//   if the file ever grew to thousands of entries.
//
// Differences vs the reference:
//   - POSIX I/O (fopen / fread / fwrite / fclose) instead of
//     CreateFileA. meek-shell is Linux-only.
//   - log.h instead of console__write.
//   - stdlib__ wrappers from meek-ui's clib for memcpy / memset /
//     strlen / strcmp / atoi. Float parsing falls back to libc
//     atof because no stdlib__atof wrapper exists yet.
//   - LF line endings instead of CRLF (Linux convention).
//   - No xor-obfuscated filename string (no anti-cheat threat
//     model in this project).
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> //getenv / atof
#include <string.h> //fallback for one helper missing from stdlib__

#include "clib/stdlib.h" //stdlib__memcpy / __memset / __strlen / __strcmp / __strncmp / __strtol
#include "settings.h"
#include "third_party/log.h"

//
// ===== defaults + ranges ==================================================
//

#define _SETTINGS_INTERNAL__DEFAULT_PREVIEW_MAX_FPS 0
#define _SETTINGS_INTERNAL__DEFAULT_ACTIVITY_WINDOW_MS 10000
#define _SETTINGS_INTERNAL__DEFAULT_IDLE_POLL_MS 250
#define _SETTINGS_INTERNAL__DEFAULT_RENDER_GATING_ENABLED 1
#define _SETTINGS_INTERNAL__DEFAULT_OPAQUE_ICON_TILES     1

#define _SETTINGS_INTERNAL__PREVIEW_MAX_FPS_MIN 0
#define _SETTINGS_INTERNAL__PREVIEW_MAX_FPS_MAX 240
#define _SETTINGS_INTERNAL__ACTIVITY_WINDOW_MS_MIN 0
#define _SETTINGS_INTERNAL__ACTIVITY_WINDOW_MS_MAX 600000
#define _SETTINGS_INTERNAL__IDLE_POLL_MS_MIN 16
#define _SETTINGS_INTERNAL__IDLE_POLL_MS_MAX 60000

//
// Save buffer is allocated on the stack; must be large enough for
// every setting line plus a safety margin. 8 KiB covers ~200
// settings at ~40 chars each which is well above what we'll ever
// have. Grow if a future struct field needs a long string value.
//
#define _SETTINGS_INTERNAL__SAVE_BUFFER_BYTES 8192

//
// Hard cap on the size of a settings file we'll read. Prevents an
// accidental gigabyte-sized file from exhausting heap. 1 MiB is
// 25 000+ short lines.
//
#define _SETTINGS_INTERNAL__MAX_LOAD_FILE_BYTES (1024 * 1024)

//
// ===== forward decls ======================================================
//

static const char *_settings_internal__resolve_path(void);
static int64 _settings_internal__clamp_i32(int64 v, int64 lo, int64 hi, const char *name);

static void _settings_internal__save_bool(const char *name, int64 value, char *buffer, int64 buffer_cap, int64 *cursor);
static void _settings_internal__save_int(const char *name, int64 value, char *buffer, int64 buffer_cap, int64 *cursor);
static void _settings_internal__save_float(const char *name, float value, char *buffer, int64 buffer_cap, int64 *cursor);
static void _settings_internal__save_char(const char *name, char value, char *buffer, int64 buffer_cap, int64 *cursor);

static int64 _settings_internal__load_bool(const char *name, int64 default_value);
static int64 _settings_internal__load_int(const char *name, int64 default_value);
static float _settings_internal__load_float(const char *name, float default_value);
static char _settings_internal__load_char(const char *name, char default_value);

static void _settings_internal__write_save_payload(char *buffer, int64 buffer_cap, int64 *cursor);
static void _settings_internal__apply_loaded_values(void);

//
// ===== singleton state ====================================================
//

static meek_shell_settings _settings_internal__current = {
	.preview_max_fps = _SETTINGS_INTERNAL__DEFAULT_PREVIEW_MAX_FPS,
	.activity_window_ms = _SETTINGS_INTERNAL__DEFAULT_ACTIVITY_WINDOW_MS,
	.idle_poll_ms = _SETTINGS_INTERNAL__DEFAULT_IDLE_POLL_MS,
	.render_gating_enabled = _SETTINGS_INTERNAL__DEFAULT_RENDER_GATING_ENABLED,
	.opaque_icon_tiles = _SETTINGS_INTERNAL__DEFAULT_OPAQUE_ICON_TILES,
};

//
// File-buffer state used by the per-type load helpers. Populated
// by meek_shell_settings__load() before it calls them; cleared
// after the load batch completes. Module-static so the per-type
// helpers don't need to take it as a parameter.
//
static char *_settings_internal__loaded_buffer = NULL;
static int64 _settings_internal__loaded_size = 0;

//
// ===== public api =========================================================
//

const meek_shell_settings *meek_shell_settings__get(void)
{
	return &_settings_internal__current;
}

void meek_shell_settings__reset(void)
{
	_settings_internal__current.preview_max_fps = _SETTINGS_INTERNAL__DEFAULT_PREVIEW_MAX_FPS;
	_settings_internal__current.activity_window_ms = _SETTINGS_INTERNAL__DEFAULT_ACTIVITY_WINDOW_MS;
	_settings_internal__current.idle_poll_ms = _SETTINGS_INTERNAL__DEFAULT_IDLE_POLL_MS;
	_settings_internal__current.render_gating_enabled = _SETTINGS_INTERNAL__DEFAULT_RENDER_GATING_ENABLED;
	_settings_internal__current.opaque_icon_tiles = _SETTINGS_INTERNAL__DEFAULT_OPAQUE_ICON_TILES;
}

void meek_shell_settings__init(void)
{
	meek_shell_settings__reset();
	log_info("settings: initialised (preview_max_fps=%d activity_window_ms=%d "
			 "idle_poll_ms=%d render_gating_enabled=%d)",
		(int)_settings_internal__current.preview_max_fps, (int)_settings_internal__current.activity_window_ms, (int)_settings_internal__current.idle_poll_ms, (int)_settings_internal__current.render_gating_enabled);
}

void meek_shell_settings__load(void)
{
	const char *path = _settings_internal__resolve_path();

	FILE *f = fopen(path, "rb");
	if (f == NULL)
	{
		log_info("settings: load skipped, %s does not exist (using defaults)", path);
		return;
	}

	if (fseek(f, 0, SEEK_END) != 0)
	{
		log_warn("settings: fseek(%s, END) failed; skipping load", path);
		fclose(f);
		return;
	}
	long size = ftell(f);
	if (fseek(f, 0, SEEK_SET) != 0)
	{
		log_warn("settings: fseek(%s, SET) failed; skipping load", path);
		fclose(f);
		return;
	}
	if (size <= 0)
	{
		log_info("settings: %s is empty; using defaults", path);
		fclose(f);
		return;
	}
	if (size > _SETTINGS_INTERNAL__MAX_LOAD_FILE_BYTES)
	{
		log_warn("settings: %s too large (%ld bytes > cap %d); refusing to load", path, size, _SETTINGS_INTERNAL__MAX_LOAD_FILE_BYTES);
		fclose(f);
		return;
	}

	char *buffer = (char *)malloc((size_t)size + 1);
	if (buffer == NULL)
	{
		log_error("settings: out of memory allocating %ld byte load buffer", size + 1);
		fclose(f);
		return;
	}

	size_t read_n = fread(buffer, 1, (size_t)size, f);
	fclose(f);
	if ((long)read_n != size)
	{
		log_warn("settings: short read on %s (got %zu of %ld bytes); using what we got", path, read_n, size);
	}
	buffer[read_n] = 0;

	_settings_internal__loaded_buffer = buffer;
	_settings_internal__loaded_size = (int64)read_n;

	_settings_internal__apply_loaded_values();

	free(buffer);
	_settings_internal__loaded_buffer = NULL;
	_settings_internal__loaded_size = 0;

	log_info("settings: loaded %s (%zu bytes) -> preview_max_fps=%d "
			 "activity_window_ms=%d idle_poll_ms=%d render_gating_enabled=%d",
		path, read_n, (int)_settings_internal__current.preview_max_fps, (int)_settings_internal__current.activity_window_ms, (int)_settings_internal__current.idle_poll_ms, (int)_settings_internal__current.render_gating_enabled);
}

void meek_shell_settings__save(void)
{
	const char *path = _settings_internal__resolve_path();

	char buffer[_SETTINGS_INTERNAL__SAVE_BUFFER_BYTES];
	int64 cursor = 0;
	stdlib__memset(buffer, 0, sizeof(buffer));

	_settings_internal__write_save_payload(buffer, (int64)sizeof(buffer), &cursor);

	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		log_warn("settings: fopen(%s, w) failed; nothing saved", path);
		return;
	}
	size_t written = fwrite(buffer, 1, (size_t)cursor, f);
	fclose(f);
	if ((int64)written != cursor)
	{
		log_warn("settings: short write on %s (wrote %zu of %d bytes)", path, written, (int)cursor);
		return;
	}
	log_info("settings: saved %d bytes to %s", (int)cursor, path);
}

void meek_shell_settings__set_preview_max_fps(int64 fps)
{
	_settings_internal__current.preview_max_fps = _settings_internal__clamp_i32(fps, _SETTINGS_INTERNAL__PREVIEW_MAX_FPS_MIN, _SETTINGS_INTERNAL__PREVIEW_MAX_FPS_MAX, "preview_max_fps");
}

void meek_shell_settings__set_activity_window_ms(int64 ms)
{
	_settings_internal__current.activity_window_ms = _settings_internal__clamp_i32(ms, _SETTINGS_INTERNAL__ACTIVITY_WINDOW_MS_MIN, _SETTINGS_INTERNAL__ACTIVITY_WINDOW_MS_MAX, "activity_window_ms");
}

void meek_shell_settings__set_idle_poll_ms(int64 ms)
{
	_settings_internal__current.idle_poll_ms = _settings_internal__clamp_i32(ms, _SETTINGS_INTERNAL__IDLE_POLL_MS_MIN, _SETTINGS_INTERNAL__IDLE_POLL_MS_MAX, "idle_poll_ms");
}

void meek_shell_settings__set_render_gating_enabled(int64 on)
{
	_settings_internal__current.render_gating_enabled = (on != 0) ? 1 : 0;
}

void meek_shell_settings__set_opaque_icon_tiles(boole on)
{
	_settings_internal__current.opaque_icon_tiles = (on != 0) ? (boole)1 : (boole)0;
}

//
// ===== save / load batch (one entry per struct field) =====================
//

//
// The two batch functions list every persisted field once. Adding
// a new setting requires touching three call sites: the struct
// (settings.h), the writer here, and the reader below. That's
// deliberate -- the symmetry keeps load/save in lock-step and a
// missed entry in either direction surfaces immediately as a
// "value not persisted" / "default never overridden" complaint
// from the user.
//

static void _settings_internal__write_save_payload(char *buffer, int64 buffer_cap, int64 *cursor)
{
	_settings_internal__save_int("preview_max_fps", _settings_internal__current.preview_max_fps, buffer, buffer_cap, cursor);
	_settings_internal__save_int("activity_window_ms", _settings_internal__current.activity_window_ms, buffer, buffer_cap, cursor);
	_settings_internal__save_int("idle_poll_ms", _settings_internal__current.idle_poll_ms, buffer, buffer_cap, cursor);
	_settings_internal__save_int("render_gating_enabled", _settings_internal__current.render_gating_enabled, buffer, buffer_cap, cursor);
	_settings_internal__save_int("opaque_icon_tiles", _settings_internal__current.opaque_icon_tiles, buffer, buffer_cap, cursor);
}

static void _settings_internal__apply_loaded_values(void)
{
	//
	// Apply through the public setters so range clamping + the
	// out-of-range warn lines fire on hand-edited files. Default
	// value passed to each loader is the CURRENT value (set by
	// __init), so a setting absent from the file keeps its
	// default.
	//
	meek_shell_settings__set_preview_max_fps(_settings_internal__load_int("preview_max_fps", _settings_internal__current.preview_max_fps));
	meek_shell_settings__set_activity_window_ms(_settings_internal__load_int("activity_window_ms", _settings_internal__current.activity_window_ms));
	meek_shell_settings__set_idle_poll_ms(_settings_internal__load_int("idle_poll_ms", _settings_internal__current.idle_poll_ms));
	meek_shell_settings__set_render_gating_enabled(_settings_internal__load_bool("render_gating_enabled", _settings_internal__current.render_gating_enabled));
	meek_shell_settings__set_opaque_icon_tiles((boole)_settings_internal__load_bool("opaque_icon_tiles", _settings_internal__current.opaque_icon_tiles));
}

//
// ===== per-type save helpers ==============================================
//
// Each helper writes one line of "<name> <value>\n" to (buffer +
// *cursor), advancing *cursor past the bytes written. Bounds-check
// against buffer_cap; on overflow log + skip the field.
//

static void _settings_internal__save_bool(const char *name, int64 value, char *buffer, int64 buffer_cap, int64 *cursor)
{
	int64 remaining = buffer_cap - *cursor;
	if (remaining <= 0)
	{
		log_warn("settings: save buffer full, skipping bool %s", name);
		return;
	}
	int n = snprintf(buffer + *cursor, (size_t)remaining, "%s %d\n", name, value ? 1 : 0);
	if (n < 0 || n >= remaining)
	{
		log_warn("settings: save buffer overflow on bool %s (need %d, have %d)", name, n, remaining);
		return;
	}
	*cursor += n;
}

static void _settings_internal__save_int(const char *name, int64 value, char *buffer, int64 buffer_cap, int64 *cursor)
{
	int64 remaining = buffer_cap - *cursor;
	if (remaining <= 0)
	{
		log_warn("settings: save buffer full, skipping int %s", name);
		return;
	}
	int n = snprintf(buffer + *cursor, (size_t)remaining, "%s %d\n", name, (int)value);
	if (n < 0 || n >= remaining)
	{
		log_warn("settings: save buffer overflow on int %s (need %d, have %d)", name, n, remaining);
		return;
	}
	*cursor += n;
}

static void _settings_internal__save_float(const char *name, float value, char *buffer, int64 buffer_cap, int64 *cursor)
{
	int64 remaining = buffer_cap - *cursor;
	if (remaining <= 0)
	{
		log_warn("settings: save buffer full, skipping float %s", name);
		return;
	}
	//
	// %g picks the shorter of fixed / scientific representation;
	// on a settings file readable by humans, %g is the friendliest
	// default. 7 significant digits is plenty for float-precision
	// round trips.
	//
	int n = snprintf(buffer + *cursor, (size_t)remaining, "%s %.7g\n", name, value);
	if (n < 0 || n >= remaining)
	{
		log_warn("settings: save buffer overflow on float %s (need %d, have %d)", name, n, remaining);
		return;
	}
	*cursor += n;
}

static void _settings_internal__save_char(const char *name, char value, char *buffer, int64 buffer_cap, int64 *cursor)
{
	int64 remaining = buffer_cap - *cursor;
	if (remaining <= 0)
	{
		log_warn("settings: save buffer full, skipping char %s", name);
		return;
	}
	//
	// Char is stored literally as one ASCII byte after the space.
	// Non-printable values write through anyway -- the loader
	// takes whatever byte is at that position. Use case is single-
	// key bindings stored as ASCII codes.
	//
	int n = snprintf(buffer + *cursor, (size_t)remaining, "%s %c\n", name, value);
	if (n < 0 || n >= remaining)
	{
		log_warn("settings: save buffer overflow on char %s (need %d, have %d)", name, n, remaining);
		return;
	}
	*cursor += n;
}

//
// ===== per-type load helpers ==============================================
//
// Walk every line of _settings_internal__loaded_buffer, split each
// line on the first space, compare the first half to `name`. On
// match, parse the second half into the target type and return it.
// On no-match-anywhere or empty buffer, return default_value.
//
// Lines without a space are skipped silently (covers blank lines +
// comments-without-space). Lines with a name longer than the
// internal scratch buffer are also skipped.
//

#define _SETTINGS_INTERNAL__SCRATCH_BYTES 256

static int _settings_internal__find_match(const char *name, char *out_value, int64 out_value_cap)
{
	char *buf = _settings_internal__loaded_buffer;
	int64 size = _settings_internal__loaded_size;
	if (buf == NULL || size <= 0)
	{
		return 0;
	}

	int64_t name_len = stdlib__strlen(name);

	int64 line_start = 0;
	for (int64 i = 0; i <= size; i++)
	{
		int at_eof = (i == size);
		int at_eol = !at_eof && buf[i] == '\n';
		if (!at_eof && !at_eol)
		{
			continue;
		}

		int64 line_end = i;
		//
		// Strip a trailing CR so files saved on a Windows tool
		// with CRLF line endings still parse cleanly. The save
		// path always emits LF; this is purely defensive.
		//
		if (line_end > line_start && buf[line_end - 1] == '\r')
		{
			line_end--;
		}

		int64 line_len = line_end - line_start;
		if (line_len <= 0)
		{
			line_start = i + 1;
			continue;
		}

		//
		// Find the first space; everything before is the name,
		// everything after up to line_end is the value text.
		//
		int64 space_at = -1;
		for (int64 j = line_start; j < line_end; j++)
		{
			if (buf[j] == ' ')
			{
				space_at = j;
				break;
			}
		}
		if (space_at < 0)
		{
			line_start = i + 1;
			continue;
		}

		int64 cur_name_len = space_at - line_start;
		if ((int64_t)cur_name_len != name_len)
		{
			line_start = i + 1;
			continue;
		}
		if (stdlib__strncmp(buf + line_start, name, name_len) != 0)
		{
			line_start = i + 1;
			continue;
		}

		int64 value_len = line_end - (space_at + 1);
		if (value_len <= 0 || value_len >= out_value_cap)
		{
			log_warn("settings: %s value too long (%d bytes); using default", name, (int)value_len);
			return 0;
		}
		stdlib__memcpy(out_value, buf + space_at + 1, (int64_t)value_len);
		out_value[value_len] = 0;
		return 1;
	}
	return 0;
}

static int64 _settings_internal__load_bool(const char *name, int64 default_value)
{
	char value[_SETTINGS_INTERNAL__SCRATCH_BYTES];
	if (!_settings_internal__find_match(name, value, sizeof(value)))
	{
		return default_value;
	}
	//
	// "1" / "true" / "yes" all read as TRUE; "0" / "false" / "no"
	// / anything else as FALSE. Generous parsing keeps hand-edited
	// files forgiving.
	//
	if (stdlib__strcmp(value, "1") == 0)
	{
		return 1;
	}
	if (stdlib__strcmp(value, "true") == 0)
	{
		return 1;
	}
	if (stdlib__strcmp(value, "yes") == 0)
	{
		return 1;
	}
	if (stdlib__strcmp(value, "0") == 0)
	{
		return 0;
	}
	if (stdlib__strcmp(value, "false") == 0)
	{
		return 0;
	}
	if (stdlib__strcmp(value, "no") == 0)
	{
		return 0;
	}
	log_warn("settings: %s='%s' is not a recognised bool; using default %d", name, value, (int)default_value);
	return default_value;
}

static int64 _settings_internal__load_int(const char *name, int64 default_value)
{
	char value[_SETTINGS_INTERNAL__SCRATCH_BYTES];
	if (!_settings_internal__find_match(name, value, sizeof(value)))
	{
		return default_value;
	}
	return (int64)stdlib__strtol(value, NULL, 10);
}

static float _settings_internal__load_float(const char *name, float default_value)
{
	char value[_SETTINGS_INTERNAL__SCRATCH_BYTES];
	if (!_settings_internal__find_match(name, value, sizeof(value)))
	{
		return default_value;
	}
	//
	// No stdlib__atof wrapper exists yet; libc atof is fine here
	// since meek-shell already links libc.
	//
	return (float)atof(value);
}

static char _settings_internal__load_char(const char *name, char default_value)
{
	char value[_SETTINGS_INTERNAL__SCRATCH_BYTES];
	if (!_settings_internal__find_match(name, value, sizeof(value)))
	{
		return default_value;
	}
	if (value[0] == 0)
	{
		return default_value;
	}
	return value[0];
}

//
// ===== misc ===============================================================
//

static const char *_settings_internal__resolve_path(void)
{
	const char *env = getenv("MEEK_SHELL_SETTINGS_PATH");
	if (env != NULL && env[0] != 0)
	{
		return env;
	}
	return "meek_shell_settings.cfg";
}

static int64 _settings_internal__clamp_i32(int64 v, int64 lo, int64 hi, const char *name)
{
	if (v < lo)
	{
		log_warn("settings: %s=%d below min %d; clamping", name, (int)v, (int)lo);
		return lo;
	}
	if (v > hi)
	{
		log_warn("settings: %s=%d above max %d; clamping", name, (int)v, (int)hi);
		return hi;
	}
	return v;
}

//
// _save_bool / _save_float / _save_char and _load_float / _load_char
// have no current call site. They're compiled in so future
// settings of those types only require additions to the struct +
// a line each in __write_save_payload + __apply_loaded_values.
// build.sh sets -Wno-unused-function; nothing else needed to keep
// them in the binary.
//
