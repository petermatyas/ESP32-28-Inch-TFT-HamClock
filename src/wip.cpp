
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
const uint8_t MAX_PAGES = 11;   // page 1 merged the old dual-clock and big-clock pages
unsigned long lastTouchMs = 0;
bool wasTouching = false;
int scanCount = 0;
bool inAPmode = false;
bool autoPageChange = false;
char bigClockLastDigit[4] = {' ', ' ', ' ', ' '};
int8_t bigClockColonState = -1;   // big clock colon: -1 unknown, 0 hidden, 1 shown
bool bigClockShowsUtc = false;    // big clock time base: false = QTH, true = UTC

// How the big clock draws the time.  Chosen on the web page; the QTH/UTC badge
// underneath works the same way whichever is picked.
enum BigClockStyle : uint8_t
{
    BIGCLOCK_SEVENSEG = 0,   // the seven segment digits this page started with
    BIGCLOCK_ANALOG   = 1,   // dial, numerals and hands
    BIGCLOCK_BINARY   = 2,   // one column of bits per digit, hours to seconds
    BIGCLOCK_DUAL     = 3,   // QTH and UTC shown together - the old separate page 1
    BIGCLOCK_STYLE_COUNT
};
uint8_t bigClockStyle = BIGCLOCK_SEVENSEG;
bool bigClockFullRedraw = true;   // the style changed, or the page just opened
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
int brightness = 100;   // backlight, percent - see applyBrightness()
static const int BACKLIGHT_LEDC_CHANNEL = 0;

// Page 1 (dual clock) frame geometry.  No page header here - the page is
// nothing but the two clocks, so the full screen height goes to them instead.
static const int FRAME_TOP1 = 0, FRAME_H = 87;                   // 0..87
static const int FRAME_TOP2 = FRAME_TOP1 + FRAME_H + 18;         // 105..192
const int FRAME1_DIGIT_Y = FRAME_TOP1 + 5;
const int FRAME2_DIGIT_Y = FRAME_TOP2 + 2;

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
void applyBrightness();
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
void drawBigClockPage();
static void drawPageHeader(const char *title);
static void drawHeaderCornerClock(bool utc);
static void drawPageHeaderWithClock(const char *title, bool utc);
static void activatePage(uint8_t page);
static void handleScreenshot();
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

    // Backlight PWM: channel set up now, duty applied once the saved
    // brightness is known (after loadSettings(), below).
    ledcSetup(BACKLIGHT_LEDC_CHANNEL, 5000, 8);
    ledcAttachPin(TFT_BLP, BACKLIGHT_LEDC_CHANNEL);

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
    applyBrightness();
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
            server.serveStatic("/clock.html", SPIFFS, "/clock.html");
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
doc ["bigClockStyle"] = bigClockStyle;
doc ["bigClockColour"] = bigClockColour;
doc ["brightness"] = brightness;

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
    if (activePage==1 && bigClockStyle == BIGCLOCK_DUAL){
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

    // Redraw labels immediately on screen, but only if the dual-clock face is
    // actually the one showing - otherwise this would draw its frame over
    // whichever of the other three faces is on screen.
    if (activePage == 1 && bigClockStyle == BIGCLOCK_DUAL)
    {
        refreshFrames = true;
        drawOrredrawStaticElements();
    }

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
    if (activePage == 1 && bigClockStyle == BIGCLOCK_DUAL) drawOrredrawStaticElements();
    weather.city[0] = 0;      // re-resolve the place name for the new position
    fetchWeatherData();
    server.send(200, "text/plain", "OK"); });

            // Jump the display to a page, so the web side can drive it - and so
            // the screenshots below can be taken of every page in turn.
            server.on("/setpage", HTTP_GET, []()
                      {
    int p = server.hasArg("p") ? server.arg("p").toInt() : 0;
    if (p < 1 || p > MAX_PAGES) {
        server.send(400, "text/plain", "page out of range");
        return;
    }
    activatePage((uint8_t)p);
    Serial.printf("Page set to %d from the web\n", p);
    server.send(200, "text/plain", "OK"); });

            server.on("/screenshot", HTTP_GET, []() { handleScreenshot(); });

            server.on("/setbigclockstyle", HTTP_POST, []()
                      {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }
    int style = doc["bigClockStyle"] | (int)bigClockStyle;
    if (style < 0 || style >= BIGCLOCK_STYLE_COUNT) {
        server.send(400, "text/plain", "unknown style");
        return;
    }
    bigClockStyle = (uint8_t)style;
    saveSettings();
    Serial.printf("Big clock style -> %u\n", bigClockStyle);

    // Only repaint when that page is the one on screen; the redraw clears the
    // whole display and would wipe whatever else is showing.
    if (activePage == 1) {
        tft.fillScreen(TFT_BLACK);
        if (bigClockStyle == BIGCLOCK_DUAL) {
            drawOrredrawStaticElements();
        } else {
            bigClockFullRedraw = true;
            LASTbigClockTimeStr = "";
            for (int i = 0; i < 4; i++) bigClockLastDigit[i] = ' ';
            bigClockColonState = -1;
            bigClockLabelDirty = true;
        }
    }
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

    if (activePage == 1 && bigClockStyle == BIGCLOCK_DUAL) drawOrredrawStaticElements();

    server.send(200, "text/plain", "OK"); });

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

            server.on("/setbrightness", HTTP_POST, []()
                      {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "JSON parse error");
        return;
    }
    brightness = constrain((int)(doc["brightness"] | brightness), 10, 100);
    applyBrightness();
    saveSettings();
    Serial.printf("\U0001F4A1 Brightness set to %d%%\n", brightness);
    server.send(200, "text/plain", "OK"); });

            server.on("/setAutoPage", HTTP_GET, []()
                      {
                          if (server.hasArg("enabled"))
                          {
                              autoPageChange = (server.arg("enabled") == "true");
                          }
                          server.send(200, "text/plain", autoPageChange ? "AutoPage ON" : "AutoPage OFF");
                          saveSettings();

                          activatePage(1);
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
            Serial.println("📄 Active page -> 1");
            activatePage(1);
            lastActivity = currentMillis; // 🔄 Reset inactivity timer
        }
        return;
    }
    // 📺 Normal Mode
    else
        switch (activePage)
        {
        case 1:
            // Page 1 is now all four clock faces - the dual QTH+UTC clock is
            // just one of the four, picked the same way as the other three.
            if (bigClockStyle == BIGCLOCK_DUAL)
            {
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
                    displayTime(8, FRAME1_DIGIT_Y, localTime, previousLocalTime, 0, localTimeColour);
                    displayTime(10, FRAME2_DIGIT_Y, utcTime, previousUTCtime, 0, utcTimeColour);
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
            }
            else
            {
                currentMillis = millis();
                drawBigClockPage();
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
            satellitesDrawPage(tft, (time_t)timeClient.getEpochTime(), tOffset,
                               redrawSatellitePage);
            redrawSatellitePage = false;
            break;
        }

        case 8:
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

        case 9:
        {
            // Cheap: the renderer decides for itself what actually changed.
            drawBeaconPage(redrawBeaconPage);
            redrawBeaconPage = false;
            break;
        }

        case 10:
        {
            drawSunMoonPage(redrawSunMoonPage);
            redrawSunMoonPage = false;
            break;
        }

        case 11:
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
                activatePage(1);
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
    bigClockStyle = doc["bigClockStyle"] | bigClockStyle;
    if (bigClockStyle >= BIGCLOCK_STYLE_COUNT) bigClockStyle = BIGCLOCK_SEVENSEG;
    weatherIntervalMin = doc["weatherIntervalMin"] | weatherIntervalMin;
    if (weatherIntervalMin < WEATHER_INTERVAL_MIN_LIMIT) weatherIntervalMin = WEATHER_INTERVAL_MIN_LIMIT;
    if (weatherIntervalMin > WEATHER_INTERVAL_MAX_LIMIT) weatherIntervalMin = WEATHER_INTERVAL_MAX_LIMIT;
    brightness = doc["brightness"] | brightness;
    brightness = constrain(brightness, 10, 100);

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
    Serial.printf("💡 Brightness          : %d%%\n", brightness);
    Serial.println("-----------------------------------------------------------------");
}

// Duty scaled for the backlight's own polarity: TFT_BACKLIGHT_ON is defined
// HIGH for the cyd board (higher duty = brighter) and left undefined for the
// generic board, whose backlight is wired active-low (higher duty = dimmer).
void applyBrightness()
{
#if defined(TFT_BACKLIGHT_ON) && (TFT_BACKLIGHT_ON == HIGH)
    uint32_t duty = (uint32_t)(brightness * 255L / 100);
#else
    uint32_t duty = (uint32_t)(255 - brightness * 255L / 100);
#endif
    ledcWrite(BACKLIGHT_LEDC_CHANNEL, duty);
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
    doc["bigClockStyle"] = bigClockStyle;
    doc["weatherIntervalMin"] = weatherIntervalMin;
    doc["screenSaverTimeout"] = screenSaverTimeout;
    doc["brightness"] = brightness;

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
    tft.fillRect(25, FRAME_TOP1 + FRAME_H - 10, 270, 20, TFT_BLACK);
    tft.fillRect(25, FRAME_TOP2 + FRAME_H - 10, 270, 20, TFT_BLACK);

    // 🟩 Local Frame
    tft.fillRect(0, FRAME_TOP1, 320, FRAME_H, TFT_BLACK); // Clear previous frame

    tft.drawRoundRect(1, FRAME_TOP1 + 1, 319, FRAME_H - 2, 5, localFrameColour);
    if (doubleFrame)
    {
        tft.drawRoundRect(1, FRAME_TOP1 + 1, 319, FRAME_H - 2, 4, localFrameColour);
        tft.drawRoundRect(2, FRAME_TOP1 + 2, 317, FRAME_H - 4, 4, localFrameColour);
        tft.drawRoundRect(3, FRAME_TOP1 + 3, 315, FRAME_H - 6, 4, localFrameColour);
    }

    // 🟦 Local Time Label

    // The caption sits on the frame's centre line; the locator now anchors to
    // the left edge instead of the right, because at this font's width (8
    // monospace chars, ~104 px) a right-aligned locator collided with the
    // default "  QTH Time  " caption and silently failed to draw at all.
    const int LABEL_CX = 180;
    const int LOCATOR_LEFT = 8;
    const int LABEL_Y1 = FRAME_TOP1 + FRAME_H - 11;   // same 11 px clearance to the frame's bottom edge as before
    const int LOCATOR_Y1 = LABEL_Y1;                  // shares the caption's baseline - see below

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(localTimeLabel, LABEL_CX, LABEL_Y1, 1);

    // The Maidenhead locator belongs to the QTH, so it goes on the caption row
    // of the QTH frame, in the caption's own colour.  The row is clear of the
    // big digits, which stop above it.
    {
        char loc[10];
        maidenhead(latitude, longitude, loc, sizeof(loc));

        // Measured while the caption font is still the current one.
        int labelLeft = LABEL_CX - tft.textWidth(localTimeLabel) / 2;

        // The grid locator is real operating information, not just an
        // annotation next to the caption, so it gets a bolder, brighter face -
        // this font's ascent (15 px) matches the caption's, so the two share a
        // baseline with no extra offset needed.
        tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
        int locRight = LOCATOR_LEFT + tft.textWidth(loc);

        // The caption is renamable from the web page, so check the two still
        // clear each other rather than letting them collide.
        if (locRight < labelLeft - 8)
        {
            tft.setTextColor(TFT_CYAN, TFT_BLACK);
            tft.drawString(loc, LOCATOR_LEFT, LOCATOR_Y1);
        }

        tft.setFreeFont(&Orbitron_Medium8pt7b);   // hand the font back
    }

    // 🟥 UTC Frame
    tft.fillRect(0, FRAME_TOP2, 320, FRAME_H, TFT_BLACK); // Clear previous frame

    tft.drawRoundRect(1, FRAME_TOP2, 319, FRAME_H - 2, 5, utcFrameColour);
    if (doubleFrame)
    {
        tft.drawRoundRect(1, FRAME_TOP2 + 1, 319, FRAME_H - 2, 4, utcFrameColour);
        tft.drawRoundRect(2, FRAME_TOP2 + 2, 317, FRAME_H - 4, 4, utcFrameColour);
        tft.drawRoundRect(3, FRAME_TOP2 + 3, 315, FRAME_H - 6, 4, utcFrameColour);
    }

    // ⬜ UTC Label
    tft.drawCentreString(utcTimeLabel, LABEL_CX, FRAME_TOP2 + FRAME_H - 11, 1);
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
        // Page 1 is all four clock faces now; only the dual QTH+UTC face uses
        // drawOrredrawStaticElements() - the other three share the big-clock
        // reset sequence the old page 7 used on entry.
        if (bigClockStyle == BIGCLOCK_DUAL)
        {
            drawOrredrawStaticElements();
        }
        else
        {
            LASTbigClockTimeStr = "";
            for (int i = 0; i < 4; i++)
            {
                bigClockLastDigit[i] = ' ';
            }
            // Unknown state, so the first pass through the loop paints the colon
            // immediately after the screen clear.
            bigClockColonState = -1;
            bigClockLabelDirty = true;
            bigClockFullRedraw = true;
        }
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
        redrawSatellitePage = true;
        break;
    case 8:
        redrawWeatherPage = true;
        break;
    case 9:
        redrawBeaconPage = true;
        break;
    case 10:
        redrawSunMoonPage = true;
        break;
    case 11:
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

            // On the three single-time clock faces the badge is a button:
            // tapping it switches the time base rather than paging.  Checked
            // first so the paging halves cannot swallow the tap.  The dual
            // face has no badge - it already shows both times, and that
            // screen region is its weather banner instead.
            if (activePage == 1 && bigClockStyle != BIGCLOCK_DUAL &&
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
                bigClockFullRedraw = true;
                return;
            }

            // The DX page has a button of its own, and while its filter panel
            // is up it owns every tap - otherwise a miss would page out from
            // under the panel.
            if (activePage == 11 && dxClusterHandleTouch((int16_t)x, (int16_t)y))
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
    drawPageHeader("BAND CONDITIONS");

    // draw frames
    //  Define positions and dimensions
    int dayX = 10;
    int nightX = 170;
    int blockWidth = 140;
    int cornerRadius = 8;

    // DAY / NIGHT labels sit above their box now instead of notched into its
    // border - that notch trick clashed with the header rule at y=19 and isn't
    // how the newer pages label anything.
    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString("DAY", 80, 21, 1);
    tft.drawCentreString("NIGHT", 240, 21, 1);

    int blockY = 39;
    int blockHeight = 114;
    tft.drawRoundRect(dayX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
    tft.drawRoundRect(nightX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);

    // Band conditions by time
    tft.setFreeFont(&JetBrainsMono_Bold15pt7b);

    int yStart = 45;
    int rowSpacing = 27;
    for (int i = 0; i < 4; i++)
    {
        // DAY

        String band = solarData.bandConditions[i].name;
        String cond = solarData.bandConditions[i].condition;
        uint16_t color = cond == "Good" ? TFT_GREEN : cond == "Fair" ? TFT_YELLOW
                                                                     : TFT_RED;
        tft.setTextColor(color);
        tft.drawCentreString(band, 80, yStart + i * rowSpacing, 1);

        // NIGHT
        band = solarData.bandConditions[i + 4].name;
        cond = solarData.bandConditions[i + 4].condition;
        color = cond == "Good" ? TFT_GREEN : cond == "Fair" ? TFT_YELLOW
                                                            : TFT_RED;
        tft.setTextColor(color);

        tft.drawCentreString(band, 240, yStart + i * rowSpacing, 1);
    }

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString("Updated: " + solarData.updated, 160, 160, 1);

    int LocalX = 10;
    int UTCX = 170;

    // Same "label above a plain box" treatment as DAY/NIGHT.  The clock digits
    // (drawLOCALTime()/drawUTCTime() in loop(), fixed at y=205) still land
    // comfortably inside this box - only its top edge and height changed.
    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString("Local", 80, 176, 1);
    tft.drawCentreString("UTC", 240, 176, 1);

    blockY = 194;
    blockHeight = 44;
    tft.drawRoundRect(LocalX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
    tft.drawRoundRect(UTCX, blockY, blockWidth, blockHeight, cornerRadius, TFT_DARKGREY);
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
    // 13 rows is the tightest fit of the three solar pages, hence the slightly
    // shorter lineSpacing than pages 2 and 3 - it's what keeps the last row
    // clear of the bottom edge once the header claims the top of the screen.
    //
    // tft.print()/setCursor() treats y as the text BASELINE, not its top (only
    // drawString() does that top-to-baseline conversion) - so y has to clear
    // the rule by a whole ascent (~10 px for this font), not just a few px, or
    // the first row's glyphs poke up into the header.
    int y = 32;
    int lineSpacing = 16;
    tft.fillScreen(TFT_BLACK);
    drawPageHeaderWithClock("SOLAR INDICES", true);

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
    // See drawSolarSummaryPage1(): y is a print()/setCursor() baseline, so it
    // has to clear the rule by a full ascent, not just a few pixels.
    int y = 32;
    int lineSpacing = 18;
    tft.fillScreen(TFT_BLACK);
    drawPageHeaderWithClock("GEOMAG / MUF", true);
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
    // See drawSolarSummaryPage1(): y is a print()/setCursor() baseline, so it
    // has to clear the rule by a full ascent, not just a few pixels.
    int y = 32;
    int lineSpacing = 18;
    int paragraphSpacing = 6;

    tft.fillScreen(TFT_BLACK);
    // "VHF/UHF CONDITIONS" ran right up against the corner clock at this
    // font's width - shortened so the two don't touch.
    drawPageHeaderWithClock("VHF/UHF", true);
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

// Shared with drawWiFiQualityPage()/drawNtpStatus(): the list starts right
// under the page header, one row per field, all at the same 18 px pitch.
//
// This is a print()/setCursor() list, where y is the text BASELINE rather than
// its top (only drawString() does the top-to-baseline conversion) - so Y0 has
// to clear the header rule by a whole ascent (~10 px), not just a few pixels,
// or the first row's glyphs poke up into the title.
static const int WIFI_Y0 = 32;
static const int WIFI_LINE_H = 18;

void updateWiFiSignalDisplay()
{
    // Pin the font rather than inheriting whatever was selected last.
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
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
    int rssiY = WIFI_Y0 + 3 * WIFI_LINE_H;
    int signalX = 130;
    int signalY = WIFI_Y0 + 4 * WIFI_LINE_H;

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

    drawHeaderCornerClock(true);
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

// =============================================================================
// Big clock: three of page 1's four faces (the fourth, BIGCLOCK_DUAL, is the
// QTH+UTC display drawn separately via drawOrredrawStaticElements())
//
// All three share the QTH/UTC badge at the foot of the screen and the touch
// target that goes with it, so only the area above y=200 differs between them.
// =============================================================================

// --- style 0: seven segment --------------------------------------------------
static void drawBigClockSevenSeg(long shownEpoch, bool full)
{
    // One second on, one second off.  Driven by the parity of the clock's own
    // seconds rather than a millis() timer, so the blink stays in step with the
    // time on screen instead of drifting against it.
    int8_t wantColon = (timeClient.getEpochTime() % 2 == 0) ? 1 : 0;
    if (full || wantColon != bigClockColonState)
    {
        bigClockColonState = wantColon;
        tft.setFreeFont(&digits60pt7b);
        tft.setTextColor(wantColon ? bigClockColour : TFT_BLACK);
        tft.drawString(":", 151, 65, 1);
    }

    struct tm *ptm = gmtime((time_t *)&shownEpoch);
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ptm->tm_hour, ptm->tm_min);

    const int digitX[4] = {5, 78, 180, 253};
    const int digitY = 65;

    if (full || String(hhmm) != LASTbigClockTimeStr)
    {
        // Set the font at the point of use: the badge draws with its own.
        tft.setFreeFont(&digits60pt7b);

        char now[4] = {hhmm[0], hhmm[1], hhmm[3], hhmm[4]};
        for (int i = 0; i < 4; i++)
        {
            if (!full && now[i] == bigClockLastDigit[i]) continue;

            tft.setTextColor(TFT_BLACK);
            tft.drawString(String(bigClockLastDigit[i]), digitX[i], digitY, 1);
            tft.setTextColor(bigClockColour);
            tft.drawString(String(now[i]), digitX[i], digitY, 1);
            bigClockLastDigit[i] = now[i];
        }
        LASTbigClockTimeStr = hhmm;
    }
}

// --- style 1: hands ----------------------------------------------------------
// The dial furniture is painted once.  After that only the hands move, and they
// are kept strictly inside the ring of numerals, so erasing one can never take
// a bite out of the face.
// The numerals sit outside the tick ring rather than inside it.  Inside, the
// two-digit ones reach about 15 px diagonally from their centre, which leaves
// no room between the hub and the ticks for a hand that does not touch them -
// and a hand that touches one takes a bite out of it when it is erased.
static const int DIAL_CX = 160, DIAL_CY = 100;
static const int DIAL_R  = 74;          // the ring itself
static const int TICK_OUT = 73;
static const int TICK_IN_HOUR = 63;
static const int TICK_IN_MIN  = 68;
// 90 is as far out as the numerals can go: any further and the 12 runs off the
// top of the screen.  The ring is drawn inside them rather than the other way
// round, which is what keeps the two clear of each other.
static const int DIAL_NUMERAL_R = 90;
static const int HAND_HOUR = 40, HAND_MIN = 56, HAND_SEC = 58;

static int lastHandH = -1, lastHandM = -1, lastHandS = -1;

// A hand is a triangle from a base across the hub to a point at the tip, which
// makes it taper like a real one and lets it be erased exactly.
static void drawHand(float deg, int len, int halfWidth, uint16_t colour)
{
    float a = deg * DEG_TO_RAD;
    float sn = sinf(a), cs = cosf(a);

    int tx = DIAL_CX + (int)lroundf(len * sn);
    int ty = DIAL_CY - (int)lroundf(len * cs);
    int x1 = DIAL_CX + (int)lroundf(halfWidth * cs);
    int y1 = DIAL_CY + (int)lroundf(halfWidth * sn);
    int x2 = DIAL_CX - (int)lroundf(halfWidth * cs);
    int y2 = DIAL_CY - (int)lroundf(halfWidth * sn);

    tft.fillTriangle(tx, ty, x1, y1, x2, y2, colour);
}

static void drawSecondHand(float deg, uint16_t colour)
{
    float a = deg * DEG_TO_RAD;
    float sn = sinf(a), cs = cosf(a);
    tft.drawLine(DIAL_CX - (int)lroundf(14 * sn), DIAL_CY + (int)lroundf(14 * cs),
                 DIAL_CX + (int)lroundf(HAND_SEC * sn), DIAL_CY - (int)lroundf(HAND_SEC * cs),
                 colour);
}

static void drawDialFace()
{
    tft.drawCircle(DIAL_CX, DIAL_CY, DIAL_R, TFT_DARKGREY);

    for (int i = 0; i < 60; i++)
    {
        float a = i * 6.0f * DEG_TO_RAD;
        float sn = sinf(a), cs = cosf(a);
        bool onHour = (i % 5 == 0);
        int inner = onHour ? TICK_IN_HOUR : TICK_IN_MIN;
        uint16_t colour = onHour ? TFT_LIGHTGREY : TFT_DARKGREY;

        tft.drawLine(DIAL_CX + (int)lroundf(inner * sn), DIAL_CY - (int)lroundf(inner * cs),
                     DIAL_CX + (int)lroundf(TICK_OUT * sn),
                     DIAL_CY - (int)lroundf(TICK_OUT * cs), colour);
    }

    tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    for (int h = 1; h <= 12; h++)
    {
        float a = h * 30.0f * DEG_TO_RAD;
        tft.drawString(String(h),
                       DIAL_CX + (int)lroundf(DIAL_NUMERAL_R * sinf(a)),
                       DIAL_CY - (int)lroundf(DIAL_NUMERAL_R * cosf(a)));
    }
    tft.setTextDatum(TL_DATUM);
}

static void drawBigClockAnalog(long shownEpoch, bool full)
{
    struct tm *ptm = gmtime((time_t *)&shownEpoch);
    int hh = ptm->tm_hour % 12, mm = ptm->tm_min, ss = ptm->tm_sec;

    if (full)
    {
        drawDialFace();
        lastHandH = lastHandM = lastHandS = -1;
    }
    else if (ss == lastHandS && mm == lastHandM && hh == lastHandH)
    {
        return;
    }

    // Rub out only what has actually moved.  The hour and minute hands are then
    // redrawn unconditionally, which also repairs the gash the second hand
    // leaves behind where it crossed them.
    if (lastHandS >= 0)
        drawSecondHand(lastHandS * 6.0f, TFT_BLACK);
    if (lastHandM >= 0 && (mm != lastHandM || hh != lastHandH))
    {
        drawHand(lastHandM * 6.0f, HAND_MIN, 4, TFT_BLACK);
        drawHand(lastHandH * 30.0f + lastHandM * 0.5f, HAND_HOUR, 5, TFT_BLACK);
    }

    drawHand(hh * 30.0f + mm * 0.5f, HAND_HOUR, 5, bigClockColour);
    drawHand(mm * 6.0f, HAND_MIN, 4, bigClockColour);
    drawSecondHand(ss * 6.0f, TFT_RED);
    tft.fillCircle(DIAL_CX, DIAL_CY, 4, bigClockColour);

    lastHandH = hh;
    lastHandM = mm;
    lastHandS = ss;
}

// --- style 2: binary ---------------------------------------------------------
// One column per decimal digit, hours through seconds, least significant bit at
// the bottom - the BCD layout every binary clock uses.  The decimal value is
// printed under each column, because a binary clock nobody can read is an
// ornament rather than a clock.
static const uint8_t BIN_BITS[6] = {2, 4, 3, 4, 3, 4};
static const int BIN_X0 = 52, BIN_DX = 44;
static const int BIN_Y0 = 166, BIN_DY = 38;
static const int BIN_R  = 13;

static int8_t lastBinDigit[6] = {-1, -1, -1, -1, -1, -1};

static void drawBigClockBinary(long shownEpoch, bool full)
{
    struct tm *ptm = gmtime((time_t *)&shownEpoch);
    uint8_t digits[6] = {
        (uint8_t)(ptm->tm_hour / 10), (uint8_t)(ptm->tm_hour % 10),
        (uint8_t)(ptm->tm_min / 10),  (uint8_t)(ptm->tm_min % 10),
        (uint8_t)(ptm->tm_sec / 10),  (uint8_t)(ptm->tm_sec % 10)};

    if (full)
    {
        for (int i = 0; i < 6; i++) lastBinDigit[i] = -1;

        // Bit weights down the left edge, so the columns can be read off.
        tft.setFreeFont(&JetBrainsMono_Light7pt7b);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setTextDatum(MR_DATUM);
        for (int b = 0; b < 4; b++)
            tft.drawString(String(1 << b), 26, BIN_Y0 - b * BIN_DY);

        // Which pair of columns is which.
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        const char *groups[3] = {"HOUR", "MIN", "SEC"};
        for (int g = 0; g < 3; g++)
            tft.drawString(groups[g], BIN_X0 + (int)((g * 2 + 0.5f) * BIN_DX), 30);
        tft.setTextDatum(TL_DATUM);
    }

    for (int i = 0; i < 6; i++)
    {
        if (!full && digits[i] == lastBinDigit[i]) continue;

        int cx = BIN_X0 + i * BIN_DX;
        for (int b = 0; b < BIN_BITS[i]; b++)
        {
            int cy = BIN_Y0 - b * BIN_DY;
            bool on = (digits[i] >> b) & 1;
            if (on)
            {
                tft.fillCircle(cx, cy, BIN_R, bigClockColour);
            }
            else
            {
                tft.fillCircle(cx, cy, BIN_R, TFT_BLACK);
                tft.drawCircle(cx, cy, BIN_R, TFT_DARKGREY);
            }
        }

        // The decimal reading underneath.
        tft.setFreeFont(&JetBrainsMono_Bold11pt7b);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(String(digits[i]), cx, 194);
        tft.setTextDatum(TL_DATUM);

        lastBinDigit[i] = digits[i];
    }
}

// --- dispatch ----------------------------------------------------------------
void drawBigClockPage()
{
    bool full = bigClockFullRedraw;
    bigClockFullRedraw = false;

    long shownEpoch = timeClient.getEpochTime() +
                      (bigClockShowsUtc ? 0L : (long)tOffset * 3600L);

    switch (bigClockStyle)
    {
    case BIGCLOCK_ANALOG: drawBigClockAnalog(shownEpoch, full); break;
    case BIGCLOCK_BINARY: drawBigClockBinary(shownEpoch, full); break;
    default:              drawBigClockSevenSeg(shownEpoch, full); break;
    }

    // Drawn last so nothing the style above paints near the foot of the
    // screen - the binary style's decimal readout at y=194 sits right against
    // it - can ever come back and overwrite a freshly-changed label.
    if (bigClockLabelDirty)
    {
        drawBigClockModeBadge();
        bigClockLabelDirty = false;
    }
}

// =============================================================================
// Screen capture
//
// The ILI9341 can be read back over the same SPI bus it is written on, so a
// screenshot is the panel's own memory rather than a redrawn approximation of
// it.  The image is streamed a row at a time: a whole 320x240 frame is 150 kB,
// which is more than this board has to spare.
// =============================================================================
static void handleScreenshot()
{
    const int W = 320, H = 240;
    const uint32_t rowBytes = (uint32_t)W * 3;
    const uint32_t pixelBytes = rowBytes * H;
    const uint32_t fileSize = 54 + pixelBytes;

    uint8_t header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    memcpy(header + 2, &fileSize, 4);
    uint32_t dataOffset = 54;
    memcpy(header + 10, &dataOffset, 4);
    uint32_t dibSize = 40;
    memcpy(header + 14, &dibSize, 4);
    int32_t bw = W, bh = H;
    memcpy(header + 18, &bw, 4);
    memcpy(header + 22, &bh, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bpp, 2);
    memcpy(header + 34, &pixelBytes, 4);

    server.setContentLength(fileSize);
    server.send(200, "image/bmp", "");
    server.sendContent((const char *)header, sizeof(header));

    // Static rather than automatic: 1.6 kB is a lot to ask of the loop stack.
    static uint16_t line[W];
    static uint8_t  row[W * 3];

    // BMP stores its rows bottom up.
    for (int y = H - 1; y >= 0; y--)
    {
        tft.readRect(0, y, W, 1, line);
        for (int x = 0; x < W; x++)
        {
            // readRect hands back the pixel with its bytes the other way round
            // from the way drawString wrote it, which leaves white and black
            // looking right and everything else on the wrong channel.
            uint16_t raw = line[x];
            uint16_t c = (uint16_t)((raw >> 8) | (raw << 8));
            row[x * 3 + 0] = (uint8_t)((c & 0x001F) << 3);   // blue
            row[x * 3 + 1] = (uint8_t)((c & 0x07E0) >> 3);   // green
            row[x * 3 + 2] = (uint8_t)((c & 0xF800) >> 8);   // red
        }
        server.sendContent((const char *)row, sizeof(row));
    }
    server.sendContent("", 0);
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

// =============================================================================
// Shared page chrome
//
// The satellite/weather/beacon/sun-moon/DX pages all hand-roll the same title +
// rule (and, where the page's own content doesn't already show the time, the
// same top-right corner clock).  These two helpers give the older pages (1-6)
// that same look without duplicating the literal coordinates six more times.
// =============================================================================
static void drawPageHeader(const char *title)
{
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&Orbitron_Medium8pt7b);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(title, 6, 2);
    tft.drawFastHLine(4, 19, 312, TFT_DARKGREY);
}

// The corner clock alone, so a page with its own 1 Hz tick (e.g. the WiFi page)
// can refresh just this instead of redrawing the whole header every second.
static void drawHeaderCornerClock(bool utc)
{
    long shownEpoch = timeClient.getEpochTime() + (utc ? 0L : (long)tOffset * 3600L);
    struct tm *ptm = gmtime((time_t *)&shownEpoch);

    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s",
              ptm->tm_hour, ptm->tm_min, ptm->tm_sec, utc ? "UTC" : "LOC");

    tft.setFreeFont(&JetBrainsMono_Light7pt7b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(buf, 314, 4);
    tft.setTextDatum(TL_DATUM);
}

static void drawPageHeaderWithClock(const char *title, bool utc)
{
    drawPageHeader(title);
    drawHeaderCornerClock(utc);
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
    const int textY = WIFI_Y0 + 10 * WIFI_LINE_H;   // the row after "Hostname 2"

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
    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
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
    drawPageHeaderWithClock("WIFI STATUS", true);

    tft.setFreeFont(&UbuntuMono_Regular8pt7b);
    tft.setTextSize(1);

    int y = WIFI_Y0;
    const int lineSpacing = WIFI_LINE_H;

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