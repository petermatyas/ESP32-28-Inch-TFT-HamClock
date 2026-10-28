// -----------------------------------------------------------------------------
// news.h - RSS / Atom headline page for the HamClock
//
// Up to NEWS_MAX_FEEDS feeds, set at /news.html, are fetched in turn from the
// main loop and the newest headlines of each are shown on their own page.  The
// page can be switched off there as well, in which case nothing is fetched and
// paging skips it.
//
// Feeds are read as a stream and only the item titles are kept, so a large
// feed costs no more memory than a small one.  The display fonts are 7-bit
// ASCII, so accented letters are shown without their accents.
// -----------------------------------------------------------------------------
#ifndef HAMCLOCK_NEWS_H
#define HAMCLOCK_NEWS_H

#include <Arduino.h>
#include <WebServer.h>
#include <TFT_eSPI.h>

#define NEWS_MAX_FEEDS      4
#define NEWS_ITEMS_PER_FEED 6

// Load the config from SPIFFS.  Call once from setup(), after SPIFFS is mounted.
void newsBegin();

// Register the web routes (/news.html, /newscfg, /newsnow).
void newsRegisterRoutes(WebServer &server);

// Whether the page is switched on; when it is not, paging skips it.
bool newsEnabled();

// True when a feed is due for a refresh.  The caller makes room for TLS and
// then calls newsFetchNext(), which fetches that one feed (blocking, a few
// seconds at most).
bool newsFetchDue(unsigned long nowMs);
void newsFetchNext();

// Draw the headline page.  fullRedraw forces the static furniture to be
// repainted; otherwise only the corner clock and, every few seconds, the next
// screenful of headlines are drawn.
void newsDrawPage(TFT_eSPI &tft, time_t utcNow, int tOffsetHours, bool fullRedraw);

#endif // HAMCLOCK_NEWS_H
