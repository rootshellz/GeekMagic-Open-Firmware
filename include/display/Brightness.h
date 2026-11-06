// SPDX-License-Identifier: GPL-3.0-or-later
// Backlight brightness (PWM on the backlight pin) with an optional daily "night" window, e.g. dim/off from 22:00 to 07:00.
// The schedule runs on the device from its own clock, so it keeps working when the server is down.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace Brightness {

// Re-evaluate the schedule and drive the backlight. Cheap; call from loop(). force=true writes the pin even if the level is unchanged.
void update(bool force = false);

// "HH:MM" -> minutes since midnight, or -1 for empty/invalid ("" disables the window).
int parseClock(const char* text);

// Minutes since midnight -> "HH:MM" (buffer of at least 6 bytes); "" when disabled (-1).
void formatClock(int minutes, char* out, size_t size);

// Apply brightness / night_from / night_to ("HH:MM" or "") / night_level from a JSON object (API request or server config).
// Returns true if a setting changed (the caller saves the config). On invalid input nothing is applied and *err names the problem.
bool applyJson(JsonVariantConst doc, const char** err);

// Level (0..100) in effect right now.
uint8_t current();

}  // namespace Brightness
