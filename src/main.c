//
//  main.c
//  ORBIT - cmark markdown parser and page renderer for Playdate
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "pd_api.h"
#include "cmark.h"

static PlaydateAPI* pd = NULL;

// ============================================================================
// Page Rendering Data Structures
// ============================================================================

#define MAX_LINKS 64
#define MAX_SEGMENTS_PER_LINK 8
#define MAX_TEXT_SEGMENTS 1024
#define SCREEN_HEIGHT 240

typedef struct {
    int x, y, w;
} LinkSegment;

typedef struct {
    LCDSprite* sprite;
    LCDBitmap* image;
    char url[512];
    LinkSegment segments[MAX_SEGMENTS_PER_LINK];
    int segmentCount;
    int offsetX, offsetY;
    int width, height;
    int textHeight;
} LinkData;

typedef struct {
    int x, y;
    char text[512];
    int width;
} TextSegment;

static struct {
    LCDFont* font;
    int fontHeight;
    int tracking;
    int pageWidth;
    int pagePadding;
    int contentWidth;
    LCDBitmap* pageImage;
    int pageHeight;
    LinkData links[MAX_LINKS];
    int linkCount;
    int initialized;
} renderState = {0};

// ============================================================================
// JSON output buffer (kept for backwards compatibility)
// ============================================================================

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

// ============================================================================
// Page Rendering Functions
// ============================================================================

// Initialize the renderer with font and page dimensions
// Args: fontPath, pageWidth, pagePadding, tracking
static int initRenderer(lua_State* L) {
    (void)L;

    const char* fontPath = pd->lua->getArgString(1);
    int pageWidth = pd->lua->getArgInt(2);
    int pagePadding = pd->lua->getArgInt(3);
    int tracking = pd->lua->getArgInt(4);

    if (!fontPath) {
        pd->system->logToConsole("initRenderer: missing font path");
        pd->lua->pushBool(0);
        return 1;
    }

    const char* err = NULL;
    renderState.font = pd->graphics->loadFont(fontPath, &err);
    if (err || !renderState.font) {
        pd->system->logToConsole("Failed to load font '%s': %s", fontPath, err ? err : "unknown error");
        pd->lua->pushBool(0);
        return 1;
    }

    renderState.fontHeight = pd->graphics->getFontHeight(renderState.font);
    renderState.tracking = tracking;
    renderState.pageWidth = pageWidth;
    renderState.pagePadding = pagePadding;
    renderState.contentWidth = pageWidth - 2 * pagePadding;
    renderState.initialized = 1;

    pd->system->logToConsole("Renderer initialized: fontHeight=%d, tracking=%d, contentWidth=%d",
                             renderState.fontHeight, renderState.tracking, renderState.contentWidth);

    pd->lua->pushBool(1);
    return 1;
}

// Internal function to clean up all link sprites
static void cleanupLinksInternal(void) {
    for (int i = 0; i < renderState.linkCount; i++) {
        LinkData* link = &renderState.links[i];
        if (link->sprite) {
            pd->sprite->removeSprite(link->sprite);
            pd->sprite->freeSprite(link->sprite);
            link->sprite = NULL;
        }
        if (link->image) {
            pd->graphics->freeBitmap(link->image);
            link->image = NULL;
        }
    }
    renderState.linkCount = 0;
}

// Lua-callable cleanup function
static int cleanupLinks(lua_State* L) {
    (void)L;
    cleanupLinksInternal();
    return 0;
}

// Word-wrap layout algorithm
// Returns: number of segments created, updates endX and endY
static int layoutWords(const char* text, int startX, int startY,
                       TextSegment* segments, int maxSegments,
                       int* endX, int* endY) {
    if (!text || !segments) return 0;

    int segmentCount = 0;
    int x = startX;
    int y = startY;
    int h = renderState.fontHeight;
    int contentWidth = renderState.contentWidth;
    int tracking = renderState.tracking;

    // Get space width (without tracking - we add tracking manually)
    int spaceWidth = pd->graphics->getTextWidth(renderState.font, " ", 1, kASCIIEncoding, 0);

    int pos = 0;
    int len = (int)strlen(text);

    char segment[512] = "";
    int segX = x, segY = y;
    int segLen = 0;

    while (pos < len) {
        if (text[pos] == ' ') {
            // Handle space
            if (x + spaceWidth <= contentWidth) {
                x += spaceWidth + tracking;
                if (segLen < 511) {
                    segment[segLen++] = ' ';
                    segment[segLen] = '\0';
                }
            }
            pos++;
        } else {
            // Find word end
            int wordEnd = pos;
            while (wordEnd < len && text[wordEnd] != ' ') {
                wordEnd++;
            }

            // Extract word
            int wordLen = wordEnd - pos;
            char word[256];
            if (wordLen > 255) wordLen = 255;
            strncpy(word, text + pos, wordLen);
            word[wordLen] = '\0';

            // Get word width without tracking, then add tracking manually
            int wordWidth = pd->graphics->getTextWidth(renderState.font, word, wordLen, kASCIIEncoding, 0);

            // Wrap if needed
            if (x > 0 && x + wordWidth > contentWidth) {
                // Save current segment
                if (segLen > 0 && segmentCount < maxSegments) {
                    strncpy(segments[segmentCount].text, segment, 511);
                    segments[segmentCount].text[511] = '\0';
                    segments[segmentCount].x = segX;
                    segments[segmentCount].y = segY;
                    segments[segmentCount].width = pd->graphics->getTextWidth(
                        renderState.font, segment, segLen, kASCIIEncoding, 0);
                    segmentCount++;
                }

                // Start new line
                y += h;
                x = 0;
                segment[0] = '\0';
                segLen = 0;
                segX = x;
                segY = y;
            }

            // Add word to segment (add tracking after word like Lua does)
            x += wordWidth + tracking;
            if (segLen + wordLen < 511) {
                strcat(segment, word);
                segLen += wordLen;
            }
            pos = wordEnd;
        }
    }

    // Save final segment
    if (segLen > 0 && segmentCount < maxSegments) {
        strncpy(segments[segmentCount].text, segment, 511);
        segments[segmentCount].text[511] = '\0';
        segments[segmentCount].x = segX;
        segments[segmentCount].y = segY;
        segments[segmentCount].width = pd->graphics->getTextWidth(
            renderState.font, segment, segLen, kASCIIEncoding, 0);
        segmentCount++;
    }

    *endX = x;
    *endY = y;
    return segmentCount;
}

// Draw link image (for initial render and hover state updates)
static void drawLinkImage(LinkData* link, int isHovered) {
    if (!link->image) return;

    pd->graphics->pushContext(link->image);
    pd->graphics->clear(kColorClear);

    int lineWidth = isHovered ? 2 : 1;

    for (int i = 0; i < link->segmentCount; i++) {
        LinkSegment* seg = &link->segments[i];
        int localX = seg->x - link->offsetX;
        int localY = seg->y - link->offsetY;

        // Draw white background (for alpha collision detection)
        pd->graphics->fillRect(localX, localY, seg->w, link->textHeight, kColorWhite);

        // Draw underline
        int underlineY = localY + link->textHeight - 2;
        pd->graphics->drawLine(localX, underlineY, localX + seg->w, underlineY, lineWidth, kColorBlack);
    }

    pd->graphics->popContext();
}

// Create a link sprite from segment data
static void createLinkSprite(LinkData* link) {
    if (link->width <= 0 || link->height <= 0) return;

    // Create sprite
    link->sprite = pd->sprite->newSprite();
    if (!link->sprite) return;

    // Create image
    link->image = pd->graphics->newBitmap(link->width, link->height, kColorClear);
    if (!link->image) {
        pd->sprite->freeSprite(link->sprite);
        link->sprite = NULL;
        return;
    }

    // Draw initial (non-hovered) state
    drawLinkImage(link, 0);

    // Configure sprite
    pd->sprite->setImage(link->sprite, link->image, kBitmapUnflipped);
    pd->sprite->setCenter(link->sprite, 0.0f, 0.0f);  // Top-left anchor
    pd->sprite->moveTo(link->sprite,
                       (float)(renderState.pagePadding + link->offsetX),
                       (float)(renderState.pagePadding + link->offsetY));
    pd->sprite->setZIndex(link->sprite, -1);  // Below page content

    // Set collision rect
    PDRect collideRect = PDRectMake(0, 0, (float)link->width, (float)link->height);
    pd->sprite->setCollideRect(link->sprite, collideRect);

    // Store pointer to link data in userdata (for getLinkData)
    pd->sprite->setUserdata(link->sprite, link);

    pd->sprite->addSprite(link->sprite);
}

// Main render function - parse markdown, create page image and link sprites
static int renderPage(lua_State* L) {
    (void)L;

    if (!renderState.initialized) {
        pd->system->logToConsole("renderPage: renderer not initialized");
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        return 2;
    }

    const char* markdown = pd->lua->getArgString(1);
    if (!markdown) {
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        return 2;
    }

    // Cleanup previous links
    cleanupLinksInternal();

    // Parse markdown
    size_t len = strlen(markdown);
    cmark_node* doc = cmark_parse_document(markdown, len, CMARK_OPT_DEFAULT);
    if (!doc) {
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        return 2;
    }

    // Collect all text segments for drawing
    static TextSegment allSegments[MAX_TEXT_SEGMENTS];
    int totalSegments = 0;

    // Layout state
    int x = 0, y = 0;
    int h = renderState.fontHeight;
    int firstParagraph = 1;

    // Track current link state
    int inLink = 0;
    const char* linkUrl = NULL;
    static TextSegment linkSegments[MAX_SEGMENTS_PER_LINK];
    int linkSegmentCount = 0;

    // Iterate through AST
    cmark_iter* iter = cmark_iter_new(doc);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node* node = cmark_iter_get_node(iter);
        cmark_node_type type = cmark_node_get_type(node);

        if (ev_type == CMARK_EVENT_ENTER) {
            switch (type) {
                case CMARK_NODE_PARAGRAPH:
                    if (!firstParagraph) {
                        x = 0;
                        y += h * 2;  // Paragraph break
                    }
                    firstParagraph = 0;
                    break;

                case CMARK_NODE_LINK:
                    inLink = 1;
                    linkUrl = cmark_node_get_url(node);
                    linkSegmentCount = 0;
                    break;

                case CMARK_NODE_TEXT:
                case CMARK_NODE_CODE: {
                    const char* nodeText = cmark_node_get_literal(node);
                    if (nodeText) {
                        static TextSegment tempSegments[256];
                        int newX, newY;
                        int count = layoutWords(nodeText, x, y, tempSegments, 256, &newX, &newY);

                        // Add segments to total list
                        for (int i = 0; i < count && totalSegments < MAX_TEXT_SEGMENTS; i++) {
                            allSegments[totalSegments++] = tempSegments[i];
                        }

                        // If in a link, also add to link segments
                        if (inLink) {
                            for (int i = 0; i < count && linkSegmentCount < MAX_SEGMENTS_PER_LINK; i++) {
                                linkSegments[linkSegmentCount++] = tempSegments[i];
                            }
                        }

                        x = newX;
                        y = newY;
                    }
                    break;
                }

                case CMARK_NODE_SOFTBREAK:
                    // Treat as space - already handled in text
                    break;

                default:
                    break;
            }
        } else if (ev_type == CMARK_EVENT_EXIT) {
            if (type == CMARK_NODE_LINK && inLink) {
                // Create link from collected segments
                if (linkSegmentCount > 0 && renderState.linkCount < MAX_LINKS) {
                    LinkData* link = &renderState.links[renderState.linkCount];
                    memset(link, 0, sizeof(LinkData));

                    // Copy URL
                    if (linkUrl) {
                        strncpy(link->url, linkUrl, 511);
                        link->url[511] = '\0';
                    }

                    // Calculate bounding box
                    int minX = INT_MAX, minY = INT_MAX;
                    int maxX = INT_MIN, maxY = INT_MIN;

                    link->segmentCount = 0;
                    for (int i = 0; i < linkSegmentCount && i < MAX_SEGMENTS_PER_LINK; i++) {
                        TextSegment* seg = &linkSegments[i];

                        link->segments[link->segmentCount].x = seg->x;
                        link->segments[link->segmentCount].y = seg->y;
                        link->segments[link->segmentCount].w = seg->width;
                        link->segmentCount++;

                        if (seg->x < minX) minX = seg->x;
                        if (seg->y < minY) minY = seg->y;
                        if (seg->x + seg->width > maxX) maxX = seg->x + seg->width;
                        if (seg->y > maxY) maxY = seg->y;
                    }

                    link->offsetX = minX;
                    link->offsetY = minY;
                    link->width = maxX - minX;
                    link->height = maxY - minY + h;
                    link->textHeight = h;

                    if (link->width > 0 && link->height > 0) {
                        createLinkSprite(link);
                        renderState.linkCount++;
                    }
                }

                inLink = 0;
                linkUrl = NULL;
                linkSegmentCount = 0;
            }
        }
    }

    cmark_iter_free(iter);
    cmark_node_free(doc);

    // Calculate page height
    renderState.pageHeight = y + h + 2 * renderState.pagePadding;
    if (renderState.pageHeight < SCREEN_HEIGHT) {
        renderState.pageHeight = SCREEN_HEIGHT;
    }

    // Free old page image
    if (renderState.pageImage) {
        pd->graphics->freeBitmap(renderState.pageImage);
        renderState.pageImage = NULL;
    }

    // Create page image
    renderState.pageImage = pd->graphics->newBitmap(
        renderState.pageWidth, renderState.pageHeight, kColorClear);

    if (!renderState.pageImage) {
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        return 2;
    }

    // Draw all text to page image
    pd->graphics->pushContext(renderState.pageImage);
    pd->graphics->setFont(renderState.font);

    for (int i = 0; i < totalSegments; i++) {
        pd->graphics->drawText(allSegments[i].text, strlen(allSegments[i].text),
                               kASCIIEncoding,
                               renderState.pagePadding + allSegments[i].x,
                               renderState.pagePadding + allSegments[i].y);
    }

    pd->graphics->popContext();

    // Return page image, height, and link count to Lua
    pd->lua->pushBitmap(renderState.pageImage);
    pd->lua->pushInt(renderState.pageHeight);
    pd->lua->pushInt(renderState.linkCount);

    return 3;
}

// Get link data from a sprite (returns URL if it's a link sprite, nil otherwise)
static int getLinkData(lua_State* L) {
    (void)L;

    LCDSprite* sprite = pd->lua->getSprite(1);
    if (!sprite) {
        pd->lua->pushNil();
        return 1;
    }

    // Get userdata - if it's one of our link sprites, it will point to LinkData
    void* userdata = pd->sprite->getUserdata(sprite);
    if (!userdata) {
        pd->lua->pushNil();
        return 1;
    }

    // Verify this is actually one of our link sprites
    LinkData* link = (LinkData*)userdata;
    int isOurLink = 0;
    for (int i = 0; i < renderState.linkCount; i++) {
        if (&renderState.links[i] == link) {
            isOurLink = 1;
            break;
        }
    }

    if (!isOurLink) {
        pd->lua->pushNil();
        return 1;
    }

    pd->lua->pushString(link->url);
    return 1;
}

// Get link info by index (returns sprite, url)
static int getLinkInfo(lua_State* L) {
    (void)L;

    int index = pd->lua->getArgInt(1);

    if (index < 0 || index >= renderState.linkCount) {
        pd->lua->pushNil();
        pd->lua->pushNil();
        return 2;
    }

    LinkData* link = &renderState.links[index];
    if (!link->sprite) {
        pd->lua->pushNil();
        pd->lua->pushNil();
        return 2;
    }

    pd->lua->pushSprite(link->sprite);
    pd->lua->pushString(link->url);
    return 2;
}

// Set link hover state (redraws the link image)
static int setLinkHovered(lua_State* L) {
    (void)L;

    LCDSprite* sprite = pd->lua->getSprite(1);
    int isHovered = pd->lua->getArgBool(2);

    if (!sprite) {
        return 0;
    }

    // Get userdata
    void* userdata = pd->sprite->getUserdata(sprite);
    if (!userdata) {
        return 0;
    }

    // Verify this is one of our link sprites
    LinkData* link = (LinkData*)userdata;
    int isOurLink = 0;
    for (int i = 0; i < renderState.linkCount; i++) {
        if (&renderState.links[i] == link) {
            isOurLink = 1;
            break;
        }
    }

    if (!isOurLink || !link->image) {
        return 0;
    }

    // Redraw link image with new hover state
    drawLinkImage(link, isHovered);
    pd->sprite->markDirty(link->sprite);

    return 0;
}

// Move all link sprites by a delta (for viewport scrolling)
static int moveLinkSprites(lua_State* L) {
    (void)L;

    float dx = pd->lua->getArgFloat(1);
    float dy = pd->lua->getArgFloat(2);

    for (int i = 0; i < renderState.linkCount; i++) {
        LinkData* link = &renderState.links[i];
        if (link->sprite) {
            float x, y;
            pd->sprite->getPosition(link->sprite, &x, &y);
            pd->sprite->moveTo(link->sprite, x + dx, y + dy);
        }
    }

    return 0;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;

    if (event == kEventInitLua) {
        pd = playdate;

        const char* err;

        // Register cmark.parse function (kept for backwards compatibility)
        if (!pd->lua->addFunction(parseMarkdown, "cmark.parse", &err)) {
            pd->system->logToConsole("Failed to register cmark.parse: %s", err);
        }

        // Register rendering functions
        if (!pd->lua->addFunction(initRenderer, "cmark.initRenderer", &err)) {
            pd->system->logToConsole("Failed to register cmark.initRenderer: %s", err);
        }

        if (!pd->lua->addFunction(renderPage, "cmark.render", &err)) {
            pd->system->logToConsole("Failed to register cmark.render: %s", err);
        }

        if (!pd->lua->addFunction(cleanupLinks, "cmark.cleanupLinks", &err)) {
            pd->system->logToConsole("Failed to register cmark.cleanupLinks: %s", err);
        }

        if (!pd->lua->addFunction(getLinkData, "cmark.getLinkData", &err)) {
            pd->system->logToConsole("Failed to register cmark.getLinkData: %s", err);
        }

        if (!pd->lua->addFunction(getLinkInfo, "cmark.getLinkInfo", &err)) {
            pd->system->logToConsole("Failed to register cmark.getLinkInfo: %s", err);
        }

        if (!pd->lua->addFunction(setLinkHovered, "cmark.setLinkHovered", &err)) {
            pd->system->logToConsole("Failed to register cmark.setLinkHovered: %s", err);
        }

        if (!pd->lua->addFunction(moveLinkSprites, "cmark.moveLinkSprites", &err)) {
            pd->system->logToConsole("Failed to register cmark.moveLinkSprites: %s", err);
        }

        pd->system->logToConsole("cmark renderer functions registered");
    }

    return 0;
}
