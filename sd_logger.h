// ============================================================
// sd_logger.h  —  CSV wardriving log on the Adafruit ADA254 SD breakout.
//
// One row per detection. GPS fields are filled when a fix is valid,
// blank otherwise. The file is opened/closed per write so a power
// yank loses at most the last row.
//
// Hardware (Adafruit ADA254 -> XIAO ESP32-S3):
//   CLK  -> GPIO 7  (D8, default HSPI SCK)
//   DO   -> GPIO 8  (D9, default HSPI MISO)
//   DI   -> GPIO 9  (D10, default HSPI MOSI)
//   CS   -> GPIO 2  (D1)
//   VCC  -> 3V3   (the breakout has its own regulator + level shifter)
//   GND  -> GND
//
// Public API:
//   bool sdInit();
//   bool sdReady();
//   void sdLogDetection(const char* mac, const char* method,
//                       int8_t rssi, uint8_t channel, const char* ssid);
//   void sdMaybeRotateFilename();   // call after first GPS time fix
// ============================================================
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "gps_logger.h"

#ifndef SD_CS_PIN
#define SD_CS_PIN 2     // XIAO D1
#endif
#ifndef SD_CSV_HEADER
#define SD_CSV_HEADER "timestamp_utc,uptime_ms,mac,oui,method,rssi,channel,frequency,ssid,lat,lon,hdop,sats\n"
#endif

static bool   g_sdReady = false;
static char   g_sdFilename[40] = "";
static bool   g_sdFilenameIsBoot = true;  // true = boot-counter name, false = GPS-stamped

// sd_logger uses Serial directly rather than the project's dualPrintf, because
// this header is included at the top of main.cpp (before dualPrintf is defined)
// and dualPrintf has static linkage. With MIRROR_SERIAL=0 the two are equivalent.

static inline uint16_t sd__channelFreq(uint8_t ch) {
    return (ch >= 1 && ch <= 14) ? (uint16_t)(2407 + 5 * ch) : 0;
}

// Open a fresh file with a header row. Caller has chosen g_sdFilename.
static bool sd__createFile() {
    if (SD.exists(g_sdFilename)) {
        // Append mode: don't overwrite an existing log.
        return true;
    }
    File f = SD.open(g_sdFilename, FILE_WRITE);
    if (!f) {
        Serial.printf("[sd] open %s failed\n", g_sdFilename);
        return false;
    }
    f.print(SD_CSV_HEADER);
    f.close();
    Serial.printf("[sd] created %s\n", g_sdFilename);
    return true;
}

inline bool sdInit() {
    // SD library uses the default HSPI for the ESP32-S3 (pins 7/8/9).
    // If your wiring differs, call SPI.begin(SCK, MISO, MOSI, CS) before
    // this and pass that SPIClass to SD.begin().
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("[sd] mount failed (no card, or wiring issue)");
        g_sdReady = false;
        return false;
    }
    uint8_t type = SD.cardType();
    if (type == CARD_NONE) {
        Serial.println("[sd] no card detected");
        g_sdReady = false;
        return false;
    }
    uint64_t sizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[sd] mounted: type=%u size=%lluMB\n",
               (unsigned)type, (unsigned long long)sizeMB);

    // Pick a temporary filename until GPS gives us real time. Scans for
    // the next free slot since millis() resets every boot and would collide.
    for (int boot = 0; boot < 1000; boot++) {
        snprintf(g_sdFilename, sizeof(g_sdFilename),
                 "/fy_boot_%03d.csv", boot);
        if (!SD.exists(g_sdFilename)) break;
    }
    g_sdFilenameIsBoot = true;
    g_sdReady = sd__createFile();
    return g_sdReady;
}

inline bool sdReady() { return g_sdReady; }

// Once GPS gives us a real timestamp, rename the boot-named file to the
// timestamped one. Safe to call repeatedly — only acts once.
inline void sdMaybeRotateFilename() {
    if (!g_sdReady) return;
    if (!g_sdFilenameIsBoot) return;
    if (!gpsHasTime()) return;

    char stamp[24];
    if (gpsFormatFileStamp(stamp, sizeof(stamp)) == 0) return;

    char newName[40];
    snprintf(newName, sizeof(newName), "/flockyou_%s.csv", stamp);

    if (SD.exists(newName)) {
        // Already exists (rare: re-boot at same UTC second). Just keep
        // writing to the boot-named file.
        g_sdFilenameIsBoot = false;  // stop trying
        return;
    }
    if (SD.rename(g_sdFilename, newName)) {
        Serial.printf("[sd] renamed %s -> %s\n", g_sdFilename, newName);
        strncpy(g_sdFilename, newName, sizeof(g_sdFilename) - 1);
        g_sdFilename[sizeof(g_sdFilename) - 1] = '\0';
        g_sdFilenameIsBoot = false;
    } else {
        // Rename failed (some SD libs don't support it). Fall back: just
        // close out the old and open a new file. Old data is preserved.
        File n = SD.open(newName, FILE_WRITE);
        if (n) {
            n.print(SD_CSV_HEADER);
            n.close();
            strncpy(g_sdFilename, newName, sizeof(g_sdFilename) - 1);
            g_sdFilename[sizeof(g_sdFilename) - 1] = '\0';
            g_sdFilenameIsBoot = false;
            Serial.printf("[sd] started fresh %s (rename unsupported)\n", newName);
        }
    }
}

// Quote a string for CSV: wrap in quotes if it contains comma/quote/newline,
// and double up internal quotes. dst is bounded.
static size_t sd__csvQuote(char* dst, size_t cap, const char* src) {
    if (!src) { if (cap > 0) dst[0] = '\0'; return 0; }
    bool needsQuoting = false;
    for (const char* p = src; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needsQuoting = true; break;
        }
    }
    if (!needsQuoting) {
        size_t n = strlen(src);
        if (n >= cap) n = cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        return n;
    }
    size_t o = 0;
    if (o < cap - 1) dst[o++] = '"';
    for (const char* p = src; *p && o < cap - 2; p++) {
        if (*p == '"') {
            if (o < cap - 2) dst[o++] = '"';
            if (o < cap - 2) dst[o++] = '"';
        } else {
            dst[o++] = *p;
        }
    }
    if (o < cap - 1) dst[o++] = '"';
    dst[o] = '\0';
    return o;
}

inline void sdLogDetection(const char* mac, const char* method,
                           int8_t rssi, uint8_t channel, const char* ssid) {
    if (!g_sdReady) return;

    char ts[24] = "";
    gpsFormatUTC(ts, sizeof(ts));

    char ssidQ[80] = "";
    sd__csvQuote(ssidQ, sizeof(ssidQ), ssid ? ssid : "");

    // OUI from the MAC string (assumes "xx:xx:xx:xx:xx:xx" format).
    char oui[9] = "";
    if (mac && strlen(mac) >= 8) {
        memcpy(oui, mac, 8);
        oui[8] = '\0';
    }

    const GpsFix& fix = gpsCurrentFix();
    char latBuf[16] = "";
    char lonBuf[16] = "";
    char hdopBuf[8] = "";
    char satsBuf[4] = "";
    if (fix.valid) {
        snprintf(latBuf, sizeof(latBuf), "%.6f", fix.lat);
        snprintf(lonBuf, sizeof(lonBuf), "%.6f", fix.lon);
        if (fix.hdop > 0) snprintf(hdopBuf, sizeof(hdopBuf), "%.1f", fix.hdop);
        if (fix.sats > 0) snprintf(satsBuf, sizeof(satsBuf), "%u", (unsigned)fix.sats);
    }

    File f = SD.open(g_sdFilename, FILE_APPEND);
    if (!f) {
        // One-shot retry — sometimes a card hiccup needs a re-mount.
        SD.end();
        if (SD.begin(SD_CS_PIN)) {
            f = SD.open(g_sdFilename, FILE_APPEND);
        }
        if (!f) {
            Serial.printf("[sd] append %s failed\n", g_sdFilename);
            return;
        }
    }
    f.printf("%s,%lu,%s,%s,%s,%d,%u,%u,%s,%s,%s,%s,%s\n",
             ts, (unsigned long)millis(),
             mac ? mac : "", oui, method ? method : "",
             (int)rssi, (unsigned)channel, (unsigned)sd__channelFreq(channel),
             ssidQ, latBuf, lonBuf, hdopBuf, satsBuf);
    f.close();
}
