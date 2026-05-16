// ============================================================
// gps_logger.h  —  ATGM336H / generic NMEA parser, dep-free.
//
// Parses $GxRMC for time/date/lat/lon/valid and $GxGGA for HDOP +
// satellite count. Talks to the GPS on Serial1 (default 9600 baud).
//
// Drop this file next to main.cpp and #include it once. All state
// is file-scope static — only one GPS instance.
//
// Hardware:
//   GPS TX -> XIAO GPIO 44 (D7, Serial1 RX)
//   GPS RX -> XIAO GPIO 43 (D6, Serial1 TX)   [optional, only for PMTK]
//   GPS VCC -> XIAO 3V3
//   GPS GND -> XIAO GND
//
// Public API:
//   void gpsInit();
//   void gpsPoll();              // call from loop(), often
//   const GpsFix& gpsCurrentFix();
//   bool gpsHasFix();
//   bool gpsHasTime();           // RMC date/time valid (used for filenames)
//   size_t gpsFormatUTC(char* buf, size_t cap);   // ISO-8601, "" if no time
// ============================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#ifndef GPS_RX_PIN
#define GPS_RX_PIN 44   // XIAO D7
#endif
#ifndef GPS_TX_PIN
#define GPS_TX_PIN 43   // XIAO D6  (reuses ex-debug-mirror pin)
#endif
#ifndef GPS_BAUD
#define GPS_BAUD 9600
#endif

struct GpsFix {
    double   lat;        // decimal degrees, +N / -S
    double   lon;        // decimal degrees, +E / -W
    float    hdop;       // horizontal dilution of precision
    uint8_t  sats;       // satellites in use
    uint16_t year;       // 2026, etc
    uint8_t  month;      // 1-12
    uint8_t  day;        // 1-31
    uint8_t  hour;       // 0-23 (UTC)
    uint8_t  minute;     // 0-59
    uint8_t  second;     // 0-59
    bool     valid;      // RMC status == A
    bool     dateValid;  // RMC date+time parsed at least once
    uint32_t lastFixMs;  // millis() of latest valid RMC
};

static GpsFix g_gpsFix = {};
static char   g_nmeaBuf[96];
static size_t g_nmeaLen = 0;

// ---- helpers ----
static inline bool gps__isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
static inline uint8_t gps__hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// Parses "ddmm.mmmm" + 'N'/'S' or "dddmm.mmmm" + 'E'/'W' -> decimal degrees.
// degDigits = 2 for lat, 3 for lon.
static double gps__parseLatLon(const char* s, char hemi, int degDigits) {
    if (!s || !*s) return 0.0;
    char degBuf[4] = {0};
    if (degDigits > (int)sizeof(degBuf) - 1) degDigits = sizeof(degBuf) - 1;
    for (int i = 0; i < degDigits; i++) {
        if (!s[i]) return 0.0;
        degBuf[i] = s[i];
    }
    double deg = atof(degBuf);
    double min = atof(s + degDigits);
    double dec = deg + (min / 60.0);
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

// Splits a NMEA sentence into comma-delimited fields in-place.
// Returns number of fields. Modifies buf (writes nulls at commas).
static int gps__splitFields(char* buf, char** fields, int maxFields) {
    int n = 0;
    char* p = buf;
    fields[n++] = p;
    while (*p && n < maxFields) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        } else if (*p == '*') {
            // checksum starts here, terminate last field
            *p = '\0';
            break;
        }
        p++;
    }
    return n;
}

// Verifies the trailing *XX checksum on a NMEA sentence (with leading '$').
// buf is null-terminated. Returns true if checksum matches.
static bool gps__verifyChecksum(const char* buf, size_t len) {
    if (len < 4 || buf[0] != '$') return false;
    // Find the '*'
    const char* star = nullptr;
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '*') { star = buf + i; break; }
    }
    if (!star) return false;
    if (star + 3 > buf + len) return false;
    if (!gps__isHex(star[1]) || !gps__isHex(star[2])) return false;
    uint8_t expected = (gps__hexVal(star[1]) << 4) | gps__hexVal(star[2]);
    uint8_t calc = 0;
    for (const char* p = buf + 1; p < star; p++) calc ^= (uint8_t)*p;
    return calc == expected;
}

// ---- sentence handlers ----
static void gps__handleRMC(char* body) {
    // Fields (after talker+type): time,status,lat,N/S,lon,E/W,speed,course,date,...
    char* f[14] = {0};
    int n = gps__splitFields(body, f, 14);
    if (n < 10) return;
    const char* time   = f[1];
    char        status = f[2][0];
    const char* lat    = f[3];
    char        ns     = f[4][0];
    const char* lon    = f[5];
    char        ew     = f[6][0];
    const char* date   = f[9];

    // Time HHMMSS(.sss)
    if (time && strlen(time) >= 6) {
        char hh[3] = {time[0], time[1], 0};
        char mm[3] = {time[2], time[3], 0};
        char ss[3] = {time[4], time[5], 0};
        g_gpsFix.hour   = (uint8_t)atoi(hh);
        g_gpsFix.minute = (uint8_t)atoi(mm);
        g_gpsFix.second = (uint8_t)atoi(ss);
    }
    // Date DDMMYY
    if (date && strlen(date) >= 6) {
        char dd[3] = {date[0], date[1], 0};
        char mo[3] = {date[2], date[3], 0};
        char yy[3] = {date[4], date[5], 0};
        g_gpsFix.day   = (uint8_t)atoi(dd);
        g_gpsFix.month = (uint8_t)atoi(mo);
        g_gpsFix.year  = (uint16_t)(2000 + atoi(yy));
        g_gpsFix.dateValid = true;
    }

    if (status == 'A' && lat && *lat && lon && *lon) {
        g_gpsFix.lat = gps__parseLatLon(lat, ns, 2);
        g_gpsFix.lon = gps__parseLatLon(lon, ew, 3);
        g_gpsFix.valid = true;
        g_gpsFix.lastFixMs = millis();
    } else if (status == 'V') {
        g_gpsFix.valid = false;
    }
}

static void gps__handleGGA(char* body) {
    // Fields: time,lat,N/S,lon,E/W,quality,numSats,hdop,alt,M,geoidSep,M,...
    char* f[15] = {0};
    int n = gps__splitFields(body, f, 15);
    if (n < 9) return;
    if (f[7] && *f[7]) g_gpsFix.sats = (uint8_t)atoi(f[7]);
    if (f[8] && *f[8]) g_gpsFix.hdop = (float)atof(f[8]);
}

// Sentence dispatcher — called from the byte ingest loop once a full
// CR/LF-terminated NMEA line is in g_nmeaBuf. Verifies checksum, then
// hands off based on the 3-letter type suffix (RMC, GGA).
static void gps__dispatchSentence() {
    if (g_nmeaLen < 7) return;
    if (!gps__verifyChecksum(g_nmeaBuf, g_nmeaLen)) return;
    // Make a working copy because splitFields mutates the buffer.
    char work[sizeof(g_nmeaBuf)];
    memcpy(work, g_nmeaBuf, g_nmeaLen);
    work[g_nmeaLen] = '\0';
    // Field 0 is the type prefix like "$GNRMC". Compare the 3-char type suffix.
    if (g_nmeaLen < 7) return;
    const char* type = work + 3;
    if (strncmp(type, "RMC", 3) == 0) {
        gps__handleRMC(work);
    } else if (strncmp(type, "GGA", 3) == 0) {
        gps__handleGGA(work);
    }
}

// ---- public API ----
inline void gpsInit() {
    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    g_nmeaLen = 0;
    memset(&g_gpsFix, 0, sizeof(g_gpsFix));
}

inline void gpsPoll() {
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\r') continue;
        if (c == '\n') {
            g_nmeaBuf[g_nmeaLen] = '\0';
            gps__dispatchSentence();
            g_nmeaLen = 0;
            continue;
        }
        if (c == '$') {
            // Resync on every new sentence start, even if mid-line.
            g_nmeaLen = 0;
        }
        if (g_nmeaLen < sizeof(g_nmeaBuf) - 1) {
            g_nmeaBuf[g_nmeaLen++] = c;
        } else {
            // Overflow: drop the whole line.
            g_nmeaLen = 0;
        }
    }
}

inline const GpsFix& gpsCurrentFix() { return g_gpsFix; }
inline bool gpsHasFix()  { return g_gpsFix.valid; }
inline bool gpsHasTime() { return g_gpsFix.dateValid; }

// Returns ISO-8601 UTC string ("2026-05-13T19:42:18Z") or empty if no time.
inline size_t gpsFormatUTC(char* buf, size_t cap) {
    if (!g_gpsFix.dateValid || cap < 21) {
        if (cap > 0) buf[0] = '\0';
        return 0;
    }
    int n = snprintf(buf, cap, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                     (unsigned)g_gpsFix.year, (unsigned)g_gpsFix.month,
                     (unsigned)g_gpsFix.day,  (unsigned)g_gpsFix.hour,
                     (unsigned)g_gpsFix.minute, (unsigned)g_gpsFix.second);
    return (n > 0) ? (size_t)n : 0;
}

// Compact filename-safe stamp: "20260513_194218".
inline size_t gpsFormatFileStamp(char* buf, size_t cap) {
    if (!g_gpsFix.dateValid || cap < 16) {
        if (cap > 0) buf[0] = '\0';
        return 0;
    }
    int n = snprintf(buf, cap, "%04u%02u%02u_%02u%02u%02u",
                     (unsigned)g_gpsFix.year, (unsigned)g_gpsFix.month,
                     (unsigned)g_gpsFix.day,  (unsigned)g_gpsFix.hour,
                     (unsigned)g_gpsFix.minute, (unsigned)g_gpsFix.second);
    return (n > 0) ? (size_t)n : 0;
}
