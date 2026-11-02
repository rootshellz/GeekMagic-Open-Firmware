// SPDX-License-Identifier: GPL-3.0-or-later
// Native on-device screens (clock, weather) with timed rotation. Host-pushed frames and GIFs take over the display via hold().
#ifndef SCREENS_SCREEN_MANAGER_H
#define SCREENS_SCREEN_MANAGER_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "screens/Weather.h"

namespace ScreenManager {

void begin();                          // apply the saved time zone; call once after the config is loaded
void update();                         // call every loop (after WiFi is up)
bool setOrder(const char* csv);        // "clock,weather"; unknown names are ignored, empty = all. Returns true if something valid was set
bool inRotation(const char* name);       // is this screen in the current rotation order?
bool show(const char* name, uint32_t holdSecs);  // switch to a screen now and keep it for holdSecs (0 = until rotation resumes normally)
void hold(uint32_t secs);              // something else (pushed frame, GIF) owns the display for this long; rotation then resumes
const char* current();                 // name of the screen on the display ("pushed" while held)
const char* available();               // comma separated names
void applyTimezone(const char* tz);    // POSIX TZ string, e.g. "MST7MDT,M3.2.0,M11.1.0"

}  // namespace ScreenManager

namespace ScreenImpl {
void clockEnter();
void clockTick();
void weatherEnter();
void weatherTick();
void quoteEnter();
void quoteTick();
void drawHeader(Arduino_GFX* gfx, const char* title);
void drawWeatherIcon(Arduino_GFX* gfx, Weather::Kind kind, int cx, int cy, int size, bool day);
}  // namespace ScreenImpl

#endif  // SCREENS_SCREEN_MANAGER_H
