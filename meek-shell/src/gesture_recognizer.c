//
// gesture_recognizer.c - edge-swipe recognition for meek-shell.
//
// Consumes the raw-touch stream forwarded by meek-compositor via
// meek_shell_v1.touch_*_raw. State machine per finger; v1 tracks
// only slot 0 (single-finger). At TOUCH_UP we classify by:
//
//   (a) start zone: which of 8 edge zones did the touch begin in?
//       top_left, top, top_right
//       left,         right
//       bottom_left, bottom, bottom_right
//
//   (b) primary direction: up / down / left / right based on
//       dx,dy dominant axis + sign, past travel threshold.
//
// Dispatches a handler named `on_swipe_<direction>_<zone>` via
// scene__dispatch_gesture_by_name. If that handler isn't
// registered, also tries the generic side name (e.g. a swipe from
// "top_left" falls back to "top" if the specific corner handler
// isn't bound).
//
// Multi-finger + progress-tracked animations are deferred to a v2
// pass.
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gesture_recognizer.h"
#include "scene.h"
#include "third_party/log.h"

//
// Edge zones. ZONE_NONE means the touch didn't start on any edge --
// probably an in-widget tap or drag; no gesture fires.
//
enum _gesture_recognizer_internal__zone
{
	_ZONE_NONE = 0,
	_ZONE_TOP_LEFT,
	_ZONE_TOP,
	_ZONE_TOP_RIGHT,
	_ZONE_LEFT,
	_ZONE_RIGHT,
	_ZONE_BOTTOM_LEFT,
	_ZONE_BOTTOM,
	_ZONE_BOTTOM_RIGHT,
};

//
// Single-finger v1: one slot of active-touch state.
//
typedef struct _gesture_recognizer_internal__touch
{
	int active;
	int64 id;
	int64 start_x, start_y;
	int64 last_x, last_y;
	uint start_time_ms;
	enum _gesture_recognizer_internal__zone start_zone;
} _gesture_recognizer_internal__touch;

static _gesture_recognizer_internal__touch _gesture_recognizer_internal__slot0;

//
// Metrics for the most recently recognized gesture. Stashed at
// dispatch time so handlers can query them via the public accessors
// (gesture_recognizer__last_dx / _dy / _duration_ms). Lets a handler
// branch on speed/distance without the recognizer having to encode
// every variant into a separate handler name.
//
static int64  _gesture_recognizer_internal__last_dx          = 0;
static int64  _gesture_recognizer_internal__last_dy          = 0;
static uint _gesture_recognizer_internal__last_duration_ms = 0;
static int _gesture_recognizer_internal__panel_w = 1080;
static int _gesture_recognizer_internal__panel_h = 2246;

//
// Edge-proximity fraction: a touch within 10% of the panel's
// smaller dimension from any edge is an edge-swipe candidate.
// On 1080x2246 that's ~108 px -- fat-finger safe, doesn't bleed
// into in-app UI in the middle of the screen.
//
#define _GR_EDGE_FRAC 0.10f

//
// Travel threshold: gesture must cover at least 15% of the
// panel's LARGER dimension to commit. Shorter movements classify
// as taps (ignored here; widget hit-test handles taps).
//
#define _GR_TRAVEL_FRAC 0.15f

static const char *_gesture_recognizer_internal__zone_name(enum _gesture_recognizer_internal__zone z)
{
	switch (z)
	{
	case _ZONE_TOP_LEFT:
		return "top_left";
	case _ZONE_TOP:
		return "top";
	case _ZONE_TOP_RIGHT:
		return "top_right";
	case _ZONE_LEFT:
		return "left";
	case _ZONE_RIGHT:
		return "right";
	case _ZONE_BOTTOM_LEFT:
		return "bottom_left";
	case _ZONE_BOTTOM:
		return "bottom";
	case _ZONE_BOTTOM_RIGHT:
		return "bottom_right";
	default:
		return NULL;
	}
}

//
// For the 4 corner zones, collapse to their "side" form so a
// handler bound to "on_swipe_up_bottom" fires for bottom_left /
// bottom / bottom_right when no corner-specific handler is bound.
// Middle zones stay as-is (bottom -> bottom, left -> left, etc.).
//
static const char *_gesture_recognizer_internal__zone_side(enum _gesture_recognizer_internal__zone z)
{
	switch (z)
	{
	case _ZONE_TOP_LEFT:
	case _ZONE_TOP:
	case _ZONE_TOP_RIGHT:
		return "top";
	case _ZONE_LEFT:
		return "left";
	case _ZONE_RIGHT:
		return "right";
	case _ZONE_BOTTOM_LEFT:
	case _ZONE_BOTTOM:
	case _ZONE_BOTTOM_RIGHT:
		return "bottom";
	default:
		return NULL;
	}
}

void gesture_recognizer__init(int panel_width, int panel_height)
{
	memset(&_gesture_recognizer_internal__slot0, 0, sizeof(_gesture_recognizer_internal__slot0));
	_gesture_recognizer_internal__panel_w = (panel_width > 0) ? panel_width : 1080;
	_gesture_recognizer_internal__panel_h = (panel_height > 0) ? panel_height : 2246;
	log_info("gesture_recognizer: init %dx%d edge_frac=%.2f travel_frac=%.2f (8 "
			 "zones)",
		_gesture_recognizer_internal__panel_w, _gesture_recognizer_internal__panel_h, _GR_EDGE_FRAC, _GR_TRAVEL_FRAC);
}

float gesture_recognizer__bottom_swipe_progress(void)
{
	_gesture_recognizer_internal__touch *t = &_gesture_recognizer_internal__slot0;
	if (!t->active)
	{
		return -1.0f;
	}
	if (t->start_zone != _ZONE_BOTTOM_LEFT && t->start_zone != _ZONE_BOTTOM && t->start_zone != _ZONE_BOTTOM_RIGHT)
	{
		return -1.0f;
	}
	int dy = t->last_y - t->start_y; // negative when moving up.
	if (dy >= 0)
	{
		return 0.0f;
	} // no upward displacement yet; show overlay just-barely-peeking.
	float dist = (float)(-dy);
	int longer = (_gesture_recognizer_internal__panel_h > _gesture_recognizer_internal__panel_w) ? _gesture_recognizer_internal__panel_h : _gesture_recognizer_internal__panel_w;
	float threshold = (float)longer * _GR_TRAVEL_FRAC;
	if (threshold <= 0.0f)
	{
		return 0.0f;
	}
	float p = dist / threshold;
	if (p < 0.0f)
	{
		p = 0.0f;
	}
	if (p > 1.0f)
	{
		p = 1.0f;
	}
	return p;
}

int gesture_recognizer__bottom_swipe_displacement_px(void)
{
	_gesture_recognizer_internal__touch *t = &_gesture_recognizer_internal__slot0;
	if (!t->active)
	{
		return -1;
	}
	if (t->start_zone != _ZONE_BOTTOM_LEFT && t->start_zone != _ZONE_BOTTOM && t->start_zone != _ZONE_BOTTOM_RIGHT)
	{
		return -1;
	}
	int dy = t->last_y - t->start_y; // negative when moving up.
	int up = (dy < 0) ? -dy : 0;
	if (up > _gesture_recognizer_internal__panel_h)
	{
		up = _gesture_recognizer_internal__panel_h;
	}
	return up;
}

//
// Zone classifier. Returns the zone the touch started in; corners
// take priority over edge-middles (a touch in the top-left is
// top_left, not top). Non-edge starts return _ZONE_NONE.
//
static enum _gesture_recognizer_internal__zone _gesture_recognizer_internal__classify(int64 x, int64 y, int panel_w, int panel_h)
{
	int smaller = panel_w < panel_h ? panel_w : panel_h;
	int edge_px = (int)(_GR_EDGE_FRAC * (float)smaller);

	int on_top = (y <= edge_px);
	int on_bottom = (y >= panel_h - edge_px);
	int on_left = (x <= edge_px);
	int on_right = (x >= panel_w - edge_px);

	int third_w = panel_w / 3;
	int horiz_third = (x < third_w) ? 0 : (x < 2 * third_w) ? 1 :
															  2;
	int third_h = panel_h / 3;
	int vert_third = (y < third_h) ? 0 : (y < 2 * third_h) ? 1 :
															 2;

	//
	// Corners take priority: if the touch is simultaneously near
	// two perpendicular edges, classify by the combination.
	//
	if (on_top && (horiz_third == 0 || on_left))
	{
		return _ZONE_TOP_LEFT;
	}
	if (on_top && (horiz_third == 2 || on_right))
	{
		return _ZONE_TOP_RIGHT;
	}
	if (on_bottom && (horiz_third == 0 || on_left))
	{
		return _ZONE_BOTTOM_LEFT;
	}
	if (on_bottom && (horiz_third == 2 || on_right))
	{
		return _ZONE_BOTTOM_RIGHT;
	}

	//
	// Edge middles. By this point only one edge flag is true (no
	// corner combination fired above). If we're on the top/bottom
	// edge, the touch must have been in the middle horizontal
	// third, i.e. zone == TOP or BOTTOM.
	//
	if (on_top)
	{
		return _ZONE_TOP;
	}
	if (on_bottom)
	{
		return _ZONE_BOTTOM;
	}

	//
	// Left/right middles. Must be in middle vertical third (else
	// corners already matched). If vert_third isn't 1 we're in
	// the top/bottom band but not close enough to the top/bottom
	// edge -- still a side-edge start, just biased.
	//
	if (on_left && vert_third == 1)
	{
		return _ZONE_LEFT;
	}
	if (on_right && vert_third == 1)
	{
		return _ZONE_RIGHT;
	}

	//
	// Not an edge start. Tap/drag in center; no gesture.
	//
	return _ZONE_NONE;
}

void gesture_recognizer__on_touch_down(uint time_ms, int64 id, int64 x, int64 y)
{
	if (_gesture_recognizer_internal__slot0.active)
	{
		return;
	}

	_gesture_recognizer_internal__slot0.active = 1;
	_gesture_recognizer_internal__slot0.id = id;
	_gesture_recognizer_internal__slot0.start_x = x;
	_gesture_recognizer_internal__slot0.start_y = y;
	_gesture_recognizer_internal__slot0.last_x = x;
	_gesture_recognizer_internal__slot0.last_y = y;
	_gesture_recognizer_internal__slot0.start_time_ms = time_ms;
	_gesture_recognizer_internal__slot0.start_zone = _gesture_recognizer_internal__classify(x, y, _gesture_recognizer_internal__panel_w, _gesture_recognizer_internal__panel_h);

	const char *zname = _gesture_recognizer_internal__zone_name(_gesture_recognizer_internal__slot0.start_zone);
	log_trace("gesture_recognizer: down id=%d (%d,%d) zone=%s", id, x, y, zname != NULL ? zname : "(none)");
}

void gesture_recognizer__on_touch_motion(uint time_ms, int64 id, int64 x, int64 y)
{
	(void)time_ms;
	if (!_gesture_recognizer_internal__slot0.active)
	{
		return;
	}
	if (_gesture_recognizer_internal__slot0.id != id)
	{
		return;
	}
	_gesture_recognizer_internal__slot0.last_x = x;
	_gesture_recognizer_internal__slot0.last_y = y;
}

//
// Is `z` a valid start-zone for a swipe in direction `dir`?
// Up-swipes must start on a bottom-side zone; down on top-side;
// right-swipes on left-side; left-swipes on right-side. "Side"
// includes both the corner zones and the middle.
//
static int _gesture_recognizer_internal__zone_matches_direction(enum _gesture_recognizer_internal__zone z, const char *dir)
{
	if (strcmp(dir, "up") == 0)
	{
		return (z == _ZONE_BOTTOM_LEFT || z == _ZONE_BOTTOM || z == _ZONE_BOTTOM_RIGHT);
	}
	if (strcmp(dir, "down") == 0)
	{
		return (z == _ZONE_TOP_LEFT || z == _ZONE_TOP || z == _ZONE_TOP_RIGHT);
	}
	if (strcmp(dir, "right") == 0)
	{
		return (z == _ZONE_TOP_LEFT || z == _ZONE_LEFT || z == _ZONE_BOTTOM_LEFT);
	}
	if (strcmp(dir, "left") == 0)
	{
		return (z == _ZONE_TOP_RIGHT || z == _ZONE_RIGHT || z == _ZONE_BOTTOM_RIGHT);
	}
	return 0;
}

void gesture_recognizer__on_touch_up(uint time_ms, int64 id)
{
	if (!_gesture_recognizer_internal__slot0.active)
	{
		return;
	}
	if (_gesture_recognizer_internal__slot0.id != id)
	{
		return;
	}

	int64 dx = _gesture_recognizer_internal__slot0.last_x - _gesture_recognizer_internal__slot0.start_x;
	int64 dy = _gesture_recognizer_internal__slot0.last_y - _gesture_recognizer_internal__slot0.start_y;
	int longer = _gesture_recognizer_internal__panel_w > _gesture_recognizer_internal__panel_h ? _gesture_recognizer_internal__panel_w : _gesture_recognizer_internal__panel_h;
	int travel_min = (int)(_GR_TRAVEL_FRAC * (float)longer);

	int64 abs_dx = dx < 0 ? -dx : dx;
	int64 abs_dy = dy < 0 ? -dy : dy;

	//
	// Pick dominant direction. Only fire a gesture if magnitude
	// exceeds the travel threshold; shorter moves = tap/drag.
	//
	const char *direction = NULL;
	if (abs_dy > abs_dx && abs_dy > travel_min)
	{
		direction = (dy < 0) ? "up" : "down";
	}
	else if (abs_dx > abs_dy && abs_dx > travel_min)
	{
		direction = (dx < 0) ? "left" : "right";
	}

	enum _gesture_recognizer_internal__zone z = _gesture_recognizer_internal__slot0.start_zone;

	if (direction != NULL && z != _ZONE_NONE && _gesture_recognizer_internal__zone_matches_direction(z, direction))
	{
		uint duration = time_ms - _gesture_recognizer_internal__slot0.start_time_ms;

		//
		// Build two handler names:
		//   specific: on_swipe_<direction>_<zone_name>
		//     e.g. on_swipe_up_bottom_left
		//   side:     on_swipe_<direction>_<zone_side>
		//     e.g. on_swipe_up_bottom
		// Dispatch both; if only one is bound, only that one fires.
		// If both are bound, both fire (user's choice).
		//
		char specific[64];
		char side[64];
		const char *zname = _gesture_recognizer_internal__zone_name(z);
		const char *sname = _gesture_recognizer_internal__zone_side(z);
		snprintf(specific, sizeof(specific), "on_swipe_%s_%s", direction, zname);
		snprintf(side, sizeof(side), "on_swipe_%s_%s", direction, sname);

		log_info("gesture_recognizer: recognized %s (also trying %s) "
				 "dx=%d dy=%d travel_min=%d duration=%u ms",
			specific, side, dx, dy, travel_min, duration);

		_gesture_recognizer_internal__last_dx          = dx;
		_gesture_recognizer_internal__last_dy          = dy;
		_gesture_recognizer_internal__last_duration_ms = duration;

		scene__dispatch_gesture_by_name(specific);
		if (strcmp(specific, side) != 0)
		{
			scene__dispatch_gesture_by_name(side);
		}
	}

	memset(&_gesture_recognizer_internal__slot0, 0, sizeof(_gesture_recognizer_internal__slot0));
}

int64 gesture_recognizer__last_dx(void)
{
	return _gesture_recognizer_internal__last_dx;
}

int64 gesture_recognizer__last_dy(void)
{
	return _gesture_recognizer_internal__last_dy;
}

uint gesture_recognizer__last_duration_ms(void)
{
	return _gesture_recognizer_internal__last_duration_ms;
}
