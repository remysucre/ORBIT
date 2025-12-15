//
//  main.c
//  ORBIT - cmark markdown parser and lexbor HTML parser for Playdate
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "cmark.h"
#include "lexbor/html/html.h"
#include "lexbor/dom/interfaces/character_data.h"
#include "lexbor/css/css.h"
#include "lexbor/selectors/selectors.h"

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
// HTML Rendering Context
// ============================================================================

#define MAX_LINKS_JSON 16384
#define MAX_SEGMENTS_PER_LINK 8

typedef struct {
    // Layout state
    int x, y;
    int contentWidth;
    int tracking;
    int firstParagraph;

    // Text segments for final drawing
    TextSegment* segments;
    int segmentCount;
    int maxSegments;

    // Link tracking
    json_encoder* linkEncoder;
    TextSegment linkSegments[MAX_SEGMENTS_PER_LINK];
    int linkSegmentCount;
} RenderContext;

// JSON encoder buffer
static char* jsonBuffer;
static int jsonBufferPos;
static int jsonBufferSize;

static void jsonWrite(void* userdata, const char* str, int len) {
    (void)userdata;
    if (jsonBufferPos + len < jsonBufferSize) {
        memcpy(jsonBuffer + jsonBufferPos, str, len);
        jsonBufferPos += len;
    }
}

// ============================================================================
// HTML Text Extraction and Cleaning
// ============================================================================

// Extract all text content from a DOM node recursively
static void extractText(lxb_dom_node_t* node, char* buffer, int maxLen, int* pos) {
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_character_data_t* char_data = lxb_dom_interface_character_data(node);
            const lxb_char_t* text = char_data->data.data;
            size_t len = char_data->data.length;

            for (size_t i = 0; i < len && *pos < maxLen - 1; i++) {
                buffer[(*pos)++] = (char)text[i];
            }
        }

        if (node->first_child) {
            extractText(node->first_child, buffer, maxLen, pos);
        }

        node = node->next;
    }
    buffer[*pos] = '\0';
}

// Clean text: collapse whitespace, decode entities, trim
static void cleanText(char* text) {
    if (!text || !*text) return;

    char* src = text;
    char* dst = text;
    int lastWasSpace = 1;  // Treat start as space to trim leading

    while (*src) {
        char c = *src++;

        // Convert whitespace to space
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }

        // Collapse multiple spaces
        if (c == ' ') {
            if (!lastWasSpace) {
                *dst++ = ' ';
                lastWasSpace = 1;
            }
        } else {
            *dst++ = c;
            lastWasSpace = 0;
        }
    }

    // Trim trailing space
    if (dst > text && *(dst - 1) == ' ') {
        dst--;
    }
    *dst = '\0';

    // Decode common HTML entities in-place
    src = text;
    dst = text;
    while (*src) {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0) {
                *dst++ = '&';
                src += 5;
            } else if (strncmp(src, "&lt;", 4) == 0) {
                *dst++ = '<';
                src += 4;
            } else if (strncmp(src, "&gt;", 4) == 0) {
                *dst++ = '>';
                src += 4;
            } else if (strncmp(src, "&quot;", 6) == 0) {
                *dst++ = '"';
                src += 6;
            } else if (strncmp(src, "&apos;", 6) == 0) {
                *dst++ = '\'';
                src += 6;
            } else if (strncmp(src, "&#39;", 5) == 0) {
                *dst++ = '\'';
                src += 5;
            } else if (strncmp(src, "&nbsp;", 6) == 0) {
                *dst++ = ' ';
                src += 6;
            } else if (strncmp(src, "&#x", 3) == 0) {
                // Hex numeric entity
                char* end;
                unsigned long val = strtoul(src + 3, &end, 16);
                if (*end == ';' && val < 128) {
                    *dst++ = (char)val;
                    src = end + 1;
                } else {
                    *dst++ = *src++;
                }
            } else if (strncmp(src, "&#", 2) == 0) {
                // Decimal numeric entity
                char* end;
                unsigned long val = strtoul(src + 2, &end, 10);
                if (*end == ';' && val < 128) {
                    *dst++ = (char)val;
                    src = end + 1;
                } else {
                    *dst++ = *src++;
                }
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ============================================================================
// HTML Rendering Primitives
// ============================================================================

// Forward declaration of layoutWords (defined later)
static int layoutWords(const char* text, int startX, int startY,
                       TextSegment* segments, int maxSegments,
                       int* endX, int* endY,
                       int contentWidth, int tracking);

// Render plain text to the context
static void renderPlainText(RenderContext* ctx, const char* text) {
    if (!text || !*text || !ctx->segments) return;

    static TextSegment tempSegments[256];
    int newX, newY;
    int count = layoutWords(text, ctx->x, ctx->y, tempSegments, 256,
                            &newX, &newY, ctx->contentWidth, ctx->tracking);

    // Add segments to total list
    for (int i = 0; i < count && ctx->segmentCount < ctx->maxSegments; i++) {
        ctx->segments[ctx->segmentCount++] = tempSegments[i];
    }

    ctx->x = newX;
    ctx->y = newY;
}

// Render a link (text + record segments for JSON)
static void renderLink(RenderContext* ctx, const char* text, const char* url) {
    if (!text || !*text || !ctx->linkEncoder) return;

    // Render text normally
    static TextSegment tempSegments[256];
    int newX, newY;
    int count = layoutWords(text, ctx->x, ctx->y, tempSegments, 256,
                            &newX, &newY, ctx->contentWidth, ctx->tracking);

    // Add segments to total list and link segments
    ctx->linkSegmentCount = 0;
    for (int i = 0; i < count && ctx->segmentCount < ctx->maxSegments; i++) {
        ctx->segments[ctx->segmentCount++] = tempSegments[i];
        if (ctx->linkSegmentCount < MAX_SEGMENTS_PER_LINK) {
            ctx->linkSegments[ctx->linkSegmentCount++] = tempSegments[i];
        }
    }

    ctx->x = newX;
    ctx->y = newY;

    // Record link to JSON
    if (ctx->linkSegmentCount > 0) {
        json_encoder* enc = ctx->linkEncoder;
        enc->addArrayMember(enc);
        enc->startTable(enc);

        // URL
        enc->addTableMember(enc, "url", 3);
        enc->writeString(enc, url ? url : "", url ? (int)strlen(url) : 0);

        // Segments array
        enc->addTableMember(enc, "segments", 8);
        enc->startArray(enc);
        for (int i = 0; i < ctx->linkSegmentCount; i++) {
            TextSegment* seg = &ctx->linkSegments[i];
            enc->addArrayMember(enc);
            enc->startArray(enc);
            enc->addArrayMember(enc);
            enc->writeInt(enc, seg->x);
            enc->addArrayMember(enc);
            enc->writeInt(enc, seg->y);
            enc->addArrayMember(enc);
            enc->writeInt(enc, seg->width);
            enc->endArray(enc);
        }
        enc->endArray(enc);

        enc->endTable(enc);
    }
}

// Render a newline (paragraph break)
static void renderNewline(RenderContext* ctx) {
    ctx->x = 0;
    ctx->y += fontCache.fontHeight;
}

// ============================================================================
// Site-Specific HTML Renderers
// ============================================================================

// NPR Frontpage: Render list of article links
static void renderNPRFrontpage(RenderContext* ctx, lxb_html_document_t* document) {
    // Title
    renderPlainText(ctx, "NPR News");
    renderNewline(ctx);
    renderNewline(ctx);

    // Find all <a> tags and filter for article links
    lxb_dom_collection_t* collection = lxb_dom_collection_make(
        &document->dom_document, 128);

    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"a", 1);

    for (size_t i = 0; i < lxb_dom_collection_length(collection); i++) {
        lxb_dom_element_t* element = lxb_dom_collection_element(collection, i);

        // Get href attribute
        size_t hrefLen;
        const lxb_char_t* href = lxb_dom_element_get_attribute(
            element, (const lxb_char_t*)"href", 4, &hrefLen);

        if (!href || hrefLen == 0) continue;

        // Filter: only links starting with /g, /n, or /nx (article links)
        if (href[0] == '/' && (href[1] == 'g' || href[1] == 'n' ||
            (hrefLen > 2 && href[1] == 'n' && href[2] == 'x'))) {

            // Extract link text
            char text[512];
            int pos = 0;
            extractText(lxb_dom_interface_node(element)->first_child, text, sizeof(text), &pos);
            cleanText(text);

            if (text[0]) {
                // Build full URL
                char fullUrl[256];
                snprintf(fullUrl, sizeof(fullUrl), "https://text.npr.org%.*s", (int)hrefLen, href);

                renderLink(ctx, text, fullUrl);
                renderNewline(ctx);
                renderNewline(ctx);
            }
        }
    }

    lxb_dom_collection_destroy(collection, 1);
}

// Check if element is inside a tag with given name
static int isInsideTag(lxb_dom_node_t* node, const char* tagName, size_t tagLen) {
    lxb_dom_node_t* parent = node->parent;
    while (parent) {
        if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t* elem = lxb_dom_interface_element(parent);
            size_t len;
            const lxb_char_t* name = lxb_dom_element_qualified_name(elem, &len);
            if (name && len == tagLen && strncasecmp((const char*)name, tagName, tagLen) == 0) {
                return 1;
            }
        }
        parent = parent->parent;
    }
    return 0;
}

// Check if element has a class attribute containing the given class name
static int hasClass(lxb_dom_element_t* element, const char* className) {
    size_t len;
    const lxb_char_t* classAttr = lxb_dom_element_get_attribute(
        element, (const lxb_char_t*)"class", 5, &len);
    if (classAttr && strstr((const char*)classAttr, className)) {
        return 1;
    }
    return 0;
}

// NPR Article: Render article content
static void renderNPRArticle(RenderContext* ctx, lxb_html_document_t* document) {
    lxb_dom_collection_t* collection = lxb_dom_collection_make(
        &document->dom_document, 64);

    // Find title (h1) - skip if inside <header>
    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"h1", 2);

    for (size_t i = 0; i < lxb_dom_collection_length(collection); i++) {
        lxb_dom_element_t* h1 = lxb_dom_collection_element(collection, i);
        // Skip h1 inside <header>
        if (isInsideTag(lxb_dom_interface_node(h1), "header", 6)) {
            continue;
        }
        char text[512];
        int pos = 0;
        extractText(lxb_dom_interface_node(h1)->first_child, text, sizeof(text), &pos);
        cleanText(text);
        if (text[0]) {
            renderPlainText(ctx, text);
            renderNewline(ctx);
            renderNewline(ctx);
            break;  // Only render first non-header h1
        }
    }
    lxb_dom_collection_clean(collection);

    // Find paragraphs
    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"p", 1);

    for (size_t i = 0; i < lxb_dom_collection_length(collection); i++) {
        lxb_dom_element_t* p = lxb_dom_collection_element(collection, i);

        // Skip paragraphs inside <header>, <nav>, or <footer>
        lxb_dom_node_t* pNode = lxb_dom_interface_node(p);
        if (isInsideTag(pNode, "header", 6) ||
            isInsideTag(pNode, "nav", 3) ||
            isInsideTag(pNode, "footer", 6)) {
            continue;
        }

        // Skip paragraphs with class="slug-line"
        if (hasClass(p, "slug-line")) {
            continue;
        }

        char text[2048];
        int pos = 0;
        extractText(lxb_dom_interface_node(p)->first_child, text, sizeof(text), &pos);
        cleanText(text);

        if (text[0]) {
            renderPlainText(ctx, text);
            renderNewline(ctx);
            renderNewline(ctx);
        }
    }

    lxb_dom_collection_destroy(collection, 1);
}

// CSMonitor Frontpage: Render list of article links
static void renderCSMonitorFrontpage(RenderContext* ctx, lxb_html_document_t* document) {
    // Title
    renderPlainText(ctx, "Christian Science Monitor");
    renderNewline(ctx);
    renderNewline(ctx);

    // Find all <a> tags
    lxb_dom_collection_t* collection = lxb_dom_collection_make(
        &document->dom_document, 128);

    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"a", 1);

    for (size_t i = 0; i < lxb_dom_collection_length(collection); i++) {
        lxb_dom_element_t* element = lxb_dom_collection_element(collection, i);

        // Get href attribute
        size_t hrefLen;
        const lxb_char_t* href = lxb_dom_element_get_attribute(
            element, (const lxb_char_t*)"href", 4, &hrefLen);

        if (!href || hrefLen == 0) continue;

        // Filter: only links containing /text_edition/ and /20 (year pattern)
        if (strstr((const char*)href, "/text_edition/") &&
            strstr((const char*)href, "/20")) {

            // Build full URL
            char fullUrl[512];
            if (href[0] == '/') {
                snprintf(fullUrl, sizeof(fullUrl), "https://www.csmonitor.com%.*s", (int)hrefLen, href);
            } else {
                snprintf(fullUrl, sizeof(fullUrl), "%.*s", (int)hrefLen, href);
            }

            // Look for title element with data-field="title"
            char headline[512] = "";
            char summary[512] = "";

            // Search children for data-field attributes
            lxb_dom_node_t* child = lxb_dom_interface_node(element)->first_child;
            while (child) {
                if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    lxb_dom_element_t* childElem = lxb_dom_interface_element(child);
                    size_t attrLen;
                    const lxb_char_t* dataField = lxb_dom_element_get_attribute(
                        childElem, (const lxb_char_t*)"data-field", 10, &attrLen);

                    if (dataField) {
                        int pos = 0;
                        char text[512];
                        extractText(child->first_child, text, sizeof(text), &pos);
                        cleanText(text);

                        if (strncmp((const char*)dataField, "title", 5) == 0) {
                            strncpy(headline, text, sizeof(headline) - 1);
                        } else if (strncmp((const char*)dataField, "summary", 7) == 0) {
                            strncpy(summary, text, sizeof(summary) - 1);
                        }
                    }
                }
                child = child->next;
            }

            // Render if headline found
            if (headline[0]) {
                renderLink(ctx, headline, fullUrl);
                renderNewline(ctx);
                if (summary[0]) {
                    renderPlainText(ctx, summary);
                    renderNewline(ctx);
                }
                renderNewline(ctx);
            }
        }
    }

    lxb_dom_collection_destroy(collection, 1);
}

// CSMonitor Article: Render article content
static void renderCSMonitorArticle(RenderContext* ctx, lxb_html_document_t* document) {
    lxb_dom_collection_t* collection = lxb_dom_collection_make(
        &document->dom_document, 64);

    // Find title (h1)
    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"h1", 2);

    if (lxb_dom_collection_length(collection) > 0) {
        lxb_dom_element_t* h1 = lxb_dom_collection_element(collection, 0);
        char text[512];
        int pos = 0;
        extractText(lxb_dom_interface_node(h1)->first_child, text, sizeof(text), &pos);
        cleanText(text);
        if (text[0]) {
            renderPlainText(ctx, text);
            renderNewline(ctx);
            renderNewline(ctx);
        }
    }
    lxb_dom_collection_clean(collection);

    // Find time element for date
    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"time", 4);

    if (lxb_dom_collection_length(collection) > 0) {
        lxb_dom_element_t* timeElem = lxb_dom_collection_element(collection, 0);
        char text[256];
        int pos = 0;
        extractText(lxb_dom_interface_node(timeElem)->first_child, text, sizeof(text), &pos);
        cleanText(text);
        if (text[0]) {
            renderPlainText(ctx, text);
            renderNewline(ctx);
            renderNewline(ctx);
        }
    }
    lxb_dom_collection_clean(collection);

    // Find paragraphs
    lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        (const lxb_char_t*)"p", 1);

    for (size_t i = 0; i < lxb_dom_collection_length(collection); i++) {
        lxb_dom_element_t* p = lxb_dom_collection_element(collection, i);
        char text[2048];
        int pos = 0;
        extractText(lxb_dom_interface_node(p)->first_child, text, sizeof(text), &pos);
        cleanText(text);

        if (text[0]) {
            renderPlainText(ctx, text);
            renderNewline(ctx);
            renderNewline(ctx);
        }
    }

    lxb_dom_collection_destroy(collection, 1);
}

// Site renderer function pointer type
typedef void (*SiteRenderer)(RenderContext*, lxb_html_document_t*);

// Find appropriate renderer for URL
static SiteRenderer findRenderer(const char* url) {
    if (!url) return NULL;

    // NPR
    if (strcmp(url, "https://text.npr.org/") == 0 ||
        strcmp(url, "https://text.npr.org") == 0) {
        return renderNPRFrontpage;
    }
    if (strstr(url, "text.npr.org/") != NULL) {
        return renderNPRArticle;
    }

    // CSMonitor
    if (strcmp(url, "https://www.csmonitor.com/text_edition/") == 0 ||
        strcmp(url, "https://www.csmonitor.com/text_edition") == 0) {
        return renderCSMonitorFrontpage;
    }
    if (strstr(url, "csmonitor.com/text_edition/") != NULL) {
        return renderCSMonitorArticle;
    }

    return NULL;
}

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
    int spaceWidth = pd->graphics->getTextWidth(fontCache.font, " ", 1, kUTF8Encoding, 0);

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
            int wordWidth = pd->graphics->getTextWidth(fontCache.font, word, wordLen, kUTF8Encoding, 0);

            // Wrap if needed
            if (x > 0 && x + wordWidth > contentWidth) {
                // Save current segment
                if (segLen > 0 && segmentCount < maxSegments) {
                    strncpy(segments[segmentCount].text, segment, 511);
                    segments[segmentCount].text[511] = '\0';
                    segments[segmentCount].x = segX;
                    segments[segmentCount].y = segY;
                    segments[segmentCount].width = pd->graphics->getTextWidth(
                        fontCache.font, segment, segLen, kUTF8Encoding, 0);
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
            fontCache.font, segment, segLen, kUTF8Encoding, 0);
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

    // Build links JSON using encoder
    #define MAX_LINKS_JSON 16384
    #define MAX_SEGMENTS_PER_LINK 8
    static char linksJson[MAX_LINKS_JSON];
    jsonBuffer = linksJson;
    jsonBufferPos = 0;
    jsonBufferSize = MAX_LINKS_JSON;

    json_encoder encoder;
    pd->json->initEncoder(&encoder, jsonWrite, NULL, 0);
    encoder.startArray(&encoder);

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
                if (linkSegmentCount > 0) {
                    encoder.addArrayMember(&encoder);
                    encoder.startTable(&encoder);

                    // URL
                    encoder.addTableMember(&encoder, "url", 3);
                    encoder.writeString(&encoder, linkUrl ? linkUrl : "", linkUrl ? (int)strlen(linkUrl) : 0);

                    // Segments array
                    encoder.addTableMember(&encoder, "segments", 8);
                    encoder.startArray(&encoder);
                    for (int i = 0; i < linkSegmentCount; i++) {
                        TextSegment* seg = &linkSegments[i];
                        encoder.addArrayMember(&encoder);
                        encoder.startArray(&encoder);
                        encoder.addArrayMember(&encoder);
                        encoder.writeInt(&encoder, seg->x);
                        encoder.addArrayMember(&encoder);
                        encoder.writeInt(&encoder, seg->y);
                        encoder.addArrayMember(&encoder);
                        encoder.writeInt(&encoder, seg->width);
                        encoder.endArray(&encoder);
                    }
                    encoder.endArray(&encoder);

                    encoder.endTable(&encoder);
                }

                inLink = 0;
                linkUrl = NULL;
                linkSegmentCount = 0;
            }
        }
    }

    // Close JSON array
    encoder.endArray(&encoder);
    linksJson[jsonBufferPos] = '\0';

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
                               kUTF8Encoding,
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

// ============================================================================
// HTML Parsing Functions (lexbor)
// ============================================================================

#define MAX_JSON_SIZE 65536

// Recursive function to serialize DOM node to JSON
static void serializeNode(lxb_dom_node_t *node, json_encoder* enc, int* first) {
    if (node == NULL) return;

    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *element = lxb_dom_interface_element(node);

            // Get tag name
            size_t tagLen;
            const lxb_char_t *tagName = lxb_dom_element_qualified_name(element, &tagLen);

            // Skip script and style tags entirely
            if (tagName && tagLen > 0) {
                if ((tagLen == 6 && strncasecmp((const char*)tagName, "script", 6) == 0) ||
                    (tagLen == 5 && strncasecmp((const char*)tagName, "style", 5) == 0)) {
                    node = node->next;
                    continue;
                }
            }

            enc->addArrayMember(enc);
            enc->startTable(enc);

            // Tag name (lowercase)
            enc->addTableMember(enc, "tag", 3);
            char lowerTag[64];
            for (size_t i = 0; i < tagLen && i < 63; i++) {
                char c = tagName[i];
                lowerTag[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            }
            lowerTag[tagLen < 63 ? tagLen : 63] = '\0';
            enc->writeString(enc, lowerTag, (int)strlen(lowerTag));

            // Serialize attributes
            lxb_dom_attr_t *attr = lxb_dom_element_first_attribute(element);
            if (attr != NULL) {
                enc->addTableMember(enc, "attrs", 5);
                enc->startTable(enc);
                while (attr != NULL) {
                    size_t nameLen, valueLen;
                    const lxb_char_t *attrName = lxb_dom_attr_qualified_name(attr, &nameLen);
                    const lxb_char_t *attrValue = lxb_dom_attr_value(attr, &valueLen);

                    if (attrName && nameLen > 0) {
                        enc->addTableMember(enc, (const char*)attrName, (int)nameLen);
                        enc->writeString(enc, attrValue ? (const char*)attrValue : "", attrValue ? (int)valueLen : 0);
                    }

                    attr = lxb_dom_element_next_attribute(attr);
                }
                enc->endTable(enc);
            }

            // Serialize children
            if (node->first_child != NULL) {
                enc->addTableMember(enc, "children", 8);
                enc->startArray(enc);
                int childFirst = 1;
                serializeNode(node->first_child, enc, &childFirst);
                enc->endArray(enc);
            }

            enc->endTable(enc);
            *first = 0;

        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_character_data_t *char_data = lxb_dom_interface_character_data(node);
            const lxb_char_t *textContent = char_data->data.data;
            size_t textLen = char_data->data.length;

            if (textContent && textLen > 0) {
                enc->addArrayMember(enc);
                enc->startTable(enc);
                enc->addTableMember(enc, "text", 4);
                enc->writeString(enc, (const char*)textContent, (int)textLen);
                enc->endTable(enc);
                *first = 0;
            }
        }

        node = node->next;
    }
}

// Render HTML page using site-specific renderer
// Args: htmlString, url, pageWidth, pagePadding, tracking
// Returns: pageImage, pageHeight, linksJSON
static int renderHTML(lua_State* L) {
    (void)L;

    if (!fontCache.font) {
        pd->system->logToConsole("renderHTML: font not loaded");
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    const char* html = pd->lua->getArgString(1);
    const char* url = pd->lua->getArgString(2);
    int pageWidth = pd->lua->getArgInt(3);
    int pagePadding = pd->lua->getArgInt(4);
    int tracking = pd->lua->getArgInt(5);

    if (!html || !url) {
        pd->system->logToConsole("renderHTML: missing arguments");
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    // Find site-specific renderer
    SiteRenderer renderer = findRenderer(url);
    if (!renderer) {
        pd->system->logToConsole("renderHTML: no renderer for URL: %s", url);
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    // Parse HTML
    lxb_html_document_t* document = lxb_html_document_create();
    if (!document) {
        pd->system->logToConsole("renderHTML: failed to create document");
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    lxb_status_t status = lxb_html_document_parse(document,
        (const lxb_char_t*)html, strlen(html));

    if (status != LXB_STATUS_OK || !document->body) {
        pd->system->logToConsole("renderHTML: failed to parse HTML");
        lxb_html_document_destroy(document);
        pd->lua->pushNil();
        pd->lua->pushInt(SCREEN_HEIGHT);
        pd->lua->pushString("[]");
        return 3;
    }

    // Initialize render context
    static TextSegment allSegments[MAX_TEXT_SEGMENTS];
    static char linksJson[MAX_LINKS_JSON];

    jsonBuffer = linksJson;
    jsonBufferPos = 0;
    jsonBufferSize = MAX_LINKS_JSON;

    json_encoder encoder;
    pd->json->initEncoder(&encoder, jsonWrite, NULL, 0);
    encoder.startArray(&encoder);

    RenderContext ctx = {
        .x = 0,
        .y = 0,
        .contentWidth = pageWidth - 2 * pagePadding,
        .tracking = tracking,
        .firstParagraph = 1,
        .segments = allSegments,
        .segmentCount = 0,
        .maxSegments = MAX_TEXT_SEGMENTS,
        .linkEncoder = &encoder,
        .linkSegmentCount = 0
    };

    // Run site-specific renderer
    renderer(&ctx, document);

    // Close JSON array
    encoder.endArray(&encoder);
    linksJson[jsonBufferPos] = '\0';

    // Cleanup document
    lxb_html_document_destroy(document);

    // Calculate page height
    int pageHeight = ctx.y + fontCache.fontHeight + 2 * pagePadding;
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

    for (int i = 0; i < ctx.segmentCount; i++) {
        pd->graphics->drawText(allSegments[i].text, strlen(allSegments[i].text),
                               kUTF8Encoding,
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

// Parse HTML and return JSON DOM tree (legacy, kept for compatibility)
// Args: html (string)
// Returns: json (string) or nil on error
static int parseHTML(lua_State* L) {
    (void)L;

    const char* html = pd->lua->getArgString(1);
    if (!html) {
        pd->system->logToConsole("parseHTML: missing html argument");
        pd->lua->pushNil();
        return 1;
    }

    // Create HTML document
    lxb_html_document_t *document = lxb_html_document_create();
    if (document == NULL) {
        pd->system->logToConsole("parseHTML: failed to create document");
        pd->lua->pushNil();
        return 1;
    }

    // Parse HTML
    lxb_status_t status = lxb_html_document_parse(document,
        (const lxb_char_t *)html, strlen(html));

    if (status != LXB_STATUS_OK) {
        pd->system->logToConsole("parseHTML: failed to parse HTML");
        lxb_html_document_destroy(document);
        pd->lua->pushNil();
        return 1;
    }

    // Set up JSON encoder
    static char json[MAX_JSON_SIZE];
    jsonBuffer = json;
    jsonBufferPos = 0;
    jsonBufferSize = MAX_JSON_SIZE;

    json_encoder enc;
    pd->json->initEncoder(&enc, jsonWrite, NULL, 0);

    // Start with root object containing the document
    enc.startTable(&enc);
    enc.addTableMember(&enc, "children", 8);
    enc.startArray(&enc);

    // Serialize the document body (or full document if no body)
    lxb_dom_node_t *root = lxb_dom_interface_node(document);
    int first = 1;
    if (document->body != NULL) {
        root = lxb_dom_interface_node(document->body);
        serializeNode(root->first_child, &enc, &first);
    } else if (root->first_child != NULL) {
        serializeNode(root->first_child, &enc, &first);
    }

    enc.endArray(&enc);
    enc.endTable(&enc);
    json[jsonBufferPos] = '\0';

    // Cleanup
    lxb_html_document_destroy(document);

    // Return JSON string
    pd->lua->pushString(json);
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

        if (!pd->lua->addFunction(initRenderer, "cmark.initRenderer", &err)) {
            pd->system->logToConsole("Failed to register cmark.initRenderer: %s", err);
        }

        if (!pd->lua->addFunction(renderPage, "cmark.render", &err)) {
            pd->system->logToConsole("Failed to register cmark.render: %s", err);
        }

        if (!pd->lua->addFunction(parseHTML, "html.parse", &err)) {
            pd->system->logToConsole("Failed to register html.parse: %s", err);
        }

        if (!pd->lua->addFunction(renderHTML, "html.render", &err)) {
            pd->system->logToConsole("Failed to register html.render: %s", err);
        }

        pd->system->logToConsole("cmark and html functions registered");
    }

    return 0;
}
