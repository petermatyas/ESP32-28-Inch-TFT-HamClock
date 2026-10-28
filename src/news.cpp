#include "news.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <UbuntuMono_Regular8pt7b.h>
#include <JetBrainsMono_Light7pt7b.h>
#include <HB9IIUOrbitronMed8pt.h>

// =============================================================================
// 1. Configuration and state
// =============================================================================

// The global colour theme (wip.cpp), so this page follows the chosen style.
extern uint16_t themeBg, themeFg, themeDim, themeDim2, themeAccent, themeWarn;

#define NEWS_CONFIG_FILE "/news.json"
#define NEWS_TITLE_LEN   128

static const uint16_t REFRESH_MIN_LIMIT = 5;
static const uint16_t REFRESH_MAX_LIMIT = 240;

// A failed feed is tried again sooner than the normal refresh, and fetches are
// spaced out so a refresh of every feed never freezes the screen for long.
static const unsigned long RETRY_MS     = 2UL * 60UL * 1000UL;
static const unsigned long FETCH_GAP_MS = 20UL * 1000UL;

// Boot already runs the weather, solar, satellite and DX fetches; a TLS
// session on top of those left under 1 KB of heap, so news waits its turn.
static const unsigned long BOOT_DELAY_MS = 60UL * 1000UL;

// How long one fetch may take, and how much of a feed is read at most.  Only
// the first few items are wanted, and they come first.
static const unsigned long FETCH_DEADLINE_MS = 8000;
static const size_t        FETCH_MAX_BYTES   = 300000;

struct NewsFeed
{
    char name[24];          // as entered; empty means the feed's own title
    char url[160];
    char channel[24];       // the feed's own <title>
    char title[NEWS_ITEMS_PER_FEED][NEWS_TITLE_LEN];
    uint8_t count;
    bool tried;             // fetched at least once since the URL was set
    bool ok;                // last fetch found items
    unsigned long fetchedMs;
    char status[32];
};

static bool g_enabled = false;
static uint16_t g_refreshMin = 15;

// Static RAM is full, so the feeds and the parser's scratch titles (about
// 5 KB) are one heap block taken in newsBegin(), before the heap fragments.
struct NewsStore
{
    NewsFeed feeds[NEWS_MAX_FEEDS];
    char tmpTitles[NEWS_ITEMS_PER_FEED][NEWS_TITLE_LEN]; // parsed here first, so
    char tmpChannel[24];                                 // a failed fetch keeps the old ones
};
static NewsStore *g_store = nullptr;
static NewsFeed *g_feeds = nullptr;

static uint8_t g_nextFeed = 0;          // which feed newsFetchNext() fetches
static unsigned long g_lastFetchMs = 0; // any feed, for FETCH_GAP_MS
static bool g_itemsChanged = true;      // the page must repaint its list

static void setFeedDefaults()
{
    memset(g_feeds, 0, sizeof(g_store->feeds));
    strlcpy(g_feeds[0].name, "ARRL", sizeof(g_feeds[0].name));
    // Plain HTTP where a site offers it: the TLS handshake with arrl.org took
    // the heap down to under 1 KB.
    strlcpy(g_feeds[0].url, "http://www.arrl.org/news/rss", sizeof(g_feeds[0].url));
    strlcpy(g_feeds[1].name, "Telex", sizeof(g_feeds[1].name));
    strlcpy(g_feeds[1].url, "https://telex.hu/rss", sizeof(g_feeds[1].url));
}

static const char *feedLabel(const NewsFeed &f)
{
    if (f.name[0]) return f.name;
    if (f.channel[0]) return f.channel;
    return "RSS";
}

// =============================================================================
// 2. Text: entities and UTF-8 down to the 7-bit ASCII the fonts carry
// =============================================================================

// U+00C0..U+00FF and U+0100..U+017F with their accents taken off.  That covers
// Hungarian and the other Latin-script European languages.
static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
                             "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
static const char LATIN_EXT_A[] =
    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Ii" "Jj"
    "Kkk" "LlLlLlLlLl" "NnNnNnn" "Nn" "OoOoOo" "Oo" "RrRrRr" "SsSsSsSs" "TtTtTt"
    "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s";
static_assert(sizeof(LATIN1) == 64 + 1, "LATIN1 must cover U+00C0..U+00FF");
static_assert(sizeof(LATIN_EXT_A) == 128 + 1, "LATIN_EXT_A must cover U+0100..U+017F");

// Appends code point cp as ASCII, collapsing runs of whitespace.  Anything with
// no ASCII stand-in (Cyrillic, emoji...) is dropped.
static void putAscii(uint32_t cp, char *out, size_t n, size_t &o)
{
    char one[2] = {0, 0};
    const char *s = one;

    if (cp < 0x80)                       one[0] = (cp < 0x20) ? ' ' : (char)cp;
    else if (cp == 0xA0)                 one[0] = ' ';
    else if (cp >= 0xC0 && cp <= 0xFF)   one[0] = LATIN1[cp - 0xC0];
    else if (cp >= 0x100 && cp <= 0x17F) one[0] = LATIN_EXT_A[cp - 0x100];
    else
    {
        switch (cp)
        {
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
            s = "'"; break;
        case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: case 0x2033:
            s = "\""; break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212:
            s = "-"; break;
        case 0x2022: case 0xB7:
            s = "*"; break;
        case 0x2026:
            s = "..."; break;
        default:
            return;
        }
    }

    for (; *s; s++)
    {
        if (*s == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        if (o + 1 >= n) return;
        out[o++] = *s;
    }
}

struct NamedEntity
{
    const char *name;
    uint16_t cp;
};

static const NamedEntity ENTITIES[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"bdquo", 0x201E}, {"laquo", 0xAB}, {"raquo", 0xBB},
};

// "&amp;" / "&#337;" / "&#x151;" at in[0].  Returns the length consumed, or 0
// when it is not an entity this knows - the '&' is then shown as it is.
static size_t decodeEntity(const char *in, uint32_t &cp)
{
    const char *semi = (const char *)memchr(in, ';', 12);
    if (!semi) return 0;
    size_t len = semi - in + 1;

    if (in[1] == '#')
    {
        bool hex = (in[2] == 'x' || in[2] == 'X');
        cp = strtoul(in + (hex ? 3 : 2), nullptr, hex ? 16 : 10);
        return len;
    }
    for (const NamedEntity &e : ENTITIES)
    {
        size_t nl = strlen(e.name);
        if (nl == len - 2 && strncmp(in + 1, e.name, nl) == 0)
        {
            cp = e.cp;
            return len;
        }
    }
    return 0;
}

static void decodeToAscii(const char *in, char *out, size_t n)
{
    size_t o = 0;
    while (*in)
    {
        uint8_t b = (uint8_t)*in;
        uint32_t cp;

        if (b == '&')
        {
            size_t used = decodeEntity(in, cp);
            if (used)
            {
                putAscii(cp, out, n, o);
                in += used;
                continue;
            }
            putAscii('&', out, n, o);
            in++;
            continue;
        }

        int extra = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : (b >= 0xC0) ? 1 : 0;
        if (b >= 0x80 && extra == 0)
        {
            in++; // stray continuation byte
            continue;
        }
        cp = extra ? (b & (0x3F >> extra)) : b;
        in++;
        bool good = true;
        for (int i = 0; i < extra; i++, in++)
        {
            if (((uint8_t)*in & 0xC0) != 0x80)
            {
                good = false;
                break;
            }
            cp = (cp << 6) | ((uint8_t)*in & 0x3F);
        }
        if (good) putAscii(cp, out, n, o);
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

// =============================================================================
// 3. Streaming RSS / Atom parser
// =============================================================================

// Fed one byte at a time.  It only follows <item>/<entry> and <title>, so it
// needs no tree and no more memory than one raw title.  CDATA sections and
// attributes are handled; anything else between angle brackets is skipped.
enum ParseState : uint8_t { P_TEXT, P_TAG, P_BANG, P_CDATA, P_SKIP };

static struct
{
    ParseState state;
    char tag[16];
    uint8_t tagLen;
    bool tagNameDone;
    bool lastSlash;
    char bang[8];
    uint8_t bangLen;
    uint8_t brackets;
    bool inItem, itemHasTitle, capturing, haveChannel;
    char cap[256];
    uint16_t capLen;
    uint8_t count;
} P;

static void parserReset()
{
    memset(&P, 0, sizeof(P));
    g_store->tmpChannel[0] = '\0';
}

static inline void capAppend(char c)
{
    if (P.capturing && P.capLen < sizeof(P.cap) - 1) P.cap[P.capLen++] = c;
}

static void onTag(const char *name, bool selfClosing)
{
    bool closing = (name[0] == '/');
    const char *n = closing ? name + 1 : name;

    if (!strcmp(n, "item") || !strcmp(n, "entry"))
    {
        if (!closing)
        {
            P.inItem = true;
            P.itemHasTitle = false;
        }
        else
        {
            if (P.inItem && P.itemHasTitle) P.count++;
            P.inItem = false;
        }
    }
    else if (!strcmp(n, "title"))
    {
        if (!closing && !selfClosing)
        {
            P.capturing = true;
            P.capLen = 0;
        }
        else if (closing && P.capturing)
        {
            P.capturing = false;
            P.cap[P.capLen] = '\0';
            if (P.inItem)
            {
                if (!P.itemHasTitle && P.count < NEWS_ITEMS_PER_FEED)
                {
                    decodeToAscii(P.cap, g_store->tmpTitles[P.count], NEWS_TITLE_LEN);
                    P.itemHasTitle = g_store->tmpTitles[P.count][0] != '\0';
                }
            }
            else if (!P.haveChannel)
            {
                // The first title outside an item names the feed; later ones
                // (e.g. inside <image>) do not.
                decodeToAscii(P.cap, g_store->tmpChannel, sizeof(g_store->tmpChannel));
                P.haveChannel = true;
            }
        }
    }
}

// Returns false once enough items have been read.
static bool parserFeed(char c)
{
    switch (P.state)
    {
    case P_TEXT:
        if (c == '<')
        {
            P.state = P_TAG;
            P.tagLen = 0;
            P.tagNameDone = false;
            P.lastSlash = false;
        }
        else
            capAppend(c);
        break;

    case P_TAG:
        if (P.tagLen == 0 && !P.tagNameDone && c == '!')
        {
            P.state = P_BANG;
            P.bangLen = 0;
        }
        else if (P.tagLen == 0 && c == '?')
            P.state = P_SKIP;
        else if (c == '>')
        {
            P.tag[P.tagLen] = '\0';
            onTag(P.tag, P.lastSlash);
            P.state = P_TEXT;
        }
        else
        {
            P.lastSlash = (c == '/');
            if (!P.tagNameDone)
            {
                if (isspace((unsigned char)c) || (c == '/' && P.tagLen > 0))
                    P.tagNameDone = true;
                else if (P.tagLen < sizeof(P.tag) - 1)
                    P.tag[P.tagLen++] = (char)tolower((unsigned char)c);
            }
        }
        break;

    case P_BANG:
        P.bang[P.bangLen++] = c;
        if (memcmp(P.bang, "[CDATA[", P.bangLen) != 0)
            P.state = (c == '>') ? P_TEXT : P_SKIP;
        else if (P.bangLen == 7)
        {
            P.state = P_CDATA;
            P.brackets = 0;
        }
        break;

    case P_CDATA:
        if (c == ']' && P.brackets < 2)
            P.brackets++;
        else if (c == '>' && P.brackets == 2)
            P.state = P_TEXT;
        else
        {
            for (; P.brackets; P.brackets--) capAppend(']');
            capAppend(c);
        }
        break;

    case P_SKIP:
        if (c == '>') P.state = P_TEXT;
        break;
    }
    return P.count < NEWS_ITEMS_PER_FEED;
}

// =============================================================================
// 4. Fetching
// =============================================================================

bool newsEnabled()
{
    return g_enabled && g_store;
}

bool newsFetchDue(unsigned long nowMs)
{
    if (!newsEnabled() || WiFi.status() != WL_CONNECTED) return false;
    if (nowMs < BOOT_DELAY_MS) return false;
    if (g_lastFetchMs != 0 && nowMs - g_lastFetchMs < FETCH_GAP_MS) return false;

    unsigned long refreshMs = (unsigned long)g_refreshMin * 60UL * 1000UL;
    for (uint8_t i = 0; i < NEWS_MAX_FEEDS; i++)
    {
        const NewsFeed &f = g_feeds[i];
        if (!f.url[0]) continue;
        if (!f.tried || nowMs - f.fetchedMs >= (f.ok ? refreshMs : RETRY_MS))
        {
            g_nextFeed = i;
            return true;
        }
    }
    return false;
}

void newsFetchNext()
{
    NewsFeed &f = g_feeds[g_nextFeed];
    f.tried = true;
    f.fetchedMs = g_lastFetchMs = millis();

    HTTPClient http;
    // HTTP/1.0 keeps the body free of chunk headers, so it can be parsed as
    // it arrives.
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(FETCH_DEADLINE_MS);
    http.setUserAgent("HamClock");
    if (!http.begin(f.url))
    {
        f.ok = false;
        strlcpy(f.status, "bad URL", sizeof(f.status));
        Serial.printf("News: %s - bad URL %s\n", feedLabel(f), f.url);
        g_itemsChanged = true;
        return;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        http.end();
        f.ok = false;
        if (code > 0)
            snprintf(f.status, sizeof(f.status), "HTTP %d", code);
        else
            snprintf(f.status, sizeof(f.status), "%.31s", HTTPClient::errorToString(code).c_str());
        Serial.printf("News: %s - %s (free heap %u, largest block %u)\n", feedLabel(f), f.status,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        g_itemsChanged = true;
        return;
    }

    parserReset();
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[256];
    size_t total = 0;
    bool more = true;
    unsigned long deadline = millis() + FETCH_DEADLINE_MS;

    while (more && total < FETCH_MAX_BYTES && (long)(deadline - millis()) > 0)
    {
        int avail = stream->available();
        if (avail <= 0)
        {
            if (!stream->connected()) break;
            delay(5);
            continue;
        }
        int n = stream->read(buf, avail < (int)sizeof(buf) ? avail : (int)sizeof(buf));
        for (int i = 0; i < n && more; i++) more = parserFeed((char)buf[i]);
        total += (n > 0) ? n : 0;
    }
    http.end();

    if (P.count == 0)
    {
        f.ok = false;
        strlcpy(f.status, "no headlines found", sizeof(f.status));
    }
    else
    {
        memcpy(f.title, g_store->tmpTitles, sizeof(f.title));
        f.count = P.count;
        if (g_store->tmpChannel[0]) strlcpy(f.channel, g_store->tmpChannel, sizeof(f.channel));
        f.ok = true;
        snprintf(f.status, sizeof(f.status), "%u headlines", f.count);
    }
    g_itemsChanged = true;
    Serial.printf("News: %s - %s (%u bytes read)\n", feedLabel(f), f.status, (unsigned)total);
}

// =============================================================================
// 5. Persistence
// =============================================================================

static void saveConfig()
{
    JsonDocument doc;
    doc["enabled"] = g_enabled;
    doc["refreshMin"] = g_refreshMin;
    JsonArray arr = doc["feeds"].to<JsonArray>();
    for (uint8_t i = 0; i < NEWS_MAX_FEEDS; i++)
    {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = g_feeds[i].name;
        o["url"] = g_feeds[i].url;
    }

    fs::File file = SPIFFS.open(NEWS_CONFIG_FILE, "w");
    if (!file)
    {
        Serial.println("News: could not write " NEWS_CONFIG_FILE);
        return;
    }
    serializeJsonPretty(doc, file);
    file.close();
}

// Takes a feed entry from the web page or the config file.  A changed URL
// throws away what the old one had shown.
static void applyFeed(NewsFeed &f, JsonObjectConst o)
{
    char name[sizeof(f.name)];
    decodeToAscii(o["name"] | "", name, sizeof(name));
    strlcpy(f.name, name, sizeof(f.name));

    char url[sizeof(f.url)];
    strlcpy(url, o["url"] | "", sizeof(url));
    // Trim the spaces a paste tends to bring along.
    char *start = url;
    while (*start == ' ') start++;
    size_t len = strlen(start);
    while (len > 0 && start[len - 1] == ' ') start[--len] = '\0';

    if (strcmp(start, f.url) != 0)
    {
        strlcpy(f.url, start, sizeof(f.url));
        f.count = 0;
        f.channel[0] = '\0';
        f.tried = false;
        f.ok = false;
        f.status[0] = '\0';
    }
}

static void applyConfig(const JsonDocument &doc)
{
    g_enabled = doc["enabled"] | g_enabled;
    g_refreshMin = constrain((int)(doc["refreshMin"] | (int)g_refreshMin),
                             (int)REFRESH_MIN_LIMIT, (int)REFRESH_MAX_LIMIT);
    JsonArrayConst arr = doc["feeds"];
    if (arr.isNull()) return;
    for (uint8_t i = 0; i < NEWS_MAX_FEEDS; i++)
    {
        if (i < arr.size())
            applyFeed(g_feeds[i], arr[i].as<JsonObjectConst>());
        else
            applyFeed(g_feeds[i], JsonObjectConst());
    }
}

static void loadConfig()
{
    setFeedDefaults();

    fs::File file = SPIFFS.open(NEWS_CONFIG_FILE, "r");
    if (!file)
    {
        Serial.println("News: no " NEWS_CONFIG_FILE " yet - page off, default feeds");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err)
    {
        Serial.printf("News: %s is unreadable (%s) - using defaults\n", NEWS_CONFIG_FILE, err.c_str());
        return;
    }
    applyConfig(doc);
}

void newsBegin()
{
    g_store = (NewsStore *)calloc(1, sizeof(NewsStore));
    if (!g_store)
    {
        Serial.println("News: no memory for the feeds - news page disabled");
        return;
    }
    g_feeds = g_store->feeds;

    loadConfig();
    Serial.printf("News: page %s, refresh %u min\n", g_enabled ? "on" : "off", g_refreshMin);
    for (uint8_t i = 0; i < NEWS_MAX_FEEDS; i++)
        if (g_feeds[i].url[0]) Serial.printf("News:   %s  %s\n", feedLabel(g_feeds[i]), g_feeds[i].url);
}

// =============================================================================
// 6. Web routes
// =============================================================================

static void handleNewsPage(WebServer &server)
{
    fs::File f = SPIFFS.open("/news.html", "r");
    if (!f)
    {
        server.send(404, "text/plain", "news.html is missing - upload the SPIFFS image");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

static void buildConfigJson(String &out)
{
    JsonDocument doc;
    doc["enabled"] = g_enabled;
    doc["refreshMin"] = g_refreshMin;
    doc["minMin"] = REFRESH_MIN_LIMIT;
    doc["maxMin"] = REFRESH_MAX_LIMIT;

    unsigned long now = millis();
    JsonArray arr = doc["feeds"].to<JsonArray>();
    for (uint8_t i = 0; g_feeds && i < NEWS_MAX_FEEDS; i++)
    {
        const NewsFeed &f = g_feeds[i];
        JsonObject o = arr.add<JsonObject>();
        o["name"] = f.name;
        o["url"] = f.url;
        o["channel"] = f.channel;
        o["status"] = f.status;
        o["ok"] = f.ok;
        o["ageMin"] = f.tried ? (long)((now - f.fetchedMs) / 60000UL) : -1;
        JsonArray t = o["titles"].to<JsonArray>();
        for (uint8_t i = 0; i < f.count; i++) t.add(f.title[i]);
    }
    serializeJson(doc, out);
}

static void handleSaveConfig(WebServer &server)
{
    JsonDocument doc;
    if (!g_store)
    {
        server.send(503, "application/json", "{\"error\":\"no memory for the feeds\"}");
        return;
    }
    if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain")))
    {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    applyConfig(doc);
    saveConfig();
    g_itemsChanged = true;
    Serial.printf("News: config saved - page %s, refresh %u min\n",
                  g_enabled ? "on" : "off", g_refreshMin);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void newsRegisterRoutes(WebServer &server)
{
    server.on("/news.html", HTTP_GET, [&server]() { handleNewsPage(server); });
    server.on("/newscfg", HTTP_GET, [&server]() {
        String out;
        buildConfigJson(out);
        server.send(200, "application/json", out);
    });
    server.on("/newscfg", HTTP_POST, [&server]() { handleSaveConfig(server); });
    server.on("/newsnow", HTTP_GET, [&server]() {
        for (uint8_t i = 0; g_feeds && i < NEWS_MAX_FEEDS; i++) g_feeds[i].tried = false;
        g_lastFetchMs = 0;
        server.send(200, "text/plain", "OK");
    });
}

// =============================================================================
// 7. Rendering
// =============================================================================

static const int NY_RULE1 = 19;
static const int NY_BODY  = 26;
static const int NY_RULE2 = 206;
static const int NY_FOOT  = 212;
static const int NX_LEFT  = 6;
static const int N_LINE_H = 16;    // UbuntuMono 8pt line advance
static const int N_ITEM_GAP = 6;
static const int N_MAX_LINES = 3;  // per headline, the last one ends in "..."

static const unsigned long ROTATE_MS = 12000; // next screenful of headlines

struct Slot
{
    uint8_t feed, item;
};

// Feeds take turns, so one busy feed does not push the others off the page.
static uint8_t buildOrder(Slot *out)
{
    uint8_t n = 0;
    if (!g_feeds) return 0;
    for (uint8_t i = 0; i < NEWS_ITEMS_PER_FEED; i++)
        for (uint8_t f = 0; f < NEWS_MAX_FEEDS; f++)
            if (g_feeds[f].url[0] && i < g_feeds[f].count) out[n++] = {f, i};
    return n;
}

// Splits text into at most N_MAX_LINES lines of `width` characters, the first
// `firstIndent` shorter.  Breaks at spaces where it can.
static int wrapLines(const char *text, int width, int firstIndent,
                     char lines[N_MAX_LINES][64])
{
    int count = 0;
    const char *p = text;
    while (*p && count < N_MAX_LINES)
    {
        int cap = width - (count == 0 ? firstIndent : 0);
        if (cap > 63) cap = 63;
        int len = strlen(p);
        bool last = (count == N_MAX_LINES - 1);

        int take = len;
        if (len > cap)
        {
            if (last)
                take = cap - 3;
            else
            {
                take = cap;
                for (int i = cap; i > cap / 2; i--)
                    if (p[i] == ' ')
                    {
                        take = i;
                        break;
                    }
            }
        }
        memcpy(lines[count], p, take);
        lines[count][take] = '\0';
        if (last && len > cap) strcat(lines[count], "...");
        count++;
        p += take;
        while (*p == ' ') p++;
    }
    return count;
}

// Draws one headline at y, prefixed with its feed's name.  Returns the height
// used, or 0 when it would run past `bottom` (nothing is drawn then).
static int drawHeadline(TFT_eSPI &tft, int y, int bottom, const char *source,
                        const char *text, bool force)
{
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    // One glyph's width is its ink, not its advance; ten of them give the pitch.
    int cw = (tft.textWidth("MMMMMMMMMM") + 9) / 10;
    int width = (320 - 2 * NX_LEFT) / cw;

    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%.10s ", source);
    int indent = strlen(prefix);

    char lines[N_MAX_LINES][64];
    int count = wrapLines(text, width, indent, lines);
    int h = count * N_LINE_H;
    if (!force && y + h > bottom) return 0;

    tft.setTextColor(themeAccent, themeBg);
    tft.drawString(prefix, NX_LEFT, y);
    tft.setTextColor(themeFg, themeBg);
    for (int i = 0; i < count; i++)
        tft.drawString(lines[i], NX_LEFT + (i == 0 ? indent * cw : 0), y + i * N_LINE_H);
    return h + N_ITEM_GAP;
}

void newsDrawPage(TFT_eSPI &tft, time_t utcNow, int tOffsetHours, bool fullRedraw)
{
    static uint8_t showStart = 0;     // first slot on screen
    static uint8_t nextStart = 0;     // first slot of the next screenful
    static unsigned long lastRotateMs = 0;
    static long lastSecond = -1;

    unsigned long nowMs = millis();
    bool listDirty = fullRedraw || g_itemsChanged;

    if (fullRedraw)
    {
        tft.fillScreen(themeBg);
        tft.setTextDatum(TL_DATUM);
        tft.setFreeFont(&Orbitron_Medium8pt7b);
        tft.setTextColor(themeAccent, themeBg);
        tft.drawString("NEWS", 6, 2);
        tft.drawFastHLine(4, NY_RULE1, 312, themeDim);
        tft.drawFastHLine(4, NY_RULE2, 312, themeDim);
        lastRotateMs = nowMs;
        lastSecond = -1;
    }

    Slot order[NEWS_MAX_FEEDS * NEWS_ITEMS_PER_FEED];
    uint8_t total = buildOrder(order);

    if (!listDirty && nowMs - lastRotateMs >= ROTATE_MS)
    {
        lastRotateMs = nowMs;
        // Nothing to turn to when every headline already fits on one screen.
        if (!(showStart == 0 && nextStart == 0))
        {
            showStart = nextStart;
            listDirty = true;
        }
    }
    if (showStart >= total) showStart = 0;

    if (listDirty)
    {
        g_itemsChanged = false;
        tft.fillRect(0, NY_BODY - 2, 320, NY_RULE2 - NY_BODY, themeBg);
        tft.setTextDatum(TL_DATUM);

        if (total == 0)
        {
            tft.setFreeFont(&UbuntuMono_Regular8pt7b);
            tft.setTextColor(themeDim2, themeBg);
            tft.drawString("No headlines yet.", NX_LEFT, NY_BODY + 10);
            tft.drawString("Feeds are set at:", NX_LEFT, NY_BODY + 40);
            tft.setTextColor(themeAccent, themeBg);
            tft.drawString("http://hamclock.local/news.html", NX_LEFT, NY_BODY + 60);
            tft.fillRect(0, NY_FOOT, 320, 240 - NY_FOOT, themeBg);
            nextStart = 0;
        }
        else
        {
            int y = NY_BODY;
            uint8_t i = showStart;
            while (i < total)
            {
                const NewsFeed &f = g_feeds[order[i].feed];
                int h = drawHeadline(tft, y, NY_RULE2 - 4, feedLabel(f),
                                     f.title[order[i].item], i == showStart);
                if (h == 0) break;
                y += h;
                i++;
            }
            nextStart = (i >= total) ? 0 : i;

            // Footer: where in the list this screen is, or what went wrong.
            char foot[48], padded[48];
            const NewsFeed *bad = nullptr;
            for (uint8_t f = 0; f < NEWS_MAX_FEEDS; f++)
                if (g_feeds[f].url[0] && g_feeds[f].tried && !g_feeds[f].ok)
                {
                    bad = &g_feeds[f];
                    break;
                }
            if (bad)
                snprintf(foot, sizeof(foot), "%u-%u/%u  %.10s: %.16s", showStart + 1, i, total,
                         feedLabel(*bad), bad->status);
            else
                snprintf(foot, sizeof(foot), "%u-%u of %u headlines", showStart + 1, i, total);
            snprintf(padded, sizeof(padded), "%-40.40s", foot);
            tft.setFreeFont(&JetBrainsMono_Light7pt7b);
            tft.setTextColor(bad ? themeWarn : themeDim2, themeBg);
            tft.drawString(padded, NX_LEFT, NY_FOOT);
        }
    }

    // Local time in the corner, like the weather page.
    long second = (long)utcNow;
    if (second == lastSecond) return;
    lastSecond = second;

    time_t t = (time_t)(utcNow + (long)tOffsetHours * 3600L);
    struct tm tm_;
    gmtime_r(&t, &tm_);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d LOC", tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim2, themeBg);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(clock, 314, 4);
    tft.setTextDatum(TL_DATUM);
}
