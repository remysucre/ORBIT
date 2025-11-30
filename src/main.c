//
//  main.c
//  ORBIT - Clay-based markdown renderer for Playdate
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "cmark.h"

// Clay configuration - must be before including clay.h
#define CLAY_DISABLE_SIMD  // Playdate ARM Cortex-M7 doesn't have NEON
#define CLAY_IMPLEMENTATION
#include "clay.h"

static PlaydateAPI* pd = NULL;

// Clay memory arena - sized for small element/word counts
// With 256 elements and 1024 words, Clay needs ~600KB
#define CLAY_ARENA_SIZE (768 * 1024)  // 768KB
static char clay_memory[CLAY_ARENA_SIZE];
static Clay_Arena clay_arena;
static Clay_Context* clay_ctx = NULL;

// Font for rendering
static LCDFont* page_font = NULL;
static int font_height = 0;
static int line_height = 0;  // font_height + leading

// Screen dimensions
#define SCREEN_WIDTH 400
#define SCREEN_HEIGHT 240
#define PAGE_PADDING 10
#define CONTENT_WIDTH (SCREEN_WIDTH - 2 * PAGE_PADDING)  // Small margin for inter-word tracking

// Link storage for collision detection
#define MAX_LINKS 64
typedef struct {
    float x, y, w, h;
    char url[256];
    int active;
} LinkRect;

static LinkRect links[MAX_LINKS];
static int link_count = 0;

// Current link being built (for multi-line links)
static int building_link = 0;
static char current_link_url[256];

// ============================================================
// Text measurement callback for Clay
// ============================================================

static Clay_Dimensions measureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
    (void)userData;
    (void)config;

    if (page_font == NULL || text.length == 0) {
        return (Clay_Dimensions){0, 0};
    }

    // Playdate's getTextWidth needs null-terminated string
    // Since text.chars may not be null-terminated, we need to handle this
    char temp[512];
    int len = text.length < 511 ? text.length : 511;
    memcpy(temp, text.chars, len);
    temp[len] = '\0';

    int width = pd->graphics->getTextWidth(page_font, temp, strlen(temp), kUTF8Encoding, 1) + 1;

    return (Clay_Dimensions){
        .width = (float)width,
        .height = (float)line_height
    };
}

// ============================================================
// Clay error handler
// ============================================================

static void clayErrorHandler(Clay_ErrorData errorData) {
    pd->system->logToConsole("Clay error: %s", errorData.errorText.chars);
}

// ============================================================
// Initialize Clay
// ============================================================

static void initClay(void) {
    // Set Clay limits - need enough for wrapped lines and word cache
    Clay_SetMaxElementCount(512);
    Clay_SetMaxMeasureTextCacheWordCount(2048);

    // Check minimum memory requirement
    uint32_t minSize = Clay_MinMemorySize();
    pd->system->logToConsole("Clay min memory: %u, arena size: %d", minSize, CLAY_ARENA_SIZE);

    if (minSize > CLAY_ARENA_SIZE) {
        pd->system->logToConsole("ERROR: Clay arena too small!");
        return;
    }

    clay_arena = Clay_CreateArenaWithCapacityAndMemory(CLAY_ARENA_SIZE, clay_memory);

    Clay_Dimensions dims = { .width = (float)CONTENT_WIDTH, .height = (float)SCREEN_HEIGHT };
    Clay_ErrorHandler errHandler = { .errorHandlerFunction = clayErrorHandler, .userData = NULL };
    clay_ctx = Clay_Initialize(clay_arena, dims, errHandler);

    if (clay_ctx == NULL) {
        pd->system->logToConsole("ERROR: Clay_Initialize returned NULL!");
        return;
    }

    Clay_SetMeasureTextFunction(measureText, NULL);

    // Disable culling since we render to an offscreen bitmap larger than the screen
    Clay_SetCullingEnabled(false);

    pd->system->logToConsole("Clay initialized successfully");
}

// ============================================================
// Render markdown to bitmap and return link data
// ============================================================

// Helper to add a link rect
static void addLinkRect(float x, float y, float w, float h, const char* url) {
    if (link_count >= MAX_LINKS) return;

    links[link_count].x = x;
    links[link_count].y = y;
    links[link_count].w = w;
    links[link_count].h = h;
    strncpy(links[link_count].url, url, 255);
    links[link_count].url[255] = '\0';
    links[link_count].active = 1;
    link_count++;
}

// Helper to create Clay_String from char buffer
static Clay_String makeString(const char* str, int len) {
    return (Clay_String){
        .isStaticallyAllocated = false,
        .length = len,
        .chars = str
    };
}

// Helper to emit text element
static void emitText(const char* text, int len, Clay_TextElementConfig* config) {
    Clay_String str = makeString(text, len);
    Clay__OpenTextElement(str, Clay__StoreTextElementConfig(*config));
}

// Build Clay layout from markdown text
static void buildLayout(const char* markdown) {
    if (markdown == NULL) return;

    // Parse markdown
    size_t len = strlen(markdown);
    cmark_node* doc = cmark_parse_document(markdown, len, CMARK_OPT_DEFAULT);
    if (doc == NULL) return;

    // Reset link state
    link_count = 0;
    building_link = 0;
    current_link_url[0] = '\0';

    // Text config
    Clay_TextElementConfig textConfig = {
        .textColor = {0, 0, 0, 255},
        .fontId = 0,
        .fontSize = font_height,
        .wrapMode = CLAY_TEXT_WRAP_WORDS
    };

    // Begin Clay layout
    Clay_BeginLayout();

    // Root container
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(CONTENT_WIDTH),
                .height = CLAY_SIZING_FIT(0, 10000)
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childGap = 0
        }
    });

    // Large buffer to hold ALL text - each paragraph gets its own section
    // Clay stores pointers, so we can't reuse the same buffer
    static char all_text[16384];
    int all_text_pos = 0;
    int para_start = 0;  // Start of current paragraph in all_text
    int first_paragraph = 1;
    int para_count = 0;

    cmark_iter* iter = cmark_iter_new(doc);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node* node = cmark_iter_get_node(iter);
        cmark_node_type type = cmark_node_get_type(node);

        if (ev_type == CMARK_EVENT_ENTER) {
            switch (type) {
                case CMARK_NODE_PARAGRAPH:
                    // Add vertical space before non-first paragraphs
                    if (!first_paragraph) {
                        Clay__OpenElement();
                        Clay__ConfigureOpenElement((Clay_ElementDeclaration){
                            .layout = {
                                .sizing = { .height = CLAY_SIZING_FIXED((float)font_height) }
                            }
                        });
                        Clay__CloseElement();
                    }
                    first_paragraph = 0;
                    para_start = all_text_pos;  // Mark start of this paragraph
                    break;

                case CMARK_NODE_LINK:
                    building_link = 1;
                    break;

                case CMARK_NODE_TEXT: {
                    const char* literal = cmark_node_get_literal(node);
                    if (literal) {
                        size_t literal_len = strlen(literal);
                        if (all_text_pos + literal_len < sizeof(all_text) - 1) {
                            memcpy(all_text + all_text_pos, literal, literal_len);
                            all_text_pos += literal_len;
                        }
                    }
                    break;
                }

                case CMARK_NODE_SOFTBREAK:
                    if (all_text_pos < (int)sizeof(all_text) - 1) {
                        all_text[all_text_pos++] = ' ';
                    }
                    break;

                case CMARK_NODE_LINEBREAK:
                    if (all_text_pos < (int)sizeof(all_text) - 1) {
                        all_text[all_text_pos++] = '\n';
                    }
                    break;

                case CMARK_NODE_CODE:
                case CMARK_NODE_CODE_BLOCK: {
                    const char* literal = cmark_node_get_literal(node);
                    if (literal) {
                        size_t literal_len = strlen(literal);
                        if (all_text_pos + literal_len < sizeof(all_text) - 1) {
                            memcpy(all_text + all_text_pos, literal, literal_len);
                            all_text_pos += literal_len;
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        } else if (ev_type == CMARK_EVENT_EXIT) {
            switch (type) {
                case CMARK_NODE_PARAGRAPH: {
                    // Emit entire paragraph as one text element
                    int para_len = all_text_pos - para_start;
                    if (para_len > 0) {
                        all_text[all_text_pos] = '\0';  // Null terminate
                        emitText(all_text + para_start, para_len, &textConfig);
                        all_text_pos++;  // Move past the null terminator
                        para_count++;
                    }
                    break;
                }

                case CMARK_NODE_LINK:
                    building_link = 0;
                    break;

                default:
                    break;
            }
        }
    }

    cmark_iter_free(iter);

    // Close root element
    Clay__CloseElement();

    cmark_node_free(doc);
}

// Render the layout to a bitmap
static LCDBitmap* renderLayout(int* out_height) {
    Clay_RenderCommandArray commands = Clay_EndLayout();

    // Calculate total height from commands
    float max_y = 0;
    for (int i = 0; i < commands.length; i++) {
        Clay_RenderCommand* cmd = Clay_RenderCommandArray_Get(&commands, i);
        float bottom = cmd->boundingBox.y + cmd->boundingBox.height;
        if (bottom > max_y) max_y = bottom;
    }

    // Add padding: PAGE_PADDING at top (rendering offset) + PAGE_PADDING at bottom + extra for font baseline
    int page_height = (int)(max_y + PAGE_PADDING * 2 + font_height);
    if (page_height < SCREEN_HEIGHT) page_height = SCREEN_HEIGHT;

    // Create bitmap
    LCDBitmap* bitmap = pd->graphics->newBitmap(SCREEN_WIDTH, page_height, kColorWhite);
    if (!bitmap) return NULL;

    pd->graphics->pushContext(bitmap);
    pd->graphics->setFont(page_font);
    // pd->graphics->setTextTracking(0);  // Ensure measurement and drawing match

    // Render commands
    for (int i = 0; i < commands.length; i++) {
        Clay_RenderCommand* cmd = Clay_RenderCommandArray_Get(&commands, i);

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                Clay_TextRenderData* textData = &cmd->renderData.text;

                // Draw text
                int x = (int)(cmd->boundingBox.x + PAGE_PADDING);
                int y = (int)(cmd->boundingBox.y + PAGE_PADDING);

                // Need null-terminated string
                char temp[512];
                int len = textData->stringContents.length < 511 ? textData->stringContents.length : 511;
                memcpy(temp, textData->stringContents.chars, len);
                temp[len] = '\0';

                pd->graphics->drawText(temp, strlen(temp), kUTF8Encoding, x, y);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                // Could be used for link underlines later
                break;
            }

            default:
                break;
        }
    }

    pd->graphics->popContext();

    *out_height = page_height;
    return bitmap;
}

// ============================================================
// Lua-callable render function
// ============================================================

static int renderMarkdown(lua_State* L) {
    (void)L;

    const char* markdown = pd->lua->getArgString(1);
    if (markdown == NULL) {
        pd->lua->pushNil();
        return 1;
    }

    // Build layout
    buildLayout(markdown);

    // Render to bitmap
    int page_height = 0;
    LCDBitmap* bitmap = renderLayout(&page_height);

    if (!bitmap) {
        pd->lua->pushNil();
        return 1;
    }

    // Push bitmap as userdata (Lua can use it as sprite image)
    pd->lua->pushBitmap(bitmap);
    pd->lua->pushInt(page_height);
    pd->lua->pushInt(link_count);

    return 3;  // bitmap, height, link_count
}

// Get link data at index
static int getLinkData(lua_State* L) {
    (void)L;

    int index = pd->lua->getArgInt(1);
    if (index < 0 || index >= link_count) {
        pd->lua->pushNil();
        return 1;
    }

    LinkRect* link = &links[index];
    pd->lua->pushFloat(link->x);
    pd->lua->pushFloat(link->y);
    pd->lua->pushFloat(link->w);
    pd->lua->pushFloat(link->h);
    pd->lua->pushString(link->url);

    return 5;  // x, y, w, h, url
}

// ============================================================
// Event handler
// ============================================================

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;

    if (event == kEventInitLua) {
        pd = playdate;

        const char* err;

        // Load font
        page_font = pd->graphics->loadFont("fonts/SYSTEM6", &err);
        if (!page_font) {
            pd->system->logToConsole("Failed to load font: %s", err);
            return 0;
        }
        font_height = pd->graphics->getFontHeight(page_font);
        line_height = font_height + 2;  // Add leading (SYSTEM6 uses 2px leading)
        pd->system->logToConsole("Font loaded, height: %d, line_height: %d", font_height, line_height);

        // Initialize Clay
        initClay();

        // Register render function
        if (!pd->lua->addFunction(renderMarkdown, "clay.render", &err)) {
            pd->system->logToConsole("Failed to register clay.render: %s", err);
        }

        // Register link data function
        if (!pd->lua->addFunction(getLinkData, "clay.getLinkData", &err)) {
            pd->system->logToConsole("Failed to register clay.getLinkData: %s", err);
        }

        pd->system->logToConsole("Clay renderer registered successfully");
    }

    return 0;
}
