//
//  main.c
//  ORBIT - cmark markdown parser + Clay layout for Playdate
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "cmark.h"
#include "clay_layout.h"

static PlaydateAPI* pd = NULL;
static LCDFont* layout_font = NULL;

// JSON output buffer
#define MAX_JSON_SIZE 65536
static char json_buffer[MAX_JSON_SIZE];
static int json_pos = 0;

// Write callback for JSON encoder
static void json_write(void* userdata, const char* str, int len) {
    (void)userdata;
    if (json_pos + len < MAX_JSON_SIZE) {
        memcpy(json_buffer + json_pos, str, len);
        json_pos += len;
    }
}

// Text accumulator for building fragments
#define MAX_TEXT_SIZE 8192
static char text_buffer[MAX_TEXT_SIZE];
static int text_pos = 0;

static void text_reset(void) {
    text_pos = 0;
    text_buffer[0] = '\0';
}

static void text_append(const char* str) {
    if (str == NULL) return;
    size_t len = strlen(str);
    if (text_pos + len < MAX_TEXT_SIZE) {
        memcpy(text_buffer + text_pos, str, len);
        text_pos += len;
        text_buffer[text_pos] = '\0';
    }
}

static void text_append_char(char c) {
    if (text_pos + 1 < MAX_TEXT_SIZE) {
        text_buffer[text_pos++] = c;
        text_buffer[text_pos] = '\0';
    }
}

// Emit a text fragment to JSON
static void emit_text_fragment(json_encoder* encoder) {
    if (text_pos == 0) return;

    encoder->addArrayMember(encoder);
    encoder->startTable(encoder);
    encoder->addTableMember(encoder, "type", 4);
    encoder->writeString(encoder, "text", 4);
    encoder->addTableMember(encoder, "text", 4);
    encoder->writeString(encoder, text_buffer, text_pos);
    encoder->endTable(encoder);

    text_reset();
}

// Emit a link fragment to JSON
static void emit_link_fragment(json_encoder* encoder, const char* url) {
    if (text_pos == 0) return;

    encoder->addArrayMember(encoder);
    encoder->startTable(encoder);
    encoder->addTableMember(encoder, "type", 4);
    encoder->writeString(encoder, "link", 4);
    encoder->addTableMember(encoder, "text", 4);
    encoder->writeString(encoder, text_buffer, text_pos);
    encoder->addTableMember(encoder, "url", 3);
    encoder->writeString(encoder, url, strlen(url));
    encoder->endTable(encoder);

    text_reset();
}

// Emit a break element to JSON
static void emit_break(json_encoder* encoder) {
    encoder->addArrayMember(encoder);
    encoder->startTable(encoder);
    encoder->addTableMember(encoder, "type", 4);
    encoder->writeString(encoder, "break", 5);
    encoder->endTable(encoder);
}

// Parse markdown and return JSON string
static int parseMarkdown(lua_State* L) {
    (void)L;

    const char* markdown = pd->lua->getArgString(1);
    if (markdown == NULL) {
        pd->lua->pushString("[]");
        return 1;
    }

    // Parse the markdown document
    size_t len = strlen(markdown);
    cmark_node* doc = cmark_parse_document(markdown, len, CMARK_OPT_DEFAULT);
    if (doc == NULL) {
        pd->lua->pushString("[]");
        return 1;
    }

    // Initialize JSON encoder
    json_encoder encoder;
    json_pos = 0;
    pd->json->initEncoder(&encoder, json_write, NULL, 0);

    // Reset text accumulator
    text_reset();

    // Start JSON array
    encoder.startArray(&encoder);

    // Track state
    int in_link = 0;
    const char* link_url = NULL;
    int first_paragraph = 1;

    // Iterate through AST
    cmark_iter* iter = cmark_iter_new(doc);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node* node = cmark_iter_get_node(iter);
        cmark_node_type type = cmark_node_get_type(node);

        if (ev_type == CMARK_EVENT_ENTER) {
            switch (type) {
                case CMARK_NODE_PARAGRAPH:
                    // Add paragraph break (except for first paragraph)
                    if (!first_paragraph) {
                        emit_text_fragment(&encoder);
                        emit_break(&encoder);
                    }
                    first_paragraph = 0;
                    break;

                case CMARK_NODE_LINK:
                    // Flush any pending text before link
                    emit_text_fragment(&encoder);
                    in_link = 1;
                    link_url = cmark_node_get_url(node);
                    break;

                case CMARK_NODE_TEXT:
                    text_append(cmark_node_get_literal(node));
                    break;

                case CMARK_NODE_SOFTBREAK:
                    text_append_char(' ');
                    break;

                case CMARK_NODE_LINEBREAK:
                    text_append_char('\n');
                    break;

                case CMARK_NODE_CODE:
                    // Inline code - just add the text
                    text_append(cmark_node_get_literal(node));
                    break;

                case CMARK_NODE_CODE_BLOCK:
                    // Code block - add literal content
                    if (!first_paragraph) {
                        emit_text_fragment(&encoder);
                        emit_break(&encoder);
                    }
                    first_paragraph = 0;
                    text_append(cmark_node_get_literal(node));
                    break;

                default:
                    // Other nodes: just process their children
                    break;
            }
        } else if (ev_type == CMARK_EVENT_EXIT) {
            switch (type) {
                case CMARK_NODE_LINK:
                    // Emit the link fragment
                    emit_link_fragment(&encoder, link_url ? link_url : "");
                    in_link = 0;
                    link_url = NULL;
                    break;

                case CMARK_NODE_DOCUMENT:
                    // End of document - flush any remaining text
                    emit_text_fragment(&encoder);
                    break;

                default:
                    break;
            }
        }
    }

    // End JSON array
    encoder.endArray(&encoder);

    // Null-terminate
    json_buffer[json_pos] = '\0';

    // Cleanup
    cmark_iter_free(iter);
    cmark_node_free(doc);

    // Return JSON string to Lua
    pd->lua->pushString(json_buffer);
    return 1;
}

// Layout markdown with Clay - returns positioned elements
// cmark.layout(markdown, contentWidth, fontPath) -> JSON
static int layoutMarkdown(lua_State* L) {
    (void)L;

    const char* markdown = pd->lua->getArgString(1);
    if (markdown == NULL) {
        pd->lua->pushString("[]");
        return 1;
    }

    // Get content width (default 380)
    int content_width = 380;
    if (pd->lua->getArgCount() >= 2) {
        content_width = (int)pd->lua->getArgFloat(2);
    }

    // Get font path if provided (arg 3)
    if (pd->lua->getArgCount() >= 3) {
        const char* font_path = pd->lua->getArgString(3);
        if (font_path) {
            const char* err = NULL;
            LCDFont* font = pd->graphics->loadFont(font_path, &err);
            if (font) {
                layout_font = font;
                clay_layout_set_font(font);
            } else if (err) {
                pd->system->logToConsole("Font load error: %s", err);
            }
        }
    }

    // Ensure font is set
    if (!layout_font) {
        pd->system->logToConsole("No font set for layout");
        pd->lua->pushString("[]");
        return 1;
    }

    // Do the layout
    const char* result = clay_layout_markdown(markdown, content_width, 0);
    pd->lua->pushString(result);
    return 1;
}

// Set the font for layout (without doing layout)
// cmark.setFont(fontPath) -> boolean
static int setLayoutFont(lua_State* L) {
    (void)L;

    const char* font_path = pd->lua->getArgString(1);
    if (!font_path) {
        pd->lua->pushBool(0);
        return 1;
    }

    const char* err = NULL;
    LCDFont* font = pd->graphics->loadFont(font_path, &err);
    if (font) {
        layout_font = font;
        clay_layout_set_font(font);
        pd->lua->pushBool(1);
        return 1;
    }

    if (err) {
        pd->system->logToConsole("Font load error: %s", err);
    }
    pd->lua->pushBool(0);
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;

    if (event == kEventInitLua) {
        pd = playdate;

        // Initialize Clay layout system
        clay_layout_init(pd);

        const char* err;

        // Register cmark.parse function (legacy)
        if (!pd->lua->addFunction(parseMarkdown, "cmark.parse", &err)) {
            pd->system->logToConsole("Failed to register cmark.parse: %s", err);
        } else {
            pd->system->logToConsole("cmark.parse registered successfully");
        }

        // Register cmark.layout function (new Clay-based layout)
        if (!pd->lua->addFunction(layoutMarkdown, "cmark.layout", &err)) {
            pd->system->logToConsole("Failed to register cmark.layout: %s", err);
        } else {
            pd->system->logToConsole("cmark.layout registered successfully");
        }

        // Register cmark.setFont function
        if (!pd->lua->addFunction(setLayoutFont, "cmark.setFont", &err)) {
            pd->system->logToConsole("Failed to register cmark.setFont: %s", err);
        } else {
            pd->system->logToConsole("cmark.setFont registered successfully");
        }
    }

    return 0;
}
