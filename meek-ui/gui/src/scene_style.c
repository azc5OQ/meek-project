//
//scene_style.c -- selector-based style registration + two-pass
//resolution, split out of scene.c for readability. Owns the rule
//table + all selector parsing, class-set hashing, rule matching,
//and per-node style overlay logic.
//
//Public API (scene__register_style, scene__clear_styles,
//scene__set_background_color_override, etc.) stays declared in
//scene.h; this TU implements it. The entry point scene.c calls
//every frame is scene_style__apply_rules_tree +
//scene_style__resolve_tree -- see scene_style.h.
//

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scene_style.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

//
//===== selector-based style registration ====================================
//
//each registered rule has a parsed selector (optional type, optional
//class, optional id, optional pseudo-state) and a gui_style payload.
//rules are applied in three specificity passes:
//    pass 0: type-only rules
//    pass 1: rules with a class
//    pass 2: rules with an id
//within one pass, registration order decides ties (later wins).
//

#define _SCENE_STYLE_INTERNAL__MAX_STYLES 256

/**
 *one parsed style-registration row. specificity tier and per-state
 *destination are precomputed at registration time so application is
 *just a sweep of three passes.
 */
typedef struct _scene_style_internal__style_rule
{
    char           type[32];
    char           klass[32];
    char           id[32];
    gui_node_state state;
    int64          specificity;
    gui_style      style;
    gui_node_type  type_enum;
    uint           klass_hash;
    uint           id_hash;
} _scene_style_internal__style_rule;

static _scene_style_internal__style_rule _scene_style_internal__styles[_SCENE_STYLE_INTERNAL__MAX_STYLES];
static int64                             _scene_style_internal__style_count = 0;

//
//tiny char tests so we don't drag in <ctype.h>.
//
static boole _scene_style_internal__is_alpha(char c)
{
    return (boole)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}
static boole _scene_style_internal__is_alnum(char c)
{
    return (boole)(_scene_style_internal__is_alpha(c) || (c >= '0' && c <= '9') || c == '-');
}

static void _scene_style_internal__copy_token(char* start, char* end, char* out, int64 cap)
{
    int64 n = (int64)(end - start);
    if (n >= cap) { n = cap - 1; }
    memcpy(out, start, (size_t)n);
    out[n] = 0;
}

static boole _scene_style_internal__parse_selector(char* sel, _scene_style_internal__style_rule* out)
{
    out->type[0]  = 0;
    out->klass[0] = 0;
    out->id[0]    = 0;
    out->state    = GUI_STATE_DEFAULT;

    char* p   = sel;
    char* end = sel + strlen(sel);

    char* colon = NULL;
    for (char* q = p; q < end; q++)
    {
        if (*q == ':') { colon = q; break; }
    }
    if (colon != NULL)
    {
        char* ps = colon + 1;
        if      (strcmp(ps, "hover")     == 0) { out->state = GUI_STATE_HOVER; }
        else if (strcmp(ps, "pressed")   == 0 || strcmp(ps, "active") == 0) { out->state = GUI_STATE_PRESSED; }
        else if (strcmp(ps, "disabled")  == 0) { out->state = GUI_STATE_DISABLED; }
        else if (strcmp(ps, "default")   == 0) { out->state = GUI_STATE_DEFAULT; }
        else if (strcmp(ps, "appear")    == 0) { out->state = GUI_STATE_APPEAR; }
        else if (strcmp(ps, "disappear") == 0 || strcmp(ps, "dissapear") == 0) { out->state = GUI_STATE_DISAPPEAR; }
        else
        {
            log_warn("unknown pseudo-state ':%s'", ps);
            return FALSE;
        }
        end = colon;
    }

    if (p < end && _scene_style_internal__is_alpha(*p))
    {
        char* start = p;
        while (p < end && _scene_style_internal__is_alnum(*p)) { p++; }
        _scene_style_internal__copy_token(start, p, out->type, (int64)sizeof(out->type));
    }

    while (p < end)
    {
        if (*p == '.')
        {
            p++;
            char* start = p;
            while (p < end && _scene_style_internal__is_alnum(*p)) { p++; }
            if (start == p)
            {
                log_warn("empty class after '.'");
                return FALSE;
            }
            _scene_style_internal__copy_token(start, p, out->klass, (int64)sizeof(out->klass));
        }
        else if (*p == '#')
        {
            p++;
            char* start = p;
            while (p < end && _scene_style_internal__is_alnum(*p)) { p++; }
            if (start == p)
            {
                log_warn("empty id after '#'");
                return FALSE;
            }
            _scene_style_internal__copy_token(start, p, out->id, (int64)sizeof(out->id));
        }
        else
        {
            log_warn("unexpected '%c' in selector '%s'", *p, sel);
            return FALSE;
        }
    }

    if      (out->id[0]    != 0) { out->specificity = 2; }
    else if (out->klass[0] != 0) { out->specificity = 1; }
    else                         { out->specificity = 0; }

    if (out->type[0] != 0)
    {
        gui_node_type te = GUI_NODE_TYPE_COUNT;
        if (widget_registry__lookup_by_name(out->type, &te)) { out->type_enum = te; }
        else                                                  { out->type_enum = GUI_NODE_TYPE_COUNT; }
    }
    else
    {
        out->type_enum = GUI_NODE_TYPE_COUNT;
    }
    out->klass_hash = (out->klass[0] != 0) ? scene__hash_name(out->klass) : 0u;
    out->id_hash    = (out->id[0]    != 0) ? scene__hash_name(out->id)    : 0u;

    if (out->type[0] == 0 && out->klass[0] == 0 && out->id[0] == 0)
    {
        log_warn("empty selector");
        return FALSE;
    }
    return TRUE;
}

void scene__register_style(char* selector, const gui_style* style)
{
    if (selector == NULL || style == NULL) { return; }
    if (_scene_style_internal__style_count >= _SCENE_STYLE_INTERNAL__MAX_STYLES)
    {
        log_error("scene__register_style: table full (%d)", _SCENE_STYLE_INTERNAL__MAX_STYLES);
        return;
    }
    _scene_style_internal__style_rule rule;
    memset(&rule, 0, sizeof(rule));
    if (!_scene_style_internal__parse_selector(selector, &rule)) { return; }
    rule.style = *style;
    _scene_style_internal__styles[_scene_style_internal__style_count++] = rule;
}

void scene__clear_styles(void)
{
    _scene_style_internal__style_count = 0;
}

//
// Multi-class support. A node's klass field stores the literal
// `class="..."` attribute string ("primary big-button"); we tokenize
// on whitespace into up to 8 individual hashes so the rule's
// klass_hash can match ANY of them.
//
#define _SCENE_STYLE_INTERNAL__MAX_CLASS_TOKENS 8

typedef struct _scene_style_internal__class_set
{
    uint  hashes[_SCENE_STYLE_INTERNAL__MAX_CLASS_TOKENS];
    int64 count;
} _scene_style_internal__class_set;

static void _scene_style_internal__build_class_set(const char* klass, _scene_style_internal__class_set* out)
{
    out->count = 0;
    if (klass == NULL || klass[0] == 0) { return; }

    const char* p = klass;
    while (*p != 0)
    {
        while (*p == ' ' || *p == '\t') { p++; }
        if (*p == 0) { break; }
        const char* tok_start = p;
        while (*p != 0 && *p != ' ' && *p != '\t') { p++; }
        if (out->count >= _SCENE_STYLE_INTERNAL__MAX_CLASS_TOKENS) { return; }
        //
        // fnv-1a 32-bit inline. Mirrors scene__hash_name exactly so
        // a rule's klass_hash (computed via scene__hash_name at parse
        // time) compares cleanly.
        //
        uint h = 2166136261u;
        for (const char* c = tok_start; c < p; c++)
        {
            h ^= (uint)(unsigned char)*c;
            h *= 16777619u;
        }
        out->hashes[out->count++] = h;
    }
}

static boole _scene_style_internal__class_set_contains(const _scene_style_internal__class_set* set, uint h)
{
    for (int64 i = 0; i < set->count; i++)
    {
        if (set->hashes[i] == h) { return TRUE; }
    }
    return FALSE;
}

static boole _scene_style_internal__rule_matches(gui_node* n, const _scene_style_internal__style_rule* r, const _scene_style_internal__class_set* node_classes, uint node_id_hash)
{
    if (r->type_enum != GUI_NODE_TYPE_COUNT && r->type_enum != n->type) { return FALSE; }
    if (r->klass_hash != 0 && !_scene_style_internal__class_set_contains(node_classes, r->klass_hash)) { return FALSE; }
    if (r->id_hash    != 0 && r->id_hash != node_id_hash) { return FALSE; }
    return TRUE;
}

static void _scene_style_internal__overlay_style(gui_style* dst, const gui_style* src)
{
    if (src->has_background_color) { dst->background_color = src->background_color; dst->has_background_color = TRUE; }
    if (src->has_accent_color)     { dst->accent_color     = src->accent_color;     dst->has_accent_color     = TRUE; }
    if (src->has_font_color)       { dst->font_color       = src->font_color;       dst->has_font_color       = TRUE; }

    if (src->border_width > 0.0f)  { dst->border_width = src->border_width; }
    if (src->has_border_color)     { dst->border_color = src->border_color; dst->has_border_color = TRUE; }
    if (src->has_border_gradient)
    {
        dst->border_gradient_from = src->border_gradient_from;
        dst->border_gradient_to   = src->border_gradient_to;
        dst->border_gradient_dir  = src->border_gradient_dir;
        dst->has_border_gradient  = TRUE;
    }
    if (src->border_style != GUI_BORDER_NONE) { dst->border_style = src->border_style; }
    if (src->font_family[0] != 0)             { memcpy(dst->font_family, src->font_family, sizeof(dst->font_family)); }
    if (src->font_size > 0.0f)                { dst->font_size = src->font_size; }
    if (src->font_size_explicit)              { dst->font_size_explicit = TRUE; }
    if (src->radius    > 0.0f)                { dst->radius    = src->radius; }
    if (src->pad_t     > 0.0f)                { dst->pad_t     = src->pad_t; }
    if (src->pad_r     > 0.0f)                { dst->pad_r     = src->pad_r; }
    if (src->pad_b     > 0.0f)                { dst->pad_b     = src->pad_b; }
    if (src->pad_l     > 0.0f)                { dst->pad_l     = src->pad_l; }
    if (src->gap       > 0.0f)                { dst->gap       = src->gap; }
    if (src->size_w    > 0.0f)                { dst->size_w    = src->size_w; }
    if (src->size_h    > 0.0f)                { dst->size_h    = src->size_h; }
    if (src->size_w_explicit)                 { dst->size_w_explicit = TRUE; }
    if (src->size_h_explicit)                 { dst->size_h_explicit = TRUE; }
    if (src->width_pct > 0.0f)                { dst->width_pct = src->width_pct; }
    if (src->height_pct> 0.0f)                { dst->height_pct= src->height_pct; }
    if (src->min_w     > 0.0f)                { dst->min_w     = src->min_w; }
    if (src->min_h     > 0.0f)                { dst->min_h     = src->min_h; }

    if (src->overflow_x != GUI_OVERFLOW_VISIBLE) { dst->overflow_x = src->overflow_x; }
    if (src->overflow_y != GUI_OVERFLOW_VISIBLE) { dst->overflow_y = src->overflow_y; }
    if (src->scrollbar_size   > 0.0f)            { dst->scrollbar_size   = src->scrollbar_size; }
    if (src->scrollbar_radius > 0.0f)            { dst->scrollbar_radius = src->scrollbar_radius; }
    if (src->has_scrollbar_track) { dst->scrollbar_track = src->scrollbar_track; dst->has_scrollbar_track = TRUE; }
    if (src->has_scrollbar_thumb) { dst->scrollbar_thumb = src->scrollbar_thumb; dst->has_scrollbar_thumb = TRUE; }

    if (src->visibility != GUI_VISIBILITY_VISIBLE) { dst->visibility = src->visibility; }
    if (src->display    != GUI_DISPLAY_BLOCK)      { dst->display    = src->display; }
    if (src->position   != GUI_POSITION_STATIC)    { dst->position   = src->position; }
    if (src->inset_t      > 0.0f)                  { dst->inset_t      = src->inset_t; }
    if (src->inset_r      > 0.0f)                  { dst->inset_r      = src->inset_r; }
    if (src->inset_b      > 0.0f)                  { dst->inset_b      = src->inset_b; }
    if (src->inset_l      > 0.0f)                  { dst->inset_l      = src->inset_l; }
    if (src->inset_t_pct  > 0.0f)                  { dst->inset_t_pct  = src->inset_t_pct; }
    if (src->inset_r_pct  > 0.0f)                  { dst->inset_r_pct  = src->inset_r_pct; }
    if (src->inset_b_pct  > 0.0f)                  { dst->inset_b_pct  = src->inset_b_pct; }
    if (src->inset_l_pct  > 0.0f)                  { dst->inset_l_pct  = src->inset_l_pct; }

    if (src->scroll_smooth_ms > 0.0f) { dst->scroll_smooth_ms = src->scroll_smooth_ms; }
    if (src->scroll_fade_px   > 0.0f) { dst->scroll_fade_px   = src->scroll_fade_px; }

    if (src->appear_ms > 0.0f)
    {
        dst->appear_ms     = src->appear_ms;
        dst->appear_easing = src->appear_easing;
        dst->appear_easing_params[0] = src->appear_easing_params[0];
        dst->appear_easing_params[1] = src->appear_easing_params[1];
        dst->appear_easing_params[2] = src->appear_easing_params[2];
        dst->appear_easing_params[3] = src->appear_easing_params[3];
    }

    if (src->transition_duration_ms > 0.0f)
    {
        dst->transition_duration_ms      = src->transition_duration_ms;
        dst->transition_easing           = src->transition_easing;
        dst->transition_easing_params[0] = src->transition_easing_params[0];
        dst->transition_easing_params[1] = src->transition_easing_params[1];
        dst->transition_easing_params[2] = src->transition_easing_params[2];
        dst->transition_easing_params[3] = src->transition_easing_params[3];
    }

    if (src->has_shadow)
    {
        dst->shadow_dx    = src->shadow_dx;
        dst->shadow_dy    = src->shadow_dy;
        dst->shadow_blur  = src->shadow_blur;
        dst->shadow_color = src->shadow_color;
        dst->has_shadow   = TRUE;
    }

    if (src->has_opacity) { dst->opacity = src->opacity; dst->has_opacity = TRUE; }
    if (src->z_index != 0) { dst->z_index = src->z_index; }
    if (src->blur_px > 0.0f) { dst->blur_px = src->blur_px; }

    if (src->has_bg_gradient)
    {
        dst->bg_gradient_from = src->bg_gradient_from;
        dst->bg_gradient_to   = src->bg_gradient_to;
        dst->bg_gradient_dir  = src->bg_gradient_dir;
        dst->has_bg_gradient  = TRUE;
    }

    if (src->has_background_image)
    {
        memcpy(dst->background_image, src->background_image, sizeof(dst->background_image));
        dst->has_background_image = TRUE;
    }
    if (src->background_size != GUI_FIT_FILL) { dst->background_size = src->background_size; }
    if (src->object_fit      != GUI_FIT_FILL) { dst->object_fit      = src->object_fit; }

    if (src->halign != GUI_HALIGN_LEFT) { dst->halign = src->halign; }
    if (src->valign != GUI_VALIGN_TOP)  { dst->valign = src->valign; }

    if (src->collection_layout  != GUI_COLLECTION_GRID) { dst->collection_layout  = src->collection_layout; }
    if (src->collection_columns > 0)                    { dst->collection_columns = src->collection_columns; }
    if (src->item_width         > 0.0f)                 { dst->item_width         = src->item_width; }
    if (src->item_height        > 0.0f)                 { dst->item_height        = src->item_height; }
}

static void _scene_style_internal__apply_rules_to_node(gui_node* n)
{
    //
    // See long-form comment in the pre-split scene.c for why we
    // preserve visibility + display across the memset: handlers
    // mutate them between frames, and a naive wipe would clobber.
    //
    gui_visibility saved_vis  = n->style[GUI_STATE_DEFAULT].visibility;
    gui_display    saved_disp = n->style[GUI_STATE_DEFAULT].display;

    //
    // Preserve runtime-driven absolute-positioning fields the same
    // way visibility + display are preserved. main.c animates a
    // task-switcher overlay's inset_t each tick to follow the
    // user's swipe-up progress; without this preservation the rule
    // re-apply below wipes the writes and the overlay snaps back to
    // its rule-defined inset every frame.
    //
    gui_position saved_pos    = n->style[GUI_STATE_DEFAULT].position;
    float        saved_in_t   = n->style[GUI_STATE_DEFAULT].inset_t;
    float        saved_in_r   = n->style[GUI_STATE_DEFAULT].inset_r;
    float        saved_in_b   = n->style[GUI_STATE_DEFAULT].inset_b;
    float        saved_in_l   = n->style[GUI_STATE_DEFAULT].inset_l;
    boole        saved_has_op = n->style[GUI_STATE_DEFAULT].has_opacity;
    float        saved_op     = n->style[GUI_STATE_DEFAULT].opacity;
    //
    // Preserve runtime-driven width/height so main.c can animate
    // sizing fields on the task-switcher's cards-deck (grow-from-
    // small effect during the swipe-progress reveal). Same memset-
    // restore pattern as display + opacity above.
    //
    float        saved_w      = n->style[GUI_STATE_DEFAULT].size_w;
    float        saved_h      = n->style[GUI_STATE_DEFAULT].size_h;
    boole        saved_w_expl = n->style[GUI_STATE_DEFAULT].size_w_explicit;
    boole        saved_h_expl = n->style[GUI_STATE_DEFAULT].size_h_explicit;
    float        saved_w_pct  = n->style[GUI_STATE_DEFAULT].width_pct;
    float        saved_h_pct  = n->style[GUI_STATE_DEFAULT].height_pct;
    //
    // Preserve runtime-driven background color so per-tile color
    // sampling (icon_color_sampler in meek-shell) survives the
    // per-frame style resolve. Otherwise the .style rules don't
    // declare these and the memset wipes them between ticks.
    //
    boole     saved_has_bg = n->style[GUI_STATE_DEFAULT].has_background_color;
    gui_color saved_bg     = n->style[GUI_STATE_DEFAULT].background_color;
    //
    // Same story for the runtime-driven gradient: meek-shell sets
    // bg_gradient_from / _to / _dir on per-tile launcher frames so
    // the tile reads as a soft vertical fade. Without preserving
    // here those wipes back to zero each frame.
    //
    boole            saved_has_grad = n->style[GUI_STATE_DEFAULT].has_bg_gradient;
    gui_color        saved_grad_from = n->style[GUI_STATE_DEFAULT].bg_gradient_from;
    gui_color        saved_grad_to   = n->style[GUI_STATE_DEFAULT].bg_gradient_to;
    gui_gradient_dir saved_grad_dir  = n->style[GUI_STATE_DEFAULT].bg_gradient_dir;
    //
    // Preserve the PRESSED-state gradient too. Hosts (meek-shell)
    // may set per-tile pressed gradients programmatically (e.g. a
    // darker variant of the sampled DEFAULT gradient) so the
    // animator can interpolate between them on press. Without
    // preserving across the memset, the PRESSED slot gets wiped
    // each frame and no .style rule re-fills it (the press
    // state's gradient is set per-instance, not via class rules).
    //
    boole            saved_has_grad_p  = n->style[GUI_STATE_PRESSED].has_bg_gradient;
    gui_color        saved_grad_from_p = n->style[GUI_STATE_PRESSED].bg_gradient_from;
    gui_color        saved_grad_to_p   = n->style[GUI_STATE_PRESSED].bg_gradient_to;
    gui_gradient_dir saved_grad_dir_p  = n->style[GUI_STATE_PRESSED].bg_gradient_dir;

    if (_scene_style_internal__style_count > 0)
    {
        memset(n->style, 0, sizeof(n->style));
    }

    n->style[GUI_STATE_DEFAULT].visibility            = saved_vis;
    n->style[GUI_STATE_DEFAULT].display               = saved_disp;
    n->style[GUI_STATE_DEFAULT].position              = saved_pos;
    n->style[GUI_STATE_DEFAULT].inset_t               = saved_in_t;
    n->style[GUI_STATE_DEFAULT].inset_r               = saved_in_r;
    n->style[GUI_STATE_DEFAULT].inset_b               = saved_in_b;
    n->style[GUI_STATE_DEFAULT].inset_l               = saved_in_l;
    n->style[GUI_STATE_DEFAULT].has_opacity           = saved_has_op;
    n->style[GUI_STATE_DEFAULT].opacity               = saved_op;
    n->style[GUI_STATE_DEFAULT].size_w                = saved_w;
    n->style[GUI_STATE_DEFAULT].size_h                = saved_h;
    n->style[GUI_STATE_DEFAULT].size_w_explicit       = saved_w_expl;
    n->style[GUI_STATE_DEFAULT].size_h_explicit       = saved_h_expl;
    n->style[GUI_STATE_DEFAULT].width_pct             = saved_w_pct;
    n->style[GUI_STATE_DEFAULT].height_pct            = saved_h_pct;
    n->style[GUI_STATE_DEFAULT].has_background_color  = saved_has_bg;
    n->style[GUI_STATE_DEFAULT].background_color      = saved_bg;
    n->style[GUI_STATE_DEFAULT].has_bg_gradient       = saved_has_grad;
    n->style[GUI_STATE_DEFAULT].bg_gradient_from      = saved_grad_from;
    n->style[GUI_STATE_DEFAULT].bg_gradient_to        = saved_grad_to;
    n->style[GUI_STATE_DEFAULT].bg_gradient_dir       = saved_grad_dir;
    n->style[GUI_STATE_PRESSED].has_bg_gradient       = saved_has_grad_p;
    n->style[GUI_STATE_PRESSED].bg_gradient_from      = saved_grad_from_p;
    n->style[GUI_STATE_PRESSED].bg_gradient_to        = saved_grad_to_p;
    n->style[GUI_STATE_PRESSED].bg_gradient_dir       = saved_grad_dir_p;

    _scene_style_internal__class_set node_classes;
    _scene_style_internal__build_class_set(n->klass, &node_classes);
    uint node_id_hash = (n->id[0] != 0) ? scene__hash_name(n->id) : 0u;

    for (int64 spec = 0; spec <= 2; spec++)
    {
        for (int64 i = 0; i < _scene_style_internal__style_count; i++)
        {
            _scene_style_internal__style_rule* r = &_scene_style_internal__styles[i];
            if (r->specificity != spec) { continue; }
            if (!_scene_style_internal__rule_matches(n, r, &node_classes, node_id_hash)) { continue; }
            _scene_style_internal__overlay_style(&n->style[r->state], &r->style);
        }
    }
}

static void _scene_style_internal__apply_rules_recursive(gui_node* n)
{
    _scene_style_internal__apply_rules_to_node(n);

    gui_node* c = n->first_child;
    while (c != NULL)
    {
        _scene_style_internal__apply_rules_recursive(c);
        c = c->next_sibling;
    }
}

void scene_style__apply_rules_tree(gui_node* root)
{
    if (root == NULL) { return; }
    _scene_style_internal__apply_rules_recursive(root);
}

//
//===== style resolution =====================================================
//

static void _scene_style_internal__resolve_recursive(gui_node* n)
{
    gui_style* base = &n->style[GUI_STATE_DEFAULT];
    gui_style* cur  = &n->style[n->state];

    n->resolved = *base;
    //
    // Overlay per-state slot onto resolved. Was previously only
    // background_color; the rest of the fields below were being
    // silently ignored, which broke `:hover` / `:active` rules
    // that touched opacity, accent color, font color, gradient,
    // border, radius, etc.
    //
    if (cur->has_background_color)
    {
        n->resolved.background_color     = cur->background_color;
        n->resolved.has_background_color = TRUE;
    }
    if (cur->has_bg_gradient)
    {
        n->resolved.bg_gradient_from = cur->bg_gradient_from;
        n->resolved.bg_gradient_to   = cur->bg_gradient_to;
        n->resolved.bg_gradient_dir  = cur->bg_gradient_dir;
        n->resolved.has_bg_gradient  = TRUE;
    }
    if (cur->has_opacity)
    {
        n->resolved.opacity     = cur->opacity;
        n->resolved.has_opacity = TRUE;
    }
    if (cur->has_font_color)
    {
        n->resolved.font_color     = cur->font_color;
        n->resolved.has_font_color = TRUE;
    }
    if (cur->has_accent_color)
    {
        n->resolved.accent_color     = cur->accent_color;
        n->resolved.has_accent_color = TRUE;
    }
    if (cur->has_border_color)
    {
        n->resolved.border_color     = cur->border_color;
        n->resolved.has_border_color = TRUE;
    }
    if (cur->border_width > 0.0f) { n->resolved.border_width = cur->border_width; }
    if (cur->radius > 0.0f)       { n->resolved.radius       = cur->radius; }
    //
    // Box-model + sizing + typography overlays. Same "non-default
    // value wins" pattern used by the rule-overlay code in
    // _overlay_style. Lets `:hover { padding: 16px }` etc. actually
    // change the layout when the state matches.
    //
    if (cur->pad_t      > 0.0f) { n->resolved.pad_t      = cur->pad_t; }
    if (cur->pad_r      > 0.0f) { n->resolved.pad_r      = cur->pad_r; }
    if (cur->pad_b      > 0.0f) { n->resolved.pad_b      = cur->pad_b; }
    if (cur->pad_l      > 0.0f) { n->resolved.pad_l      = cur->pad_l; }
    if (cur->margin_t   > 0.0f) { n->resolved.margin_t   = cur->margin_t; }
    if (cur->margin_r   > 0.0f) { n->resolved.margin_r   = cur->margin_r; }
    if (cur->margin_b   > 0.0f) { n->resolved.margin_b   = cur->margin_b; }
    if (cur->margin_l   > 0.0f) { n->resolved.margin_l   = cur->margin_l; }
    if (cur->gap        > 0.0f) { n->resolved.gap        = cur->gap; }
    if (cur->size_w     > 0.0f) { n->resolved.size_w     = cur->size_w; }
    if (cur->size_h     > 0.0f) { n->resolved.size_h     = cur->size_h; }
    if (cur->size_w_explicit)   { n->resolved.size_w_explicit = TRUE; }
    if (cur->size_h_explicit)   { n->resolved.size_h_explicit = TRUE; }
    if (cur->width_pct  > 0.0f) { n->resolved.width_pct  = cur->width_pct; }
    if (cur->height_pct > 0.0f) { n->resolved.height_pct = cur->height_pct; }
    if (cur->min_w      > 0.0f) { n->resolved.min_w      = cur->min_w; }
    if (cur->min_h      > 0.0f) { n->resolved.min_h      = cur->min_h; }
    if (cur->font_size  > 0.0f) { n->resolved.font_size  = cur->font_size; }
    if (cur->font_size_explicit){ n->resolved.font_size_explicit = TRUE; }
    if (cur->font_family[0] != 0)
    {
        memcpy(n->resolved.font_family, cur->font_family, sizeof(n->resolved.font_family));
    }
    //
    // Box-shadow. Only swap if the state slot has its own shadow
    // (has_shadow guard, mirrors background_color pattern).
    //
    if (cur->has_shadow)
    {
        n->resolved.shadow_dx    = cur->shadow_dx;
        n->resolved.shadow_dy    = cur->shadow_dy;
        n->resolved.shadow_blur  = cur->shadow_blur;
        n->resolved.shadow_color = cur->shadow_color;
        n->resolved.has_shadow   = TRUE;
    }

    //
    //CSS-style inheritance for typography. See original scene.c
    //comment: font family, font size, font color, and appear
    //animation inherit from parent. bg/radius/pad/gap etc. do not.
    //
    if (n->parent != NULL)
    {
        gui_style* p = &n->parent->resolved;
        if (n->resolved.font_family[0] == 0 && p->font_family[0] != 0)
        {
            memcpy(n->resolved.font_family, p->font_family, sizeof(n->resolved.font_family));
        }
        if (n->resolved.font_size <= 0.0f && p->font_size > 0.0f)
        {
            n->resolved.font_size          = p->font_size;
            //
            // Carry the explicit flag along with the inherited value
            // so the animator's transition gate (which requires both
            // endpoints to be rule-defined) keeps firing on a child
            // that inherits its size from a transitioning parent.
            //
            n->resolved.font_size_explicit = p->font_size_explicit;
        }
        if (!n->resolved.has_font_color && p->has_font_color)
        {
            n->resolved.font_color     = p->font_color;
            n->resolved.has_font_color = TRUE;
        }
        if (n->resolved.appear_ms <= 0.0f && p->appear_ms > 0.0f)
        {
            n->resolved.appear_ms     = p->appear_ms;
            n->resolved.appear_easing = p->appear_easing;
            n->resolved.appear_easing_params[0] = p->appear_easing_params[0];
            n->resolved.appear_easing_params[1] = p->appear_easing_params[1];
            n->resolved.appear_easing_params[2] = p->appear_easing_params[2];
            n->resolved.appear_easing_params[3] = p->appear_easing_params[3];
        }
    }

    //
    // Host overrides. Applied AFTER inherited-style overlay so they
    // win regardless of what rules / animator wrote.
    //
    if (n->has_background_color_override)
    {
        n->resolved.background_color     = n->background_color_override;
        n->resolved.has_background_color = TRUE;
    }
    if (n->has_font_color_override)
    {
        n->resolved.font_color     = n->font_color_override;
        n->resolved.has_font_color = TRUE;
    }

    gui_node* c = n->first_child;
    while (c != NULL)
    {
        _scene_style_internal__resolve_recursive(c);
        c = c->next_sibling;
    }
}

void scene_style__resolve_tree(gui_node* root)
{
    if (root == NULL) { return; }
    _scene_style_internal__resolve_recursive(root);
}

void scene__set_background_color_override(gui_node* n, gui_color c)
{
    if (n == NULL) { return; }
    n->background_color_override     = c;
    n->has_background_color_override = TRUE;
}

void scene__clear_background_color_override(gui_node* n)
{
    if (n == NULL) { return; }
    n->has_background_color_override = FALSE;
}

void scene__set_font_color_override(gui_node* n, gui_color c)
{
    if (n == NULL) { return; }
    n->font_color_override     = c;
    n->has_font_color_override = TRUE;
}

void scene__clear_font_color_override(gui_node* n)
{
    if (n == NULL) { return; }
    n->has_font_color_override = FALSE;
}
