#ifndef MEEK_SHELL_CARD_DRAG_H
#define MEEK_SHELL_CARD_DRAG_H

//
// card_drag.h - per-finger gesture handler for the task-switcher
// cards deck.
//
// Wired into the raw-touch stream from meek_shell_v1_client.c
// alongside the global edge-swipe gesture_recognizer. Two axes:
//
//   VERTICAL (up only)   -> swipe-up-to-dismiss. Past threshold,
//                           fires kill_toplevel on the card's
//                           toplevel handle and pushes the card
//                           off-screen.
//   HORIZONTAL           -> scroll the cards-deck row left/right
//                           by mutating its inset_l. 1:1 finger-
//                           follow, no momentum / snap.
//
// Touch-down captures the deepest .tile-card ancestor of the hit
// node + the deck's current inset_l (anchor for the scroll
// delta). On first motion clearing the slop ring, the dominant
// component picks an axis lock that holds for the rest of the
// touch; meek-ui's regular click pipeline is cancelled at that
// point so the underlying card doesn't also fullscreen-on-tap.
//
// All three entry points are no-ops when the task switcher isn't
// visible (queried via meek_shell__is_task_switcher_visible).
// Single-finger only -- multi-touch is deferred to v2.
//

#include "types.h"

void meek_shell__card_drag_on_touch_down(int64 id, int64 x, int64 y);
void meek_shell__card_drag_on_touch_motion(int64 id, int64 x, int64 y);
void meek_shell__card_drag_on_touch_up(int64 id);

//
// Per-tick driver for the deck-scroll release-snap animation.
// Called once per main-loop tick from main.c. Cheap when no snap
// is in flight (single bool check + early return). When a snap is
// active, advances the deck's inset_l toward its target with an
// ease-out curve over SNAP_DURATION_MS, finishing on the target
// pixel-exactly. now_ms is CLOCK_MONOTONIC ms (same clock the rest
// of the shell uses via scene__frame_time_ms).
//
void meek_shell__card_drag_per_tick(int64 now_ms);

#endif
