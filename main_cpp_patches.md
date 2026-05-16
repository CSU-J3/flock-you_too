# main.cpp patches

Apply these edits in order. Each one is small and self-contained. Line numbers are approximate — search for the surrounding context shown in the "find" block.

The three new headers (`gps_logger.h`, `sd_logger.h`, `silent_mode.h`) go in the same directory as `main.cpp`.

---

## 1. Add includes (top of file)

**Find:**
```cpp
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <SPIFFS.h>
```

**Replace with:**
```cpp
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <SPIFFS.h>

// --- Standalone additions ---
#include "gps_logger.h"
#include "sd_logger.h"
#include "silent_mode.h"
```

---

## 2. Disable the Serial1 TX mirror

Serial1 (GPIO 43/44) is now the GPS UART. The dual-print mirror conflicts with it. Either disable the mirror or move it to Serial2 with different pins.

**Find:**
```cpp
#define MIRROR_SERIAL 1
#define MIRROR_TX_PIN 43
#define MIRROR_BAUD 115200
```

**Replace with:**
```cpp
#define MIRROR_SERIAL 0   // GPS now owns Serial1 (GPIO 43/44)
#define MIRROR_TX_PIN 43
#define MIRROR_BAUD 115200
```

(`dualPrintf` still works, it just doesn't mirror to a second UART anymore.)

---

## 3. Mute guards on all audio functions

**Find:**
```cpp
static void buzzerBeep(unsigned int ms) {
#if USE_BUZZER
    digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
#endif
}
```

**Replace with:**
```cpp
static void buzzerBeep(unsigned int ms) {
#if USE_BUZZER
    if (audioMuted()) return;
    digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
#endif
}
```

**Find:**
```cpp
static void newDetectChirp() {
#if USE_BUZZER
    tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
```

**Insert a guard right inside the `#if USE_BUZZER`:**
```cpp
static void newDetectChirp() {
#if USE_BUZZER
    if (audioMuted()) return;
    tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
```

**Do the same for `heartbeatBeep()` and `startupBeep()`** — first line inside `#if USE_BUZZER` becomes `if (audioMuted()) return;`.

---

## 4. Add GPS fields to the JSON emission

**Find:**
```cpp
static void emitDetectionJSON(const char* mac, const char* method,
                              int8_t rssi, uint8_t ch, const char* ssid) {
    char ssidEsc[sizeof(((FYDetection*)0)->ssid) * 6 + 1];
    jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
    char oui[9];
    uint8_t mbytes[6] = {0};
    sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
    ouiFromMac(mbytes, oui, sizeof(oui));
    dualPrintf(
        "{\"event\":\"detection\","
        "\"detection_method\":\"wifi_%s\","
        "\"protocol\":\"wifi_2_4ghz\","
        "\"mac_address\":\"%s\","
        "\"oui\":\"%s\","
        "\"device_name\":\"\","
        "\"rssi\":%d,"
        "\"channel\":%u,"
        "\"frequency\":%u,"
        "\"ssid\":\"%s\"}\n",
        method, mac, oui, rssi,
        (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc);
}
```

**Replace with:**
```cpp
static void emitDetectionJSON(const char* mac, const char* method,
                              int8_t rssi, uint8_t ch, const char* ssid) {
    // Skip the JSON line entirely on battery — Flask isn't listening,
    // saves USB CDC overhead. Comment out this guard if you'd rather
    // always emit (e.g. for serial logging to a separate adapter).
    if (!usbHostConnected()) return;

    char ssidEsc[sizeof(((FYDetection*)0)->ssid) * 6 + 1];
    jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
    char oui[9];
    uint8_t mbytes[6] = {0};
    sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
    ouiFromMac(mbytes, oui, sizeof(oui));

    // Build optional GPS suffix.
    char gpsField[96] = "";
    const GpsFix& fix = gpsCurrentFix();
    if (fix.valid) {
        // Flask uses gps.accuracy for the marker radius in meters.
        // HDOP * 5 is a rough conversion (HDOP ~1 = ~5m).
        snprintf(gpsField, sizeof(gpsField),
                 ",\"gps\":{\"latitude\":%.6f,\"longitude\":%.6f,\"accuracy\":%.1f}",
                 fix.lat, fix.lon, fix.hdop * 5.0f);
    }

    dualPrintf(
        "{\"event\":\"detection\","
        "\"detection_method\":\"wifi_%s\","
        "\"protocol\":\"wifi_2_4ghz\","
        "\"mac_address\":\"%s\","
        "\"oui\":\"%s\","
        "\"device_name\":\"\","
        "\"rssi\":%d,"
        "\"channel\":%u,"
        "\"frequency\":%u,"
        "\"ssid\":\"%s\"%s}\n",
        method, mac, oui, rssi,
        (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc, gpsField);
}
```

---

## 5. Add SD logging in the alert drain loop

**Find** (inside `drainAlertQueue`, right after the `emitDetectionJSON(...)` call):
```cpp
        // Flask-compatible JSON line (parsed by api/flockyou.py over USB CDC).
        emitDetectionJSON(macStr, method, e.rssi, e.channel,
                          (e.type == ALERT_SSID) ? e.ssid : "");
```

**Add immediately after that call:**
```cpp
        // Flask-compatible JSON line (parsed by api/flockyou.py over USB CDC).
        emitDetectionJSON(macStr, method, e.rssi, e.channel,
                          (e.type == ALERT_SSID) ? e.ssid : "");

        // SD wardriving log — always-on regardless of USB state.
        sdLogDetection(macStr, method, e.rssi, e.channel,
                       (e.type == ALERT_SSID) ? e.ssid : "");
```

---

## 6. setup() — init the new subsystems

The exact contents of `setup()` aren't in the snippets I have, but the calls go at these conceptual points. Drop them in the order shown:

```cpp
void setup() {
    // ... existing Serial.begin, pinMode for BUZZER/LED, etc ...

    // --- Standalone additions ---
    silentInit();   // do this early so startupBeep respects mute
    gpsInit();      // takes ~5ms to set up Serial1
    sdInit();       // logs result via Serial.printf; non-fatal if it fails
    // ----------------------------

    // ... existing precompileOuis(), SPIFFS init, WiFi init, etc ...
    // ... existing startupBeep() call ...
}
```

**Check for unguarded `Serial1.begin(...)`:** the existing `Serial1.begin(MIRROR_BAUD, ...)` call in `setup()` should already be wrapped in `#if MIRROR_SERIAL`, so flipping that define to 0 takes care of it. If for some reason it's not guarded, delete that line. `gpsInit()` is the only thing that should touch Serial1 now.

---

## 7. loop() — call the pollers each pass

Add three calls anywhere in `loop()`. They're cheap and idempotent. Recommend near the top, before `drainAlertQueue()`:

```cpp
void loop() {
    // --- Standalone additions ---
    gpsPoll();                  // drain NMEA from Serial1
    silentPoll();               // refresh mute state ~4x/sec
    sdMaybeRotateFilename();    // promote /fy_boot_*.csv -> /flockyou_<ts>.csv once
    // ----------------------------

    // ... existing drainAlertQueue() / updateChannelMode() / ledTick() / etc ...
}
```

---

## 8. (Optional) Skip SPIFFS autosave on USB

Not required, but if you want to save flash wear when plugged in (since Flask gets the data live), gate the SPIFFS autosave on battery mode:

**Find the autosave tick** (somewhere in `loop()`, calls `fySaveSession()` every `AUTOSAVE_INTERVAL_MS`).

You can wrap it:
```cpp
if (!usbHostConnected()) {
    autosaveTick();   // or whatever the existing call is
}
```

Skip this if you'd rather always autosave — it's a paranoia option.

---

## What this gets you

- **On battery, no host:** GPS-tagged CSV rows pile up on the SD card. No serial output. Silent switch mutes the buzzer.
- **On USB with Flask:** same SD logging, plus JSON lines stream live to the dashboard. GPS coords are embedded in each JSON message so the existing Flask code picks them up without changes.
- **SD card pulled out:** SPIFFS session save still runs as before. You won't get the per-detection CSV row, but the SPIFFS unique-MAC table survives reboot.
- **No GPS fix yet:** CSV rows have blank `timestamp_utc`, `lat`, `lon` columns. The `uptime_ms` column always has a value, so you can stitch rows to a GPS timeline later if needed.

## Files to add to the repo

```
flock-you_too/
├── main.cpp           (edited per this doc)
├── gps_logger.h       (new)
├── sd_logger.h        (new)
├── silent_mode.h      (new)
├── platformio.ini     (unchanged — SD and SPI are in the ESP32 Arduino core)
├── partitions.csv     (unchanged)
└── ...
```

No new PlatformIO library dependencies. SPI and SD are both part of the espressif32 core.
