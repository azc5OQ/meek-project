#ifndef MEEK_SHELL_ICON_COLOR_SAMPLER_H
#define MEEK_SHELL_ICON_COLOR_SAMPLER_H

//
// icon_color_sampler.h - dominant-color extraction for app icons.
//
// Given a PNG file path on disk, decode it once, walk the opaque
// pixels, and return an averaged RGB triple. The returned color is
// suitable as the background fill of a square frame behind the
// icon -- pairing each app's tile with a backdrop that vibe-matches
// the icon (e.g. orange tile behind a yellow icon, dark blue tile
// behind a black-and-white icon, etc.).
//
// Sampling strategy:
//   - Decode via stb_image at native resolution.
//   - Iterate every pixel; count only those with alpha > 32
//     (skips fully-transparent corners + very-translucent edges).
//   - Sum R/G/B channel-wise, divide by count.
//   - Apply a mild darken so the tile bg sits visually behind the
//     icon rather than competing for attention.
//   - Cache by path: subsequent calls for the same path return
//     instantly from a small linear table.
//
// Out-of-band cases:
//   - File missing / decode fails: returns 0 (caller falls back to
//     a neutral default tile color).
//   - Empty image (no opaque pixels): returns 0.
//
// Thread safety: not thread-safe (uses a static cache + a static
// scratch buffer during decode). Single-threaded shell only.
//

#include "types.h"

//
// Returns TRUE on success and writes an opaque (a=255) RGB color
// into out_rgba (8-bit per channel, packed RGBA in least-to-most-
// significant order: R | G<<8 | B<<16 | A<<24). Returns FALSE on
// failure (caller should fall back to a default tile color).
//
boole icon_color_sampler__sample(const char *path, uint *out_rgba);

#endif
