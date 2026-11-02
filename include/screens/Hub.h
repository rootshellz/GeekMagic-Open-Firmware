// SPDX-License-Identifier: GPL-3.0-or-later
// Optional link to the screens server (plain HTTP on the LAN): the device polls its config (order, seconds, tz, location)
// and small data documents for native screens (stock quote). Everything keeps working without it.
#ifndef SCREENS_HUB_H
#define SCREENS_HUB_H

#include <Arduino.h>
#include <time.h>

namespace Hub {

struct Quote {
    char sym[12] = "";
    float price = 0;
    float chg = 0;
    float pct = 0;
    bool open = false;
    bool stale = false;  // the server says its upstream data is old
    time_t when = 0;     // device wall-clock time of the last successful fetch
};

bool configured();       // server_url and device_id are both set
void update();           // call every loop
void refreshQuoteSoon(); // e.g. when the quote screen is entered
bool quoteHas();
bool quoteLastOk();
uint32_t quoteVersion(); // increments on every successful quote fetch
const Quote& quote();
bool configLastOk();

}  // namespace Hub

#endif  // SCREENS_HUB_H
