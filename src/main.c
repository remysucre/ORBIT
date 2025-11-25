//
//  main.c
//  ORBIT - cmark markdown parser for Playdate
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "cmark.h"

static PlaydateAPI* pd = NULL;

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
                        text_append("\n\n");
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
                        text_append("\n\n");
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

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;

    if (event == kEventInitLua) {
        pd = playdate;

        const char* err;

        // Register cmark.parse function
        if (!pd->lua->addFunction(parseMarkdown, "cmark.parse", &err)) {
            pd->system->logToConsole("Failed to register cmark.parse: %s", err);
        } else {
            pd->system->logToConsole("cmark.parse registered successfully");
        }
    }

    return 0;
}
