#ifndef MEEK_SHELL_ICON_RESOLVER_H
#define MEEK_SHELL_ICON_RESOLVER_H

//
// icon_resolver.h - .desktop "Icon=" value -> absolute file path.
//
// XDG Icon Theme Specification compliance is intentionally minimal:
// we search a hardcoded list of (root, theme, size, extension)
// tuples in priority order and return the first file that exists.
// No index.theme parsing, no Inherits= chains, no SVG-only theme
// rendering yet (PNG-first; SVG paths are returned but the caller
// will reject them today since stb_image can't decode SVG and
// nanosvg integration is a separate pass).
//
// This is enough to render icons for the common case (firefox,
// gnome-calculator, etc. all ship a hicolor PNG fallback).
//
// Spec: https://specifications.freedesktop.org/icon-theme-spec/
//
// Lifecycle:
//
//   icon_resolver__resolve(name_or_path, out, out_cap)
//       name_or_path: the raw value of `Icon=` from the .desktop
//                     file. Either a bare name ("firefox") or an
//                     absolute path ("/opt/foo/share/foo.png").
//       out, out_cap: caller-provided buffer to receive an absolute
//                     path on success.
//       returns 1 on hit (out is populated), 0 on miss.
//
// Thread safety: not thread-safe (uses a couple of static scratch
// buffers internally during resolve). Called only from the shell's
// main thread.
//

int icon_resolver__resolve(const char *icon_name, char *out, int out_cap);

#endif
