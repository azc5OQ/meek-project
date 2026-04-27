//
//scene.c - retained scene graph orchestration: tree primitives, style
//registration + resolution, layout coordination, hit test, input
//state machine, handler dispatch.
//
//all type-aware behavior (per-widget layout, draw, attribute parsing,
//drag handling) moved to gui/src/widgets/widget_<name>.c. scene.c
//never switches on gui_node_type any more -- it dispatches through
//the widget vtable retrieved via widget_registry__get.
//
//no os calls here; everything in this file is portable c.
//

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "renderer.h"
#include "scene.h"
#include "scene_style.h"
#include "scene_render.h"
#include "scene_input.h"
#include "scroll.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

//
//===== forward declarations of file-scope statics ===========================
//
//Project convention: every static helper used inside this TU is
//forward-declared here so any function defined later can refer to
//it regardless of textual order. The actual definitions live near
//the file-scope state they touch.
//

static void _scene_internal__on_node_freed(gui_node* node);

//
//===== color constructors ===================================================
//

gui_color scene__rgba(float r, float g, float b, float a)
{
    gui_color c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

gui_color scene__rgb(float r, float g, float b)
{
    return scene__rgba(r, g, b, 1.0f);
}

gui_color scene__hex(uint rrggbb)
{
    float r = (float)((rrggbb >> 16) & 0xFFu) / 255.0f;
    float g = (float)((rrggbb >>  8) & 0xFFu) / 255.0f;
    float b = (float)((rrggbb      ) & 0xFFu) / 255.0f;
    return scene__rgba(r, g, b, 1.0f);
}

//
//===== fnv-1a 32-bit =========================================================
//
//used by the handler registration table and the .ui parser to hash
//on_click="name" / on_change="name" once at parse time.
//

uint scene__hash_name(char* name)
{
    uint hash = 2166136261u;
    const ubyte* p = (const ubyte*)name;
    while (*p != 0)
    {
        hash ^= (uint)(*p);
        hash *= 16777619u;
        p++;
    }
    return hash;
}

//
//===== handler registration table ===========================================
//

#define _SCENE_INTERNAL__MAX_HANDLERS 256

/**
 *one row of the handler registration table. parser_xml computes the
 *fnv-1a hash once at parse time; dispatch compares hashes.
 */
typedef struct _scene_internal__handler_entry
{
    uint           hash;
    char           name[64]; // copy of the registered name; reserved for future strcmp-based collision tie-break.
    gui_handler_fn fn;
} _scene_internal__handler_entry;

static _scene_internal__handler_entry _scene_internal__handlers[_SCENE_INTERNAL__MAX_HANDLERS];
static int64                          _scene_internal__handler_count = 0;

void scene__register_handler(char* name, gui_handler_fn fn)
{
    if (_scene_internal__handler_count >= _SCENE_INTERNAL__MAX_HANDLERS)
    {
        log_error("scene__register_handler: table full");
        return;
    }
    _scene_internal__handler_entry* e = &_scene_internal__handlers[_scene_internal__handler_count++];
    e->hash = scene__hash_name(name);
    e->fn   = fn;

    size_t n = strlen(name);
    if (n >= sizeof(e->name))
    {
        n = sizeof(e->name) - 1;
    }
    memcpy(e->name, name, n);
    e->name[n] = 0;
}

static gui_handler_fn _scene_internal__lookup_handler(uint hash)
{
    for (int64 i = 0; i < _scene_internal__handler_count; i++)
    {
        if (_scene_internal__handlers[i].hash == hash)
        {
            return _scene_internal__handlers[i].fn;
        }
    }
    return NULL;
}

//
//host symbol resolver. installed by the platform layer (Win32:
//GetProcAddress against the host exe). NULL means "no fallback".
//
static gui_symbol_resolver_fn _scene_internal__symbol_resolver = NULL;

void scene__set_symbol_resolver(gui_symbol_resolver_fn resolver)
{
    _scene_internal__symbol_resolver = resolver;
}

//
//try the registration table first; on miss, ask the host resolver
//and cache the result so subsequent lookups don't pay the syscall.
//
static gui_handler_fn _scene_internal__resolve_handler(uint hash, char* name)
{
    gui_handler_fn fn = _scene_internal__lookup_handler(hash);
    if (fn != NULL)
    {
        return fn;
    }
    if (_scene_internal__symbol_resolver == NULL || name == NULL || name[0] == 0)
    {
        return NULL;
    }
    fn = _scene_internal__symbol_resolver(name);
    if (fn != NULL)
    {
        scene__register_handler(name, fn);
    }
    return fn;
}

//
//===== tree construction ====================================================
//

gui_node* scene__node_new(gui_node_type type)
{
    gui_node* n = (gui_node*)GUI_CALLOC_T(1, sizeof(gui_node), MM_TYPE_NODE);
    if (n == NULL)
    {
        return NULL;
    }
    n->type  = type;
    n->state = GUI_STATE_DEFAULT;
    return n;
}

void scene__add_child(gui_node* parent, gui_node* child)
{
    if (parent == NULL || child == NULL)
    {
        return;
    }
    child->parent       = parent;
    child->next_sibling = NULL;

    if (parent->first_child == NULL)
    {
        parent->first_child = child;
        parent->last_child  = child;
    }
    else
    {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
    parent->child_count++;

    //
    // Fire on_attach AFTER the link is established so the hook can
    // walk up via child->parent. Optional; widgets that don't care
    // leave the slot NULL.
    //
    const widget_vtable* vt = widget_registry__get(child->type);
    if (vt != NULL && vt->on_attach != NULL)
    {
        vt->on_attach(child, parent);
    }
}

void* scene__widget_state(gui_node* n)
{
    if (n == NULL) { return NULL; }
    if (n->user_data != NULL) { return n->user_data; }

    const widget_vtable* vt = widget_registry__get(n->type);
    if (vt == NULL || vt->state_size <= 0) { return NULL; }

    n->user_data = GUI_CALLOC_T(1, (size_t)vt->state_size, MM_TYPE_GENERIC);
    return n->user_data;
}

void scene__set_on_click(gui_node* node, char* handler_name)
{
    if (node == NULL || handler_name == NULL)
    {
        return;
    }
    node->on_click_hash = scene__hash_name(handler_name);
    size_t n = strlen(handler_name);
    if (n >= sizeof(node->on_click_name))
    {
        n = sizeof(node->on_click_name) - 1;
    }
    memcpy(node->on_click_name, handler_name, n);
    node->on_click_name[n] = 0;
}

void scene__set_on_change(gui_node* node, char* handler_name)
{
    if (node == NULL || handler_name == NULL)
    {
        return;
    }
    node->on_change_hash = scene__hash_name(handler_name);
    size_t n = strlen(handler_name);
    if (n >= sizeof(node->on_change_name))
    {
        n = sizeof(node->on_change_name) - 1;
    }
    memcpy(node->on_change_name, handler_name, n);
    node->on_change_name[n] = 0;
}

//
// Per-node teardown shared between the iterative + recursive paths
// of scene__node_free. Clears any file-scope pointers that may
// reference `n` (overlay / root / focus / pressed / hover), runs
// the widget's on_destroy hook, and frees auto-allocated user_data.
// Does NOT free `n` itself or recurse into children.
//
static void _scene_internal__node_teardown_one(gui_node* n)
{
    if (n == NULL) { return; }
    //
    // Defensive clear of every file-scope slot that might point at
    // this node. Each clear is independent so a node that's both,
    // say, the focused AND overlay owner gets BOTH slots NULLed.
    //
    _scene_internal__on_node_freed(n);
    //
    //Let the widget release any per-node heap / GPU resources before
    //we drop user_data + the node itself. Dispatch through the vtable
    //so widget_image's texture, widget_select's popup state, etc. all
    //get proper cleanup.
    //
    const widget_vtable* vt = widget_registry__get(n->type);
    if (vt != NULL && vt->on_destroy != NULL)
    {
        vt->on_destroy(n);
    }
    //
    // Auto-free user_data IF the widget opted into auto-alloc via
    // vtable->state_size. on_destroy had its chance to release
    // sub-resources stashed inside; now drop the outer allocation.
    // on_destroy is allowed to NULL user_data itself (legacy widgets
    // do this), in which case the auto-free becomes a no-op.
    //
    if (vt != NULL && vt->state_size > 0 && n->user_data != NULL)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL;
    }
}

//
// Recursive fallback used only when the iterative path can't allocate
// its work list. Same semantics as the historical recursive
// implementation; depth is bounded by the C stack, so a pathological
// 100k-deep tree could blow it -- but if we got here, the system is
// already out of memory, and recursion at least makes forward progress.
//
static void _scene_internal__node_free_recursive(gui_node* n)
{
    if (n == NULL) { return; }
    _scene_internal__node_teardown_one(n);
    gui_node* c = n->first_child;
    while (c != NULL)
    {
        gui_node* next = c->next_sibling;
        _scene_internal__node_free_recursive(c);
        c = next;
    }
    GUI_FREE(n);
}

void scene__node_free(gui_node* node)
{
    if (node == NULL)
    {
        return;
    }
    //
    // Iterative free so deeply nested trees can't overflow the C
    // stack. Three-phase:
    //
    //   Phase 1: walk the tree level-order, collecting every node
    //            pointer into a dynamic array. PURE collection --
    //            no teardown, no free. The walk uses first_child /
    //            next_sibling, which we don't touch.
    //
    //   Phase 2: iterate the collected list once, calling per-node
    //            teardown (on_destroy + user_data free) on each.
    //            Order doesn't matter for correctness; we go in
    //            collected order (parents-before-children) to
    //            match the historical recursive contract.
    //
    //   Phase 3: reverse-iterate the list and GUI_FREE each node
    //            so children's allocations get returned before
    //            their parents'.
    //
    // Splitting collection from teardown means the realloc-failure
    // recovery path is straightforward: we know exactly which nodes
    // are in the list (none teardown'd yet) and which still need
    // recursive handling (the un-collected suffix of `n`'s child
    // chain). No risk of double-teardown.
    //
    // Falls back to recursion entirely if the initial allocation
    // fails -- recursion uses C stack but at least makes forward
    // progress, which is what matters when memory is already
    // exhausted.
    //
    int64       cap   = 64;
    gui_node**  list  = (gui_node**)GUI_MALLOC_T(cap * sizeof(gui_node*), MM_TYPE_GENERIC);
    if (list == NULL)
    {
        _scene_internal__node_free_recursive(node);
        return;
    }
    int64       count          = 0;
    int64       visit_idx      = 0;
    gui_node*   uncollected    = NULL;  // first un-collected sibling, or NULL.
    list[count++] = node;

    while (visit_idx < count && uncollected == NULL)
    {
        gui_node* n = list[visit_idx++];
        for (gui_node* c = n->first_child; c != NULL; c = c->next_sibling)
        {
            if (count >= cap)
            {
                int64       new_cap  = cap * 2;
                gui_node**  new_list = (gui_node**)GUI_REALLOC(list, new_cap * sizeof(gui_node*));
                if (new_list == NULL)
                {
                    //
                    // Grow failed. Stop collecting. Remember `c` so
                    // phase-2.5 can recursively free it and its
                    // later siblings -- those subtrees never made
                    // it into the list, so handling them via the
                    // recursive helper (which does its own
                    // teardown + free) keeps the bookkeeping clean.
                    //
                    uncollected = c;
                    break;
                }
                list = new_list;
                cap  = new_cap;
            }
            list[count++] = c;
        }
    }

    //
    // Phase 2: per-node teardown. on_destroy hooks may inspect
    // first_child / next_sibling to walk children -- those pointers
    // are still valid here because phase 3 hasn't run yet, and the
    // un-collected siblings (if any) haven't been freed yet either.
    //
    for (int64 i = 0; i < count; i++)
    {
        _scene_internal__node_teardown_one(list[i]);
    }

    //
    // Phase 2.5 (only if grow failed): recursively free the
    // un-collected suffix of children. Walks via next_sibling so
    // we visit ALL un-collected siblings, not just `c`'s subtree.
    // recursive_free does its own teardown + free.
    //
    while (uncollected != NULL)
    {
        gui_node* next = uncollected->next_sibling;
        _scene_internal__node_free_recursive(uncollected);
        uncollected = next;
    }

    //
    // Phase 3: reverse-iterate the list so children are freed
    // before their parents. Matches the historical recursive
    // ordering for any external observer (memory-tracker logs,
    // leak-sweep tools, etc.).
    //
    for (int64 i = count - 1; i >= 0; i--)
    {
        GUI_FREE(list[i]);
    }
    GUI_FREE(list);
}

//
// ===== tree reconciliation for hot reload ====================================
//
// hot_reload__reload_ui parses the new .ui file and wants to swap
// it in without resetting every stateful interaction. The naive
// approach (free old, install new) loses scroll position, open
// dropdowns, slider values, text input contents, canvas pixels,
// image textures -- everything on user_data. scene__reconcile_tree
// fixes that by walking both trees in parallel, matching nodes by
// id (or a path-based fallback), and moving runtime state from
// old to new in place.
//
// Matching strategy:
//   1. Collect every node in the OLD tree that has a non-empty id
//      into a small flat array keyed by id hash. O(n) build, O(1)
//      lookup during the walk.
//   2. Walk the NEW tree. For each node with an id, try the hash
//      table. If hit AND types match, transfer state.
//   3. For id-less nodes, recurse pairwise: if both parents matched
//      and the child at the same sibling index has the same type,
//      transfer state. This is best-effort; a child reordering
//      breaks the match for that subtree.
//
// State moved:
//   - scroll_x, scroll_y, scroll_y_target, scroll_drag_* -- scroll
//     position survives a .ui edit.
//   - value, value_min, value_max -- slider / checkbox / select
//     selection.
//   - is_open -- select dropdowns stay open, popup stays open.
//   - text, text_len, appear_age_ms -- input / textarea content
//     survives; appear-animation doesn't replay.
//   - user_data -- widget-specific heap state (image texture,
//     canvas pixels, colorpicker HSV, select items, etc.). After
//     transfer, old->user_data is NULLed so the old node's
//     on_destroy becomes a no-op instead of freeing the resource
//     we just handed to the new node.
//

#define _SCENE_INTERNAL__RECONCILE_MAP_CAP 256

typedef struct _scene_internal__recon_entry
{
    uint       id_hash;
    gui_node*  old_node;
} _scene_internal__recon_entry;

static void _scene_internal__recon_collect_ids(gui_node* n, _scene_internal__recon_entry* map, int64* map_count)
{
    if (n == NULL) { return; }
    if (n->id[0] != 0 && *map_count < _SCENE_INTERNAL__RECONCILE_MAP_CAP)
    {
        map[*map_count].id_hash  = scene__hash_name(n->id);
        map[*map_count].old_node = n;
        (*map_count)++;
    }
    gui_node* c = n->first_child;
    while (c != NULL)
    {
        _scene_internal__recon_collect_ids(c, map, map_count);
        c = c->next_sibling;
    }
}

static gui_node* _scene_internal__recon_find(_scene_internal__recon_entry* map, int64 count, uint id_hash)
{
    for (int64 i = 0; i < count; i++)
    {
        if (map[i].id_hash == id_hash) { return map[i].old_node; }
    }
    return NULL;
}

//
// Recursive id-walk used by scene__reconcile_tree. Visits every
// node in the new subtree; for each one that has an id, looks it
// up in the old-tree map and transfers state. The root is matched
// by the caller before this fires (and skipped here via the
// `root` parameter compare) so we don't double-transfer.
//
static void _scene_internal__recon_transfer(gui_node* old, gui_node* new_n);

static void _scene_internal__recon_walk_ids(gui_node* n, gui_node* root, _scene_internal__recon_entry* map, int64 map_count)
{
    if (n == NULL) { return; }
    if (n->id[0] != 0 && n != root)
    {
        uint h = scene__hash_name(n->id);
        gui_node* matched = _scene_internal__recon_find(map, map_count, h);
        if (matched != NULL)
        {
            _scene_internal__recon_transfer(matched, n);
        }
    }
    gui_node* c = n->first_child;
    while (c != NULL)
    {
        _scene_internal__recon_walk_ids(c, root, map, map_count);
        c = c->next_sibling;
    }
}

//
// Copy runtime state from `old` to `new` in place. Both pointers
// must be non-NULL, the nodes must be of the same type, and the
// caller takes responsibility for not double-freeing old's
// user_data (we NULL it here so on_destroy becomes safe).
//
static void _scene_internal__recon_transfer(gui_node* old, gui_node* new_n)
{
    if (old == NULL || new_n == NULL) { return; }
    if (old->type != new_n->type)     { return; }

    new_n->scroll_x                 = old->scroll_x;
    new_n->scroll_y                 = old->scroll_y;
    new_n->scroll_y_target          = old->scroll_y_target;
    new_n->scroll_drag_axis         = old->scroll_drag_axis;
    new_n->scroll_drag_mouse_start  = old->scroll_drag_mouse_start;
    new_n->scroll_drag_scroll_start = old->scroll_drag_scroll_start;

    //
    // Value fields: slider / checkbox / select / colorpicker all
    // stash their interactable value here. Transfer unconditionally
    // -- if the .ui file re-declared `value="0.5"` the parser set
    // that into `new_n->value`, and we overwrite it with the old
    // runtime value. That's the desired behavior for hot reload:
    // the user's current slider position survives a style tweak.
    //
    new_n->value     = old->value;
    new_n->value_min = old->value_min;
    new_n->value_max = old->value_max;

    new_n->is_open         = old->is_open;
    new_n->appear_age_ms   = old->appear_age_ms;      // don't replay appear animation
    new_n->prev_visibility = old->prev_visibility;
    new_n->prev_display    = old->prev_display;

    //
    // Text: only preserve for editable widgets (<input>, <textarea>).
    // Otherwise a hot reload that changed `text="..."` on a <label>
    // or <button> in the .ui would silently lose its update because
    // the runtime-side text would clobber the parser-loaded text.
    // Editable widgets are the inverse: their text IS the runtime
    // state, and the parser doesn't override it. Restricting the
    // transfer to those types gets both right.
    //
    if (old->text_len > 0 && (new_n->type == GUI_NODE_INPUT || new_n->type == GUI_NODE_TEXTAREA))
    {
        memcpy(new_n->text, old->text, sizeof(new_n->text));
        new_n->text_len = old->text_len;
    }

    //
    // user_data transfer policy. Two modes, per-widget vtable flag:
    //
    //   preserve_user_data = TRUE (canvas, colorpicker, ...):
    //     OLD's user_data takes over the new node, parser-allocated
    //     user_data on the new node is freed first via on_destroy.
    //     Runtime state (canvas pixels, picked color) survives the
    //     reload at the cost of declarative state changes being
    //     ignored.
    //
    //   preserve_user_data = FALSE (default; popup, image, ...):
    //     New's parser-allocated user_data wins. Old's gets freed
    //     when the old tree is torn down (on_destroy, which sees
    //     old->user_data still set and releases it normally). A
    //     hot-reload edit to `type="..."` / `src="..."` etc. takes
    //     effect immediately at the cost of losing any runtime
    //     state the widget had built up there.
    //
    const widget_vtable* vt = widget_registry__get(new_n->type);
    if (vt != NULL && vt->preserve_user_data)
    {
        if (new_n->user_data != NULL)
        {
            //
            // Drop the parser-allocated state on the new node first
            // so it's not leaked when we overwrite the slot below.
            // on_destroy handles widget-specific cleanup (releasing
            // GPU textures, free'ing sub-allocations, etc.).
            //
            if (vt->on_destroy != NULL) { vt->on_destroy(new_n); }
            //
            // on_destroy may have set user_data = NULL itself;
            // belt-and-suspenders to ensure no double-free below.
            //
            new_n->user_data = NULL;
        }
        new_n->user_data = old->user_data;
        old->user_data   = NULL;
    }
    //
    // The default branch leaves new_n->user_data alone (parser
    // allocated it). The old's user_data is NOT touched here -- it
    // gets released naturally when scene__node_free walks the old
    // tree.
    //

    //
    // Host-side overrides survive the transfer the same way. If
    // the host toggled a theme override on a node, the new tree's
    // corresponding node should start in that same themed state
    // rather than reverting to the raw .style color.
    //
    new_n->background_color_override     = old->background_color_override;
    new_n->has_background_color_override = old->has_background_color_override;
    new_n->font_color_override           = old->font_color_override;
    new_n->has_font_color_override       = old->has_font_color_override;
}

//
// Sibling-index pairwise transfer for the id-less case. Old and
// new must have already had their children counted; we iterate
// by index. If types diverge at some index we stop -- the trees
// have forked at that point and deeper matching would be wrong
// more often than right.
//
static void _scene_internal__recon_pair_siblings(gui_node* old, gui_node* new_n)
{
    if (old == NULL || new_n == NULL) { return; }

    gui_node* oc = old->first_child;
    gui_node* nc = new_n->first_child;
    while (oc != NULL && nc != NULL)
    {
        if (oc->type != nc->type) { break; }
        //
        // Skip transfer if the new node has an id -- it'll be (or
        // has already been) matched via the id map; doing it here
        // too would be double-work and could race if we haven't
        // hit it in the id-walk yet.
        //
        if (nc->id[0] == 0)
        {
            _scene_internal__recon_transfer(oc, nc);
        }
        _scene_internal__recon_pair_siblings(oc, nc);
        oc = oc->next_sibling;
        nc = nc->next_sibling;
    }
}

void scene__reconcile_tree(gui_node* old_tree, gui_node* new_tree)
{
    if (old_tree == NULL || new_tree == NULL) { return; }

    //
    // Build the id -> old_node map. Capped at 256 ids; beyond that
    // we silently drop the later ones (unlikely in practice --
    // most .ui files have a dozen ids max).
    //
    _scene_internal__recon_entry map[_SCENE_INTERNAL__RECONCILE_MAP_CAP];
    int64 map_count = 0;
    _scene_internal__recon_collect_ids(old_tree, map, &map_count);

    //
    // Walk the new tree, matching by id first. For each id hit,
    // transfer state. id-less nodes piggyback on their parent's
    // subtree pairing below.
    //
    //
    // Match the root pair first (even if they have no ids; the
    // root is always the root). Then recurse.
    //
    _scene_internal__recon_transfer(old_tree, new_tree);

    //
    // Visit the new tree; every new node with an id looks itself
    // up in the old map and pulls state from there. Plain recursion
    // walks the whole tree regardless of breadth -- the previous
    // 256-slot fixed stack would silently drop ids past that bound
    // on wide trees.
    //
    _scene_internal__recon_walk_ids(new_tree, new_tree, map, map_count);

    //
    // Pairwise sibling walk for id-less nodes. Starts from the
    // root pair; each descent follows the subtree structure.
    // Skips nodes with ids (already matched above).
    //
    _scene_internal__recon_pair_siblings(old_tree, new_tree);
}

//
//===== root + input + scale =================================================
//

/**
 *current input state for the hover + press state machine.
 */

static gui_node*                     _scene_internal__root  = NULL;

//
//global ui scale factor. multiplied into every dimension by widget
//layout functions and into corner radii at draw time. 1.0 = identity.
//
static float _scene_internal__scale = 1.0f;

void scene__set_root(gui_node* root)
{
    _scene_internal__root = root;
}

gui_node* scene__root(void)
{
    return _scene_internal__root;
}

void scene__set_scale(float scale)
{
    if (scale < 0.05f)
    {
        scale = 0.05f;
    }
    if (scale > 100.0f)
    {
        scale = 100.0f;
    }
    _scene_internal__scale = scale;
}

float scene__scale(void)
{
    return _scene_internal__scale;
}

float scene__px(float logical)
{
    return logical * _scene_internal__scale;
}

//
//===== frame timing =========================================================
//
//Captured by the platform layer at the START of each tick via
//scene__begin_frame_time. Stable for the entire frame. The animator
//reads frame_delta_ms once per tick to advance per-node animation
//timers.
//
//Implementation note on the delta clamp: large gaps happen when the
//debugger is paused or the OS suspends the process. Without a clamp,
//resuming would fast-forward every running animation through its
//entire duration in a single frame -- the user would see every
//in-flight transition snap straight to its target. Capping at 100 ms
//bounds a single frame's animation advance to ~6 frames worth at
//60 Hz, which is enough for legitimate frame-rate variation but not
//enough for a debugger pause to skip an animation.
//

static int64 _scene_internal__frame_time_ms     = 0;
static int64 _scene_internal__frame_prev_time   = 0;
static int64 _scene_internal__frame_delta_ms    = 0;
//
// Deterministic-clock override for the visual-regression runner.
// When _sim_step_ms > 0, scene__begin_frame_time ignores the
// platform's real wall-clock value and instead advances the scene
// clock by exactly _sim_step_ms each tick. Without this, different
// graphics backends present at different rates (vsync timing, swap
// interval differences), so "frame 8" means different elapsed ms on
// different backends, and animations captured at frame 8 end up at
// different points. Deterministic mode eliminates that variance:
// every backend sees the same animation progress.
//
static int64 _scene_internal__sim_step_ms       = 0;
static int64 _scene_internal__sim_clock_ms      = 0;

void scene__set_deterministic_clock(int64 step_ms)
{
    _scene_internal__sim_step_ms  = step_ms;
    _scene_internal__sim_clock_ms = 0;
    //
    // Reset the delta tracker so the first tick after switching
    // modes doesn't report a gigantic delta between the real clock
    // and the synthetic one.
    //
    _scene_internal__frame_prev_time = 0;
}

void scene__begin_frame_time(int64 now_ms)
{
    //
    // If deterministic mode is active, discard the platform's
    // wall-clock reading and substitute a monotonic synthetic clock
    // that advances by step_ms each call. Produces identical
    // animation-progress readings on every tick across every
    // backend, regardless of how long the real frame took.
    //
    if (_scene_internal__sim_step_ms > 0)
    {
        _scene_internal__sim_clock_ms += _scene_internal__sim_step_ms;
        now_ms = _scene_internal__sim_clock_ms;
    }

    if (_scene_internal__frame_prev_time == 0)
    {
        //
        //first frame: no previous timestamp, so delta is 0. animator
        //simply doesn't advance on frame 1, which is fine -- new
        //nodes start at appear_age_ms = 0 anyway.
        //
        _scene_internal__frame_delta_ms = 0;
    }
    else
    {
        int64 delta = now_ms - _scene_internal__frame_prev_time;
        if (delta < 0)   { delta = 0;   } // clock went backwards (rare; system clock change).
        if (delta > 100) { delta = 100; } // clamp huge jumps (debugger pause, sleep).
        _scene_internal__frame_delta_ms = delta;
    }
    _scene_internal__frame_prev_time = now_ms;
    _scene_internal__frame_time_ms   = now_ms;
}

int64 scene__frame_time_ms(void)
{
    return _scene_internal__frame_time_ms;
}

int64 scene__frame_delta_ms(void)
{
    return _scene_internal__frame_delta_ms;
}

//
//===== overlay (top-most popup layer) =======================================
//
//Single-slot overlay used by widgets that need to draw ABOVE the rest
//of the tree (currently only <select>'s dropdown menu). The tree-walk
//draw order is depth-first depth, so a widget's emit_draws/_post fires
//at the widget's tree position -- meaning siblings drawn later cover
//it. The overlay path bypasses that: we record (node, bounds, draw_fn)
//here, scene__emit_draws calls draw_fn AFTER the normal walk is done,
//and scene__hit_test routes clicks inside bounds to the overlay node
//instead of whatever's spatially under the cursor.
//
//Only ONE overlay can be active at a time. Opening a second
//auto-closes the first (which matches how native OS dropdowns
//behave -- only ever one menu open).
//

static gui_node*           _scene_internal__overlay_node     = NULL;
static gui_rect            _scene_internal__overlay_bounds   = { 0.0f, 0.0f, 0.0f, 0.0f };
static gui_overlay_draw_fn _scene_internal__overlay_draw_fn  = NULL;

//
// Defensive clear of every file-scope slot that might point at a
// node about to be freed. Called from scene__node_free at the top
// of the destruction path so the next emit_draws / hit-test walk
// can't dereference dangling pointers. Each clear is independent
// so a node that was, say, both the overlay owner AND focused
// gets BOTH slots cleared.
//
// Definition lives down here (rather than next to scene__node_free
// up at line ~250) because every static it touches is declared in
// THIS region of the file. Forward declaration at the top of the
// TU lets scene__node_free call it.
//
static void _scene_internal__on_node_freed(gui_node* node)
{
    if (node == NULL) { return; }

    if (_scene_internal__overlay_node == node)
    {
        _scene_internal__overlay_node    = NULL;
        _scene_internal__overlay_draw_fn = NULL;
    }
    if (_scene_internal__root == node)
    {
        _scene_internal__root = NULL;
    }
    scene_input__on_node_freed(node);
}

void scene__set_overlay(gui_node* node, gui_rect bounds, gui_overlay_draw_fn draw_fn)
{
    _scene_internal__overlay_node    = node;
    _scene_internal__overlay_bounds  = bounds;
    _scene_internal__overlay_draw_fn = draw_fn;
}

gui_node* scene__overlay_node(void)
{
    return _scene_internal__overlay_node;
}

//
//
//===== style entry point =====================================================
//
//Heavy lifting lives in scene_style.c. scene.c keeps the root
//pointer + calls in once per frame. See scene_style.h.
//

void scene__resolve_styles(void)
{
    if (_scene_internal__root == NULL) { return; }
    scene_style__apply_rules_tree(_scene_internal__root);
    scene_style__resolve_tree(_scene_internal__root);
}

//
//===== draw emission entry point ============================================
//
//Heavy lifting lives in scene_render.c. scene.c retains this
//orchestration because it also drives the overlay pass.
//

void scene__emit_draws(void)
{
    if (_scene_internal__root == NULL) { return; }
    scene_render__emit_tree(_scene_internal__root, _scene_internal__scale);

    //
    //Overlay pass. Lands ON TOP of the normal walk so a <select>
    //dropdown etc. sits above its siblings in the tree.
    //
    if (_scene_internal__overlay_node != NULL && _scene_internal__overlay_draw_fn != NULL)
    {
        _scene_internal__overlay_draw_fn(_scene_internal__overlay_node, _scene_internal__scale);
    }
}

//===== hit testing ==========================================================
//

static boole _scene_internal__rect_contains(gui_rect r, float x, float y)
{
    return (boole)(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
}

static gui_node* _scene_internal__hit_recursive(gui_node* n, float x, float y)
{
    if (n == NULL)
    {
        return NULL;
    }
    //
    // display:none and visibility:hidden nodes must NOT be hit-tested.
    // They're not drawn either (emit_recursive early-outs on them), so
    // clicking over one would otherwise reach children that the user
    // cannot see. Without this check, a hidden shell-column under a
    // visible fullscreen-view steals every click that lands outside
    // the fullscreen-view's own widget bounds.
    //
    if (n->resolved.display    == GUI_DISPLAY_NONE)      { return NULL; }
    if (n->resolved.visibility == GUI_VISIBILITY_HIDDEN) { return NULL; }
    if (!_scene_internal__rect_contains(n->bounds, x, y))
    {
        return NULL;
    }

    //
    //scrollbar-strip priority. if this node is a scrollable container
    //and the click lands inside the rightmost bar_size pixels of its
    //bounds, route the click to THIS node regardless of whether any
    //child would otherwise match deeper. without this, pinned-width
    //children (e.g. a Column with size_w = 380 px scaled up past the
    //parent's content_w) extend past the scrollbar strip at large
    //scale, and the deepest-wins rule below sends clicks to the
    //column -- which has no scroll handler, so the thumb becomes
    //unclickable even though wheel scrolling still works (wheel
    //walks the parent chain explicitly in scene__on_mouse_wheel).
    //
    //applies to both AUTO and SCROLL overflow modes. VISIBLE / HIDDEN
    //don't draw a bar so there's nothing to intercept.
    //
    if (scroll__vbar_visible(n))
    {
        float bar_size  = scroll__bar_size(n, _scene_internal__scale);
        float strip_x0  = n->bounds.x + n->bounds.w - bar_size;
        float strip_x1  = n->bounds.x + n->bounds.w;
        if (x >= strip_x0 && x < strip_x1 &&
            y >= n->bounds.y && y < n->bounds.y + n->bounds.h)
        {
            return n;
        }
    }

    gui_node* best = n;
    gui_node* c    = n->first_child;
    while (c != NULL)
    {
        gui_node* child_hit = _scene_internal__hit_recursive(c, x, y);
        if (child_hit != NULL)
        {
            best = child_hit;
        }
        c = c->next_sibling;
    }
    return best;
}

gui_node* scene__hit_test(gui_node* root, int64 x, int64 y)
{
    //
    //Overlay takes priority over the normal tree walk. When a
    //<select>'s dropdown is open and the cursor lands inside the
    //popup's projected bounds, we route the click to the select
    //(its on_mouse_down figures out which option was hit from x/y).
    //Without this the click would fall through to whatever sibling
    //the popup is visually covering.
    //
    if (_scene_internal__overlay_node != NULL)
    {
        gui_rect ob = _scene_internal__overlay_bounds;
        if ((float)x >= ob.x && (float)x < ob.x + ob.w &&
            (float)y >= ob.y && (float)y < ob.y + ob.h)
        {
            return _scene_internal__overlay_node;
        }
    }
    return _scene_internal__hit_recursive(root, (float)x, (float)y);
}

//
//===== event dispatch =======================================================
//

void scene__dispatch_click(gui_node* n, int64 x, int64 y, int64 button)
{
    if (n == NULL) { return; }
    //
    // Walk UP from the deepest hit until we find a node with
    // on_click set. Lets a host put the handler on a parent (e.g.
    // a tile column) and have clicks anywhere in the tile's
    // subtree dispatch through that single handler. Without this
    // walk, on_click had to be set on every clickable leaf of a
    // tile (icon-frame, image, label, etc.) which made the press-
    // group state-chain logic in scene_input awkward.
    //
    // ev.sender is set to the node that ACTUALLY carries the
    // handler (not the deepest hit) so the handler sees the click-
    // target, not the leaf decoration that intercepted the press.
    //
    gui_node* target = n;
    while (target != NULL && target->on_click_hash == 0 && target->parent != NULL)
    {
        target = target->parent;
    }
    if (target == NULL || target->on_click_hash == 0)
    {
        return;
    }
    gui_handler_fn fn = _scene_internal__resolve_handler(target->on_click_hash, target->on_click_name);
    if (fn == NULL)
    {
        log_warn("on_click handler not found: name='%s' hash=%u", target->on_click_name, target->on_click_hash);
        return;
    }
    gui_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type         = GUI_EVENT_CLICK;
    ev.sender       = target;
    ev.mouse.x      = x;
    ev.mouse.y      = y;
    ev.mouse.button = button;
    fn(&ev);
}

//
// Fire a window-level handler by name. Unlike dispatch_click, this
// doesn't involve a scene node -- the caller has the handler name
// directly (gesture recognizer calls with "on_swipe_up_bottom" etc.).
// Looks up by hash in the same registry used by on_click/on_change;
// silently no-ops if the handler isn't registered (user's UI just
// doesn't care about this gesture).
//
void scene__dispatch_gesture_by_name(const char* handler_name)
{
    if (handler_name == NULL || handler_name[0] == 0) { return; }
    uint hash = scene__hash_name((char*)handler_name);
    gui_handler_fn fn = _scene_internal__resolve_handler(hash, (char*)handler_name);
    if (fn == NULL)
    {
        //
        // Logged at info so it's visible by default during
        // development. If a user .ui simply doesn't bind a
        // gesture, this is "harmless" but confusing without the
        // log line. When the shell stabilizes we can drop this
        // back to trace.
        //
        log_info("scene__dispatch_gesture_by_name: no handler registered for '%s'", handler_name);
        return;
    }
    gui_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type   = GUI_EVENT_CLICK;  //closest existing type; ev.sender=NULL signals global.
    ev.sender = NULL;
    fn(&ev);
}

void scene__dispatch_change(gui_node* n, float new_value)
{
    if (n == NULL || n->on_change_hash == 0)
    {
        return;
    }
    gui_handler_fn fn = _scene_internal__resolve_handler(n->on_change_hash, n->on_change_name);
    if (fn == NULL)
    {
        log_warn("on_change handler not found: name='%s' hash=%u", n->on_change_name, n->on_change_hash);
        return;
    }
    gui_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type          = GUI_EVENT_CHANGE;
    ev.sender        = n;
    ev.change.scalar = new_value;
    fn(&ev);
}

void scene__dispatch_event(gui_node* n, gui_event* ev)
{
    if (n == NULL || ev == NULL)
    {
        return;
    }
    //
    // The event's type decides which handler to resolve. Same
    // fallback path as scene__dispatch_click / scene__dispatch_change:
    // the scene's handler table first, then the platform symbol
    // resolver (GetProcAddress / dlsym) if the named handler was
    // exported via UI_HANDLER.
    //
    uint  hash = 0;
    char* name = NULL;
    if (ev->type == GUI_EVENT_CLICK)
    {
        hash = n->on_click_hash;
        name = n->on_click_name;
    }
    else
    {
        hash = n->on_change_hash;
        name = n->on_change_name;
    }
    if (hash == 0) { return; }
    gui_handler_fn fn = _scene_internal__resolve_handler(hash, name);
    if (fn == NULL)
    {
        log_warn("handler not found: name='%s' hash=%u", name, hash);
        return;
    }
    ev->sender = n;
    fn(ev);
}

//

//
//===== id lookup ===========================================================
//
//Left in scene.c (not scene_input.c) because id lookup is tree-walk
//utility, not input-state. Used by hot_reload and meek-shell's
//meek_shell_v1_client to resolve a named widget at runtime.
//

static gui_node* _scene_internal__find_by_id_recursive(gui_node* n, char* id)
{
    if (n == NULL) { return NULL; }
    if (n->id[0] != 0 && strcmp(n->id, id) == 0) { return n; }
    gui_node* c = n->first_child;
    while (c != NULL)
    {
        gui_node* hit = _scene_internal__find_by_id_recursive(c, id);
        if (hit != NULL) { return hit; }
        c = c->next_sibling;
    }
    return NULL;
}

gui_node* scene__find_by_id(char* id)
{
    if (id == NULL || id[0] == 0 || _scene_internal__root == NULL) { return NULL; }
    return _scene_internal__find_by_id_recursive(_scene_internal__root, id);
}
