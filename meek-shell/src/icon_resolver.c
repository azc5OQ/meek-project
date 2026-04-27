//
// icon_resolver.c - .desktop "Icon=" value -> absolute file path.
//
// Search strategy: try each (root, theme, size, extension) tuple
// in priority order; first existing file wins. This is a thinned-
// down version of the XDG Icon Theme Specification's
// FindIconHelper algorithm; we don't load index.theme or follow
// Inherits chains, but the result is good enough for ~90% of
// installed applications which ship a hicolor PNG fallback.
//
// Roots checked, in order:
//   1. $HOME/.icons/                       (per-user themed)
//   2. $XDG_DATA_HOME/icons/               (per-user XDG)
//   3. /usr/local/share/icons/             (admin-installed)
//   4. /usr/share/icons/                   (system)
//   5. /usr/share/pixmaps/                 (legacy flat dir)
//
// Themes checked (only relevant for roots #1-#4): Adwaita, hicolor.
// Adwaita first because it ships proper square 96/128 PNG fallbacks
// for some app suites; hicolor second as the universal spec-mandated
// fallback.
//
// Sizes preferred (largest first, scaled down beats scaled up):
// 128, 96, 64, 48, 256, 32. We bias toward 96/128 because the
// shell's launcher tile renders the icon at 96x96 logical px.
//
// Extensions preferred: .png first. .svg returned only if no PNG
// found at any size; the caller (widget_image) will silently fail
// to decode SVG until nanosvg integration lands, so an SVG hit
// today produces the letter-fallback in main.c.
//

#include <stdio.h>
#include <stdlib.h> //getenv
#include <string.h>
#include <sys/stat.h> //stat

#include "icon_resolver.h"
#include "third_party/log.h"

//
// ===== forward decls =====================================================
//

static int _icon_resolver_internal__file_exists(const char *path);
static int _icon_resolver_internal__try(char *out, int out_cap, const char *fmt, ...);

//
// ===== public api ========================================================
//

int icon_resolver__resolve(const char *icon_name, char *out, int out_cap)
{
	if (icon_name == NULL || icon_name[0] == 0)
	{
		return 0;
	}
	if (out == NULL || out_cap < 2)
	{
		return 0;
	}

	//
	// Absolute path: trust the .desktop file. Just probe existence.
	// Many third-party packages ship icons under /opt/<app>/share/
	// and reference that path directly.
	//
	if (icon_name[0] == '/')
	{
		if (_icon_resolver_internal__file_exists(icon_name))
		{
			int n = snprintf(out, (size_t)out_cap, "%s", icon_name);
			return (n > 0 && n < out_cap) ? 1 : 0;
		}
		return 0;
	}

	//
	// Strip a trailing extension if the .desktop file already
	// baked one in (e.g. Icon=firefox.png). Per spec, Icon= should
	// be a bare name, but enough .desktop files violate this that
	// we tolerate it.
	//
	char base[160];
	{
		int j = 0;
		for (int i = 0; icon_name[i] != 0 && j + 1 < (int)sizeof(base); i++)
		{
			base[j++] = icon_name[i];
		}
		base[j] = 0;
		//
		// Drop a trailing .png / .svg / .xpm if present.
		//
		const char *exts[] = { ".png", ".svg", ".xpm", NULL };
		for (int e = 0; exts[e] != NULL; e++)
		{
			int blen = (int)strlen(base);
			int xlen = (int)strlen(exts[e]);
			if (blen > xlen && strcmp(base + blen - xlen, exts[e]) == 0)
			{
				base[blen - xlen] = 0;
				break;
			}
		}
	}

	//
	// Build the search roots. Ordering is deliberate.
	//
	char root_user_icons[512];
	char root_xdg_icons[512];
	root_user_icons[0] = 0;
	root_xdg_icons[0] = 0;

	const char *home = getenv("HOME");
	const char *xdg_data_home = getenv("XDG_DATA_HOME");

	if (home != NULL && home[0] != 0)
	{
		snprintf(root_user_icons, sizeof(root_user_icons), "%s/.icons", home);
	}
	if (xdg_data_home != NULL && xdg_data_home[0] != 0)
	{
		snprintf(root_xdg_icons, sizeof(root_xdg_icons), "%s/icons", xdg_data_home);
	}
	else if (home != NULL && home[0] != 0)
	{
		snprintf(root_xdg_icons, sizeof(root_xdg_icons), "%s/.local/share/icons", home);
	}

	const char *themed_roots[] = {
		(root_user_icons[0] != 0) ? root_user_icons : NULL,
		(root_xdg_icons[0] != 0) ? root_xdg_icons : NULL,
		"/usr/local/share/icons",
		"/usr/share/icons",
		NULL,
	};

	const char *themes[] = { "Adwaita", "hicolor", NULL };
	const char *sizes[] = { "128x128", "96x96", "64x64", "48x48", "256x256", "32x32", NULL };
	const char *exts[] = { "png", "svg", NULL };

	//
	// Priority order: PNG before SVG at every (root,theme,size).
	// The outer loop is "extension" so the FIRST hit on .png at any
	// size beats the FIRST hit on .svg at any size.
	//
	for (int e = 0; exts[e] != NULL; e++)
	{
		for (int r = 0; themed_roots[r] != NULL; r++)
		{
			for (int t = 0; themes[t] != NULL; t++)
			{
				for (int s = 0; sizes[s] != NULL; s++)
				{
					if (_icon_resolver_internal__try(out, out_cap, "%s/%s/%s/apps/%s.%s", themed_roots[r], themes[t], sizes[s], base, exts[e]))
					{
						return 1;
					}
				}
				//
				// Try theme/scalable/apps/<name>.svg only -- no PNGs
				// live under scalable/.
				//
				if (strcmp(exts[e], "svg") == 0)
				{
					if (_icon_resolver_internal__try(out, out_cap, "%s/%s/scalable/apps/%s.%s", themed_roots[r], themes[t], base, exts[e]))
					{
						return 1;
					}
				}
			}
		}

		//
		// /usr/share/pixmaps/ is a flat dir (no theme/size split).
		// Only meaningful for legacy apps.
		//
		if (_icon_resolver_internal__try(out, out_cap, "/usr/share/pixmaps/%s.%s", base, exts[e]))
		{
			return 1;
		}
		if (_icon_resolver_internal__try(out, out_cap, "/usr/local/share/pixmaps/%s.%s", base, exts[e]))
		{
			return 1;
		}
	}

	log_trace("icon_resolver: '%s' not found in any theme/size combo", icon_name);
	return 0;
}

//
// ===== helpers ===========================================================
//

static int _icon_resolver_internal__file_exists(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
	{
		return 0;
	}
	return S_ISREG(st.st_mode) ? 1 : 0;
}

#include <stdarg.h>

static int _icon_resolver_internal__try(char *out, int out_cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out, (size_t)out_cap, fmt, ap);
	va_end(ap);
	if (n <= 0 || n >= out_cap)
	{
		return 0;
	}
	if (!_icon_resolver_internal__file_exists(out))
	{
		return 0;
	}
	return 1;
}
