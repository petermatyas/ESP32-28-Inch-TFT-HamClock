// -----------------------------------------------------------------------------
// dxcluster.h - DX cluster spot page for the HamClock
//
// Holds a telnet session to a public DX cluster node, parses the "DX de ..."
// lines it sends and shows the most recent spots.  The socket lives in its own
// FreeRTOS task, so a slow or dead node never stalls the clock.
//
// A cluster login needs the operator's own callsign, which is why there is no
// default for it: nothing connects until one is entered at /dx.html.
//
// Spots are kept unfiltered and filtered only as they are drawn, so changing
// the band or mode filter shows history that was already collected instead of
// starting again from nothing.
// -----------------------------------------------------------------------------
#ifndef HAMCLOCK_DXCLUSTER_H
#define HAMCLOCK_DXCLUSTER_H

#include <Arduino.h>
#include <WebServer.h>
#include <TFT_eSPI.h>

#define DX_MAX_SPOTS   32     // ring buffer depth
#define DX_BAND_COUNT  12
#define DX_MODE_COUNT   4

// Load the config from SPIFFS and start the cluster task.  Call once from
// setup(), after SPIFFS is mounted.
void dxClusterBegin();

// Register the web routes (/dx.html, /dxcfg, /dxspots).
void dxClusterRegisterRoutes(WebServer &server);

// Draw the spot list, or the filter panel when it is open.  fullRedraw forces
// the static furniture to be repainted.
void dxClusterDrawPage(TFT_eSPI &tft, time_t utcNow, bool fullRedraw);

// Offer a touch to the page.  Returns true when the page used it - the filter
// button, or anything at all while the filter panel is open - in which case the
// caller must not treat the touch as a page turn.
bool dxClusterHandleTouch(int16_t x, int16_t y);

// True while the filter panel is covering the spot list.
bool dxClusterFilterOpen();

// Prints the cluster task's remaining stack, for the health line.
void dxClusterReportHealth();

#endif // HAMCLOCK_DXCLUSTER_H
