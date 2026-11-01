// SPDX-License-Identifier: GPL-3.0-or-later
// Native clock: NTP time, local time zone with DST (POSIX TZ from the config), redraws only the digits that changed.
#include <Logger.h>
#include <time.h>

#include "display/DisplayManager.h"
#include "screens/ScreenManager.h"
#include "screens/Theme.h"

namespace {

constexpr time_t REASONABLE_EPOCH = 1700000000;
constexpr int CELL_W = 42;  // text size 7: 6 px per column * 7
constexpr int CELL_H = 56;
constexpr int TIME_X = 15;
constexpr int TIME_Y = 34;
constexpr int BAR_X = 15;
constexpr int BAR_Y = 100;
constexpr int BAR_W = 210;
constexpr int BAR_H = 8;
constexpr int DATE_Y = 118;
constexpr int WX_LINE_Y = 146;

char g_cells[5];          // what is currently drawn in the 5 time cells (" 9:41")
char g_date[16];          // "SUN NOV 1"
char g_ampm[3];
int g_lastSec = -1;
uint32_t g_wxVersion = 0xFFFFFFFFU;
bool g_drewSynced = false;

void drawCell(Arduino_GFX* gfx, int i, char c) {
    const int x = TIME_X + i * CELL_W;
    gfx->fillRect(x, TIME_Y, CELL_W, CELL_H, Theme::BG);
    if (c != ' ') {
        gfx->setTextSize(7);
        gfx->setTextColor(Theme::GREEN, Theme::BG);
        gfx->setCursor(x, TIME_Y);
        char s[2] = {c, '\0'};
        gfx->print(s);
    }
}

void drawBar(Arduino_GFX* gfx, int sec) {
    const int inner = BAR_W - 2;
    const int fill = (sec + 1) * inner / 60;
    gfx->fillRect(BAR_X + 1, BAR_Y + 1, inner, BAR_H - 2, Theme::BG);
    gfx->fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, Theme::LABEL);
}

void drawWeatherBlock(Arduino_GFX* gfx) {
    gfx->fillRect(0, WX_LINE_Y + 2, 240, 240 - WX_LINE_Y - 2, Theme::BG);
    gfx->setTextSize(2);
    if (!Weather::hasData()) {
        gfx->setTextColor(Theme::DIM, Theme::BG);
        gfx->setCursor(15, 186);
        gfx->print("WEATHER ...");
        return;
    }
    const Weather::Data& d = Weather::data();
    ScreenImpl::drawWeatherIcon(gfx, Weather::kind(), 44, 192, 52, d.day);
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0fF", static_cast<double>(d.temp));
    gfx->setTextSize(4);
    gfx->setTextColor(Theme::WHITE, Theme::BG);
    gfx->setCursor(92, 158);
    gfx->print(buf);
    gfx->setTextSize(2);
    gfx->setTextColor(Theme::LABEL, Theme::BG);
    gfx->setCursor(92, 196);
    gfx->print(Weather::text());
    snprintf(buf, sizeof(buf), "H %.0f  L %.0f", static_cast<double>(d.hi), static_cast<double>(d.lo));
    gfx->setTextColor(Theme::DIM, Theme::BG);
    gfx->setCursor(92, 216);
    gfx->print(buf);
}

void drawSyncing(Arduino_GFX* gfx) {
    gfx->fillRect(0, 22, 240, 218, Theme::BG);
    gfx->setTextSize(3);
    gfx->setTextColor(Theme::DIM, Theme::BG);
    gfx->setCursor(30, 100);
    gfx->print("SYNCING...");
    gfx->setTextSize(1);
    gfx->setCursor(30, 140);
    gfx->print("waiting for NTP");
}

}  // namespace

namespace ScreenImpl {

void clockEnter() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    drawHeader(gfx, "CLOCK");
    memset(g_cells, 0, sizeof(g_cells));
    g_date[0] = '\0';
    g_ampm[0] = '\0';
    g_lastSec = -1;
    g_wxVersion = 0xFFFFFFFFU;
    g_drewSynced = false;
    gfx->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, Theme::FRAME);
    gfx->drawFastHLine(10, WX_LINE_Y, 220, Theme::FAINT);
}

void clockTick() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    const time_t now = time(nullptr);

    if (now < REASONABLE_EPOCH) {
        if (g_drewSynced || g_lastSec != -2) {
            drawSyncing(gfx);
            g_drewSynced = false;
            g_lastSec = -2;
        }

        return;
    }

    struct tm t;
    localtime_r(&now, &t);

    if (!g_drewSynced) {  // first time with a valid clock (or after "syncing"): redraw the static parts
        gfx->fillRect(0, 22, 240, 218, Theme::BG);
        gfx->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, Theme::FRAME);
        gfx->drawFastHLine(10, WX_LINE_Y, 220, Theme::FAINT);
        memset(g_cells, 0, sizeof(g_cells));
        g_date[0] = '\0';
        g_ampm[0] = '\0';
        g_wxVersion = 0xFFFFFFFFU;
        g_lastSec = -1;
        g_drewSynced = true;
    }

    if (t.tm_sec == g_lastSec) {
        return;
    }

    // time digits: 12 hour clock, leading space instead of a zero, colon blinks with the seconds
    const int h12 = (t.tm_hour % 12) == 0 ? 12 : (t.tm_hour % 12);
    char cells[6];
    snprintf(cells, sizeof(cells), "%2d:%02d", h12, t.tm_min);
    if ((t.tm_sec & 1) != 0) {
        cells[2] = ' ';
    }
    for (int i = 0; i < 5; i++) {
        if (cells[i] != g_cells[i]) {
            drawCell(gfx, i, cells[i]);
            g_cells[i] = cells[i];
        }
    }

    // seconds bar: clear on a new minute, otherwise only the new segment
    if (t.tm_sec < g_lastSec || g_lastSec < 0) {
        drawBar(gfx, t.tm_sec);
    } else {
        const int inner = BAR_W - 2;
        gfx->fillRect(BAR_X + 1 + g_lastSec * inner / 60, BAR_Y + 1, ((t.tm_sec + 1) - g_lastSec) * inner / 60 + 1, BAR_H - 2,
                      Theme::LABEL);
    }
    g_lastSec = t.tm_sec;

    // date + AM/PM only when they change
    char date[16];
    strftime(date, sizeof(date), "%a %b %-d", &t);
    for (char* p = date; *p != '\0'; p++) {
        *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    }
    if (strcmp(date, g_date) != 0) {
        gfx->fillRect(0, DATE_Y, 190, 18, Theme::BG);
        gfx->setTextSize(2);
        gfx->setTextColor(Theme::LABEL, Theme::BG);
        gfx->setCursor(15, DATE_Y);
        gfx->print(date);
        strlcpy(g_date, date, sizeof(g_date));
    }
    const char* ampm = t.tm_hour < 12 ? "AM" : "PM";
    if (strcmp(ampm, g_ampm) != 0) {
        gfx->setTextSize(2);
        gfx->setTextColor(Theme::GREEN, Theme::BG);
        gfx->fillRect(195, DATE_Y, 30, 18, Theme::BG);
        gfx->setCursor(201, DATE_Y);
        gfx->print(ampm);
        strlcpy(g_ampm, ampm, sizeof(g_ampm));
    }

    if (g_wxVersion != Weather::version() || (g_wxVersion == 0xFFFFFFFFU)) {
        drawWeatherBlock(gfx);
        g_wxVersion = Weather::version();
    }
}

}  // namespace ScreenImpl
