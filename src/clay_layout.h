// clay_layout.h
// Markdown layout using Clay text measurement for Playdate

#ifndef CLAY_LAYOUT_H
#define CLAY_LAYOUT_H

#include "pd_api.h"

// Initialize Clay layout system with Playdate API
void clay_layout_init(PlaydateAPI* pd);

// Layout markdown text, returns JSON with positioned elements
// JSON format: [{"type":"text"|"link", "text":"...", "url":"...", "x":n, "y":n, "w":n, "h":n}, ...]
const char* clay_layout_markdown(const char* markdown, int content_width, int font_id);

// Set the current font for text measurement
void clay_layout_set_font(LCDFont* font);

#endif // CLAY_LAYOUT_H
