#ifndef PARSER_XML_H
#define PARSER_XML_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
//parser_xml.h - .ui file loading.
//paired with parser_xml.c. minimal xml subset (see DESIGN.md):
//tags + attributes + comments + the four mandated entities. no
//namespaces, no doctype, no processing instructions, no cdata.
//

/**
 * Reads the file at the given path (UTF-8) and parses it into a tree.
 *
 * @function parser_xml__load_ui
 * @param {char*} path - Path to the file to read.
 * @return {gui_node*} Parsed tree root node, or NULL on I/O or parse failure.
 *
 * Errors are logged to stderr on failure.
 * The caller becomes the owner of the returned tree and must call scene__node_free.
 */
GUI_API gui_node* parser_xml__load_ui(char* path);

/**
 * The directory portion (including trailing slash or backslash) of
 * the path most recently passed to parser_xml__load_ui. Widgets that
 * accept path-valued attributes (e.g. `<image src="foo.png"/>`) use
 * this as the base to resolve relative paths against, so a .ui file
 * can reference sibling assets without hardcoding a platform-specific
 * prefix. Empty string if no .ui has been loaded yet.
 *
 * @function parser_xml__base_dir
 * @return {const char*} Read-only pointer; valid until the next parser_xml__load_ui call.
 */
GUI_API const char* parser_xml__base_dir(void);

#endif
