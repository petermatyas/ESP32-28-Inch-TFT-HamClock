#include "dxcluster.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <UbuntuMono_Regular8pt7b.h>
#include <JetBrainsMono_Light7pt7b.h>
#include <HB9IIUOrbitronMed8pt.h>

// =============================================================================
// 1. Configuration and state
// =============================================================================

// The global colour theme (wip.cpp) - background and the shared chrome roles
// every page draws with, so this page's look follows whatever style is chosen
// on the General web page instead of hardcoding TFT_BLACK/TFT_WHITE etc.
extern uint16_t themeBg, themeFg, themeDim, themeDim2, themeAccent, themeWarn;

#define DX_CONFIG_FILE "/dxcluster.json"

// A cluster login is an identification on a shared amateur network, so there is
// no default callsign and nothing connects until the operator enters their own
// at /dx.html.  The default node is a public DXSpider that states everyone is
// welcome; any other can be entered on the same page.
static char     g_call[12] = {0};
static char     g_host[48] = "hrd.wa9pie.net";
static uint16_t g_port     = 8000;

// One bit per band and per mode group.  All on to begin with: a filter that
// hides everything on first use looks like a broken page.
static uint16_t g_bandMask = 0x0FFF;   // DX_BAND_COUNT bits
static uint8_t  g_modeMask = 0x0F;     // DX_MODE_COUNT bits
static bool     g_euOnly   = false;    // show only spots of European DX

struct DxSpot
{
    double  freqKHz;
    char    call[14];
    char    spotter[12];
    char    hhmm[6];      // as the node reported it, "1432"
    uint8_t band;         // index into BANDS, or DX_BAND_NONE
    uint8_t mode;
};

#define DX_BAND_NONE 0xFF

static DxSpot  g_spots[DX_MAX_SPOTS];
static uint8_t g_spotCount = 0;    // valid entries, up to DX_MAX_SPOTS
static uint8_t g_spotHead  = 0;    // where the next spot is written
static uint32_t g_spotsSeen = 0;   // total ever accepted, for the status line

static SemaphoreHandle_t g_mutex = nullptr;
static TaskHandle_t      g_task  = nullptr;

// Short sentence for the footer and the web page.
static char g_status[48] = "starting";

static bool g_filterOpen = false;
static bool g_panelDirty = false;          // a chip was toggled, repaint it
static volatile bool g_reconnect = false;  // settings changed - drop the link

static void startTask();

static void setStatus(const char *s)
{
    strlcpy(g_status, s, sizeof(g_status));
}

// =============================================================================
// 2. Bands and modes
// =============================================================================

struct BandDef
{
    const char *name;
    double lo, hi;      // kHz
};

// IARU region 1 edges, which are the widest of the three for most bands, so a
// spot from any region still lands in the right row.
static const BandDef BANDS[DX_BAND_COUNT] = {
    {"160",   1800.0,   2000.0}, {"80",    3500.0,   4000.0},
    {"60",    5250.0,   5450.0}, {"40",    7000.0,   7300.0},
    {"30",   10100.0,  10150.0}, {"20",   14000.0,  14350.0},
    {"17",   18068.0,  18168.0}, {"15",   21000.0,  21450.0},
    {"12",   24890.0,  24990.0}, {"10",   28000.0,  29700.0},
    {"6",    50000.0,  54000.0}, {"2",   144000.0, 148000.0},
};

enum { DX_MODE_CW = 0, DX_MODE_PHONE, DX_MODE_DIGI, DX_MODE_OTHER };
static const char *MODE_NAMES[DX_MODE_COUNT] = {"CW", "SSB", "DIGI", "OTHER"};

static uint8_t bandFromFreq(double kHz)
{
    for (uint8_t i = 0; i < DX_BAND_COUNT; i++)
        if (kHz >= BANDS[i].lo && kHz <= BANDS[i].hi) return i;
    return DX_BAND_NONE;
}

// Case-insensitive whole-word-ish search: the comment is free text, so "CW" must
// not match inside "NEWCOMER".
static const char *findNoCase(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++)
        if (strncasecmp(p, needle, n) == 0) return p;
    return nullptr;
}

static bool mentions(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++)
    {
        if (strncasecmp(p, needle, n) != 0) continue;
        bool leftOk  = (p == haystack) || !isalnum((unsigned char)p[-1]);
        bool rightOk = !isalnum((unsigned char)p[n]);
        if (leftOk && rightOk) return true;
    }
    return false;
}

// The comment is where a spotter usually says the mode.  When it does not, the
// frequency is the only clue left, and the band plan gives a fair guess - it is
// a guess, though, which is why "OTHER" exists as a place for it to be wrong in.
static uint8_t modeFromSpot(double kHz, const char *comment)
{
    static const char *digi[] = {"FT8", "FT4", "RTTY", "PSK", "PSK31", "JT65",
                                 "JT9", "JS8", "MSK144", "OLIVIA", "SSTV",
                                 "DIGI", "DATA", "Q65"};
    for (const char *d : digi)
        if (mentions(comment, d)) return DX_MODE_DIGI;

    if (mentions(comment, "CW")) return DX_MODE_CW;

    static const char *phone[] = {"SSB", "USB", "LSB", "PHONE", "FM", "AM"};
    for (const char *p : phone)
        if (mentions(comment, p)) return DX_MODE_PHONE;

    // The usual digital watering holes.
    static const double digiSpots[] = {1840.0, 3573.0, 7074.0, 10136.0, 14074.0,
                                       18100.0, 21074.0, 24915.0, 28074.0, 50313.0};
    for (double f : digiSpots)
        if (fabs(kHz - f) < 3.0) return DX_MODE_DIGI;

    // CW sits at the bottom of every band that has a CW segment, with the
    // narrow-band digital modes immediately above it.
    static const struct { double lo, hi; } cwSeg[] = {
        {1800.0, 1838.0}, {3500.0, 3570.0}, {7000.0, 7040.0}, {10100.0, 10130.0},
        {14000.0, 14070.0}, {18068.0, 18095.0}, {21000.0, 21070.0},
        {24890.0, 24915.0}, {28000.0, 28070.0}, {50000.0, 50100.0},
        {144000.0, 144150.0},
    };
    for (auto &c : cwSeg)
        if (kHz >= c.lo && kHz <= c.hi) return DX_MODE_CW;

    static const struct { double lo, hi; } digiSeg[] = {
        {1838.0, 1843.0}, {3570.0, 3600.0}, {7040.0, 7060.0}, {10130.0, 10150.0},
        {14070.0, 14099.0}, {18095.0, 18109.0}, {21070.0, 21149.0},
        {24915.0, 24929.0}, {28070.0, 28190.0}, {50100.0, 50400.0},
    };
    for (auto &d : digiSeg)
        if (kHz >= d.lo && kHz <= d.hi) return DX_MODE_DIGI;

    // Anything left inside a band is above the CW and data segments, which the
    // band plan gives to phone.  Outside every band there is nothing to go on,
    // and OTHER is where that honestly belongs.
    return (bandFromFreq(kHz) == DX_BAND_NONE) ? DX_MODE_OTHER : DX_MODE_PHONE;
}

// =============================================================================
// 2b. Continent (Europe) classification
//
// Amateur callsign prefixes are allocated in blocks per DXCC entity, not per
// continent, so this is a lookup table rather than a formula.  It only needs
// to answer "is this DXCC entity in Europe", not classify every prefix on
// Earth by continent, since that is all the EU-only filter needs.
//
// A few entries are geographically outside Europe but share a prefix block
// with one that is (Spain's Canary Islands EA8 and Ceuta & Melilla EA9 are
// Africa; mainland/Balearic Spain EA/EA6 is Europe) - the longer, more
// specific prefix is listed so it wins over the shorter block it carves out
// of.  Russia is handled separately below: European and Asiatic Russia are
// two different DXCC entities sharing one prefix block, split by the call
// area digit rather than by letters.
//
// Known gap: Turkey's TA/TC block is not split by call area here, so all of
// Turkey reads as outside Europe even though its European provinces issue
// callsigns from the same block.
// =============================================================================
struct PrefixEntry { const char *prefix; bool europe; };

static const PrefixEntry EU_PREFIXES[] = {
    {"EA6", true}, {"EA8", false}, {"EA9", false}, {"EA", true},   // Spain / Canaries / Ceuta&Melilla
    {"CT3", true}, {"CT", true}, {"CU", true},                     // Portugal / Madeira / Azores
    {"SV5", true}, {"SV9", true}, {"SV", true}, {"SW", true}, {"SX", true}, {"SY", true}, {"SZ", true}, {"J4", true}, // Greece + islands
    {"IS0", true}, {"IT9", true}, {"I", true},                     // Italy + Sardinia/Sicily
    {"GD", true}, {"GI", true}, {"GJ", true}, {"GM", true}, {"GU", true}, {"GW", true}, {"G", true}, // UK + Crown dependencies
    {"EI", true}, {"EJ", true},                                    // Ireland
    {"F", true},                                                   // France (metropolitan)
    {"ON", true}, {"OO", true}, {"OP", true}, {"OQ", true}, {"OR", true}, {"OS", true}, {"OT", true}, // Belgium
    {"PA", true}, {"PB", true}, {"PC", true}, {"PD", true}, {"PE", true}, {"PF", true}, {"PG", true}, {"PH", true}, {"PI", true}, // Netherlands
    {"DA", true}, {"DB", true}, {"DC", true}, {"DD", true}, {"DF", true}, {"DG", true}, {"DH", true},
    {"DJ", true}, {"DK", true}, {"DL", true}, {"DM", true}, {"DO", true}, {"DQ", true}, {"DR", true}, // Germany
    {"HB0", true}, {"HB", true},                                   // Switzerland + Liechtenstein
    {"OE", true},                                                  // Austria
    {"9H", true},                                                  // Malta
    {"LX", true},                                                  // Luxembourg
    {"OZ", true}, {"5P", true}, {"5Q", true},                      // Denmark
    {"OY", true},                                                  // Faroe Islands
    {"TF", true},                                                  // Iceland
    {"LA", true}, {"LB", true}, {"LJ", true}, {"LN", true},        // Norway
    {"JW", true}, {"JX", true},                                    // Svalbard / Jan Mayen
    {"SM", true}, {"SA", true}, {"SB", true}, {"SC", true}, {"SD", true}, {"SE", true},
    {"SF", true}, {"SG", true}, {"SH", true}, {"SI", true}, {"SJ", true}, {"SK", true}, {"SL", true}, // Sweden
    {"OH", true}, {"OF", true}, {"OG", true}, {"OI", true}, {"OJ", true}, // Finland + Market Reef
    {"ES", true},                                                  // Estonia
    {"YL", true},                                                  // Latvia
    {"LY", true},                                                  // Lithuania
    {"SN", true}, {"SO", true}, {"SP", true}, {"SQ", true}, {"SR", true}, {"3Z", true}, {"HF", true}, // Poland
    {"OK", true}, {"OL", true},                                    // Czech Republic
    {"OM", true},                                                  // Slovakia
    {"HA", true}, {"HG", true},                                    // Hungary
    {"S5", true},                                                  // Slovenia
    {"9A", true},                                                  // Croatia
    {"E7", true},                                                  // Bosnia and Herzegovina
    {"YU", true}, {"YT", true}, {"YZ", true},                      // Serbia
    {"4O", true},                                                  // Montenegro
    {"Z3", true}, {"Z6", true},                                    // North Macedonia / Kosovo
    {"5B", true}, {"C4", true}, {"H2", true}, {"P3", true},        // Cyprus
    {"YO", true}, {"YP", true}, {"YQ", true}, {"YR", true},        // Romania
    {"LZ", true},                                                  // Bulgaria
    {"ER", true},                                                  // Moldova
    {"UR", true}, {"UT", true}, {"UU", true}, {"UV", true}, {"UW", true}, {"UX", true}, {"UY", true},
    {"UZ", true}, {"EM", true}, {"EN", true}, {"EO", true},        // Ukraine
    {"EW", true}, {"EU", true}, {"EV", true},                      // Belarus
    {"C3", true},                                                  // Andorra
    {"3A", true},                                                  // Monaco
    {"T7", true},                                                  // San Marino
    {"HV", true},                                                  // Vatican
    {"ZB", true},                                                  // Gibraltar
};

static bool isEuropeanPrefix(const char *call)
{
    if (!call || !call[0]) return false;

    // Russia/Kaliningrad: European call areas are 1, 2, 3, 4 and 6; the rest
    // (0, 5, 7, 8, 9) are Asiatic Russia.
    bool maybeRussian = (call[0] == 'R') ||
                        (call[0] == 'U' && (call[1] == 'A' || call[1] == 'B' ||
                                             call[1] == 'C' || call[1] == 'F' || call[1] == 'I'));
    if (maybeRussian)
    {
        const char *d = call;
        while (*d && !isdigit((unsigned char)*d)) d++;
        if (isdigit((unsigned char)*d))
            return *d == '1' || *d == '2' || *d == '3' || *d == '4' || *d == '6';
    }

    for (int len = 3; len >= 1; len--)
        for (const PrefixEntry &e : EU_PREFIXES)
            if ((int)strlen(e.prefix) == len && strncmp(call, e.prefix, len) == 0)
                return e.europe;

    return false;
}

// =============================================================================
// 3. Persistence
// =============================================================================

static void saveConfig()
{
    JsonDocument doc;
    doc["call"]     = g_call;
    doc["host"]     = g_host;
    doc["port"]     = g_port;
    doc["bandMask"] = g_bandMask;
    doc["modeMask"] = g_modeMask;
    doc["euOnly"]   = g_euOnly;

    fs::File f = SPIFFS.open(DX_CONFIG_FILE, "w");
    if (!f)
    {
        Serial.println("DX: could not write " DX_CONFIG_FILE);
        return;
    }
    serializeJsonPretty(doc, f);
    f.close();
}

static void loadConfig()
{
    fs::File f = SPIFFS.open(DX_CONFIG_FILE, "r");
    if (!f)
    {
        Serial.println("DX: no " DX_CONFIG_FILE " yet - set a callsign at /dx.html");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err == DeserializationError::EmptyInput)
    {
        Serial.println("DX: " DX_CONFIG_FILE " is empty - set a callsign at /dx.html");
        return;
    }
    if (err)
    {
        Serial.printf("DX: %s is unreadable (%s) - using defaults\n",
                      DX_CONFIG_FILE, err.c_str());
        return;
    }

    strlcpy(g_call, doc["call"] | g_call, sizeof(g_call));
    strlcpy(g_host, doc["host"] | g_host, sizeof(g_host));
    g_port     = doc["port"]     | g_port;
    g_bandMask = doc["bandMask"] | g_bandMask;
    g_modeMask = doc["modeMask"] | g_modeMask;
    g_euOnly   = doc["euOnly"]   | g_euOnly;

    if (g_port == 0) g_port = 8000;

    Serial.printf("DX: %s@%s:%u, bands 0x%03X modes 0x%X, euOnly %s\n",
                  g_call[0] ? g_call : "(no callsign)", g_host, g_port,
                  g_bandMask, g_modeMask, g_euOnly ? "true" : "false");
}

// =============================================================================
// 4. Parsing
// =============================================================================

// A DXSpider or AR-Cluster spot line looks like
//
//   DX de EA5ABC:    14074.0  JA1XYZ       FT8 -12 dB          1432Z IM98
//
// The columns are conventional rather than guaranteed, so the fields are picked
// off by scanning instead of by offset: the spotter runs to the colon, then the
// frequency, then the callsign, and the "1432Z" nearest the end is the time.
// Whatever lies between the callsign and that time is the comment.
static bool parseSpot(const char *line, DxSpot &out)
{
    if (strncasecmp(line, "DX de ", 6) != 0) return false;
    const char *p = line + 6;

    const char *colon = strchr(p, ':');
    if (!colon) return false;

    size_t n = colon - p;
    while (n && p[n - 1] == ' ') n--;
    if (n == 0 || n >= sizeof(out.spotter)) return false;
    memcpy(out.spotter, p, n);
    out.spotter[n] = 0;

    p = colon + 1;
    while (*p == ' ') p++;

    char *end = nullptr;
    double kHz = strtod(p, &end);
    if (end == p || kHz <= 0.0 || kHz > 2000000.0) return false;
    out.freqKHz = kHz;

    p = end;
    while (*p == ' ') p++;

    const char *cs = p;
    while (*p && *p != ' ') p++;
    n = p - cs;
    if (n == 0 || n >= sizeof(out.call)) return false;
    memcpy(out.call, cs, n);
    out.call[n] = 0;

    while (*p == ' ') p++;
    const char *rest = p;

    // Last "HHMMZ" standing on its own is the spot time.
    const char *timeTok = nullptr;
    for (const char *q = rest; *q && q[1] && q[2] && q[3] && q[4]; q++)
    {
        if ((q == rest || q[-1] == ' ') &&
            isdigit((unsigned char)q[0]) && isdigit((unsigned char)q[1]) &&
            isdigit((unsigned char)q[2]) && isdigit((unsigned char)q[3]) &&
            (q[4] == 'Z' || q[4] == 'z'))
            timeTok = q;
    }

    if (timeTok)
        snprintf(out.hhmm, sizeof(out.hhmm), "%.4s", timeTok);
    else
        strlcpy(out.hhmm, "----", sizeof(out.hhmm));

    char comment[64];
    size_t clen = timeTok ? (size_t)(timeTok - rest) : strlen(rest);
    if (clen >= sizeof(comment)) clen = sizeof(comment) - 1;
    memcpy(comment, rest, clen);
    comment[clen] = 0;
    while (clen && comment[clen - 1] == ' ') comment[--clen] = 0;

    out.band = bandFromFreq(kHz);
    out.mode = modeFromSpot(kHz, comment);
    return true;
}

static void pushSpot(const DxSpot &s)
{
    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_spots[g_spotHead] = s;
    g_spotHead = (g_spotHead + 1) % DX_MAX_SPOTS;
    if (g_spotCount < DX_MAX_SPOTS) g_spotCount++;
    g_spotsSeen++;
    xSemaphoreGive(g_mutex);
}

// =============================================================================
// 5. The cluster task
// =============================================================================

static void dxTask(void *)
{
    WiFiClient client;
    char line[200];
    size_t lineLen = 0;
    bool loggedIn = false;
    uint16_t logLines = 0;          // raw lines echoed to serial per session
    unsigned long nextAttempt = 0;
    unsigned long backoffMs = 10000;

    for (;;)
    {
        if (!g_call[0])
        {
            if (client.connected()) client.stop();
            setStatus("no callsign - set one at /dx.html");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (WiFi.status() != WL_CONNECTED)
        {
            if (client.connected()) client.stop();
            setStatus("waiting for wifi");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (g_reconnect)
        {
            g_reconnect = false;
            if (client.connected()) client.stop();
            nextAttempt = 0;
            backoffMs = 10000;
            setStatus("settings changed - reconnecting");
        }

        if (!client.connected())
        {
            unsigned long now = millis();
            if ((long)(now - nextAttempt) < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }

            char msg[64];
            snprintf(msg, sizeof(msg), "connecting to %.24s", g_host);
            setStatus(msg);
            Serial.printf("DX: connecting to %s:%u\n", g_host, g_port);

            lineLen = 0;
            loggedIn = false;
            logLines = 0;

            // Blocking, but this task is the only thing waiting on it.
            if (client.connect(g_host, g_port, 8000))
            {
                setStatus("connected - logging in");
                backoffMs = 10000;
            }
            else
            {
                snprintf(msg, sizeof(msg), "cannot reach %.20s - retrying", g_host);
                setStatus(msg);
                nextAttempt = millis() + backoffMs;
                // Back off politely rather than hammering a node that is down.
                if (backoffMs < 120000) backoffMs *= 2;
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        int avail = client.available();
        if (avail <= 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        while (client.available())
        {
            int c = client.read();
            if (c < 0) break;

            if (c == '\n' || lineLen >= sizeof(line) - 1)
            {
                line[lineLen] = 0;

                // Strip the carriage return and any trailing space.
                while (lineLen && (line[lineLen - 1] == '\r' || line[lineLen - 1] == ' '))
                    line[--lineLen] = 0;

                if (lineLen)
                {
                    if (logLines < 25)
                    {
                        Serial.printf("DX< %s\n", line);
                        logLines++;
                    }

                    DxSpot s;
                    if (parseSpot(line, s))
                    {
                        pushSpot(s);
                        char msg[48];
                        snprintf(msg, sizeof(msg), "online - %lu spots",
                                 (unsigned long)g_spotsSeen);
                        setStatus(msg);
                    }
                }
                lineLen = 0;
                continue;
            }

            if (c != '\r') line[lineLen++] = (char)c;
            line[lineLen] = 0;

            // The login prompt arrives without a newline, so it has to be
            // spotted in the partial line rather than waiting for one.
            if (!loggedIn && lineLen >= 6 && findNoCase(line, "login:"))
            {
                Serial.printf("DX: sending callsign %s\n", g_call);
                client.printf("%s\r\n", g_call);
                loggedIn = true;
                lineLen = 0;
                line[0] = 0;
                setStatus("logged in - waiting for spots");
            }
        }

        if (!client.connected())
        {
            setStatus("node closed the link - reconnecting");
            client.stop();
            nextAttempt = millis() + 5000;
        }
    }
}

// =============================================================================
// 6. Web routes
// =============================================================================

static void handleDxPage(WebServer &server)
{
    fs::File f = SPIFFS.open("/dx.html", "r");
    if (!f)
    {
        server.send(404, "text/plain", "dx.html is missing - upload the SPIFFS image");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

static void buildConfigJson(String &out)
{
    JsonDocument doc;
    doc["call"]     = g_call;
    doc["host"]     = g_host;
    doc["port"]     = g_port;
    doc["bandMask"] = g_bandMask;
    doc["modeMask"] = g_modeMask;
    doc["euOnly"]   = g_euOnly;
    doc["status"]   = g_status;
    doc["seen"]     = g_spotsSeen;

    JsonArray b = doc["bands"].to<JsonArray>();
    for (uint8_t i = 0; i < DX_BAND_COUNT; i++) b.add(BANDS[i].name);
    JsonArray m = doc["modes"].to<JsonArray>();
    for (uint8_t i = 0; i < DX_MODE_COUNT; i++) m.add(MODE_NAMES[i]);

    serializeJson(doc, out);
}

static void handleSaveConfig(WebServer &server)
{
    JsonDocument doc;
    if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain")))
    {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    if (doc["call"].is<const char *>())
    {
        char call[sizeof(g_call)];
        strlcpy(call, doc["call"] | "", sizeof(call));
        // Callsigns are case-insensitive on the air and upper case on a cluster.
        for (char *p = call; *p; p++) *p = toupper((unsigned char)*p);
        strlcpy(g_call, call, sizeof(g_call));
    }
    if (doc["host"].is<const char *>())
        strlcpy(g_host, doc["host"] | g_host, sizeof(g_host));
    if (doc["port"].is<uint16_t>() || doc["port"].is<int>())
    {
        int p = doc["port"] | (int)g_port;
        if (p > 0 && p < 65536) g_port = (uint16_t)p;
    }
    if (doc["bandMask"].is<int>()) g_bandMask = (uint16_t)(doc["bandMask"].as<int>() & 0x0FFF);
    if (doc["modeMask"].is<int>()) g_modeMask = (uint8_t)(doc["modeMask"].as<int>() & 0x0F);
    if (doc["euOnly"].is<bool>()) g_euOnly = doc["euOnly"].as<bool>();

    saveConfig();
    Serial.printf("DX: config saved - %s@%s:%u\n",
                  g_call[0] ? g_call : "(no callsign)", g_host, g_port);

    // Drop the link so a changed callsign or node takes effect now rather than
    // whenever the current session happens to end.  The first callsign to
    // arrive is also what brings the task into being.
    if (g_call[0]) startTask();
    g_reconnect = true;
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void buildSpotsJson(String &out)
{
    JsonDocument doc;
    doc["status"] = g_status;
    JsonArray arr = doc["spots"].to<JsonArray>();

    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < g_spotCount; i++)
    {
        // Newest first.
        uint8_t idx = (uint8_t)((g_spotHead + DX_MAX_SPOTS - 1 - i) % DX_MAX_SPOTS);
        const DxSpot &s = g_spots[idx];
        JsonObject o = arr.add<JsonObject>();
        o["freq"]    = s.freqKHz;
        o["call"]    = s.call;
        o["spotter"] = s.spotter;
        o["time"]    = s.hhmm;
        o["band"]    = (s.band == DX_BAND_NONE) ? "-" : BANDS[s.band].name;
        o["mode"]    = MODE_NAMES[s.mode];
    }
    if (g_mutex) xSemaphoreGive(g_mutex);

    serializeJson(doc, out);
}

void dxClusterRegisterRoutes(WebServer &server)
{
    server.on("/dx.html", HTTP_GET, [&server]() { handleDxPage(server); });
    server.on("/dxcfg", HTTP_GET, [&server]() {
        String out;
        buildConfigJson(out);
        server.send(200, "application/json", out);
    });
    server.on("/dxcfg", HTTP_POST, [&server]() { handleSaveConfig(server); });
    server.on("/dxspots", HTTP_GET, [&server]() {
        String out;
        buildSpotsJson(out);
        server.send(200, "application/json", out);
    });
}

// =============================================================================
// 7. Rendering
// =============================================================================

// Spot list.  y is the top of the text, which is what TL_DATUM uses.
static const int DXY_RULE1  = 19;
static const int DXY_COLHDR = 26;
static const int DXY_RULE2  = 40;
static const int DXY_ROW0   = 48;
static const int DXY_ROWH   = 18;
static const int DX_ROWS    = 8;
static const int DXY_RULE3  = 192;
static const int DXY_FOOT   = 200;

static const int DXX_TIME = 6;
static const int DXX_FREQ = 46;
static const int DXX_CALL = 118;
static const int DXX_MODE = 220;
static const int DXX_BY   = 264;

// The filter button, and the panel it opens.
static const int BTN_X = 238, BTN_Y = 196, BTN_W = 78, BTN_H = 28;
static const int DONE_X = 246, DONE_Y = 2, DONE_W = 68, DONE_H = 26;

static const int CHIP_H = 26;
static const int BAND_X0 = 6, BAND_W = 48, BAND_GAP = 4;
static const int BAND_ROW0 = 54, BAND_ROW1 = 84;
static const int MODE_X0 = 7, MODE_W = 72, MODE_GAP = 6, MODE_Y = 134;
static const int CONT_X = 6, CONT_Y = 194, CONT_W = 90;

static void bandChipRect(uint8_t i, int &x, int &y)
{
    x = BAND_X0 + (i % 6) * (BAND_W + BAND_GAP);
    y = (i < 6) ? BAND_ROW0 : BAND_ROW1;
}

static void modeChipRect(uint8_t i, int &x, int &y)
{
    x = MODE_X0 + i * (MODE_W + MODE_GAP);
    y = MODE_Y;
}

static void drawChip(TFT_eSPI &tft, int x, int y, int w, const char *label, bool on)
{
    // Lit chips are filled, dark ones outlined: legible at a glance from across
    // a bench, where a colour difference alone would not be.
    if (on)
    {
        tft.fillRoundRect(x, y, w, CHIP_H, 4, TFT_DARKGREEN);
        tft.drawRoundRect(x, y, w, CHIP_H, 4, TFT_GREEN);
        tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    }
    else
    {
        tft.fillRoundRect(x, y, w, CHIP_H, 4, themeBg);
        tft.drawRoundRect(x, y, w, CHIP_H, 4, themeDim);
        tft.setTextColor(themeDim, themeBg);
    }

    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, x + w / 2, y + CHIP_H / 2);
    tft.setTextDatum(TL_DATUM);
}

static void drawButton(TFT_eSPI &tft, int x, int y, int w, int h,
                       const char *label, uint16_t colour)
{
    tft.fillRoundRect(x, y, w, h, 4, themeBg);
    tft.drawRoundRect(x, y, w, h, 4, colour);
    tft.setTextColor(colour, themeBg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, x + w / 2, y + h / 2);
    tft.setTextDatum(TL_DATUM);
}

static void drawFilterPanel(TFT_eSPI &tft)
{
    tft.fillScreen(themeBg);
    tft.setTextDatum(TL_DATUM);

    tft.setFreeFont(&Orbitron_Medium8pt7b);
    tft.setTextColor(themeAccent, themeBg);
    tft.drawString("FILTER", 6, 2);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    drawButton(tft, DONE_X, DONE_Y, DONE_W, DONE_H, "DONE", TFT_GREEN);

    tft.drawFastHLine(4, 32, 312, themeDim);

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim2, themeBg);
    tft.drawString("BANDS", 6, 38);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    for (uint8_t i = 0; i < DX_BAND_COUNT; i++)
    {
        int x, y;
        bandChipRect(i, x, y);
        drawChip(tft, x, y, BAND_W, BANDS[i].name, g_bandMask & (1u << i));
    }

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim2, themeBg);
    tft.drawString("MODES", 6, 118);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    for (uint8_t i = 0; i < DX_MODE_COUNT; i++)
    {
        int x, y;
        modeChipRect(i, x, y);
        drawChip(tft, x, y, MODE_W, MODE_NAMES[i], g_modeMask & (1u << i));
    }

    tft.drawFastHLine(4, 172, 312, themeDim);

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim2, themeBg);
    tft.drawString("CONTINENT", 6, 178);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    drawChip(tft, CONT_X, CONT_Y, CONT_W, "EU ONLY", g_euOnly);

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim, themeBg);
    tft.drawString("tap a chip to toggle it", 6, 224);
}

static bool spotPasses(const DxSpot &s)
{
    if (s.band == DX_BAND_NONE) return false;
    if (!(g_bandMask & (1u << s.band))) return false;
    if (!(g_modeMask & (1u << s.mode))) return false;
    if (g_euOnly && !isEuropeanPrefix(s.call)) return false;
    return true;
}

void dxClusterDrawPage(TFT_eSPI &tft, time_t utcNow, bool fullRedraw)
{
    static long lastSecond = -1;
    static bool lastFilterOpen = false;

    if (g_filterOpen)
    {
        if (fullRedraw || !lastFilterOpen || g_panelDirty)
        {
            drawFilterPanel(tft);
            g_panelDirty = false;
        }
        lastFilterOpen = true;
        return;
    }

    bool repaint = fullRedraw || lastFilterOpen;
    lastFilterOpen = false;

    if (repaint)
    {
        tft.fillScreen(themeBg);
        tft.setTextDatum(TL_DATUM);

        tft.setFreeFont(&Orbitron_Medium8pt7b);
        tft.setTextColor(themeAccent, themeBg);
        tft.drawString("DX CLUSTER", 6, 2);
        tft.drawFastHLine(4, DXY_RULE1, 312, themeDim);

        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(themeDim, themeBg);
        tft.drawString("TIME", DXX_TIME, DXY_COLHDR);
        tft.drawString("FREQ", DXX_FREQ, DXY_COLHDR);
        tft.drawString("DX CALL", DXX_CALL, DXY_COLHDR);
        tft.drawString("MODE", DXX_MODE, DXY_COLHDR);
        tft.drawString("BY", DXX_BY, DXY_COLHDR);
        tft.drawFastHLine(4, DXY_RULE2, 312, themeDim);
        tft.drawFastHLine(4, DXY_RULE3, 312, themeDim);

        tft.setFreeFont(&UbuntuMono_Regular8pt7b);
        drawButton(tft, BTN_X, BTN_Y, BTN_W, BTN_H, "FILTER", themeAccent);
    }

    // The rows only move when a spot arrives or the filter changes, so they are
    // left alone otherwise - redrawing eight opaque lines a second would flicker
    // for nothing.  The footer below is a single line and follows the clock.
    static uint32_t lastSeen = 0xFFFFFFFF;
    static uint16_t lastBands = 0xFFFF;
    static uint8_t  lastModes = 0xFF;

    long second = (long)utcNow;
    if (!repaint && second == lastSecond) return;
    lastSecond = second;

    bool rowsChanged = repaint || lastSeen != g_spotsSeen ||
                       lastBands != g_bandMask || lastModes != g_modeMask;
    lastSeen = g_spotsSeen;
    lastBands = g_bandMask;
    lastModes = g_modeMask;

    // Copy out what is shown, so the socket task is never blocked on the panel.
    DxSpot shown[DX_ROWS];
    uint8_t shownCount = 0;
    uint8_t total = 0;

    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < g_spotCount && shownCount < DX_ROWS; i++)
    {
        uint8_t idx = (uint8_t)((g_spotHead + DX_MAX_SPOTS - 1 - i) % DX_MAX_SPOTS);
        if (!spotPasses(g_spots[idx])) continue;
        shown[shownCount++] = g_spots[idx];
    }
    for (uint8_t i = 0; i < g_spotCount; i++)
    {
        uint8_t idx = (uint8_t)((g_spotHead + DX_MAX_SPOTS - 1 - i) % DX_MAX_SPOTS);
        if (spotPasses(g_spots[idx])) total++;
    }
    char status[sizeof(g_status)];
    strlcpy(status, g_status, sizeof(status));
    if (g_mutex) xSemaphoreGive(g_mutex);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextDatum(TL_DATUM);

    for (uint8_t r = 0; rowsChanged && r < DX_ROWS; r++)
    {
        int y = DXY_ROW0 + r * DXY_ROWH;
        char buf[24];

        if (r >= shownCount)
        {
            // Blank the row rather than leaving a stale spot behind.
            tft.fillRect(0, y, 320, DXY_ROWH - 2, themeBg);
            continue;
        }

        const DxSpot &s = shown[r];

        tft.setTextColor(themeDim, themeBg);
        snprintf(buf, sizeof(buf), "%-4.4s", s.hhmm);
        tft.drawString(buf, DXX_TIME, y);

        // Right-aligned by padding, so the decimal points line up.
        tft.setTextColor(themeWarn, themeBg);
        snprintf(buf, sizeof(buf), "%8.1f", s.freqKHz);
        tft.drawString(buf, DXX_FREQ, y);

        tft.setTextColor(themeFg, themeBg);
        snprintf(buf, sizeof(buf), "%-12.12s", s.call);
        tft.drawString(buf, DXX_CALL, y);

        tft.setTextColor(TFT_GREEN, themeBg);
        snprintf(buf, sizeof(buf), "%-5.5s", MODE_NAMES[s.mode]);
        tft.drawString(buf, DXX_MODE, y);

        tft.setTextColor(themeDim, themeBg);
        snprintf(buf, sizeof(buf), "%-6.6s", s.spotter);
        tft.drawString(buf, DXX_BY, y);
    }

    // Footer: what the link is doing, and how much the filter is letting past.
    char foot[48], padded[48];
    if (total > 0)
        snprintf(foot, sizeof(foot), "%.28s  %u shown", status, total);
    else
        snprintf(foot, sizeof(foot), "%.38s", status);
    snprintf(padded, sizeof(padded), "%-28.28s", foot);

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim2, themeBg);
    tft.drawString(padded, DXX_TIME, DXY_FOOT);

    // UTC in the corner: the spot times the node sends are in UTC, so this is
    // what they should be read against.  Fixed width, so the opaque background
    // wipes the previous value without a clearing rectangle.
    {
        time_t t = (time_t)utcNow;
        struct tm tm_;
        gmtime_r(&t, &tm_);
        char clock[16];
        snprintf(clock, sizeof(clock), "%02d:%02d:%02d UTC",
                 tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
        tft.setTextColor(themeDim2, themeBg);
        tft.setTextDatum(TR_DATUM);
        tft.drawString(clock, 314, 4);
        tft.setTextDatum(TL_DATUM);
    }
}

// =============================================================================
// 8. Touch
// =============================================================================

static bool inside(int16_t x, int16_t y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

bool dxClusterFilterOpen()
{
    return g_filterOpen;
}

bool dxClusterHandleTouch(int16_t x, int16_t y)
{
    if (!g_filterOpen)
    {
        // A generous margin: the button is small and fingers are not.
        if (inside(x, y, BTN_X - 8, BTN_Y - 8, BTN_W + 16, BTN_H + 16))
        {
            g_filterOpen = true;
            return true;
        }
        return false;   // anything else is a page turn
    }

    // While the panel is up it owns every touch, so a stray tap cannot page out
    // from under it.  The chips are checked first; everything else closes the
    // panel, the DONE button included.  Making the whole background dismiss it
    // means a mistap on a miscalibrated panel can never strand the page.
    for (uint8_t i = 0; i < DX_BAND_COUNT; i++)
    {
        int cx, cy;
        bandChipRect(i, cx, cy);
        if (inside(x, y, cx, cy - 2, BAND_W, CHIP_H + 4))
        {
            g_bandMask ^= (1u << i);
            g_panelDirty = true;
            return true;
        }
    }

    for (uint8_t i = 0; i < DX_MODE_COUNT; i++)
    {
        int cx, cy;
        modeChipRect(i, cx, cy);
        if (inside(x, y, cx, cy - 2, MODE_W, CHIP_H + 4))
        {
            g_modeMask ^= (1u << i);
            g_panelDirty = true;
            return true;
        }
    }

    if (inside(x, y, CONT_X, CONT_Y - 2, CONT_W, CHIP_H + 4))
    {
        g_euOnly = !g_euOnly;
        g_panelDirty = true;
        return true;
    }

    g_filterOpen = false;
    saveConfig();
    Serial.printf("DX: filter saved - bands 0x%03X modes 0x%X, euOnly %s\n",
                  g_bandMask, g_modeMask, g_euOnly ? "true" : "false");
    return true;
}

// =============================================================================
// 9. Parser self test
// =============================================================================

// The line format is the DXSpider / AR-Cluster convention rather than anything
// guaranteed, so the parser is pinned here against the variations that actually
// turn up: a grid on the end, a signal report full of digits, portable and
// prefixed callsigns, six-figure VHF frequencies, and the node chatter that must
// not be mistaken for a spot.  Each session also echoes its first 25 raw lines
// to serial, so the real feed can be checked against this the moment a callsign
// is set.
struct DxParseCase
{
    const char *line;
    bool        want;          // should it parse at all
    const char *call;
    double      kHz;
    uint8_t     band;
    uint8_t     mode;
    const char *hhmm;
};

static void dxClusterSelfTest()
{
    static const DxParseCase cases[] = {
        {"DX de EA5ABC:    14074.0  JA1XYZ       FT8 -12 dB           1432Z IM98",
         true, "JA1XYZ", 14074.0, 5, DX_MODE_DIGI, "1432"},
        {"DX de W3LPL:      7025.0  UA9CDC       CW 599               1401Z",
         true, "UA9CDC", 7025.0, 3, DX_MODE_CW, "1401"},
        {"DX de OH2BH:     50313.0  VK3ABC/P     tnx qso              0803Z",
         true, "VK3ABC/P", 50313.0, 10, DX_MODE_DIGI, "0803"},
        {"DX de DL1ABC:   144300.0  IK2XYZ       SSB                  1210Z",
         true, "IK2XYZ", 144300.0, 11, DX_MODE_PHONE, "1210"},
        {"DX de JA1XYZ:     3573.0  VP2E/W3ABC   FT8                  2359Z",
         true, "VP2E/W3ABC", 3573.0, 1, DX_MODE_DIGI, "2359"},
        // "NEWCOMER" must not read as CW, and 14200 is outside the CW segment.
        {"DX de N1ABC:     14200.0  DL/PA0XYZ    NEWCOMER calling     1500Z",
         true, "DL/PA0XYZ", 14200.0, 5, DX_MODE_PHONE, "1500"},
        // No time on the end: still a spot, just without one.
        {"DX de G3ABC:     21025.0  ZS6XYZ       up 2",
         true, "ZS6XYZ", 21025.0, 7, DX_MODE_CW, "----"},
        // Not spots.
        {"WWV de VE7CC <18Z> : SFI=142, A=12, K=3, No Storms", false, "", 0, 0, 0, ""},
        {"To ALL de OH2BH: anyone on 6m tonight?", false, "", 0, 0, 0, ""},
        {"Hello and welcome to the cluster", false, "", 0, 0, 0, ""},
        {"DX de", false, "", 0, 0, 0, ""},
    };

    int failures = 0;
    for (const DxParseCase &c : cases)
    {
        DxSpot s;
        memset(&s, 0, sizeof(s));
        bool got = parseSpot(c.line, s);

        if (got != c.want)
        {
            Serial.printf("DX selftest FAIL (parsed=%d want=%d): %s\n", got, c.want, c.line);
            failures++;
            continue;
        }
        if (!c.want) continue;

        bool ok = strcmp(s.call, c.call) == 0 &&
                  fabs(s.freqKHz - c.kHz) < 0.05 &&
                  s.band == c.band && s.mode == c.mode &&
                  strcmp(s.hhmm, c.hhmm) == 0;
        if (!ok)
        {
            Serial.printf("DX selftest FAIL: %s\n", c.line);
            Serial.printf("   got  call=%s freq=%.1f band=%u mode=%u time=%s\n",
                          s.call, s.freqKHz, s.band, s.mode, s.hhmm);
            Serial.printf("   want call=%s freq=%.1f band=%u mode=%u time=%s\n",
                          c.call, c.kHz, c.band, c.mode, c.hhmm);
            failures++;
        }
    }

    Serial.printf("DX selftest: %u cases, %d failure(s) - %s\n",
                  (unsigned)(sizeof(cases) / sizeof(cases[0])), failures,
                  failures ? "FAIL" : "PASS");
}

// =============================================================================
// 10. Life cycle
// =============================================================================

// Core 0, low priority: the socket spends nearly all its time waiting, while
// the drawing and the web server run on core 1.  The task is only started once
// there is a callsign to log in with - its four kilobytes of stack are worth
// keeping out of the heap on a clock whose owner does not use the cluster, and
// boot is when the heap is tightest anyway.
static void startTask()
{
    if (g_task) return;
    xTaskCreatePinnedToCore(dxTask, "dxcluster", 4096, nullptr, 1, &g_task, 0);
    Serial.println("DX: cluster task started");
}

void dxClusterBegin()
{
    g_mutex = xSemaphoreCreateMutex();
    dxClusterSelfTest();
    loadConfig();

    setStatus(g_call[0] ? "starting" : "no callsign - set one at /dx.html");
    if (g_call[0]) startTask();
}

void dxClusterReportHealth()
{
    if (!g_task) return;
    Serial.printf("DX health: stack free %u, %u spot(s) held, %s\n",
                  (unsigned)uxTaskGetStackHighWaterMark(g_task),
                  (unsigned)g_spotCount, g_status);
}
