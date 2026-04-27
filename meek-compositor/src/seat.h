//
//seat.h -- wl_seat + wl_touch + wl_pointer + wl_keyboard subsystem.
//
//Split out of globals.c (which previously owned everything). This
//file holds the touch resource list, the broadcast helpers called
//from input.c, and the directed routing helpers used by
//meek_shell_v1.c for Phase-6 tap delivery.
//
//External API is still exposed through globals.h so callers don't
//need to change their includes. seat.h exists for the internal
//register entry point and any cross-file helpers.
//
#ifndef MEEK_COMPOSITOR_SEAT_H
#define MEEK_COMPOSITOR_SEAT_H

#include <stdint.h>

struct wl_display;

//
//Register the wl_seat global on `display`. Advertises touch
//capability; pointer + keyboard resources are created as no-op
//stubs if a client asks for them. Called by globals__register_all.
//
void seat__register(struct wl_display* display);

//
//Initialize the touches list. Must run before any wl_seat bind can
//reach get_touch. Called by globals__register_all.
//
void seat__init(void);

#endif
