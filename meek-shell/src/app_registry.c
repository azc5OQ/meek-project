//
// app_registry.c - implementation of installed-app discovery +
// launch. See header for scope + lifecycle.
//
// Scan path:
//   1. /usr/share/applications/
//   2. /usr/local/share/applications/
//   3. $XDG_DATA_HOME/applications/  (default:
//   $HOME/.local/share/applications/)
//
// Per .desktop file we read the [Desktop Entry] section, capture
// Name / Exec / Icon / Type / Hidden / NoDisplay, and emit an
// entry only when Type=Application AND !Hidden AND !NoDisplay.
// Duplicate basenames in later directories override earlier ones
// (XDG override semantics).
//
// Exec field handling: the spec defines %f / %u / %F / %U / %i /
// %c / %k / %% format codes. The first six expand at launch time
// to file/url args meek-shell isn't going to provide, so we strip
// them. %% becomes a single literal %. The remaining string is
// passed to /bin/sh -c so shell quoting in the .desktop file
// continues to work.
//
// We do NOT strip quoted-arg structure. /bin/sh handles that.
//
// References consulted:
//   freedesktop Desktop Entry Specification 1.5
//

#include <dirent.h> //opendir / readdir / closedir
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> //getenv / malloc / free
#include <string.h> //legacy ascii fallback for one-shot helpers
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h> //fork / setsid / execl / _exit

#include "app_registry.h"
#include "clib/stdlib.h" //stdlib__memcpy / __memset / __strlen / __strcmp / __strncmp
#include "third_party/log.h"

//
// ===== forward decls ======================================================
//

static void _app_registry_internal__scan_dir(const char *dir);
static void _app_registry_internal__parse_desktop_file(const char *path);
static int _app_registry_internal__ends_with(const char *s, const char *suffix);
static void _app_registry_internal__trim(char *s);
static void _app_registry_internal__strip_exec_codes(const char *in, char *out, int out_cap);
static int _app_registry_internal__exists_by_name(const char *name);
static void _app_registry_internal__sort_alpha(void);

//
// ===== module state =======================================================
//

static app_entry _app_registry_internal__entries[APP_REGISTRY_MAX];
static int _app_registry_internal__count = 0;

//
// ===== public api =========================================================
//

void app_registry__scan(void)
{
	_app_registry_internal__count = 0;
	stdlib__memset(_app_registry_internal__entries, 0, sizeof(_app_registry_internal__entries));

	_app_registry_internal__scan_dir("/usr/share/applications");
	_app_registry_internal__scan_dir("/usr/local/share/applications");

	//
	// $XDG_DATA_HOME, with $HOME/.local/share fallback, per the
	// XDG Base Directory Specification.
	//
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	char user_dir[512];
	if (xdg_data_home != NULL && xdg_data_home[0] != 0)
	{
		snprintf(user_dir, sizeof(user_dir), "%s/applications", xdg_data_home);
	}
	else
	{
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == 0)
		{
			home = "/home/user";
		}
		snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications", home);
	}
	_app_registry_internal__scan_dir(user_dir);

	_app_registry_internal__sort_alpha();

	log_info("app_registry: scanned %d application(s)", _app_registry_internal__count);
	for (int i = 0; i < _app_registry_internal__count; i++)
	{
		log_trace("app_registry: [%d] name='%s' exec='%s' icon='%s'", i, _app_registry_internal__entries[i].name, _app_registry_internal__entries[i].exec, _app_registry_internal__entries[i].icon);
	}
}

int app_registry__count(void)
{
	return _app_registry_internal__count;
}

const app_entry *app_registry__get(int index)
{
	if (index < 0 || index >= _app_registry_internal__count)
	{
		return NULL;
	}
	return &_app_registry_internal__entries[index];
}

int app_registry__launch(int index)
{
	const app_entry *e = app_registry__get(index);
	if (e == NULL)
	{
		log_warn("app_registry: launch idx=%d -> out of range", index);
		return -1;
	}
	if (e->exec[0] == 0)
	{
		log_warn("app_registry: launch idx=%d name='%s' -> empty Exec", index, e->name);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0)
	{
		log_error("app_registry: fork() failed for '%s'", e->name);
		return -1;
	}
	if (pid == 0)
	{
		//
		// Child. setsid detaches from the parent's controlling
		// terminal + process group so a shell crash doesn't take
		// the launched apps with it. /bin/sh -c handles the
		// .desktop file's quoting correctly without us reimplementing
		// shell tokenisation.
		//
		// The child inherits WAYLAND_DISPLAY + XDG_RUNTIME_DIR
		// from the shell's environment, so the launched app
		// connects to the same compositor automatically.
		//
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", e->exec, (char *)NULL);
		//
		// execl only returns on failure. Use _exit (not exit) so
		// we don't run the parent's atexit handlers from the
		// child.
		//
		_exit(127);
	}

	log_info("app_registry: launched '%s' (pid=%d) cmd='%s'", e->name, (int)pid, e->exec);
	return 0;
}

//
// ===== scan helpers =======================================================
//

static void _app_registry_internal__scan_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (d == NULL)
	{
		//
		// Missing dir is normal (user might not have a per-user
		// applications dir). Log at trace, not warn.
		//
		log_trace("app_registry: dir '%s' not openable; skipping", dir);
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL)
	{
		const char *name = ent->d_name;
		if (name[0] == '.')
		{
			continue;
		}
		if (!_app_registry_internal__ends_with(name, ".desktop"))
		{
			continue;
		}

		char full[512];
		int n = snprintf(full, sizeof(full), "%s/%s", dir, name);
		if (n <= 0 || n >= (int)sizeof(full))
		{
			continue;
		}

		_app_registry_internal__parse_desktop_file(full);
	}

	closedir(d);
}

static void _app_registry_internal__parse_desktop_file(const char *path)
{
	if (_app_registry_internal__count >= APP_REGISTRY_MAX)
	{
		log_warn("app_registry: cap reached (%d); dropping '%s'", APP_REGISTRY_MAX, path);
		return;
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL)
	{
		log_trace("app_registry: open failed '%s'", path);
		return;
	}

	char name[APP_REGISTRY_NAME_LEN];
	name[0] = 0;
	char exec[APP_REGISTRY_EXEC_LEN];
	exec[0] = 0;
	char icon[APP_REGISTRY_ICON_LEN];
	icon[0] = 0;
	char type[32];
	type[0] = 0;
	int hidden = 0;
	int no_display = 0;
	int in_desktop_entry = 0;

	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL)
	{
		_app_registry_internal__trim(line);
		if (line[0] == 0 || line[0] == '#')
		{
			continue;
		}

		if (line[0] == '[')
		{
			in_desktop_entry = (stdlib__strcmp(line, "[Desktop Entry]") == 0);
			continue;
		}
		if (!in_desktop_entry)
		{
			continue;
		}

		char *eq = strchr(line, '=');
		if (eq == NULL)
		{
			continue;
		}
		*eq = 0;
		char *key = line;
		char *val = eq + 1;
		_app_registry_internal__trim(key);
		_app_registry_internal__trim(val);

		//
		// Localised entries (Name[en_US], etc.) are spec-correct
		// but we ignore them in v1 -- the unlocalised fallback
		// Name= is always present for spec-conforming files.
		// The bracket-suffix lines never match these strcmps.
		//
		if (stdlib__strcmp(key, "Name") == 0)
		{
			snprintf(name, sizeof(name), "%s", val);
		}
		else if (stdlib__strcmp(key, "Exec") == 0)
		{
			snprintf(exec, sizeof(exec), "%s", val);
		}
		else if (stdlib__strcmp(key, "Icon") == 0)
		{
			snprintf(icon, sizeof(icon), "%s", val);
		}
		else if (stdlib__strcmp(key, "Type") == 0)
		{
			snprintf(type, sizeof(type), "%s", val);
		}
		else if (stdlib__strcmp(key, "Hidden") == 0)
		{
			hidden = (stdlib__strcmp(val, "true") == 0);
		}
		else if (stdlib__strcmp(key, "NoDisplay") == 0)
		{
			no_display = (stdlib__strcmp(val, "true") == 0);
		}
	}
	fclose(f);

	if (stdlib__strcmp(type, "Application") != 0)
	{
		return;
	}
	if (hidden || no_display)
	{
		return;
	}
	if (name[0] == 0 || exec[0] == 0)
	{
		return;
	}

	//
	// XDG override: a .desktop with the same Name= already loaded
	// from an earlier (system) directory is replaced by the later
	// (user) one. We dedupe by display name rather than basename
	// to keep the "user override wins" semantic close enough; for
	// mismatched basenames vs Name= the duplicate-by-Name check
	// is the more useful one anyway (avoids two "Firefox" tiles).
	//
	int existing = _app_registry_internal__exists_by_name(name);
	app_entry *e;
	if (existing >= 0)
	{
		e = &_app_registry_internal__entries[existing];
	}
	else
	{
		e = &_app_registry_internal__entries[_app_registry_internal__count++];
		stdlib__memset(e, 0, sizeof(*e));
	}

	snprintf(e->name, sizeof(e->name), "%s", name);
	_app_registry_internal__strip_exec_codes(exec, e->exec, sizeof(e->exec));
	snprintf(e->icon, sizeof(e->icon), "%s", icon);
}

static int _app_registry_internal__exists_by_name(const char *name)
{
	for (int i = 0; i < _app_registry_internal__count; i++)
	{
		if (stdlib__strcmp(_app_registry_internal__entries[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

//
// ===== string helpers =====================================================
//

static int _app_registry_internal__ends_with(const char *s, const char *suffix)
{
	int64 sl = stdlib__strlen(s);
	int64 fl = stdlib__strlen(suffix);
	if (fl > sl)
	{
		return 0;
	}
	return stdlib__strcmp(s + (sl - fl), suffix) == 0;
}

static void _app_registry_internal__trim(char *s)
{
	//
	// Strip a trailing CR / LF / space / tab in-place. Then move
	// forward over leading whitespace by shifting remaining bytes
	// down. Total: O(n).
	//
	int64 n = stdlib__strlen(s);
	while (n > 0)
	{
		char c = s[n - 1];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
		{
			s[--n] = 0;
		}
		else
		{
			break;
		}
	}
	int leading = 0;
	while (s[leading] == ' ' || s[leading] == '\t')
	{
		leading++;
	}
	if (leading > 0)
	{
		int64 left = n - leading;
		if (left < 0)
		{
			left = 0;
		}
		for (int64 i = 0; i < left; i++)
		{
			s[i] = s[i + leading];
		}
		s[left] = 0;
	}
}

static void _app_registry_internal__strip_exec_codes(const char *in, char *out, int out_cap)
{
	int j = 0;
	for (int i = 0; in[i] != 0 && j < out_cap - 1; i++)
	{
		if (in[i] == '%')
		{
			char nxt = in[i + 1];
			if (nxt == '%')
			{
				//
				// %% is a literal %. Emit it.
				//
				out[j++] = '%';
				i++;
				continue;
			}
			if (nxt == 'f' || nxt == 'F' || nxt == 'u' || nxt == 'U' || nxt == 'i' || nxt == 'c' || nxt == 'k')
			{
				//
				// %f / %u / etc. expand to args we don't pass.
				// Skip both characters.
				//
				i++;
				continue;
			}
			//
			// Unknown %X: be conservative, drop the % only and
			// keep the next character. The spec doesn't define
			// any other codes today.
			//
			continue;
		}
		out[j++] = in[i];
	}
	out[j] = 0;
	//
	// Trim a trailing space left over from "firefox %u" -> "firefox ".
	//
	while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t'))
	{
		out[--j] = 0;
	}
}

//
// Insertion sort by name. N < 200; the simpler algorithm wins on
// readability + no qsort dependency.
//
static void _app_registry_internal__sort_alpha(void)
{
	for (int i = 1; i < _app_registry_internal__count; i++)
	{
		app_entry tmp = _app_registry_internal__entries[i];
		int j = i - 1;
		while (j >= 0 && stdlib__strcmp(_app_registry_internal__entries[j].name, tmp.name) > 0)
		{
			_app_registry_internal__entries[j + 1] = _app_registry_internal__entries[j];
			j--;
		}
		_app_registry_internal__entries[j + 1] = tmp;
	}
}
