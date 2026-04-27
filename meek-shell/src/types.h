//
// meek-shell/src/types.h
//
// One-line forwarding shim to meek-ui's real types.h so that every
// meek-shell .c file picks up the same type vocabulary (`int64`,
//`ubyte`, `boole`, `uint`, `nint`, etc.) meek-ui uses. Keeping the
// vocabulary aligned avoids impedance mismatch at the library
// boundary when meek-shell starts calling meek-ui's public API.
//
#ifndef MEEK_SHELL_TYPES_H
#define MEEK_SHELL_TYPES_H

#include "../../meek-ui/gui/src/types.h"

#endif
