//
//  main.c
//  ORBIT - cmark markdown parser and page renderer for Playdate
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "cmark.h"

static PlaydateAPI* pd = NULL;

// ============================================================================
// Page Rendering Data Structures
// ============================================================================

#define MAX_TEXT_SEGMENTS 1024
#define SCREEN_HEIGHT 240

typedef struct {
    int x, y;
    char text[512];
    int width;
} TextSegment;

// Font cache only - no other persistent state
static struct {
    LCDFont* font;
    int fontHeight;
} fontCache = {0};

// ============================================================================
// Page Rendering Functions
// ============================================================================

// Initialize renderer - just caches the font
// Args: fontPath
static int initRenderer(lua_State* L) {
    (void)L;

    const char* fontPath = pd->lua->getArgString(1);
    if (!fontPath) {
        pd->system->logToConsole("initRenderer: missing font path");
        pd->lua->pushBool(0);
        return 1;
    }

    const char* err = NULL;
    fontCache.font = pd->graphics->loadFont(fontPath, &err);
    if (err || !fontCache.font) {
        pd->system->logToConsole("Failed to load font '%s': %s", fontPath, err ? err : "unknown error");
        pd->lua->pushBool(0);
        return 1;
    }

    fontCache.fontHeight = pd->graphics->getFontHeight(fontCache.font);
    pd->system->logToConsole("Font loaded: height=%d", fontCache.fontHeight);

    pd->lua->pushBool(1);
    return 1;
}


// Word-wrap layout algorithm
// Returns: number of segments created, updates endX and endY
static int layoutWords(const char* text, int startX, int startY,
                       TextSegment* segments, int maxSegments,
                       int* endX, int* endY,
                       int contentWidth, int tracking) {
    if (!text || !segments || !fontCache.font) return 0;

    int segmentCount = 0;
    int x = startX;
    int y = startY;
    int h = fontCache.fontHeight;

    // Get space width (without tracking - we add tracking manually)
    int spaceWidth = pd->graphics->getTextWidth(fontCache.font, " ", 1, kASCIIEncoding, 0);

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
            int wordWidth = pd->graphics->getTextWidth(fontCache.font, word, wordLen, kASCIIEncoding, 0);

            // Wrap if needed
            if (x > 0 && x + wordWidth > contentWidth) {
                // Save current segment
                if (segLen > 0 && segmentCount < maxSegments) {
                    strncpy(segments[segmentCount].text, segment, 511);
                    segments[segmentCount].text[511] = '\0';
                    segments[segmentCount].x = segX;
                    segments[segmentCount].y = segY;
                    segments[segmentCount].width = pd->graphics->getTextWidth(
                        fontCache.font, segment, segLen, kASCIIEncoding, 0);
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
            fontCache.font, segment, segLen, kASCIIEncoding, 0);
        segmentCount++;
    }

    *endX = x;
    *endY = y;
    return segmentCount;
}


// Pure render function - parse markdown, create page image, return links as JSON
// Args: markdown, pageWidth, pagePadding, tracking
// Returns: pageImage, pageHeight, linksJSON
static int renderPage(lua_State* L) {
    (void)L;

    if (!fontCache.font) {
        pd->system->logToConsole("renderPage: font not loaded");
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    const char* markdown = pd->lua->getArgString(1);
    int pageWidth = pd->lua->getArgInt(2);
    int pagePadding = pd->lua->getArgInt(3);
    int tracking = pd->lua->getArgInt(4);

    if (!markdown) {
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    int contentWidth = pageWidth - 2 * pagePadding;
    int h = fontCache.fontHeight;

    // Parse markdown
    size_t len = strlen(markdown);
    cmark_node* doc = cmark_parse_document(markdown, len, CMARK_OPT_DEFAULT);
    if (!doc) {
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    // Collect all text segments for drawing
    static TextSegment allSegments[MAX_TEXT_SEGMENTS];
    int totalSegments = 0;

    // Build links JSON
    #define MAX_LINKS_JSON 16384
    #define MAX_SEGMENTS_PER_LINK 8
    static char linksJson[MAX_LINKS_JSON];
    int jsonPos = 0;
    int linkCount = 0;
    jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos, "[");

    // Layout state
    int x = 0, y = 0;
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
                        int count = layoutWords(nodeText, x, y, tempSegments, 256,
                                               &newX, &newY, contentWidth, tracking);

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
                // Add link to JSON
                if (linkSegmentCount > 0 && jsonPos < MAX_LINKS_JSON - 512) {
                    if (linkCount > 0) {
                        jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos, ",");
                    }

                    // Start link object with URL
                    jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos,
                                       "{\"url\":\"%s\",\"segments\":[",
                                       linkUrl ? linkUrl : "");

                    // Add segments as [x, y, w] arrays
                    for (int i = 0; i < linkSegmentCount; i++) {
                        TextSegment* seg = &linkSegments[i];
                        if (i > 0) {
                            jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos, ",");
                        }
                        jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos,
                                           "[%d,%d,%d]", seg->x, seg->y, seg->width);
                    }

                    jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos, "]}");
                    linkCount++;
                }

                inLink = 0;
                linkUrl = NULL;
                linkSegmentCount = 0;
            }
        }
    }

    // Close JSON array
    jsonPos += snprintf(linksJson + jsonPos, MAX_LINKS_JSON - jsonPos, "]");

    cmark_iter_free(iter);
    cmark_node_free(doc);

    // Calculate page height
    int pageHeight = y + h + 2 * pagePadding;
    if (pageHeight < SCREEN_HEIGHT) {
        pageHeight = SCREEN_HEIGHT;
    }

    // Create page image
    LCDBitmap* pageImage = pd->graphics->newBitmap(pageWidth, pageHeight, kColorClear);
    if (!pageImage) {
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    // Draw all text to page image
    pd->graphics->pushContext(pageImage);
    pd->graphics->setFont(fontCache.font);

    for (int i = 0; i < totalSegments; i++) {
        pd->graphics->drawText(allSegments[i].text, strlen(allSegments[i].text),
                               kASCIIEncoding,
                               pagePadding + allSegments[i].x,
                               pagePadding + allSegments[i].y);
    }

    pd->graphics->popContext();

    // Return page image, height, and links JSON to Lua
    pd->lua->pushBitmap(pageImage);
    pd->lua->pushInt(pageHeight);
    pd->lua->pushString(linksJson);

    return 3;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;

    if (event == kEventInitLua) {
        pd = playdate;

        const char* err;

        if (!pd->lua->addFunction(initRenderer, "cmark.initRenderer", &err)) {
            pd->system->logToConsole("Failed to register cmark.initRenderer: %s", err);
        }

        if (!pd->lua->addFunction(renderPage, "cmark.render", &err)) {
            pd->system->logToConsole("Failed to register cmark.render: %s", err);
        }

        pd->system->logToConsole("cmark functions registered");
    }

    return 0;
}
