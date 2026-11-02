// SPDX-License-Identifier: GPL-3.0-or-later
// Native stock quote screen. Data comes from the screens server (Hub::quote()); the device never talks to a quote API.
#include <math.h>
#include <time.h>

#include "display/DisplayManager.h"
#include "screens/ScreenManager.h"
#include "screens/Hub.h"
#include "screens/Theme.h"

namespace {

uint32_t g_shownVersion = 0xFFFFFFFFU;
bool g_shownHad = false;
bool g_shownOk = false;

void drawQuote() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    ScreenImpl::drawHeader(gfx, "QUOTE");

    if (!Hub::quoteHas()) {
        gfx->setTextSize(3);
        gfx->setTextColor(Theme::DIM, Theme::BG);
        gfx->setCursor(30, 100);
        gfx->print(Hub::configured() ? "FETCHING..." : "NO SERVER");
        if (!Hub::configured()) {
            gfx->setTextSize(1);
            gfx->setCursor(30, 134);
            gfx->print("set server + device_id");
        }

        return;
    }

    const Hub::Quote& q = Hub::quote();
    const bool up = q.chg >= 0;
    const uint16_t col = up ? Theme::GREEN : Theme::CRIT;

    gfx->setTextSize(1);
    gfx->setTextColor(q.open ? Theme::INFO : Theme::DIM, Theme::BG);
    gfx->setCursor(q.open ? 204 : 192, 7);
    gfx->print(q.open ? "OPEN" : "CLOSED");

    gfx->setTextSize(4);
    gfx->setTextColor(Theme::WHITE, Theme::BG);
    gfx->setCursor(14, 40);
    gfx->print(q.sym);

    char buf[24];
    snprintf(buf, sizeof(buf), "$%.2f", static_cast<double>(q.price));
    const int len = static_cast<int>(strlen(buf));
    const int size = len <= 6 ? 6 : 5;
    gfx->setTextSize(size);
    gfx->setTextColor(col, Theme::BG);
    gfx->setCursor((240 - len * 6 * size) / 2, 94);
    gfx->print(buf);

    // arrow + change + percent
    const int ay = 160;
    if (up) {
        gfx->fillTriangle(24, ay + 14, 40, ay + 14, 32, ay, col);
    } else {
        gfx->fillTriangle(24, ay, 40, ay, 32, ay + 14, col);
    }
    snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(fabsf(q.chg)));
    gfx->setTextSize(2);
    gfx->setTextColor(col, Theme::BG);
    gfx->setCursor(50, ay);
    gfx->print(buf);
    snprintf(buf, sizeof(buf), "%+.2f%%", static_cast<double>(q.pct));
    gfx->setCursor(240 - 14 - static_cast<int>(strlen(buf)) * 12, ay);
    gfx->print(buf);

    gfx->drawFastHLine(10, 190, 220, Theme::FAINT);
    gfx->setTextSize(1);
    gfx->setTextColor(Theme::DIM, Theme::BG);
    gfx->setCursor(10, 228);
    if (q.when > 0) {
        struct tm t;
        localtime_r(&q.when, &t);
        snprintf(buf, sizeof(buf), "UPDATED %02d:%02d", t.tm_hour, t.tm_min);
        gfx->print(buf);
    } else {
        gfx->print("UPDATED");
    }
    if (!Hub::quoteLastOk() || q.stale) {
        gfx->setTextColor(Theme::WARN, Theme::BG);
        gfx->setCursor(150, 228);
        gfx->print("STALE");
    }
}

void remember() {
    g_shownVersion = Hub::quoteVersion();
    g_shownHad = Hub::quoteHas();
    g_shownOk = Hub::quoteLastOk();
}

}  // namespace

namespace ScreenImpl {

void quoteEnter() {
    Hub::refreshQuoteSoon();
    drawQuote();
    remember();
}

void quoteTick() {
    if (g_shownVersion != Hub::quoteVersion() || g_shownHad != Hub::quoteHas() || g_shownOk != Hub::quoteLastOk()) {
        drawQuote();
        remember();
    }
}

}  // namespace ScreenImpl
