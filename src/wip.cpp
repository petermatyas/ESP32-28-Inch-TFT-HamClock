
#include <TFT_eSPI.h>
#ifdef USE_XPT2046_SPI_TOUCH
#include <XPT2046_Touchscreen.h>
#endif
#include <WiFi.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <TimeLib.h>
#include <HB9IIU7seg42ptItalic.h> // https://rop.nl/truetype2gfx/ https://fontforge.org/en-US/
#include <HB9IIUOrbitronMed8pt.h>
#include <HB9IIOrbitronMed10pt.h>
#include <HB9IIU7seg42ptNormal.h>
#include <HB97DIGITS12pt7b.h>
#include "qrcode.h"
#include <UbuntuMono_Regular8pt7b.h>
#include <JetBrainsMono_Bold15pt7b.h>
#include <JetBrainsMono_Light7pt7b.h>
#include <JetBrainsMono_Bold11pt7b.h>
#include <PNGdec.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <Preferences.h>
#include "html_page.h"
#include "satellites.h"
#include "dxcluster.h"
#include <new>
#include <esp_heap_caps.h>
#include <sgp4.h>
#include "html_success.h"
#include "digits60pt7b.h"

const byte DNS_PORT = 53;
DNSServer dnsServer;
IPAddress apIP(192, 168, 4, 1);

static unsigned long lastDotUpdate = 0;                 // for screen saver
static unsigned long nextDotDelay = random(1000, 2001); // for screen saver
unsigned long currentMillis = millis();
unsigned long lastActivity = 0;                        // Last time user interacted (for screensaver)
unsigned long screenSaverTimeout = 1000 * 60 * 60 * 2; // 120 minute
bool useScreenSaver = false;
// NTPClient::update() returns false both when a request fails and when it was
// simply not due yet, so the only trustworthy signal is a true return.  Track
// when that last happened.
unsigned long lastNtpSyncMs = 0;
bool ntpSyncedAtLeastOnce = false;
int tOffset = 0; // will be updated via configuration device time (Iphone) and later via API call that contains offset according to lat & lon
Preferences prefs;
// --- globals ---
uint8_t activePage = 1;
const uint8_t MAX_PAGES = 12;
unsigned long lastTouchMs = 0;
bool wasTouching = false;
int scanCount = 0;
bool inAPmode = false;
bool autoPageChange = false;
char bigClockLastDigit[4] = {' ', ' ', ' ', ' '};
int8_t bigClockColonState = -1;   // big clock colon: -1 unknown, 0 hidden, 1 shown
bool bigClockShowsUtc = false;    // big clock time base: false = QTH, true = UTC
bool bigClockLabelDirty = true;

// The QTH/UTC label, sat at the bottom of the screen.  The digits end at
// y=175 and the screen at 240, so 208..236 leaves a small bottom margin.
const int BIGCLOCK_BADGE_X = 118;
const int BIGCLOCK_BADGE_Y = 208;
const int BIGCLOCK_BADGE_W = 84;
const int BIGCLOCK_BADGE_H = 28;
// Struct to store all parsed solar data
struct SolarData
{
    String source;
    String updated;
    int solarFlux;
    int aIndex;
    int kIndex;
    String kIndexNT;
    String xRay;
    int sunspots;
    float heliumLine;
    String protonFlux;
    String electronFlux;
    int aurora;
    float normalization;
    float latDegree;
    float solarWind;
    float magneticField;
    String geomagneticField;
    String signalNoise;
    String fof2;
    String mufFactor;
    String muf;

    struct BandCondition
    {
        String name;
        String time;
        String condition;
    } bandConditions[8];

    struct VHFCondition
    {
        String name;
        String location;
        String condition;
    } vhfConditions[5];
};

SolarData solarData;

// Create web server
WebServer server(80); // HTTP server on port 80

// Configurable Settings (replace all previous #defines)
bool weatherFetchOk = false;   // last Open-Meteo fetch succeeded
// How often to ask Open-Meteo, in minutes.  Configurable at /weather.html; the
// floor keeps the clock a polite client of a free service.
uint8_t weatherIntervalMin = 10;
static const uint8_t WEATHER_INTERVAL_MIN_LIMIT = 5;
static const uint8_t WEATHER_INTERVAL_MAX_LIMIT = 120;

static unsigned long weatherIntervalMs()
{
    return (unsigned long)weatherIntervalMin * 60UL * 1000UL;
}
float latitude = 46.4667118;
float longitude = 6.8590456;
uint16_t localTimeColour = TFT_GREEN;
uint16_t utcTimeColour = TFT_GOLD;
bool doubleFrame = false;
uint16_t localFrameColour = TFT_DARKGREY;
uint16_t utcFrameColour = TFT_DARKGREY;
uint16_t bannerColour = TFT_DARKGREEN;
uint16_t bigClockColour = TFT_GREEN;

int bannerSpeed = 5;
String localTimeLabel = "  QTH Time  ";
String utcTimeLabel = "  UTC Time  ";
String startupLogo = "logo3.png";
bool italicClockFonts = false;

volatile bool refreshDigits = false;
// Open-Meteo needs no API key and lets the query name exactly the fields the
// clock shows, so today's weather arrives in about 1.1 kB.  Its "current" block
// steps every 15 minutes, and it reports the location's UTC offset in the same
// reply - the clock's local time comes from there.
const String openMeteoBase = "https://api.open-meteo.com/v1/forecast";
// Open-Meteo has no reverse geocoder; this one needs no key either.  It is only
// asked once per boot, to put a place name above the readings.
const String reverseGeocodeBase = "https://api.bigdatacloud.net/data/reverse-geocode-client";

// Global variables for previous time tracking
String previousLocalTime = "";
String previousUTCtime = "";
int refreshDigitsCounter = 0;
volatile bool refreshFrames = false;
int refreshFramesCounter = 0;

// for solar data to be cleaned
const char *solarDataUrl = "https://www.hamqsl.com/solarxml.php";
String formatUpdatedTimestampToUTC(const String &raw);

String LOCALlastTimeStr = "        "; // 8 characters: HH:MM:SS
String UTClastTimeStr = "        ";   // 8 characters: HH:MM:SS
String LASTbigClockTimeStr = "";
uint16_t LOCALdigitColor = TFT_LIGHTGREY;
uint16_t UTCdigitColor = TFT_LIGHTGREY;
bool blinkingDot = false; // colons on Propagation page clocks
bool colonVisible = true; // global var used to show/hide colons on Propagation page clocks
bool redrawMainPropagationPage = true;
bool redrawSolarSummaryPage1 = true;
bool redrawSolarSummaryPage2 = true;
bool redrawSolarSummaryPage3 = true;
bool reDrawWiFiQualityPage = true;
bool redrawSatellitePage = true;
bool redrawWeatherPage = true;
bool redrawBeaconPage = true;
bool redrawSunMoonPage = true;
bool redrawDxPage = true;

// fetchWeatherData() used to parse the OpenWeather response into locals and
// throw everything away except the scrolling banner; the weather page needs the
// values to stick around.
struct WeatherInfo
{
    bool  valid = false;
    char  city[32] = {0};
    char  country[16] = {0};
    char  description[40] = {0};
    float temp = 0, feelsLike = 0;
    float tempMin = 0, tempMax = 0;   // today's range
    bool  dailyValid = false;
    int   pressure = 0, humidity = 0, clouds = 0;
    float visibilityKm = 0, windSpeed = 0, windGust = 0, rainMM = 0;
    int   windDeg = 0;
    char  windDir[6] = {0};           // compass point derived from windDeg
    char  sunrise[8] = {0};           // "06:06" local
    char  sunset[8] = {0};
    long  fetchedUnix = 0;
};
WeatherInfo weather;


// Relative x-offsets for HB97DIGITS12pt7b font layout
const int xOffsets[8] = {
    0,  // H1
    15, // H2
    30, // :
    36, // M1
    51, // M2
    66, // :
    72, // S1
    87  // S2
};

// TFT Display Setup
TFT_eSPI tft = TFT_eSPI();                     // Create TFT display object
TFT_eSprite scrollingText = TFT_eSprite(&tft); // Sprite object for "Hello World" text

TFT_eSprite labelSprite = TFT_eSprite(&tft); // Global sprite

// ---------------------------------------------------------------------------
// Touch input
// ---------------------------------------------------------------------------
// On boards like the CYD (ESP32-2432S028R) the XPT2046 touch controller sits on
// its own SPI bus, so TFT_eSPI's shared-bus getTouch() can never reach it and
// always reports "not touched" (or worse, noise from a floating MISO line).
// When USE_XPT2046_SPI_TOUCH is defined we drive the controller directly on a
// second SPI port; otherwise we keep the original shared-bus behaviour.
// Every touch call site goes through readTouchPoint() below.
#ifdef USE_XPT2046_SPI_TOUCH

// TFT_eSPI uses VSPI (no USE_HSPI_PORT), so the touch panel gets HSPI.
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touchPanel(TOUCH_XPT_CS, TOUCH_XPT_IRQ);

static void initTouch()
{
    touchSPI.begin(TOUCH_XPT_SCLK, TOUCH_XPT_MISO, TOUCH_XPT_MOSI, TOUCH_XPT_CS);
    touchPanel.begin(touchSPI);
    touchPanel.setRotation(TOUCH_ROTATION);
    Serial.println("XPT2046 touch initialized on its own SPI bus.");
}

// Returns screen coordinates in the display's current rotation, matching what
// tft.getTouch() would have produced on a shared-bus panel.
static bool readTouchPoint(uint16_t *x, uint16_t *y)
{
    if (!touchPanel.touched())
        return false;

    TS_Point p = touchPanel.getPoint();

    long mx = map(p.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, tft.width() - 1);
    long my = map(p.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, tft.height() - 1);

    *x = (uint16_t)constrain(mx, 0, tft.width() - 1);
    *y = (uint16_t)constrain(my, 0, tft.height() - 1);

#ifdef TOUCH_DEBUG
    Serial.printf("touch raw=%d,%d z=%d -> screen=%u,%u\n", p.x, p.y, p.z, *x, *y);
#endif
    return true;
}

#else // shared-bus touch handled by TFT_eSPI itself

static void initTouch() {}

static bool readTouchPoint(uint16_t *x, uint16_t *y)
{
    return tft.getTouch(x, y);
}

#endif

// Scrolling Text
int scrollingTextXposition;                                                                                                                        // Variable for text position (to start at the rightmost side)
String scrollText = "Sorry, No Weather Info At This Moment!!!    Have you enterred your API key via the Web Interface at http://hamclock.local ?"; // Text to scroll
// Timing variables
unsigned long previousMillisForScroller = 0; // Store last time the action was performed

// NTP Client Setup
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 15000); // UTC offset and update interval

// Function Prototypes
void saveSettings();
void drawQRCode(const char *text, int x, int y, int scale);
void drawQRcodeInstructions();
void startConfigurationPortal();
bool tryToConnectSavedWiFi();
// Saved WiFi list: defined next to tryToConnectSavedWiFi(), used from
// setup() and the captive portal handlers above it.
static void wifiRegisterRoutes();
static uint8_t wifiNetCount();
static bool wifiNetAdd(const String &ssid, const String &pass);
void fetchWeatherData();
String formatLocalTime(long epochTime);
String convertEpochToTimeString(long epochTime);
void displayTime(int x, int y, String time, String &previousTime, int yOffset, uint16_t fontColor);
String convertTimestampToDate(long timestamp);
void loadSettings();
void handleRoot();
void handleSave();
void drawOrredrawStaticElements();
void mountAndListSPIFFS(uint8_t levels = 255, bool listContent = true);
void handlePNGUpload();
void handleTouchToRotatePage();
void drawMainPropagationPage();
void drawSolarSummaryPage1();
void drawSolarSummaryPage2();
void drawSolarSummaryPage3();
void drawWiFiQualityPage();
void drawBigClockModeBadge();
static void maidenhead(double latDeg, double lonDeg, char *out, size_t n);
bool ntpTick();
void drawNtpStatus();
void drawWeatherPage();
void drawBeaconPage(bool fullRedraw);
void drawSunMoonPage(bool fullRedraw);
void updateWeatherPageClock();
void redrawdrawMainPropagationPagePage1();
void fetchSolarData();
void drawLOCALTime(const String &timeStr, int x, int y, uint16_t digitColor, uint16_t backgroundColor, bool blinkColon);
void drawUTCTime(const String &timeStr, int x, int y, uint16_t digitColor, uint16_t backgroundColor, bool blinkColon);
void updateWiFiSignalDisplay();
void drawWiFiSignalMeter(int qualityPercent);
void handleRootCaptivePortal();
void handleScanCaptivePortal();
void handleSaveCaptivePortal();
void startConfigurationPortal();
void checkIfscreenIsTouchedDuringStartUpForFactoryReset();
void tryToRetrieveUTCoffsetFromFirstConfiguration();
// PNG Decoder Setup
PNG png;
fs::File pngFile; // Global File handle (required for PNGdec callbacks)

// Callback functions for PNGdec
void *fileOpen(const char *filename, int32_t *size);
void fileClose(void *handle);
int32_t fileRead(PNGFILE *handle, uint8_t *buffer, int32_t length);
int32_t fileSeek(PNGFILE *handle, int32_t position);
void displayPNGfromSPIFFS(const char *filename, int duration_ms);

// When operator new cannot allocate it throws, and with no handler in the app
// that becomes abort() - the device reboots and the backtrace says nothing
// about how much was being asked for or which pool ran dry.  This runs first.
//
// MALLOC_CAP_8BIT is the pool new/malloc actually draw byte-addressable memory
// from; ESP.getFreeHeap() also counts IRAM and reads healthy while this one is
// empty, which is exactly how an earlier reboot loop stayed hidden.
static void reportAllocationFailure()
{
    Serial.println();
    Serial.println("!!! ALLOCATION FAILED - heap at the moment of failure:");
    Serial.printf("    8-bit    free %7u  largest %7u\n",
                  heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("    internal free %7u  largest %7u\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    Serial.printf("    lowest 8-bit free ever %u\n",
                  heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    Serial.flush();

    Serial.println("    checking heap integrity...");
    bool intact = heap_caps_check_integrity_all(true);
    Serial.printf("    heap integrity: %s\n", intact ? "OK" : "CORRUPT");
    Serial.flush();

    // Throw rather than abort, so a caller that guards its parse can carry on.
    throw std::bad_alloc();
}

// The ESP32 records why it last reset; printing it in words turns "it
// rebooted" into an actual diagnosis.
static const char *resetReasonName()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC - exception or abort()";
    case ESP_RST_INT_WDT:   return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT - supply voltage dipped";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
    }
}

// Refresh interval, read and written by /weather.html.
static void weatherRegisterRoutes()
{
    server.on("/weathercfg", HTTP_GET, []()
              {
        JsonDocument doc;
        doc["intervalMin"] = weatherIntervalMin;
        doc["minMin"] = WEATHER_INTERVAL_MIN_LIMIT;
        doc["maxMin"] = WEATHER_INTERVAL_MAX_LIMIT;
        doc["ok"] = weatherFetchOk;
        doc["city"] = weather.city;
        doc["country"] = weather.country;
        doc["description"] = weather.description;
        doc["temp"] = weather.temp;
        doc["feelsLike"] = weather.feelsLike;
        doc["tempMin"] = weather.tempMin;
        doc["tempMax"] = weather.tempMax;
        doc["humidity"] = weather.humidity;
        doc["clouds"] = weather.clouds;
        doc["pressure"] = weather.pressure;
        doc["windGust"] = weather.windGust;
        doc["visibilityKm"] = weather.visibilityKm;
        doc["windSpeed"] = weather.windSpeed;
        doc["windDir"] = weather.windDir;
        doc["rainMM"] = weather.rainMM;
        doc["sunrise"] = weather.sunrise;
        doc["sunset"] = weather.sunset;
        doc["fetchedUnix"] = (uint32_t)weather.fetchedUnix;
        doc["utc"] = (uint32_t)timeClient.getEpochTime();
        doc["tOffset"] = tOffset;
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out); });

    server.on("/weathercfg", HTTP_POST, []()
              {
        JsonDocument doc;
        if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        int mins = doc["intervalMin"] | (int)weatherIntervalMin;
        if (mins < WEATHER_INTERVAL_MIN_LIMIT) mins = WEATHER_INTERVAL_MIN_LIMIT;
        if (mins > WEATHER_INTERVAL_MAX_LIMIT) mins = WEATHER_INTERVAL_MAX_LIMIT;
        weatherIntervalMin = (uint8_t)mins;
        saveSettings();
        Serial.printf("Weather refresh interval set to %u min\n", weatherIntervalMin);
        server.send(200, "application/json", "{\"status\":\"ok\"}"); });

    server.on("/weathernow", HTTP_GET, []()
              {
        server.send(200, "application/json", "{\"status\":\"fetching\"}");
        fetchWeatherData(); });
}

void setup()
{

    // Start Serial Monitor
    Serial.begin(115200);
    Serial.println("Starting setup...");
    std::set_new_handler(reportAllocationFailure);
    Serial.printf("\U0001FA7A Last reset reason: %s\n", resetReasonName());

    // Backlight pin setup
    pinMode(TFT_BLP, OUTPUT);
    digitalWrite(TFT_BLP, HIGH); // Turn backlight ON permanently

    //   Initialize TFT display
    tft.init();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);
    Serial.println("TFT Display initialized!");
    tft.setFreeFont(&digits60pt7b);

    initTouch();

    checkIfscreenIsTouchedDuringStartUpForFactoryReset();
    // 🔧 Mount SPIFFS
    mountAndListSPIFFS();
    // Load saved settings first
    loadSettings();
    // Display PNG from SPIFFS
    displayPNGfromSPIFFS(startupLogo.c_str(), 0);
    // BETA release display
    //tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    //tft.setTextColor(TFT_GREEN);
    //tft.drawCentreString("Beta Pre-Release", 160, 210, 1);


    labelSprite.setColorDepth(8);
    labelSprite.createSprite(120, 30); // Size depends on font & text
    labelSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    labelSprite.setTextDatum(MC_DATUM);
    labelSprite.setFreeFont(&FreeSansBold12pt7b);

    if (!tryToConnectSavedWiFi())
    {
        inAPmode = true;
        
        startConfigurationPortal();
    }

    if (inAPmode == false)
    {
        // Start mDNS
        if (!MDNS.begin("hamclock"))
        {
            Serial.println("⚠️ Failed to start mDNS responder!");
        }
        else
        {
            Serial.println("🌍 mDNS started successfully. You can access via http://hamclock.local");
        }

        {
            // Start Web Server
            server.on("/", handleRoot);                     // Serve the HTML page
                                                            // Serve all static files (HTML, PNG, CSS, etc.)
            server.serveStatic("/fonts", SPIFFS, "/fonts"); // optional
            server.serveStatic("/logo1.png", SPIFFS, "/logo1.png");
            server.serveStatic("/logo2.png", SPIFFS, "/logo2.png");
            server.serveStatic("/logo3.png", SPIFFS, "/logo3.png");
            server.serveStatic("/logo4.png", SPIFFS, "/logo4.png");
            server.serveStatic("/logo4.png", SPIFFS, "/logo4.png");
            server.serveStatic("/github.png", SPIFFS, "/github.png");
            server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");
            // Shared theme, navigation and the pages added since.
            server.serveStatic("/hamclock.css", SPIFFS, "/hamclock.css");
            server.serveStatic("/hamclock.js", SPIFFS, "/hamclock.js");
            server.serveStatic("/wifi.html", SPIFFS, "/wifi.html");
            server.serveStatic("/weather.html", SPIFFS, "/weather.html");
            wifiRegisterRoutes();
            weatherRegisterRoutes();
            server.on("/config", HTTP_GET, []()
                      {
  JsonDocument doc;

  doc["latitude"] = latitude;
  doc["longitude"] = longitude;
  doc["localTimeColour"] = localTimeColour;
  doc["utcTimeColour"] = utcTimeColour;
  doc["doubleFrame"] = doubleFrame;
  doc["localFrameColour"] = localFrameColour;
  doc["utcFrameColour"] = utcFrameColour;
  doc["bannerColour"] = bannerColour;
  doc["bannerSpeed"] = bannerSpeed;
  doc["localTimeLabel"] = localTimeLabel;
  doc["utcTimeLabel"] = utcTimeLabel;
  doc["startupLogo"] = startupLogo;
  doc["italicClockFonts"] = italicClockFonts;
doc["screenSaverTimeout"] = screenSaverTimeout / 60000;  // convert ms → minutes
doc ["autoPageChange"] =autoPageChange;
doc ["bigClockShowsUtc"] = bigClockShowsUtc;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response); });

            server.on("/scrolltext", []()
                      { server.send(200, "text/plain", scrollText); });

            server.on("/setcolor", HTTP_POST, []()
                      {


   JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }

    String target = doc["target"];

    // ✅ Handle doubleFrame checkbox
    if (target == "doubleFrame") {
        bool thinBorder = doc["value"];
        doubleFrame = !thinBorder; // Inverse logic
        Serial.printf("🪟 doubleFrame set to: %s (thinBorder: %s)\n", doubleFrame ? "true" : "false", thinBorder ? "true" : "false");
        server.send(200, "text/plain", "OK");
        return;
    }

    // ✅ All other color-based updates
    uint16_t color = doc["color"];

    if (target == "localTimeDigits") {
        localTimeColour = color;
        Serial.printf("🎨 localTimeDigits set to: 0x%04X\n", color);
    } else if (target == "localTimeFrame") {
        localFrameColour = color;
        Serial.printf("🖼️ localTimeFrame set to: 0x%04X\n", color);
    } else if (target == "utcTimeDigits") {
        utcTimeColour = color;
        Serial.printf("🎨 utcTimeDigits set to: 0x%04X\n", color);
    } else if (target == "utcTimeFrame") {
        utcFrameColour = color;
        Serial.printf("🖼️ utcTimeFrame set to: 0x%04X\n", color);
    } else if (target == "weatherBannerText") {
        bannerColour = color;
        Serial.printf("🟩 bannerColour set to: 0x%04X\n", color);
    }      else if (target == "bigClockTime") {
        bigClockColour = color;
        LASTbigClockTimeStr="";
for (int i = 0; i < 4; i++) {
    bigClockLastDigit[i] = ' '; 
}        Serial.printf("🟩 big clock color set to: 0x%04X\n", color); }
    
    else {
        Serial.printf("⚠️ Unknown target: %s\n", target.c_str());
        server.send(400, "text/plain", "Unknown target");
        return;
    }
    saveSettings();
    if (activePage==1){
    drawOrredrawStaticElements();
    }
    refreshDigits = true;
    server.send(200, "text/plain", "OK"); });

            server.on("/setspeed", HTTP_POST, []()
                      {

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }

    int speed = doc["speed"];  // This will already be 45 - slider
    bannerSpeed = constrain(speed, 0, 45);
    Serial.printf("🎬 bannerSpeed set to %d seconds\n", bannerSpeed);
     saveSettings();
    server.send(200, "text/plain", "OK"); });

            // ESP32 WebServer endpoint for setting labels without saving
            server.on("/setlabel", HTTP_POST, []()
                      {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }

    String target = doc["target"];
    String value = doc["value"];

    if (target == "localTimeLabel") {
 localTimeLabel = "  " + value + "  ";
        Serial.printf("🕒 Updated localTimeLabel: %s\n", localTimeLabel.c_str());
    } else if (target == "utcTimeLabel") {
        utcTimeLabel = "  " + value + "  ";;
        Serial.printf("🌐 Updated utcTimeLabel: %s\n", utcTimeLabel.c_str());
    } else {
        server.send(400, "text/plain", "Unknown target");
        return;
    }

    // Redraw labels immediately on screen
    refreshFrames = true;
    drawOrredrawStaticElements();

    server.send(200, "text/plain", "OK"); });

            server.on("/setposition", HTTP_POST, []()
                      {


    String body = server.arg("plain");
    Serial.println("📩 Received JSON:");
    Serial.println(body);

    JsonDocument doc;  // enough for two numbers
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
        Serial.println("❌ JSON parse error");
        server.send(400, "text/plain", "JSON parse error");
        return;
    }

    // parse as string -> float
    latitude  = String(doc["latitude"].as<const char*>()).toFloat();
    longitude = String(doc["longitude"].as<const char*>()).toFloat();

    Serial.printf("📍 Latitude updated to: %.6f\n", latitude);
    Serial.printf("📍 Longitude updated to: %.6f\n", longitude);
    saveSettings();
    // The locator on the QTH frame moves with the position.  Only repaint when
    // that page is the one on screen - the redraw clears the whole display.
    if (activePage == 1) drawOrredrawStaticElements();
    weather.city[0] = 0;      // re-resolve the place name for the new position
    fetchWeatherData();
    server.send(200, "text/plain", "OK"); });

            server.on("/setitalic", HTTP_POST, []()
                      {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }
    italicClockFonts = doc["italicClockFonts"] | italicClockFonts;
    saveSettings();
    Serial.printf("✏️ italicClockFonts set to: %s\n", italicClockFonts ? "true" : "false");

    drawOrredrawStaticElements();

    server.send(200, "text/plain", "OK"); });

            server.on("/saveall", HTTP_POST, []()
                      {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "❌ Missing JSON body");
        Serial.println("❌ No JSON payload received!");
        return;
    }

    String json = server.arg("plain");
    Serial.println("\n📨 Received JSON from webpage:");
    Serial.println(json);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    // 🔧 Apply settings directly to global variables (not config struct!)
    latitude             = doc["latitude"] | latitude;
    longitude            = doc["longitude"] | longitude;
    localTimeLabel       = doc["localTimeLabel"] | localTimeLabel;
    utcTimeLabel         = doc["utcTimeLabel"] | utcTimeLabel;
    italicClockFonts     = doc["italicClockFonts"] | italicClockFonts;
    doubleFrame          = doc["doubleFrame"] | doubleFrame;
    bannerSpeed          = doc["bannerSpeed"] | bannerSpeed;
    screenSaverTimeout   = doc["screenSaverTimeout"] | screenSaverTimeout;

    // 📋 Debug printout of applied values
    Serial.println("📋 Parsed and applied config:");
    Serial.println("──────────────────────────────────────────────");
    Serial.printf("📍 Latitude             : %.6f\n", latitude);
    Serial.printf("📍 Longitude            : %.6f\n", longitude);
    Serial.printf("🕒 Local Time Label     : %s\n", localTimeLabel.c_str());
    Serial.printf("🕒 UTC Time Label       : %s\n", utcTimeLabel.c_str());
    Serial.printf("✍️  Italic Fonts         : %s\n", italicClockFonts ? "true" : "false");
    Serial.printf("🖼️  Double Frame         : %s\n", doubleFrame ? "true" : "false");
    Serial.printf("🏃 Banner Speed         : %d\n", bannerSpeed);
    Serial.printf("💤 ScreenSaver Timeout  : %lu ms (%.2f min)\n",
                  screenSaverTimeout,
                  screenSaverTimeout / 60000.0);
    Serial.println("──────────────────────────────────────────────");

    // 💾 Save settings to SPIFFS (your version will do the actual work)
    saveSettings();
    Serial.println("✅ Settings saved to flash.");

    server.send(200, "text/plain", "💾 Settings saved to flash");
    esp_restart(); });

            server.on("/setbootimage", HTTP_POST, []()
                      {
                  if (!server.hasArg("plain"))
                  {
                      server.send(400, "text/plain", "Missing body");
                      return;
                  }

                  JsonDocument doc;
                  DeserializationError error = deserializeJson(doc, server.arg("plain"));
                  if (error)
                  {
                      server.send(400, "text/plain", "JSON parse error");
                      return;
                  }

          if (!doc["bootImageId"].is<const char*>()) {
    server.send(400, "text/plain", "Missing bootImageId");
    return;
}

                  startupLogo = doc["bootImageId"].as<String>();
                  Serial.printf("🖼️ Boot logo updated to: %s\n", startupLogo.c_str());
                  saveSettings(); // 💾 Persist the change

                  server.send(200, "text/plain", "Boot logo saved");
                esp_restart(); });

            server.on("/setbootimage", HTTP_POST, []()
                      {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }

   if (!doc["bootImageId"].is<const char*>()) {
    server.send(400, "text/plain", "Missing bootImageId");
    return;
}

    startupLogo = doc["bootImageId"].as<String>();
    Serial.printf("🖼️ Boot logo updated to: %s\n", startupLogo.c_str());

    saveSettings(); // 💾 Persist the change

    server.send(200, "text/plain", "Boot logo saved"); });

            server.on("/ping", HTTP_GET, []()
                      { server.send(200, "text/plain", "pong"); });

            server.on("/scrolltext", HTTP_GET, []()
                      { server.send(200, "text/plain", scrollText); });
            server.on("/uploadpng", HTTP_POST, []()
                      {
                          // ✅ no early response here
                      },
                      handlePNGUpload);

            server.on("/setScreenSaverTime", HTTP_POST, []()
                      {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      screenSaverTimeout = doc["screenSaverTimeout"] | 0;
      saveSettings();
      Serial.printf("🖥️ Screen Saver Timeout set to %lu ms (%lu minutes)\n",
                    screenSaverTimeout, screenSaverTimeout / 60000);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"bad request\"}"); });



            server.on("/setAutoPage", HTTP_GET, []()
                      {
                          if (server.hasArg("enabled"))
                          {
                              autoPageChange = (server.arg("enabled") == "true");
                          }
                          server.send(200, "text/plain", autoPageChange ? "AutoPage ON" : "AutoPage OFF");
                          saveSettings();

                          activePage = 1;
                          drawOrredrawStaticElements(); // 🖼️ Redraw Big Clock frames
                      });

            satellitesRegisterRoutes(server);
            dxClusterRegisterRoutes(server);

            server.begin();

            // Initialize NTP Client
            timeClient.begin();

            // Wait until time is valid
            while (!ntpTick())
            {
                delay(500);
            }
            tryToRetrieveUTCoffsetFromFirstConfiguration();

            fetchWeatherData();
            fetchSolarData();
            satellitesBegin(latitude, longitude);
            dxClusterBegin();
            drawOrredrawStaticElements();

            scrollingText.setColorDepth(8);
            scrollingText.createSprite(320, 30);      // Create a 310x20 sprite to accommodate the text width
            scrollingText.setTextColor(bannerColour); // White text
            scrollingText.setTextDatum(TL_DATUM);     // Top-left alignment for text

            // Set the font for the sprite
            scrollingText.setFreeFont(&Orbitron_Medium10pt7b);

            // Calculate the initial position (rightmost position)
            scrollingTextXposition = scrollingText.width();
        }
    }
}

void loop()
{
    server.handleClient();
    if (inAPmode == true)
    {
        dnsServer.processNextRequest(); // <- IMPORTANT
        return;
    }

    unsigned long currentMillis = millis();
    static unsigned long previousMillisForHealth = 0;
    static unsigned long previousMillisForWeatherDataUpdate = 0;
    static unsigned long previousMillisForPropagationDataUpdate = 0;
    // Half-length first interval, so the solar fetch settles out of step with
    // the weather one and their peak allocations never add up.
    static unsigned long propagationInterval = 150000UL;
    static unsigned long previousMillisForLargeClockUpdate = 0;
    static unsigned long previousMillisForPropagationClockUpdate = 0;
    static unsigned long previousMillisForWiFiPageUpdate = 0;
    static unsigned long previousMillisForWeatherPageUpdate = 0;
    static unsigned long previousMillisForScroller = 0;
    static unsigned long previousMillisForAutoPageChanger = 0;
    static unsigned long previousMillisForBlinkDotsOnBigClock = 0;

    static unsigned long lastDotUpdate = 0;
    static bool screenSaver = false;

    // 🛰️ Keeps the element sets fresh and hands the current time to the
    // prediction task.  Rate-limits itself, so calling it every pass is fine.
    satellitesLoop((time_t)timeClient.getEpochTime());

    // A heap that only ever shrinks points at a leak; a largest-block that
    // shrinks faster than the total points at fragmentation.  MALLOC_CAP_8BIT
    // is the pool new/malloc actually draw from - ESP.getFreeHeap() also counts
    // IRAM and reads healthy while this one is empty.
    if (currentMillis - previousMillisForHealth >= 30000UL)
    {
        previousMillisForHealth = currentMillis;
        Serial.printf("\U0001FA7A HEALTH up=%lus 8bit=%u min8=%u largest8=%u stack=%u page=%u\n",
                      millis() / 1000UL,
                      heap_caps_get_free_size(MALLOC_CAP_8BIT),
                      heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned)uxTaskGetStackHighWaterMark(NULL),
                      activePage);
        satellitesReportHealth();
        dxClusterReportHealth();
        if (!heap_caps_check_integrity_all(false))
            Serial.println("!!! HEAP CORRUPTION DETECTED by the periodic check");
    }

    // Keep the clock disciplined on every page.  Previously only pages 1, 2 and
    // 7 polled, so the other pages let the clock free-run.  NTPClient rate-limits
    // itself to its update interval, so calling this every pass is cheap.
    ntpTick();

    if (!screenSaver)
    {
        handleTouchToRotatePage();
    }

    // 🌤️ Weather from Open-Meteo, on the interval set in the web interface.
    if (currentMillis - previousMillisForWeatherDataUpdate >= weatherIntervalMs())
    {
        previousMillisForWeatherDataUpdate = currentMillis;
        Serial.printf("Getting fresh weather data (every %u min)\n", weatherIntervalMin);
        fetchWeatherData();
    }

    // 🌤️ Refresh propagation data every 5 minutes
    if (currentMillis - previousMillisForPropagationDataUpdate >= propagationInterval)
    {
        previousMillisForPropagationDataUpdate = currentMillis;
        propagationInterval = 1000UL * 60 * 5;
        Serial.println("Getting fresh propagation data");
        fetchSolarData();
        redrawMainPropagationPage = true;
        redrawSolarSummaryPage1 = true;
        redrawSolarSummaryPage2 = true;
        redrawSolarSummaryPage3 = true;
    }

    // Check for inactivity → Enable screensaver
    if (!screenSaver && currentMillis - lastActivity > screenSaverTimeout)
    {
        if (screenSaverTimeout != 7200000)
        {
            screenSaver = true;
            Serial.println("⏳ Inactivity detected — entering screensaver.");
        }
    }

    // 💤 Screensaver Mode
    if (screenSaver)
    {
        // 🌈 Refresh random pixel animation every 1 second
        if (currentMillis - lastDotUpdate >= 1000)
        {
            tft.fillScreen(TFT_BLACK);

            for (int i = 0; i < 200; i++)
            {
                int x = random(0, 320);
                int y = random(0, 240);
                uint16_t color = tft.color565(random(256), random(256), random(256));
                tft.drawPixel(x, y, color);
            }

            lastDotUpdate = currentMillis;
        }
        uint16_t x, y;
        if (readTouchPoint(&x, &y))
        {
            Serial.println("🖐 Touch detected — exiting screensaver.");
            screenSaver = false;
            tft.fillScreen(TFT_BLACK);
            activePage = 1;
            Serial.println("📄 Active page -> 1");
            drawOrredrawStaticElements(); // 🖼️ Redraw Big Clock frames
            lastActivity = currentMillis; // 🔄 Reset inactivity timer
        }
        return;
    }
    // 📺 Normal Mode
    else
        switch (activePage)
        {
        case 1:
            if (currentMillis - previousMillisForLargeClockUpdate >= 1000) // to not overflow
            {
                previousMillisForLargeClockUpdate = currentMillis;
                UTClastTimeStr = "        ";
                LOCALlastTimeStr = "        ";

                // 🕒 Update time display
                ntpTick();
                long localEpoch = timeClient.getEpochTime() + (tOffset * 3600);
                String localTime = formatLocalTime(localEpoch);
                String utcTime = timeClient.getFormattedTime();

                tft.setTextColor(TFT_WHITE);
                tft.setFreeFont(italicClockFonts ? &digital_7_monoitalic42pt7b : &digital_7__mono_42pt7b);
                displayTime(8, 5, localTime, previousLocalTime, 0, localTimeColour);
                displayTime(10, 107, utcTime, previousUTCtime, 0, utcTimeColour);
            }
            // 📰 Scroll banner text
            if (currentMillis - previousMillisForScroller >= bannerSpeed)
            {
                previousMillisForScroller = currentMillis;
                scrollingText.fillSprite(TFT_BLACK);
                scrollingText.setTextColor(bannerColour);
                scrollingText.drawString(scrollText, scrollingTextXposition, 0);
                scrollingTextXposition -= 1;
                if (scrollingTextXposition < -scrollingText.textWidth(scrollText))
                    scrollingTextXposition = scrollingText.width();
                scrollingText.pushSprite(5, 205);
            }
            break;

        case 2:

            if (redrawMainPropagationPage == true)
            {
                Serial.println("Displaying Main Propagation Page");
                drawMainPropagationPage();
                redrawMainPropagationPage = false;
                LOCALlastTimeStr = "        "; // 8 characters: HH:MM:SS
                UTClastTimeStr = "        ";   // 8 characters: HH:MM:SS
            }

            if (currentMillis - previousMillisForPropagationClockUpdate >= 1000) // to not overflow
            {
                previousMillisForPropagationClockUpdate = currentMillis;

                colonVisible = !colonVisible;
                ntpTick();
                long localEpoch = timeClient.getEpochTime() + (tOffset * 3600);
                String localTime = formatLocalTime(localEpoch);
                String utcTime = timeClient.getFormattedTime();

                drawLOCALTime(String(localTime), 30, 205, LOCALdigitColor, TFT_BLACK, blinkingDot);
                drawUTCTime(String(utcTime), 30 + 160, 205, UTCdigitColor, TFT_BLACK, blinkingDot);
            }
            break;

        case 3:
            if (redrawSolarSummaryPage1 == true)
            {
                Serial.println("Displaying Propagation Page 2");
                redrawSolarSummaryPage1 = false;
                drawSolarSummaryPage1();
            }

            break;

        case 4:
            if (redrawSolarSummaryPage2 == true)
            {
                Serial.println("Displaying Propagation Page 2");
                redrawSolarSummaryPage2 = false;
                drawSolarSummaryPage2();
            }

            break;

        case 5:
            if (redrawSolarSummaryPage3 == true)
            {
                Serial.println("Displaying Propagation Page 2");
                redrawSolarSummaryPage3 = false;
                drawSolarSummaryPage3();
            }

            break;
        case 6:
            if (reDrawWiFiQualityPage == true)
            {
                Serial.println("Displaying Wifi Quality Page");
                reDrawWiFiQualityPage = false;

                drawWiFiQualityPage();
            }
            if (currentMillis - previousMillisForWiFiPageUpdate >= 1000) // to not overflow
            {
                updateWiFiSignalDisplay();
                previousMillisForWiFiPageUpdate = currentMillis;
            }
            break;
        case 7:
        {
            currentMillis = millis();
            tft.setFreeFont(&digits60pt7b);

            // One second on, one second off.  Driven by the parity of the clock's
            // own seconds rather than a millis() timer, so the blink stays in
            // step with the time on screen instead of drifting against it.
            {
                int8_t wantColon = (timeClient.getEpochTime() % 2 == 0) ? 1 : 0;
                if (wantColon != bigClockColonState)
                {
                    bigClockColonState = wantColon;
                    tft.setFreeFont(&digits60pt7b);
                    tft.setTextColor(wantColon ? bigClockColour : TFT_BLACK);
                    tft.drawString(":", 151, 65, 1);
                }
            }
            if (bigClockLabelDirty)
            {
                drawBigClockModeBadge();
                bigClockLabelDirty = false;
            }

            long shownEpoch = timeClient.getEpochTime() +
                              (bigClockShowsUtc ? 0L : (long)tOffset * 3600L);

            struct tm *ptm = gmtime((time_t *)&shownEpoch);

            String localTime = String(ptm->tm_hour < 10 ? "0" : "") + String(ptm->tm_hour) + ":" +
                               String(ptm->tm_min < 10 ? "0" : "") + String(ptm->tm_min);

            // Positions for HH:MM digits
            const int digitX[4] = {5, 78, 180, 253}; // x positions for H1, H2, M1, M2
            const int digitY = 65;                   // same Y for all digits

            if (localTime != LASTbigClockTimeStr)
            {
                // Set the font at the point of use: the badge above draws with
                // its own font and anything else added here would too.
                tft.setFreeFont(&digits60pt7b);

                // Break current time "HH:MM" into 4 chars
                char currentDigits[4];
                currentDigits[0] = localTime.charAt(0); // H tens
                currentDigits[1] = localTime.charAt(1); // H ones
                currentDigits[2] = localTime.charAt(3); // M tens
                currentDigits[3] = localTime.charAt(4); // M ones

                // Compare digit by digit
                for (int i = 0; i < 4; i++)
                {
                    if (currentDigits[i] != bigClockLastDigit[i])
                    {
                        // Erase old digit
                        tft.setTextColor(TFT_BLACK);
                        tft.drawString(String(bigClockLastDigit[i]), digitX[i], digitY, 1);

                        // Draw new digit
                        tft.setTextColor(bigClockColour);
                        tft.drawString(String(currentDigits[i]), digitX[i], digitY, 1);

                        // Update bigClockLastDigit
                        bigClockLastDigit[i] = currentDigits[i];
                    }
                }
                LASTbigClockTimeStr = localTime;
            }
            break;
        }

        case 8:
        {
            satellitesDrawPage(tft, (time_t)timeClient.getEpochTime(), tOffset,
                               redrawSatellitePage);
            redrawSatellitePage = false;
            break;
        }

        case 9:
        {
            if (redrawWeatherPage)
            {
                drawWeatherPage();
                redrawWeatherPage = false;
            }
            if (currentMillis - previousMillisForWeatherPageUpdate >= 1000)
            {
                previousMillisForWeatherPageUpdate = currentMillis;
                updateWeatherPageClock();
            }
            break;
        }

        case 10:
        {
            // Cheap: the renderer decides for itself what actually changed.
            drawBeaconPage(redrawBeaconPage);
            redrawBeaconPage = false;
            break;
        }

        case 11:
        {
            drawSunMoonPage(redrawSunMoonPage);
            redrawSunMoonPage = false;
            break;
        }

        case 12:
        {
            dxClusterDrawPage(tft, (time_t)timeClient.getEpochTime(), redrawDxPage);
            redrawDxPage = false;
            break;
        }
        }

    if (autoPageChange)
    {
        // Serial.println();
        if (currentMillis - previousMillisForAutoPageChanger >= 1000 * 15)
        {
            previousMillisForAutoPageChanger = currentMillis;
            if (activePage == 1)
            {
                activePage = 2;
                redrawMainPropagationPage = true;
                return;
            }
            if (activePage == 2)
            {
                activePage = 1;
                drawOrredrawStaticElements(); // 🖼️ Redraw Big Clock frames
            }
        }
    }
}

// Today's temperature range, from the 3-hourly forecast plus whatever this
// clock has already measured today.
static const char *windCompass(int deg)
{
    static const char *pts[16] = {"N",  "NNE", "NE", "ENE", "E",  "ESE", "SE", "SSE",
                                  "S",  "SSW", "SW", "WSW", "W",  "WNW", "NW", "NNW"};
    while (deg < 0) deg += 360;
    deg %= 360;
    return pts[(int)((deg + 11.25) / 22.5) % 16];
}

// Open-Meteo reports the sky as a WMO present-weather code rather than text.
static const char *wmoDescription(int code)
{
    switch (code)
    {
    case 0:  return "clear sky";
    case 1:  return "mainly clear";
    case 2:  return "partly cloudy";
    case 3:  return "overcast";
    case 45: return "fog";
    case 48: return "depositing rime fog";
    case 51: return "light drizzle";
    case 53: return "moderate drizzle";
    case 55: return "dense drizzle";
    case 56: return "light freezing drizzle";
    case 57: return "dense freezing drizzle";
    case 61: return "slight rain";
    case 63: return "moderate rain";
    case 65: return "heavy rain";
    case 66: return "light freezing rain";
    case 67: return "heavy freezing rain";
    case 71: return "slight snowfall";
    case 73: return "moderate snowfall";
    case 75: return "heavy snowfall";
    case 77: return "snow grains";
    case 80: return "slight rain showers";
    case 81: return "moderate rain showers";
    case 82: return "violent rain showers";
    case 85: return "slight snow showers";
    case 86: return "heavy snow showers";
    case 95: return "thunderstorm";
    case 96: return "thunderstorm, slight hail";
    case 99: return "thunderstorm, heavy hail";
    default: return "unknown";
    }
}

// "2026-08-27T06:06" -> "06:06".  Open-Meteo already reports these in local
// time when the query asks for timezone=auto.
static void isoTimeOfDay(const char *iso, char *out, size_t n)
{
    const char *t = iso ? strchr(iso, 'T') : NULL;
    if (!t || strlen(t) < 6)
    {
        snprintf(out, n, "--:--");
        return;
    }
    snprintf(out, n, "%.5s", t + 1);
}

// A place name for the top of the weather page.  Open-Meteo does not return
// one, and this only changes when the configured position does, so it is asked
// for once per boot rather than on every refresh.
static void fetchPlaceName()
{
    HTTPClient http;
    String url = reverseGeocodeBase + "?latitude=" + String(latitude, 4) +
                 "&longitude=" + String(longitude, 4) + "&localityLanguage=en";
    http.setTimeout(8000);
    if (!http.begin(url)) return;

    int code = http.GET();
    String body = (code == HTTP_CODE_OK) ? http.getString() : String();
    http.end();
    if (body.isEmpty()) return;

    JsonDocument filter;
    filter["locality"] = true;
    filter["city"] = true;
    filter["countryName"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;

    const char *place = doc["locality"] | doc["city"] | "";
    if (place[0])
    {
        strlcpy(weather.city, place, sizeof(weather.city));
        strlcpy(weather.country, doc["countryName"] | "", sizeof(weather.country));
        Serial.printf("Location resolved to %s, %s\n", weather.city, weather.country);
    }
}

// Fetch weather data
void fetchWeatherData()
{
    if (weather.city[0] == 0) fetchPlaceName();

    HTTPClient http;
    String url = openMeteoBase +
                 "?latitude=" + String(latitude, 4) +
                 "&longitude=" + String(longitude, 4) +
                 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
                 "pressure_msl,cloud_cover,wind_speed_10m,wind_direction_10m,"
                 "wind_gusts_10m,precipitation,weather_code" +
                 "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset" +
                 "&hourly=visibility&forecast_days=1&forecast_hours=1" +
                 "&timezone=auto&wind_speed_unit=ms";
    Serial.println("Weather URL: " + url);

    http.setTimeout(12000);
    if (!http.begin(url))
    {
        Serial.println("Open-Meteo: http begin failed");
        weatherFetchOk = false;
        weather.valid = false;
        redrawWeatherPage = true;
        return;
    }

    int httpCode = http.GET();
    String payload = (httpCode == HTTP_CODE_OK) ? http.getString() : String();

    // Free the TLS session before parsing, not after.
    http.end();

    if (httpCode != HTTP_CODE_OK || payload.isEmpty())
    {
        Serial.printf("Error fetching weather data, HTTP code: %d\n", httpCode);
        scrollText = "Sorry, No Weather Info At This Moment!!!            Open-Meteo did not answer.";
        scrollingTextXposition = scrollingText.width();
        weatherFetchOk = false;
        weather.valid = false;
        redrawWeatherPage = true;
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    payload = String();

    if (err)
    {
        Serial.printf("Open-Meteo parse failed: %s\n", err.c_str());
        weatherFetchOk = false;
        weather.valid = false;
        redrawWeatherPage = true;
        return;
    }

    JsonObject cur = doc["current"];
    JsonObject day = doc["daily"];
    if (cur.isNull() || day.isNull())
    {
        Serial.println("Open-Meteo reply had no current block");
        weatherFetchOk = false;
        weather.valid = false;
        redrawWeatherPage = true;
        return;
    }

    // The offset comes with the weather, so daylight saving follows by itself.
    int newOffset = (int)((long)(doc["utc_offset_seconds"] | 0) / 3600);
    if (newOffset != tOffset)
        Serial.printf("\U0001F570\uFE0F UTC offset %+d h (was %+d)\n", newOffset, tOffset);
    tOffset = newOffset;

    weather.temp      = cur["temperature_2m"] | 0.0f;
    weather.feelsLike = cur["apparent_temperature"] | 0.0f;
    weather.humidity  = cur["relative_humidity_2m"] | 0;
    weather.pressure  = (int)(cur["pressure_msl"] | 0.0f);
    weather.clouds    = cur["cloud_cover"] | 0;
    weather.windSpeed = cur["wind_speed_10m"] | 0.0f;
    weather.windGust  = cur["wind_gusts_10m"] | 0.0f;
    weather.windDeg   = cur["wind_direction_10m"] | 0;
    weather.rainMM    = cur["precipitation"] | 0.0f;

    strlcpy(weather.windDir, windCompass(weather.windDeg), sizeof(weather.windDir));
    strlcpy(weather.description, wmoDescription(cur["weather_code"] | -1),
            sizeof(weather.description));

    weather.tempMin    = day["temperature_2m_min"][0] | 0.0f;
    weather.tempMax    = day["temperature_2m_max"][0] | 0.0f;
    weather.dailyValid = true;

    isoTimeOfDay(day["sunrise"][0] | "", weather.sunrise, sizeof(weather.sunrise));
    isoTimeOfDay(day["sunset"][0] | "", weather.sunset, sizeof(weather.sunset));

    // visibility is an hourly field; the query asks for a single hour of it.
    weather.visibilityKm = (float)(doc["hourly"]["visibility"][0] | 0.0f) / 1000.0f;

    weather.fetchedUnix = timeClient.getEpochTime();
    weather.valid = true;
    weatherFetchOk = true;
    redrawWeatherPage = true;

    Serial.printf("%s, %s: %.1f C (feels %.1f), %d%%RH, %d hPa, %s %.1f m/s (gust %.1f), today %.0f..%.0f C\n",
                  weather.city, weather.country, weather.temp, weather.feelsLike,
                  weather.humidity, weather.pressure, weather.windDir, weather.windSpeed,
                  weather.windGust, weather.tempMin, weather.tempMax);

    scrollText = String(weather.city) + "     " + weather.country + "    " +
                 convertTimestampToDate(weather.fetchedUnix) + "     " +
                 "Tmp: " + String(weather.temp, 1) + " C     " +
                 "RH: " + String(weather.humidity) + "%" + "       " +
                 "Pres: " + String(weather.pressure) + "hPa" + "       " +
                 String(weather.description) + "       " +
                 "Sunrise: " + weather.sunrise + "     " +
                 "Sunset: " + weather.sunset;

    scrollingText.drawString(scrollText, scrollingTextXposition, 0);
    scrollingTextXposition = scrollingText.width();
    Serial.println(scrollText);
}

// Function to format the local time from epoch time
String formatLocalTime(long epochTime)
{
    struct tm *timeInfo;
    timeInfo = localtime(&epochTime); // Convert epoch to local time
    char buffer[9];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", timeInfo); // Format time as HH:MM:SS
    return String(buffer);
}

// Function to convert an epoch time to a human-readable time string
String convertEpochToTimeString(long epochTime)
{
    struct tm *timeInfo;
    timeInfo = localtime(&epochTime); // Convert epoch to local time
    char buffer[9];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", timeInfo); // Format time as HH:MM:SS
    return String(buffer);
}

void displayTime(int x, int y, String time, String &previousTime, int yOffset, uint16_t fontColor)
{
    if (refreshDigits)
    {
        refreshDigitsCounter++;
        if (refreshDigitsCounter == 1)
        {
            // First call — force clear by using empty string
            previousTime = "";
        }
        else if (refreshDigitsCounter >= 2)
        {
            // Second call — stop refreshing
            previousTime = "";
            refreshDigits = false;
            refreshDigitsCounter = 0;
        }
    }

    // Define the calculated positions for each character
    int positions[] = {x, x + 48, x + 78, x + 108, x + 156, x + 186, x + 216, x + 264};

    // Loop over the time string and compare it with the previous time
    for (int i = 0; i < time.length(); i++)
    {
        if (time[i] != previousTime[i])
        {
            tft.setTextColor(TFT_BLACK);
            tft.drawString(String(previousTime[i]), positions[i], y + yOffset, 1);
            tft.setTextColor(fontColor);
            tft.drawString(String(time[i]), positions[i], y + yOffset, 1);
        }
    }

    previousTime = time;
}

void *fileOpen(const char *filename, int32_t *size)
{
    String fullPath = "/" + String(filename);
    pngFile = SPIFFS.open(fullPath, "r");
    if (!pngFile)
        return nullptr;
    *size = pngFile.size();
    return (void *)&pngFile;
}

void fileClose(void *handle)
{
    ((fs::File *)handle)->close();
}

int32_t fileRead(PNGFILE *handle, uint8_t *buffer, int32_t length)
{
    return ((fs::File *)handle->fHandle)->read(buffer, length);
}

int32_t fileSeek(PNGFILE *handle, int32_t position)
{
    return ((fs::File *)handle->fHandle)->seek(position);
}

void displayPNGfromSPIFFS(const char *filename, int duration_ms)
{
    if (!SPIFFS.begin(true))
    {
        Serial.println("Failed to mount SPIFFS!");
        return;
    }

    int16_t rc = png.open(filename, fileOpen, fileClose, fileRead, fileSeek, [](PNGDRAW *pDraw)
                          {
    uint16_t lineBuffer[480];  // Adjust to your screen width if needed
    png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
    tft.pushImage(0, pDraw->y, pDraw->iWidth, 1, lineBuffer); });

    if (rc == PNG_SUCCESS)
    {
        Serial.printf("Displaying PNG: %s\n", filename);
        tft.startWrite();
        png.decode(nullptr, 0);
        tft.endWrite();
    }
    else
    {
        Serial.println("PNG decode failed.");
    }

    delay(duration_ms);
}

String convertTimestampToDate(long timestamp)
{
    struct tm *timeinfo;
    timeinfo = localtime(&timestamp);                       // Convert epoch to local time
    char buffer[11];                                        // Buffer for "DD:MM:YY"
    strftime(buffer, sizeof(buffer), "%d:%m:%y", timeinfo); // Format as DD:MM:YY
    return String(buffer);
}

void loadSettings()
{

    fs::File file = SPIFFS.open("/settings.json", "r");
    if (!file)
    {
        Serial.println("⚠️ Could not open settings file. Using defaults.");
        saveSettings();
        esp_restart();
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error)
    {
        Serial.println("⚠️ Failed to parse settings file. Using defaults.");
        file.close();
        return;
    }

    file.close(); // Always close file after use

    latitude = doc["latitude"] | latitude;
    longitude = doc["longitude"] | longitude;
    localTimeColour = doc["localTimeColour"] | localTimeColour;
    utcTimeColour = doc["utcTimeColour"] | utcTimeColour;
    doubleFrame = doc["doubleFrame"] | doubleFrame;
    localFrameColour = doc["localFrameColour"] | localFrameColour;
    utcFrameColour = doc["utcFrameColour"] | utcFrameColour;
    bannerColour = doc["bannerColour"] | bannerColour;
    bannerSpeed = doc["bannerSpeed"] | bannerSpeed;
    localTimeLabel = doc["localTimeLabel"] | localTimeLabel;
    utcTimeLabel = doc["utcTimeLabel"] | utcTimeLabel;
    startupLogo = doc["startupLogo"] | startupLogo;
    italicClockFonts = doc["italicClockFonts"] | italicClockFonts;
    screenSaverTimeout = doc["screenSaverTimeout"] | screenSaverTimeout;
    autoPageChange = doc["autoPageChange"] | autoPageChange;
    useScreenSaver = doc["useScreenSaver"] | useScreenSaver;
    bigClockColour = doc["bigClockColour"] | bigClockColour;
    bigClockShowsUtc = doc["bigClockShowsUtc"] | bigClockShowsUtc;
    weatherIntervalMin = doc["weatherIntervalMin"] | weatherIntervalMin;
    if (weatherIntervalMin < WEATHER_INTERVAL_MIN_LIMIT) weatherIntervalMin = WEATHER_INTERVAL_MIN_LIMIT;
    if (weatherIntervalMin > WEATHER_INTERVAL_MAX_LIMIT) weatherIntervalMin = WEATHER_INTERVAL_MAX_LIMIT;

    Serial.println();
    Serial.println("-----------------------------------------------------------------");
    Serial.println("✅ Settings loaded from SPIFFS:");
    Serial.printf("📍 latitude: %.6f\n", latitude);
    Serial.printf("📍 longitude: %.6f\n", longitude);
    Serial.printf("🎨 localTimeColour: 0x%04X\n", localTimeColour);
    Serial.printf("🎨 utcTimeColour: 0x%04X\n", utcTimeColour);
    Serial.printf("🌀 doubleFrame: %s\n", doubleFrame ? "true" : "false");
    Serial.printf("🎨 localFrameColour: 0x%04X\n", localFrameColour);
    Serial.printf("🎨 utcFrameColour: 0x%04X\n", utcFrameColour);
    Serial.printf("🖍️ bannerColour: 0x%04X\n", bannerColour);
    Serial.printf("🖍️ bigClockColour: 0x%04X\n", bigClockColour);
    Serial.printf("🕑 bigClockShowsUtc: %s\n", bigClockShowsUtc ? "UTC" : "QTH");

    Serial.printf("🐢 bannerSpeed: %d\n", bannerSpeed);
    Serial.printf("🕓 localTimeLabel: %s\n", localTimeLabel.c_str());
    Serial.printf("🌍 utcTimeLabel: %s\n", utcTimeLabel.c_str());
    Serial.printf("🖼️ startupLogo: %s\n", startupLogo.c_str());
    Serial.printf("🔤 italicClockFonts: %s\n", italicClockFonts ? "true" : "false");
    Serial.printf("🕓 screenSaverTimeout: %lu ms\n", screenSaverTimeout);
    Serial.printf("⚡ Auto Page Change    : %s\n", autoPageChange ? "true" : "false");
    Serial.printf("⚡ Use Screen Saver    : %s\n", useScreenSaver ? "true" : "false");
    Serial.println("-----------------------------------------------------------------");
}

void saveSettings()
{
    JsonDocument doc;
    doc["latitude"] = latitude;
    doc["longitude"] = longitude;
    doc["localTimeColour"] = localTimeColour;
    doc["utcTimeColour"] = utcTimeColour;
    doc["doubleFrame"] = doubleFrame;
    doc["localFrameColour"] = localFrameColour;
    doc["utcFrameColour"] = utcFrameColour;
    doc["bannerColour"] = bannerColour;
    doc["bannerSpeed"] = bannerSpeed;
    doc["localTimeLabel"] = localTimeLabel;
    doc["utcTimeLabel"] = utcTimeLabel;
    doc["startupLogo"] = startupLogo;
    doc["italicClockFonts"] = italicClockFonts;
    doc["autoPageChange"] = autoPageChange;
    doc["useScreenSaver"] = useScreenSaver;
    doc["bigClockColour"] = bigClockColour;
    doc["bigClockShowsUtc"] = bigClockShowsUtc;
    doc["weatherIntervalMin"] = weatherIntervalMin;
    doc["screenSaverTimeout"] = screenSaverTimeout;

    fs::File file = SPIFFS.open("/settings.json", "w");

    if (!file)
    {
        Serial.println("❌ Failed to open settings file for writing");
        return;
    }

    serializeJsonPretty(doc, file);
    file.close();

    // ✅ Nicely formatted output
    Serial.println("");
    Serial.println(F("────────────────────────────────────────"));

    Serial.println("💾 Saving settings to SPIFFS:");
    Serial.println(F("────────────────────────────────────────"));
    Serial.printf("🌍 Latitude           : %f\n", latitude);
    Serial.printf("🌍 Longitude          : %f\n", longitude);
    {
        char loc[10];
        maidenhead(latitude, longitude, loc, sizeof(loc));
        Serial.printf("   Locator            : %s\n", loc);
    }
    Serial.printf("🕒 Local Time Color   : 0x%04X\n", localTimeColour);
    Serial.printf("🕒 UTC Time Color     : 0x%04X\n", utcTimeColour);
    Serial.printf("🕒 Big Time Color     : 0x%04X\n", bigClockColour);
    Serial.printf("🕑 Big Clock Base     : %s\n", bigClockShowsUtc ? "UTC" : "QTH");
    Serial.printf("🖼️  Double Frame      : %s\n", doubleFrame ? "true" : "false");
    Serial.printf("🟩 Local Frame Color  : 0x%04X\n", localFrameColour);
    Serial.printf("🟦 UTC Frame Color    : 0x%04X\n", utcFrameColour);
    Serial.printf("📜 Banner Color       : 0x%04X\n", bannerColour);
    Serial.printf("⚡ Banner Speed       : %d\n", bannerSpeed);
    Serial.printf("🔤 Local Time Label   : %s\n", localTimeLabel.c_str());
    Serial.printf("🔤 UTC Time Label     : %s\n", utcTimeLabel.c_str());
    Serial.printf("🖼️  Startup Logo      : %s\n", startupLogo.c_str());
    Serial.printf("✏️  Italic Fonts      : %s\n", italicClockFonts ? "true" : "false");
    Serial.printf("😴 Screensaver (ms)   : %lu\n", screenSaverTimeout);
    Serial.printf("⚡ Use screensaver    : %s\n", useScreenSaver ? "true" : "false");
    Serial.printf("⚡ Auto Page Change    : %s\n", autoPageChange ? "true" : "false");

    Serial.println(F("────────────────────────────────────────"));

    Serial.println("✅ Settings saved to SPIFFS");
    Serial.println("");
}

void handleRoot()
{
    fs::File file = SPIFFS.open("/index.html", "r"); // ✅ Declare 'file' properly here
    if (!file)
    {
        server.send(500, "text/plain", "⚠️ Failed to open index.html");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}


void handleSave()
{
    if (server.hasArg("latitude"))
        latitude = server.arg("latitude").toFloat();
    if (server.hasArg("longitude"))
        longitude = server.arg("longitude").toFloat();
    if (server.hasArg("bannerSpeed"))
        bannerSpeed = server.arg("bannerSpeed").toInt();
    if (server.hasArg("localLabel"))
        localTimeLabel = server.arg("localLabel");
    if (server.hasArg("utcLabel"))
        utcTimeLabel = server.arg("utcLabel");
    if (server.hasArg("logo"))
        startupLogo = server.arg("logo");
    if (server.hasArg("italicFont"))
        italicClockFonts = (server.arg("italicFont") == "on");

    saveSettings(); // Save updated settings

    server.send(200, "text/html", "<h1>✅ Settings saved!</h1><a href='/'>Back</a>");
}

void drawOrredrawStaticElementsOLD()
{
    // Only run if we want to refresh the frames
    if (refreshFrames)
    {
        refreshFramesCounter++;
        if (refreshFramesCounter < 2)
        {
            return; // Wait for second execution
        }
        refreshFrames = false;
        refreshFramesCounter = 0;
    }
    previousLocalTime = "";
    previousUTCtime = "";
    tft.setFreeFont(&Orbitron_Medium8pt7b);
    tft.fillRect(25, 0 + 85 - 10, 270, 20, TFT_BLACK);
    tft.fillRect(25, 106 + 85 - 10, 270, 20, TFT_BLACK);

    // 🟩 Local Frame
    tft.fillRect(0, 0, 320, 87, TFT_BLACK); // Clear previous frame
    tft.drawRoundRect(1, 1, 318, 85, 4, TFT_BLACK);

    tft.drawRoundRect(0, 0, 320, 87, 5, localFrameColour);
    if (doubleFrame)
    {
        tft.drawRoundRect(1, 1, 318, 85, 4, localFrameColour);
        tft.drawRoundRect(2, 2, 316, 83, 4, localFrameColour);
        tft.drawRoundRect(3, 3, 314, 81, 4, localFrameColour);
    }

    // 🟦 Local Time Label

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(localTimeLabel, 160, 76, 1);

    // 🟥 UTC Frame
    tft.fillRect(0, 105, 320, 87, TFT_BLACK); // Clear previous frame
    tft.drawRoundRect(1, 106, 318, 85, 4, TFT_BLACK);

    tft.drawRoundRect(0, 105, 320, 87, 5, utcFrameColour);
    if (doubleFrame)
    {
        tft.drawRoundRect(1, 106, 318, 85, 4, utcFrameColour);
        tft.drawRoundRect(2, 107, 316, 83, 4, utcFrameColour);
        tft.drawRoundRect(3, 108, 314, 81, 4, utcFrameColour);
    }

    // ⬜ UTC Label
    tft.drawCentreString(utcTimeLabel, 160, 76 + 105, 1);
}

void drawOrredrawStaticElements()
{
    // Only run if we want to refresh the frames
    tft.fillScreen(TFT_BLACK);
    if (refreshFrames)
    {
        refreshFramesCounter++;
        if (refreshFramesCounter < 2)
        {
            return; // Wait for second execution
        }
        refreshFrames = false;
        refreshFramesCounter = 0;
    }
    previousLocalTime = "";
    previousUTCtime = "";
    tft.setFreeFont(&Orbitron_Medium8pt7b);
    tft.fillRect(25, 0 + 85 - 10, 270, 20, TFT_BLACK);
    tft.fillRect(25, 106 + 85 - 10, 270, 20, TFT_BLACK);

    // 🟩 Local Frame
    tft.fillRect(0, 0, 320, 87, TFT_BLACK); // Clear previous frame

    tft.drawRoundRect(1, 1, 319, 85, 5, localFrameColour);
    if (doubleFrame)
    {
        tft.drawRoundRect(1, 1, 319, 85, 4, localFrameColour);
        tft.drawRoundRect(2, 2, 317, 83, 4, localFrameColour);
        tft.drawRoundRect(3, 3, 315, 81, 4, localFrameColour);
    }

    // 🟦 Local Time Label

    // Both captions sit right of the frame's centre line.
    const int LABEL_CX = 180;
    const int LOCATOR_RIGHT = 312;

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(localTimeLabel, LABEL_CX, 76, 1);

    // The Maidenhead locator belongs to the QTH, so it goes on the caption row
    // of the QTH frame, right-aligned inside it and in the caption own colour.
    // The row is clear of the big digits, which stop above it.
    {
        char loc[10];
        maidenhead(latitude, longitude, loc, sizeof(loc));

        // Measured while the caption font is still the current one.
        int labelRight = LABEL_CX + tft.textWidth(localTimeLabel) / 2;

        // Eight characters next to the caption need a narrower face, and a
        // lighter one reads as an annotation rather than a second caption.
        // Drawn four pixels lower so the two share a baseline: the caption
        // font carries 15 pixels of ascent against this one's 11.
        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        int locLeft = LOCATOR_RIGHT - tft.textWidth(loc);

        // The caption is renamable from the web page, so check the two still
        // clear each other rather than letting them collide.
        if (locLeft > labelRight + 8)
        {
            tft.setTextDatum(TR_DATUM);
            tft.drawString(loc, LOCATOR_RIGHT, 80);
            tft.setTextDatum(TL_DATUM);
        }

        tft.setFreeFont(&Orbitron_Medium8pt7b);   // hand the font back
    }

    // 🟥 UTC Frame
    tft.fillRect(0, 105, 320, 87, TFT_BLACK); // Clear previous frame

    tft.drawRoundRect(1, 105, 319, 85, 5, utcFrameColour);
    if (doubleFrame)
    {
        tft.drawRoundRect(1, 106, 319, 85, 4, utcFrameColour);
        tft.drawRoundRect(2, 107, 317, 83, 4, utcFrameColour);
        tft.drawRoundRect(3, 108, 315, 81, 4, utcFrameColour);
    }

    // ⬜ UTC Label
    tft.drawCentreString(utcTimeLabel, LABEL_CX, 76 + 105, 1);
}

void mountAndListSPIFFS(uint8_t levels, bool listContent)
{
    Serial.println();
    if (!SPIFFS.begin(true))
    {
        Serial.println("\n❌ Failed to mount SPIFFS.");
        return;
    }
    Serial.println("\n✅ SPIFFS mounted successfully!");

    if (!listContent)
        return;

    Serial.println("📂 Listing SPIFFS content:");
    fs::File root = SPIFFS.open("/");
    if (!root || !root.isDirectory())
    {
        Serial.println("❌ Failed to open root directory or not a directory.");
        return;
    }

    fs::File file = root.openNextFile();
    while (file)
    {
        if (file.isDirectory())
        {
            Serial.print("  📁 DIR : ");
            Serial.println(file.name());
            if (levels)
            {
                String path = String("/") + file.name();
                mountAndListSPIFFS(levels - 1, true); // Recursive listing
            }
        }
        else
        {
            Serial.print("  📄 FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }

    // Calculate and print free space information
    size_t total = SPIFFS.totalBytes();
    size_t used = SPIFFS.usedBytes();
    size_t free = total - used;
    float percentFree = ((float)free / total) * 100.0;

    Serial.println();
    Serial.println("📊 SPIFFS Usage Info:");
    Serial.printf("   📦 Total: %u bytes\n", total);
    Serial.printf("   📂 Used : %u bytes\n", used);
    Serial.printf("   📭 Free : %u bytes (%.2f%%)\n", free, percentFree);
    Serial.println();
}

void handlePNGUpload()
{
    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        Serial.printf("📁 Uploading PNG: %s\n", upload.filename.c_str());

        // 🖥️ Blank screen and show "Receiving" + "New" + "Splash Screen" on 3 lines
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setFreeFont(&Orbitron_Light_32);
        tft.drawCentreString("Receiving", 160, 10, 1);

        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawCentreString("New", 160, 60, 1);
        tft.drawCentreString("Splash Screen", 160, 110, 1);

        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setFreeFont(&Orbitron_Medium8pt7b);
        tft.drawCentreString("Please wait...", 160, 170, 1);

        fs::File file = SPIFFS.open("/logo4.png", FILE_WRITE);
        if (!file)
        {
            Serial.println("❌ Failed to open file for writing");
            return;
        }
        file.close();
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        fs::File file = SPIFFS.open("/logo4.png", FILE_APPEND);
        if (file)
        {
            file.write(upload.buf, upload.currentSize);
            file.close();
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        Serial.printf("✅ Upload complete: %s (%d bytes)\n", upload.filename.c_str(), upload.totalSize);
        server.send(200, "text/plain", "✅ PNG upload complete. Will be used at next boot.");
        startupLogo = "logo4.png";
        displayPNGfromSPIFFS("logo4.png", 3000);
        saveSettings();
        tft.fillScreen(TFT_BLACK);
        drawOrredrawStaticElements();
    }
}

// Switch to `page` and queue whatever that page needs redrawn.  Shared by both
// paging directions so the two cannot drift apart.
static void activatePage(uint8_t page)
{
    activePage = page;
    tft.fillScreen(TFT_BLACK);

    switch (activePage)
    {
    case 1:
        drawOrredrawStaticElements(); // 🖼️ Redraw Big Clock frames
        break;
    case 2:
        redrawMainPropagationPage = true;
        break;
    case 3:
        redrawSolarSummaryPage1 = true;
        break;
    case 4:
        redrawSolarSummaryPage2 = true;
        break;
    case 5:
        redrawSolarSummaryPage3 = true;
        break;
    case 6:
        reDrawWiFiQualityPage = true;
        break;
    case 7:
        LASTbigClockTimeStr = "";
        for (int i = 0; i < 4; i++)
        {
            bigClockLastDigit[i] = ' ';
        }
        // Unknown state, so the first pass through the loop paints the colon
        // immediately after the screen clear.
        bigClockColonState = -1;
        bigClockLabelDirty = true;
        break;
    case 8:
        redrawSatellitePage = true;
        break;
    case 9:
        redrawWeatherPage = true;
        break;
    case 10:
        redrawBeaconPage = true;
        break;
    case 11:
        redrawSunMoonPage = true;
        break;
    case 12:
        redrawDxPage = true;
        break;
    }
}

void handleTouchToRotatePage()
{
    uint16_t x, y;
    bool touching = readTouchPoint(&x, &y);
    unsigned long now = millis();

    if (touching)
    {
        if (!wasTouching && (now - lastTouchMs > 250)) // edge detect + debounce
        {
            wasTouching = true;
            lastTouchMs = now;

            // On the big clock page the badge is a button: tapping it switches
            // the time base rather than paging.  Checked first so the paging
            // halves cannot swallow the tap.
            if (activePage == 7 &&
                x >= BIGCLOCK_BADGE_X - 10 && x <= BIGCLOCK_BADGE_X + BIGCLOCK_BADGE_W + 10 &&
                y >= BIGCLOCK_BADGE_Y - 10 && y <= BIGCLOCK_BADGE_Y + BIGCLOCK_BADGE_H + 10)
            {
                bigClockShowsUtc = !bigClockShowsUtc;
                Serial.printf("\U0001F551 Big clock time base -> %s\n",
                              bigClockShowsUtc ? "UTC" : "QTH");
                saveSettings();

                // Force a full repaint of the digits in the new time base.
                LASTbigClockTimeStr = "";
                for (int i = 0; i < 4; i++)
                {
                    bigClockLastDigit[i] = ' ';
                }
                tft.fillScreen(TFT_BLACK);
                bigClockColonState = -1;
                bigClockLabelDirty = true;
                return;
            }

            // The DX page has a button of its own, and while its filter panel
            // is up it owns every tap - otherwise a miss would page out from
            // under the panel.
            if (activePage == 12 && dxClusterHandleTouch((int16_t)x, (int16_t)y))
            {
                redrawDxPage = true;
                return;
            }

            // Tap the right half to go forward, the left half to go back.
            bool forward = (x >= tft.width() / 2);
            uint8_t next = forward ? (activePage % MAX_PAGES) + 1
                                   : (activePage + MAX_PAGES - 2) % MAX_PAGES + 1;

            // The touch x is printed so a mirrored panel is obvious without a
            // debug build: if a tap on the right reports a low x, swap
            // TOUCH_RAW_X_MIN and TOUCH_RAW_X_MAX in platformio.ini.
            Serial.printf("📄 Active page -> %u (touch x=%u, %s)\n",
                          next, x, forward ? "forward" : "back");

            activatePage(next);
        }
    }
    else
    {
        wasTouching = false;
    }
}

void drawMainPropagationPage()
{

    tft.fillScreen(TFT_BLACK);
    // draw frames
    //  Define positions and dimensions
    int dayX = 10;
    int nightX = 170;
    int blockY = 12;
    int blockWidth = 140;
    int blockHeight = 143;
    int cornerRadius = 8;

    tft.drawRoundRect(dayX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
    tft.drawRoundRect(nightX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
    tft.fillRect(80 - 27, 0, 54, 20, TFT_BLACK);
    tft.fillRect(240 - 38, 0, 76, 20, TFT_BLACK);

    // Draw headers
    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(TFT_LIGHTGREY);
    tft.drawCentreString("DAY", 80, 2, 1);
    tft.drawCentreString("NIGHT", 240, 2, 1);

    // Band conditions by time
    tft.setFreeFont(&JetBrainsMono_Bold15pt7b);

    int yStart = 22;
    for (int i = 0; i < 4; i++)
    {
        // DAY

        String band = solarData.bandConditions[i].name;
        String cond = solarData.bandConditions[i].condition;
        uint16_t color = cond == "Good" ? TFT_GREEN : cond == "Fair" ? TFT_YELLOW
                                                                     : TFT_RED;
        tft.setTextColor(color);
        tft.drawCentreString(band, 80, yStart + i * 32, 1);

        // NIGHT
        band = solarData.bandConditions[i + 4].name;
        cond = solarData.bandConditions[i + 4].condition;
        color = cond == "Good" ? TFT_GREEN : cond == "Fair" ? TFT_YELLOW
                                                            : TFT_RED;
        tft.setTextColor(color);

        tft.drawCentreString(band, 240, yStart + i * 32, 1);
    }

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY);
    tft.drawCentreString("Updated: " + solarData.updated, 160, 160, 1);
    // tft.drawCentreString("Updating...", 80, 185, 1);
    // tft.drawCentreString("Updating...", 240, 185, 1);

    int LocalX = 10;
    int UTCX = 170;
    blockY = 190;
    blockWidth = 140;
    blockHeight = 48;
    cornerRadius = 8;

    tft.drawRoundRect(LocalX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
    tft.drawRoundRect(UTCX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
    tft.fillRect(80 - 36, blockY - 15, 72, 35, TFT_BLACK);
    tft.fillRect(240 - 26, blockY - 15, 52, 35, TFT_BLACK);

    // Draw headers
    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(TFT_LIGHTGREY);

    tft.drawCentreString("Local", 80, 179, 1);
    tft.drawCentreString("UTC", 240, 179, 1);
}

// ---------------------------------------------------------------------------
// Minimal reader for the solar XML.
//
// The document is a flat list of <tag>value</tag> plus two lists of elements
// carrying attributes - about 1.6 kB in total.  tinyxml2 needed roughly 20 kB
// of heap for it (three node pools, grown 4088 bytes at a time) and was failing
// on one fetch in three once the rest of the firmware had taken its share.
// Scanning the text costs nothing beyond the string already in hand.
// ---------------------------------------------------------------------------

// Start of the opening tag <name ...> , or -1.  Matches the whole tag name so
// <sunspots> is not found by a search for <sun>.
static int xmlFindTag(const String &src, const char *name, int from = 0)
{
    String open = String("<") + name;
    int at = src.indexOf(open, from);
    while (at >= 0)
    {
        char next = src.charAt(at + open.length());
        if (next == '>' || next == ' ' || next == '\t' || next == '/' || next == '\r' || next == '\n')
            return at;
        at = src.indexOf(open, at + 1);
    }
    return -1;
}

// Text between <name ...> and </name>.  Empty when the tag is missing.
static String xmlText(const String &src, const char *name, int from = 0)
{
    int at = xmlFindTag(src, name, from);
    if (at < 0) return String();

    int gt = src.indexOf('>', at);
    if (gt < 0 || src.charAt(gt - 1) == '/') return String();

    String close = String("</") + name + ">";
    int end = src.indexOf(close, gt + 1);
    if (end < 0) return String();

    return src.substring(gt + 1, end);
}

// Value of attr="..." inside the tag opening at tagStart.
static String xmlAttr(const String &src, int tagStart, const char *attr)
{
    int gt = src.indexOf('>', tagStart);
    if (gt < 0) return String();

    String key = String(" ") + attr + "=\"";
    int at = src.indexOf(key, tagStart);
    if (at < 0 || at > gt) return String();

    at += key.length();
    int quote = src.indexOf('"', at);
    if (quote < 0 || quote > gt) return String();

    return src.substring(at, quote);
}

void fetchSolarData()
{

    // Fetch XML
    HTTPClient http;
    http.begin(solarDataUrl);
    int httpCode = http.GET();

    if (httpCode <= 0)
    {
        Serial.println("HTTP request failed");
        return;
    }

    String payload = http.getString();

    // Close the connection before touching the payload: an open TLS session
    // holds tens of kilobytes of mbedTLS buffers.
    http.end();

    Serial.printf("Solar XML: %u bytes, 8-bit free=%u largest=%u\n",
                  payload.length(),
                  heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (payload.length() < 100 || xmlFindTag(payload, "solardata") < 0)
    {
        Serial.println("Solar XML looks wrong, skipping this cycle");
        return;
    }

    auto get = [&](const char *tag) { return xmlText(payload, tag); };

    // Assign fields to struct
    solarData.source = get("source");
    // solarData.updated = get("updated"); reformatted because like this 31 Jul 2025 1321 GMT
    solarData.updated = formatUpdatedTimestampToUTC(get("updated"));
    solarData.solarFlux = get("solarflux").toInt();
    solarData.aIndex = get("aindex").toInt();
    solarData.kIndex = get("kindex").toInt();
    solarData.kIndexNT = get("kindexnt");
    solarData.xRay = get("xray");
    solarData.sunspots = get("sunspots").toInt();
    solarData.heliumLine = get("heliumline").toFloat();
    solarData.protonFlux = get("protonflux");
    solarData.electronFlux = get("electonflux");
    solarData.aurora = get("aurora").toInt();
    solarData.normalization = get("normalization").toFloat();
    solarData.latDegree = get("latdegree").toFloat();
    solarData.solarWind = get("solarwind").toFloat();
    solarData.magneticField = get("magneticfield").toFloat();
    solarData.geomagneticField = get("geomagfield");
    solarData.signalNoise = get("signalnoise");
    solarData.fof2 = get("fof2");
    solarData.mufFactor = get("muffactor");
    solarData.muf = get("muf");

    // Parse band conditions
    int bIndex = 0;
    int scan = 0;
    while (bIndex < 8)
    {
        int at = xmlFindTag(payload, "band", scan);
        if (at < 0) break;
        int gt = payload.indexOf('>', at);
        int end = payload.indexOf("</band>", gt);
        if (gt < 0 || end < 0) break;

        solarData.bandConditions[bIndex].name = xmlAttr(payload, at, "name");
        solarData.bandConditions[bIndex].time = xmlAttr(payload, at, "time");
        solarData.bandConditions[bIndex].condition = payload.substring(gt + 1, end);
        scan = end + 7;
        bIndex++;
    }

    // Parse VHF conditions
    int vIndex = 0;
    scan = 0;
    while (vIndex < 5)
    {
        int at = xmlFindTag(payload, "phenomenon", scan);
        if (at < 0) break;
        int gt = payload.indexOf('>', at);
        int end = payload.indexOf("</phenomenon>", gt);
        if (gt < 0 || end < 0) break;

        solarData.vhfConditions[vIndex].name = xmlAttr(payload, at, "name");
        solarData.vhfConditions[vIndex].location = xmlAttr(payload, at, "location");
        solarData.vhfConditions[vIndex].condition = payload.substring(gt + 1, end);
        scan = end + 13;
        vIndex++;
    }

    // --- Serial Debug Output ---
    Serial.println("\n=== Solar Data ===");
    Serial.println("Source: " + solarData.source);
    Serial.println("Updated: " + solarData.updated);
    Serial.printf("Solar Flux: %d\n", solarData.solarFlux);
    Serial.printf("A Index: %d\n", solarData.aIndex);
    Serial.printf("K Index: %d\n", solarData.kIndex);
    Serial.println("K Index NT: " + solarData.kIndexNT);
    Serial.println("X-Ray: " + solarData.xRay);
    Serial.printf("Sunspots: %d\n", solarData.sunspots);
    Serial.printf("Helium Line: %.1f\n", solarData.heliumLine);
    Serial.println("Proton Flux: " + solarData.protonFlux);
    Serial.println("Electron Flux: " + solarData.electronFlux);
    Serial.printf("Aurora: %d\n", solarData.aurora);
    Serial.printf("Normalization: %.2f\n", solarData.normalization);
    Serial.printf("Lat Degree: %.2f\n", solarData.latDegree);
    Serial.printf("Solar Wind: %.1f\n", solarData.solarWind);
    Serial.printf("Magnetic Field: %.1f\n", solarData.magneticField);
    Serial.println("Geomagnetic Field: " + solarData.geomagneticField);
    Serial.println("Signal Noise: " + solarData.signalNoise);
    Serial.println("foF2: " + solarData.fof2);
    Serial.println("MUF Factor: " + solarData.mufFactor);
    Serial.println("MUF: " + solarData.muf);

    Serial.println("--- Band Conditions ---");
    for (int i = 0; i < 8; i++)
    {
        if (solarData.bandConditions[i].name.isEmpty())
            break;
        Serial.printf("[%s] %s: %s\n",
                      solarData.bandConditions[i].time.c_str(),
                      solarData.bandConditions[i].name.c_str(),
                      solarData.bandConditions[i].condition.c_str());
    }

    Serial.println("--- VHF Conditions ---");
    for (int i = 0; i < 5; i++)
    {
        if (solarData.vhfConditions[i].name.isEmpty())
            break;
        Serial.printf("%s (%s): %s\n",
                      solarData.vhfConditions[i].name.c_str(),
                      solarData.vhfConditions[i].location.c_str(),
                      solarData.vhfConditions[i].condition.c_str());
    }
}

String formatUpdatedTimestampToUTC(const String &raw)
{
    int gmtPos = raw.indexOf("GMT");
    if (gmtPos == -1 || gmtPos < 5)
        return raw; // malformed or too short

    // Extract 4 characters before "GMT" → should be the time
    String timePart = raw.substring(gmtPos - 5, gmtPos - 1); // e.g. "1321"
    if (timePart.length() != 4)
        return raw;

    // Insert colon in the time
    String formattedTime = timePart.substring(0, 2) + ":" + timePart.substring(2, 4);

    // Everything before the time
    String datePart = raw.substring(0, gmtPos - 5);
    datePart.trim(); // remove any leading/trailing whitespace

    return datePart + " " + formattedTime + " UTC";
}

void drawLOCALTime(const String &timeStr, int x, int y, uint16_t digitColor, uint16_t backgroundColor, bool blinkColon)
{
    tft.setFreeFont(&HB97DIGITS12pt7b);

    for (int i = 0; i < 8; i++)
    {
        char newChar = timeStr.charAt(i);
        char oldChar = LOCALlastTimeStr.charAt(i);
        int xpos = x + xOffsets[i];

        // Always redraw colon, toggling its color
        if (i == 2 || i == 5)
        {
            uint16_t colonColor = blinkColon ? (colonVisible ? digitColor : backgroundColor) : digitColor;
            tft.setTextColor(colonColor, backgroundColor);
            tft.drawString(":", xpos, y, 1);
            continue; // skip rest of loop for colon
        }

        // Redraw only if digit changed
        if (newChar != oldChar)
        {
            // Erase old character
            tft.setTextColor(backgroundColor, backgroundColor);
            tft.drawString(String(oldChar), xpos, y, 1);

            // Draw new character
            tft.setTextColor(digitColor, backgroundColor);
            tft.drawString(String(newChar), xpos, y, 1);
        }
    }

    // Save current drawn string (colons not modified)
    LOCALlastTimeStr = timeStr;
}

void drawUTCTime(const String &timeStr, int x, int y, uint16_t digitColor, uint16_t backgroundColor, bool blinkColon)
{
    tft.setFreeFont(&HB97DIGITS12pt7b);

    for (int i = 0; i < 8; i++)
    {
        char newChar = timeStr.charAt(i);
        char oldChar = UTClastTimeStr.charAt(i);
        int xpos = x + xOffsets[i];

        // Always redraw colon, toggling its color
        if (i == 2 || i == 5)
        {
            uint16_t colonColor = blinkColon ? (colonVisible ? digitColor : backgroundColor) : digitColor;
            tft.setTextColor(colonColor, backgroundColor);
            tft.drawString(":", xpos, y, 1);
            continue; // skip rest of loop for colon
        }

        // Redraw only if digit changed
        if (newChar != oldChar)
        {
            // Erase old character
            tft.setTextColor(backgroundColor, backgroundColor);
            tft.drawString(String(oldChar), xpos, y, 1);

            // Draw new character
            tft.setTextColor(digitColor, backgroundColor);
            tft.drawString(String(newChar), xpos, y, 1);
        }
    }

    // Save current drawn string (colons not modified)
    UTClastTimeStr = timeStr;
}

void drawSolarSummaryPage1()
{
    int y = 13;
    int lineSpacing = 18;
    tft.fillScreen(TFT_BLACK);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextSize(1);

    // Adjust spacing
    const int labelX = 10;
    const int valueX = 120;
    const int commentX = 200;

    auto printLine = [&](const String &label, const String &value, uint16_t color, const String &comment = "")
    {
        tft.setTextColor(color, TFT_BLACK); // foreground on black
        tft.setCursor(labelX, y);
        tft.print(label);
        tft.setCursor(valueX, y);
        tft.print(": ");
        tft.print(value);
        if (comment.length() > 0)
        {
            tft.setCursor(commentX, y);
            tft.print("(" + comment + ")");
        }
        y += lineSpacing;
    };

    // Color + comment logic
    auto kIndexColorComment = [](int k)
    {
        if (k >= 7)
            return std::make_pair(TFT_RED, "Severe");
        if (k >= 5)
            return std::make_pair(TFT_RED, "Storm Risk");
        if (k >= 4)
            return std::make_pair(TFT_ORANGE, "Unsettled");
        if (k >= 2)
            return std::make_pair(TFT_YELLOW, "Quiet");
        return std::make_pair(TFT_GREEN, "Very Quiet");
    };

    auto aIndexColorComment = [](int a)
    {
        if (a >= 30)
            return std::make_pair(TFT_RED, "Disturbed");
        if (a >= 20)
            return std::make_pair(TFT_ORANGE, "Unsettled");
        if (a >= 10)
            return std::make_pair(TFT_YELLOW, "Normal");
        return std::make_pair(TFT_GREEN, "Quiet");
    };

    auto solarFluxColorComment = [](int sfi)
    {
        if (sfi >= 150)
            return std::make_pair(TFT_GREEN, "Excellent");
        if (sfi >= 100)
            return std::make_pair(TFT_YELLOW, "Good");
        return std::make_pair(TFT_RED, "Poor");
    };

    auto xrayColorComment = [](const String &x)
    {
        if (x.startsWith("X"))
            return std::make_pair(TFT_RED, "Extreme");
        if (x.startsWith("M"))
            return std::make_pair(TFT_ORANGE, "Moderate");
        if (x.startsWith("C"))
            return std::make_pair(TFT_YELLOW, "Low");
        return std::make_pair(TFT_GREEN, "Quiet");
    };

    auto [sfColor, sfComment] = solarFluxColorComment(solarData.solarFlux);
    printLine("Solar Flux", String(solarData.solarFlux), sfColor, sfComment);

    auto [aColor, aComment] = aIndexColorComment(solarData.aIndex);
    printLine("A Index", String(solarData.aIndex), aColor, aComment);

    auto [kColor, kComment] = kIndexColorComment(solarData.kIndex);
    printLine("K Index", String(solarData.kIndex), kColor, kComment);

    printLine("K Index NT", solarData.kIndexNT, TFT_WHITE);

    auto [xrColor, xrComment] = xrayColorComment(solarData.xRay);
    printLine("X-Ray", solarData.xRay, xrColor, xrComment);

    printLine("Sunspots", String(solarData.sunspots), TFT_WHITE);
    printLine("Helium Line", String(solarData.heliumLine, 1), TFT_WHITE);
    printLine("Proton Flux", solarData.protonFlux, TFT_WHITE);
    printLine("Electron Flux", solarData.electronFlux, TFT_WHITE);
    printLine("Aurora", String(solarData.aurora), TFT_WHITE);
    printLine("Normalization", String(solarData.normalization, 2), TFT_WHITE);
    printLine("Lat Degree", String(solarData.latDegree, 2), TFT_WHITE);
    printLine("Solar Wind", String(solarData.solarWind, 1), TFT_WHITE);
}

void drawSolarSummaryPage2()
{
    int y = 13;
    int lineSpacing = 18;
    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextSize(1);

    const int labelX = 10;
    const int valueX = 120;
    const int commentX = 200;

    auto printLine = [&](const String &label, const String &value, uint16_t color = TFT_WHITE, const String &comment = "")
    {
        tft.setTextColor(color, TFT_BLACK);
        tft.setCursor(labelX, y);
        tft.print(label);
        tft.setCursor(valueX, y);
        tft.print(": ");
        tft.print(value);
        if (comment.length() > 0)
        {
            tft.setCursor(commentX, y);
            tft.print("(" + comment + ")");
        }
        y += lineSpacing;
    };

    // Color and comment logic for text conditions
    auto conditionColorComment = [](const String &cond)
    {
        if (cond.equalsIgnoreCase("Good"))
            return std::make_pair(TFT_GREEN, "Good");
        if (cond.equalsIgnoreCase("Fair"))
            return std::make_pair(TFT_YELLOW, "Fair");
        if (cond.equalsIgnoreCase("Poor"))
            return std::make_pair(TFT_RED, "Poor");
        if (cond.indexOf("Storm") >= 0)
            return std::make_pair(TFT_RED, "Storm");
        if (cond.indexOf("Unsettled") >= 0)
            return std::make_pair(TFT_ORANGE, "Unsettled");
        return std::make_pair(TFT_WHITE, "");
    };

    printLine("Mag Field", String(solarData.magneticField, 1), TFT_WHITE);

    auto [geoColor, geoComment] = conditionColorComment(solarData.geomagneticField);
    printLine("Geo Field", solarData.geomagneticField, geoColor, geoComment);

    auto [snrColor, snrComment] = conditionColorComment(solarData.signalNoise);
    printLine("S/N", solarData.signalNoise, snrColor, snrComment);

    printLine("foF2", solarData.fof2, TFT_WHITE);
    printLine("MUF Fact", solarData.mufFactor, TFT_WHITE);
    printLine("MUF", solarData.muf, TFT_WHITE);
}

void drawSolarSummaryPage3()
{
    int y = 20;
    int lineSpacing = 18;
    int paragraphSpacing = 6;

    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextSize(1);

    const int titleX = 10;
    const int resultX = 20;

    auto beautifyLocation = [](const String &raw) -> String
    {
        if (raw == "europe")
            return "Europe";
        if (raw == "north_america")
            return "North America";
        if (raw == "northern_hemi")
            return "Northern Hemisphere";
        if (raw == "europe_6m")
            return "Europe 6m";
        if (raw == "europe_4m")
            return "Europe 4m";
        return raw;
    };

    auto annotatePhenomenon = [](const String &name) -> String
    {
        if (name.equalsIgnoreCase("E-Skip"))
            return "E-Skip (Sporadic-E)";
        return name;
    };

    auto vhfColorComment = [](const String &val) -> std::pair<uint16_t, String>
    {
        if (val.equalsIgnoreCase("Band Open"))
            return {TFT_GREEN, "Excellent"};
        if (val.equalsIgnoreCase("Band Weak"))
            return {TFT_YELLOW, "Marginal"};
        if (val.equalsIgnoreCase("Band Closed"))
            return {TFT_RED, "No Propagation"};
        if (val.indexOf("ES") >= 0)
            return {TFT_GREEN, "Sporadic-E Active"};
        return {TFT_WHITE, ""};
    };

    auto printLine = [&](const String &title, const String &value, uint16_t color = TFT_WHITE, const String &comment = "")
    {
        tft.setTextColor(TFT_WHITE, TFT_BLACK); // title line always white
        tft.setCursor(titleX, y);
        tft.print(title);
        y += lineSpacing;

        tft.setTextColor(color, TFT_BLACK);
        tft.setCursor(resultX, y);
        tft.print(value);
        if (!comment.isEmpty())
        {
            tft.print("   (" + comment + ")");
        }
        y += lineSpacing + paragraphSpacing;
    };

    for (int i = 0; i < 5; i++)
    {
        if (solarData.vhfConditions[i].name.isEmpty())
            break;

        String name = annotatePhenomenon(solarData.vhfConditions[i].name);
        String location = beautifyLocation(solarData.vhfConditions[i].location);
        String condition = solarData.vhfConditions[i].condition;

        auto [color, comment] = vhfColorComment(condition);
        String title = name + " (" + location + ")";

        printLine(title, condition, color, comment);
    }
}

void updateWiFiSignalDisplay()
{
    // Pin the font rather than inheriting whatever was selected last.
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextSize(1);

    int rssi = WiFi.RSSI();
    int quality = constrain(2 * (rssi + 100), 0, 100);

    // Static variables to track previous values
    static String lastRSSI = "";
    static String lastSignal = "";

    // Convert new values to strings
    String newRSSI = String(rssi) + " dBm";
    String newSignal = String(quality) + "%";

    // Coordinates based on drawWiFiQualityPage
    int rssiX = 130;
    int rssiY = 15 + 3 * 18;
    int signalX = 130;
    int signalY = 15 + 4 * 18;

    tft.setTextColor(TFT_BLACK, TFT_BLACK); // erase with background color

    // Erase previous RSSI
    tft.setCursor(rssiX, rssiY);
    tft.print(": ");
    tft.print(lastRSSI);

    // Erase previous Signal %
    tft.setCursor(signalX, signalY);
    tft.print(": ");
    tft.print(lastSignal);

    // Draw updated values
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.setCursor(rssiX, rssiY);
    tft.print(": ");
    tft.print(newRSSI);

    tft.setCursor(signalX, signalY);
    tft.print(": ");
    tft.print(newSignal);

    drawNtpStatus();

    // Save current values for next comparison
    lastRSSI = newRSSI;
    lastSignal = newSignal;
}

void drawWiFiSignalMeter(int qualityPercent)
{
    const int meterX = 18;
    const int meterY = 200;
    const int barWidth = 24;
    const int barSpacing = 5;
    const int barHeight = 18;
    const int numBars = 10;

    // Map quality (0–100%) to number of bars (0–10)
    int activeBars = map(qualityPercent, 0, 100, 0, numBars);

    for (int i = 0; i < numBars; i++)
    {
        int x = meterX + i * (barWidth + barSpacing);
        uint16_t color = TFT_DARKGREY;

        if (i < activeBars)
        {
            if (qualityPercent <= 30)
                color = TFT_RED;
            else if (qualityPercent <= 70)
                color = TFT_YELLOW;
            else
                color = TFT_GREEN;
        }

        tft.fillRect(x, meterY, barWidth, barHeight, color);
    }
    // draw a border around the full meter
    tft.drawRect(meterX - 2, meterY - 2, numBars * (barWidth + barSpacing) - barSpacing + 4, barHeight + 4, TFT_LIGHTGREY);
}

// Small badge under the big clock saying which time base is on screen.  It is
// a touch target too - see handleTouchToRotatePage().
// Maidenhead locator for a position.  Eight characters: the field (20 x 10
// degrees), the square (2 x 1), the subsquare (5 x 2.5 arc-minutes) and the
// extended square, which divides the subsquare ten ways again - about 460 by
// 460 metres here.  Case follows the IARU convention: field upper, subsquare
// lower.
static void maidenhead(double latDeg, double lonDeg, char *out, size_t n)
{
    if (n < 9)
    {
        if (n) out[0] = 0;
        return;
    }

    // The scheme is defined on a grid whose origin is the antimeridian at the
    // south pole, so both angles are shifted positive first.
    double lon = fmod(lonDeg + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;

    double lat = latDeg + 90.0;
    if (lat < 0.0) lat = 0.0;
    if (lat >= 180.0) lat = 179.999999;   // the pole itself has no square

    // The subsquare index is kept as a real number, so the extended square is
    // simply its fractional part.  That avoids a second modulus against a step
    // like 1/24 of a degree, which binary floating point cannot hold exactly.
    double lonSub = fmod(lon, 2.0) * 12.0;    // 0..24 across the square
    double latSub = fmod(lat, 1.0) * 24.0;

    out[0] = (char)('A' + (int)(lon / 20.0));
    out[1] = (char)('A' + (int)(lat / 10.0));
    out[2] = (char)('0' + (int)(fmod(lon, 20.0) / 2.0));
    out[3] = (char)('0' + (int)fmod(lat, 10.0));
    out[4] = (char)('a' + (int)lonSub);
    out[5] = (char)('a' + (int)latSub);
    out[6] = (char)('0' + (int)((lonSub - (int)lonSub) * 10.0));
    out[7] = (char)('0' + (int)((latSub - (int)latSub) * 10.0));
    out[8] = 0;
}

void drawBigClockModeBadge()
{
    // No frame: just clear the strip so the previous label cannot show through.
    tft.fillRect(BIGCLOCK_BADGE_X, BIGCLOCK_BADGE_Y,
                 BIGCLOCK_BADGE_W, BIGCLOCK_BADGE_H, TFT_BLACK);

    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(bigClockColour, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(bigClockShowsUtc ? "UTC" : "QTH",
                   BIGCLOCK_BADGE_X + BIGCLOCK_BADGE_W / 2,
                   BIGCLOCK_BADGE_Y + BIGCLOCK_BADGE_H / 2);

    // Put back what the big clock page expects: top-left datum and the big
    // digit font.  setFreeFont() is global state, so a helper that changes it
    // has to hand it back.
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&digits60pt7b);
}

// Every NTP poll goes through here so the sync state is recorded in one place.
bool ntpTick()
{
    bool synced = timeClient.update();
    if (synced)
    {
        lastNtpSyncMs = millis();
        ntpSyncedAtLeastOnce = true;
    }
    return synced;
}

// NTP health, in the place the signal meter used to occupy.  Laid out as one
// more row of the list above: same x positions, same font, same 18 px pitch as
// drawWiFiQualityPage()'s printLine().
void drawNtpStatus()
{
    const int labelX = 10;
    const int valueX = 130;
    const int textY = 15 + 10 * 18;   // the row after "Hostname 2"

    char value[40];

    // The client re-syncs every 15 s, so the age itself says whether the
    // requests are getting through - the row is drawn in the list's own colour.
    if (!ntpSyncedAtLeastOnce)
    {
        snprintf(value, sizeof(value), "no sync yet");
    }
    else
    {
        unsigned long ageSec = (millis() - lastNtpSyncMs) / 1000UL;
        if (ageSec < 120)
            snprintf(value, sizeof(value), "synced %lu s ago", ageSec);
        else if (ageSec < 7200)
            snprintf(value, sizeof(value), "synced %lu min ago", ageSec / 60);
        else
            snprintf(value, sizeof(value), "synced %lu h ago", ageSec / 3600);
    }

    // Same erase-then-redraw idiom the rest of this page uses.
    static String lastValue = "";
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextSize(1);

    tft.setTextColor(TFT_BLACK, TFT_BLACK);
    tft.setCursor(valueX, textY);
    tft.print(": ");
    tft.print(lastValue);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(labelX, textY);
    tft.print(" NTP");

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(valueX, textY);
    tft.print(": ");
    tft.print(value);

    lastValue = value;
}

void drawWiFiQualityPage()
{
    tft.fillScreen(TFT_BLACK);
    // tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setFreeFont(&FreeSans9pt7b);

    tft.setTextSize(1);

    int y = 15;
    const int lineSpacing = 18;

    auto printLine = [&](const String &label, const String &value, uint16_t color = TFT_WHITE)
    {
        tft.setTextColor(color, TFT_BLACK);
        tft.setCursor(10, y);
        tft.print(label);
        tft.setCursor(130, y);
        tft.print(": ");
        tft.print(value);
        y += lineSpacing;
    };

    String ssid = WiFi.SSID();
    String ip = WiFi.localIP().toString();
    String mac = WiFi.macAddress();
    int rssi = WiFi.RSSI();
    int quality = constrain(2 * (rssi + 100), 0, 100);
    String gateway = WiFi.gatewayIP().toString();
    String subnet = WiFi.subnetMask().toString();
    String dns = WiFi.dnsIP().toString();
    String hostname = WiFi.getHostname();

    printLine(" SSID", ssid);
    printLine(" IP", ip);
    printLine(" MAC", mac);
    printLine(" RSSI", String(rssi) + " dBm");
    printLine(" Signal", String(quality) + "%");
    printLine(" Gateway", gateway);
    printLine(" Subnet", subnet);
    printLine(" DNS", dns);
    printLine(" Hostname 1", hostname + ".local");
    printLine(" Hostname 2", "hamclock.local");

    drawNtpStatus();
}
// =============================================================================
// Weather page (page 9)
// =============================================================================

// The bundled fonts only cover 0x20-0x7E, so there is no degree glyph: it is
// drawn as a small circle next to the number instead.
static void drawDegreeMark(int x, int y, uint16_t colour)
{
    tft.drawCircle(x, y, 3, colour);
}

static uint16_t temperatureColour(float c)
{
    if (c < 0.0f)  return TFT_CYAN;
    if (c < 10.0f) return TFT_SKYBLUE;
    if (c < 20.0f) return TFT_GREEN;
    if (c < 28.0f) return TFT_YELLOW;
    if (c < 34.0f) return TFT_ORANGE;
    return TFT_RED;
}

// Layout constants (320x240, rotation 3).  y is the top of the text, which is
// what drawString() uses with TL_DATUM.
static const int WX_RULE1 = 19;
static const int WX_CITY  = 24;
static const int WX_DESC  = 44;
static const int WX_TEMP  = 60;
static const int WX_RULE2 = 92;
static const int WX_ROW0  = 100;
static const int WX_ROWH  = 22;
static const int WX_RULE3 = 206;
static const int WX_FOOT  = 212;

static const int WX_LLABEL = 8;
static const int WX_LVALUE = 88;
static const int WX_RLABEL = 160;
static const int WX_RVALUE = 232;

void drawWeatherPage()
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    tft.setFreeFont(&Orbitron_Medium8pt7b);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("WEATHER", WX_LLABEL, 2);
    tft.drawFastHLine(4, WX_RULE1, 312, TFT_DARKGREY);

    // Open-Meteo needs no key, so the only way to be empty is a failed fetch.
    if (!weather.valid)
    {
        tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.drawString("NO WEATHER DATA", 10, 40);

        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.drawString("Open-Meteo has not answered yet.", 10, 80);
        tft.drawString("It is retried on the interval set at:", 10, 104);

        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString("http://hamclock.local/weather.html", 10, 128);

        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("No account or API key is needed.", 10, 160);
        return;
    }

    char buf[48], val[24];

    // --- location -----------------------------------------------------------
    // 13 px per character in this font: 23 characters is the most that fits.
    snprintf(buf, sizeof(buf), "%.19s%s%s", weather.city,
             weather.country[0] ? ", " : "", weather.country);
    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(buf, 10, WX_CITY);

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    snprintf(buf, sizeof(buf), "%.38s", weather.description);   // 8 px per character
    tft.drawString(buf, 10, WX_DESC);

    // --- temperature --------------------------------------------------------
    uint16_t tc = temperatureColour(weather.temp);
    snprintf(buf, sizeof(buf), "%.1f", weather.temp);
    tft.setFreeFont(&JetBrainsMono_Bold15pt7b);
    tft.setTextColor(tc, TFT_BLACK);
    tft.drawString(buf, 10, WX_TEMP);
    int tw = tft.textWidth(buf);
    drawDegreeMark(10 + tw + 9, WX_TEMP + 6, tc);
    tft.drawString("C", 10 + tw + 18, WX_TEMP);

    snprintf(buf, sizeof(buf), "feels %.1f C", weather.feelsLike);
    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(buf, 312, WX_TEMP + 10);
    tft.setTextDatum(TL_DATUM);

    tft.drawFastHLine(4, WX_RULE2, 312, TFT_DARKGREY);

    // --- two-column detail grid --------------------------------------------
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);

    auto cell = [&](int row, int labelX, int valueX, const char *label, const char *value)
    {
        int y = WX_ROW0 + row * WX_ROWH;
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString(label, labelX, y);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(value, valueX, y);
    };

    // Top row: the day's temperature range and the humidity.
    // Whole degrees: "-40/-30" is the widest this can get and still clears
    // the right-hand column.
    if (weather.dailyValid)
        snprintf(val, sizeof(val), "%.0f/%.0f", weather.tempMin, weather.tempMax);
    else
        snprintf(val, sizeof(val), "-");
    cell(0, WX_LLABEL, WX_LVALUE, "Min/Max", val);
    snprintf(val, sizeof(val), "%d %%", weather.humidity);
    cell(0, WX_RLABEL, WX_RVALUE, "Humidity", val);

    snprintf(val, sizeof(val), "%d hPa", weather.pressure);
    cell(1, WX_LLABEL, WX_LVALUE, "Pressure", val);
    snprintf(val, sizeof(val), "%d %%", weather.clouds);
    cell(1, WX_RLABEL, WX_RVALUE, "Clouds", val);

    snprintf(val, sizeof(val), "%.1f %s", weather.windSpeed, weather.windDir);
    cell(2, WX_LLABEL, WX_LVALUE, "Wind m/s", val);
    if (weather.windGust > 0.05f)
        snprintf(val, sizeof(val), "%.1f", weather.windGust);
    else
        snprintf(val, sizeof(val), "-");
    cell(2, WX_RLABEL, WX_RVALUE, "Gust m/s", val);

    snprintf(val, sizeof(val), "%.0f km", weather.visibilityKm);
    cell(3, WX_LLABEL, WX_LVALUE, "Visib", val);
    if (weather.rainMM > 0.005f)
        snprintf(val, sizeof(val), "%.1f mm", weather.rainMM);
    else
        snprintf(val, sizeof(val), "-");
    cell(3, WX_RLABEL, WX_RVALUE, "Rain", val);

    // Bottom row: the sun times, already local and 24-hour.
    cell(4, WX_LLABEL, WX_LVALUE, "Sunrise", weather.sunrise);
    cell(4, WX_RLABEL, WX_RVALUE, "Sunset", weather.sunset);

    tft.drawFastHLine(4, WX_RULE3, 312, TFT_DARKGREY);

    updateWeatherPageClock();
}

// Redrawn once a second: only the clock and the data age actually change.
void updateWeatherPageClock()
{
    char buf[40], padded[40];

    long localEpoch = timeClient.getEpochTime() + (tOffset * 3600);
    time_t t = (time_t)localEpoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d LOC", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(buf, 314, 4);
    tft.setTextDatum(TL_DATUM);

    if (!weather.valid) return;

    long ageMin = ((long)timeClient.getEpochTime() - weather.fetchedUnix) / 60;
    if (ageMin < 0) ageMin = 0;
    snprintf(buf, sizeof(buf), "Updated %ld min ago", ageMin);
    snprintf(padded, sizeof(padded), "%-26s", buf);

    // The fetch runs every five minutes, so anything much older means the
    // requests are failing.
    tft.setTextColor(ageMin > 15 ? TFT_ORANGE : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(padded, WX_LLABEL, WX_FOOT);
}

// =============================================================================
// NCDXF/IARU beacon tracker (page 10)
//
// Eighteen beacons share five frequencies in a strictly timed round.  Each one
// transmits for ten seconds, then steps up to the next band, so the whole set
// repeats every three minutes.  The schedule is anchored to 00:00:00 UTC and
// 180 divides a day exactly, which makes the unix epoch a usable clock for it:
//
//     slot   = (epoch % 180) / 10          0..17
//     beacon = (slot - bandIndex) mod 18
//
// Nothing is fetched - accurate time and this table are the whole mechanism.
// =============================================================================

struct BeaconSite
{
    const char *call;
    const char *location;   // kept to 12 characters, the width of its column
};

// Canonical transmission order.  Position in this list *is* the schedule, so
// entries must not be reordered.
static const BeaconSite beaconSites[18] = {
    {"4U1UN",  "New York"},     {"VE8AT",  "Inuvik NWT"},  {"W6WX",   "California"},
    {"KH6RS",  "Maui HI"},      {"ZL6B",   "Masterton"},   {"VK6RBP", "Rolystone"},
    {"JA2IGY", "Mt Asama"},     {"RR9O",   "Novosibirsk"}, {"VR2B",   "Hong Kong"},
    {"4S7B",   "Colombo"},      {"ZS6DN",  "Pretoria"},    {"5Z4B",   "Kariobangi"},
    {"4X6TU",  "Tel Aviv"},     {"OH2B",   "Lohja"},       {"CS3B",   "Madeira"},
    {"LU4AA",  "Buenos Aires"}, {"OA4B",   "Lima"},        {"YV5B",   "Caracas"}};

struct BeaconBand
{
    const char *band;
    const char *freq;
};

static const BeaconBand beaconBands[5] = {{"20m", "14.100"}, {"17m", "18.110"},
                                          {"15m", "21.150"}, {"12m", "24.930"},
                                          {"10m", "28.200"}};

// Which beacon is on `band` at this instant, and which one takes over next.
static uint8_t beaconAt(long epoch, uint8_t band)
{
    long slot = (epoch % 180L) / 10L;
    return (uint8_t)(((slot - band) % 18 + 18) % 18);
}

static const int BX_BAND = 6;
static const int BX_FREQ = 44;
static const int BX_CALL = 100;
static const int BX_LOC  = 156;
static const int BX_NEXT = 258;

static const int BX_RULE1 = 19;
static const int BX_COLHDR = 26;
static const int BX_RULE2 = 40;
static const int BX_ROW0 = 52;
static const int BX_ROWH = 28;
static const int BX_RULE3 = 186;
static const int BX_FOOT1 = 194;
static const int BX_FOOT2 = 214;

// =============================================================================
// Sun and moon (page 11)
//
// Everything here is computed, not fetched: given the QTH and the time, the
// ephemeris in lib/Sgp4 answers where both bodies are, when they cross the
// horizon, and how much of the moon is lit.  The page therefore works with no
// network at all, and it keeps working when Open-Meteo does not answer.
//
// The rise and set times are for the local calendar day, the window almanacs
// use, so "today's sunset" stays on screen after it has passed rather than
// jumping to tomorrow's.
// =============================================================================

static const int SM_RULE1  = 19;
static const int SM_SUNHDR = 24;
static const int SM_SROW0  = 42;
static const int SM_RULE2  = 104;
static const int SM_MOONHDR = 110;
static const int SM_MROW0  = 128;
static const int SM_ROWH   = 20;
static const int SM_RULE3  = 208;
static const int SM_FOOT   = 214;

static const int SM_LLABEL = 8;
static const int SM_LVALUE = 92;
static const int SM_RLABEL = 168;
static const int SM_RVALUE = 250;

// Start of the local calendar day containing `utcNow`, as a unix time.
static double localDayStart(long utcNow)
{
    long shift = (long)tOffset * 3600L;
    long localNow = utcNow + shift;   // 'local' is a macro inside PNGdec
    long day = localNow - ((localNow % 86400L) + 86400L) % 86400L;
    return (double)(day - shift);
}

// "06:08" in local time, or "--:--" when the event does not happen.
static void localHm(bool valid, double unixT, char *out, size_t n)
{
    if (!valid)
    {
        snprintf(out, n, "--:--");
        return;
    }
    time_t t = (time_t)(unixT + (double)tOffset * 3600.0);
    struct tm tm_;
    gmtime_r(&t, &tm_);
    snprintf(out, n, "%02d:%02d", tm_.tm_hour, tm_.tm_min);
}

static void hoursMinutes(double seconds, char *out, size_t n)
{
    if (seconds < 0) seconds = 0;
    long m = (long)(seconds / 60.0);
    snprintf(out, n, "%ldh%02ldm", m / 60, m % 60);
}

static const char *moonPhaseName(double frac, bool waxing)
{
    if (frac < 0.02) return "New moon";
    if (frac > 0.98) return "Full moon";
    if (fabs(frac - 0.5) < 0.03) return waxing ? "First quarter" : "Last quarter";
    if (frac < 0.5) return waxing ? "Waxing crescent" : "Waning crescent";
    return waxing ? "Waxing gibbous" : "Waning gibbous";
}

// The almanac part changes once a day, so it is worked out once and kept.
struct SkyAlmanac
{
    long   dayKey = -1;          // local day this was computed for
    sgp4::RiseSet sun;
    sgp4::RiseSet moon;
    double nextSunEvent = 0;     // next horizon crossing after "now"
    bool   nextSunIsRise = false;
    double nextMoonEvent = 0;
    bool   nextMoonIsRise = false;
    bool   haveNextSun = false, haveNextMoon = false;
};

static SkyAlmanac skyAlmanac;

// Earliest rise or set still ahead of `nowUnix`.  Today may have none left,
// which is why tomorrow is searched too.
static bool nextSkyEvent(int body, double nowUnix, double dayStart,
                         double latRad, double lonRad, double altKm,
                         double &whenUnix, bool &isRise)
{
    bool found = false;
    for (int d = 0; d < 2; d++)
    {
        sgp4::RiseSet rs;
        sgp4::riseSet(body, dayStart + d * 86400.0, latRad, lonRad, altKm, rs);

        const double cands[2] = {rs.riseUnix, rs.setUnix};
        const bool   valid[2] = {rs.riseValid, rs.setValid};
        for (int k = 0; k < 2; k++)
        {
            if (!valid[k] || cands[k] <= nowUnix) continue;
            if (!found || cands[k] < whenUnix)
            {
                whenUnix = cands[k];
                isRise = (k == 0);
                found = true;
            }
        }
        if (found) break;   // today's events, if any remain, always win
    }
    return found;
}

static void refreshSkyAlmanac(double nowUnix, bool force)
{
    double dayStart = localDayStart((long)nowUnix);
    long key = (long)(dayStart / 86400.0);

    // The rise and set times only change at local midnight, but the countdown
    // has to be re-aimed the moment the event it was counting to goes by -
    // otherwise it sits at zero until midnight.
    bool dayChanged = (key != skyAlmanac.dayKey);
    bool sunPassed  = skyAlmanac.haveNextSun  && nowUnix >= skyAlmanac.nextSunEvent;
    bool moonPassed = skyAlmanac.haveNextMoon && nowUnix >= skyAlmanac.nextMoonEvent;
    if (!force && !dayChanged && !sunPassed && !moonPassed) return;

    double latRad = latitude * DEG_TO_RAD;
    double lonRad = longitude * DEG_TO_RAD;
    double altKm  = satellitesSiteAltitudeM() / 1000.0;

    if (force || dayChanged)
    {
        sgp4::riseSet(sgp4::SKY_SUN,  dayStart, latRad, lonRad, altKm, skyAlmanac.sun);
        sgp4::riseSet(sgp4::SKY_MOON, dayStart, latRad, lonRad, altKm, skyAlmanac.moon);
    }

    skyAlmanac.haveNextSun = nextSkyEvent(sgp4::SKY_SUN, nowUnix, dayStart,
                                          latRad, lonRad, altKm,
                                          skyAlmanac.nextSunEvent, skyAlmanac.nextSunIsRise);
    skyAlmanac.haveNextMoon = nextSkyEvent(sgp4::SKY_MOON, nowUnix, dayStart,
                                           latRad, lonRad, altKm,
                                           skyAlmanac.nextMoonEvent, skyAlmanac.nextMoonIsRise);
    skyAlmanac.dayKey = key;
}

void drawSunMoonPage(bool fullRedraw)
{
    static long lastSecond = -1;

    long utcNow = (long)timeClient.getEpochTime();
    if (!fullRedraw && utcNow == lastSecond) return;
    lastSecond = utcNow;

    if (fullRedraw)
    {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TL_DATUM);

        tft.setFreeFont(&Orbitron_Medium8pt7b);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString("SUN & MOON", SM_LLABEL, 2);
        tft.drawFastHLine(4, SM_RULE1, 312, TFT_DARKGREY);

        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("SUN", SM_LLABEL, SM_SUNHDR);
        tft.drawFastHLine(4, SM_RULE2, 312, TFT_DARKGREY);
        tft.setTextColor(TFT_SILVER, TFT_BLACK);
        tft.drawString("MOON", SM_LLABEL, SM_MOONHDR);
        tft.drawFastHLine(4, SM_RULE3, 312, TFT_DARKGREY);

        // Labels never change; only the values are repainted each second.
        tft.setFreeFont(&UbuntuMono_Regular8pt7b);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        const char *sunLabels[3][2] = {{"Elevation", "Azimuth"},
                                       {"Rise", "Set"},
                                       {"Grey line", "Day len"}};
        for (int r = 0; r < 3; r++)
        {
            tft.drawString(sunLabels[r][0], SM_LLABEL, SM_SROW0 + r * SM_ROWH);
            tft.drawString(sunLabels[r][1], SM_RLABEL, SM_SROW0 + r * SM_ROWH);
        }
        const char *moonLabels[4][2] = {{"Elevation", "Azimuth"},
                                        {"Rise", "Set"},
                                        {"Illum", "Dist km"},
                                        {"Phase", ""}};
        for (int r = 0; r < 4; r++)
        {
            tft.drawString(moonLabels[r][0], SM_LLABEL, SM_MROW0 + r * SM_ROWH);
            if (moonLabels[r][1][0])
                tft.drawString(moonLabels[r][1], SM_RLABEL, SM_MROW0 + r * SM_ROWH);
        }
    }

    refreshSkyAlmanac((double)utcNow, fullRedraw);

    double latRad = latitude * DEG_TO_RAD;
    double lonRad = longitude * DEG_TO_RAD;
    double altKm  = satellitesSiteAltitudeM() / 1000.0;
    double jd = sgp4::jdFromUnix((double)utcNow);

    double rsun[3], sunAz, sunEl, sunRange;
    sgp4::sunEci(jd, rsun);
    sgp4::topocentric(rsun, jd, latRad, lonRad, altKm, sunAz, sunEl, sunRange);

    sgp4::MoonInfo moon;
    sgp4::moonInfo(jd, latRad, lonRad, altKm, moon);

    char val[24];
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextDatum(TL_DATUM);

    // Values are padded to a fixed width so the opaque background wipes what
    // was there before - no flicker from clearing rectangles first.
    auto value = [&](int x, int y, const char *text, uint16_t colour)
    {
        char padded[24];
        snprintf(padded, sizeof(padded), "%-8.8s", text);
        tft.setTextColor(colour, TFT_BLACK);
        tft.drawString(padded, x, y);
    };

    // A degree mark after a number that has just been drawn 8 px per character.
    auto degreeAfter = [&](int x, int y, const char *text, uint16_t colour)
    {
        drawDegreeMark(x + (int)strlen(text) * 8 + 4, y + 6, colour);
    };

    // --- sun ----------------------------------------------------------------
    uint16_t sunColour = (sunEl >= 0.0) ? TFT_YELLOW : TFT_DARKGREY;

    snprintf(val, sizeof(val), "%+.1f", sunEl);
    value(SM_LVALUE, SM_SROW0, val, sunColour);
    degreeAfter(SM_LVALUE, SM_SROW0, val, sunColour);

    snprintf(val, sizeof(val), "%.1f", sunAz);
    value(SM_RVALUE, SM_SROW0, val, TFT_WHITE);
    degreeAfter(SM_RVALUE, SM_SROW0, val, TFT_WHITE);

    localHm(skyAlmanac.sun.riseValid, skyAlmanac.sun.riseUnix, val, sizeof(val));
    value(SM_LVALUE, SM_SROW0 + SM_ROWH, val, TFT_WHITE);
    localHm(skyAlmanac.sun.setValid, skyAlmanac.sun.setUnix, val, sizeof(val));
    value(SM_RVALUE, SM_SROW0 + SM_ROWH, val, TFT_WHITE);

    // Grey line: the band around sunrise and sunset when the terminator runs
    // through the path and the low bands open up over very long distances.
    bool greyLine = fabs(sunEl) <= 6.0;
    value(SM_LVALUE, SM_SROW0 + 2 * SM_ROWH, greyLine ? "YES" : "no",
          greyLine ? TFT_GREEN : TFT_DARKGREY);

    if (skyAlmanac.sun.riseValid && skyAlmanac.sun.setValid)
        hoursMinutes(skyAlmanac.sun.setUnix - skyAlmanac.sun.riseUnix, val, sizeof(val));
    else
        snprintf(val, sizeof(val), "-");
    value(SM_RVALUE, SM_SROW0 + 2 * SM_ROWH, val, TFT_WHITE);

    // --- moon ---------------------------------------------------------------
    uint16_t moonColour = (moon.elDeg >= 0.0) ? TFT_SILVER : TFT_DARKGREY;

    snprintf(val, sizeof(val), "%+.1f", moon.elDeg);
    value(SM_LVALUE, SM_MROW0, val, moonColour);
    degreeAfter(SM_LVALUE, SM_MROW0, val, moonColour);

    snprintf(val, sizeof(val), "%.1f", moon.azDeg);
    value(SM_RVALUE, SM_MROW0, val, TFT_WHITE);
    degreeAfter(SM_RVALUE, SM_MROW0, val, TFT_WHITE);

    localHm(skyAlmanac.moon.riseValid, skyAlmanac.moon.riseUnix, val, sizeof(val));
    value(SM_LVALUE, SM_MROW0 + SM_ROWH, val, TFT_WHITE);
    localHm(skyAlmanac.moon.setValid, skyAlmanac.moon.setUnix, val, sizeof(val));
    value(SM_RVALUE, SM_MROW0 + SM_ROWH, val, TFT_WHITE);

    snprintf(val, sizeof(val), "%.0f %%", moon.illuminatedFrac * 100.0);
    value(SM_LVALUE, SM_MROW0 + 2 * SM_ROWH, val, TFT_WHITE);
    snprintf(val, sizeof(val), "%.0f", moon.distanceKm);
    value(SM_RVALUE, SM_MROW0 + 2 * SM_ROWH, val, TFT_WHITE);

    // The phase name is wider than a value column, and nothing sits to its
    // right, so it is drawn on its own with the age tacked on the end.
    char phase[32], padded[40];
    snprintf(phase, sizeof(phase), "%s  %.1f d",
             moonPhaseName(moon.illuminatedFrac, moon.waxing), moon.ageDays);
    snprintf(padded, sizeof(padded), "%-28.28s", phase);
    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.drawString(padded, SM_LVALUE, SM_MROW0 + 3 * SM_ROWH);

    // --- countdown footer ---------------------------------------------------
    char sunPart[24] = "", moonPart[24] = "", foot[48], footPad[48];
    if (skyAlmanac.haveNextSun)
    {
        char d[12];
        hoursMinutes(skyAlmanac.nextSunEvent - (double)utcNow, d, sizeof(d));
        snprintf(sunPart, sizeof(sunPart), "%s %s",
                 skyAlmanac.nextSunIsRise ? "Sunrise" : "Sunset", d);
    }
    if (skyAlmanac.haveNextMoon)
    {
        char d[12];
        hoursMinutes(skyAlmanac.nextMoonEvent - (double)utcNow, d, sizeof(d));
        snprintf(moonPart, sizeof(moonPart), "%s %s",
                 skyAlmanac.nextMoonIsRise ? "Moonrise" : "Moonset", d);
    }
    snprintf(foot, sizeof(foot), "%s   %s", sunPart, moonPart);
    snprintf(footPad, sizeof(footPad), "%-38.38s", foot);
    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(footPad, SM_LLABEL, SM_FOOT);

    // Local time top right, so the rise and set columns read unambiguously.
    time_t lt = (time_t)(utcNow + tOffset * 3600L);
    struct tm tm_;
    gmtime_r(&lt, &tm_);
    snprintf(val, sizeof(val), "%02d:%02d:%02d QTH", tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(val, 314, 4);
    tft.setTextDatum(TL_DATUM);
}

void drawBeaconPage(bool fullRedraw)
{
    static long lastSlot = -1;
    static long lastSecond = -1;

    long epoch = (long)timeClient.getEpochTime();
    long slot = (epoch % 180L) / 10L;

    if (fullRedraw)
    {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TL_DATUM);

        tft.setFreeFont(&Orbitron_Medium8pt7b);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString("NCDXF BEACONS", BX_BAND, 2);
        tft.drawFastHLine(4, BX_RULE1, 312, TFT_DARKGREY);

        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("BAND", BX_BAND, BX_COLHDR);
        tft.drawString("FREQ", BX_FREQ, BX_COLHDR);
        tft.drawString("CALL", BX_CALL, BX_COLHDR);
        tft.drawString("LOCATION", BX_LOC, BX_COLHDR);
        tft.drawString("NEXT", BX_NEXT, BX_COLHDR);
        tft.drawFastHLine(4, BX_RULE2, 312, TFT_DARKGREY);
        tft.drawFastHLine(4, BX_RULE3, 312, TFT_DARKGREY);

        // The band and frequency columns never change.
        tft.setFreeFont(&UbuntuMono_Regular8pt7b);
        for (uint8_t b = 0; b < 5; b++)
        {
            int y = BX_ROW0 + b * BX_ROWH;
            tft.setTextColor(TFT_CYAN, TFT_BLACK);
            tft.drawString(beaconBands[b].band, BX_BAND, y);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString(beaconBands[b].freq, BX_FREQ, y);
        }

        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("call @22wpm then 100/10/1/0.1 W dashes", BX_BAND, BX_FOOT2);

        lastSlot = -1;      // force the callsigns to be painted
        lastSecond = -1;
    }

    // Callsigns only move every ten seconds.
    if (slot != lastSlot)
    {
        lastSlot = slot;
        tft.setTextDatum(TL_DATUM);
        tft.setFreeFont(&UbuntuMono_Regular8pt7b);

        for (uint8_t b = 0; b < 5; b++)
        {
            int y = BX_ROW0 + b * BX_ROWH;
            const BeaconSite &now = beaconSites[beaconAt(epoch, b)];
            const BeaconSite &next = beaconSites[beaconAt(epoch + 10, b)];

            char buf[16];

            snprintf(buf, sizeof(buf), "%-6.6s", now.call);
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString(buf, BX_CALL, y);

            snprintf(buf, sizeof(buf), "%-12.12s", now.location);
            tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            tft.drawString(buf, BX_LOC, y);

            snprintf(buf, sizeof(buf), "%-6.6s", next.call);
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.drawString(buf, BX_NEXT, y);
        }
    }

    if (epoch != lastSecond)
    {
        lastSecond = epoch;

        struct tm tm_;
        time_t t = (time_t)epoch;
        gmtime_r(&t, &tm_);

        char buf[24];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d UTC", tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setTextDatum(TR_DATUM);
        tft.drawString(buf, 314, 4);
        tft.setTextDatum(TL_DATUM);

        long into = epoch % 10L;
        char foot[48];
        snprintf(foot, sizeof(foot), "CW  %lds in slot  next %lds  round %ld/18",
                 into, 10 - into, slot + 1);
        // Fixed width, hard-truncated: 38 characters is what the row holds.
        char padded[48];
        snprintf(padded, sizeof(padded), "%-38.38s", foot);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.drawString(padded, BX_BAND, BX_FOOT1);
    }
}

void drawQRCode(const char *text, int x, int y, int scale)
{
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, text);

    for (uint8_t row = 0; row < qrcode.size; row++)
    {
        for (uint8_t col = 0; col < qrcode.size; col++)
        {
            int color = qrcode_getModule(&qrcode, col, row) ? TFT_BLACK : TFT_WHITE;
            tft.fillRect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}
void drawQRcodeInstructions()
{
    ;
    // Draw QR instructions
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Wi-Fi Configuration", 160, 10, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawCentreString("1", 80, 38, 4);
    tft.drawCentreString("2", 160 + 80, 38, 4);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.drawCentreString("Scan to Join", 80, 85, 2);
    drawQRCode("WIFI:T:nopass;S:HB9IIUSetup;;", 80 - 116 / 2, 105, 4);

    tft.drawCentreString("Open config page", 240, 85, 2);
    drawQRCode("http://192.168.4.1", 240 - 116 / 2, 105, 4);
}

void handleRootCaptivePortal()
{
    server.send_P(200, "text/html", index_html);
}

void handleScanCaptivePortal()
{
    Serial.println("Returning scan list");
    String json = "[";
    for (int i = 0; i < scanCount; i++)
    {
        if (i > 0)
            json += ",";
        json += "\"" + WiFi.SSID(i) + "\"";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handleSaveCaptivePortal()
{
    Serial.println("Saving");

    // Networks are added one at a time through /wifiadd; this finishes the
    // session: remember the phone's clock and reboot into station mode.
    if (server.hasArg("ssid") && server.arg("ssid").length())
        wifiNetAdd(server.arg("ssid"),
                   server.hasArg("password") ? server.arg("password") : String(""));

    if (wifiNetCount() == 0)
    {
        server.send(400, "text/plain", "Add at least one network first.");
        return;
    }

    if (server.hasArg("time"))
    {
        String timeStr = server.arg("time");

        // --- Parse JSON from "time" ---
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, timeStr);

        if (!err)
        {
            String isoTime = doc["iso"].as<String>();
            unsigned long long unixMillis = doc["unix"].as<unsigned long long>();
            int offsetMinutes = doc["offset"].as<int>();

            prefs.begin("iPhonetime", false);
            prefs.putString("iso", isoTime);
            prefs.putLong64("unix", unixMillis); // store as 64-bit
            prefs.putInt("offsetMinutes", offsetMinutes);
            prefs.end();

            Serial.printf("✅ Saved Phone Time:\n   ISO: %s\n   Unix: %llu\n   Offset: %d minutes\n",
                          isoTime.c_str(), unixMillis, offsetMinutes);
        }
        else
        {
            Serial.println("⚠️ Failed to parse time JSON, saving raw string instead.");
            prefs.begin("iPhonetime", false);
            prefs.putString("localTime", timeStr);
            prefs.end();
        }

    }

    server.send_P(200, "text/html", html_success);
    delay(500);
    ESP.restart();
}

void startConfigurationPortal()
{
    Serial.println("🌐 Starting Captive Portal...");
    drawQRcodeInstructions();

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("HB9IIUSetup");
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    // DNS: redirect all domains to our AP IP
    dnsServer.start(DNS_PORT, "*", apIP);

    Serial.println("📡 Scanning for networks...");
    WiFi.scanDelete();
    scanCount = WiFi.scanNetworks();
    Serial.printf("📶 Found %d networks\n", scanCount);

    // Routes
    server.on("/", handleRootCaptivePortal);
    server.on("/scan", handleScanCaptivePortal);
    server.on("/save", HTTP_POST, handleSaveCaptivePortal);
    wifiRegisterRoutes();
    // The portal has no internet, but the theme is on the device itself.
    server.serveStatic("/hamclock.css", SPIFFS, "/hamclock.css");
    server.onNotFound([]()
                      {
    Serial.print("Unknown request: ");
    Serial.println(server.uri());

    // Redirect everything to the root captive portal page
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", ""); });
    server.begin();
    Serial.println("🚀 Web server started.");
}

// =============================================================================
// Saved WiFi networks
//
// Preferences namespace "wifi": "n" holds the count, "s0".."s4" / "p0".."p4"
// the credentials.  The single "ssid"/"pass" pair older firmware wrote is
// migrated into slot 0 on first use.
// =============================================================================
#define WIFI_MAX_NETS 5

static void wifiMigrateLegacy()
{
    prefs.begin("wifi", false);
    if (prefs.getUChar("n", 0xFF) == 0xFF)
    {
        String s = prefs.getString("ssid", "");
        String p = prefs.getString("pass", "");
        if (s.length())
        {
            prefs.putString("s0", s);
            prefs.putString("p0", p);
            prefs.putUChar("n", 1);
            Serial.printf("\U0001F4F6 Migrated saved network '%s' into the list\n", s.c_str());
        }
        else
        {
            prefs.putUChar("n", 0);
        }
    }
    prefs.end();
}

static uint8_t wifiNetCount()
{
    prefs.begin("wifi", true);
    uint8_t n = prefs.getUChar("n", 0);
    prefs.end();
    return n > WIFI_MAX_NETS ? WIFI_MAX_NETS : n;
}

static void wifiNetGet(uint8_t i, String &ssid, String &pass)
{
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "s%u", i);
    snprintf(kp, sizeof(kp), "p%u", i);
    prefs.begin("wifi", true);
    ssid = prefs.getString(ks, "");
    pass = prefs.getString(kp, "");
    prefs.end();
}

// Adding an SSID that is already stored replaces its password and keeps its
// place in the list.  Returns false only when the list is full.
static bool wifiNetAdd(const String &ssid, const String &pass)
{
    if (ssid.isEmpty()) return false;

    prefs.begin("wifi", false);
    uint8_t n = prefs.getUChar("n", 0);
    if (n > WIFI_MAX_NETS) n = WIFI_MAX_NETS;

    int slot = -1;
    for (uint8_t i = 0; i < n; i++)
    {
        char ks[8];
        snprintf(ks, sizeof(ks), "s%u", i);
        if (prefs.getString(ks, "") == ssid) { slot = i; break; }
    }
    if (slot < 0)
    {
        if (n >= WIFI_MAX_NETS) { prefs.end(); return false; }
        slot = n++;
    }

    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "s%d", slot);
    snprintf(kp, sizeof(kp), "p%d", slot);
    prefs.putString(ks, ssid);
    prefs.putString(kp, pass);
    prefs.putUChar("n", n);
    prefs.end();
    return true;
}

static bool wifiNetRemove(const String &ssid)
{
    prefs.begin("wifi", false);
    uint8_t n = prefs.getUChar("n", 0);
    if (n > WIFI_MAX_NETS) n = WIFI_MAX_NETS;

    int slot = -1;
    for (uint8_t i = 0; i < n; i++)
    {
        char ks[8];
        snprintf(ks, sizeof(ks), "s%u", i);
        if (prefs.getString(ks, "") == ssid) { slot = i; break; }
    }
    if (slot < 0) { prefs.end(); return false; }

    // Close the gap so the list stays contiguous.
    for (uint8_t i = slot; i + 1 < n; i++)
    {
        char ks[8], kp[8], ks2[8], kp2[8];
        snprintf(ks,  sizeof(ks),  "s%u", i);
        snprintf(kp,  sizeof(kp),  "p%u", i);
        snprintf(ks2, sizeof(ks2), "s%u", i + 1);
        snprintf(kp2, sizeof(kp2), "p%u", i + 1);
        prefs.putString(ks, prefs.getString(ks2, ""));
        prefs.putString(kp, prefs.getString(kp2, ""));
    }
    prefs.putUChar("n", (uint8_t)(n - 1));
    prefs.end();
    return true;
}

// The same four endpoints back both the captive portal and /wifi.html, so the
// two never drift apart.
static void wifiRegisterRoutes()
{
    server.on("/wifilist", HTTP_GET, []()
              {
        JsonDocument doc;
        doc["max"] = WIFI_MAX_NETS;
        doc["connected"] = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
        JsonArray arr = doc["nets"].to<JsonArray>();
        uint8_t n = wifiNetCount();
        for (uint8_t i = 0; i < n; i++) {
            String ssid, pass;
            wifiNetGet(i, ssid, pass);
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = ssid;
            o["hasPass"] = pass.length() > 0;
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out); });

    server.on("/wifiadd", HTTP_POST, []()
              {
        JsonDocument doc;
        if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        String ssid = doc["ssid"] | "";
        String pass = doc["pass"] | "";
        if (ssid.isEmpty()) {
            server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
            return;
        }
        if (!wifiNetAdd(ssid, pass)) {
            server.send(409, "application/json", "{\"error\":\"list full\"}");
            return;
        }
        Serial.printf("\U0001F4F6 Saved network '%s'\n", ssid.c_str());
        server.send(200, "application/json", "{\"status\":\"ok\"}"); });

    server.on("/wifidel", HTTP_POST, []()
              {
        JsonDocument doc;
        if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        String ssid = doc["ssid"] | "";
        if (!wifiNetRemove(ssid)) {
            server.send(404, "application/json", "{\"error\":\"not found\"}");
            return;
        }
        Serial.printf("\U0001F4F6 Removed network '%s'\n", ssid.c_str());
        server.send(200, "application/json", "{\"status\":\"ok\"}"); });

    server.on("/wifiscan", HTTP_GET, []()
              {
        WiFi.scanDelete();
        int n = WiFi.scanNetworks();
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = WiFi.SSID(i);
            o["rssi"] = WiFi.RSSI(i);
            o["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out); });
}

bool tryToConnectSavedWiFi()
{
    wifiMigrateLegacy();

    uint8_t netCount = wifiNetCount();
    Serial.printf("🔍 %u saved network(s)\n", netCount);
    if (netCount == 0)
    {
        Serial.println("⚠️ No saved credentials found.");
        return false;
    }

    // Scan first: trying a network that is not even in range wastes the whole
    // per-network timeout, and the strongest one is the one worth trying first.
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    int found = WiFi.scanNetworks();
    Serial.printf("📡 %d network(s) in range\n", found);

    struct Cand { uint8_t idx; int rssi; };
    Cand order[WIFI_MAX_NETS];
    for (uint8_t i = 0; i < netCount; i++)
    {
        String s, p;
        wifiNetGet(i, s, p);
        order[i].idx = i;
        order[i].rssi = -999;
        for (int j = 0; j < found; j++)
            if (WiFi.SSID(j) == s && WiFi.RSSI(j) > order[i].rssi)
                order[i].rssi = WiFi.RSSI(j);
    }
    for (uint8_t i = 1; i < netCount; i++)   // strongest first
    {
        Cand key = order[i];
        int j = i - 1;
        while (j >= 0 && order[j].rssi < key.rssi) { order[j + 1] = order[j]; j--; }
        order[j + 1] = key;
    }

    for (uint8_t c = 0; c < netCount; c++)
    {
        String ssid, pass;
        wifiNetGet(order[c].idx, ssid, pass);
        if (ssid.isEmpty()) continue;

        bool inRange = order[c].rssi > -999;
        int  attempts = inRange ? 24 : 10;   // 12 s if seen, 5 s for a hidden SSID

        Serial.printf("🔌 Connecting to '%s' (%s)...\n", ssid.c_str(),
                      inRange ? "in range" : "not seen in scan");
        WiFi.begin(ssid.c_str(), pass.c_str());

        for (int i = 0; i < attempts; i++)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.printf("✅ Connected to '%s'\n", ssid.c_str());
                Serial.print("📶 IP Address: ");
                Serial.println(WiFi.localIP());

                // 👉 Override DNS after DHCP has completed
                IPAddress dns2(8, 8, 8, 8);
                IPAddress dns1(1, 1, 1, 1);
                if (!WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2))
                {
                    Serial.println("⚠️ Failed to set DNS servers.");
                }

                Serial.print("🌍 DNS #1: ");
                Serial.println(WiFi.dnsIP(0));
                Serial.print("🌍 DNS #2: ");
                Serial.println(WiFi.dnsIP(1));
                return true;
            }
            Serial.print(".");
            delay(500);
        }

        Serial.println();
        WiFi.disconnect(true);
        delay(200);
    }

    Serial.println("❌ None of the saved networks worked.");
    return false;
}

void checkIfscreenIsTouchedDuringStartUpForFactoryReset()
{
    // Wait up to 1 second to detect if screen touched
    bool screenWasTouched = false;
    uint32_t screenScanTimeStart = millis();
    while (millis() - screenScanTimeStart < 1000)
    {
        uint16_t x, y;
        if (readTouchPoint(&x, &y))
        {
            screenWasTouched = true;
            break;
        }
    }
    if (!screenWasTouched)
        return;

    // Local helpers encapsulated inside this function
    auto drawButton = [&](int x, int y, const char *label,
                          uint16_t bgColor, uint16_t textColor = TFT_WHITE)
    {
        tft.fillRoundRect(x, y, 120, 40, 6, bgColor);
        tft.drawRoundRect(x, y, 120, 40, 6, TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(textColor, bgColor);
        tft.drawString(label, x + 60, y + 20, 4);
    };

    auto isInsideButton = [&](int tx, int ty, int bx, int by)
    {
        return (tx >= bx && tx <= bx + 120 &&
                ty >= by && ty <= by + 40);
    };

    // Draw UI
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Factory Reset ?", tft.width() / 2, 35, 4);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("⚠ All settings will be lost", tft.width() / 2, 70, 2);
    tft.drawString("This action cannot be undone", tft.width() / 2, 100, 2);

    const int BTN_X = (tft.width() / 2 - 60);
    const int BTN_Y1 = 130;
    const int BTN_Y2 = BTN_Y1 + 40 + 20;

    drawButton(BTN_X, BTN_Y1, "Proceed", TFT_GREEN, TFT_BLACK);
    drawButton(BTN_X, BTN_Y2, "Exit", TFT_RED, TFT_WHITE);

    // readTouchPoint() already returns coordinates in display orientation
    bool invertY = false;
    bool invertX = false;

    // Modal loop
    while (true)
    {
        uint16_t x, y;
        if (readTouchPoint(&x, &y))
        {
            if (invertX)
                x = tft.width() - x;
            if (invertY)
                y = tft.height() - y;

            if (isInsideButton(x, y, BTN_X, BTN_Y1))
            {
                Serial.println("Proceed pressed!");
                // --- Clear Preferences ---
                prefs.begin("wifi", false);
                prefs.clear();
                prefs.end();
                prefs.begin("config", false);
                prefs.clear();
                prefs.end();
                // --- Remove /settings.json from SPIFFS ---
                if (SPIFFS.begin(true)) // true = format if mount fails
                {
                    if (SPIFFS.exists("/settings.json"))
                    {
                        if (SPIFFS.remove("/settings.json"))
                        {
                            Serial.println("Deleted /settings.json successfully!");
                        }
                        else
                        {
                            Serial.println("Failed to delete /settings.json!");
                        }
                    }
                    else
                    {
                        Serial.println("/settings.json not found.");
                    }
                }
                else
                {
                    Serial.println("Failed to mount SPIFFS!");
                }
                // Restart ESP
                ESP.restart();
            }
            else if (isInsideButton(x, y, BTN_X, BTN_Y2))
            {
                Serial.println("Exit pressed!");
                // Restart ESP
                ESP.restart();
            }
            delay(300); // debounce
        }
    }
}





void tryToRetrieveUTCoffsetFromFirstConfiguration()
{
    prefs.begin("iPhonetime", true);

    String storedIso = prefs.getString("iso", "N/A");          // Full ISO timestamp
    unsigned long long setupUnix = prefs.getLong64("unix", 0); // ms since 1970
    int offsetMinutes = prefs.getInt("offsetMinutes", 0);

    prefs.end();

    // Debug output
    Serial.println("📂 Retrieved iPhonetime prefs:");
    Serial.printf("   ISO Timestamp: %s\n", storedIso.c_str());
    Serial.printf("   UTC Offset Minutes: %d (%.1f hours)\n",
                  offsetMinutes, offsetMinutes / 60.0);

    // ✅ Use NTPClient's clock, not system time
    unsigned long long nowUnix = timeClient.getEpochTime(); // seconds since 1970 (UTC)

    // Debug raw numbers
    Serial.printf("   Raw setupUnix (ms): %llu\n", setupUnix);
    Serial.printf("   Now (s): %llu\n", nowUnix);
    Serial.printf("   Setup (s): %llu\n", setupUnix / 1000ULL);

    // Calculate elapsed
    unsigned long long elapsed = nowUnix - (setupUnix / 1000ULL);

    unsigned long days = elapsed / 86400;
    unsigned long hours = (elapsed % 86400) / 3600;
    unsigned long minutes = (elapsed % 3600) / 60;
    unsigned long seconds = elapsed % 60;

    Serial.printf("⏱️ Configured %lu days, %lu hours, %lu minutes, %lu seconds ago\n",
                  days, hours, minutes, seconds);

    // Update Global variable
    tOffset = offsetMinutes / 60.0;
}