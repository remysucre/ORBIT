// clay_layout.c
// Markdown layout using Clay text measurement for Playdate

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_layout.h"
#include "cmark.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static PlaydateAPI* pd = NULL;
static LCDFont* current_font = NULL;
static Clay_Arena clay_arena;
static bool clay_initialized = false;

// Text measurement callback for Clay
static Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
    (void)config;
    (void)userData;

    Clay_Dimensions dims = {0, 0};
    if (!current_font || !text.chars || text.length == 0) {
        return dims;
    }

    // Create null-terminated string for Playdate API
    char* str = malloc(text.length + 1);
    if (!str) return dims;
    memcpy(str, text.chars, text.length);
    str[text.length] = '\0';

    dims.width = (float)pd->graphics->getTextWidth(current_font, str, text.length, kUTF8Encoding, 0);
    dims.height = (float)pd->graphics->getFontHeight(current_font);

    free(str);
    return dims;
}

void clay_layout_init(PlaydateAPI* playdate) {
    pd = playdate;

    if (!clay_initialized) {
        uint32_t mem_size = Clay_MinMemorySize();
        void* memory = malloc(mem_size);
        if (!memory) {
            pd->system->logToConsole("Failed to allocate Clay memory");
            return;
        }

        clay_arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, memory);
        Clay_Initialize(clay_arena, (Clay_Dimensions){400, 240}, (Clay_ErrorHandler){0});
        Clay_SetMeasureTextFunction(measure_text, NULL);
        clay_initialized = true;

        pd->system->logToConsole("Clay initialized with %u bytes", mem_size);
    }
}

void clay_layout_set_font(LCDFont* font) {
    current_font = font;
}

// JSON output helpers
#define MAX_JSON_SIZE 65536
static char json_buffer[MAX_JSON_SIZE];
static int json_pos = 0;

static void json_reset(void) {
    json_pos = 0;
    json_buffer[0] = '\0';
}

static void json_append(const char* str) {
    size_t len = strlen(str);
    if (json_pos + len < MAX_JSON_SIZE) {
        memcpy(json_buffer + json_pos, str, len);
        json_pos += len;
        json_buffer[json_pos] = '\0';
    }
}

static void json_append_int(int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    json_append(buf);
}

static void json_append_escaped(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (c == '"') json_append("\\\"");
        else if (c == '\\') json_append("\\\\");
        else if (c == '\n') json_append("\\n");
        else if (c == '\r') json_append("\\r");
        else if (c == '\t') json_append("\\t");
        else {
            char buf[2] = {c, '\0'};
            json_append(buf);
        }
    }
}

// Layout state
typedef struct {
    int x;
    int y;
    int content_width;
    int line_height;
    int space_width;
    int tracking;
} LayoutState;

// Measure text width using Clay/Playdate
static int measure_width(const char* text, size_t len) {
    if (!current_font || !text || len == 0) return 0;
    return pd->graphics->getTextWidth(current_font, text, len, kUTF8Encoding, 0);
}

// Emit a positioned element to JSON
static void emit_element(const char* type, const char* text, size_t text_len,
                         const char* url, int x, int y, int w, int h, int first) {
    if (!first) json_append(",");
    json_append("{\"type\":\"");
    json_append(type);
    json_append("\",\"text\":\"");
    json_append_escaped(text, text_len);
    json_append("\"");
    if (url) {
        json_append(",\"url\":\"");
        json_append_escaped(url, strlen(url));
        json_append("\"");
    }
    json_append(",\"x\":");
    json_append_int(x);
    json_append(",\"y\":");
    json_append_int(y);
    json_append(",\"w\":");
    json_append_int(w);
    json_append(",\"h\":");
    json_append_int(h);
    json_append("}");
}

// Layout a text fragment with word wrapping
// Returns number of elements emitted
static int layout_text(LayoutState* state, const char* text, size_t len,
                       const char* url, int* first) {
    if (!text || len == 0) return 0;

    const char* type = url ? "link" : "text";
    int count = 0;
    size_t pos = 0;

    while (pos < len) {
        // Skip leading spaces at line start
        while (pos < len && text[pos] == ' ' && state->x == 0) {
            pos++;
        }
        if (pos >= len) break;

        // Handle space
        if (text[pos] == ' ') {
            if (state->x + state->space_width <= state->content_width) {
                state->x += state->space_width + state->tracking;
            }
            pos++;
            continue;
        }

        // Find word end
        size_t word_start = pos;
        while (pos < len && text[pos] != ' ') {
            pos++;
        }
        size_t word_len = pos - word_start;

        // Measure word
        int word_width = measure_width(text + word_start, word_len);

        // Wrap if needed
        if (state->x > 0 && state->x + word_width > state->content_width) {
            state->y += state->line_height;
            state->x = 0;
        }

        // Emit element
        emit_element(type, text + word_start, word_len, url,
                     state->x, state->y, word_width, state->line_height, *first);
        *first = 0;
        count++;

        state->x += word_width + state->tracking;
    }

    return count;
}

const char* clay_layout_markdown(const char* markdown, int content_width, int font_id) {
    (void)font_id; // Currently using current_font set separately

    if (!markdown || !current_font) {
        return "[]";
    }

    // Parse markdown
    size_t len = strlen(markdown);
    cmark_node* doc = cmark_parse_document(markdown, len, CMARK_OPT_DEFAULT);
    if (!doc) {
        return "[]";
    }

    // Initialize layout state
    LayoutState state = {
        .x = 0,
        .y = 0,
        .content_width = content_width,
        .line_height = pd->graphics->getFontHeight(current_font),
        .space_width = measure_width(" ", 1),
        .tracking = 0 // Could get from font if API available
    };

    // Start JSON output
    json_reset();
    json_append("[");
    int first = 1;

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
                    if (!first_paragraph) {
                        // Paragraph break
                        state.y += state.line_height * 2;
                        state.x = 0;
                    }
                    first_paragraph = 0;
                    break;

                case CMARK_NODE_LINK:
                    in_link = 1;
                    link_url = cmark_node_get_url(node);
                    break;

                case CMARK_NODE_TEXT: {
                    const char* text = cmark_node_get_literal(node);
                    if (text) {
                        layout_text(&state, text, strlen(text),
                                   in_link ? link_url : NULL, &first);
                    }
                    break;
                }

                case CMARK_NODE_SOFTBREAK:
                    // Treat as space
                    if (state.x > 0 && state.x + state.space_width <= state.content_width) {
                        state.x += state.space_width + state.tracking;
                    }
                    break;

                case CMARK_NODE_LINEBREAK:
                    state.y += state.line_height;
                    state.x = 0;
                    break;

                case CMARK_NODE_CODE: {
                    const char* code = cmark_node_get_literal(node);
                    if (code) {
                        layout_text(&state, code, strlen(code), NULL, &first);
                    }
                    break;
                }

                case CMARK_NODE_CODE_BLOCK: {
                    if (!first_paragraph) {
                        state.y += state.line_height * 2;
                        state.x = 0;
                    }
                    first_paragraph = 0;
                    const char* code = cmark_node_get_literal(node);
                    if (code) {
                        layout_text(&state, code, strlen(code), NULL, &first);
                    }
                    break;
                }

                default:
                    break;
            }
        } else if (ev_type == CMARK_EVENT_EXIT) {
            switch (type) {
                case CMARK_NODE_LINK:
                    in_link = 0;
                    link_url = NULL;
                    break;

                default:
                    break;
            }
        }
    }

    json_append("]");

    // Cleanup
    cmark_iter_free(iter);
    cmark_node_free(doc);

    return json_buffer;
}
