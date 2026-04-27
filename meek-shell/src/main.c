//
// meek-shell/src/main.c - D2 pass: meek-ui host running as a Wayland client.
//
// WHAT THIS FILE DOES (D2 scope):
//   - Sets up meek-ui via platform__init (which internally connects
//     to the host compositor via Wayland, binds xdg-shell, creates
//     a fullscreen-ish toplevel, wires EGL + GLES3).
//   - Loads the shell view (views/shell.ui + shell.style staged into
//     ${SHELL_ASSET_DIR} at build time).
//   - Enters the tick loop: pump Wayland events, tick animators,
//     layout, render, swap buffers.
//   - Exits cleanly when the compositor asks us to close
//     (xdg_toplevel.close or socket death).
//
// WHAT ISN'T HERE YET:
//   - meek_shell_v1 extension binding. Defined + implemented on
//     meek-compositor side at pass C5; bound on our side at D3.
//     Until then meek-shell is a plain Wayland client indistinguishable
//     from any other meek-ui app (any standard Wayland compositor
//     hosts it just fine).
//   - Shell views beyond a placeholder. D6 lands home / launcher /
//     task-switcher / lock-screen / status-bar / notification-drawer
//     as proper .ui / .style files under views/.
//   - App-window bridge (toplevel_bridge.c) + input router
//     (input_router.c) -- depend on meek_shell_v1 existing first.
//
// LOGGING: everything runs through log_info / log_warn /
// log_error / log_fatal so any failure in init or teardown shows
// up with a level tag + file + line. Useful because this binary
// will run over SSH on a phone; stderr is the only cheap visibility.
//

#include <stdlib.h>
#include <string.h>
#include <strings.h> //strcasecmp() for MEEK_LOG_LEVEL parsing.

#include "animator.h"
#include "debug_definitions.h"
#include "gui.h"
#include "gui_api.h"
#include "hot_reload.h"
#include "platform.h"
#include "scene.h"
#include "third_party/log.h"
#include "types.h"

//
// Wayland-specific plumbing: the meek_shell_v1 client extension
// binding, and the accessor that exposes meek-ui's wl_display so
// our client glue can piggy-back on the same connection. Both are
// only relevant when meek-shell is built as a Wayland client; in
// the direct-DRM build (PLATFORM=drm, MEEK_SHELL_PLATFORM_DRM) the
// binary never talks Wayland, so these includes + every call site
// below are wrapped in the same preprocessor gate.
//
#ifdef MEEK_SHELL_PLATFORM_WAYLAND_CLIENT
#include "gesture_recognizer.h" //Phase 5: edge-swipe recognition.
#include "meek_shell_v1_client.h"
#include "platforms/linux/platform_linux_wayland_client.h"
#include "toplevel_registry.h" //Phase 6: find app's width/height for coord translation.
#include "widgets/widget_process_window.h" //Phase 6: get handle from the clicked tile.
#endif

#include "app_registry.h" //installed-app discovery + launcher tile dispatch.
#include "card_drag.h"    //task-switcher per-card gesture handler + release-snap driver.
#include "clib/stdlib.h" //stdlib__memcpy / __memset / __strncmp / __strlen / __atoi.
#include "icon_color_sampler.h" //extract dominant color from a PNG icon for tile-frame styling.
#include "icon_resolver.h" //.desktop Icon= -> absolute PNG path on Linux.
#include "settings.h" //runtime preference: opaque_icon_tiles toggle.
#include "widget_registry.h" //widget vtable lookup for setting <image src=...> at runtime.

//
// Asset paths. SHELL_ASSET_DIR is defined at build time via
// -include build/shell_build_defines.h (build.sh) or via a
// CMake target_compile_definitions (CMakeLists.txt). Using the
// macro instead of a hardcoded path lets the same binary run
// whether deployed to /opt/meek-shell or to a dev checkout.
//
#ifndef SHELL_ASSET_DIR
#define SHELL_ASSET_DIR "assets"
#endif

//
// MEEK_LOG_LEVEL support. Same pattern as meek-compositor: parse
// the env var once at startup, set log level accordingly, default
// to LOG_TRACE when unset or unrecognized.
//
static int _main_internal__parse_log_level(const char *s, int fallback)
{
	if (s == NULL)
	{
		return fallback;
	}
	if (strcasecmp(s, "trace") == 0)
	{
		return LOG_TRACE;
	}
	if (strcasecmp(s, "debug") == 0)
	{
		return LOG_DEBUG;
	}
	if (strcasecmp(s, "info") == 0)
	{
		return LOG_INFO;
	}
	if (strcasecmp(s, "warn") == 0)
	{
		return LOG_WARN;
	}
	if (strcasecmp(s, "error") == 0)
	{
		return LOG_ERROR;
	}
	if (strcasecmp(s, "fatal") == 0)
	{
		return LOG_FATAL;
	}
	return fallback;
}

//
// ===== handlers referenced from views/shell.ui ==========================
//
// UI_HANDLER makes the symbol dlsym-findable from inside the toolkit
// at -fvisibility=hidden. Without it scene's dispatcher couldn't
// resolve on_click="on_launcher_tap" strings.
//

//
// Default home view = the app launcher grid (always visible
// unless an app is fullscreened). Running-apps view (#task-switcher)
// is hidden by default and revealed by the swipe-up-from-bottom
// gesture; same gesture from the running-apps view returns to home.
//
//
// File-local statics + cross-file public API forward decls. Per
// project_static_fwd_decls.md every static used in this TU is
// forward-declared here regardless of definition order; the public
// meek_shell__* entries are declared here too because there is no
// dedicated meek_shell.h yet (consumers in meek_shell_v1_client.c
// pick them up via these prototypes).
//
static void _main_internal__populate_app_grid(void);
static void _main_internal__toggle_task_switcher(void);
static void _main_internal__show_switcher(void);
static void _main_internal__show_fullscreen(uint handle);

//
// Public meek_shell__* entry points consumed by meek_shell_v1_client.c
// + card_drag.c. Declared here (no dedicated meek_shell.h yet) so
// the call sites in those TUs resolve at link time.
//
void  meek_shell__show_fullscreen(uint handle);
void  meek_shell__show_switcher(void);
boole meek_shell__handle_is_visible(uint handle);
boole meek_shell__try_consume_pending_launch(uint handle);
boole meek_shell__is_task_switcher_visible(void);

//
// Task-switcher visibility state. main.c owns display for the
// switcher column the same way it owns fullscreen-view's display:
// the .style rule omits display, the per-tick reassert at the
// bottom of the main loop writes the current intent, runtime
// toggles flip THIS bool rather than mutating the node directly
// (which would be clobbered next frame by the style resolver).
//
static int _main_internal__task_switcher_visible = 0;

//
// "Container transform" launch animation state. On a launcher
// tile tap we capture the tile's pixel bounds + start a timer;
// per-tick we drive the fullscreen-view's insets so it grows from
// the icon's bounds to the full panel over ~280 ms with ease-out
// cubic. the CSS "container transform" name applies; modal-from-
// source is the standard shape for this kind of launch animation.
//
// _launch_pending_until_ms is a separate clock. The grow animation
// completes at ~280 ms, but the tapped app may take longer to
// commit its first frame (Firefox can take seconds). We hold the
// "promote next toplevel_added to fullscreen" signal alive for up
// to LAUNCH_PENDING_WINDOW_MS so the user sees the new app fly to
// fullscreen even on cold-launch latency.
//
#define _MEEK_SHELL_INTERNAL__LAUNCH_ANIM_DURATION_MS 350
#define _MEEK_SHELL_INTERNAL__LAUNCH_PENDING_WINDOW_MS 5000

static int _main_internal__launch_anim_active = 0;
static int64 _main_internal__launch_anim_start_ms = 0;
static gui_rect _main_internal__launch_anim_from = { 0.0f, 0.0f, 0.0f, 0.0f };
static int64 _main_internal__launch_pending_until_ms = 0;

//
// Swap the home grid and the running-apps view. Standard phone-shell
// pattern: swipe-up is a view transition, not a stacked overlay.
// The home grid hides, the task switcher shows (or vice versa).
//
static void _main_internal__toggle_task_switcher(void)
{
	_main_internal__task_switcher_visible = !_main_internal__task_switcher_visible;
	log_info("[meek-shell] %s", _main_internal__task_switcher_visible ? "home -> task switcher" : "task switcher -> home");
	//
	// Wake the render gate -- the per-tick reassert at the bottom
	// of the main loop will pick up the new state on the next tick,
	// but the gate doesn't know a UI-relevant change happened.
	//
	platform_wayland__request_render();
}

//
// Tile tap handler in the launcher grid. The tiles are dynamically
// inserted by _populate_app_grid with id="app-N" where N is the
// app_registry index. We extract N from the sender's id, look up
// the registry entry, fork+exec via app_registry__launch.
//
UI_HANDLER void on_app_tile_tap(gui_event *ev)
{
	if (ev == NULL || ev->sender == NULL)
	{
		log_warn("[meek-shell] on_app_tile_tap: null sender");
		return;
	}
	//
	// Hit-test returns the deepest node under the touch point,
	// which for a tile is one of the inner <label>s (icon or name)
	// -- not the tile <column> itself. Walk up the parent chain
	// until we find an ancestor whose id starts with "app-".
	//
	gui_node *tile = ev->sender;
	while (tile != NULL && stdlib__strncmp(tile->id, "app-", 4) != 0)
	{
		tile = tile->parent;
	}
	if (tile == NULL)
	{
		log_warn("[meek-shell] on_app_tile_tap: no 'app-N' ancestor (sender id='%s')", ev->sender->id);
		return;
	}
	int idx = stdlib__atoi(tile->id + 4);
	const app_entry *a = app_registry__get(idx);
	if (a == NULL)
	{
		log_warn("[meek-shell] on_app_tile_tap: idx %d -> no entry (count=%d)", idx, app_registry__count());
		return;
	}
	log_info("[meek-shell] launcher tile tap idx=%d name='%s' exec='%s'", idx, a->name, a->exec);
	if (app_registry__launch(idx) != 0)
	{
		log_warn("[meek-shell] launch failed for '%s'", a->name);
		return;
	}

	//
	// Container-transform launch animation. Capture the tapped
	// tile's pixel bounds (the launcher-tile <column>'s bounds in
	// panel-space, set by the layout pass last frame) so the
	// per-tick reassert can drive the fullscreen-view from those
	// bounds to full-panel. Also arm a "pending launch" window so
	// the next toplevel_added that arrives within the window auto-
	// promotes to fullscreen rather than just landing in the
	// task-switcher deck.
	//
	//
	// Don't start the animation here. The app is forking right now;
	// it will take 100-500 ms to spawn + connect to Wayland + commit
	// its first frame. If we ran the grow animation NOW it'd play
	// against an empty rect for most of its 280 ms and finish before
	// the texture arrives -- the user perceives this as lag (foot
	// is the worst offender). Instead: stash the tile bounds + arm
	// the pending-launch latch, and let try_consume_pending_launch
	// kick off the animation when on_toplevel_added fires. By then
	// the app exists and the mirror path will populate the texture
	// inside one frame of the animation starting.
	//
	int64 now_ms = scene__frame_time_ms();
	_main_internal__launch_anim_active = 0;
	_main_internal__launch_anim_start_ms = 0;
	_main_internal__launch_anim_from = tile->bounds;
	_main_internal__launch_pending_until_ms = now_ms + _MEEK_SHELL_INTERNAL__LAUNCH_PENDING_WINDOW_MS;
	DBG_ANIM log_info("[dbg-launch] tile.bounds=(%.0f,%.0f %.0fx%.0f) tile.id='%s' tile.klass='%s'", tile->bounds.x, tile->bounds.y, tile->bounds.w, tile->bounds.h, tile->id, tile->klass);
	log_info("[meek-shell] launch pending: from=(%.0f,%.0f %.0fx%.0f) pending_window=%dms (anim defers until toplevel_added)", tile->bounds.x, tile->bounds.y, tile->bounds.w, tile->bounds.h, _MEEK_SHELL_INTERNAL__LAUNCH_PENDING_WINDOW_MS);

	//
	// Clear fullscreen-app's stale texture + handle. Without this the
	// growing rect during the animation shows the PREVIOUSLY focused
	// app's preview (or whatever was last bound to fullscreen-app),
	// creating the "I tapped foo, I see bar growing, then foo
	// appears" misdirection. Resetting handle to 0 + texture to 0
	// makes the growing rect render just its bg color (#1f2029)
	// until the new app commits its first frame, at which point the
	// texture-mirror in meek_shell_v1_client.c on_toplevel_buffer
	// populates it.
	//
	gui_node *fa = scene__find_by_id("fullscreen-app");
	if (fa != NULL)
	{
		widget_process_window__set_handle(fa, 0);
		widget_process_window__set_texture(fa, 0, 0, 0);
	}

	//
	// Wake the render gate so the per-tick reassert + animator
	// start running this frame instead of waiting for the next
	// input event.
	//
	platform_wayland__request_render();
}

//
// Called from meek_shell_v1_client::on_toplevel_added. If a launch
// is currently pending (user tapped a launcher tile within the
// pending window), consume it and signal the caller to promote
// this handle to fullscreen. Returns TRUE to consume, FALSE to leave
// the new toplevel landing in the task-switcher deck the normal way.
//
boole meek_shell__try_consume_pending_launch(uint handle)
{
	(void)handle;
	int64 now_ms = scene__frame_time_ms();
	if (_main_internal__launch_pending_until_ms == 0)
	{
		return FALSE;
	}
	if (now_ms > _main_internal__launch_pending_until_ms)
	{
		//
		// Window expired (e.g. firefox took longer than 5 s to
		// commit its first frame). Clear the latch so a
		// user-initiated tap on a switcher card later doesn't get
		// misinterpreted; the fullscreen-view animation has long
		// since completed and the user is back on the launcher.
		//
		_main_internal__launch_pending_until_ms = 0;
		_main_internal__launch_anim_active = 0;
		return FALSE;
	}
	//
	// Latched. Consume it: clear the pending latch and START the
	// container-transform animation now (the app exists; texture
	// will populate within one or two frames via the mirror path
	// in meek_shell_v1_client.c on_toplevel_buffer). This is the
	// "launch in background, then animate when ready" flow --
	// avoids the empty-rect grow that happens for slow-starting
	// apps like foot.
	//
	_main_internal__launch_pending_until_ms = 0;
	_main_internal__launch_anim_active = 1;
	_main_internal__launch_anim_start_ms = now_ms;
	log_info("[meek-shell] launch-anim start (deferred): from=(%.0f,%.0f %.0fx%.0f)", _main_internal__launch_anim_from.x, _main_internal__launch_anim_from.y, _main_internal__launch_anim_from.w, _main_internal__launch_anim_from.h);
	platform_wayland__request_render();
	return TRUE;
}

//
// Card-drag is implemented in card_drag.{c,h}. main.c declares
// only the is_task_switcher_visible getter that card_drag.c reads
// + the public meek_shell__* entry-point forward decls at the top.
//
boole meek_shell__is_task_switcher_visible(void)
{
	return _main_internal__task_switcher_visible ? TRUE : FALSE;
}

//
// Build one <button class="app-tile" id="app-N" text="..."> per
// installed app, three per row. Called once after the initial scene
// load and again on every hot-reload (since hot-reload throws away
// the entire scene tree, the dynamically inserted tiles vanish
// with it; we rebuild them to match).
//
// Sender of on_app_tile_tap recovers the registry index by parsing
// the integer suffix off the tile's id, so the tile id is the
// only data path between this builder and the tap handler.
//
//
// Build one <column class="launcher-tile" id="app-N"> per
// detected app, with two <label> children (placeholder letter
// "icon" + the app name). The parent #app-grid is a
// <collection layout="grid" columns="4"> which arranges the tiles
// in a 4-column grid with the cell size from item-height; each
// tile's own width:100% / height:150 makes it fill the cell so
// the rounded background paints the full slot.
//
// Called once after the initial scene load and on every subsequent
// hot-reload (hot-reload throws away the entire scene tree, so the
// dynamically inserted tiles vanish with it; rebuild to match).
//
static void _main_internal__populate_app_grid(void)
{
	gui_node *grid = scene__find_by_id("app-grid");
	if (grid == NULL)
	{
		log_warn("[meek-shell] no #app-grid in shell.ui; launcher tiles skipped");
		return;
	}

	//
	// Clear any existing children before adding fresh tiles. This
	// matters because hot-reload of .style does NOT rebuild the
	// scene tree (only .ui reload does), so a style edit would
	// re-trigger _populate_app_grid on a tree that still has the
	// tiles from the previous populate, doubling them. Walking
	// first_child / freeing is idempotent for the .ui-rebuild case
	// too -- the rebuild starts with an empty grid, so the loop
	// is a no-op there.
	//
	while (grid->first_child != NULL)
	{
		gui_node *dead = grid->first_child;
		grid->first_child = dead->next_sibling;
		if (grid->first_child == NULL)
		{
			grid->last_child = NULL;
		}
		dead->parent = NULL;
		dead->next_sibling = NULL;
		scene__node_free(dead);
	}
	grid->child_count = 0;

	int n = app_registry__count();

	for (int i = 0; i < n; i++)
	{
		const app_entry *a = app_registry__get(i);
		if (a == NULL)
		{
			continue;
		}

		//
		// Tile = <column> with two <label> children. on_click on
		// the column makes the whole tile tappable; gui_event.sender
		// is this column node so on_app_tile_tap can recover the
		// registry index from the id.
		//
		gui_node *tile = scene__node_new(GUI_NODE_COLUMN);
		if (tile == NULL)
		{
			log_warn("[meek-shell] scene__node_new(COLUMN) failed");
			break;
		}
		stdlib__memset(tile->id, 0, sizeof(tile->id));
		stdlib__memset(tile->klass, 0, sizeof(tile->klass));
		snprintf(tile->id, sizeof(tile->id), "app-%d", i);
		snprintf(tile->klass, sizeof(tile->klass), "launcher-tile");
		scene__set_on_click(tile, "on_app_tile_tap");

		//
		// Real icon path: ask the resolver to map the .desktop's
		// Icon= value (bare name like "firefox" OR an absolute path)
		// to a concrete file under /usr/share/icons / pixmaps / etc.
		// widget_image's path-safety check has a Linux-only allowlist
		// that accepts these prefixes, so the resolved path will load.
		//
		// PNG hits go through GUI_NODE_IMAGE via apply_attribute.
		// SVG hits are reported by the resolver too but stb_image
		// can't decode SVG today (nanosvg integration is a separate
		// pass), so we treat .svg as a miss here and fall through
		// to the letter placeholder. Many newer apps are SVG-only
		// and will land on the letter path until that's wired.
		//
		gui_node *icon = NULL;
		char icon_path[512];
		boole have_image = FALSE;
		if (a->icon[0] != 0 && icon_resolver__resolve(a->icon, icon_path, sizeof(icon_path)))
		{
			//
			// Accept .png AND .svg now that widget_image rasterizes
			// SVG via nanosvg. icon_color_sampler still works on
			// the SVG path -- nanosvg's rasterizer outputs RGBA8
			// pixels through stb_image's same byte layout, so the
			// edge-ring + body-average algorithm doesn't care which
			// format produced them. (BUT: icon_color_sampler uses
			// stb_image directly to decode, so for SVG paths it'll
			// fall back to a default color until we extend it.)
			//
			int ipl = (int)stdlib__strlen(icon_path);
			if (ipl > 4)
			{
				const char *ext = icon_path + ipl - 4;
				if (stdlib__strcmp(ext, ".png") == 0 || stdlib__strcmp(ext, ".svg") == 0)
				{
					have_image = TRUE;
				}
			}
		}

		if (have_image)
		{
			//
			// Build a colored frame column wrapping the <image>. The
			// frame carries the sampled background color and the
			// rounded-square shape; the inner <image> renders the
			// icon a notch smaller so the frame shows around it as
			// a visible halo (modern phone home-screen look).
			//
			//   <column class="launcher-icon-frame" bg=sampled_color>
			//       <image class="launcher-icon-img" src="..."/>
			//   </column>
			//
			gui_node *frame = scene__node_new(GUI_NODE_COLUMN);
			if (frame != NULL)
			{
				stdlib__memset(frame->klass, 0, sizeof(frame->klass));
				snprintf(frame->klass, sizeof(frame->klass), "launcher-icon-frame");
				//
				// on_click NOT set on frame/image/label individually.
				// The launcher-tile (parent column) carries the
				// click handler; meek-ui's dispatch_click walks up
				// from the deepest hit to find the handler, and
				// scene_input's state-chain walks DOWN through the
				// handler-owning ancestor's subtree. Result: any
				// click in the tile fires on_app_tile_tap once AND
				// presses the whole tile (frame+image+label) as
				// one visual unit. With on_click on every leaf the
				// press-group expansion shrunk to the leaf only,
				// causing the "image doesn't shrink in 20% of
				// clicks" alignment bug.
				//

				if (meek_shell_settings__get()->opaque_icon_tiles)
				{
					uint rgba = 0;
					if (icon_color_sampler__sample(icon_path, &rgba))
					{
						//
						// Vertical gradient. Top is a brighter shade
						// of the sampled color (~+35% on each
						// channel, clamped at 1.0); bottom is the
						// sampled color as-is (already darkened by
						// the sampler). Reads as a soft shaded tile
						// rather than a flat block of one color --
						// matches modern phone home-screen looks.
						//
						float r = ((rgba >>  0) & 0xff) / 255.0f;
						float g = ((rgba >>  8) & 0xff) / 255.0f;
						float b = ((rgba >> 16) & 0xff) / 255.0f;

						gui_color top;
						top.r = r * 1.35f; if (top.r > 1.0f) { top.r = 1.0f; }
						top.g = g * 1.35f; if (top.g > 1.0f) { top.g = 1.0f; }
						top.b = b * 1.35f; if (top.b > 1.0f) { top.b = 1.0f; }
						top.a = 1.0f;

						gui_color bottom;
						bottom.r = r;
						bottom.g = g;
						bottom.b = b;
						bottom.a = 1.0f;

						frame->style[GUI_STATE_DEFAULT].bg_gradient_from = top;
						frame->style[GUI_STATE_DEFAULT].bg_gradient_to   = bottom;
						frame->style[GUI_STATE_DEFAULT].bg_gradient_dir  = GUI_GRADIENT_VERTICAL;
						frame->style[GUI_STATE_DEFAULT].has_bg_gradient  = TRUE;

						//
						// Pressed-state darkening of the gradient.
						// The image inside is tinted to ~62% on press
						// via accent-color in shell.style; mirror the
						// same factor on the frame's gradient so the
						// background and the icon darken together.
						// Without this, only the icon image dims and
						// the colored frame keeps its full brightness
						// -- looks like just the icon faded into the
						// frame's bg rather than the whole tile
						// reacting to the press.
						//
						const float k = 0.625f;
						gui_color top_p;
						top_p.r = top.r * k;
						top_p.g = top.g * k;
						top_p.b = top.b * k;
						top_p.a = 1.0f;
						gui_color bottom_p;
						bottom_p.r = bottom.r * k;
						bottom_p.g = bottom.g * k;
						bottom_p.b = bottom.b * k;
						bottom_p.a = 1.0f;
						frame->style[GUI_STATE_PRESSED].bg_gradient_from = top_p;
						frame->style[GUI_STATE_PRESSED].bg_gradient_to   = bottom_p;
						frame->style[GUI_STATE_PRESSED].bg_gradient_dir  = GUI_GRADIENT_VERTICAL;
						frame->style[GUI_STATE_PRESSED].has_bg_gradient  = TRUE;
					}
				}

				gui_node *img = scene__node_new(GUI_NODE_IMAGE);
				if (img != NULL)
				{
					stdlib__memset(img->klass, 0, sizeof(img->klass));
					snprintf(img->klass, sizeof(img->klass), "launcher-icon-img");
					//on_click on launcher-tile parent (see frame block).

					const widget_vtable *vt = widget_registry__get(GUI_NODE_IMAGE);
					if (vt != NULL && vt->apply_attribute != NULL)
					{
						vt->apply_attribute(img, "src", icon_path);
						scene__add_child(frame, img);
					}
					else
					{
						log_warn("[meek-shell] widget_image vtable missing; falling back to letter for '%s'", a->name);
						scene__node_free(img);
						scene__node_free(frame);
						frame = NULL;
					}
				}
				else
				{
					scene__node_free(frame);
					frame = NULL;
				}

				icon = frame; // becomes the tile's first child below.
			}
		}

		if (icon == NULL)
		{
			//
			// Letter fallback: resolver missed (no PNG icon installed),
			// SVG-only theme, or image node creation failed. First
			// letter of the app name in the existing .launcher-icon
			// style; '?' if the name is empty.
			//
			icon = scene__node_new(GUI_NODE_TEXT);
			if (icon == NULL)
			{
				log_warn("[meek-shell] scene__node_new(TEXT icon) failed");
				break;
			}
			stdlib__memset(icon->klass, 0, sizeof(icon->klass));
			snprintf(icon->klass, sizeof(icon->klass), "launcher-icon");
			char first = a->name[0];
			if (first >= 'a' && first <= 'z')
			{
				first = (char)(first - 'a' + 'A');
			}
			if (first == 0)
			{
				first = '?';
			}
			icon->text[0] = first;
			icon->text[1] = 0;
			icon->text_len = 1;
		}
		//
		// on_click on launcher-tile parent only -- meek-ui's
		// dispatch_click walks up to find a handler, so the leaves
		// don't each need to carry one. Plus the press-group state
		// chain expands DOWN through the on_click ancestor's
		// subtree, so the whole tile flashes as one unit on tap.
		//
		scene__add_child(tile, icon);

		gui_node *nm = scene__node_new(GUI_NODE_TEXT);
		if (nm == NULL)
		{
			log_warn("[meek-shell] scene__node_new(TEXT name) failed");
			break;
		}
		stdlib__memset(nm->klass, 0, sizeof(nm->klass));
		snprintf(nm->klass, sizeof(nm->klass), "launcher-label");
		snprintf(nm->text, sizeof(nm->text), "%s", a->name);
		nm->text_len = stdlib__strlen(nm->text);
		//on_click on launcher-tile parent only.
		scene__add_child(tile, nm);

		scene__add_child(grid, tile);
	}

	log_info("[meek-shell] app grid populated with %d tile(s)", n);
}

//
// ===== Phase 5 gesture handlers =========================================
//
// The recognizer in gesture_recognizer.c fires these by calling
// scene__dispatch_gesture_by_name with the gesture name. UI_HANDLER
// makes the symbol dlsym-visible + scene__register_handler-discoverable.
// Handler body is a stub (log only) until the shell grows real
// semantics (task-switcher reveal, notification drawer, etc.).
//

//
// Shell-level state. _focused_handle tracks which app is currently
// fullscreened; 0 means "no fullscreen, the task switcher is up".
// meek_shell_v1_client queries this via meek_shell__focused_handle()
// when a new buffer arrives so it can mirror the texture onto the
// fullscreen-view node if that app is the active one.
//
static uint _main_internal__focused_handle = 0;

uint meek_shell__focused_handle(void)
{
	return _main_internal__focused_handle;
}

//
// Phase 3 visibility predicate: would a buffer / title update for
// `handle` produce pixels the user can see right now?
//
// The shell has two display modes that are mutually exclusive:
//
//   * switcher mode (_focused_handle == 0): shell-column is
//     display:block, every <process-window> tile in the cards-deck
//     is on screen. Returns 1 for any handle.
//
//   * fullscreen mode (_focused_handle == H): shell-column is
//     display:none, fullscreen-view is display:block with
//     fullscreen-app pointed at H. The only handle whose pixels
//     are on screen is H. Returns 1 only when handle == H.
//
// Used by meek_shell_v1_client.c's tile-content handlers to gate
// platform_wayland__request_render() -- invisible buffer updates
// still touch the texture cache (so the tile is current when it
// becomes visible again) but no longer reset the Phase 2 idle
// activity clock. Without this, any foreign app committing at
// vblank rate (htop, gnome-usage's CPU graph) keeps meek-shell at
// vblank-rate forever, even when the user is fullscreened on a
// different idle app.
//
// Caveat: assumes binary switcher-or-fullscreen modes. If a future
// pass adds partial-visibility states (PIP, split-screen,
// peek-during-swipe), this predicate must grow before those modes
// are wired or affected tiles will freeze on stale buffers.
//
boole meek_shell__handle_is_visible(uint handle)
{
	uint focused = _main_internal__focused_handle;
	if (focused == 0)
	{
		return TRUE;
	}
	return (handle == focused) ? TRUE : FALSE;
}

//
// Public wrapper around _show_switcher so meek_shell_v1_client's
// toplevel_removed handler can exit fullscreen-on-dead-handle.
//
// Without this, when an app the shell is fullscreened on dies
// externally (kill, crash, SIGSEGV), the shell stays on the
// fullscreen-view pointed at the now-vanished handle, renders
// only the bg color (near-black), and can't be recovered because
// there's no tap target to exit: every touch in fullscreen-view
// routes to the dead handle via route_touch_* and does nothing.
// Display goes black, user hard-reboots. See bugs_to_investigate.md
// entry #1.
//
void meek_shell__show_switcher(void)
{
	_main_internal__show_switcher();
}

//
// Toggle the shell-column / fullscreen-view display pair. Reaches
// into node->style[GUI_STATE_DEFAULT].display directly -- supported
// by the style resolver (apply_rules_to_node preserves display
// across the wipe specifically so handlers can flip it between
// frames).
//
static void _main_internal__show_switcher(void)
{
	gui_node *shell_col = scene__find_by_id("shell-column");
	gui_node *fv = scene__find_by_id("fullscreen-view");
	DBG_FULLSCREEN log_info("[dbg-fs] show_switcher enter; shell_col=%p fv=%p prev_focused=%u", (void *)shell_col, (void *)fv, _main_internal__focused_handle);
	if (shell_col != NULL)
	{
		shell_col->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_BLOCK;
	}
	if (fv != NULL)
	{
		fv->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_NONE;
	}
	_main_internal__focused_handle = 0;
	//
	// Hide the on-screen keyboard on exit. Symmetric with the
	// always-show in show_fullscreen. Also clear the char-redirect
	// so subsequent typing (if any in the shell itself) goes to
	// local widgets again.
	//
	// And tell meek_shell_v1_client there's no keyboard-focused app
	// anymore, so widget_keyboard taps (if user somehow triggers
	// one) fall back to the ime path only.
	//
	scene__hide_keyboard();
	meek_shell_v1_client__clear_char_redirect();
	meek_shell_v1_client__set_keyboard_focus(0);
	log_info("[meek-shell] exit fullscreen -> switcher");
}

static void _main_internal__show_fullscreen(uint handle)
{
	DBG_FULLSCREEN log_info("[dbg-fs] show_fullscreen ENTER handle=%u (prev focused=%u)", handle, _main_internal__focused_handle);
	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e == NULL)
	{
		log_warn("[meek-shell] show_fullscreen handle=%u not in registry", handle);
		return;
	}
	DBG_FULLSCREEN log_info("[dbg-fs] registry entry: width=%d height=%d tex=%u", e->width, e->height, e->gl_texture);

	gui_node *shell_col = scene__find_by_id("shell-column");
	gui_node *fv = scene__find_by_id("fullscreen-view");
	gui_node *app = scene__find_by_id("fullscreen-app");
	DBG_FULLSCREEN log_info("[dbg-fs] nodes: shell_col=%p fv=%p app=%p", (void *)shell_col, (void *)fv, (void *)app);
	if (fv == NULL || app == NULL)
	{
		log_warn("[meek-shell] show_fullscreen: scene missing #fullscreen-view or "
				 "#fullscreen-app");
		return;
	}

	//
	// Point the inner process-window at the focused app. Handle +
	// texture go on #fullscreen-app (the process-window); the outer
	// #fullscreen-view is a layout wrapper (back bar + app area).
	//
	widget_process_window__set_handle(app, handle);
	//
	// ALWAYS call set_texture, even when the target app hasn't
	// committed a buffer yet (gl_texture==0). If we skipped the
	// call, fullscreen-app would keep the texture from the
	// PREVIOUS focused app and the user would see the wrong
	// content when tapping an app that hasn't rendered yet --
	// manifests as "all taps open the first window" because the
	// first window is the only one with a live texture.
	//
	// Passing gl_texture=0 clears the tile; emit_draws then only
	// paints the bg color. When the target app eventually commits
	// its first buffer, the mirror path in meek_shell_v1_client.c
	// (gated on focused_handle == incoming handle) fills in the
	// live texture.
	//
	widget_process_window__set_texture(app, e->gl_texture, e->width, e->height);

	//
	// Show the on-screen keyboard whenever we enter fullscreen on
	// any app. Most foreign clients (foot, gtk, qt) don't enable
	// zwp_text_input_v3 for English typing -- they use wl_keyboard.
	// The IME-driven show in meek_shell_v1_client triggers only for
	// text_input enable; that's useful for CJK but misses the common
	// case. Always-show-on-fullscreen gives the user the keyboard
	// up-front; they can ignore it if they don't need to type.
	//
	// Also install the char-redirect so widget_keyboard's key taps
	// go through meek_shell_v1 to the foreign app, not to a local
	// meek-shell widget.
	//
	scene__show_keyboard();
	meek_shell_v1_client__install_char_redirect();
	//
	// Tell the client module which app receives synthesized
	// wl_keyboard events. Without this, widget_keyboard taps only
	// fire text_input_v3.commit_string (useless for terminals that
	// don't bind text_input). See meek_shell_v1_client.c's
	// _ime_char_redirect for how this gates the wl_keyboard path.
	//
	meek_shell_v1_client__set_keyboard_focus(handle);
	fv->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_BLOCK;
	if (shell_col != NULL)
	{
		shell_col->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_NONE;
	}
	_main_internal__focused_handle = handle;
	log_info("[meek-shell] enter fullscreen: handle=%u (%dx%d)", handle, e->width, e->height);
}

void meek_shell__show_fullscreen(uint handle)
{
	_main_internal__show_fullscreen(handle);
}

UI_HANDLER void __fullscreen_back(gui_event *ev)
{
	(void)ev;
	_main_internal__show_switcher();
}

//
// Tap on the task-switcher's background area (anywhere not on a
// tile) dismisses the overlay. Hit-test rules: a tap deeper on a
// tile-card fires __process_window_tile_tap and never bubbles up
// here, so we only run for taps on the dim / blurred backdrop +
// the section-label area + cards-deck gap.
//
UI_HANDLER void __on_task_switcher_bg_tap(gui_event *ev)
{
	(void)ev;
	if (!_main_internal__task_switcher_visible)
	{
		//
		// Defensive: shouldn't fire when the overlay is hidden
		// (display:none nodes don't hit-test), but log + bail just
		// in case the gate ever drifts.
		//
		return;
	}
	log_info("[meek-shell] task-switcher bg tap -> dismiss");
	_main_internal__task_switcher_visible = 0;
	platform_wayland__request_render();
}

//
// No-op consumer for taps inside the cards-deck. Without this, a
// tap that lands on the deck row's background (between cards, or
// the dim banner around them) bubbles up to task-switcher's
// on_click and dismisses the overlay -- which a user mid-swipe
// reads as the deck "cancelling itself" or "closing on me".
//
// The deck-scroll path (horizontal-axis card_drag) is how taps
// here translate to motion; this stub just makes sure the
// fall-through tap doesn't reach a dismiss handler.
//
UI_HANDLER void __on_deck_consume_tap(gui_event *ev)
{
	(void)ev;
}

UI_HANDLER void on_swipe_up_bottom(gui_event *ev)
{
	(void)ev;
	//
	// From fullscreen, the destination depends on the swipe's speed
	// + length, modern-phone style:
	//
	//   short / slow  -> task-switcher (running apps deck)
	//   long  / fast  -> home (app launcher grid)
	//
	// Reasoning: a brief flick is the "show me what else is open"
	// signal; a long fast pull is the "get me out of here, all the
	// way home" signal. Tunable thresholds below.
	//
	if (_main_internal__focused_handle != 0)
	{
		int64  dy_panel    = gesture_recognizer__last_dy();
		uint duration_ms = gesture_recognizer__last_duration_ms();
		int64  dy_abs      = (dy_panel < 0) ? -dy_panel : dy_panel;

		//
		// Thresholds (panel pixels + ms). Picked from observation of
		// real swipes on a 1080x2246 panel:
		//   - dy_abs > 1100 px (~half panel height) marks a "long pull".
		//   - duration < 280 ms marks a "fast flick".
		// Both must hold simultaneously to count as long+fast --
		// otherwise the swipe is short or slow and falls through to
		// the task-switcher branch.
		//
		const int64  LONG_DY_PX     = 1100;
		const uint FAST_DURATION  = 280;

		boole long_fast = (dy_abs > LONG_DY_PX && duration_ms < FAST_DURATION) ? TRUE : FALSE;

		log_info("[meek-shell] swipe-up from fullscreen: dy=%d duration=%u ms -> %s", (int)dy_panel, duration_ms, long_fast ? "HOME" : "task-switcher");

		//
		// Set the task-switcher visibility BEFORE exiting fullscreen.
		// _show_switcher only flips the shell-column / fullscreen-view
		// display flags; the per-tick reassert reads
		// _task_switcher_visible to decide whether the cards-deck
		// overlay paints over the launcher grid.
		//
		_main_internal__task_switcher_visible = long_fast ? 0 : 1;
		_main_internal__show_switcher();
		return;
	}
	//
	// From home / task switcher: swap views. Same gesture toggles.
	//
	_main_internal__toggle_task_switcher();
}

//
// Tile tap handler. Fired by a <process-window> tile in the
// cards-deck; promotes that tile's app to fullscreen. If an app
// is already fullscreened, this is a no-op (shouldn't fire anyway
// because the shell-column is display:none in that state).
//
UI_HANDLER void __process_window_tile_tap(gui_event *ev)
{
	if (ev == NULL || ev->sender == NULL)
	{
		return;
	}
	gui_node *sender = ev->sender;

	//
	// The tile is a wrapper column with a process-window + label
	// child; ev->sender can be any of those depending on which
	// sub-rect the hit-test landed in. Resolve to the process-window
	// node so widget_process_window__get_handle returns the real
	// handle. Walk up to the tile-card column then scan its children.
	//
	gui_node *pw = NULL;
	if (sender->type == GUI_NODE_PROCESS_WINDOW)
	{
		pw = sender;
	}
	else
	{
		gui_node *card = sender;
		while (card != NULL && stdlib__strcmp(card->klass, "tile-card") != 0)
		{
			card = card->parent;
		}
		if (card != NULL)
		{
			for (gui_node *c = card->first_child; c != NULL; c = c->next_sibling)
			{
				if (c->type == GUI_NODE_PROCESS_WINDOW)
				{
					pw = c;
					break;
				}
			}
		}
	}
	if (pw == NULL)
	{
		log_warn("[meek-shell] tile_tap: could not resolve process-window for "
				 "sender=%p (klass='%s')",
			(void *)sender, sender->klass);
		return;
	}
	gui_node *tile = pw;
	uint handle = widget_process_window__get_handle(tile);
	//
	// Debug log unconditional here so the "multi-tile hit-test always
	// resolves to the first tile" bug can be diagnosed from the log
	// without having to guess which flag to enable. Shows: sender
	// node pointer, tile bounds, handle the tile claims, and the
	// position of the click. Compare across taps on different tiles;
	// if sender pointer differs but handle is identical, the
	// widget_process_window state is wrong. If sender pointer is
	// identical regardless of which tile was tapped, hit-test is
	// broken.
	//
	log_info("[meek-shell] tile_tap sender=%p bounds=(%.0f,%.0f %.0fx%.0f) "
			 "handle=%u click=(%ld,%ld)",
		(void *)tile, tile->bounds.x, tile->bounds.y, tile->bounds.w, tile->bounds.h, handle, (long)ev->mouse.x, (long)ev->mouse.y);
	if (handle == 0)
	{
		log_warn("[meek-shell] tile_tap: sender has no handle");
		return;
	}
	_main_internal__show_fullscreen(handle);
}

UI_HANDLER void on_swipe_down_top(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] gesture: swipe down from top (stub; future: pull "
			 "notifications)");
}

UI_HANDLER void on_swipe_right_left_edge(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] gesture: swipe right from left edge (stub; future: "
			 "back navigation)");
}

UI_HANDLER void on_swipe_left_right_edge(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] gesture: swipe left from right edge (stub; future: "
			 "forward/recents)");
}

//
// ===== Phase 6: route tile-tap back to the source app ==================
//
// Convention: every <process-window> node gets its on_click wired
// to "__process_window_tap" by meek_shell_v1_client when the node
// is created. ev->sender is the process-window; we read its handle
// + translate click coords from tile-local to surface-local of the
// target app, then fire route_touch_down + route_touch_up on the
// meek_shell_v1 bridge.
//

UI_HANDLER void __process_window_tap(gui_event *ev)
{
	if (ev == NULL || ev->sender == NULL)
	{
		return;
	}
	gui_node *tile = ev->sender;
	uint handle = widget_process_window__get_handle(tile);
	if (handle == 0)
	{
		log_warn("[meek-shell] __process_window_tap: tile has no handle");
		return;
	}
	struct toplevel_entry *e = toplevel_registry__find(handle);
	if (e == NULL || e->width <= 0 || e->height <= 0)
	{
		log_warn("[meek-shell] __process_window_tap: handle=%u unknown or sizeless", handle);
		return;
	}

	//
	// Click position in shell coords (event delivered by
	// wl_touch/mouse which lives in the shell's surface space).
	// Translate to the target app's surface-local coords using the
	// tile's bounds + app's native size.
	//
	float tile_x = tile->bounds.x;
	float tile_y = tile->bounds.y;
	float tile_w = tile->bounds.w;
	float tile_h = tile->bounds.h;
	if (tile_w <= 0 || tile_h <= 0)
	{
		log_warn("[meek-shell] __process_window_tap: tile has zero bounds");
		return;
	}

	float fx = ((float)ev->mouse.x - tile_x) / tile_w; // 0..1 within tile
	float fy = ((float)ev->mouse.y - tile_y) / tile_h;
	int64 sx = (int64)(fx * (float)e->width);
	int64 sy = (int64)(fy * (float)e->height);

	//
	// A single tap = down + up at the same point, time_ms=0 for
	// now (compositor doesn't care about absolute time, only about
	// ordering). id=0 for single-finger.
	//
	log_info("[meek-shell] route tap -> handle=%u at surface (%d,%d) [tile "
			 "%.0fx%.0f, app %dx%d]",
		handle, sx, sy, tile_w, tile_h, e->width, e->height);
	meek_shell_v1_client__route_touch_down(handle, /*time_ms*/ 0, /*id*/ 0, sx, sy);
	meek_shell_v1_client__route_touch_up(handle, /*time_ms*/ 0, /*id*/ 0);
}

//
// Handler stubs for the stress-test UI. Every widget that has an
// on_click in shell.ui needs a corresponding UI_HANDLER here or
// meek-ui logs "on_click handler not found" warnings. They're
// stubs -- real toggling wiring (actual wifi on/off, etc.) is far
// future work; for now they log so we can verify touch reaches
// the right widget on the real phone.
//

UI_HANDLER void on_toggle_wifi(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] toggle: Wi-Fi (stub)");
}

UI_HANDLER void on_toggle_bt(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] toggle: Bluetooth (stub)");
}

UI_HANDLER void on_toggle_air(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] toggle: Airplane (stub)");
}

UI_HANDLER void on_toggle_torch(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] toggle: Torch (stub)");
}

UI_HANDLER void on_music_prev(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] music: prev (stub)");
}

UI_HANDLER void on_music_play(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] music: play/pause (stub)");
}

UI_HANDLER void on_music_next(gui_event *ev)
{
	(void)ev;
	log_info("[meek-shell] music: next (stub)");
}

//
// ===== entry ============================================================
//

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	//
	// Log level from env. Do this BEFORE platform__init so
	// platform init logs respect the operator's choice. log_set_level
	// is safe to call pre-init (rxi's log.h has no setup step).
	//
	// Default level is INFO -- silences the per-event log_trace calls
	// scattered across meek_shell_v1_client.c (pointer_motion_raw,
	// touch_*_raw, frame_presented, etc.) that would otherwise fire
	// on every input event and every vblank. Set MEEK_LOG_LEVEL=trace
	// when actively debugging input or frame timing.
	//
	log_set_level(_main_internal__parse_log_level(getenv("MEEK_LOG_LEVEL"), LOG_INFO));

	//
	// Initialise runtime settings to compile-time defaults, then
	// override from disk if a settings file exists. Persisted
	// values take precedence over defaults; values absent from the
	// file keep their default. Out-of-range values are clamped +
	// logged at warn level by the setters that __load calls
	// through.
	//
	extern void meek_shell_settings__init(void);
	extern void meek_shell_settings__load(void);
	meek_shell_settings__init();
	meek_shell_settings__load();

	gui_app_config cfg;
	cfg.title = L"meek-shell";
	cfg.width = 720; // typical phone portrait-mode width.
	cfg.height = 1280; // typical phone portrait-mode height.
	cfg.clear_color = scene__hex(0x0a0a0d);

	log_info("[meek-shell] starting up (D2 pass; meek_shell_v1 binding not yet live)");

	if (!platform__init(&cfg))
	{
		log_fatal("[meek-shell] platform__init failed -- see previous log lines "
				  "for which step (display connect, registry, toplevel, EGL, "
				  "renderer, font) bailed");
		return 1;
	}

	//
	// Opt into meek-ui's size_w / size_h / font_size transition
	// extension. Off by default in the toolkit; meek-shell's launcher
	// tiles use it for the press eye-candy (160 -> 136 frame, 24 -> 20
	// label font, 130ms ease-out). Gated per-property on the rule
	// having explicitly set the value, so unrelated nodes that don't
	// declare width/height/font-size are unaffected.
	//
	animator__set_size_transitions_enabled(TRUE);

#ifdef MEEK_SHELL_PLATFORM_WAYLAND_CLIENT
	//
	// Bind our privileged extension (D3). Uses the same wl_display
	// meek-ui opened, via the Wayland-client-specific accessor.
	// Degrades to "shell-chrome-only" mode when the compositor
	// doesn't advertise meek_shell_v1 (e.g. running under another
	// compositor for shell-UI iteration).
	//
	// Compiled out under PLATFORM=drm (F1 sketch) -- that variant
	// owns the screen directly and has no compositor to speak the
	// extension with.
	//
	if (meek_shell_v1_client__init(platform_wayland__get_display()) != 0)
	{
		log_fatal("[meek-shell] meek_shell_v1_client__init failed");
		platform__shutdown();
		return 1;
	}
	if (meek_shell_v1_client__is_live())
	{
		log_info("[meek-shell] meek_shell_v1 live (app-window composition enabled)");
		//
		// Bring up the gesture recognizer. Needs the PANEL-NATIVE
		// dimensions because compositor forwards touches via
		// meek_shell_v1 in panel pixels (from libinput
		// get_x/y_transformed against output_drm's mode). cfg.width/
		// cfg.height here are the shell's LOGICAL size (720x1280)
		// which doesn't match the raw-touch stream.
		//
		// Read from meek-ui's wl_output.mode latch. If the compositor
		// hasn't advertised a current mode yet (rare but possible
		// during startup races), fall back to Poco F1's panel-native
		// size as a safe default rather than a 0x0 init that would
		// classify every touch as ZONE_NONE.
		//
		int panel_w = 0;
		int panel_h = 0;
		if (!platform_wayland__get_output_pixel_size(&panel_w, &panel_h))
		{
			log_warn("[meek-shell] wl_output mode not yet known; falling back to 1080x2246 for gesture recognizer init");
			panel_w = 1080;
			panel_h = 2246;
		}
		else
		{
			log_info("[meek-shell] gesture_recognizer init from wl_output: %dx%d", panel_w, panel_h);
		}
		gesture_recognizer__init(panel_w, panel_h);
	}
	else
	{
		log_info("[meek-shell] meek_shell_v1 NOT advertised (shell-chrome-only mode)");
	}
#else
	log_info("[meek-shell] PLATFORM=%s (direct-KMS; no Wayland, no meek_shell_v1)", MEEK_SHELL_PLATFORM_STR);
#endif

	//
	// Hot reload so we can iterate on shell.ui / shell.style without
	// rebuilding meek-shell. hot_reload__watch_* records the file
	// paths; the tick below calls hot_reload__tick to check mtimes
	// on every frame and reparse when needed.
	//
	if (!hot_reload__watch_style(SHELL_ASSET_DIR "/shell.style"))
	{
		log_error("[meek-shell] could not watch " SHELL_ASSET_DIR "/shell.style");
		platform__shutdown();
		return 1;
	}
	if (!hot_reload__watch_ui(SHELL_ASSET_DIR "/shell.ui"))
	{
		log_error("[meek-shell] could not watch " SHELL_ASSET_DIR "/shell.ui");
		platform__shutdown();
		return 1;
	}
	log_info("[meek-shell] hot reload armed on " SHELL_ASSET_DIR "/shell.{ui,style}");

	//
	// Discover installed apps (.desktop files in /usr/share/applications
	// + user-local + XDG override paths) and graft a <button> per app
	// into #app-grid. The scan runs once; the grid populate has to
	// run again on every hot-reload because the reload rebuilds the
	// entire scene tree.
	//
	app_registry__scan();
	_main_internal__populate_app_grid();

	while (platform__tick())
	{
		//
		// Drive the deck-scroll release-snap animation. Cheap
		// no-op when no snap is in flight.
		//
		meek_shell__card_drag_per_tick(scene__frame_time_ms());

		if (hot_reload__tick() > 0)
		{
			//
			// .ui or .style reloaded this tick -- the platform's
			// render gate doesn't watch hot-reload state on its own,
			// so wake the loop or the new content would only show on
			// the next input event.
			//
			platform_wayland__request_render();
			//
			// The reload threw away the entire scene tree, including
			// every dynamically inserted launcher tile. Rebuild them
			// so an .ui edit doesn't silently empty the launcher.
			//
			_main_internal__populate_app_grid();
		}

		//
		// Re-assert display on fullscreen-view every tick. The style
		// resolver runs each frame and if CSS set display:none on
		// .fullscreen-view, the rule would clobber the runtime
		// setting. Since we now OMIT display from the CSS rule,
		// whatever we set here persists (the resolver's memset
		// preserves display + visibility across the wipe). One-time
		// initial hide: if no app is focused, keep the view hidden.
		//
		{
			static uint last_logged_focused = 0xffffffffu;
			gui_node *fv = scene__find_by_id("fullscreen-view");

			//
			// Container-transform launch animation. While active,
			// fullscreen-view is shown regardless of focused_handle
			// and its insets are interpolated from the tapped-tile
			// bounds toward 0/0/0/0. Animation runs over
			// LAUNCH_ANIM_DURATION_MS with ease-out cubic.
			//
			int launch_active = _main_internal__launch_anim_active;
			float launch_t = 0.0f;
			if (launch_active)
			{
				int64 now_ms = scene__frame_time_ms();
				int64 dt_ms = now_ms - _main_internal__launch_anim_start_ms;
				if (dt_ms < 0)
				{
					dt_ms = 0;
				}
				if (dt_ms >= _MEEK_SHELL_INTERNAL__LAUNCH_ANIM_DURATION_MS)
				{
					//
					// Animation done. Insets at 0/0/0/0 (full panel).
					//
					launch_t = 1.0f;
					_main_internal__launch_anim_active = 0;
				}
				else
				{
					launch_t = (float)dt_ms / (float)_MEEK_SHELL_INTERNAL__LAUNCH_ANIM_DURATION_MS;
				}
			}

			gui_display want_fv;
			if (launch_active || _main_internal__focused_handle != 0)
			{
				want_fv = GUI_DISPLAY_BLOCK;
			}
			else
			{
				want_fv = GUI_DISPLAY_NONE;
			}

			if (fv != NULL)
			{
				DBG_FULLSCREEN if (last_logged_focused != _main_internal__focused_handle)
				{
					log_info("[dbg-fs] reassert fv display=%s (focused=%u was=%u)", want_fv == GUI_DISPLAY_BLOCK ? "BLOCK" : "NONE", _main_internal__focused_handle, (unsigned)fv->style[GUI_STATE_DEFAULT].display);
				}
				fv->style[GUI_STATE_DEFAULT].display = want_fv;

				if (launch_active)
				{
					//
					// Smoothstep ease-in-out on the displacement.
					// Earlier we used pure ease-out cubic, which
					// front-loaded the motion (27% in the first
					// ~10% of the duration) and read as "linear and
					// fast" because the back half of the curve was
					// imperceptibly small motion. Smoothstep is
					// symmetric (gentle accel, smooth deceleration)
					// and gives the container-transform
					// feel.
					//
					//   smoothstep(0, 1, t) = t*t*(3 - 2*t)
					//
					// u3 below is "remaining inset fraction" =
					// 1 - smoothstep, multiplied by captured tile
					// bounds to drive the grow-from-icon effect.
					//
					float ss = launch_t * launch_t * (3.0f - 2.0f * launch_t);
					float u3 = 1.0f - ss;
					float panel_w = 1080.0f;
					float panel_h = 2246.0f;
					gui_rect from = _main_internal__launch_anim_from;
					float ins_l = from.x * u3;
					float ins_t = from.y * u3;
					float ins_r = (panel_w - (from.x + from.w)) * u3;
					float ins_b = (panel_h - (from.y + from.h)) * u3;
					if (ins_l < 0.0f)
					{
						ins_l = 0.0f;
					}
					if (ins_t < 0.0f)
					{
						ins_t = 0.0f;
					}
					if (ins_r < 0.0f)
					{
						ins_r = 0.0f;
					}
					if (ins_b < 0.0f)
					{
						ins_b = 0.0f;
					}
					fv->style[GUI_STATE_DEFAULT].inset_l = ins_l;
					fv->style[GUI_STATE_DEFAULT].inset_t = ins_t;
					fv->style[GUI_STATE_DEFAULT].inset_r = ins_r;
					fv->style[GUI_STATE_DEFAULT].inset_b = ins_b;
					//
					// Opacity ramp on fullscreen-view tied to launch_t.
					// Fades from 0 -> 1 over the same 350 ms duration
					// the inset shrink runs. Smooths the perceived
					// "pop" at start: instead of an opaque rectangle
					// suddenly appearing at the icon's bounds and
					// growing, the view materializes alongside the
					// grow. container-transform does the same
					// trick. Linear ramp (vs eased) feels right
					// because the inset motion is already eased --
					// double-easing reads as sluggish.
					//
					fv->style[GUI_STATE_DEFAULT].has_opacity = TRUE;
					fv->style[GUI_STATE_DEFAULT].opacity = launch_t;
					//
					// Throttled per-frame trace so we can see the
					// animation actually firing (or not) without
					// spamming the log. Every 4th frame ~= ~67 ms
					// at 60Hz; over a 350ms run that's 5 lines.
					//
					{
						static int _dbg_frame_count = 0;
						_dbg_frame_count++;
						if ((_dbg_frame_count & 3) == 0)
						{
							DBG_ANIM log_info("[dbg-launch] t=%.3f u3=%.3f insets=(l=%.0f t=%.0f "
									 "r=%.0f b=%.0f) from=(%.0f,%.0f %.0fx%.0f)",
								launch_t, u3, ins_l, ins_t, ins_r, ins_b, from.x, from.y, from.w, from.h);
						}
					}
					//
					// Wake the gate so the next tick re-runs even
					// if the user isn't touching the panel.
					//
					platform_wayland__request_render();
				}
				else
				{
					//
					// Animation finished or not running. Pin insets
					// to 0 so the .style rule stays authoritative,
					// and reset opacity so a future fullscreen entry
					// from the task-switcher (without launch anim)
					// doesn't carry a stale low-alpha override.
					//
					fv->style[GUI_STATE_DEFAULT].inset_l = 0.0f;
					fv->style[GUI_STATE_DEFAULT].inset_t = 0.0f;
					fv->style[GUI_STATE_DEFAULT].inset_r = 0.0f;
					fv->style[GUI_STATE_DEFAULT].inset_b = 0.0f;
					fv->style[GUI_STATE_DEFAULT].has_opacity = FALSE;
					fv->style[GUI_STATE_DEFAULT].opacity = 0.0f;
				}
			}
			gui_node *sc = scene__find_by_id("shell-column");
			//
			// Keep shell-column visible during the launch anim --
			// matches modern phone container-transform feel
			// where the launcher remains behind the icon as it
			// grows. Only hide it once an app is fully focused.
			//
			gui_display want_sc = (_main_internal__focused_handle != 0) ? GUI_DISPLAY_NONE : GUI_DISPLAY_BLOCK;
			if (sc != NULL)
			{
				sc->style[GUI_STATE_DEFAULT].display = want_sc;
			}
			//
			// Task-switcher overlay. Three states drive its display
			// + inset_t:
			//
			//   1. Bottom-edge swipe in progress (gesture_recognizer
			//      reports a 0..1 progress fraction). Overlay is
			//      shown and slides up from off-bottom following the
			//      finger: inset_t = panel_h * (1 - progress). At
			//      progress=0 the overlay is fully off-screen below;
			//      at progress=1 it's fully on-screen (inset_t=0).
			//   2. Settled visible (post-commit; visible bool true).
			//      inset_t=0, display=BLOCK. The bool is flipped by
			//      on_swipe_up_bottom on TOUCH_UP past threshold.
			//   3. Hidden. display=NONE.
			//
			// The progress branch wins over the visible branch, so a
			// user swiping up while the overlay is already settled
			// gets the slide-out behaviour for free (their second
			// swipe will fire on_swipe_up_bottom on release, which
			// toggles the bool to false).
			//
			// Suppressed while an app is fullscreened: the
			// fullscreen-view block above already hides shell-column
			// in that mode, and the task-switcher lives inside
			// shell-column.
			//
			if (_main_internal__focused_handle == 0)
			{
				gui_node *ts = scene__find_by_id("task-switcher");
				if (ts != NULL)
				{
					int disp_px = gesture_recognizer__bottom_swipe_displacement_px();
					float progress = gesture_recognizer__bottom_swipe_progress();
					gui_node *deck = scene__find_by_id("cards-deck");
					//
					// Animation curve constants. Two factors:
					//   * SLIDE_GAIN: how many pixels the overlay
					//     moves per pixel the finger moves. 1:1 felt
					//     too slow (overlay barely revealed before
					//     the finger ran out of screen); the
					//     original implicit ~6.6x was too fast (felt
					//     unnatural). 2x is the sweet spot.
					//   * Ease-out cubic on the position so the
					//     overlay lands smoothly: it accelerates
					//     during early swipe, decelerates as it
					//     approaches its centered resting place. The
					//     opacity ramps linearly because gradual
					//     opacity makes the swipe feel "more deliberate"
					//     -- a non-linear opacity curve would clash.
					//   * cards-deck width grows from 50% -> 100% as
					//     progress climbs, giving the cards a
					//     "grow into place" feel familiar from
					//     modern phone task-switcher reveals.
					//
					static const float SLIDE_GAIN = 2.0f;
					if (disp_px >= 0)
					{
						ts->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_BLOCK;
						float panel_h = 2246.0f;
						//
						// Effective up-displacement after gain. Capped
						// so inset never goes below 0 (overlay fully
						// revealed; further up-finger has no effect).
						//
						float scaled_disp = (float)disp_px * SLIDE_GAIN;
						if (scaled_disp > panel_h)
						{
							scaled_disp = panel_h;
						}
						float lin = scaled_disp / panel_h; // 0..1 linear
						float u = 1.0f - lin; // distance to settled
						float eased_u = u * u * u; // ease-out cubic on (1-x)
						float inset = panel_h * eased_u;
						ts->style[GUI_STATE_DEFAULT].inset_t = inset;
						ts->style[GUI_STATE_DEFAULT].has_opacity = TRUE;
						ts->style[GUI_STATE_DEFAULT].opacity = progress;
						//
						// cards-deck grows from 50% to 100%. width_pct
						// hits 100 once `progress` (0..1 over the commit
						// threshold) hits 1.0 -- earlier than the slide
						// settles, which is fine: the deck is already
						// full-size by the time the user crosses commit.
						//
						if (deck != NULL)
						{
							float p = progress;
							if (p < 0.0f)
							{
								p = 0.0f;
							}
							if (p > 1.0f)
							{
								p = 1.0f;
							}
							deck->style[GUI_STATE_DEFAULT].width_pct = 50.0f + 50.0f * p;
						}
					}
					else if (_main_internal__task_switcher_visible)
					{
						ts->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_BLOCK;
						ts->style[GUI_STATE_DEFAULT].inset_t = 0.0f;
						ts->style[GUI_STATE_DEFAULT].has_opacity = TRUE;
						ts->style[GUI_STATE_DEFAULT].opacity = 1.0f;
						if (deck != NULL)
						{
							deck->style[GUI_STATE_DEFAULT].width_pct = 100.0f;
						}
					}
					else
					{
						ts->style[GUI_STATE_DEFAULT].display = GUI_DISPLAY_NONE;
						ts->style[GUI_STATE_DEFAULT].inset_t = 0.0f;
						ts->style[GUI_STATE_DEFAULT].has_opacity = FALSE;
						ts->style[GUI_STATE_DEFAULT].opacity = 0.0f;
						if (deck != NULL)
						{
							deck->style[GUI_STATE_DEFAULT].width_pct = 100.0f;
						}
					}
				}
			}
			last_logged_focused = _main_internal__focused_handle;
		}
	}

	log_info("[meek-shell] tick loop exited cleanly; tearing down");

	hot_reload__shutdown();
#ifdef MEEK_SHELL_PLATFORM_WAYLAND_CLIENT
	meek_shell_v1_client__shutdown();
#endif
	platform__shutdown();
	return 0;
}
