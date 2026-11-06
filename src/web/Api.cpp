// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <Logger.h>
#include <ArduinoJson.h>
#include <Updater.h>

#include "web/Webserver.h"
#include "web/Api.h"
#include "project_version.h"
#include "screens/ScreenManager.h"
#include "screens/Hub.h"
#include "screens/Weather.h"

static constexpr size_t DISPLAY_FRAME_MAX_WIDTH = 240;  // widest row we buffer (the panel is 240x240)
#include "display/DisplayManager.h"

#include "config/ConfigManager.h"
#include "display/Brightness.h"
#include "wireless/WiFiManager.h"
#include "ntp/NTPClient.h"

extern ConfigManager configManager;
extern WiFiManager* wifiManager;
extern NTPClient* ntpClient;

static bool otaError = false;
static size_t otaSize = 0;
static String otaStatus;
static volatile bool otaInProgress = false;
static volatile bool otaCancelRequested = false;
static size_t otaTotal = 0;

static constexpr int OTA_TEXT_X_OFFSET = 50;
static constexpr int OTA_TEXT_Y_OFFSET = 80;
static constexpr int OTA_LOADING_Y_OFFSET = 110;

static void otaHandleStart(HTTPUpload& upload, int mode);
static void otaHandleWrite(HTTPUpload& upload);
static void otaHandleEnd(HTTPUpload& upload, int mode);
static void otaHandleAborted(HTTPUpload& upload);
void handleDeleteGif(Webserver* webserver);

static constexpr int WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr size_t NTP_CONFIG_DOC_SIZE = 512;
static constexpr int BEARER_LEN = 7;

/**
 * @brief Register API endpoints for the webserver
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void registerApiEndpoints(Webserver* webserver) {
    Logger::info("Registering API endpoints", "API");

    // @openapi {get} /wifi/scan version=v1 group=WiFi summary="Scan available WiFi networks" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/scan", HTTP_GET, [webserver]() { handleWifiScan(webserver); });

    // @openapi {post} /wifi/connect version=v1 group=WiFi summary="Connect to a WiFi network" requiresAuth=true
    // requestBody=application/json requestBodySchema=ssid:string,password:string
    // example={"ssid":"MyNetwork","password":"password123"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/connect", HTTP_POST, [webserver]() { handleWifiConnect(webserver); });

    // @openapi {get} /wifi/status version=v1 group=WiFi summary="Get WiFi connection status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/status", HTTP_GET, [webserver]() { handleWifiStatus(webserver); });

    // @openapi {post} /ntp/sync version=v1 group=NTP summary="Trigger NTP sync" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/sync", HTTP_POST, [webserver]() { handleNtpSync(webserver); });

    // @openapi {get} /ntp/status version=v1 group=NTP summary="Get NTP status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/status", HTTP_GET, [webserver]() { handleNtpStatus(webserver); });

    // @openapi {get} /ntp/config version=v1 group=NTP summary="Get NTP configuration" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_GET, [webserver]() { handleNtpConfigGet(webserver); });

    // @openapi {post} /ntp/config version=v1 group=NTP summary="Set NTP configuration" requiresAuth=true
    // requestBody=application/json requestBodySchema=ntp_server:string example={"ntp_server":"pool.ntp.org"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_POST, [webserver]() { handleNtpConfigSet(webserver); });

    // @openapi {get} /display/rotation version=v1 group=Display summary="Get display rotation" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/display/rotation", HTTP_GET, [webserver]() { handleDisplayRotationGet(webserver); });

    // @openapi {post} /display/rotation version=v1 group=Display summary="Set display rotation" requiresAuth=true
    // requestBody=application/json requestBodySchema=rotation:integer example={"rotation":4}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/display/rotation", HTTP_POST, [webserver]() { handleDisplayRotationSet(webserver); });

    // @openapi {post} /reboot version=v1 group=System summary="Reboot the device" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/reboot", HTTP_POST, [webserver]() { handleReboot(webserver); });

    // @openapi {post} /ota/fw version=v1 group=OTA summary="Upload firmware (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fw", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FLASH); });

    // @openapi {post} /ota/fs version=v1 group=OTA summary="Upload filesystem (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fs", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FS); });

    // @openapi {get} /ota/status version=v1 group=OTA summary="Get OTA status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/status", HTTP_GET, [webserver]() { handleOtaStatus(webserver); });

    // @openapi {post} /ota/cancel version=v1 group=OTA summary="Cancel OTA" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/cancel", HTTP_POST, [webserver]() { handleOtaCancel(webserver); });

    // @openapi {post} /gif version=v1 group=GIF summary="Upload a GIF" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/gif", HTTP_POST, [webserver]() { handleGifUpload(webserver); },
        [webserver]() { handleGifUpload(webserver); });

    // @openapi {post} /gif/play version=v1 group=GIF summary="Play a GIF by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"animation.gif"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->raw().on("/api/v1/gif/play", HTTP_POST, [webserver]() { handlePlayGif(webserver); });

    // @openapi {post} /gif/stop version=v1 group=GIF summary="Stop GIF playback" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/gif/stop", HTTP_POST, [webserver]() { handleStopGif(webserver); });

    // @openapi {delete} /gif version=v1 group=GIF summary="Delete a GIF by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"animation.gif"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->raw().on("/api/v1/gif", HTTP_DELETE, [webserver]() { handleDeleteGif(webserver); });

    // @openapi {get} /gif version=v1 group=GIF summary="List GIFs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/gif", HTTP_GET, [webserver]() { handleListGifs(webserver); });

    // @openapi {get} /token/check version=v1 group=Authentication summary="Check bearer token validity"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/token/check", HTTP_GET, [webserver]() { handleTokenCheck(webserver); });

    // @openapi {get} /system version=v1 group=System summary="Get system info (heap, uptime, version)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/system", HTTP_GET, [webserver]() { handleSystemInfo(webserver); });

    // @openapi {get} /screens version=v1 group=Screens summary="Native screens: current screen, order, rotation, time zone, weather" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/screens", HTTP_GET, [webserver]() { handleScreensGet(webserver); });

    // @openapi {post} /screens version=v1 group=Screens summary="Set rotation order/seconds, time zone, weather location, or show a screen now"
    // requiresAuth=true requestBody=application/json
    // requestBodySchema=order:string,secs:integer,tz:string,lat:number,lon:number,server:string,device_id:string,brightness:integer,night_from:string,night_to:string,night_level:integer,show:string,hold:integer
    // example={"order":"clock,weather","secs":20,"show":"weather","hold":60}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/screens", HTTP_POST, [webserver]() { handleScreensSet(webserver); });

    // @openapi {post} /display/frame version=v1 group=Display summary="Stream raw RGB565 pixels to the display (x,y,w,h query)"
    // requiresAuth=true responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/display/frame", HTTP_POST, [webserver]() { handleDisplayFrameDone(webserver); },
        [webserver]() { handleDisplayFrame(webserver); });

    // @openapi {post} /token/save version=v1 group=Authentication summary="Save a new bearer token" requiresAuth=true
    // requestBody=application/json requestBodySchema=token:string example={"token":"your_secure_token_value"}
    // responses=200:application/json,401:application/json,400:application/json
    webserver->raw().on("/api/v1/token/save", HTTP_POST, [webserver]() { handleTokenSave(webserver); });

    // @openapi {get} /logs version=v1 group=System summary="Get recent logs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/logs", HTTP_GET, [webserver]() { handleLogsGet(webserver); });

    // @openapi {get} /logs/download version=v1 group=System summary="Download logs as text file" requiresAuth=true
    // responses=200:text/plain,401:application/json
    webserver->raw().on("/api/v1/logs/download", HTTP_GET, [webserver]() { handleLogsDownload(webserver); });

    // @openapi {post} /logs/clear version=v1 group=System summary="Clear log buffer" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/logs/clear", HTTP_POST, [webserver]() { handleLogsClear(webserver); });

    webserver->raw().onNotFound([webserver]() {
        if (webserver->raw().method() == HTTP_OPTIONS) {
            setCorsHeaders(webserver);
            webserver->raw().send(HTTP_CODE_OK);
        }
    });
}

/**
 * @brief Set CORS headers for API responses
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void setCorsHeaders(Webserver* webserver) {
    webserver->raw().sendHeader("Access-Control-Allow-Origin", "*");
    webserver->raw().sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    webserver->raw().sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webserver->raw().sendHeader("Access-Control-Max-Age", "3600");
}

/**
 * @brief Validate bearer token from Authorization header
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto validateBearerToken(Webserver* webserver) -> bool {
    if (!webserver->raw().hasHeader("Authorization")) {
        return false;
    }

    String authHeader = webserver->raw().header("Authorization");

    if (!authHeader.startsWith("Bearer ")) {
        return false;
    }

    String providedToken = authHeader.substring(BEARER_LEN);
    String storedToken = configManager.getApiToken();

    if (storedToken.length() == 0) {
        return false;
    }

    return providedToken.equals(storedToken);
}

/**
 * @brief Enforce bearer token check and send 401 response if invalid
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto requireBearerToken(Webserver* webserver) -> bool {
    if (validateBearerToken(webserver)) {
        return true;
    }

    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = "Invalid or missing token";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

    Logger::warn(("Unauthorized request from " + webserver->raw().client().remoteIP().toString()).c_str(), "API");

    return false;
}

/**
 * @brief Check if bearer token is valid
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenCheck(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token is valid";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief System info: uptime, heap, version (heap numbers are what matters on an ESP8266 with ~80 KB RAM)
 */
void handleSystemInfo(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["version"] = PROJECT_VER_STR;
    doc["uptimeMs"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();                        // NOLINT(readability-static-accessed-through-instance)
    doc["maxFreeBlock"] = ESP.getMaxFreeBlockSize();            // NOLINT(readability-static-accessed-through-instance)
    doc["heapFragmentation"] = ESP.getHeapFragmentation();      // NOLINT(readability-static-accessed-through-instance)
    doc["resetReason"] = ESP.getResetReason();                  // NOLINT(readability-static-accessed-through-instance)

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

namespace {
auto screensJson() -> String {
    JsonDocument doc;
    doc["current"] = ScreenManager::current();
    doc["available"] = ScreenManager::available();
    doc["order"] = configManager.screen_order.c_str();
    doc["secs"] = configManager.screen_secs;
    doc["tz"] = configManager.tz.c_str();
    doc["lat"] = configManager.weather_lat;
    doc["lon"] = configManager.weather_lon;
    doc["server"] = configManager.server_url.c_str();
    doc["device_id"] = configManager.device_id.c_str();
    doc["serverConfigOk"] = Hub::configLastOk();
    doc["brightness"] = configManager.brightness;
    char clock[8];
    Brightness::formatClock(configManager.night_from, clock, sizeof(clock));
    doc["night_from"] = clock;
    Brightness::formatClock(configManager.night_to, clock, sizeof(clock));
    doc["night_to"] = clock;
    doc["night_level"] = configManager.night_level;
    doc["level"] = Brightness::current();
    JsonObject weather = doc["weather"].to<JsonObject>();
    weather["hasData"] = Weather::hasData();
    weather["lastFetchOk"] = Weather::lastFetchOk();
    if (Weather::hasData()) {
        weather["temp"] = Weather::data().temp;
        weather["text"] = Weather::text();
    }

    String json;
    serializeJson(doc, json);

    return json;
}
}  // namespace

/**
 * @brief GET /api/v1/screens: what the native screens are doing
 */
void handleScreensGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", screensJson());
}

/**
 * @brief POST /api/v1/screens: change rotation order/seconds, time zone, weather location; or show a screen now
 */
void handleScreensSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    auto fail = [webserver](const char* message) {
        JsonDocument err;
        err["status"] = "error";
        err["message"] = message;
        String json;
        serializeJson(err, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
    };

    JsonDocument doc;
    if (!webserver->raw().hasArg("plain") || deserializeJson(doc, webserver->raw().arg("plain"))) {
        fail("Missing or invalid JSON body");

        return;
    }

    bool changed = false;

    if (doc["order"].is<const char*>()) {
        const char* order = doc["order"];
        if (!ScreenManager::setOrder(order)) {
            fail("order has no known screens (available: clock,weather,quote)");

            return;
        }
        configManager.screen_order = order;
        changed = true;
    }

    if (doc["secs"].is<int>()) {
        const int secs = doc["secs"];
        if (secs < 1 || secs > 3600) {
            fail("secs must be 1..3600");

            return;
        }
        configManager.screen_secs = static_cast<uint16_t>(secs);
        changed = true;
    }

    if (doc["tz"].is<const char*>()) {
        const char* tz = doc["tz"];
        if (strlen(tz) == 0 || strlen(tz) > 63) {
            fail("tz must be a POSIX TZ string up to 63 chars, e.g. MST7MDT,M3.2.0,M11.1.0");

            return;
        }
        configManager.tz = tz;
        ScreenManager::applyTimezone(tz);
        changed = true;
    }

    if (doc["server"].is<const char*>() || doc["device_id"].is<const char*>()) {
        const std::string url = doc["server"].is<const char*>() ? doc["server"].as<const char*>() : configManager.server_url;
        const std::string id = doc["device_id"].is<const char*>() ? doc["device_id"].as<const char*>() : configManager.device_id;
        if ((!url.empty() && url.rfind("http://", 0) != 0) || url.size() > 100 || id.size() > 32) {
            fail("server must be empty or http://host[:port] (no TLS), device_id up to 32 chars");

            return;
        }
        configManager.server_url = url;
        configManager.device_id = id;
        changed = true;
    }

    {
        const char* brightnessError = nullptr;
        if (Brightness::applyJson(doc, &brightnessError)) {
            changed = true;
        }
        if (brightnessError != nullptr) {
            fail(brightnessError);

            return;
        }
    }

    if (doc["lat"].is<float>() && doc["lon"].is<float>()) {
        configManager.weather_lat = doc["lat"];
        configManager.weather_lon = doc["lon"];
        Weather::refreshSoon();
        changed = true;
    }

    if (changed) {
        configManager.save();
    }

    if (doc["show"].is<const char*>()) {
        const uint32_t hold = doc["hold"] | 0U;
        if (!ScreenManager::show(doc["show"], hold)) {
            fail("unknown screen (available: clock,weather,quote)");

            return;
        }
    }

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", screensJson());
}

namespace {
// State for the streaming frame upload. Pixels arrive as raw RGB565 words (little-endian, i.e. native uint16_t on the
// ESP8266) in row-major order and are written to the display one row at a time, so no frame buffer is needed.
struct FrameState {
    bool authorized = false;
    bool error = false;
    int16_t x = 0;
    int16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    uint16_t row = 0;      // rows fully drawn so far
    size_t rowFill = 0;    // bytes currently in rowBuf
    uint16_t rowBuf[DISPLAY_FRAME_MAX_WIDTH];
};
FrameState g_frame;
}  // namespace

/**
 * @brief Streaming upload of raw RGB565 pixels (multipart field with any name). Query: x, y, w, h (default: full screen).
 * Uses ~480 bytes of RAM (one row), unlike GIF decoding which needs ~25 KB that the ESP8266 does not have to spare.
 */
void handleDisplayFrame(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();
    Arduino_GFX* gfx = DisplayManager::getGfx();

    if (upload.status == UPLOAD_FILE_START) {
        g_frame.authorized = validateBearerToken(webserver);
        g_frame.error = !g_frame.authorized || gfx == nullptr;
        g_frame.row = 0;
        g_frame.rowFill = 0;

        if (g_frame.error) {
            return;
        }

        auto argInt = [webserver](const char* name, long def) -> long {
            return webserver->raw().hasArg(name) ? webserver->raw().arg(name).toInt() : def;
        };
        const long screenW = gfx->width();
        const long screenH = gfx->height();
        long fx = argInt("x", 0);
        long fy = argInt("y", 0);
        long fw = argInt("w", screenW);
        long fh = argInt("h", screenH);

        if (fx < 0 || fy < 0 || fw <= 0 || fh <= 0 || fx + fw > screenW || fy + fh > screenH ||
            fw > static_cast<long>(DISPLAY_FRAME_MAX_WIDTH)) {
            g_frame.error = true;
            return;
        }

        g_frame.x = static_cast<int16_t>(fx);
        g_frame.y = static_cast<int16_t>(fy);
        g_frame.w = static_cast<uint16_t>(fw);
        g_frame.h = static_cast<uint16_t>(fh);
        DisplayManager::stopGif();  // do not fight a running GIF for the display
        ScreenManager::hold(webserver->raw().hasArg("hold") ? static_cast<uint32_t>(webserver->raw().arg("hold").toInt()) : 60U);
        return;
    }

    if (upload.status != UPLOAD_FILE_WRITE || g_frame.error || gfx == nullptr) {
        return;
    }

    auto* rowBytes = reinterpret_cast<uint8_t*>(g_frame.rowBuf);
    const size_t rowSize = static_cast<size_t>(g_frame.w) * 2U;
    size_t pos = 0;

    while (pos < upload.currentSize && g_frame.row < g_frame.h) {
        const size_t take = min(rowSize - g_frame.rowFill, static_cast<size_t>(upload.currentSize) - pos);
        memcpy(rowBytes + g_frame.rowFill, upload.buf + pos, take);
        g_frame.rowFill += take;
        pos += take;

        if (g_frame.rowFill == rowSize) {
            gfx->draw16bitRGBBitmap(g_frame.x, static_cast<int16_t>(g_frame.y + g_frame.row), g_frame.rowBuf,
                                    static_cast<int16_t>(g_frame.w), 1);
            g_frame.row++;
            g_frame.rowFill = 0;
        }
    }
}

/**
 * @brief Response for the frame upload (runs after the whole body has been streamed)
 */
void handleDisplayFrameDone(Webserver* webserver) {
    JsonDocument doc;
    int code = HTTP_CODE_OK;

    if (!g_frame.authorized) {
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";
        code = HTTP_CODE_UNAUTHORIZED;
        Logger::warn(("Unauthorized frame upload from " + webserver->raw().client().remoteIP().toString()).c_str(), "API");
    } else if (g_frame.error) {
        doc["status"] = "error";
        doc["message"] = "Bad x/y/w/h (must fit the screen; w <= 240)";
        code = HTTP_CODE_BAD_REQUEST;
    } else {
        doc["status"] = g_frame.row == g_frame.h ? "ok" : "partial";
        doc["rows"] = g_frame.row;
        doc["expectedRows"] = g_frame.h;
    }

    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(code, "application/json", json);
}

/**
 * @brief Save a new bearer token
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenSave(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save API token with invalid JSON", "API");

        return;
    }

    const char* newToken = ddoc["token"] | "";

    if (strlen(newToken) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "token field is required";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save empty API token", "API");
        return;
    }

    configManager.setApiToken(newToken);
    configManager.save();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token saved successfully";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("API token updated", "API");
}

/**
 * @brief OTA status endpoint
 */
void handleOtaStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["inProgress"] = otaInProgress;
    doc["bytesWritten"] = otaSize;
    doc["totalBytes"] = otaTotal;
    doc["error"] = otaError;
    doc["message"] = otaStatus;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief OTA cancel endpoint
 */
void handleOtaCancel(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    otaCancelRequested = true;
    otaStatus = "Cancel requested";

    JsonDocument doc;
    doc["status"] = "cancelling";
    doc["message"] = "Cancel request received";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief List GIF files and FS info
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleListGifs(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();

    size_t usedBytes = 0;
    size_t totalBytes = 0;

    if (LittleFS.begin()) {
        Dir dir = LittleFS.openDir("/gif");

        while (dir.next()) {
            String name = dir.fileName();
            if (name.endsWith(".gif") || name.endsWith(".GIF")) {
                JsonObject fileObj = files.add<JsonObject>();

                fileObj["name"] = name;            // NOLINT(readability-misplaced-array-index)
                fileObj["size"] = dir.fileSize();  // NOLINT(readability-misplaced-array-index)
                usedBytes += dir.fileSize();
            }
        }

        FSInfo fs_info;

        if (LittleFS.info(fs_info)) {
            totalBytes = fs_info.totalBytes;
            usedBytes = fs_info.usedBytes;
        }
    }

    doc["usedBytes"] = usedBytes;
    doc["totalBytes"] = totalBytes;
    doc["freeBytes"] = totalBytes > usedBytes ? totalBytes - usedBytes : 0;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload start
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadStart(const String& currentFilename, File& gifFile, bool& uploadError) {
    uploadError = false;
    Logger::info((String("UPLOAD_FILE_START for: ") + currentFilename).c_str(), "API::GIF");

    if (!LittleFS.exists("/gif")) {
        Logger::info("/gif directory does not exist, creating...", "API::GIF");
        if (!LittleFS.mkdir("/gif")) {
            Logger::error("Failed to create /gif directory!", "API::GIF");
        }
    }

    gifFile = LittleFS.open(currentFilename, "w");
    if (!gifFile) {
        uploadError = true;
        Logger::error((String("Impossible to open file: ") + currentFilename).c_str(), "API::GIF");
        Logger::error("GIF UPLOAD Failed to open file", "API::GIF");
    } else {
        Logger::info("File opened successfully for writing.", "API::GIF");
    }
}

/**
 * @brief Handle GIF upload write
 * @param upload Reference to the HTTPUpload object
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadWrite(HTTPUpload& upload, File& gifFile, bool& uploadError) {
    if (!uploadError && gifFile) {
        size_t total = 0;

        while (total < upload.currentSize) {
            size_t remaining = upload.currentSize - total;
            int toWrite = static_cast<int>(remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : remaining);
            size_t written = gifFile.write(upload.buf + total, toWrite);

            if (written == 0) {
                Logger::error("Write returned 0 bytes!", "API::GIF");
                uploadError = true;
                break;
            }

            total += written;
        }
    } else {
        Logger::error("Cannot write, file not open or previous error", "API::GIF");
    }
}

/**
 * @brief Handle GIF upload end
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 *
 * @return void
 */
void handleGifUploadEnd(const String& currentFilename, File& gifFile) {
    if (gifFile) {
        gifFile.close();
    }

    Logger::info((String("Gif upload end: ") + currentFilename).c_str(), "API::GIF");
}

/**
 * @brief Handle GIF upload aborted
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadAborted(const String& currentFilename, File& gifFile, bool& uploadError) {
    Logger::warn("UPLOAD_FILE_ABORTED", "API::GIF");

    if (gifFile) {
        gifFile.close();

        Logger::warn("File closed after abort", "API::GIF");
    }

    if (!currentFilename.isEmpty()) {
        if (LittleFS.remove(currentFilename)) {
            Logger::warn((String("Removed incomplete file: ") + currentFilename).c_str(), "API::GIF");
        } else {
            Logger::error((String("Failed to remove incomplete file: ") + currentFilename).c_str(), "API::GIF");
        }
    }

    uploadError = true;
}

/**
 * @brief Send GIF upload result
 * @param webserver Pointer to the Webserver instance
 * @param currentFilename The current filename being uploaded
 * @param uploadError The upload error flag
 *
 * @return void
 */
void sendGifUploadResult(Webserver* webserver, const String& currentFilename, bool uploadError) {
    JsonDocument doc;

    if (uploadError) {
        doc["status"] = "error";
        doc["message"] = "Error during GIF upload";

        Logger::error("GIF UPLOAD Error during upload", "API::GIF");
    } else {
        doc["status"] = "success";
        doc["message"] = "GIF uploaded successfully";
        doc["filename"] = currentFilename;

        Logger::info((String("Gif upload success, filename: ") + currentFilename).c_str(), "API::GIF");
    }

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleGifUpload(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();
    static File gifFile;
    static bool uploadError = false;

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        uploadError = true;
        JsonDocument doc;

        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    String filename = upload.filename;
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String currentFilename = "/gif/" + filename;

    switch (upload.status) {
        case UPLOAD_FILE_START:
            handleGifUploadStart(currentFilename, gifFile, uploadError);
            break;
        case UPLOAD_FILE_WRITE:
            handleGifUploadWrite(upload, gifFile, uploadError);
            break;
        case UPLOAD_FILE_END:
            handleGifUploadEnd(currentFilename, gifFile);
            break;
        case UPLOAD_FILE_ABORTED:
            handleGifUploadAborted(currentFilename, gifFile, uploadError);
            break;
        default:
            Logger::warn("Unknown upload status.", "API::GIF");
            break;
    }

    if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        sendGifUploadResult(webserver, currentFilename, uploadError);
    }
}

/**
 * @brief Reboot endpoint
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleReboot(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 1000;

    doc["status"] = "rebooting";
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    delay(rebootDelayMs);
    ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
}

/**
 * @brief Manual NTP sync trigger endpoint
 */
void handleNtpSync(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    bool syncOk = ntpClient->syncNow();
    doc["status"] = syncOk ? "ok" : "error";
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Return NTP status
 */
void handleNtpStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    doc["lastOk"] = ntpClient->lastSyncOk();
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get NTP configuration
 */
void handleNtpConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["ntp_server"] = configManager.getNtpServer();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set NTP configuration
 */
void handleNtpConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const char* server = ddoc["ntp_server"] | "";

    if (strlen(server) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "ntp_server missing";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setNtpServer(server);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;

        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    // optionally trigger a sync
    if (ntpClient != nullptr) {
        ntpClient->syncNow();
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["ntp_server"] = server;
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get display rotation configuration
 */
void handleDisplayRotationGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["rotation"] = configManager.getLCDRotationSafe();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set display rotation configuration
 */
void handleDisplayRotationSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err || !ddoc["rotation"].is<int>()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing rotation";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    int rotation = ddoc["rotation"].as<int>();
    const int rotation_range_min = 0;
    const int rotation_range_max = 7;

    if (rotation < rotation_range_min || rotation > rotation_range_max) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] =
            "rotation must be between " + String(rotation_range_min) + " and " + String(rotation_range_max);

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    auto newRotation = static_cast<uint8_t>(rotation);
    configManager.setLCDRotation(newRotation);
    String currentIP = "unknown";

    if (wifiManager != nullptr) {
        currentIP = wifiManager->getIP().toString();
    }

    DisplayManager::setRotation(newRotation, currentIP);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["rotation"] = newRotation;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Display rotation updated to " + String(newRotation)).c_str(), "API");
}

/**
 * @brief Handle OTA upload
 * @param webserver Pointer to the Webserver instance
 * @param mode Update mode U_FLASH U_FS
 *
 * @return void
 */
void handleOtaUpload(Webserver* webserver, int mode) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        otaError = true;
        otaStatus = "Unauthorized";

        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    switch (upload.status) {
        case UPLOAD_FILE_START:
            otaHandleStart(upload, mode);
            break;
        case UPLOAD_FILE_WRITE:
            otaHandleWrite(upload);
            break;
        case UPLOAD_FILE_END:
            otaHandleEnd(upload, mode);
            break;
        case UPLOAD_FILE_ABORTED:
            otaHandleAborted(upload);
            break;
        default:
            break;
    }
}

/**
 * @brief Handle OTA finished
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleOtaFinished(Webserver* webserver) {
    if (!validateBearerToken(webserver)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 5000;

    doc["status"] = "Upload successful";
    doc["message"] = otaStatus;

    if (otaError) {
        doc["status"] = "Error";
    }

    otaInProgress = false;
    otaCancelRequested = false;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    if (!otaError) {
        delay(rebootDelayMs);
        ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
    }
}

/**
 * @brief Play a GIF from LittleFS full screen
 *
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handlePlayGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);

    String path1 = String("/gifs/") + filename;
    String path2 = String("/gif/") + filename;
    String foundPath;

    if (LittleFS.exists(path1)) {
        foundPath = path1;
    } else if (LittleFS.exists(path2)) {
        foundPath = path2;
    } else {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    // One-shot by default: a looping GIF keeps the ~25 KB decoder allocated for as long as it plays, which on an
    // ESP8266 leaves the web server without heap. Callers that really want a loop pass {"loop": true}.
    const bool loop = doc["loop"] | false;
    bool playOk = DisplayManager::playGifFullScreen(foundPath, 0, loop);
    if (playOk) {
        ScreenManager::hold(120U);  // keep the native screens off the display while the GIF owns it
    }

    JsonDocument resp;

    resp["status"] = playOk ? "playing" : "error";
    resp["file"] = foundPath;

    String jsonOut;

    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Stop currently playing GIF
 */
void handleStopGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    const bool stopped = DisplayManager::stopGif();

    resp["status"] = stopped ? "stopped" : "error";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Delete a GIF file from storage
 */
void handleDeleteGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String path = String("/gif/") + filename;

    if (!LittleFS.exists(path)) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    if (LittleFS.remove(path)) {
        JsonDocument resp;
        resp["status"] = "success";
        resp["message"] = "file removed";
        resp["file"] = path;

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);

        Logger::info((String("Removed file: ") + path).c_str(), "API::GIF");
    } else {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "failed to remove file";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        Logger::error((String("Failed to remove file: ") + path).c_str(), "API::GIF");
    }
}

/**
 * @brief Handle WiFi scan
 */
void handleWifiScan(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (wifiManager != nullptr) {
        WiFiManager::scanNetworks(networks);
    }

    String out;
    serializeJson(doc["networks"], out);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", out);
}

/**
 * @brief Handle WiFi connect request
 */
void handleWifiConnect(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";

    if (strlen(ssid) == 0) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "missing ssid";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    bool connectOk = false;
    if (wifiManager != nullptr) {
        connectOk = wifiManager->connectToNetwork(ssid, password, WIFI_CONNECT_TIMEOUT_MS);
    }

    JsonDocument resp;

    resp["status"] = connectOk ? "connected" : "error";
    resp["ssid"] = ssid;

    if (connectOk) {
        resp["ip"] = wifiManager->getIP().toString();
        configManager.setWiFi(ssid, password);
        configManager.save();
    }

    if (!connectOk) {
        resp["message"] = "failed to connect";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief WiFi status
 */
void handleWifiStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    bool connected = (wifiManager != nullptr) && WiFiManager::isConnected();

    resp["connected"] = connected;
    resp["ssid"] = connected ? WiFiManager::getConnectedSSID() : "";
    resp["ip"] = connected ? wifiManager->getIP().toString() : "";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Handle OTA start
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleStart(HTTPUpload& upload, int mode) {
    Logger::info((String("OTA start: ") + upload.filename).c_str(), "API::OTA");

    otaError = false;
    otaSize = 0;
    otaStatus = "";
    otaInProgress = true;
    otaCancelRequested = false;
    otaTotal = static_cast<size_t>(upload.contentLength);

    DisplayManager::clearScreen();
    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Uploading...", 2, LCD_WHITE, LCD_BLACK,
                                    true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

    int constexpr security_space = 0x1000;
    u_int constexpr bin_mask = 0xFFFFF000;

    FSInfo fs_info;
    LittleFS.info(fs_info);
    size_t fsSize = fs_info.totalBytes;
    size_t maxSketchSpace =
        (ESP.getFreeSketchSpace() - security_space) &  // NOLINT(readability-static-accessed-through-instance)
        bin_mask;
    size_t place = (mode == U_FS) ? fsSize : maxSketchSpace;

    if (!Update.begin(place, mode)) {
        otaError = true;
        otaStatus = Update.getErrorString();
        Logger::error((String("Update.begin failed: ") + otaStatus).c_str(), "API::OTA");
    }
}

/**
 * @brief Handle OTA write
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleWrite(HTTPUpload& upload) {
    if (!otaError) {
        if (otaCancelRequested) {
            Update.end();
            otaError = true;
            otaStatus = "Update canceled";
            otaInProgress = false;
            Logger::warn("OTA canceled by user", "API::OTA");

            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Canceled", 2, LCD_WHITE, LCD_BLACK,
                                            true);
            DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

            return;
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaError = true;
            otaStatus = Update.getErrorString();
            Logger::error((String("Write failed: ") + otaStatus).c_str(), "API::OTA");
        }

        otaSize += upload.currentSize;

        float progress = 0.0F;
        if (otaTotal > 0) {
            progress = static_cast<float>(otaSize) / static_cast<float>(otaTotal);
        }

        DisplayManager::drawLoadingBar(progress, OTA_LOADING_Y_OFFSET);
    }
}

/**
 * @brief Handle OTA end
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleEnd(HTTPUpload& /*upload*/, int mode) {
    if (!otaError) {
        if (Update.end(true)) {
            if (mode == U_FS) {
                Logger::info("OTA FS update complete, mounting file system...", "API::OTA");
                LittleFS.begin();
            }

            otaStatus = String("Update OK (") + String(otaSize) + " bytes)";
            Logger::info(otaStatus.c_str(), "API::OTA");

            DisplayManager::drawLoadingBar(1.0F, OTA_LOADING_Y_OFFSET);
            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Success!", 2, LCD_WHITE, LCD_BLACK,
                                            true);
        } else {
            otaError = true;
            otaStatus = Update.getErrorString();
        }
    }
}

/**
 * @brief Handle OTA aborted
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleAborted(HTTPUpload& /*upload*/) {
    Update.end();
    otaError = true;
    otaStatus = "Update aborted";
    otaInProgress = false;
    otaCancelRequested = false;

    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Aborted", 2, LCD_WHITE, LCD_BLACK, true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);
}

/**
 * @brief Get recent logs
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray logsArray = doc["logs"].to<JsonArray>();

    size_t count = Logger::getLogCount();
    for (size_t i = 0; i < count; i++) {
        const char* entry = Logger::getLogEntry(i);
        if (entry != nullptr) {
            logsArray.add(entry);
        }
    }

    doc["count"] = count;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Download logs as a text file
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsDownload(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String logs = Logger::getLogsAsString();

    setCorsHeaders(webserver);
    webserver->raw().sendHeader("Content-Disposition", "attachment; filename=\"logs.log\"");
    webserver->raw().send(HTTP_CODE_OK, "text/plain", logs);
}

/**
 * @brief Clear log buffer
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsClear(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    Logger::clearLogs();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Logs cleared";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}
