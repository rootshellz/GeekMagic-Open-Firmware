// SPDX-License-Identifier: GPL-3.0-or-later
// Native weather screen + the shared header/icon drawing helpers. Data comes from Weather:: (device-side Open-Meteo fetch).
#include <Logger.h>
#include <math.h>
#include <time.h>

#include "display/DisplayManager.h"
#include "screens/ScreenManager.h"
#include "screens/Theme.h"

namespace {

uint32_t g_shownVersion = 0xFFFFFFFFU;
bool g_shownHadData = false;

void cloudAt(Arduino_GFX* gfx, int x, int y, int w, uint16_t col) {
    const int h = w * 45 / 100;
    gfx->fillCircle(x - w * 28 / 100, y + h * 20 / 100, w * 20 / 100, col);
    gfx->fillCircle(x - w * 4 / 100, y - h * 15 / 100, w * 26 / 100, col);
    gfx->fillCircle(x + w * 24 / 100, y + h * 15 / 100, w * 22 / 100, col);
    gfx->fillRect(x - w * 28 / 100, y + h * 20 / 100, w * 52 / 100, h * 38 / 100, col);
}

void sunAt(Arduino_GFX* gfx, int x, int y, int r, bool day) {
    if (!day) {  // moon: a disc with a bite taken out
        gfx->fillCircle(x, y, r, Theme::rgb(215, 225, 245));
        gfx->fillCircle(x + r / 2, y - r / 4, r, Theme::BG);

        return;
    }
    const uint16_t sun = Theme::rgb(255, 205, 40);
    gfx->fillCircle(x, y, r, sun);
    for (int i = 0; i < 8; i++) {
        const float a = static_cast<float>(M_PI) / 4.0F * static_cast<float>(i);
        gfx->drawLine(x + static_cast<int>(cosf(a) * (r + 3)), y + static_cast<int>(sinf(a) * (r + 3)),
                      x + static_cast<int>(cosf(a) * (r + 7)), y + static_cast<int>(sinf(a) * (r + 7)), sun);
    }
}

void drawStat(Arduino_GFX* gfx, int x, int y, const char* label, const char* value) {
    gfx->setTextSize(1);
    gfx->setTextColor(Theme::LABEL, Theme::BG);
    gfx->setCursor(x, y);
    gfx->print(label);
    gfx->setTextSize(2);
    gfx->setTextColor(Theme::WHITE, Theme::BG);
    gfx->setCursor(x, y + 12);
    gfx->print(value);
}

void drawWeatherFull() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    ScreenImpl::drawHeader(gfx, "WEATHER");

    if (!Weather::hasData()) {
        gfx->setTextSize(3);
        gfx->setTextColor(Theme::DIM, Theme::BG);
        gfx->setCursor(30, 100);
        gfx->print(Weather::lastFetchOk() ? "NO DATA" : "FETCHING...");
        return;
    }

    const Weather::Data& d = Weather::data();
    ScreenImpl::drawWeatherIcon(gfx, Weather::kind(), 54, 78, 84, d.day);

    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(d.temp));
    const int len = static_cast<int>(strlen(buf));
    gfx->setTextSize(7);
    gfx->setTextColor(Theme::GREEN, Theme::BG);
    gfx->setCursor(212 - len * 42, 44);
    gfx->print(buf);
    gfx->setTextSize(3);
    gfx->setTextColor(Theme::LABEL, Theme::BG);
    gfx->setCursor(214, 44);
    gfx->print("F");

    gfx->setTextSize(2);
    gfx->setTextColor(Theme::WHITE, Theme::BG);
    gfx->setCursor(10, 122);
    gfx->print(Weather::text());
    gfx->drawFastHLine(10, 142, 220, Theme::FAINT);

    snprintf(buf, sizeof(buf), "%.0fF", static_cast<double>(d.feels));
    drawStat(gfx, 14, 150, "FEELS LIKE", buf);
    snprintf(buf, sizeof(buf), "%.0f/%.0f", static_cast<double>(d.hi), static_cast<double>(d.lo));
    drawStat(gfx, 126, 150, "HIGH/LOW", buf);
    snprintf(buf, sizeof(buf), "%d%%", d.humidity);
    drawStat(gfx, 14, 188, "HUMIDITY", buf);
    snprintf(buf, sizeof(buf), "%.0f MPH", static_cast<double>(d.wind));
    drawStat(gfx, 126, 188, "WIND", buf);

    gfx->setTextSize(1);
    gfx->setTextColor(Theme::DIM, Theme::BG);
    gfx->setCursor(10, 228);
    if (d.when > 0) {
        struct tm t;
        localtime_r(&d.when, &t);
        snprintf(buf, sizeof(buf), "UPDATED %02d:%02d", t.tm_hour, t.tm_min);
        gfx->print(buf);
    } else {
        gfx->print("UPDATED");
    }
    if (!Weather::lastFetchOk()) {
        gfx->setTextColor(Theme::WARN, Theme::BG);
        gfx->setCursor(150, 228);
        gfx->print("STALE");
    }
}

}  // namespace

namespace ScreenImpl {

void drawHeader(Arduino_GFX* gfx, const char* title) {
    gfx->fillScreen(Theme::BG);
    gfx->setTextSize(1);
    gfx->setTextColor(Theme::GREEN, Theme::BG);
    gfx->setCursor(8, 7);
    gfx->print("> ROOTSHELLZ://");
    gfx->print(title);
    gfx->drawFastHLine(6, 20, 228, Theme::FRAME);
}

void drawWeatherIcon(Arduino_GFX* gfx, Weather::Kind kind, int cx, int cy, int size, bool day) {
    const int r = size / 2;
    const uint16_t cloud = Theme::rgb(170, 190, 200);
    const uint16_t dark = Theme::rgb(110, 125, 140);
    switch (kind) {
        case Weather::Kind::Clear:
            sunAt(gfx, cx, cy, r * 6 / 10, day);
            break;
        case Weather::Kind::Partly:
            sunAt(gfx, cx - r / 4, cy - r / 4, r * 4 / 10, day);
            cloudAt(gfx, cx + r / 10, cy + r * 15 / 100, size * 8 / 10, cloud);
            break;
        case Weather::Kind::Cloud:
            cloudAt(gfx, cx, cy, size * 9 / 10, cloud);
            break;
        case Weather::Kind::Fog:
            cloudAt(gfx, cx, cy - r / 4, size * 8 / 10, dark);
            for (int i = 0; i < 3; i++) {
                const int y = cy + r * (35 + i * 25) / 100;
                gfx->drawFastHLine(cx - r * 7 / 10, y, r * 14 / 10 - i * 4, cloud);
            }
            break;
        case Weather::Kind::Rain:
            cloudAt(gfx, cx, cy - r / 5, size * 85 / 100, dark);
            for (int dx = -1; dx <= 1; dx++) {
                gfx->drawLine(cx + dx * r * 4 / 10, cy + r * 45 / 100, cx + dx * r * 4 / 10 - 4, cy + r * 9 / 10, Theme::INFO);
            }
            break;
        case Weather::Kind::Snow:
            cloudAt(gfx, cx, cy - r / 5, size * 85 / 100, cloud);
            gfx->fillCircle(cx - r * 4 / 10, cy + r * 6 / 10, 2, Theme::WHITE);
            gfx->fillCircle(cx, cy + r * 85 / 100, 2, Theme::WHITE);
            gfx->fillCircle(cx + r * 4 / 10, cy + r * 6 / 10, 2, Theme::WHITE);
            break;
        case Weather::Kind::Storm:
            cloudAt(gfx, cx, cy - r / 5, size * 85 / 100, dark);
            gfx->fillTriangle(cx + 2, cy + r * 2 / 10, cx - 6, cy + r * 65 / 100, cx + 3, cy + r * 55 / 100, Theme::rgb(255, 205, 40));
            gfx->fillTriangle(cx + 3, cy + r * 55 / 100, cx - 4, cy + r, cx + 8, cy + r * 45 / 100, Theme::rgb(255, 205, 40));
            break;
    }
}

void weatherEnter() {
    g_shownVersion = 0xFFFFFFFFU;
    g_shownHadData = false;
    Weather::refreshSoon();  // entering the weather screen: make sure the data is not stale
    drawWeatherFull();
    g_shownVersion = Weather::version();
    g_shownHadData = Weather::hasData();
}

void weatherTick() {
    if (g_shownVersion != Weather::version() || g_shownHadData != Weather::hasData()) {
        drawWeatherFull();
        g_shownVersion = Weather::version();
        g_shownHadData = Weather::hasData();
    }
}

}  // namespace ScreenImpl
