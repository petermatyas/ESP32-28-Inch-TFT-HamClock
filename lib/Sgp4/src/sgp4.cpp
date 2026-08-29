#include "sgp4.h"
#include <stdlib.h>
#include <string.h>

namespace sgp4 {

// --- WGS-72 constants, as required by the TLE mean elements ------------------
static const double PI      = 3.14159265358979323846;
static const double TWOPI   = 2.0 * PI;
static const double DEG2RAD = PI / 180.0;
static const double RAD2DEG = 180.0 / PI;

static const double MU         = 398600.8;   // km^3/s^2
static const double RE         = 6378.135;   // km
static const double J2         = 0.001082616;
static const double J3         = -0.00000253881;
static const double J4         = -0.00000165597;
static const double J3OJ2      = J3 / J2;
static const double X2O3       = 2.0 / 3.0;
static const double FLATTENING = 1.0 / 298.26;

// xke = 60 / sqrt(RE^3 / MU): derived rather than typed in so the WGS-72 set
// stays self-consistent if RE/MU are ever adjusted.
static double xke()
{
    static const double v = 60.0 / sqrt(RE * RE * RE / MU);
    return v;
}

static double vkmpersec() { return RE * xke() / 60.0; }

static inline double fmod2p(double x)
{
    double r = fmod(x, TWOPI);
    if (r < 0.0) r += TWOPI;
    return r;
}

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------
double jdFromUnix(double unixSeconds) { return unixSeconds / 86400.0 + 2440587.5; }
double unixFromJd(double jd)          { return (jd - 2440587.5) * 86400.0; }

static double jdayJan1(int year)
{
    // Julian date of <year>-01-01 00:00 UT.
    return 367.0 * year - floor((7.0 * year) * 0.25) + 30.0 + 1.0 + 1721013.5;
}

double gstime(double jdut1)
{
    double tut1 = (jdut1 - 2451545.0) / 36525.0;
    double temp = -6.2e-6 * tut1 * tut1 * tut1
                + 0.093104 * tut1 * tut1
                + (876600.0 * 3600.0 + 8640184.812866) * tut1
                + 67310.54841;            // seconds of arc-time
    temp = fmod(temp * DEG2RAD / 240.0, TWOPI);
    if (temp < 0.0) temp += TWOPI;
    return temp;
}

// -----------------------------------------------------------------------------
// TLE parsing
// -----------------------------------------------------------------------------

// Copy [start, start+len) of a TLE line into a scratch buffer and atof() it.
static double field(const char *line, int start, int len)
{
    char buf[24];
    if (len > 23) len = 23;
    memcpy(buf, line + start, len);
    buf[len] = '\0';
    return atof(buf);
}

// TLE exponential fields: "-11606-4" is -0.11606e-4, " 00000+0" is 0.
static double expField(const char *line, int start, int len)
{
    char buf[16];
    if (len > 15) len = 15;
    memcpy(buf, line + start, len);
    buf[len] = '\0';

    char *p = buf;
    while (*p == ' ') p++;

    int sign = 1;
    if (*p == '-')      { sign = -1; p++; }
    else if (*p == '+') { p++; }

    char mant[16];
    int m = 0;
    while (*p && *p != '+' && *p != '-' && m < 14) mant[m++] = *p++;
    mant[m] = '\0';
    if (m == 0) return 0.0;

    int expo = (*p) ? atoi(p) : 0;
    return sign * atof(mant) * 1.0e-5 * pow(10.0, (double)expo);
}

bool parseTle(const char *line1, const char *line2, SatRec &s)
{
    memset(&s, 0, sizeof(s));
    s.valid = false;
    s.deepSpace = false;

    if (!line1 || !line2 || strlen(line1) < 68 || strlen(line2) < 68 ||
        line1[0] != '1' || line2[0] != '2') {
        s.error = ERR_BAD_TLE;
        return false;
    }

    int    epochyr   = (int)field(line1, 18, 2);
    double epochdays = field(line1, 20, 12);
    s.bstar          = expField(line1, 53, 8);

    s.inclo = field(line2,  8, 8) * DEG2RAD;
    s.nodeo = field(line2, 17, 8) * DEG2RAD;
    s.ecco  = field(line2, 26, 7) * 1.0e-7;
    s.argpo = field(line2, 34, 8) * DEG2RAD;
    s.mo    = field(line2, 43, 8) * DEG2RAD;

    double revPerDay = field(line2, 52, 11);
    if (revPerDay <= 0.0 || epochdays <= 0.0) { s.error = ERR_BAD_TLE; return false; }
    if (s.ecco < 0.0 || s.ecco >= 1.0)        { s.error = ERR_ECC_OUT_OF_RANGE; return false; }

    s.periodMin = 1440.0 / revPerDay;

    // Deep-space regime: period >= 225 min.  SDP4 is not implemented here, and
    // running SGP4 on such an object produces garbage rather than an error, so
    // reject it explicitly.
    if (s.periodMin >= 225.0) { s.error = ERR_DEEP_SPACE; s.deepSpace = true; return false; }

    double no_kozai = revPerDay * TWOPI / 1440.0;   // rad/min

    int year = (epochyr < 57) ? epochyr + 2000 : epochyr + 1900;
    s.jdsatepoch = jdayJan1(year) + (epochdays - 1.0);

    // ---- sgp4init, near-earth branch ----------------------------------------
    const double ss     = 78.0 / RE + 1.0;
    const double qzms2t = pow(((120.0 - 78.0) / RE), 4.0);

    double a1     = pow(xke() / no_kozai, X2O3);
    double cosio  = cos(s.inclo);
    double theta2 = cosio * cosio;
    double con42  = 1.0 - 5.0 * theta2;
    double x3thm1 = 3.0 * theta2 - 1.0;
    double eosq   = s.ecco * s.ecco;
    double betao2 = 1.0 - eosq;
    double betao  = sqrt(betao2);

    // Un-kozai the mean motion.  The classic formulation is written in terms of
    // k2 = J2/2, hence the 0.75 rather than 1.5 here.
    double del1 = 0.75 * J2 * x3thm1 / (a1 * a1 * betao * betao2);
    double ao   = a1 * (1.0 - del1 * (0.5 * X2O3 + del1 * (1.0 + 134.0 / 81.0 * del1)));
    double delo = 0.75 * J2 * x3thm1 / (ao * ao * betao * betao2);

    s.no_unkozai = no_kozai / (1.0 + delo);

    ao = pow(xke() / s.no_unkozai, X2O3);
    double sinio = sin(s.inclo);
    double po    = ao * betao2;
    double posq  = po * po;
    double rp    = ao * (1.0 - s.ecco);

    s.con41  = x3thm1;
    s.x1mth2 = 1.0 - theta2;
    s.x7thm1 = 7.0 * theta2 - 1.0;

    if (rp < 1.0) { s.error = ERR_DECAYED; return false; }

    double sfour  = ss;
    double qzms24 = qzms2t;
    double perige = (rp - 1.0) * RE;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        qzms24 = pow((120.0 - sfour) / RE, 4.0);
        sfour  = sfour / RE + 1.0;
    }

    double pinvsq = 1.0 / posq;
    double tsi    = 1.0 / (ao - sfour);
    s.eta         = ao * s.ecco * tsi;
    double etasq  = s.eta * s.eta;
    double eeta   = s.ecco * s.eta;
    double psisq  = fabs(1.0 - etasq);
    double coef   = qzms24 * pow(tsi, 4.0);
    double coef1  = coef / pow(psisq, 3.5);

    double cc2 = coef1 * s.no_unkozai *
                 (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                  0.375 * J2 * tsi / psisq * s.con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    s.cc1 = s.bstar * cc2;

    double cc3 = 0.0;
    if (s.ecco > 1.0e-4)
        cc3 = -2.0 * coef * tsi * J3OJ2 * s.no_unkozai * sinio / s.ecco;

    s.cc4 = 2.0 * s.no_unkozai * coef1 * ao * betao2 *
            (s.eta * (2.0 + 0.5 * etasq) + s.ecco * (0.5 + 2.0 * etasq) -
             J2 * tsi / (ao * psisq) *
             (-3.0 * s.con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
              0.75 * s.x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cos(2.0 * s.argpo)));
    s.cc5 = 2.0 * coef1 * ao * betao2 * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);

    double cosio4 = theta2 * theta2;
    double temp1  = 1.5 * J2 * pinvsq * s.no_unkozai;
    double temp2  = 0.5 * temp1 * J2 * pinvsq;
    double temp3  = -0.46875 * J4 * pinvsq * pinvsq * s.no_unkozai;

    s.mdot    = s.no_unkozai + 0.5 * temp1 * betao * s.con41 +
                0.0625 * temp2 * betao * (13.0 - 78.0 * theta2 + 137.0 * cosio4);
    s.argpdot = -0.5 * temp1 * con42 +
                0.0625 * temp2 * (7.0 - 114.0 * theta2 + 395.0 * cosio4) +
                temp3 * (3.0 - 36.0 * theta2 + 49.0 * cosio4);

    double xhdot1 = -temp1 * cosio;
    s.nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * theta2) +
                          2.0 * temp3 * (3.0 - 7.0 * theta2)) * cosio;

    s.omgcof = s.bstar * cc3 * cos(s.argpo);
    s.xmcof  = 0.0;
    if (s.ecco > 1.0e-4)
        s.xmcof = -X2O3 * coef * s.bstar / eeta;
    s.nodecf = 3.5 * betao2 * xhdot1 * s.cc1;
    s.t2cof  = 1.5 * s.cc1;

    // Guard the divide-by-zero at inclination 180 deg.
    if (fabs(cosio + 1.0) > 1.5e-12)
        s.xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
    else
        s.xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / 1.5e-12;

    s.aycof  = -0.5 * J3OJ2 * sinio;
    s.delmo  = pow(1.0 + s.eta * cos(s.mo), 3.0);
    s.sinmao = sin(s.mo);

    s.isimp = 0;
    if ((ao * (1.0 - s.ecco)) < (220.0 / RE + 1.0)) s.isimp = 1;

    if (s.isimp != 1) {
        double cc1sq = s.cc1 * s.cc1;
        s.d2 = 4.0 * ao * tsi * cc1sq;
        double temp = s.d2 * tsi * s.cc1 / 3.0;
        s.d3 = (17.0 * ao + sfour) * temp;
        s.d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s.cc1;
        s.t3cof = s.d2 + 2.0 * cc1sq;
        s.t4cof = 0.25 * (3.0 * s.d3 + s.cc1 * (12.0 * s.d2 + 10.0 * cc1sq));
        s.t5cof = 0.2 * (3.0 * s.d4 + 12.0 * s.cc1 * s.d3 + 6.0 * s.d2 * s.d2 +
                         15.0 * cc1sq * (2.0 * s.d2 + cc1sq));
    }

    s.error = ERR_NONE;
    s.valid = true;
    return true;
}

// -----------------------------------------------------------------------------
// Propagation
// -----------------------------------------------------------------------------
bool propagate(const SatRec &s, double tsince, double r[3], double v[3], int *err)
{
    if (err) *err = ERR_NONE;
    if (!s.valid) { if (err) *err = s.error ? s.error : ERR_BAD_TLE; return false; }

    // --- secular gravity and atmospheric drag --------------------------------
    double xmdf   = s.mo + s.mdot * tsince;
    double argpdf = s.argpo + s.argpdot * tsince;
    double nodedf = s.nodeo + s.nodedot * tsince;
    double argpm  = argpdf;
    double mm     = xmdf;
    double t2     = tsince * tsince;
    double nodem  = nodedf + s.nodecf * t2;
    double tempa  = 1.0 - s.cc1 * tsince;
    double tempe  = s.bstar * s.cc4 * tsince;
    double templ  = s.t2cof * t2;

    if (s.isimp != 1) {
        double delomg = s.omgcof * tsince;
        double delm   = s.xmcof * (pow(1.0 + s.eta * cos(xmdf), 3.0) - s.delmo);
        double temp   = delomg + delm;
        mm    = xmdf + temp;
        argpm = argpdf - temp;
        double t3 = t2 * tsince;
        double t4 = t3 * tsince;
        tempa = tempa - s.d2 * t2 - s.d3 * t3 - s.d4 * t4;
        tempe = tempe + s.bstar * s.cc5 * (sin(mm) - s.sinmao);
        templ = templ + s.t3cof * t3 + t4 * (s.t4cof + tsince * s.t5cof);
    }

    double nm    = s.no_unkozai;
    double em    = s.ecco;
    double inclm = s.inclo;

    if (nm <= 0.0) { if (err) *err = ERR_MEAN_MOTION; return false; }

    double am = pow((xke() / nm), X2O3) * tempa * tempa;
    nm = xke() / pow(am, 1.5);
    em = em - tempe;

    if (em >= 1.0 || em < -0.001) { if (err) *err = ERR_PERT_ECC; return false; }
    if (em < 1.0e-6) em = 1.0e-6;

    mm = mm + s.no_unkozai * templ;
    double xlm = mm + argpm + nodem;

    nodem = fmod(nodem, TWOPI);
    argpm = fmod(argpm, TWOPI);
    xlm   = fmod(xlm, TWOPI);
    mm    = fmod(xlm - argpm - nodem, TWOPI);

    double sinim = sin(inclm);
    double cosim = cos(inclm);

    if (em < 0.0 || em > 1.0) { if (err) *err = ERR_DECAYED; return false; }

    // --- long period periodics ------------------------------------------------
    double axnl = em * cos(argpm);
    double temp = 1.0 / (am * (1.0 - em * em));
    double aynl = em * sin(argpm) + temp * s.aycof;
    double xl   = mm + argpm + nodem + temp * s.xlcof * axnl;

    // --- Kepler's equation ----------------------------------------------------
    double u      = fmod(xl - nodem, TWOPI);
    double eo1    = u;
    double tem5   = 9999.9;
    double sineo1 = 0.0, coseo1 = 0.0;
    for (int ktr = 1; fabs(tem5) >= 1.0e-12 && ktr <= 10; ++ktr) {
        sineo1 = sin(eo1);
        coseo1 = cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (fabs(tem5) >= 0.95) tem5 = (tem5 > 0.0) ? 0.95 : -0.95;
        eo1 += tem5;
    }

    // --- short period preliminary quantities ---------------------------------
    double ecose = axnl * coseo1 + aynl * sineo1;
    double esine = axnl * sineo1 - aynl * coseo1;
    double el2   = axnl * axnl + aynl * aynl;
    double pl    = am * (1.0 - el2);
    if (pl < 0.0) { if (err) *err = ERR_SEMI_LATUS; return false; }

    double rl     = am * (1.0 - ecose);
    double rdotl  = sqrt(am) * esine / rl;
    double rvdotl = sqrt(pl) / rl;
    double betal  = sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    double sinu  = am / rl * (sineo1 - aynl - axnl * temp);
    double cosu  = am / rl * (coseo1 - axnl + aynl * temp);
    double su    = atan2(sinu, cosu);
    double sin2u = (cosu + cosu) * sinu;
    double cos2u = 1.0 - 2.0 * sinu * sinu;

    temp = 1.0 / pl;
    double t1 = 0.5 * J2 * temp;
    double t2s = t1 * temp;

    double cosisq = cosim * cosim;
    double con41  = 3.0 * cosisq - 1.0;
    double x1mth2 = 1.0 - cosisq;
    double x7thm1 = 7.0 * cosisq - 1.0;

    double mrt   = rl * (1.0 - 1.5 * t2s * betal * con41) + 0.5 * t1 * x1mth2 * cos2u;
    su           = su - 0.25 * t2s * x7thm1 * sin2u;
    double xnode = nodem + 1.5 * t2s * cosim * sin2u;
    double xinc  = inclm + 1.5 * t2s * cosim * sinim * cos2u;
    double mvt   = rdotl - nm * t1 * x1mth2 * sin2u / xke();
    double rvdot = rvdotl + nm * t1 * (x1mth2 * cos2u + 1.5 * con41) / xke();

    // --- orientation vectors --------------------------------------------------
    double sinsu = sin(su),    cossu = cos(su);
    double snod  = sin(xnode), cnod  = cos(xnode);
    double sini  = sin(xinc),  cosi  = cos(xinc);
    double xmx   = -snod * cosi;
    double xmy   =  cnod * cosi;
    double ux = xmx * sinsu + cnod * cossu;
    double uy = xmy * sinsu + snod * cossu;
    double uz = sini * sinsu;
    double vx = xmx * cossu - cnod * sinsu;
    double vy = xmy * cossu - snod * sinsu;
    double vz = sini * cossu;

    r[0] = mrt * ux * RE;
    r[1] = mrt * uy * RE;
    r[2] = mrt * uz * RE;
    v[0] = (mvt * ux + rvdot * vx) * vkmpersec();
    v[1] = (mvt * uy + rvdot * vy) * vkmpersec();
    v[2] = (mvt * uz + rvdot * vz) * vkmpersec();

    if (mrt < 1.0) { if (err) *err = ERR_DECAYED; return false; }
    return true;
}

// -----------------------------------------------------------------------------
// Observer geometry
// -----------------------------------------------------------------------------
void observe(const double r[3], const double v[3], double jd,
             double latRad, double lonRad, double altKm, Look &out)
{
    double theta = fmod2p(gstime(jd) + lonRad);   // local sidereal time

    double sinLat = sin(latRad), cosLat = cos(latRad);
    double sinThe = sin(theta),  cosThe = cos(theta);

    double c  = 1.0 / sqrt(1.0 + FLATTENING * (FLATTENING - 2.0) * sinLat * sinLat);
    double sq = (1.0 - FLATTENING) * (1.0 - FLATTENING) * c;
    double achcp = (RE * c + altKm) * cosLat;

    double ox = achcp * cosThe;
    double oy = achcp * sinThe;
    double oz = (RE * sq + altKm) * sinLat;

    // Observer velocity from earth rotation.
    const double omegaE = 7.29211510e-5;   // rad/s
    double ovx = -omegaE * oy;
    double ovy =  omegaE * ox;
    double ovz =  0.0;

    double rx = r[0] - ox, ry = r[1] - oy, rz = r[2] - oz;
    double dvx = v[0] - ovx, dvy = v[1] - ovy, dvz = v[2] - ovz;

    double topS =  sinLat * cosThe * rx + sinLat * sinThe * ry - cosLat * rz;
    double topE = -sinThe * rx + cosThe * ry;
    double topZ =  cosLat * cosThe * rx + cosLat * sinThe * ry + sinLat * rz;

    double range = sqrt(rx * rx + ry * ry + rz * rz);

    double az = atan2(-topE, topS) + PI;
    if (az >= TWOPI) az -= TWOPI;

    out.azDeg   = az * RAD2DEG;
    out.elDeg   = asin(topZ / range) * RAD2DEG;
    out.rangeKm = range;
    out.rangeRateKmS = (range > 0.0) ? (rx * dvx + ry * dvy + rz * dvz) / range : 0.0;
}

// -----------------------------------------------------------------------------
// Sun position and illumination
// -----------------------------------------------------------------------------
void sunEci(double jd, double rsun[3])
{
    const double AU = 149597870.0;

    double t = (jd - 2451545.0) / 36525.0;
    double meanlong = fmod(280.460 + 36000.77 * t, 360.0);
    double meananom = fmod(357.5277233 + 35999.05034 * t, 360.0) * DEG2RAD;

    double eclplong = (meanlong + 1.914666471 * sin(meananom)
                                + 0.019994643 * sin(2.0 * meananom)) * DEG2RAD;
    double obliquity = (23.439291 - 0.0130042 * t) * DEG2RAD;
    double magr = 1.000140612 - 0.016708617 * cos(meananom)
                              - 0.000139589 * cos(2.0 * meananom);

    rsun[0] = magr * cos(eclplong) * AU;
    rsun[1] = magr * cos(obliquity) * sin(eclplong) * AU;
    rsun[2] = magr * sin(obliquity) * sin(eclplong) * AU;
}

bool satSunlit(const double rsat[3], const double rsun[3])
{
    double sunMag = sqrt(rsun[0] * rsun[0] + rsun[1] * rsun[1] + rsun[2] * rsun[2]);
    double satMag = sqrt(rsat[0] * rsat[0] + rsat[1] * rsat[1] + rsat[2] * rsat[2]);
    if (sunMag <= 0.0 || satMag <= 0.0) return true;

    double ux = rsun[0] / sunMag, uy = rsun[1] / sunMag, uz = rsun[2] / sunMag;
    double dot = rsat[0] * ux + rsat[1] * uy + rsat[2] * uz;

    // Sunward side of the earth: always lit.
    if (dot > 0.0) return true;

    // Behind the earth: lit only outside the cylindrical shadow.
    double perp2 = satMag * satMag - dot * dot;
    if (perp2 < 0.0) perp2 = 0.0;
    return sqrt(perp2) > RE;
}

double sunElevationDeg(double jd, double latRad, double lonRad)
{
    double rsun[3];
    sunEci(jd, rsun);

    double theta = fmod2p(gstime(jd) + lonRad);
    double sinLat = sin(latRad), cosLat = cos(latRad);
    double sinThe = sin(theta),  cosThe = cos(theta);

    // The sun is ~1.5e8 km away, so the observer's offset from the geocentre is
    // negligible here - use the geocentric direction.
    double topZ = cosLat * cosThe * rsun[0] + cosLat * sinThe * rsun[1] + sinLat * rsun[2];
    double mag  = sqrt(rsun[0] * rsun[0] + rsun[1] * rsun[1] + rsun[2] * rsun[2]);
    return asin(topZ / mag) * RAD2DEG;
}

// -----------------------------------------------------------------------------
// The moon
// -----------------------------------------------------------------------------
namespace {

// Ecliptic longitude, latitude (radians) and distance (km) of the moon.
// Meeus, Astronomical Algorithms, chapter 47, truncated to the terms above
// about 0.01 degrees.
void moonEcliptic(double t, double &lonRad, double &latRad, double &distKm)
{
    // Mean arguments, degrees.
    double Lp = 218.3164477 + 481267.88123421 * t;   // mean longitude
    double D  = 297.8501921 + 445267.1114034  * t;   // mean elongation
    double M  = 357.5291092 +  35999.0502909  * t;   // sun's mean anomaly
    double Mp = 134.9633964 + 477198.8675055  * t;   // moon's mean anomaly
    double F  =  93.2720950 + 483202.0175233  * t;   // argument of latitude

    D  *= DEG2RAD;
    M  *= DEG2RAD;
    Mp *= DEG2RAD;
    F  *= DEG2RAD;

    double lon = Lp
        + 6.288774 * sin(Mp)
        + 1.274027 * sin(2.0 * D - Mp)
        + 0.658314 * sin(2.0 * D)
        + 0.213618 * sin(2.0 * Mp)
        - 0.185116 * sin(M)
        - 0.114332 * sin(2.0 * F)
        + 0.058793 * sin(2.0 * D - 2.0 * Mp)
        + 0.057066 * sin(2.0 * D - M - Mp)
        + 0.053322 * sin(2.0 * D + Mp)
        + 0.045758 * sin(2.0 * D - M)
        - 0.040923 * sin(M - Mp)
        - 0.034720 * sin(D)
        - 0.030383 * sin(M + Mp)
        + 0.015327 * sin(2.0 * D - 2.0 * F)
        - 0.012528 * sin(Mp + 2.0 * F)
        + 0.010980 * sin(Mp - 2.0 * F);

    double lat = 5.128122 * sin(F)
        + 0.280602 * sin(Mp + F)
        + 0.277693 * sin(Mp - F)
        + 0.173237 * sin(2.0 * D - F)
        + 0.055413 * sin(2.0 * D - Mp + F)
        + 0.046271 * sin(2.0 * D - Mp - F)
        + 0.032573 * sin(2.0 * D + F)
        + 0.017198 * sin(2.0 * Mp + F)
        + 0.009266 * sin(2.0 * D + Mp - F)
        + 0.008822 * sin(2.0 * Mp - F)
        + 0.008216 * sin(2.0 * D - M - F)
        + 0.004324 * sin(2.0 * D - 2.0 * Mp - F)
        + 0.004200 * sin(2.0 * D + Mp + F);

    distKm = 385000.56
        - 20905.355 * cos(Mp)
        -  3699.111 * cos(2.0 * D - Mp)
        -  2955.968 * cos(2.0 * D)
        -   569.925 * cos(2.0 * Mp)
        +    48.888 * cos(M)
        -     3.149 * cos(2.0 * F)
        +   246.158 * cos(2.0 * D - 2.0 * Mp)
        -   152.138 * cos(2.0 * D - M - Mp)
        -   170.733 * cos(2.0 * D + Mp)
        -   204.586 * cos(2.0 * D - M)
        -   129.620 * cos(M - Mp)
        +   108.743 * cos(D)
        +   104.755 * cos(M + Mp);

    lonRad = fmod2p(lon * DEG2RAD);
    latRad = lat * DEG2RAD;
}

// Apparent ecliptic longitude of the sun, from the same series sunEci() uses.
double sunEclipticLon(double t)
{
    double meanlong = fmod(280.460 + 36000.77 * t, 360.0);
    double meananom = fmod(357.5277233 + 35999.05034 * t, 360.0) * DEG2RAD;
    return fmod2p((meanlong + 1.914666471 * sin(meananom)
                            + 0.019994643 * sin(2.0 * meananom)) * DEG2RAD);
}

} // namespace

void moonEci(double jd, double rmoon[3])
{
    double t = (jd - 2451545.0) / 36525.0;

    double lon, lat, dist;
    moonEcliptic(t, lon, lat, dist);

    double obliquity = (23.439291 - 0.0130042 * t) * DEG2RAD;
    double cb = cos(lat), sb = sin(lat);
    double cl = cos(lon), sl = sin(lon);

    rmoon[0] = dist * cb * cl;
    rmoon[1] = dist * (cb * sl * cos(obliquity) - sb * sin(obliquity));
    rmoon[2] = dist * (cb * sl * sin(obliquity) + sb * cos(obliquity));
}

void topocentric(const double r[3], double jd, double latRad, double lonRad,
                 double altKm, double &azDeg, double &elDeg, double &rangeKm)
{
    double theta = fmod2p(gstime(jd) + lonRad);

    double sinLat = sin(latRad), cosLat = cos(latRad);
    double sinThe = sin(theta),  cosThe = cos(theta);

    double c  = 1.0 / sqrt(1.0 + FLATTENING * (FLATTENING - 2.0) * sinLat * sinLat);
    double sq = (1.0 - FLATTENING) * (1.0 - FLATTENING) * c;
    double achcp = (RE * c + altKm) * cosLat;

    double rx = r[0] - achcp * cosThe;
    double ry = r[1] - achcp * sinThe;
    double rz = r[2] - (RE * sq + altKm) * sinLat;

    double topS =  sinLat * cosThe * rx + sinLat * sinThe * ry - cosLat * rz;
    double topE = -sinThe * rx + cosThe * ry;
    double topZ =  cosLat * cosThe * rx + cosLat * sinThe * ry + sinLat * rz;

    double range = sqrt(rx * rx + ry * ry + rz * rz);

    double az = atan2(-topE, topS) + PI;
    if (az >= TWOPI) az -= TWOPI;

    azDeg   = az * RAD2DEG;
    elDeg   = asin(topZ / range) * RAD2DEG;
    rangeKm = range;
}

void moonInfo(double jd, double latRad, double lonRad, double altKm, MoonInfo &out)
{
    double t = (jd - 2451545.0) / 36525.0;

    double rmoon[3], rsun[3];
    moonEci(jd, rmoon);
    sunEci(jd, rsun);

    topocentric(rmoon, jd, latRad, lonRad, altKm, out.azDeg, out.elDeg, out.distanceKm);

    // The lit fraction follows from the phase angle at the moon, the angle
    // sun-moon-earth.  Working that out from the two distances and the
    // elongation, rather than from the elongation alone, is what keeps it
    // right near new and full.
    double dm = sqrt(rmoon[0] * rmoon[0] + rmoon[1] * rmoon[1] + rmoon[2] * rmoon[2]);
    double ds = sqrt(rsun[0] * rsun[0] + rsun[1] * rsun[1] + rsun[2] * rsun[2]);
    double dot = rmoon[0] * rsun[0] + rmoon[1] * rsun[1] + rmoon[2] * rsun[2];

    double cosPsi = dot / (dm * ds);
    if (cosPsi >  1.0) cosPsi =  1.0;
    if (cosPsi < -1.0) cosPsi = -1.0;
    double psi = acos(cosPsi);                                  // elongation

    double phase = atan2(ds * sin(psi), dm - ds * cos(psi));    // phase angle
    out.phaseAngleDeg   = phase * RAD2DEG;
    out.illuminatedFrac = 0.5 * (1.0 + cos(phase));

    // Waxing while the moon leads the sun in ecliptic longitude.  The same
    // difference gives the age, spread evenly over one synodic month; that
    // ignores the orbit's eccentricity, so it can be half a day out near the
    // quarters - which is the precision an age in days is read at anyway.
    double mlon, mlat, mdist;
    moonEcliptic(t, mlon, mlat, mdist);
    double elong = fmod2p(mlon - sunEclipticLon(t));

    out.waxing  = elong < PI;
    out.ageDays = elong / TWOPI * 29.530588853;
}

// -----------------------------------------------------------------------------
// Rise and set
// -----------------------------------------------------------------------------
namespace {

double bodyElevation(int body, double unixT, double latRad, double lonRad, double altKm)
{
    double jd = jdFromUnix(unixT);
    double r[3];
    if (body == SKY_MOON) moonEci(jd, r);
    else                  sunEci(jd, r);

    double az, el, rng;
    topocentric(r, jd, latRad, lonRad, altKm, az, el, rng);
    return el;
}

} // namespace

void riseSet(int body, double dayStartUnix, double latRad, double lonRad,
             double altKm, RiseSet &out)
{
    // Standard altitudes: the geometric horizon lowered by refraction, and by
    // the body's own apparent radius.  These are the values almanacs use, which
    // is why the results agree with published tables to about a minute.  The
    // moon's parallax needs no term of its own because the position above is
    // already topocentric.
    const double h = (body == SKY_MOON) ? -0.583 : -0.833;

    // Ten minutes.  The moon, the faster of the two in elevation, moves well
    // under half a degree in that time near the horizon, so no crossing hides
    // between samples outside the polar case where a rise and set nearly meet.
    const double STEP  = 600.0;
    const int    STEPS = 144;

    out.riseValid = out.setValid = false;
    out.riseUnix = out.setUnix = 0.0;

    double prev = bodyElevation(body, dayStartUnix, latRad, lonRad, altKm);
    out.aboveAtStart = prev >= h;

    for (int i = 1; i <= STEPS; i++)
    {
        double t = dayStartUnix + i * STEP;
        double cur = bodyElevation(body, t, latRad, lonRad, altKm);

        bool rising = cur >= h;
        if (rising != (prev >= h))
        {
            // Bisect to about a second.
            double lo = t - STEP, hi = t;
            for (int k = 0; k < 20; k++)
            {
                double mid = 0.5 * (lo + hi);
                if ((bodyElevation(body, mid, latRad, lonRad, altKm) >= h) == rising)
                    hi = mid;
                else
                    lo = mid;
            }
            double when = 0.5 * (lo + hi);

            // The first of each kind is the one an almanac lists.
            if (rising && !out.riseValid) { out.riseValid = true; out.riseUnix = when; }
            if (!rising && !out.setValid) { out.setValid  = true; out.setUnix  = when; }
        }
        prev = cur;
    }
}

// -----------------------------------------------------------------------------
// Pass prediction
// -----------------------------------------------------------------------------
namespace {

struct Sample {
    double elDeg;
    double azDeg;
};

bool sampleAt(const SatRec &s, double unixTime, double latRad, double lonRad,
              double altKm, Sample &out)
{
    double jd = jdFromUnix(unixTime);
    double tsince = (jd - s.jdsatepoch) * 1440.0;

    double r[3], v[3];
    int err = 0;
    if (!propagate(s, tsince, r, v, &err)) return false;

    Look look;
    observe(r, v, jd, latRad, lonRad, altKm, look);

    out.elDeg = look.elDeg;
    out.azDeg = look.azDeg;
    return true;
}

// Bisect a horizon crossing bracketed by tLow/tHigh.
// risingEdge: tLow is below the threshold and tHigh above it (falling is the
// other way round).
double bisectCrossing(const SatRec &s, double tLow, double tHigh, double minElDeg,
                      double latRad, double lonRad, double altKm, bool risingEdge)
{
    Sample smp;
    for (int i = 0; i < 24 && (tHigh - tLow) > 0.5; ++i) {
        double mid = 0.5 * (tLow + tHigh);
        if (!sampleAt(s, mid, latRad, lonRad, altKm, smp)) break;
        bool above = smp.elDeg >= minElDeg;
        if (above == risingEdge) tHigh = mid; else tLow = mid;
    }
    return 0.5 * (tLow + tHigh);
}

} // anonymous namespace

bool findNextPass(const SatRec &s, double startUnix, double horizonSeconds,
                  double latRad, double lonRad, double altKm, double minElDeg,
                  Pass &out, YieldFn yieldFn)
{
    int yieldCounter = 0;
    out.valid = false;
    if (!s.valid) return false;

    const double endUnix = startUnix + horizonSeconds;

    Sample smp;
    if (!sampleAt(s, startUnix, latRad, lonRad, altKm, smp)) return false;

    double prevT  = startUnix;
    double prevEl = smp.elDeg;
    bool   inPass = smp.elDeg >= minElDeg;
    double aos    = startUnix;
    double aosAz  = smp.azDeg;

    if (inPass) {
        // A pass is already running: walk backwards to recover its real AOS so
        // the display can show when it started.
        double back = startUnix;
        for (int i = 0; i < 120; ++i) {           // at most 60 min back
            double cand = back - 30.0;
            Sample b;
            if (!sampleAt(s, cand, latRad, lonRad, altKm, b)) break;
            if (b.elDeg < minElDeg) {
                aos = bisectCrossing(s, cand, back, minElDeg, latRad, lonRad, altKm, true);
                Sample a;
                if (sampleAt(s, aos, latRad, lonRad, altKm, a)) aosAz = a.azDeg;
                break;
            }
            back  = cand;
            aos   = cand;
            aosAz = b.azDeg;
        }
    }

    double maxEl = inPass ? smp.elDeg : -90.0;
    double maxT  = startUnix;
    double maxAz = smp.azDeg;

    double t = startUnix;
    while (t < endUnix) {
        // Far below the horizon nothing can happen quickly, so stride out.  Near
        // the horizon step finely enough not to skip a short, low-elevation pass.
        double step = (!inPass && prevEl < -25.0) ? 180.0 : 30.0;
        t = prevT + step;
        if (t > endUnix) t = endUnix;

        if (yieldFn && ++yieldCounter >= 256) { yieldCounter = 0; yieldFn(); }

        if (!sampleAt(s, t, latRad, lonRad, altKm, smp)) return false;
        bool above = smp.elDeg >= minElDeg;

        if (!inPass && above) {
            inPass = true;
            aos = bisectCrossing(s, prevT, t, minElDeg, latRad, lonRad, altKm, true);
            Sample a;
            if (sampleAt(s, aos, latRad, lonRad, altKm, a)) aosAz = a.azDeg;
            maxEl = smp.elDeg;
            maxT  = t;
            maxAz = smp.azDeg;
        } else if (inPass) {
            if (smp.elDeg > maxEl) { maxEl = smp.elDeg; maxT = t; maxAz = smp.azDeg; }

            if (!above) {
                double los = bisectCrossing(s, prevT, t, minElDeg, latRad, lonRad, altKm, false);

                if (los <= startUnix) {           // pass already over, keep looking
                    inPass = false;
                    maxEl  = -90.0;
                    prevT  = t;
                    prevEl = smp.elDeg;
                    continue;
                }

                // Refine the culmination with a golden-section search.
                double lo = (maxT - 60.0 < aos) ? aos : maxT - 60.0;
                double hi = (maxT + 60.0 > los) ? los : maxT + 60.0;
                const double gr = 0.6180339887498949;
                double c = hi - gr * (hi - lo);
                double d = lo + gr * (hi - lo);
                Sample sc, sd;
                if (sampleAt(s, c, latRad, lonRad, altKm, sc) &&
                    sampleAt(s, d, latRad, lonRad, altKm, sd)) {
                    for (int i = 0; i < 20 && (hi - lo) > 1.0; ++i) {
                        if (sc.elDeg > sd.elDeg) {
                            hi = d; d = c; sd = sc;
                            c = hi - gr * (hi - lo);
                            if (!sampleAt(s, c, latRad, lonRad, altKm, sc)) break;
                        } else {
                            lo = c; c = d; sc = sd;
                            d = lo + gr * (hi - lo);
                            if (!sampleAt(s, d, latRad, lonRad, altKm, sd)) break;
                        }
                    }
                    maxT = 0.5 * (lo + hi);
                    Sample m;
                    if (sampleAt(s, maxT, latRad, lonRad, altKm, m)) {
                        maxEl = m.elDeg;
                        maxAz = m.azDeg;
                    }
                }

                Sample l;
                out.valid    = true;
                out.aosUnix  = aos;
                out.losUnix  = los;
                out.maxUnix  = maxT;
                out.maxElDeg = maxEl;
                out.aosAzDeg = aosAz;
                out.losAzDeg = sampleAt(s, los, latRad, lonRad, altKm, l) ? l.azDeg : smp.azDeg;
                out.maxAzDeg = maxAz;
                return true;
            }
        }

        prevT  = t;
        prevEl = smp.elDeg;
        if (t >= endUnix) break;
    }

    return false;
}

} // namespace sgp4
