#ifndef PARSER_STYLE_H
#define PARSER_STYLE_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
//parser_style.h - .style file loading.
//paired with parser_style.c. css-shaped subset (see DESIGN.md):
//
//  Window {
//      bg: #1e1e1e;
//  }
//
//  Button {
//      bg: #333333;
//      radius: 6;
//      size: 200x40;
//
//      :hover   { bg: #555555; }
//      :pressed { bg: #222222; }
//  }
//
//  Button.primary {
//      bg: #0066cc;
//      :hover   { bg: #0077e6; }
//      :pressed { bg: #004a99; }
//  }
//
//properties supported in this iteration:
//  bg     #rrggbb               background color (sets has_bg = TRUE)
//  radius <number>              corner radius in pixels
//  pad    <number>              padding on all four sides
//  gap    <number>              gap between children (column / row)
//  size   <number>x<number>     explicit width x height in pixels
//
//comments are c-style /* ... */. nested pseudo-state rules are
//supported one level deep (the parent's selector + ":pseudo" forms
//the new selector and is registered separately). every declaration
//must end with a ';', no exceptions.
//

/**
 * Read the file at the given path (UTF-8) and parse its CSS-shaped
 * style rules. Each parsed rule becomes a scene__register_style call.
 *
 * @function parser_style__load_styles
 * @param {char*} path - Path to the .style file to read.
 * @return {boole} TRUE on success; FALSE on I/O or parse failure.
 *
 * Errors are logged to stderr. Caller is responsible for clearing the
 * style table beforehand if a fresh-state reload is desired
 * (scene__clear_styles).
 */
GUI_API boole parser_style__load_styles(char* path);

#endif
