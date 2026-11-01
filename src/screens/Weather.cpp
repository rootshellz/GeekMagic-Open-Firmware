// SPDX-License-Identifier: GPL-3.0-or-later
#include "screens/Weather.h"

#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Logger.h>
#include <WiFiClient.h>
#include <time.h>

#include "config/ConfigManager.h"

extern ConfigManager configManager;

namespace {

constexpr uint32_t FIRST_FETCH_AFTER_MS = 15000U;  // let WiFi/NTP settle after boot
constexpr uint32_t REFRESH_MS = 600000U;           // 10 minutes
constexpr uint32_t RETRY_MS = 60000U;
constexpr uint16_t HTTP_TIMEOUT_MS = 2500U;
constexpr time_t REASONABLE_EPOCH = 1700000000;

Weather::Data g_data;
bool g_has = false;
bool g_lastOk = false;
uint32_t g_version = 0;
uint32_t g_nextFetchMs = FIRST_FETCH_AFTER_MS;
bool g_scheduled = false;

auto codeToKind(int code) -> Weather::Kind {
    switch (code) {
        case 0:
        case 1:
            return Weather::Kind::Clear;
        case 2:
            return Weather::Kind::Partly;
        case 3:
            return Weather::Kind::Cloud;
        case 45:
        case 48:
            return Weather::Kind::Fog;
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return Weather::Kind::Snow;
        case 95:
        case 96:
        case 99:
            return Weather::Kind::Storm;
        default:
            return (code >= 51 && code <= 82) ? Weather::Kind::Rain : Weather::Kind::Cloud;
    }
}

auto fetch() -> bool {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    char url[384];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m"
             "&daily=temperature_2m_max,temperature_2m_min&temperature_unit=fahrenheit&wind_speed_unit=mph"
             "&timezone=auto&forecast_days=1",
             static_cast<double>(configManager.weather_lat), static_cast<double>(configManager.weather_lon));

    // The soft watchdog is ~3 s and DNS + connect + GET can take a while: stop it for the duration of this one request
    // (every wait below has its own timeout) and restart it afterwards.
    EspClass::wdtDisable();
    String body;
    int status = -1;
    {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        if (http.begin(client, url)) {
            status = http.GET();
            if (status == HTTP_CODE_OK) {
                body = http.getString();  // handles chunked transfer encoding (getStream() would not)
            }
            http.end();
        }
    }
    EspClass::wdtEnable(WDTO_2S);

    if (status != HTTP_CODE_OK) {
        Logger::warn((String("Weather HTTP status ") + status).c_str(), "Weather");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Logger::warn("Weather JSON parse failed", "Weather");
        return false;
    }

    JsonObject cur = doc["current"];
    if (cur.isNull()) {
        return false;
    }

    Weather::Data d;
    d.temp = cur["temperature_2m"] | 0.0F;
    d.feels = cur["apparent_temperature"] | d.temp;
    d.humidity = cur["relative_humidity_2m"] | 0;
    d.wind = cur["wind_speed_10m"] | 0.0F;
    d.code = cur["weather_code"] | 0;
    d.day = (cur["is_day"] | 1) != 0;
    d.hi = doc["daily"]["temperature_2m_max"][0] | d.temp;
    d.lo = doc["daily"]["temperature_2m_min"][0] | d.temp;
    const time_t now = time(nullptr);
    d.when = now > REASONABLE_EPOCH ? now : 0;

    g_data = d;
    g_has = true;
    g_version++;

    return true;
}

}  // namespace

namespace Weather {

void update() {
    const uint32_t now = millis();

    if (!g_scheduled) {
        g_scheduled = true;
        g_nextFetchMs = now + FIRST_FETCH_AFTER_MS;
    }

    if (static_cast<int32_t>(now - g_nextFetchMs) < 0) {
        return;
    }

    g_lastOk = fetch();
    g_nextFetchMs = millis() + (g_lastOk ? REFRESH_MS : RETRY_MS);
}

void refreshSoon() {
    g_scheduled = true;
    g_nextFetchMs = millis() + 500U;
}

bool hasData() { return g_has; }

uint32_t version() { return g_version; }

const Data& data() { return g_data; }

Kind kind() { return codeToKind(g_data.code); }

const char* text() {
    switch (kind()) {
        case Kind::Clear:
            return g_data.day ? "CLEAR" : "CLEAR NIGHT";
        case Kind::Partly:
            return "PARTLY CLOUDY";
        case Kind::Cloud:
            return "CLOUDY";
        case Kind::Fog:
            return "FOG";
        case Kind::Rain:
            return "RAIN";
        case Kind::Snow:
            return "SNOW";
        case Kind::Storm:
            return "THUNDERSTORM";
    }

    return "";
}

bool lastFetchOk() { return g_lastOk; }

}  // namespace Weather
