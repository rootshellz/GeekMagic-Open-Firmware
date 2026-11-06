// SPDX-License-Identifier: GPL-3.0-or-later
#include "display/Brightness.h"

#include <time.h>

#include "config/ConfigManager.h"

extern ConfigManager configManager;

namespace {

constexpr int PWM_RANGE = 255;
constexpr uint16_t PWM_FREQ_HZ = 1000;
constexpr uint32_t CHECK_EVERY_MS = 5000U;
constexpr time_t REASONABLE_EPOCH = 1700000000;

uint8_t g_level = 100;
bool g_written = false;
uint32_t g_nextCheckMs = 0;

// Perceived brightness is roughly logarithmic, so a squared curve makes the middle of the slider look like the middle.
void writeBacklight(uint8_t pct) {
    const int on = static_cast<int>((static_cast<long>(pct) * pct * PWM_RANGE) / 10000L);  // 0..255 "on" duty
    const int duty = LCD_BACKLIGHT_ACTIVE_LOW ? (PWM_RANGE - on) : on;
    pinMode(static_cast<uint8_t>(LCD_BACKLIGHT_GPIO), OUTPUT);
    analogWriteRange(PWM_RANGE);
    analogWriteFreq(PWM_FREQ_HZ);
    analogWrite(static_cast<uint8_t>(LCD_BACKLIGHT_GPIO), duty);  // 0 or the full range stop the PWM and hold the pin steady
}

auto wantedLevel() -> uint8_t {
    const int from = configManager.night_from;
    const int to = configManager.night_to;
    const time_t now = time(nullptr);
    if (from < 0 || to < 0 || from == to || now < REASONABLE_EPOCH) {
        return configManager.brightness;  // no schedule, or the clock is not set yet
    }

    struct tm lt;
    localtime_r(&now, &lt);
    const int minutes = lt.tm_hour * 60 + lt.tm_min;
    const bool night = from < to ? (minutes >= from && minutes < to) : (minutes >= from || minutes < to);

    return night ? configManager.night_level : configManager.brightness;
}

}  // namespace

namespace Brightness {

void update(bool force) {
    const uint32_t ms = millis();
    if (!force && static_cast<int32_t>(ms - g_nextCheckMs) < 0) {
        return;
    }
    g_nextCheckMs = ms + CHECK_EVERY_MS;

    uint8_t level = wantedLevel();
    if (level > 100) {
        level = 100;
    }
    if (force || !g_written || level != g_level) {
        g_level = level;
        g_written = true;
        writeBacklight(level);
    }
}

int parseClock(const char* text) {
    int h = 0;
    int m = 0;
    if (text == nullptr || sscanf(text, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }

    return h * 60 + m;
}

void formatClock(int minutes, char* out, size_t size) {
    if (minutes < 0 || minutes >= 24 * 60) {
        snprintf(out, size, "%s", "");

        return;
    }
    snprintf(out, size, "%02d:%02d", minutes / 60, minutes % 60);
}

bool applyJson(JsonVariantConst doc, const char** err) {
    *err = nullptr;
    int brightness = configManager.brightness;
    int level = configManager.night_level;
    int from = configManager.night_from;
    int to = configManager.night_to;

    if (!doc["brightness"].isNull()) {
        if (!doc["brightness"].is<int>() || doc["brightness"].as<int>() < 0 || doc["brightness"].as<int>() > 100) {
            *err = "brightness must be 0..100";

            return false;
        }
        brightness = doc["brightness"].as<int>();
    }
    if (!doc["night_level"].isNull()) {
        if (!doc["night_level"].is<int>() || doc["night_level"].as<int>() < 0 || doc["night_level"].as<int>() > 100) {
            *err = "night_level must be 0..100";

            return false;
        }
        level = doc["night_level"].as<int>();
    }
    for (const char* key : {"night_from", "night_to"}) {
        if (doc[key].isNull()) {
            continue;
        }
        const char* text = doc[key].is<const char*>() ? doc[key].as<const char*>() : nullptr;
        const int minutes = (text != nullptr && text[0] == '\0') ? -1 : parseClock(text);
        if (text == nullptr || (text[0] != '\0' && minutes < 0)) {
            *err = "night_from / night_to must be \"HH:MM\" or \"\"";

            return false;
        }
        (key[6] == 'f' ? from : to) = minutes;
    }

    const bool changed = brightness != configManager.brightness || level != configManager.night_level ||
                         from != configManager.night_from || to != configManager.night_to;
    configManager.brightness = static_cast<uint8_t>(brightness);
    configManager.night_level = static_cast<uint8_t>(level);
    configManager.night_from = static_cast<int16_t>(from);
    configManager.night_to = static_cast<int16_t>(to);
    if (changed) {
        update(true);
    }

    return changed;
}

uint8_t current() { return g_level; }

}  // namespace Brightness
