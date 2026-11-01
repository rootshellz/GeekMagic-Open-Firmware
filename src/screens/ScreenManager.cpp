// SPDX-License-Identifier: GPL-3.0-or-later
#include "screens/ScreenManager.h"

#include <Logger.h>
#include <time.h>

#include "config/ConfigManager.h"
#include "display/DisplayManager.h"

extern ConfigManager configManager;

namespace {

struct ScreenDef {
    const char* name;
    void (*enter)();
    void (*tick)();
};

const ScreenDef SCREENS[] = {
    {"clock", ScreenImpl::clockEnter, ScreenImpl::clockTick},
    {"weather", ScreenImpl::weatherEnter, ScreenImpl::weatherTick},
};
constexpr size_t SCREEN_COUNT = sizeof(SCREENS) / sizeof(SCREENS[0]);
constexpr uint32_t SPLASH_MS = 6000U;  // keep the boot screen (version, IP) visible for a few seconds
constexpr uint32_t MAX_HOLD_S = 86400U;

uint8_t g_order[SCREEN_COUNT];
size_t g_orderLen = 0;
size_t g_idx = 0;
bool g_entered = false;
uint32_t g_lastSwitchMs = 0;
uint32_t g_holdUntilMs = 0;
uint32_t g_pendingHoldMs = 0;  // applied right after the next enter(), which resets the rotation timer
bool g_held = false;

auto holdMs(uint32_t secs) -> uint32_t {
    const uint32_t s = secs == 0 ? 60U : (secs > MAX_HOLD_S ? MAX_HOLD_S : secs);

    return s * 1000U;
}

auto indexOf(const char* name, size_t len) -> int {
    for (size_t i = 0; i < SCREEN_COUNT; i++) {
        if (strlen(SCREENS[i].name) == len && strncasecmp(SCREENS[i].name, name, len) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void defaultOrder() {
    g_orderLen = SCREEN_COUNT;
    for (size_t i = 0; i < SCREEN_COUNT; i++) {
        g_order[i] = static_cast<uint8_t>(i);
    }
}

void enterCurrent() {
    SCREENS[g_order[g_idx]].enter();
    g_entered = true;
    g_lastSwitchMs = millis() + g_pendingHoldMs;
    g_pendingHoldMs = 0;
}

}  // namespace

namespace ScreenManager {

void applyTimezone(const char* tz) {
    setenv("TZ", (tz != nullptr && tz[0] != '\0') ? tz : "UTC0", 1);
    tzset();
}

bool setOrder(const char* csv) {
    uint8_t order[SCREEN_COUNT];
    size_t len = 0;
    const char* p = csv != nullptr ? csv : "";

    while (*p != '\0' && len < SCREEN_COUNT) {
        const char* end = strchr(p, ',');
        const size_t n = end != nullptr ? static_cast<size_t>(end - p) : strlen(p);
        const int idx = indexOf(p, n);
        if (idx >= 0) {
            order[len++] = static_cast<uint8_t>(idx);
        }
        p += n;
        if (*p == ',') {
            p++;
        }
    }

    if (len == 0) {
        if (csv != nullptr && csv[0] != '\0') {
            return false;  // something was given but nothing matched: keep the current order
        }
        defaultOrder();
    } else {
        memcpy(g_order, order, len);
        g_orderLen = len;
    }

    g_idx = 0;
    g_entered = false;

    return true;
}

void begin() {
    applyTimezone(configManager.tz.c_str());
    setOrder(configManager.screen_order.c_str());
    if (g_orderLen == 0) {
        defaultOrder();
    }
}

bool show(const char* name, uint32_t holdSecs) {
    const int idx = indexOf(name, strlen(name));
    if (idx < 0) {
        return false;
    }

    for (size_t i = 0; i < g_orderLen; i++) {
        if (g_order[i] == idx) {
            g_idx = i;
            g_entered = false;
            g_held = false;
            g_pendingHoldMs = holdSecs > 0 ? holdMs(holdSecs) : 0;  // keep this screen up: pushes the next rotation switch out

            return true;
        }
    }

    // not in the rotation: temporarily put it at the front
    g_order[0] = static_cast<uint8_t>(idx);
    g_orderLen = g_orderLen > 0 ? g_orderLen : 1;
    g_pendingHoldMs = holdSecs > 0 ? holdMs(holdSecs) : 0;
    g_idx = 0;
    g_entered = false;
    g_held = false;

    return true;
}

void hold(uint32_t secs) {
    g_held = true;
    g_holdUntilMs = millis() + holdMs(secs);
    g_entered = false;  // redraw the screen when the hold ends
}

const char* current() {
    if (g_held) {
        return "pushed";
    }

    return g_orderLen > 0 ? SCREENS[g_order[g_idx]].name : "";
}

const char* available() { return "clock,weather"; }

void update() {
    const uint32_t now = millis();
    Weather::update();

    if (now < SPLASH_MS || g_orderLen == 0) {
        return;
    }

    if (g_held) {
        if (static_cast<int32_t>(now - g_holdUntilMs) < 0) {
            return;  // a pushed frame or GIF owns the display
        }
        g_held = false;
    }

    if (!g_entered) {
        enterCurrent();
    }

    SCREENS[g_order[g_idx]].tick();

    const uint32_t secs = configManager.screen_secs > 0 ? configManager.screen_secs : 20U;
    if (g_orderLen > 1 && static_cast<int32_t>(now - g_lastSwitchMs) >= static_cast<int32_t>(secs * 1000U)) {
        g_idx = (g_idx + 1) % g_orderLen;
        enterCurrent();
    }
}

}  // namespace ScreenManager
