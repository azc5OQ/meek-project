#ifndef MEEK_SHELL_GESTURE_RECOGNIZER_H
#define MEEK_SHELL_GESTURE_RECOGNIZER_H

//
// gesture_recognizer.h - watches the raw touch stream from
// meek_shell_v1.touch_*_raw and recognizes edge-style edge-swipe
// gestures. Fires named events (on_swipe_up_bottom, etc.) that .ui
// files can bind handlers to the same way they bind on_click.
//
// Recognition model: single-finger only in v1. Each touch is tracked
// as a state machine:
//
//   TOUCH_DOWN -> {start-x, start-y, start-time, start-edge}
//   TOUCH_MOTION -> update current-x/y/time
//   TOUCH_UP -> classify: direction vector + distance + start-edge
//               -> fire matching gesture handler if over threshold
//
// Start-edge classification is what separates edge-swipes from
// in-widget drags. If the touch started within `EDGE_THRESHOLD` px
// of the screen edge, it's considered an edge-swipe candidate; a
// large movement in the right direction while the touch was on that
// edge fires the corresponding on_swipe_* gesture. Otherwise the
// touch is treated as potentially a widget tap (wl_touch dispatch
// handles that path -- we don't interfere).
//
// Multi-finger + progress-tracked animations are deferred to a v2
// pass.
//

#include "types.h"

//
// Fire a reset on startup and call the three _on_touch_* from the
// meek_shell_v1_client raw-touch handlers. x/y are screen pixels.
// time_ms is CLOCK_MONOTONIC ms.
//
void gesture_recognizer__init(int panel_width, int panel_height);
void gesture_recognizer__on_touch_down(uint time_ms, int64 id, int64 x, int64 y);
void gesture_recognizer__on_touch_motion(uint time_ms, int64 id, int64 x, int64 y);
void gesture_recognizer__on_touch_up(uint time_ms, int64 id);

//
// Per-frame progress poll for the in-progress bottom-edge upward
// swipe. Polled by main.c each tick to drive the task-switcher
// overlay's slide-from-bottom animation while the user's finger is
// still on the panel. Returns:
//
//   -1.0  no active touch, or active touch did not start in any
//         bottom-edge zone, or finger is moving down (no upward
//         displacement yet).
//   0..1  upward displacement / travel-threshold, clamped. 1.0
//         means the swipe has crossed the commit threshold; the
//         eventual TOUCH_UP will fire on_swipe_up_bottom.
//
// Cheap; a state read + a couple of arithmetic ops. Safe to call
// every frame.
//
float gesture_recognizer__bottom_swipe_progress(void);

//
// Raw upward displacement in pixels for the in-progress bottom-edge
// swipe. Decoupled from the commit threshold so callers can drive
// 1:1 finger-tracking effects (e.g. an overlay sliding up at the
// same rate the finger climbs) while still using
// _bottom_swipe_progress to ramp other properties (opacity etc.)
// against the threshold. Returns:
//
//   -1   no active touch, or active touch did not start in any
//        bottom-edge zone.
//   >=0  pixels moved upward from start_y. 0 if finger is at or
//        below start_y. Capped at panel_h so callers never see a
//        value larger than the screen.
//
int gesture_recognizer__bottom_swipe_displacement_px(void);

//
// Metrics for the most recently recognized gesture. Set at the
// instant the recognizer dispatches its on_swipe_* handler. Stay
// valid until the next gesture fires (no clearing). Useful for
// handlers that branch on speed / distance (e.g. iPhone-style
// swipe-up: short+slow -> task-switcher, long+fast -> home).
//
//   _last_dx       net horizontal displacement (panel px). Sign:
//                  positive = rightward, negative = leftward.
//   _last_dy       net vertical displacement (panel px). Sign:
//                  positive = downward, negative = upward.
//   _last_duration touch start -> touch up, in milliseconds.
//
// Speed = sqrtf(dx*dx + dy*dy) / duration in pixels-per-ms (caller
// computes; recognizer doesn't keep it derived).
//
int64  gesture_recognizer__last_dx(void);
int64  gesture_recognizer__last_dy(void);
uint gesture_recognizer__last_duration_ms(void);

#endif
