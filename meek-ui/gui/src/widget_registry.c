//
//widget_registry.c - the type -> vtable table + lookups + bootstrap.
//
//table is a fixed array sized GUI_NODE_TYPE_COUNT. each registration
//slot stores a pointer to the static widget_vtable defined in the
//corresponding widget_<name>.c file.
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"

//
//forward declarations of every built-in widget's register entry point.
//each widget .c defines exactly one of these (non-static) and uses it
//to call widget_registry__register with its vtable.
//
extern void widget_window__register(void);
extern void widget_column__register(void);
extern void widget_row__register(void);
extern void widget_button__register(void);
extern void widget_slider__register(void);
extern void widget_div__register(void);
extern void widget_text__register(void);
extern void widget_input__register(void);
extern void widget_checkbox__register(void);
extern void widget_radio__register(void);
extern void widget_select__register(void);
extern void widget_option__register(void);
extern void widget_image__register(void);
extern void widget_collection__register(void);
extern void widget_colorpicker__register(void);
extern void widget_popup__register(void);
extern void widget_textarea__register(void);
extern void widget_canvas__register(void);
extern void widget_keyboard__register(void);
extern void widget_process_window__register(void);

static const widget_vtable* _widget_registry_internal__table[GUI_NODE_TYPE_COUNT];

void widget_registry__register(gui_node_type type, const widget_vtable* vt)
{
    if ((int)type < 0 || (int)type >= (int)GUI_NODE_TYPE_COUNT)
    {
        return;
    }
    _widget_registry_internal__table[(int)type] = vt;
}

const widget_vtable* widget_registry__get(gui_node_type type)
{
    if ((int)type < 0 || (int)type >= (int)GUI_NODE_TYPE_COUNT)
    {
        return NULL;
    }
    return _widget_registry_internal__table[(int)type];
}

boole widget_registry__lookup_by_name(char* name, gui_node_type* out_type)
{
    if (name == NULL || out_type == NULL)
    {
        return FALSE;
    }
    for (int64 i = 0; i < (int64)GUI_NODE_TYPE_COUNT; i++)
    {
        const widget_vtable* vt = _widget_registry_internal__table[i];
        if (vt == NULL || vt->type_name == NULL)
        {
            continue;
        }
        if (strcmp(vt->type_name, name) == 0)
        {
            *out_type = (gui_node_type)i;
            return TRUE;
        }
    }
    return FALSE;
}

char* widget_registry__type_name(gui_node_type type)
{
    if ((int)type < 0 || (int)type >= (int)GUI_NODE_TYPE_COUNT)
    {
        return "";
    }
    const widget_vtable* vt = _widget_registry_internal__table[(int)type];
    if (vt == NULL || vt->type_name == NULL)
    {
        return "";
    }
    return vt->type_name;
}

void widget_registry__bootstrap_builtins(void)
{
    widget_window__register();
    widget_column__register();
    widget_row__register();
    widget_button__register();
    widget_slider__register();
    widget_div__register();
    widget_text__register();
    widget_input__register();
    widget_checkbox__register();
    widget_radio__register();
    widget_select__register();
    widget_option__register();
    widget_image__register();
    widget_collection__register();
    widget_colorpicker__register();
    widget_popup__register();
    widget_textarea__register();
    widget_canvas__register();
    widget_keyboard__register();
    widget_process_window__register();
}
