#ifndef WIDGET_REGISTRY_H
#define WIDGET_REGISTRY_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"
#include "widget.h"

//
//widget_registry.h - the type -> vtable map.
//paired with widget_registry.c. one entry per gui_node_type.
//

/**
 * Register a widget vtable for a node type. Called once per type from
 * the widget's widget_<name>__register function.
 *
 * @function widget_registry__register
 * @param {gui_node_type} type - The node-type slot to populate.
 * @param {const widget_vtable*} vt - Pointer to a static vtable. Stored by reference; must outlive the registration.
 * @return {void}
 */
GUI_API void widget_registry__register(gui_node_type type, const widget_vtable* vt);

/**
 * Look up the registered vtable for a type. Returns NULL if no widget
 * is registered for that type (treat as "no special behavior").
 *
 * @function widget_registry__get
 * @param {gui_node_type} type - The node-type slot to query.
 * @return {const widget_vtable*} The vtable, or NULL.
 */
GUI_API const widget_vtable* widget_registry__get(gui_node_type type);

/**
 * Map an XML tag name (e.g. "Button") to a node type. parser_xml uses
 * this instead of a hardcoded tag table.
 *
 * @function widget_registry__lookup_by_name
 * @param {char*} name - Tag name to look up.
 * @param {gui_node_type*} out_type - On success, receives the matching type.
 * @return {boole} TRUE if a widget is registered under that name.
 */
GUI_API boole widget_registry__lookup_by_name(char* name, gui_node_type* out_type);

/**
 * Get a node type's registered name (for selector matching in scene's
 * style resolver). Returns "" for unregistered types.
 *
 * @function widget_registry__type_name
 * @param {gui_node_type} type - The node-type slot to query.
 * @return {char*} The registered name, or "".
 */
GUI_API char* widget_registry__type_name(gui_node_type type);

/**
 * Register every built-in widget. Called by platform_win32__init at
 * startup so that parser_xml + scene see all built-in widgets before
 * any tree is loaded.
 *
 * @function widget_registry__bootstrap_builtins
 * @return {void}
 */
GUI_API void widget_registry__bootstrap_builtins(void);

#endif
