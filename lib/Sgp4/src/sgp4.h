// -----------------------------------------------------------------------------
// sgp4.h - near-earth SGP4 propagator + observer geometry + pass prediction
//
// Implements the standard SGP4 near-earth model (Spacetrack Report #3 as
// revised by Vallado et al., WGS-72 constants).  Deep-space objects (orbital
// period >= 225 min, i.e. mean motion < 6.4 rev/day) are NOT propagated: the
// SDP4 lunar/solar resonance terms are not implemented.  parseTle() flags such
// objects via SatRec::deepSpace so callers can report them instead of showing
// nonsense.
//
// Deliberately free of Arduino headers so the maths can be compiled and
// verified on a host with plain g++.
// -----------------------------------------------------------------------------
#ifndef HAMCLOCK_SGP4_H
#define HAMCLOCK_SGP4_H

#include <math.h>
#include <stdint.h>

namespace sgp4 {

// --- error codes returned in SatRec::error / propagate() ---------------------
enum {
    ERR_NONE = 0,
    ERR_ECC_OUT_OF_RANGE = 1, // mean eccentricity outside [0,1)
    ERR_MEAN_MOTION = 2,      // mean motion <= 0
    ERR_PERT_ECC = 3,         // perturbed eccentricity outside [0,1)
    ERR_SEMI_LATUS = 4,       // semi-latus rectum < 0
    ERR_DECAYED = 6,          // satellite has decayed (below the surface)
    ERR_BAD_TLE = 7,          // could not parse the element set
    ERR_DEEP_SPACE = 8        // period >= 225 min, unsupported by this model
};

struct SatRec {
    bool   valid;
    int    error;
    bool   deepSpace;

    double jdsatepoch;   // Julian date of the element set epoch
    double periodMin;    // nodal period in minutes (from the TLE mean motion)

    // mean elements at epoch (radians / radians per minute)
    double bstar, inclo, nodeo, ecco, argpo, mo, no_unkozai;

    // constants derived once by parseTle()
    double cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao,
           t2cof, t3cof, t4cof, t5cof, mdot, nodedot, xlcof, aycof, xmcof,
           nodecf, con41, x1mth2, x7thm1;
    int    isimp;
};

// --- element set handling ----------------------------------------------------

// Parse the two TLE data lines and pre-compute the SGP4 constants.
// Returns false (and sets s.error) on a malformed or deep-space element set.
bool parseTle(const char *line1, const char *line2, SatRec &s);

// Propagate to tsinceMin minutes past the element set epoch.
// r[] is TEME position in km, v[] TEME velocity in km/s.  Returns false on
// error (see the ERR_* codes), in which case *err receives the code.
bool propagate(const SatRec &s, double tsinceMin, double r[3], double v[3], int *err);

// --- time --------------------------------------------------------------------
double jdFromUnix(double unixSeconds);
double unixFromJd(double jd);
double gstime(double jdut1); // Greenwich mean sidereal time, radians

// --- observer geometry -------------------------------------------------------
struct Look {
    double azDeg;       // 0..360, north = 0, clockwise
    double elDeg;       // -90..90
    double rangeKm;
    double rangeRateKmS; // positive = receding
};

// latRad/lonRad: observer position, east longitude positive. altKm above the
// ellipsoid.  jd must be the same instant the r/v vectors were produced for.
void observe(const double r[3], const double v[3], double jd,
             double latRad, double lonRad, double altKm, Look &out);

// --- illumination ------------------------------------------------------------
void   sunEci(double jd, double rsun[3]);                 // km, same frame as r[]
bool   satSunlit(const double rsat[3], const double rsun[3]);
double sunElevationDeg(double jd, double latRad, double lonRad);

// --- pass prediction ---------------------------------------------------------
struct Pass {
    bool   valid;
    double aosUnix;
    double losUnix;
    double maxUnix;
    double maxElDeg;
    double aosAzDeg;
    double losAzDeg;
    double maxAzDeg;
};

// Optional cooperative-yield hook, invoked every few hundred propagations so a
// long search can run inside an RTOS task without starving anything else.
typedef void (*YieldFn)(void);

// Find the first pass reaching at least minElDeg that ends after startUnix.
// Searches at most horizonSeconds ahead.  If the satellite is already above
// minElDeg at startUnix, the pass in progress is returned (its aosUnix is then
// searched backwards, so it may lie before startUnix).
// Returns false when no pass is found inside the horizon.
bool findNextPass(const SatRec &s, double startUnix, double horizonSeconds,
                  double latRad, double lonRad, double altKm, double minElDeg,
                  Pass &out, YieldFn yieldFn = 0);

} // namespace sgp4

#endif // HAMCLOCK_SGP4_H
