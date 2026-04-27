//
// card_drag.c - implementation of the task-switcher per-card
// drag gesture. See card_drag.h for the user-facing contract.
//
// Lives in its own TU because main.c was outgrowing thin-host
// territory; the gesture state machine + entry points were
// ~200 LoC of self-contained logic that doesn't need to share a
// file with platform setup, view-state, launcher tile populate,
// the per-tick reassert, etc.
//

#include "card_drag.h"

#include "debug_definitions.h"
#include "clib/stdlib.h" //stdlib__strcmp
#include "gui.h"
#include "platforms/linux/platform_linux_wayland_client.h" //platform_wayland__request_render
#include "scene.h"
#include "third_party/log.h"
#include "widgets/widget_process_window.h"

#include "meek_shell_v1_client.h" //meek_shell_v1_client__kill_toplevel

//
// Forward decl into main.c's view-state. Avoids dragging in a full
// view-state header before the rest of main.c is decomposed --
// this getter is a single-bool peek.
//
extern boole meek_shell__is_task_switcher_visible(void);

//
// ===== forward declarations of file-scope statics =========================
//

static gui_node* _card_drag_internal__walk_to_tile_card(gui_node *n);
static void      _card_drag_internal__measure_deck(gui_node *deck, float *out_content_w, float *out_viewport_w);
static float     _card_drag_internal__clamp_inset_l(gui_node *deck, float inset_l_logical);
static void      _card_drag_internal__begin_snap(gui_node *deck, int64 now_ms);
static void      _card_drag_internal__cancel_snap(void);

//
// ===== tunables ============================================================
//

//
// Vertical-up displacement at touch_up that fires the kill. ~25%
// of panel height on a 2246-px panel. Smaller = more accidental
// dismisses; larger = harder to trigger intentionally.
//
#define _CARD_DRAG_INTERNAL__DISMISS_THRESHOLD_PX 500

//
// Release-snap settle duration. ~220 ms is the modern phone
// "settling" feel -- fast enough to be responsive, slow enough
// the eye reads it as "settled" rather than "instantly jumped".
// Ease-out cubic curve in the per-tick driver.
//
#define _CARD_DRAG_INTERNAL__SNAP_DURATION_MS 220

//
// Touch needs to move at least this far before we lock to an axis.
// Below this, neither axis drives anything. Above it, the dominant
// component wins. Kept BELOW meek-ui's tap-vs-scroll slop (16 px)
// so a deliberate swipe locks our axis before scene's tap path
// dispatches on_click on the underlying card.
//
#define _CARD_DRAG_INTERNAL__AXIS_LOCK_SLOP_PX 10

#define _CARD_DRAG_INTERNAL__AXIS_NONE       0
#define _CARD_DRAG_INTERNAL__AXIS_VERTICAL   1
#define _CARD_DRAG_INTERNAL__AXIS_HORIZONTAL 2

//
// ===== state ==============================================================
//

static struct
{
	boole     active;
	int64     touch_id;
	int64     start_x;
	int64     start_y;
	int64     last_x;
	int64     last_y;
	gui_node* card;     //NULL if the touch didn't land on a card.
	gui_node* deck;     //the cards-deck row; NULL only if not found.
	uint      handle;
	int       axis;
	float     base_deck_inset_l;  //deck's inset_l at touch-down (logical px).
} _card_drag_internal__state = { FALSE, -1, 0, 0, 0, 0, NULL, NULL, 0, _CARD_DRAG_INTERNAL__AXIS_NONE, 0.0f };

//
// Snap-to-card state. After a horizontal drag releases, we
// animate the deck's inset_l from where the user let go to a
// target value that puts the closest card centered in the
// viewport. SNAP_DURATION_MS controls the settle time;
// _snap_active gates the per-tick driver.
//
// _snap_deck is captured at begin_snap time. If the deck node is
// freed before the snap finishes (hot-reload, app teardown), the
// per-tick driver detects a NULL/stale node via scene__find_by_id
// re-lookup and bails. Could go stale across a hot-reload of the
// .ui tree which rebuilds every node -- the re-lookup catches that.
//
static boole  _card_drag_internal__snap_active           = FALSE;
static int64  _card_drag_internal__snap_start_ms         = 0;
static float  _card_drag_internal__snap_start_inset_l    = 0.0f;
static float  _card_drag_internal__snap_target_inset_l   = 0.0f;

//
// ===== helpers =============================================================
//

static gui_node* _card_drag_internal__walk_to_tile_card(gui_node *n)
{
	while (n != NULL && stdlib__strcmp(n->klass, "tile-card") != 0)
	{
		n = n->parent;
	}
	return n;
}

//
// Measure the cards-deck's content width vs viewport width, both
// in PANEL pixels (post-layout bounds). Iterates deck's children
// summing their bounds.w + (n-1) gaps. Skips display:none entries.
// Sets both out params to 0 on NULL deck or no children.
//
static void _card_drag_internal__measure_deck(gui_node *deck, float *out_content_w, float *out_viewport_w)
{
	if (out_content_w  != NULL) { *out_content_w  = 0.0f; }
	if (out_viewport_w != NULL) { *out_viewport_w = 0.0f; }
	if (deck == NULL) { return; }

	if (out_viewport_w != NULL) { *out_viewport_w = deck->bounds.w; }

	float content_w = 0.0f;
	int   visible_n = 0;
	for (gui_node *c = deck->first_child; c != NULL; c = c->next_sibling)
	{
		if (c->resolved.display == GUI_DISPLAY_NONE) { continue; }
		content_w += c->bounds.w;
		visible_n++;
	}
	if (visible_n > 1)
	{
		//
		// Add (n-1) gaps. deck->resolved.gap is in logical px so
		// multiply by scale to compare against panel-px bounds.w.
		//
		content_w += deck->resolved.gap * scene__scale() * (float)(visible_n - 1);
	}
	if (out_content_w != NULL) { *out_content_w = content_w; }
}

//
// Clamp inset_l (logical px) to the deck's symmetric overhang
// range. With cards centered in the deck (halign:center), overflow
// distributes equally to left + right; the user can scroll
// inset_l in [-overflow/2, +overflow/2] (logical) and still see
// every card. Returns clamped value.
//
// Returns 0 when content fits inside the viewport (no scroll
// needed). Caller's scroll attempt collapses to "stay centered".
//
static float _card_drag_internal__clamp_inset_l(gui_node *deck, float inset_l_logical)
{
	float content_w_panel  = 0.0f;
	float viewport_w_panel = 0.0f;
	_card_drag_internal__measure_deck(deck, &content_w_panel, &viewport_w_panel);
	if (content_w_panel <= viewport_w_panel) { return 0.0f; }

	float scale = scene__scale();
	if (scale <= 0.0f) { scale = 1.0f; }
	float half_overflow_logical = (content_w_panel - viewport_w_panel) / (2.0f * scale);
	if (inset_l_logical >  half_overflow_logical) { return  half_overflow_logical; }
	if (inset_l_logical < -half_overflow_logical) { return -half_overflow_logical; }
	return inset_l_logical;
}

//
// At release, find the card whose center is closest to the
// viewport center, then schedule a snap of the deck's inset_l so
// that card lands centered. Snap target is clamped to the
// overhang range so we don't snap past edge.
//
static void _card_drag_internal__begin_snap(gui_node *deck, int64 now_ms)
{
	if (deck == NULL) { return; }

	float scale = scene__scale();
	if (scale <= 0.0f) { scale = 1.0f; }

	float viewport_center_x = deck->bounds.x + deck->bounds.w / 2.0f;

	gui_node *closest    = NULL;
	float     min_dist   = 1.0e9f;
	for (gui_node *c = deck->first_child; c != NULL; c = c->next_sibling)
	{
		if (c->resolved.display == GUI_DISPLAY_NONE) { continue; }
		float cx = c->bounds.x + c->bounds.w / 2.0f;
		float d  = (cx > viewport_center_x) ? (cx - viewport_center_x) : (viewport_center_x - cx);
		if (d < min_dist) { min_dist = d; closest = c; }
	}
	if (closest == NULL) { return; }

	float closest_center_x = closest->bounds.x + closest->bounds.w / 2.0f;
	//
	// The card is currently at closest_center_x. To put it at
	// viewport_center_x, we need to shift the deck (and its
	// children) by (viewport_center_x - closest_center_x) panel
	// pixels in addition to whatever shift is already applied.
	// Convert to logical-px delta and add to current inset_l.
	//
	float current_inset_l = deck->style[GUI_STATE_DEFAULT].inset_l;
	float shift_panel     = viewport_center_x - closest_center_x;
	float target_inset_l  = current_inset_l + shift_panel / scale;
	target_inset_l        = _card_drag_internal__clamp_inset_l(deck, target_inset_l);

	if (target_inset_l == current_inset_l)
	{
		//
		// Already aligned. Skip the animation; saves a frame of
		// "wait, did anything happen?" and avoids the snap-active
		// flag staying set on a no-op.
		//
		return;
	}

	_card_drag_internal__snap_start_ms       = now_ms;
	_card_drag_internal__snap_start_inset_l  = current_inset_l;
	_card_drag_internal__snap_target_inset_l = target_inset_l;
	_card_drag_internal__snap_active         = TRUE;
}

static void _card_drag_internal__cancel_snap(void)
{
	_card_drag_internal__snap_active = FALSE;
}

//
// ===== public ==============================================================
//

void meek_shell__card_drag_on_touch_down(int64 id, int64 x, int64 y)
{
	//
	// Single-finger only. Multi-touch in the switcher isn't part
	// of v1.
	//
	if (id != 0) { return; }
	if (!meek_shell__is_task_switcher_visible()) { return; }

	//
	// Cancel any in-flight release-snap. A new finger landing
	// claims the deck; the snap target is now stale.
	//
	_card_drag_internal__cancel_snap();

	gui_node *root = scene__root();
	if (root == NULL) { return; }
	gui_node *hit = scene__hit_test(root, x, y);
	gui_node *card = _card_drag_internal__walk_to_tile_card(hit);
	gui_node *deck = scene__find_by_id("cards-deck");

	DBG_INPUT log_info("[dbg-card-drag] DOWN id=%d at (%d,%d) hit=%p type=%d klass='%s' card=%p deck=%p deck.bounds=(%.0f,%.0f %.0fx%.0f) deck.inset_l=%.1f",
			 (int)id, (int)x, (int)y, (void*)hit, hit ? (int)hit->type : -1, hit ? hit->klass : "(null)",
			 (void*)card, (void*)deck,
			 deck ? deck->bounds.x : -1.0f, deck ? deck->bounds.y : -1.0f, deck ? deck->bounds.w : -1.0f, deck ? deck->bounds.h : -1.0f,
			 deck ? deck->style[GUI_STATE_DEFAULT].inset_l : 0.0f);

	//
	// We track EVERY touch in the switcher, even ones that didn't
	// land on a card. A touch on the deck background can still
	// horizontally scroll the deck. Vertical-dismiss requires a
	// card to dismiss, so it'll just no-op if card is NULL.
	//
	uint handle = 0;
	if (card != NULL)
	{
		for (gui_node *c = card->first_child; c != NULL; c = c->next_sibling)
		{
			if (c->type == GUI_NODE_PROCESS_WINDOW)
			{
				handle = widget_process_window__get_handle(c);
				break;
			}
		}
	}

	float base_deck_inset_l = 0.0f;
	if (deck != NULL)
	{
		base_deck_inset_l = deck->style[GUI_STATE_DEFAULT].inset_l;
	}

	_card_drag_internal__state.active            = TRUE;
	_card_drag_internal__state.touch_id          = id;
	_card_drag_internal__state.start_x           = x;
	_card_drag_internal__state.start_y           = y;
	_card_drag_internal__state.last_x            = x;
	_card_drag_internal__state.last_y            = y;
	_card_drag_internal__state.card              = card;
	_card_drag_internal__state.deck              = deck;
	_card_drag_internal__state.handle            = handle;
	_card_drag_internal__state.axis              = _CARD_DRAG_INTERNAL__AXIS_NONE;
	_card_drag_internal__state.base_deck_inset_l = base_deck_inset_l;
}

void meek_shell__card_drag_on_touch_motion(int64 id, int64 x, int64 y)
{
	if (!_card_drag_internal__state.active) { return; }
	if (id != _card_drag_internal__state.touch_id) { return; }
	_card_drag_internal__state.last_x = x;
	_card_drag_internal__state.last_y = y;

	int64 dx = x - _card_drag_internal__state.start_x;
	int64 dy = y - _card_drag_internal__state.start_y; // negative = up

	float scale = scene__scale();
	if (scale <= 0.0f) { scale = 1.0f; }

	//
	// Axis-lock decision: until total motion clears the slop ring,
	// neither vertical nor horizontal drives anything. Once it
	// does, dominant component wins for the rest of the touch.
	//
	if (_card_drag_internal__state.axis == _CARD_DRAG_INTERNAL__AXIS_NONE)
	{
		int64 adx = dx < 0 ? -dx : dx;
		int64 ady = dy < 0 ? -dy : dy;
		if (adx < _CARD_DRAG_INTERNAL__AXIS_LOCK_SLOP_PX && ady < _CARD_DRAG_INTERNAL__AXIS_LOCK_SLOP_PX) { return; }
		_card_drag_internal__state.axis = (ady > adx) ? _CARD_DRAG_INTERNAL__AXIS_VERTICAL : _CARD_DRAG_INTERNAL__AXIS_HORIZONTAL;
		DBG_INPUT log_info("[dbg-card-drag] AXIS LOCK %s dx=%d dy=%d card=%p handle=%u",
				 _card_drag_internal__state.axis == _CARD_DRAG_INTERNAL__AXIS_VERTICAL ? "VERT" : "HORIZ",
				 (int)dx, (int)dy, (void*)_card_drag_internal__state.card, _card_drag_internal__state.handle);
		//
		// Claim the touch. Without this, meek-ui's normal mouse-up
		// path will still dispatch on_click on whatever was pressed
		// at touch-down (a tile -> fullscreen, a column -> dismiss).
		// Cancelling the press here makes the upcoming touch_up a
		// no-op on meek-ui's side; we drive everything from raw.
		//
		scene__cancel_press();
	}

	if (_card_drag_internal__state.axis == _CARD_DRAG_INTERNAL__AXIS_VERTICAL)
	{
		//
		// Card-dismiss path. Up only; clamp downward drag to 0.
		// inset_t mutated on the card; layout multiplies by scale
		// so writing dy/scale gives a 1:1 finger-follow shift.
		//
		if (dy > 0) { dy = 0; }
		gui_node *card = _card_drag_internal__state.card;
		if (card != NULL)
		{
			card->style[GUI_STATE_DEFAULT].inset_t = (float)dy / scale;
			card->style[GUI_STATE_DEFAULT].inset_b = 0.0f;
			platform_wayland__request_render();
		}
	}
	else if (_card_drag_internal__state.axis == _CARD_DRAG_INTERNAL__AXIS_HORIZONTAL)
	{
		//
		// Deck-scroll path. inset_l on cards-deck shifts the whole
		// row horizontally; +dx scrolls right, -dx scrolls left.
		// Clamped to the deck's symmetric overhang range so the
		// user can't drag the deck off into nothing -- past the
		// leftmost / rightmost card the deck just stops. (v1 has
		// no rubber-band overshoot; touching the edge is a hard
		// stop. Add later if it feels too rigid.)
		//
		gui_node *deck = _card_drag_internal__state.deck;
		if (deck != NULL)
		{
			float new_inset_l = _card_drag_internal__state.base_deck_inset_l + (float)dx / scale;
			new_inset_l = _card_drag_internal__clamp_inset_l(deck, new_inset_l);
			deck->style[GUI_STATE_DEFAULT].inset_l = new_inset_l;
			deck->style[GUI_STATE_DEFAULT].inset_r = 0.0f;
			platform_wayland__request_render();
		}
	}
}

void meek_shell__card_drag_on_touch_up(int64 id)
{
	if (!_card_drag_internal__state.active) { return; }
	if (id != _card_drag_internal__state.touch_id) { return; }

	int64     dy     = _card_drag_internal__state.last_y - _card_drag_internal__state.start_y;
	gui_node* card   = _card_drag_internal__state.card;
	gui_node* deck   = _card_drag_internal__state.deck;
	uint      handle = _card_drag_internal__state.handle;
	int       axis   = _card_drag_internal__state.axis;

	_card_drag_internal__state.active   = FALSE;
	_card_drag_internal__state.touch_id = -1;
	_card_drag_internal__state.card     = NULL;
	_card_drag_internal__state.deck     = NULL;
	_card_drag_internal__state.handle   = 0;
	_card_drag_internal__state.axis     = _CARD_DRAG_INTERNAL__AXIS_NONE;

	if (axis == _CARD_DRAG_INTERNAL__AXIS_VERTICAL && dy < -_CARD_DRAG_INTERNAL__DISMISS_THRESHOLD_PX && handle != 0)
	{
		//
		// Past dismiss threshold + we have a handle: kill it.
		// Push the card far off-screen so its visible disappearance
		// matches the kill request even before toplevel_removed
		// arrives.
		//
		log_info("[meek-shell] card-drag DISMISS handle=%u dy=%d", handle, (int)dy);
		if (card != NULL)
		{
			float scale = scene__scale();
			if (scale <= 0.0f) { scale = 1.0f; }
			card->style[GUI_STATE_DEFAULT].inset_t = -3000.0f / scale;
		}
		meek_shell_v1_client__kill_toplevel(handle);
	}
	else if (axis == _CARD_DRAG_INTERNAL__AXIS_VERTICAL)
	{
		//
		// Snap back to original position. Vertical drag was below
		// the threshold; the user didn't mean to dismiss.
		//
		if (card != NULL)
		{
			card->style[GUI_STATE_DEFAULT].inset_t = 0.0f;
		}
	}
	else if (axis == _CARD_DRAG_INTERNAL__AXIS_HORIZONTAL)
	{
		//
		// Schedule the release-snap so the closest card lands
		// centered. Per-tick driver runs the easing curve.
		//
		_card_drag_internal__begin_snap(deck, scene__frame_time_ms());
	}
	platform_wayland__request_render();
}

void meek_shell__card_drag_per_tick(int64 now_ms)
{
	if (!_card_drag_internal__snap_active) { return; }

	//
	// Re-resolve the deck node every tick. Hot-reload of shell.ui
	// rebuilds the entire scene tree; a stashed pointer would
	// dangle after that. find_by_id is O(N) walk but N is small
	// here (~tens of nodes) and the snap window is ~220 ms so
	// the cost is bounded.
	//
	gui_node *deck = scene__find_by_id("cards-deck");
	if (deck == NULL)
	{
		_card_drag_internal__snap_active = FALSE;
		return;
	}

	int64 elapsed = now_ms - _card_drag_internal__snap_start_ms;
	if (elapsed >= _CARD_DRAG_INTERNAL__SNAP_DURATION_MS)
	{
		deck->style[GUI_STATE_DEFAULT].inset_l   = _card_drag_internal__snap_target_inset_l;
		_card_drag_internal__snap_active = FALSE;
		platform_wayland__request_render();
		return;
	}

	//
	// Ease-out cubic: t=elapsed/duration in [0,1], eased = 1 - (1-t)^3.
	// Starts fast (most distance covered in first 30% of duration),
	// settles smoothly into the target. Matches the modern phone
	// release-snap feel.
	//
	float t      = (float)elapsed / (float)_CARD_DRAG_INTERNAL__SNAP_DURATION_MS;
	float u      = 1.0f - t;
	float eased  = 1.0f - (u * u * u);
	float start  = _card_drag_internal__snap_start_inset_l;
	float target = _card_drag_internal__snap_target_inset_l;
	float now    = start + (target - start) * eased;
	deck->style[GUI_STATE_DEFAULT].inset_l = now;
	platform_wayland__request_render();
}
