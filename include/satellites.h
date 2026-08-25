// -----------------------------------------------------------------------------
// satellites.h - satellite pass page for the HamClock
//
// Keeps a small list of NORAD ids (entered through /sat.html), downloads their
// TLEs from Celestrak, caches them in SPIFFS and predicts upcoming passes for
// the configured site.  Pass prediction runs in a background FreeRTOS task so
// the clock never stalls; the live look angles are cheap and are recomputed on
// the main loop once a second.
// -----------------------------------------------------------------------------
#ifndef HAMCLOCK_SATELLITES_H
#define HAMCLOCK_SATELLITES_H

#include <Arduino.h>
#include <WebServer.h>
#include <TFT_eSPI.h>

#define SAT_MAX_SATS        8
#define SAT_PASSES_PER_SAT  3

// Load the config and the cached TLEs, then start the prediction task.
// Call once from setup(), after SPIFFS is mounted.
void satellitesBegin(double latitude, double longitude);

// Register the web routes (/sat.html, /satellites, /satpasses, /satrefresh).
void satellitesRegisterRoutes(WebServer &server);

// Observer position changed in the main settings - re-predict.
void satellitesSetSite(double latitude, double longitude);

// Cheap housekeeping: publishes the current time to the prediction task and
// downloads at most one due TLE per call.  Safe to call every loop iteration;
// it rate-limits itself.  utcNow is a unix timestamp.
void satellitesLoop(time_t utcNow);

// Draw the satellite page.  fullRedraw forces the static furniture to be
// repainted (page just became active, or the pass list changed).
// tOffsetHours is the local time offset already used by the rest of the clock.
void satellitesDrawPage(TFT_eSPI &tft, time_t utcNow, int tOffsetHours, bool fullRedraw);

// Run the SGP4 propagator against Vallado's published verification vector and
// print the result to Serial.  Costs a couple of milliseconds at boot.
void satellitesSelfTest();

#endif // HAMCLOCK_SATELLITES_H
