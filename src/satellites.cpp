// -----------------------------------------------------------------------------
// satellites.cpp - satellite pass page (TFT page 7) and its web configurator
//
// Layout of this file:
//   1. configuration + state          5. web routes
//   2. SPIFFS config / TLE cache      6. live look angles
//   3. TLE download (main loop)       7. TFT page rendering
//   4. prediction task                8. public entry points
//
// The font headers define everything as file-scope const, so including them
// here gives this translation unit its own copy (a few kB of flash) rather than
// a duplicate-symbol error.
// -----------------------------------------------------------------------------
#include "satellites.h"

#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <math.h>

#include <sgp4.h>

#include <UbuntuMono_Regular8pt7b.h>
#include <JetBrainsMono_Light7pt7b.h>
#include <JetBrainsMono_Bold11pt7b.h>
#include <HB9IIUOrbitronMed8pt.h>

// =============================================================================
// 1. Configuration and state
// =============================================================================

// The global colour theme (wip.cpp) - background and the shared chrome roles
// every page draws with, so this page's look follows whatever style is chosen
// on the General web page instead of hardcoding TFT_BLACK/TFT_WHITE etc.
extern uint16_t themeBg, themeFg, themeDim, themeDim2, themeAccent, themeWarn;

static const char *SAT_CONFIG_FILE = "/satellites.json";
static const double DEG2RAD_L = 0.017453292519943295;

struct SatSlot {
    uint32_t     norad;
    char         name[26];
    char         tle1[71];
    char         tle2[71];
    uint32_t     fetchedUnix;      // when the TLE was downloaded
    bool         tleLoaded;        // element set lines present
    bool         recValid;         // parsed into a usable SatRec
    int          recError;
    sgp4::SatRec rec;
    uint8_t      failures;
    uint32_t     retryAfterUnix;   // backoff after a failed download
    char         status[40];
    bool         alertEnabled;     // show the cross-page banner before this one rises
};

struct PassRow {
    uint8_t    slot;
    sgp4::Pass p;
};

static SatSlot  g_slots[SAT_MAX_SATS];
static uint8_t  g_slotCount = 0;

static PassRow  g_passes[SAT_MAX_SATS * SAT_PASSES_PER_SAT];
static uint8_t  g_passCount = 0;
static volatile uint32_t g_passVersion = 0;
static uint32_t g_predictedAtUnix = 0;
static bool     g_predictBusy = false;

// configuration (defaults are used until /satellites.json exists)
static double  g_minElevation    = 5.0;
static uint8_t g_horizonDays     = 3;
static uint8_t g_tleRefreshHours = 12;
static bool    g_useLocalTime    = true;
static double  g_altitudeM       = 150.0;
static int     g_alertLeadMin    = 5;   // minutes before AOS the alert window opens

static double  g_latDeg = 0.0, g_lonDeg = 0.0;

static volatile time_t   g_nowUnix      = 0;
static volatile bool     g_needPredict  = false;
static SemaphoreHandle_t g_mutex        = NULL;
static SemaphoreHandle_t g_predictSignal = NULL;
static TaskHandle_t      g_predictTask = NULL;

#define SAT_LOCK()   xSemaphoreTake(g_mutex, portMAX_DELAY)
#define SAT_UNLOCK() xSemaphoreGive(g_mutex)

static void ensureSync()
{
    if (!g_mutex)         g_mutex         = xSemaphoreCreateMutex();
    if (!g_predictSignal) g_predictSignal = xSemaphoreCreateBinary();
}

static void requestPrediction()
{
    g_needPredict = true;
    if (g_predictSignal) xSemaphoreGive(g_predictSignal);
}

// =============================================================================
// 2. SPIFFS: configuration and TLE cache
// =============================================================================

static void tleCachePath(uint32_t norad, char *out, size_t n)
{
    snprintf(out, n, "/tle/%lu.txt", (unsigned long)norad);
}

// Re-run the SGP4 initialisation for one slot.  Caller holds the mutex.
static void reparseSlot(SatSlot &s)
{
    s.recValid = false;
    s.recError = 0;
    if (!s.tleLoaded) {
        snprintf(s.status, sizeof(s.status), "no TLE yet");
        return;
    }
    if (sgp4::parseTle(s.tle1, s.tle2, s.rec)) {
        s.recValid = true;
        snprintf(s.status, sizeof(s.status), "ok, %.1f min orbit", s.rec.periodMin);
    } else {
        s.recError = s.rec.error;
        if (s.rec.deepSpace)
            snprintf(s.status, sizeof(s.status), "deep-space object, unsupported");
        else
            snprintf(s.status, sizeof(s.status), "bad element set (err %d)", s.rec.error);
    }
}

static void loadTleCache(SatSlot &s)
{
    char path[32];
    tleCachePath(s.norad, path, sizeof(path));

    fs::File f = SPIFFS.open(path, "r");
    if (!f) return;

    String fetched = f.readStringUntil('\n');
    String name    = f.readStringUntil('\n');
    String l1      = f.readStringUntil('\n');
    String l2      = f.readStringUntil('\n');
    f.close();

    l1.trim();
    l2.trim();
    name.trim();
    if (l1.length() < 68 || l2.length() < 68) return;

    s.fetchedUnix = (uint32_t)fetched.toInt();
    strlcpy(s.name, name.c_str(), sizeof(s.name));
    strlcpy(s.tle1, l1.c_str(), sizeof(s.tle1));
    strlcpy(s.tle2, l2.c_str(), sizeof(s.tle2));
    s.tleLoaded = true;
    reparseSlot(s);
}

static void saveTleCache(const SatSlot &s)
{
    char path[32];
    tleCachePath(s.norad, path, sizeof(path));

    fs::File f = SPIFFS.open(path, "w");
    if (!f) {
        Serial.printf("🛰️ could not write %s\n", path);
        return;
    }
    f.printf("%lu\n%s\n%s\n%s\n", (unsigned long)s.fetchedUnix, s.name, s.tle1, s.tle2);
    f.close();
}

static void saveConfig()
{
    JsonDocument doc;
    doc["minElevation"]    = g_minElevation;
    doc["horizonDays"]     = g_horizonDays;
    doc["tleRefreshHours"] = g_tleRefreshHours;
    doc["useLocalTime"]    = g_useLocalTime;
    doc["altitudeM"]       = g_altitudeM;
    doc["alertLeadMin"]    = g_alertLeadMin;

    JsonArray arr = doc["sats"].to<JsonArray>();
    for (uint8_t i = 0; i < g_slotCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["norad"] = g_slots[i].norad;
        o["alert"] = g_slots[i].alertEnabled;
    }

    fs::File f = SPIFFS.open(SAT_CONFIG_FILE, "w");
    if (!f) {
        Serial.println("🛰️ could not write /satellites.json");
        return;
    }
    serializeJsonPretty(doc, f);
    f.close();
    Serial.printf("🛰️ config saved, %u satellite(s)\n", g_slotCount);
}

static void loadConfig()
{
    fs::File f = SPIFFS.open(SAT_CONFIG_FILE, "r");
    if (!f) {
        Serial.println("🛰️ no /satellites.json yet - starting with an empty list");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("🛰️ /satellites.json is unreadable (%s) - using defaults\n", err.c_str());
        return;
    }

    g_minElevation    = doc["minElevation"]    | g_minElevation;
    g_horizonDays     = doc["horizonDays"]     | g_horizonDays;
    g_tleRefreshHours = doc["tleRefreshHours"] | g_tleRefreshHours;
    g_useLocalTime    = doc["useLocalTime"]    | g_useLocalTime;
    g_altitudeM       = doc["altitudeM"]       | g_altitudeM;
    g_alertLeadMin    = doc["alertLeadMin"]    | g_alertLeadMin;

    if (g_horizonDays < 1) g_horizonDays = 1;
    if (g_horizonDays > 5) g_horizonDays = 5;
    if (g_tleRefreshHours < 1) g_tleRefreshHours = 1;
    if (g_minElevation < 0.0)  g_minElevation = 0.0;
    if (g_minElevation > 60.0) g_minElevation = 60.0;
    if (g_alertLeadMin < 1)  g_alertLeadMin = 1;
    if (g_alertLeadMin > 15) g_alertLeadMin = 15;

    g_slotCount = 0;
    JsonArray arr = doc["sats"].as<JsonArray>();
    for (JsonVariant v : arr) {
        if (g_slotCount >= SAT_MAX_SATS) break;
        uint32_t id; bool alert = false;
        if (v.is<JsonObject>()) { id = v["norad"] | 0; alert = v["alert"] | false; }
        else                    { id = v.as<uint32_t>(); }
        if (id == 0) continue;
        SatSlot &s = g_slots[g_slotCount];
        memset(&s, 0, sizeof(s));
        s.norad = id;
        s.alertEnabled = alert;
        snprintf(s.name, sizeof(s.name), "%lu", (unsigned long)id);
        snprintf(s.status, sizeof(s.status), "no TLE yet");
        loadTleCache(s);
        g_slotCount++;
    }
    Serial.printf("🛰️ config loaded: %u satellite(s), minEl %.0f deg\n",
                  g_slotCount, g_minElevation);
}

// =============================================================================
// 3. TLE download
// =============================================================================

// Pull the three TLE lines out of a Celestrak GP response.
static bool parseTleResponse(const String &body, char *name, size_t nameLen,
                             char *l1, size_t l1Len, char *l2, size_t l2Len)
{
    if (body.indexOf("No GP data found") >= 0) return false;

    String firstName;
    bool haveL1 = false, haveL2 = false;

    int start = 0;
    while (start < (int)body.length()) {
        int nl = body.indexOf('\n', start);
        if (nl < 0) nl = body.length();
        String line = body.substring(start, nl);
        start = nl + 1;

        line.replace("\r", "");
        while (line.length() && line[line.length() - 1] == ' ')
            line.remove(line.length() - 1);
        if (line.length() == 0) continue;

        if (!haveL1 && line.startsWith("1 ") && line.length() >= 68) {
            strlcpy(l1, line.c_str(), l1Len);
            haveL1 = true;
        } else if (haveL1 && !haveL2 && line.startsWith("2 ") && line.length() >= 68) {
            strlcpy(l2, line.c_str(), l2Len);
            haveL2 = true;
        } else if (firstName.length() == 0 && !haveL1) {
            firstName = line;
        }
    }

    if (!haveL1 || !haveL2) return false;
    strlcpy(name, firstName.length() ? firstName.c_str() : "UNKNOWN", nameLen);
    return true;
}

// Blocking HTTP fetch for one satellite.  Runs on the main loop, like the
// existing weather and propagation fetches, so only one TLS session is ever
// open at a time.
static void fetchTleFor(uint8_t idx, time_t now)
{
    uint32_t norad;
    SAT_LOCK();
    norad = g_slots[idx].norad;
    SAT_UNLOCK();
    if (!norad) return;

    char url[128];
    snprintf(url, sizeof(url),
             "https://celestrak.org/NORAD/elements/gp.php?CATNR=%lu&FORMAT=TLE",
             (unsigned long)norad);

    HTTPClient http;
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(url)) {
        Serial.printf("🛰️ %lu: http begin failed\n", (unsigned long)norad);
        return;
    }
    http.setUserAgent("ESP32-HamClock/1.0 (satellite pass page)");

    int code = http.GET();
    String body = (code == HTTP_CODE_OK) ? http.getString() : String();
    http.end();

    char name[26], l1[71], l2[71];
    bool ok = (code == HTTP_CODE_OK) &&
              parseTleResponse(body, name, sizeof(name), l1, sizeof(l1), l2, sizeof(l2));

    SAT_LOCK();
    SatSlot &s = g_slots[idx];
    if (s.norad != norad) { SAT_UNLOCK(); return; }   // list changed under us

    if (ok) {
        strlcpy(s.name, name, sizeof(s.name));
        strlcpy(s.tle1, l1,   sizeof(s.tle1));
        strlcpy(s.tle2, l2,   sizeof(s.tle2));
        s.tleLoaded    = true;
        s.fetchedUnix  = (uint32_t)now;
        s.failures     = 0;
        s.retryAfterUnix = 0;
        reparseSlot(s);
        Serial.printf("🛰️ %lu -> %s (%s)\n", (unsigned long)norad, s.name, s.status);
    } else {
        if (s.failures < 8) s.failures++;
        // Exponential backoff, capped at an hour, so a wrong id or an outage
        // does not turn into a request loop against Celestrak.
        uint32_t backoff = 60u << (s.failures - 1);
        if (backoff > 3600u) backoff = 3600u;
        s.retryAfterUnix = (uint32_t)now + backoff;
        if (code != HTTP_CODE_OK)
            snprintf(s.status, sizeof(s.status), "download failed (HTTP %d)", code);
        else
            snprintf(s.status, sizeof(s.status), "no element set for this id");
        Serial.printf("🛰️ %lu: %s, retry in %lus\n",
                      (unsigned long)norad, s.status, (unsigned long)backoff);
    }
    SAT_UNLOCK();

    if (ok) {
        saveTleCache(g_slots[idx]);   // outside the lock: SPIFFS can be slow
        requestPrediction();
    }
}

// =============================================================================
// 4. Prediction task
// =============================================================================

static void satYield() { vTaskDelay(1); }

static void runPrediction(time_t now)
{
    // Task-private snapshots, kept out of the task stack.
    static sgp4::SatRec work[SAT_MAX_SATS];
    static bool         workOk[SAT_MAX_SATS];
    static PassRow      tmp[SAT_MAX_SATS * SAT_PASSES_PER_SAT];

    double latRad, lonRad, altKm, minEl;
    uint8_t count, horizonDays;

    SAT_LOCK();
    count       = g_slotCount;
    latRad      = g_latDeg * DEG2RAD_L;
    lonRad      = g_lonDeg * DEG2RAD_L;
    altKm       = g_altitudeM / 1000.0;
    minEl       = g_minElevation;
    horizonDays = g_horizonDays;
    for (uint8_t i = 0; i < count; i++) {
        workOk[i] = g_slots[i].recValid;
        if (workOk[i]) memcpy(&work[i], &g_slots[i].rec, sizeof(sgp4::SatRec));
    }
    SAT_UNLOCK();

    uint8_t n = 0;
    uint32_t startedMs = millis();

    for (uint8_t i = 0; i < count; i++) {
        if (!workOk[i]) continue;

        double t = (double)now;
        for (uint8_t k = 0; k < SAT_PASSES_PER_SAT; k++) {
            sgp4::Pass p;
            if (!sgp4::findNextPass(work[i], t, horizonDays * 86400.0,
                                    latRad, lonRad, altKm, minEl, p, satYield))
                break;
            tmp[n].slot = i;
            tmp[n].p    = p;
            n++;
            t = p.losUnix + 60.0;
            vTaskDelay(1);
        }
        vTaskDelay(1);
    }

    // Merge the per-satellite results into one AOS-ordered timeline.
    for (uint8_t i = 1; i < n; i++) {
        PassRow key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].p.aosUnix > key.p.aosUnix) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }

    SAT_LOCK();
    memcpy(g_passes, tmp, sizeof(PassRow) * n);
    g_passCount       = n;
    g_predictedAtUnix = (uint32_t)now;
    g_passVersion++;
    SAT_UNLOCK();

    Serial.printf("🛰️ predicted %u pass(es) in %lu ms\n",
                  n, (unsigned long)(millis() - startedMs));
}

static void satPredictTask(void *)
{
    for (;;) {
        // Wake on request, and in any case every 15 s to retire finished passes.
        xSemaphoreTake(g_predictSignal, pdMS_TO_TICKS(15000));

        time_t now = g_nowUnix;
        if (now < 1600000000L) continue;      // NTP has not delivered yet

        if (!g_needPredict) {
            bool stale = false;
            SAT_LOCK();
            if (g_passCount == 0 && g_slotCount > 0 && g_predictedAtUnix == 0) stale = true;
            for (uint8_t i = 0; i < g_passCount; i++)
                if (g_passes[i].p.losUnix < (double)now) { stale = true; break; }
            SAT_UNLOCK();
            if (!stale) continue;
        }

        g_needPredict = false;
        g_predictBusy = true;
        runPrediction(now);
        g_predictBusy = false;
    }
}

// =============================================================================
// 5. Web routes
// =============================================================================

static void handleSatPage(WebServer &server)
{
    fs::File f = SPIFFS.open("/sat.html", "r");
    if (!f) {
        server.send(404, "text/plain", "sat.html is missing - upload the SPIFFS image");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

static void buildConfigJson(String &out)
{
    JsonDocument doc;

    SAT_LOCK();
    doc["minElevation"]    = g_minElevation;
    doc["horizonDays"]     = g_horizonDays;
    doc["tleRefreshHours"] = g_tleRefreshHours;
    doc["useLocalTime"]    = g_useLocalTime;
    doc["altitudeM"]       = g_altitudeM;
    doc["latitude"]        = g_latDeg;
    doc["longitude"]       = g_lonDeg;
    doc["utc"]             = (uint32_t)g_nowUnix;
    doc["predictedAt"]     = g_predictedAtUnix;
    doc["busy"]            = g_predictBusy;
    doc["maxSats"]         = SAT_MAX_SATS;
    doc["alertLeadMin"]    = g_alertLeadMin;

    JsonArray arr = doc["sats"].to<JsonArray>();
    for (uint8_t i = 0; i < g_slotCount; i++) {
        SatSlot &s = g_slots[i];
        JsonObject o = arr.add<JsonObject>();
        o["norad"]  = s.norad;
        o["name"]   = s.name;
        o["ok"]     = s.recValid;
        o["status"] = s.status;
        o["deepSpace"] = s.recValid ? false : (s.recError == sgp4::ERR_DEEP_SPACE);
        o["tleAgeH"] = (s.fetchedUnix && g_nowUnix > (time_t)s.fetchedUnix)
                           ? ((double)(g_nowUnix - (time_t)s.fetchedUnix) / 3600.0)
                           : -1.0;
        o["alert"] = s.alertEnabled;
    }
    SAT_UNLOCK();

    serializeJson(doc, out);
}

static void handleSaveSatellites(WebServer &server)
{
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    // Keep the existing slots for ids that are still in the list, so a settings
    // change does not throw away TLEs that were already downloaded.
    static SatSlot old[SAT_MAX_SATS];   // too big for the loop task's stack
    uint8_t oldCount;

    SAT_LOCK();
    memcpy(old, g_slots, sizeof(g_slots));
    oldCount = g_slotCount;

    g_minElevation    = doc["minElevation"]    | g_minElevation;
    g_horizonDays     = doc["horizonDays"]     | g_horizonDays;
    g_tleRefreshHours = doc["tleRefreshHours"] | g_tleRefreshHours;
    g_useLocalTime    = doc["useLocalTime"]    | g_useLocalTime;
    g_altitudeM       = doc["altitudeM"]       | g_altitudeM;
    g_alertLeadMin    = doc["alertLeadMin"]    | g_alertLeadMin;

    if (g_horizonDays < 1) g_horizonDays = 1;
    if (g_horizonDays > 5) g_horizonDays = 5;
    if (g_tleRefreshHours < 1) g_tleRefreshHours = 1;
    if (g_minElevation < 0.0)  g_minElevation = 0.0;
    if (g_minElevation > 60.0) g_minElevation = 60.0;
    if (g_alertLeadMin < 1)  g_alertLeadMin = 1;
    if (g_alertLeadMin > 15) g_alertLeadMin = 15;

    g_slotCount = 0;
    JsonArray arr = doc["sats"].as<JsonArray>();
    for (JsonVariant v : arr) {
        if (g_slotCount >= SAT_MAX_SATS) break;
        uint32_t id; bool alertWanted = false;
        if (v.is<JsonObject>()) { id = v["norad"] | 0; alertWanted = v["alert"] | false; }
        else                    { id = v.as<uint32_t>(); }
        if (id == 0) continue;

        bool duplicate = false;
        for (uint8_t k = 0; k < g_slotCount; k++)
            if (g_slots[k].norad == id) { duplicate = true; break; }
        if (duplicate) continue;

        SatSlot &s = g_slots[g_slotCount];
        memset(&s, 0, sizeof(s));
        s.norad = id;
        snprintf(s.name, sizeof(s.name), "%lu", (unsigned long)id);
        snprintf(s.status, sizeof(s.status), "no TLE yet");

        for (uint8_t k = 0; k < oldCount; k++) {
            if (old[k].norad == id) { memcpy(&s, &old[k], sizeof(SatSlot)); break; }
        }
        // Always take the alert flag from this request - the memcpy above
        // would otherwise silently restore whatever it was before this save.
        s.alertEnabled = alertWanted;
        g_slotCount++;
    }

    g_passCount = 0;
    g_passVersion++;
    g_predictedAtUnix = 0;
    SAT_UNLOCK();

    saveConfig();
    requestPrediction();

    String out;
    buildConfigJson(out);
    server.send(200, "application/json", out);
}

static void handleSatPasses(WebServer &server)
{
    JsonDocument doc;
    doc["utc"] = (uint32_t)g_nowUnix;
    doc["useLocalTime"] = g_useLocalTime;

    SAT_LOCK();
    JsonArray arr = doc["passes"].to<JsonArray>();
    for (uint8_t i = 0; i < g_passCount; i++) {
        PassRow &r = g_passes[i];
        JsonObject o = arr.add<JsonObject>();
        o["norad"]   = g_slots[r.slot].norad;
        o["name"]    = g_slots[r.slot].name;
        o["aos"]     = (uint32_t)r.p.aosUnix;
        o["los"]     = (uint32_t)r.p.losUnix;
        o["max"]     = (uint32_t)r.p.maxUnix;
        o["maxEl"]   = r.p.maxElDeg;
        o["aosAz"]   = r.p.aosAzDeg;
        o["losAz"]   = r.p.losAzDeg;
    }
    SAT_UNLOCK();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleSatRefresh(WebServer &server)
{
    SAT_LOCK();
    for (uint8_t i = 0; i < g_slotCount; i++) {
        g_slots[i].fetchedUnix    = 0;
        g_slots[i].failures       = 0;
        g_slots[i].retryAfterUnix = 0;
    }
    SAT_UNLOCK();
    server.send(200, "application/json", "{\"status\":\"refreshing\"}");
}

void satellitesRegisterRoutes(WebServer &server)
{
    ensureSync();
    server.on("/sat.html",   HTTP_GET,  [&server]() { handleSatPage(server); });
    server.on("/satellites", HTTP_GET,  [&server]() {
        String out;
        buildConfigJson(out);
        server.send(200, "application/json", out);
    });
    server.on("/satellites", HTTP_POST, [&server]() { handleSaveSatellites(server); });
    server.on("/satpasses",  HTTP_GET,  [&server]() { handleSatPasses(server); });
    server.on("/satrefresh", HTTP_GET,  [&server]() { handleSatRefresh(server); });
}

// =============================================================================
// 6. Live look angles
// =============================================================================

// How many satellites the banner can list at once.  Four still leaves three
// rows for the prediction table underneath.
#define SAT_BANNER_MAX 4

// Upper bound on the rows the pass list can ever hold, for its scratch arrays.
#define SAT_LIST_MAX 8

struct SatLive {
    uint8_t slot;
    char    name[16];
    double  elDeg, azDeg, rangeKm;
    bool    haveLos;
    double  losUnix;
    double  maxElDeg;
};

// Every satellite currently above the configured minimum elevation, highest
// first, capped at what the banner can show.  Returns how many were found.
static uint8_t computeLive(time_t now, SatLive *out, uint8_t maxOut)
{
    uint8_t n = 0;
    double jd = sgp4::jdFromUnix((double)now);

    SAT_LOCK();
    double latRad = g_latDeg * DEG2RAD_L;
    double lonRad = g_lonDeg * DEG2RAD_L;
    double altKm  = g_altitudeM / 1000.0;
    double minEl  = g_minElevation;

    for (uint8_t i = 0; i < g_slotCount; i++) {
        if (!g_slots[i].recValid) continue;

        double tsince = (jd - g_slots[i].rec.jdsatepoch) * 1440.0;
        double r[3], v[3];
        int err = 0;
        if (!sgp4::propagate(g_slots[i].rec, tsince, r, v, &err)) continue;

        sgp4::Look look;
        sgp4::observe(r, v, jd, latRad, lonRad, altKm, look);
        if (look.elDeg < minEl) continue;

        SatLive entry;
        entry.slot     = i;
        entry.elDeg    = look.elDeg;
        entry.azDeg    = look.azDeg;
        entry.rangeKm  = look.rangeKm;
        entry.haveLos  = false;
        entry.losUnix  = 0.0;
        entry.maxElDeg = look.elDeg;
        strlcpy(entry.name, g_slots[i].name, sizeof(entry.name));

        // Insertion sort by elevation, highest first; when full the lowest one
        // falls off the end.
        uint8_t pos = n;
        while (pos > 0 && out[pos - 1].elDeg < entry.elDeg) pos--;
        if (pos >= maxOut) continue;
        for (uint8_t k = (n < maxOut ? n : (uint8_t)(maxOut - 1)); k > pos; k--)
            out[k] = out[k - 1];
        out[pos] = entry;
        if (n < maxOut) n++;
    }

    // Attach the LOS and culmination of the pass each one is inside.
    for (uint8_t e = 0; e < n; e++) {
        for (uint8_t i = 0; i < g_passCount; i++) {
            if (g_passes[i].slot != out[e].slot) continue;
            if (g_passes[i].p.aosUnix - 60.0 <= (double)now &&
                g_passes[i].p.losUnix >= (double)now) {
                out[e].haveLos  = true;
                out[e].losUnix  = g_passes[i].p.losUnix;
                out[e].maxElDeg = g_passes[i].p.maxElDeg;
                break;
            }
        }
    }
    SAT_UNLOCK();
    return n;
}

// Soonest alert-enabled pass whose AOS is inside the lead window right now.
// g_passes is already AOS-ascending (see the insertion sort in
// runPrediction()), so this can stop as soon as a row is further out than the
// window - nothing later in the list can qualify either.
static uint32_t g_lastAlertedNorad = 0;
static double   g_lastAlertedAos   = 0.0;

bool satellitesCheckAlert(time_t utcNow, char *outName, size_t nameLen,
                           long *outSecsToAos, bool *outIsNewTrigger)
{
    bool found = false;
    *outIsNewTrigger = false;

    SAT_LOCK();
    for (uint8_t i = 0; i < g_passCount; i++) {
        double secs = g_passes[i].p.aosUnix - (double)utcNow;
        if (secs > g_alertLeadMin * 60.0) break;   // sorted - nothing further qualifies
        if (secs <= 0.0) continue;                  // already risen
        const SatSlot &s = g_slots[g_passes[i].slot];
        if (!s.alertEnabled) continue;

        strlcpy(outName, s.name, nameLen);
        *outSecsToAos = (long)secs;
        found = true;
        if (s.norad != g_lastAlertedNorad || g_passes[i].p.aosUnix != g_lastAlertedAos) {
            *outIsNewTrigger = true;
            g_lastAlertedNorad = s.norad;
            g_lastAlertedAos   = g_passes[i].p.aosUnix;
        }
        break;
    }
    SAT_UNLOCK();
    return found;
}

// =============================================================================
// 7. TFT page rendering
// =============================================================================

// Fixed furniture (320x240, rotation 3).  y values are the top of the text,
// which is what drawString() with TL_DATUM uses.
static const int HDR_Y         = 2;
static const int RULE1_Y       = 19;
static const int BANNER_Y      = 22;
static const int BANNER_LINE_H = 18;
static const int BANNER_PAD    = 8;
static const int BANNER_MAX_H  = BANNER_PAD + SAT_BANNER_MAX * BANNER_LINE_H;
static const int ROW_H         = 24;
static const int LIST_BOTTOM   = 232;   // last row's text must end by here

static const int COL_NAME = 6;
static const int COL_AOS  = 112;
static const int COL_EL   = 170;
static const int COL_DUR  = 205;
static const int COL_AZ   = 252;

// The banner reuses the list's x positions, so the columns line up all the way
// down the screen even though the two carry different fields.
static const int BCOL_NAME = COL_NAME;   // satellite
static const int BCOL_EL   = COL_AOS;    // elevation now
static const int BCOL_AZ   = COL_EL;     // azimuth now
static const int BCOL_LOS  = COL_AZ;     // how much longer it stays up

// The banner grows with the number of satellites overhead, so everything below
// it is positioned relative to that.
static int bannerHeight(uint8_t lines) { return BANNER_PAD + lines * BANNER_LINE_H; }
static int colHdrY(uint8_t lines)      { return BANNER_Y + bannerHeight(lines) + 6; }
static int rule2Y(uint8_t lines)       { return colHdrY(lines) + 14; }
static int row0Y(uint8_t lines)        { return rule2Y(lines) + 6; }

// As many rows as physically fit under the banner - the list is not capped by
// a setting, it simply fills the screen.
static uint8_t tableCapacity(uint8_t lines)
{
    int avail = LIST_BOTTOM - 10 - row0Y(lines);
    if (avail < 0) return 0;
    uint8_t rows = (uint8_t)(avail / ROW_H + 1);
    return rows > SAT_LIST_MAX ? SAT_LIST_MAX : rows;
}

static const char *compass16(double az)
{
    static const char *pts[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                                  "S","SSW","SW","WSW","W","WNW","NW","NNW"};
    int idx = (int)floor(fmod(az + 11.25, 360.0) / 22.5);
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return pts[idx];
}

static void fmtClock(time_t utc, int tOffsetHours, char *out, size_t n)
{
    time_t t = g_useLocalTime ? utc + (time_t)tOffsetHours * 3600 : utc;
    struct tm tm_;
    gmtime_r(&t, &tm_);
    snprintf(out, n, "%02d:%02d", tm_.tm_hour, tm_.tm_min);
}

// "4d23h" / "1:23:45" / "04:12"
static void fmtCountdown(long secs, char *out, size_t n)
{
    if (secs < 0) secs = 0;
    if (secs >= 86400)      snprintf(out, n, "%ldd%02ldh", secs / 86400, (secs % 86400) / 3600);
    else if (secs >= 3600)  snprintf(out, n, "%ld:%02ld:%02ld", secs / 3600, (secs % 3600) / 60, secs % 60);
    else                    snprintf(out, n, "%02ld:%02ld", secs / 60, secs % 60);
}

static void fmtDuration(long secs, char *out, size_t n)
{
    if (secs < 0) secs = 0;
    snprintf(out, n, "%02ld:%02ld", secs / 60, secs % 60);
}

// Pad to a fixed width so opaque text overwrites whatever was there before.
static void padTo(char *s, size_t width, size_t bufLen)
{
    size_t len = strlen(s);
    while (len < width && len + 1 < bufLen) s[len++] = ' ';
    s[len] = '\0';
}

// "TLE 6h" / "TLE 5d" / "no TLE" for the oldest element set on file.
static void tleHealth(time_t utc, char *out, size_t n, uint16_t *colour)
{
    double oldestAgeH = -1.0;
    uint8_t count, bad = 0;

    SAT_LOCK();
    count = g_slotCount;
    for (uint8_t i = 0; i < g_slotCount; i++) {
        if (!g_slots[i].recValid) { bad++; continue; }
        if (g_slots[i].fetchedUnix && utc > (time_t)g_slots[i].fetchedUnix) {
            double ageH = (double)(utc - (time_t)g_slots[i].fetchedUnix) / 3600.0;
            if (ageH > oldestAgeH) oldestAgeH = ageH;
        }
    }
    SAT_UNLOCK();

    *colour = themeDim;
    char tmp[12];
    if (bad) {
        // The bottom status line is gone, so the warning lives here now.
        snprintf(tmp, sizeof(tmp), "%u NO TLE", bad);
        *colour = TFT_RED;
    } else if (oldestAgeH < 0.0) {
        snprintf(tmp, sizeof(tmp), "%s", count ? "no TLE" : "");
        if (count) *colour = TFT_ORANGE;
    } else if (oldestAgeH < 99.0) {
        snprintf(tmp, sizeof(tmp), "TLE %.0fh", oldestAgeH);
        // TLEs go stale after about a week.
        if (oldestAgeH > 48.0) *colour = TFT_ORANGE;
    } else {
        // Days, so a badly stale set cannot widen the field past its slot.
        snprintf(tmp, sizeof(tmp), "TLE %.0fd", oldestAgeH / 24.0);
        *colour = (oldestAgeH > 168.0) ? TFT_RED : TFT_ORANGE;
    }
    snprintf(out, n, "%-8s", tmp);
}

// Page title and the rule under it - the only furniture that never moves.
static void drawPageHeader(TFT_eSPI &tft)
{
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&Orbitron_Medium8pt7b);
    tft.setTextColor(themeAccent, themeBg);
    tft.drawString("SATELLITES", COL_NAME, HDR_Y);
    tft.drawFastHLine(4, RULE1_Y, 312, themeDim);
}

static void drawHeaderClock(TFT_eSPI &tft, time_t utc, int tOffsetHours)
{
    struct tm tm_;
    time_t shown = g_useLocalTime ? utc + (time_t)tOffsetHours * 3600 : utc;
    gmtime_r(&shown, &tm_);

    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s",
             tm_.tm_hour, tm_.tm_min, tm_.tm_sec, g_useLocalTime ? "LOC" : "UTC");

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim2, themeBg);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(buf, 314, HDR_Y + 2);
    tft.setTextDatum(TL_DATUM);

    char tle[12];
    uint16_t tleColour;
    tleHealth(utc, tle, sizeof(tle), &tleColour);
    tft.setTextColor(tleColour, themeBg);
    tft.drawString(tle, 146, HDR_Y + 2);
}

static void drawBannerFrame(TFT_eSPI &tft, uint8_t lines, bool anyLive)
{
    tft.drawRoundRect(2, BANNER_Y, 316, bannerHeight(lines), 6,
                      anyLive ? TFT_GREEN : themeDim);
}

// One line per satellite that is workable right now.
static void drawBannerLines(TFT_eSPI &tft, const SatLive *live, uint8_t n, time_t utc)
{
    char line[56];

    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);

    if (n == 0) {
        double aos = 0.0;
        bool found = false;
        SAT_LOCK();
        for (uint8_t i = 0; i < g_passCount; i++) {
            if (g_passes[i].p.aosUnix > (double)utc) {
                aos = g_passes[i].p.aosUnix;
                found = true;
                break;
            }
        }
        SAT_UNLOCK();

        if (found) {
            char cd[16];
            fmtCountdown((long)(aos - (double)utc), cd, sizeof(cd));
            snprintf(line, sizeof(line), "NO PASS IN PROGRESS  AOS in %s", cd);
        } else {
            snprintf(line, sizeof(line), "NO PASS IN PROGRESS");
        }
        padTo(line, 38, sizeof(line));
        tft.setTextColor(themeDim, themeBg);
        tft.drawString(line, BCOL_NAME, BANNER_Y + 5);
        return;
    }

    for (uint8_t i = 0; i < n; i++) {
        // Every field is fixed width, so the opaque text overwrites the
        // previous value without needing the row cleared first.
        char name[14];
        strlcpy(name, live[i].name, sizeof(name));
        padTo(name, 12, sizeof(name));

        char el[6];
        snprintf(el, sizeof(el), "%2.0f", live[i].elDeg);

        char az[12];
        snprintf(az, sizeof(az), "%03.0f %-3s", live[i].azDeg, compass16(live[i].azDeg));

        char los[12];
        if (live[i].haveLos) {
            char cd[16];
            fmtCountdown((long)(live[i].losUnix - (double)utc), cd, sizeof(cd));
            snprintf(los, sizeof(los), "%-7s", cd);
        } else {
            snprintf(los, sizeof(los), "%-7s", "--:--");
        }

        int y = BANNER_Y + 5 + i * BANNER_LINE_H;
        tft.setTextColor(TFT_GREEN, themeBg);
        tft.drawString(name, BCOL_NAME, y);
        tft.drawString(el,   BCOL_EL,   y);
        tft.drawString(az,   BCOL_AZ,   y);
        tft.drawString(los,  BCOL_LOS,  y);
    }
}

static void drawTableFurniture(TFT_eSPI &tft, uint8_t lines)
{
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(themeDim, themeBg);

    int y = colHdrY(lines);
    tft.drawString("SAT", COL_NAME, y);
    tft.drawString("AOS", COL_AOS,  y);
    tft.drawString("EL",  COL_EL,   y);
    tft.drawString("DUR", COL_DUR,  y);
    tft.drawString("AZ",  COL_AZ,   y);

    tft.drawFastHLine(4, rule2Y(lines), 312, themeDim);
}

// One line of the pass list.  Used both for the table and for the bottom row.
static void drawPassRow(TFT_eSPI &tft, int y, const char *satName,
                        const sgp4::Pass &p, time_t utc, int tOffsetHours)
{
    uint16_t colour = (p.aosUnix - (double)utc < 600.0) ? themeWarn : themeDim2;

    char name[14];
    strlcpy(name, satName, sizeof(name));
    padTo(name, 12, sizeof(name));

    char aos[8];
    fmtClock((time_t)p.aosUnix, tOffsetHours, aos, sizeof(aos));

    char el[5];
    snprintf(el, sizeof(el), "%2.0f", p.maxElDeg);

    char dur[8];
    fmtDuration((long)(p.losUnix - p.aosUnix), dur, sizeof(dur));

    char az[10];
    snprintf(az, sizeof(az), "%03.0f>%03.0f", p.aosAzDeg, p.losAzDeg);

    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextColor(colour, themeBg);
    tft.drawString(name, COL_NAME, y);
    tft.drawString(aos,  COL_AOS,  y);
    tft.drawString(el,   COL_EL,   y);
    tft.drawString(dur,  COL_DUR,  y);
    tft.drawString(az,   COL_AZ,   y);
}

// True when this pass is already running and its satellite is on the banner, in
// which case the table must not repeat it.
static bool shownOnBanner(const PassRow &row, time_t utc, uint16_t bannerMask)
{
    return row.p.aosUnix <= (double)utc &&
           (bannerMask & ((uint16_t)1 << row.slot)) != 0;
}

static void drawTable(TFT_eSPI &tft, time_t utc, int tOffsetHours,
                      uint8_t lines, uint16_t bannerMask, bool clearFirst)
{
    const int first = row0Y(lines);
    const uint8_t capacity = tableCapacity(lines);

    if (clearFirst)
        tft.fillRect(0, rule2Y(lines) + 1, 320, 240 - rule2Y(lines) - 1, themeBg);

    PassRow rows[SAT_LIST_MAX];
    char    names[SAT_LIST_MAX][14];
    uint8_t shown = 0;
    uint8_t configured;

    SAT_LOCK();
    configured = g_slotCount;
    for (uint8_t i = 0; i < g_passCount && shown < capacity; i++) {
        if (g_passes[i].p.losUnix < (double)utc) continue;              // already over
        if (shownOnBanner(g_passes[i], utc, bannerMask)) continue;      // on the banner
        rows[shown] = g_passes[i];
        strlcpy(names[shown], g_slots[g_passes[i].slot].name, sizeof(names[shown]));
        shown++;
    }
    SAT_UNLOCK();

    for (uint8_t i = 0; i < capacity; i++) {
        int y = first + i * ROW_H;
        if (i >= shown) {
            tft.fillRect(0, y - 2, 320, ROW_H, themeBg);
            continue;
        }
        drawPassRow(tft, y, names[i], rows[i].p, utc, tOffsetHours);
    }

    if (shown == 0) {
        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(themeDim, themeBg);
        tft.drawString(configured == 0 ? "No satellites configured."
                                       : "No further passes predicted.",
                       COL_NAME, first);
        if (configured == 0)
            tft.drawString("Open http://hamclock.local/sat.html", COL_NAME, first + 20);
    }
}

void satellitesDrawPage(TFT_eSPI &tft, time_t utcNow, int tOffsetHours, bool fullRedraw)
{
    static uint32_t lastVersion = 0xFFFFFFFFu;
    static time_t   lastSecond  = 0;
    static time_t   lastTable   = 0;
    static uint8_t  lastLines   = 0xFF;

    SatLive live[SAT_BANNER_MAX];
    uint8_t nLive = computeLive(utcNow, live, SAT_BANNER_MAX);
    uint8_t lines = nLive ? nLive : 1;

    uint16_t bannerMask = 0;
    for (uint8_t i = 0; i < nLive; i++)
        bannerMask |= (uint16_t)1 << live[i].slot;

    // A change in how many satellites are overhead resizes the banner, which
    // moves everything below it.
    bool relayout = fullRedraw || (lines != lastLines);

    if (fullRedraw) {
        tft.fillScreen(themeBg);
        drawPageHeader(tft);
    }

    if (relayout) {
        tft.fillRect(0, BANNER_Y, 320, 240 - BANNER_Y, themeBg);
        drawBannerFrame(tft, lines, nLive > 0);
        drawTableFurniture(tft, lines);
        lastLines   = lines;
        lastVersion = 0xFFFFFFFFu;
        lastSecond  = 0;
    }

    if (lastVersion != g_passVersion || relayout) {
        drawTable(tft, utcNow, tOffsetHours, lines, bannerMask, true);
        lastVersion = g_passVersion;
        lastTable   = utcNow;
    } else if (utcNow - lastTable >= 15) {
        // Refresh the row colours (imminent) without clearing.
        drawTable(tft, utcNow, tOffsetHours, lines, bannerMask, false);
        lastTable = utcNow;
    }

    if (utcNow != lastSecond) {
        drawHeaderClock(tft, utcNow, tOffsetHours);
        drawBannerLines(tft, live, nLive, utcNow);
        lastSecond = utcNow;
    }
}

// =============================================================================
// 8. Public entry points
// =============================================================================

void satellitesSelfTest()
{
    // Vallado's SGP4 verification case 5 - a wrong constant anywhere in the
    // propagator shows up here as kilometres of error.
    const char *l1 = "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
    const char *l2 = "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";

    sgp4::SatRec s;
    if (!sgp4::parseTle(l1, l2, s)) {
        Serial.printf("🛰️ SGP4 selftest: parse FAILED (err %d)\n", s.error);
        return;
    }

    struct { double t, x, y, z; } expect[] = {
        {    0.0,  7022.46529266, -1400.08296755,     0.03995155},
        {  720.0, -7134.59340119,  6531.68641334,  3260.27186483},
        { 1440.0,  -938.55923943, -6268.18748831, -4294.02924751},
    };

    double worst = 0.0;
    for (auto &e : expect) {
        double r[3], v[3];
        int err = 0;
        if (!sgp4::propagate(s, e.t, r, v, &err)) {
            Serial.printf("🛰️ SGP4 selftest: propagate FAILED at t=%.0f (err %d)\n", e.t, err);
            return;
        }
        worst = fmax(worst, fabs(r[0] - e.x));
        worst = fmax(worst, fabs(r[1] - e.y));
        worst = fmax(worst, fabs(r[2] - e.z));
    }

    Serial.printf("🛰️ SGP4 selftest: worst position error %.2e km - %s\n",
                  worst, (worst < 1.0e-4) ? "PASS" : "FAIL");

    // --- moon ---------------------------------------------------------------
    // Reference figures for one fixed instant at one fixed site, so a mistyped
    // series coefficient shows up here the way a wrong propagator constant
    // shows up above.  They come from an independent implementation of the
    // same theory that was itself checked against published rise/set tables.
    const double TEST_UNIX = 1787053560.0;          // 2026-08-18 11:46:00 UTC
    const double TEST_LAT  = 47.2297 * DEG_TO_RAD;
    const double TEST_LON  = 16.6186 * DEG_TO_RAD;
    const double TEST_ALT  = 0.150;                 // km

    double jd = sgp4::jdFromUnix(TEST_UNIX);

    sgp4::MoonInfo m;
    sgp4::moonInfo(jd, TEST_LAT, TEST_LON, TEST_ALT, m);

    // One wrong digit anywhere in the series moves the moon by far more than
    // these tolerances; they only absorb rounding.
    bool moonOk = fabs(m.azDeg - 129.7644) < 0.01 &&
                  fabs(m.elDeg - 7.8163) < 0.01 &&
                  fabs(m.distanceKm - 394643.56) < 5.0 &&
                  fabs(m.illuminatedFrac - 0.34473) < 0.001 && m.waxing;

    Serial.printf("MOON selftest: az %.4f el %.4f dist %.1f illum %.5f %s - %s\n",
                  m.azDeg, m.elDeg, m.distanceKm, m.illuminatedFrac,
                  m.waxing ? "waxing" : "waning", moonOk ? "PASS" : "FAIL");

    // Rise and set over the same local day, midnight at UTC+2.  dayStart is the
    // unix instant of local 00:00, so a local time of day just adds on.
    const long TEST_SHIFT = 2 * 3600;
    long ln = (long)TEST_UNIX + TEST_SHIFT;
    double dayStart = (double)(ln - ((ln % 86400L) + 86400L) % 86400L - TEST_SHIFT);

    sgp4::RiseSet rsSun, rsMoon;
    sgp4::riseSet(sgp4::SKY_SUN,  dayStart, TEST_LAT, TEST_LON, TEST_ALT, rsSun);
    sgp4::riseSet(sgp4::SKY_MOON, dayStart, TEST_LAT, TEST_LON, TEST_ALT, rsMoon);

    // Expected local 05:53:57, 19:59:55, 12:43:20, 21:59:11.
    double eSunRise  = dayStart +  5 * 3600 + 53 * 60 + 57;
    double eSunSet   = dayStart + 19 * 3600 + 59 * 60 + 55;
    double eMoonRise = dayStart + 12 * 3600 + 43 * 60 + 20;
    double eMoonSet  = dayStart + 21 * 3600 + 59 * 60 + 11;

    bool rsOk = rsSun.riseValid && rsSun.setValid &&
                rsMoon.riseValid && rsMoon.setValid &&
                fabs(rsSun.riseUnix  - eSunRise)  < 30.0 &&
                fabs(rsSun.setUnix   - eSunSet)   < 30.0 &&
                fabs(rsMoon.riseUnix - eMoonRise) < 30.0 &&
                fabs(rsMoon.setUnix  - eMoonSet)  < 30.0;

    Serial.printf("RISE/SET selftest: sun %+.0f/%+.0f s  moon %+.0f/%+.0f s - %s\n",
                  rsSun.riseUnix - eSunRise, rsSun.setUnix - eSunSet,
                  rsMoon.riseUnix - eMoonRise, rsMoon.setUnix - eMoonSet,
                  rsOk ? "PASS" : "FAIL");
}

void satellitesBegin(double latitude, double longitude)
{
    ensureSync();

    g_latDeg = latitude;
    g_lonDeg = longitude;

    memset(g_slots, 0, sizeof(g_slots));

    satellitesSelfTest();
    loadConfig();

    // Low priority on core 0: the pass search is pure arithmetic and must never
    // get in the way of the display or the web server on core 1.
    xTaskCreatePinnedToCore(satPredictTask, "satpredict", 10240, NULL, 1, &g_predictTask, 0);

    requestPrediction();
}

// Free stack left in the prediction task; a number creeping towards zero
// would mean the 10 kB allocation is not enough.
void satellitesReportHealth()
{
    if (!g_predictTask) return;
    Serial.printf("\U0001F6F0\uFE0F  predict task stack head-room: %u bytes, %u pass(es) held\n",
                  (unsigned)uxTaskGetStackHighWaterMark(g_predictTask), g_passCount);
}

double satellitesSiteAltitudeM()
{
    return g_altitudeM;
}

void satellitesSetSite(double latitude, double longitude)
{
    SAT_LOCK();
    bool changed = (fabs(latitude - g_latDeg) > 1e-6) || (fabs(longitude - g_lonDeg) > 1e-6);
    g_latDeg = latitude;
    g_lonDeg = longitude;
    SAT_UNLOCK();
    if (changed) requestPrediction();
}

void satellitesLoop(time_t utcNow)
{
    g_nowUnix = utcNow;

    if (utcNow < 1600000000L) return;         // wait for NTP
    if (WiFi.status() != WL_CONNECTED) return;

    // At most one download every 3 s, so a full list is fetched over ~25 s
    // without hammering Celestrak or blocking the clock for long.
    static uint32_t lastAttemptMs = 0;
    if (lastAttemptMs && (millis() - lastAttemptMs) < 3000) return;

    int due = -1;
    SAT_LOCK();
    uint32_t refreshSecs = (uint32_t)g_tleRefreshHours * 3600u;
    for (uint8_t i = 0; i < g_slotCount; i++) {
        SatSlot &s = g_slots[i];
        if (!s.norad) continue;
        if ((uint32_t)utcNow < s.retryAfterUnix) continue;
        bool stale = !s.tleLoaded || s.fetchedUnix == 0 ||
                     ((uint32_t)utcNow - s.fetchedUnix) > refreshSecs;
        if (stale) { due = i; break; }
    }
    SAT_UNLOCK();

    if (due < 0) return;

    lastAttemptMs = millis();
    fetchTleFor((uint8_t)due, utcNow);
}
