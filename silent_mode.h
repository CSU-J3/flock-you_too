// ============================================================
// silent_mode.h  —  silent mode switch + USB host detection.
//
// Hardware:
//   SPDT slide switch:  GPIO 1 (D0) <-> GND       (with internal pullup)
//   Closed-to-GND = silent ON, open = silent OFF.
//
// USB host detect uses the built-in Arduino USB CDC class.
// (bool)Serial is true when a host has enumerated the device — i.e.
// "Flask is probably listening." A dumb USB charger won't trip this.
//
// Public API:
//   void silentInit();
//   void silentPoll();         // call from loop(), often
//   bool audioMuted();
//   bool usbHostConnected();
// ============================================================
#pragma once
#include <Arduino.h>

#ifndef SILENT_PIN
#define SILENT_PIN 1            // XIAO D0
#endif
#ifndef SILENT_POLL_MS
#define SILENT_POLL_MS 250
#endif

static bool          g_audioMuted     = false;
static unsigned long g_lastSilentPoll = 0;

inline void silentInit() {
    pinMode(SILENT_PIN, INPUT_PULLUP);
    delay(2);
    g_audioMuted = (digitalRead(SILENT_PIN) == LOW);
}

inline void silentPoll() {
    unsigned long now = millis();
    if (now - g_lastSilentPoll < SILENT_POLL_MS) return;
    g_lastSilentPoll = now;
    g_audioMuted = (digitalRead(SILENT_PIN) == LOW);
}

inline bool audioMuted() { return g_audioMuted; }

inline bool usbHostConnected() {
    // Arduino-ESP32 CDC: (bool)Serial returns true once the host has
    // enumerated and a terminal is attached. False when running on
    // battery alone or with a power-only USB cable.
    return (bool)Serial;
}
