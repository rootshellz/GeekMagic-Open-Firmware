// SPDX-License-Identifier: GPL-3.0-or-later
// Current weather pulled directly by the device from Open-Meteo over plain HTTP (no key, ~740 bytes; the firmware has no TLS).
#ifndef SCREENS_WEATHER_H
#define SCREENS_WEATHER_H

#include <Arduino.h>

namespace Weather {

enum class Kind : uint8_t { Clear, Partly, Cloud, Fog, Rain, Snow, Storm };

struct Data {
    float temp = 0;
    float feels = 0;
    float hi = 0;
    float lo = 0;
    float wind = 0;
    int humidity = 0;
    int code = 0;
    bool day = true;
    time_t when = 0;  // wall-clock time of the fetch (0 if the clock was not synced yet)
};

void update();             // call every loop: fetches on a schedule (first after boot, then every 10 min, retry after 60 s on failure)
void refreshSoon();        // e.g. after the location changed
bool hasData();
uint32_t version();        // increments on every successful fetch (screens redraw when it changes)
const Data& data();
Kind kind();
const char* text();        // upper-case condition text, e.g. "PARTLY CLOUDY"
bool lastFetchOk();

}  // namespace Weather

#endif  // SCREENS_WEATHER_H
