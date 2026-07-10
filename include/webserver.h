/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 *          Peter Jakobs http://github.com/pljakobs
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * https://www.gnu.org/copyleft/gpl.html
 *
 * @section DESCRIPTION
 *
 *
 */
#ifndef APP_WEBSERVER_H_
#define APP_WEBSERVER_H_

#include <ArduinoJson.h>
#include <RGBWWLed/RGBWWLedColor.h>
#include <Network/Http/Websocket/WebsocketResource.h>

#define MAX_LOG_LINE_SIZE 512

#define MINIMUM_HEAP_ACCEPT 8000
#define MINIMUM_HEAP 8000

// While the webapp OTA is downloading, the HTTP client + filesystem writes hold
// a large chunk of heap. Any additional JSON endpoint served on top of that
// (e.g. repeated /info polls from the browser) can push the device into OOM.
// During the download we therefore raise the heap floor so non-essential
// requests are shed with 429 instead of being processed into a crash.
#define WEBAPP_OTA_MIN_HEAP 12000

// Inbound HTTP connection limits (ESP8266). Each accepted connection holds lwIP
// TCP buffers + an HttpServerConnection worth of heap. During a webapp OTA the
// outbound download client + LittleFS writes already consume most of the free
// heap, so inbound browser connections are clamped hard for the duration of the
// download to leave headroom and avoid OOM crashes. Restored when OTA finishes.
#ifdef ARCH_ESP8266
    #define WEBSERVER_MAX_CONN_OTA 3
    #define WEBSERVER_MAX_CONN_DEFAULT 4
    #define WEBAPP_OTA_MAX_CONN 3
#else
    #define WEBSERVER_MAX_CONN_OTA 5
    #define WEBSERVER_MAX_CONN_DEFAULT 10
    #define WEBAPP_OTA_MAX_CONN 5
#endif


enum API_CODES {
    API_SUCCESS = 0,
    API_BAD_REQUEST = 1,
    API_MISSING_PARAM = 2,
    API_UNAUTHORIZED = 3,
    API_UPDATE_IN_PROGRESS = 4,
};

class ApplicationWebserver: private HttpServer {
public:
    ApplicationWebserver();
    virtual ~ApplicationWebserver() = default;

    void start();
    void stop();
    void init();
    inline bool isRunning() { return _running; };
    unsigned getHttpActiveConnections() const;
    unsigned getWebsocketConnectionCount() const;

    // Change the max number of simultaneously accepted TCP connections at
    // runtime. Read live by TcpServer::onAccept, so the new limit applies to
    // the next incoming connection; existing connections are untouched. Note
    // that WebSocket connections count toward this same limit.
    void setMaxActiveConnections(uint16_t n);

    // Clamp/restore the inbound connection limit around a webapp OTA download.
    // No-op on non-ESP8266 targets where heap is plentiful.
    void applyOtaLoadShedding(bool otaActive);

    void wsSendBroadcast(const char* buffer, size_t length);

    String getApiCodeMsg(API_CODES code);

private:

    bool _init = false;
    bool _running = false;

    // Base HTTP server settings, retained so setMaxActiveConnections() can
    // adjust the connection limit without discarding heap/keep-alive config.
    HttpServerSettings _serverSettings;

    // Cached security flag: -1=not yet read, 0=unsecured, 1=secured
    int _apiSecuredCache = -1;
    String _apiPasswordCache;

    // Rate-limiting for /webapp_status: cached serialised JSON + timestamp
    String _webappStatusCache;
    unsigned long _webappStatusCacheTime = 0;
    static constexpr unsigned long WEBAPP_STATUS_CACHE_MS = 3000;

    // Short-lived /info cache to avoid heavy per-request ConfigDB churn when UI polls frequently.
    String _infoV1Cache;
    String _infoV2Cache;
    unsigned long _infoV1CacheTime = 0;
    unsigned long _infoV2CacheTime = 0;
    static constexpr unsigned long INFO_CACHE_MS = 1000;

    // Reused for /color POST to avoid per-request stack/heap churn on ESP8266.
    StaticJsonDocument<256> _colorPostDoc;

    WebsocketResource* wsResource = nullptr;
    WebsocketList webSockets;

    bool authenticated(HttpRequest &request, HttpResponse &response);
    bool authenticateExec(HttpRequest &request, HttpResponse &response);
    // Lazily load _apiSecuredCache / _apiPasswordCache from ConfigDB. Cache is
    // invalidated (set to -1) whenever security settings change (see onConfig).
    void ensureSecurityCache();

    void onFile(HttpRequest &request, HttpResponse &response);
    void onIndex(HttpRequest &request, HttpResponse &response);
    void onRedirector(HttpRequest &request, HttpResponse &response);
    void onWebapp(HttpRequest &request, HttpResponse &response);
    void onWebappCheck(HttpRequest &request, HttpResponse &response);
    void onWebappStatus(HttpRequest &request, HttpResponse &response);
    void onConfig(HttpRequest &request, HttpResponse &response);
    void onInfo(HttpRequest &request, HttpResponse &response);
    void onColor(HttpRequest &request, HttpResponse &response);
    void onNetworks(HttpRequest &request, HttpResponse &response);
    void onScanNetworks(HttpRequest &request, HttpResponse &response);
    void onSystemReq(HttpRequest &request, HttpResponse &response);
    void onUpdate(HttpRequest &request, HttpResponse &response);
    void onConnect(HttpRequest &request, HttpResponse &response);
    void onHosts(HttpRequest &request, HttpResponse &response);
    void onPing(HttpRequest &request, HttpResponse &response);
    void onStop(HttpRequest &request, HttpResponse &response);
    void onSkip(HttpRequest &request, HttpResponse &response);
    void onPause(HttpRequest &request, HttpResponse &response);
    void onContinue(HttpRequest &request, HttpResponse &response);
    void onBlink(HttpRequest &request, HttpResponse &response);
    void onToggle(HttpRequest &request, HttpResponse &response);
    void onData(HttpRequest &request, HttpResponse &response);
 
        // SetOn/SetOff endpoints
    void onSetOn(HttpRequest &request, HttpResponse &response);
    void onSetOff(HttpRequest &request, HttpResponse &response);

    void onColorGet(HttpRequest &request, HttpResponse &response);
    void onColorPost(HttpRequest &request, HttpResponse &response);
    bool onColorPostCmd(JsonObject& root, String& errorMsg);

    void addInfoFields(JsonObject& obj);
    void sendApiResponse(HttpResponse &response, JsonObjectStream* stream, HttpStatus code = HTTP_STATUS_OK);
    void sendApiCode(HttpResponse &response, API_CODES code, const char* msg = nullptr);
    void sendApiCode(HttpResponse &response, API_CODES code, const String& msg);
    void sendApiCode(HttpResponse &response, API_CODES code, const __FlashStringHelper* msg);
    bool parseJsonBody(HttpRequest& request, HttpResponse& response, JsonDocument& doc,
                       const String& noBodyMessage);

    //void onUpload(HttpRequest &request, HttpResponse &response);
    bool checkHeap(HttpResponse &response);
    bool checkHeap(HttpResponse &response, uint32_t minHeap);
    bool preflightRequest(HttpRequest& request, HttpResponse& response, std::initializer_list<HttpMethod> allowedMethods, uint32_t minHeap = 0 );
    String makeId();
    
    static bool isPrintable(const String& str);

    void setCorsHeaders(HttpResponse &response);

    void wsConnected(WebsocketConnection& socket);
    void wsDisconnected(WebsocketConnection& socket);
    void wsMessage(WebsocketConnection& socket, const String& message);
};

#endif // APP_WEBSERVER_H_
