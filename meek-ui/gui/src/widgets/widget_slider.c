//
//widget_slider.c - draggable horizontal slider.
//
//attributes:
//  min     lower bound of the value (default 0)
//  max     upper bound of the value (default 1)
//  value   initial value (default 0)
//
//draw: track (node bg) + thumb (style fg, with a sensible default).
//  thumb is square (width = track height), positioned along the
//  track by the value fraction.
//
//input:
//  mouse_down  snap value to cursor x (so a click on the empty track
//              jumps the thumb there immediately).
//  mouse_drag  same snap, called continuously while dragging.
//  mouse_up    no-op; slider sets consumes_click so scene skips the
//              on_click dispatch entirely (sliders are drag widgets).
//

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "third_party/log.h"

static boole _widget_slider_internal__parse_float(gui_node* n, char* attr_name, char* value, float* out);

//
// strtof-based parser that distinguishes "user wrote 0" from "atof
// silently returned 0 because the input wasn't a number". Logs a
// warning on garbage input and returns FALSE so the caller can keep
// the previous value (init_defaults seeded sensible numbers).
//
// atof() on its own is dangerous here: <slider value="oops" /> would
// return 0.0, the slider would silently be at min, and the bug would
// only surface when the user dragged it.
//
static boole _widget_slider_internal__parse_float(gui_node* n, char* attr_name, char* value, float* out)
{
    if (value == NULL || value[0] == 0)
    {
        log_warn("slider: %s='' on id='%s' is empty; ignoring", attr_name, (n != NULL && n->id[0] != 0) ? n->id : "(no id)");
        return FALSE;
    }
    char* end = NULL;
    float v   = strtof(value, &end);
    if (end == value)
    {
        log_warn("slider: %s='%s' on id='%s' is not a number; ignoring", attr_name, value, (n != NULL && n->id[0] != 0) ? n->id : "(no id)");
        return FALSE;
    }
    //
    // Allow trailing whitespace (XML attribute values can be
    // padded), but anything else after a number means the input
    // wasn't entirely numeric -- e.g. "0.5px", "10%", "1.0,2.0".
    // Reject so the user notices.
    //
    while (*end != 0)
    {
        if (!isspace((unsigned char)*end))
        {
            log_warn("slider: %s='%s' on id='%s' has trailing garbage after number; ignoring", attr_name, value, (n != NULL && n->id[0] != 0) ? n->id : "(no id)");
            return FALSE;
        }
        end++;
    }
    *out = v;
    return TRUE;
}

static void slider_init_defaults(gui_node* n)
{
    n->value_min = 0.0f;
    n->value_max = 1.0f;
    n->value     = 0.0f;
}

static boole slider_apply_attribute(gui_node* n, char* name, char* value)
{
    if (strcmp(name, "min") == 0)
    {
        float v;
        if (_widget_slider_internal__parse_float(n, "min", value, &v))
        {
            n->value_min = v;
        }
        return TRUE;
    }
    if (strcmp(name, "max") == 0)
    {
        float v;
        if (_widget_slider_internal__parse_float(n, "max", value, &v))
        {
            n->value_max = v;
        }
        return TRUE;
    }
    if (strcmp(name, "value") == 0)
    {
        float v;
        if (_widget_slider_internal__parse_float(n, "value", value, &v))
        {
            n->value = v;
        }
        return TRUE;
    }
    return FALSE;
}

static void slider_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : 240.0f * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 20.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

//
//compute the value from a cursor x. shared by mouse_down and mouse_drag.
//dispatches on_change only if the value actually moved.
//
static void slider_apply_drag(gui_node* n, int64 cursor_x)
{
    float thumb_w = n->bounds.h;
    float travel  = n->bounds.w - thumb_w;
    if (travel <= 0.0f)
    {
        return;
    }

    float local_x = (float)cursor_x - n->bounds.x - thumb_w * 0.5f;
    if (local_x < 0.0f)
    {
        local_x = 0.0f;
    }
    if (local_x > travel)
    {
        local_x = travel;
    }

    float fraction = local_x / travel;
    float range    = n->value_max - n->value_min;
    if (range <= 0.0f)
    {
        range = 1.0f;
    }
    float new_value = n->value_min + fraction * range;

    if (new_value == n->value)
    {
        return;
    }
    n->value = new_value;
    scene__dispatch_change(n, new_value);
}

static void slider_emit_draws(gui_node* n, float scale)
{
    //
    // Track: route through the shared default-bg helper so sliders
    // inherit gradient / shadow / bg-image / blur / border-gradient
    // automatically. The thumb is submitted separately below, on top.
    //
    scene__emit_default_bg(n, scale);

    //
    //thumb: square positioned along the track by the value fraction.
    //
    float range = n->value_max - n->value_min;
    if (range <= 0.0f)
    {
        range = 1.0f;
    }
    float fraction = (n->value - n->value_min) / range;
    if (fraction < 0.0f)
    {
        fraction = 0.0f;
    }
    if (fraction > 1.0f)
    {
        fraction = 1.0f;
    }

    float thumb_w = n->bounds.h;
    float travel  = n->bounds.w - thumb_w;
    if (travel < 0.0f)
    {
        travel = 0.0f;
    }

    gui_rect thumb;
    thumb.x = n->bounds.x + travel * fraction;
    thumb.y = n->bounds.y;
    thumb.w = thumb_w;
    thumb.h = n->bounds.h;

    //
    //Fallback thumb color when style doesn't set accent-color.
    //Neutral light gray reads against most backgrounds; once the
    //.style file specifies accent-color, this is never reached.
    //
    gui_color thumb_color;
    if (n->resolved.has_accent_color)
    {
        thumb_color = n->resolved.accent_color;
    }
    else
    {
        thumb_color = scene__rgb(0.8f, 0.8f, 0.8f);
    }

    renderer__submit_rect(thumb, thumb_color, n->resolved.radius * scale);
}

static void slider_on_mouse_down(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)y;
    (void)button;
    slider_apply_drag(n, x);
}

static boole slider_on_mouse_drag(gui_node* n, int64 x, int64 y)
{
    (void)y;
    slider_apply_drag(n, x);
    return TRUE; // suppress hover update while dragging.
}

static const widget_vtable g_slider_vtable = {
    .type_name        = "slider",
    .init_defaults    = slider_init_defaults,
    .apply_attribute  = slider_apply_attribute,
    .layout           = slider_layout,
    .emit_draws       = slider_emit_draws,
    .on_mouse_down    = slider_on_mouse_down,
    .on_mouse_up      = NULL,
    .on_mouse_drag    = slider_on_mouse_drag,
    .consumes_click   = TRUE, // drag widget; never fires on_click.
};

void widget_slider__register(void)
{
    widget_registry__register(GUI_NODE_SLIDER, &g_slider_vtable);
}
