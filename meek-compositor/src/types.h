#ifndef MEEK_COMPOSITOR_TYPES_H
#define MEEK_COMPOSITOR_TYPES_H

//
//shim: compositor code uses meek-ui's type vocabulary directly.
//This file exists so that clib/*.c's `#include "../types.h"`
//resolves without a forked copy of the type definitions. meek-ui
//remains the source of truth.
//

#include "../../meek-ui/gui/src/types.h"

#endif
