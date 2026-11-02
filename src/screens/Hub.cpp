// SPDX-License-Identifier: GPL-3.0-or-later
#include "screens/Hub.h"

#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Logger.h>
#include <WiFiClient.h>

#include "config/ConfigManager.h"
#include "screens/ScreenManager.h"
#include "screens/Weather.h"

extern ConfigManager configManager;

namespace {

constexpr uint32_t FIRST_AFTER_MS = 20000U;
constexpr uint32_t CONFIG_EVERY_MS = 60000U;
constexpr uint32_t QUOTE_EVERY_MS = 60000U;
constexpr uint32_t RETRY_MS = 30000U;
constexpr uint16_t HTTP_TIMEOUT_MS = 2500U;
constexpr time_t REASONABLE_EPOCH = 1700000000;

Hub::Quote g_quote;
bool g_quoteHas = false;
bool g_quoteOk = false;
bool g_configOk = false;
uint32_t g_quoteVersion = 0;
uint32_t g_nextConfigMs = 0;
uint32_t g_nextQuoteMs = 0;
bool g_started = false;

// GET <server_url>/device/<device_id><suffix> and parse the JSON reply.
auto getJson(const char* suffix, JsonDocument& doc) -> bool {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    String url = String(configManager.server_url.c_str()) + "/device/" + configManager.device_id.c_str() + suffix;
    EspClass::wdtDisable();  // the soft watchdog (~3 s) is shorter than DNS + connect + GET; every wait has its own timeout
    String body;
    int status = -1;
    {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        if (http.begin(client, url)) {
            status = http.GET();
            if (status == HTTP_CODE_OK) {
                body = http.getString();  // handles chunked replies too
            }
            http.end();
        }
    }
    EspClass::wdtEnable(WDTO_2S);

    if (status != HTTP_CODE_OK) {
        Logger::warn((String("Hub ") + suffix + " HTTP " + status).c_str(), "Hub");

        return false;
    }

    return !deserializeJson(doc, body);
}

// /config -> order, secs, tz, lat, lon. Applied only when different; saved only when something changed.
auto fetchConfig() -> bool {
    JsonDocument doc;
    if (!getJson("/config", doc)) {
        return false;
    }

    bool changed = false;
    if (doc["tz"].is<const char*>() && configManager.tz != doc["tz"].as<const char*>()) {
        configManager.tz = doc["tz"].as<const char*>();
        ScreenManager::applyTimezone(configManager.tz.c_str());
        changed = true;
    }
    if (doc["lat"].is<float>() && doc["lon"].is<float>() &&
        (fabsf(configManager.weather_lat - doc["lat"].as<float>()) > 0.0001F ||
         fabsf(configManager.weather_lon - doc["lon"].as<float>()) > 0.0001F)) {
        configManager.weather_lat = doc["lat"];
        configManager.weather_lon = doc["lon"];
        Weather::refreshSoon();
        changed = true;
    }
    if (doc["secs"].is<int>() && doc["secs"].as<int>() >= 1 && doc["secs"].as<int>() <= 3600 &&
        configManager.screen_secs != doc["secs"].as<int>()) {
        configManager.screen_secs = static_cast<uint16_t>(doc["secs"].as<int>());
        changed = true;
    }
    if (doc["order"].is<const char*>() && configManager.screen_order != doc["order"].as<const char*>()) {
        if (ScreenManager::setOrder(doc["order"].as<const char*>())) {
            configManager.screen_order = doc["order"].as<const char*>();
            changed = true;
        }
    }
    if (changed) {
        configManager.save();
        Logger::info("Applied config from server", "Hub");
    }

    return true;
}

auto fetchQuote() -> bool {
    JsonDocument doc;
    if (!getJson("/data/quote", doc) || !(doc["ok"] | false)) {
        return false;
    }

    Hub::Quote q;
    strlcpy(q.sym, doc["sym"] | "?", sizeof(q.sym));
    q.price = doc["price"] | 0.0F;
    q.chg = doc["chg"] | 0.0F;
    q.pct = doc["pct"] | 0.0F;
    q.open = doc["open"] | false;
    q.stale = doc["stale"] | false;
    const time_t now = time(nullptr);
    q.when = now > REASONABLE_EPOCH ? now : 0;
    g_quote = q;
    g_quoteHas = true;
    g_quoteVersion++;

    return true;
}

}  // namespace

namespace Hub {

bool configured() { return !configManager.server_url.empty() && !configManager.device_id.empty(); }

void update() {
    if (!configured()) {
        return;
    }

    const uint32_t now = millis();
    if (!g_started) {
        g_started = true;
        g_nextConfigMs = now + FIRST_AFTER_MS;
        g_nextQuoteMs = now + FIRST_AFTER_MS + 5000U;
    }

    if (static_cast<int32_t>(now - g_nextConfigMs) >= 0) {
        g_configOk = fetchConfig();
        g_nextConfigMs = millis() + (g_configOk ? CONFIG_EVERY_MS : RETRY_MS);

        return;  // one request per loop pass keeps the loop responsive
    }

    if (ScreenManager::inRotation("quote") && static_cast<int32_t>(now - g_nextQuoteMs) >= 0) {
        g_quoteOk = fetchQuote();
        g_nextQuoteMs = millis() + (g_quoteOk ? QUOTE_EVERY_MS : RETRY_MS);
    }
}

void refreshQuoteSoon() {
    g_nextQuoteMs = millis() + 300U;
}

bool quoteHas() { return g_quoteHas; }

bool quoteLastOk() { return g_quoteOk; }

uint32_t quoteVersion() { return g_quoteVersion; }

const Quote& quote() { return g_quote; }

bool configLastOk() { return g_configOk; }

}  // namespace Hub
