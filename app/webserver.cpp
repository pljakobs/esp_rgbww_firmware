#include <ArduinoJson.h>
#include <IFS/FileSystem.h>

/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 * 			Peter Jakobs http://github.com/pljakobs
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
 */

#include <RGBWWCtrl.h>
#include <apihandler.h>
#include <Data/WebHelpers/base64.h>
#include <memory>
#include <stdio.h>

#include <Network/Http/Websocket/WebsocketResource.h>
#include <Storage.h>
#include <config.h>
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
extern "C" {
#include <lwip/tcp.h>
}
#endif

#include <fileMap.h>

//#define NOCACHE

namespace {
constexpr size_t INFO_DOC_CAPACITY_V1 = 1536;
constexpr size_t INFO_DOC_CAPACITY_V2 = 2048;
constexpr size_t WS_INFO_RESPONSE_OVERHEAD = 300;
constexpr size_t WS_INFO_RESPONSE_CAPACITY = INFO_DOC_CAPACITY_V1 + WS_INFO_RESPONSE_OVERHEAD;
}

// ToDo: think about implementing a parameterized API to read objects from appData by id

ApplicationWebserver::ApplicationWebserver()
{
	_running = false;
	// keep some heap space free
	// value is a good guess and tested to not crash when issuing multiple parallel requests
	HttpServerSettings settings;
	settings.maxActiveConnections = HTTP_MAX_CONNECTIONS;
	settings.minHeapSize = MINIMUM_HEAP_ACCEPT;
	settings.keepAliveSeconds = 10; // do not close instantly when no transmission occurs. some clients are a bit slow (like FHEM)
	configure(settings);

	// workaround for bug in Sming 3.5.0
	// https://github.com/SmingHub/Sming/issues/1236
	setBodyParser("*", bodyToStringParser);
}

void ApplicationWebserver::init()
{
	paths.setDefault(HttpPathDelegate(&ApplicationWebserver::onFile, this));
	paths.set("/", HttpPathDelegate(&ApplicationWebserver::onIndex, this));
	paths.set(F("/webapp"), HttpPathDelegate(&ApplicationWebserver::onWebapp, this));
	paths.set(F("/config"), HttpPathDelegate(&ApplicationWebserver::onConfig, this));
	paths.set(F("/info"), HttpPathDelegate(&ApplicationWebserver::onInfo, this));
	paths.set(F("/color"), HttpPathDelegate(&ApplicationWebserver::onColor, this));
	paths.set(F("/networks"), HttpPathDelegate(&ApplicationWebserver::onNetworks, this));
	paths.set(F("/scan_networks"), HttpPathDelegate(&ApplicationWebserver::onScanNetworks, this));
	paths.set(F("/webapp_status"), HttpPathDelegate(&ApplicationWebserver::onWebappStatus, this));
	paths.set(F("/webapp_check"), HttpPathDelegate(&ApplicationWebserver::onWebappCheck, this));
	paths.set(F("/system"), HttpPathDelegate(&ApplicationWebserver::onSystemReq, this));
	paths.set(F("/update"), HttpPathDelegate(&ApplicationWebserver::onUpdate, this));
	paths.set(F("/connect"), HttpPathDelegate(&ApplicationWebserver::onConnect, this));
	paths.set(F("/ping"), HttpPathDelegate(&ApplicationWebserver::onPing, this));
	paths.set(F("/hosts"), HttpPathDelegate(&ApplicationWebserver::onHosts, this));
	paths.set(F("/data"), HttpPathDelegate(&ApplicationWebserver::onData, this));

	// basic settings
	paths.set(F("/on"), HttpPathDelegate(&ApplicationWebserver::onSetOn, this));
	paths.set(F("/off"), HttpPathDelegate(&ApplicationWebserver::onSetOff, this));

	// animation controls
	paths.set(F("/stop"), HttpPathDelegate(&ApplicationWebserver::onStop, this));
	paths.set(F("/skip"), HttpPathDelegate(&ApplicationWebserver::onSkip, this));
	paths.set(F("/pause"), HttpPathDelegate(&ApplicationWebserver::onPause, this));
	paths.set(F("/continue"), HttpPathDelegate(&ApplicationWebserver::onContinue, this));
	paths.set(F("/blink"), HttpPathDelegate(&ApplicationWebserver::onBlink, this));
	paths.set(F("/toggle"), HttpPathDelegate(&ApplicationWebserver::onToggle, this));

	// redirectors for initial configuration
	paths.set(F("/canonical.html"), HttpPathDelegate(&ApplicationWebserver::onIndex, this)); 
	paths.set(F("/generate_204"), HttpPathDelegate(&ApplicationWebserver::onIndex, this)); //android
	paths.set(F("/static/hotspot.txt"), HttpPathDelegate(&ApplicationWebserver::onIndex, this));
	paths.set(F("/connecttest.txt"), HttpPathDelegate(&ApplicationWebserver::onIndex, this)); //Windows
	paths.set(F("/hotspot-detect.html"), HttpPathDelegate(&ApplicationWebserver::onIndex, this)); //iOS/macOS
	paths.set(F("/nmcheck.gnome.org"), HttpPathDelegate(&ApplicationWebserver::onIndex, this)); //Linux (NetworkManager)

	// websocket api
	wsResource = new WebsocketResource();
	wsResource->setConnectionHandler([this](WebsocketConnection& socket) { this->wsConnected(socket); });
	wsResource->setMessageHandler([this](WebsocketConnection& socket, const String& message) {
		this->wsMessage(socket, message);
	});
	wsResource->setDisconnectionHandler([this](WebsocketConnection& socket) { this->wsDisconnected(socket); });
	paths.set("/ws", wsResource);

	_init = true;
}

void ApplicationWebserver::wsConnected(WebsocketConnection& socket)
{
	debug_i(ANSI_COLOR_BLUE "===>wsConnected" ANSI_COLOR_RESET);
	webSockets.addElement(&socket);
	debug_i(ANSI_COLOR_BLUE "===>nr of websockets: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, webSockets.size());

	// If a webapp OTA is in progress, push the current state immediately so
	// the updating page doesn't have to wait for the next timed broadcast.
	if(app.webappOta.isActive()) {
		StaticJsonDocument<256> doc;
		JsonObject params = doc.to<JsonObject>();
		app.webappOta.fillStatusJson(params);
		JsonRpcMessage msg(F("webapp_ota_status"));
		msg.setId(0);
		JsonObject root = msg.getParams();
		for(JsonPair kv : params) root[kv.key()] = kv.value();
		socket.sendString(Json::serialize(msg.getRoot()));
	}
}

void ApplicationWebserver::wsDisconnected(WebsocketConnection& socket)
{
	debug_i(ANSI_COLOR_BLUE "<===wsDisconnected" ANSI_COLOR_RESET);
	webSockets.removeElement(&socket);
	debug_i(ANSI_COLOR_BLUE "===>nr of websockets: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, webSockets.size());
}

void ApplicationWebserver::wsMessage(WebsocketConnection& socket, const String& message)
{
    debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::wsMessage: " ANSI_COLOR_GREEN " %s" ANSI_COLOR_RESET, message.c_str());

    StaticJsonDocument<1024> requestDoc;
    String errorMsg;

    if(!Json::deserialize(requestDoc, message)) {
        // Send immediate minimal hardcoded error payload if deserialization fails
        socket.sendString(F("{\"jsonrpc\":\"2.0\",\"error\":\"malformed json\"}"));
        return;
    }

    JsonObject requestRoot = requestDoc.as<JsonObject>();
    String method = requestRoot[F("method")] | String::nullstr;
    JsonVariant requestId = requestRoot[F("id")];

	debug_i(ANSI_COLOR_BLUE "Websocket message: method= " ANSI_COLOR_GREEN "%s" ANSI_COLOR_RESET, method.c_str());

	// Determine target stream capacity based on the specific method requested
	size_t responseCapacity = 512; // Default for simple getters/commands
	if(method == F("info") || method == F("getInfo")) {
		responseCapacity = WS_INFO_RESPONSE_CAPACITY;
	}
	const uint32_t infoHeapSnapshot = app.getFreeHeapSize();

	auto responseStream = std::make_unique<JsonObjectStream>(responseCapacity);
	JsonObject responseRoot = responseStream->getRoot();
	responseRoot[F("jsonrpc")] = F("2.0");

    if(!requestId.isNull()) {
        responseRoot[F("id")] = requestId;
    }

    if(!method.length()) {
        errorMsg = F("missing method");
    } else if(!app.api) {
        errorMsg = F("api not initialized");
    } else {
        JsonObject params = requestRoot[F("params")];
        const bool isColorGetter = method == F("color") && (params.isNull() || params.size() == 0);
        const bool isDataMethod = isColorGetter || method == F("getColor") || method == F("info") ||
                        method == F("getInfo") || method == F("networks") || method == F("getNetworks");

		if(isDataMethod) {
            JsonObject result = responseRoot.createNestedObject(F("result"));
			if(method == F("info") || method == F("getInfo")) {
				if(!app.api->handleInfo(params, result, infoHeapSnapshot)) {
					errorMsg = result[F("error")] | String(F("method not implemented"));
					responseRoot.remove(F("result"));
				}
			} else {
				if(!app.api->dispatch(method, params, result)) {
					errorMsg = result[F("error")] | String(F("method not implemented"));
					responseRoot.remove(F("result"));
				}
			}
        } else {
            if(app.api->dispatchCommand(method, params, errorMsg, false)) {
                JsonObject result = responseRoot.createNestedObject(F("result"));
                result[F("success")] = true;
            }
        }
    }

    if(errorMsg.length()) {
        responseRoot[F("error")] = errorMsg;
    }

	debug_i(ANSI_COLOR_BLUE "Websocket response prepared" ANSI_COLOR_RESET);
	socket.send(responseStream.release(), WS_FRAME_TEXT);
}

/*
*	send a websocket broadcast
*/
void ICACHE_FLASH_ATTR ApplicationWebserver::wsSendBroadcast(const char* buffer, size_t length)
{
    if (!webSockets.isEmpty()) {
        WebsocketConnection* socket = webSockets[0];
        // Use firstSocket as needed
        socket->broadcast(buffer, length, WS_FRAME_TEXT);
    }
}

unsigned ApplicationWebserver::getHttpActiveConnections() const
{
	return activeClients;
}

unsigned ApplicationWebserver::getWebsocketConnectionCount() const
{
	return webSockets.size();
}

void ApplicationWebserver::start()
{
	if(_init == false) {
		init();
	}
	listen(80);
	_running = true;
}

void ApplicationWebserver::stop()
{
	close();
	_running = false;
}

bool ICACHE_FLASH_ATTR ApplicationWebserver::authenticateExec(HttpRequest& request, HttpResponse& response)
{
	{
		debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::authenticated - checking general context" ANSI_COLOR_RESET);
		if(_apiSecuredCache < 0) {
			AppConfig::Root config(*app.cfg);
			_apiSecuredCache = config.security.getApiSecured() ? 1 : 0;
		}
		if(_apiSecuredCache == 0)
			return true;
	} // end AppConfig general context

	debug_d("ApplicationWebserver::authenticated - checking...");

	String userPass = request.getHeader(F("Authorization"));
	if(userPass == String::nullstr) {
		debug_d("ApplicationWebserver::authenticated - No auth header");
		return false; // header missing
	}

	debug_d("ApplicationWebserver::authenticated Auth header: %s", userPass.c_str());

	// header in form of: "Basic MTIzNDU2OmFiY2RlZmc="so the 6 is to get to beginning of 64 encoded string
	userPass = userPass.substring(6); //cut "Basic " from start
	if(userPass.length() > 50) {
		return false;
	}
	{
		debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::authenticated - getting password" ANSI_COLOR_RESET);
		AppConfig::Root config(*app.cfg);
		userPass = base64_decode(userPass);
		//debug_d("ApplicationWebserver::authenticated Password: '%s' - Expected password: '%s'", userPass.c_str(), config.security.getApiPassword.c_str());

		if(userPass.endsWith(config.security.getApiPassword())) {
			return true;
		}
		return false;

	} //end AppConfig general context
}

bool ICACHE_FLASH_ATTR ApplicationWebserver::authenticated(HttpRequest& request, HttpResponse& response)
{
	bool authenticated = authenticateExec(request, response);

	if(!authenticated) {
		response.code = HTTP_STATUS_UNAUTHORIZED;
		response.setHeader(F("WWW-Authenticate"), F("Basic realm=\"RGBWW Server\""));
		response.setHeader(F("401 wrong credentials"), F("wrong credentials"));
		response.setHeader(F("Connection"), F("close"));
	}
	return authenticated;
}

const char* ApplicationWebserver::getApiCodeMsg(API_CODES code)
{
	switch(code) {
	case API_CODES::API_MISSING_PARAM:
		return "missing param";
	case API_CODES::API_UNAUTHORIZED:
		return "authorization required";
	case API_CODES::API_UPDATE_IN_PROGRESS:
		return "update in progress";
	default:
		return "bad request";
	}
}

void ApplicationWebserver::sendApiResponse(HttpResponse& response, JsonObjectStream* stream, HttpStatus code)
{
	if(!checkHeap(response)) {
		if (stream) {
			delete stream;
			stream = nullptr;
		}
		return;
	}

	setCorsHeaders(response);
	response.setHeader(F("accept"), F("GET, POST, OPTIONS"));

	if(code != HTTP_STATUS_OK) {
		response.code = HTTP_STATUS_BAD_REQUEST;
	}
	response.sendDataStream(stream, MIME_JSON);

}

void ApplicationWebserver::sendApiCode(HttpResponse& response, API_CODES code, const String& msg)
{
	sendApiCode(response, code, msg.c_str());
}

void ApplicationWebserver::sendApiCode(HttpResponse& response, API_CODES code, const __FlashStringHelper* msg)
{
	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	setCorsHeaders(response);
	response.setHeader(F("accept"), F("GET, POST, OPTIONS"));

	if(code == API_CODES::API_SUCCESS) {
		json[F("success")] = true;
		sendApiResponse(response, stream.release(), HTTP_STATUS_OK);
		return;
	}

	if(code == API_CODES::API_UPDATE_IN_PROGRESS) {
		debug_i(ANSI_COLOR_BLUE "API update in progress, adding info to response" ANSI_COLOR_RESET);
		JsonObject data = json.createNestedObject(F("info"));
		addInfoFields(data);
	}

	if(msg == nullptr) {
		json[F("error")] = getApiCodeMsg(code);
	} else {
		json[F("error")] = msg;
	}
	sendApiResponse(response, stream.release(), HTTP_STATUS_BAD_REQUEST);
}

void ApplicationWebserver::addInfoFields(JsonObject& obj)
{
	JsonObject dev = obj.createNestedObject(F("device"));

	dev[F("deviceid")] = system_get_chip_id();
	dev[F("soc")] = SOC;
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
	dev[F("current_rom")] = String(app.ota.getRomPartition().name());
#endif
	JsonObject application = obj.createNestedObject(F("app"));
	{
		AppConfig::Root::Webapp webappCfg(*app.cfg);
		String installedVer = webappCfg.getInstalledVersion();
		application[F("webapp_version")] = installedVer.length() > 0 ? installedVer : String(WEBAPP_VERSION);
	}
	application[F("git_version")] = fw_git_version;
	application[F("build_type")] = BUILD_TYPE;
	application[F("git_date")] = fw_git_date;

	JsonObject sming = obj.createNestedObject(F("sming"));
	sming[F("version")] = SMING_VERSION;
	JsonObject run = obj.createNestedObject(F("runtime"));
	run[F("uptime")] = app.getUptime();
	run[F("heap_free")] = app.getFreeHeapSize();
	run[F("minimumfreeHeapRuntime")]=app.getMinimumHeapUptime();
	run[F("minimumfreeHeap10min")]=app.getMinimumHeap10min();
	run[F("heapLowErrUptime")]=app.getHeapLowErrUptime();
	run[F("heapLowErr10min")]=app.getHeapLowErr10min();

	if(app.isFilesystemMounted()) {
		auto* fs = IFS::getDefaultFileSystem();
		if(fs != nullptr) {
			IFS::FileSystem::Info fsInfo{};
			if(fs->getinfo(fsInfo) == IFS::Error::Success) {
				JsonObject lfs = obj.createNestedObject(F("lfs"));
				lfs[F("total")] = (uint32_t)fsInfo.volumeSize;
				lfs[F("used")]  = (uint32_t)fsInfo.used();
				lfs[F("free")]  = (uint32_t)fsInfo.freeSpace;
			}
		}
	}
}

void ApplicationWebserver::sendApiCode(HttpResponse& response, API_CODES code, const char* msg)
{
	if(msg == nullptr) {
		sendApiCode(response, code, (const __FlashStringHelper*)nullptr);
		return;
	}

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	setCorsHeaders(response);
	response.setHeader(F("accept"), F("GET, POST, OPTIONS"));

	if(code == API_CODES::API_SUCCESS) {
		json[F("success")] = true;
		sendApiResponse(response, stream.release(), HTTP_STATUS_OK);
	} else {
		if(code == API_CODES::API_UPDATE_IN_PROGRESS) {
			debug_i(ANSI_COLOR_BLUE "API update in progress, adding info to response" ANSI_COLOR_RESET);
			JsonObject data = json.createNestedObject(F("info"));
			addInfoFields(data);
		}

		json[F("error")] = msg;
		sendApiResponse(response, stream.release(), HTTP_STATUS_BAD_REQUEST);
	}
}

bool ApplicationWebserver::parseJsonBody(HttpRequest& request, HttpResponse& response, JsonDocument& doc,
											 const __FlashStringHelper* noBodyMessage)
{
	String body = request.getBody();
	if(body == NULL) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, noBodyMessage);
		return false;
	}

	DeserializationError err = deserializeJson(doc, body);
	if(err) {
		char parseError[96];
		snprintf(parseError, sizeof(parseError), "Invalid JSON: %s", err.c_str());
		sendApiCode(response, API_CODES::API_BAD_REQUEST, parseError);
		return false;
	}

	return true;
}

bool ApplicationWebserver::parseJsonBody(HttpRequest& request, HttpResponse& response, JsonDocument& doc,
											 const String& noBodyMessage)
{
	return parseJsonBody(request, response, doc,
					 noBodyMessage.length() ? noBodyMessage.c_str() : nullptr);
}

void ApplicationWebserver::onFile(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "http onFile" ANSI_COLOR_RESET);
	// LittleFS file serving buffers through lwIP — require more free heap than API calls.
	if(!preflightRequest(request, response, {HttpMethod::GET, HttpMethod::HEAD}, 10000)) return;

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		response.setContentType(MIME_TEXT);
		response.code = HTTP_STATUS_SERVICE_UNAVAILABLE;
		response.sendString(F("OTA in progress"));
		return;
	}
#endif
// Use client caching for better performance.
#ifndef NOCACHE
	//response.setCache(86400, true);
	response.setHeader(F("Cache-Control"),F("public, max-age=604800, immutable"));
#endif

	String fileName = request.uri.Path;
	debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onFile with uri path=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,fileName.c_str());
	if(fileName[0] == '/')
		fileName = fileName.substring(1);
	if(fileName[0] == '.') {
		response.code = HTTP_STATUS_FORBIDDEN;
		return;
	}

	String compressed = fileName + ".gz";
	debug_i(ANSI_COLOR_BLUE "searching file name " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, compressed.c_str());
	auto v = fileMap[compressed];
	if(v) {
		debug_i(ANSI_COLOR_BLUE "found" ANSI_COLOR_RESET);
		response.headers[HTTP_HEADER_CONTENT_ENCODING] = _F("gzip");
	} else {
		debug_i(ANSI_COLOR_GREEN "searching file name %s" ANSI_COLOR_RESET, fileName.c_str());
		v = fileMap[fileName];
		if(!v) {
			debug_i(ANSI_COLOR_YELLOW "file %s not found in filemap" ANSI_COLOR_RESET, fileName.c_str());
			if(!app.isFilesystemMounted()) {
				response.setContentType(MIME_TEXT);
				response.code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
				response.sendString(F("No filesystem mounted"));
				return;
			}
			if(!fileExist(fileName) && !fileExist(fileName + ".gz") && WifiAccessPoint.isEnabled()) {
				//if accesspoint is active and we couldn`t find the file - redirect to index
				debug_d(ANSI_COLOR_GREEN "ApplicationWebserver::onFile redirecting" ANSI_COLOR_RESET);
				response.headers[HTTP_HEADER_LOCATION] = F("http://") + WifiAccessPoint.getIP().toString() + "/";
			} else {
#ifndef NOCACHE
				//response.setCache(604800, true); // It's important to use cache for better performance.
				response.setHeader(F("Cache-Control"),F("public, max-age=604800, immutable"));
#endif
				// sendFile with allowGzipFileCheck=true: tries fileName+".gz" first, sets
				// Content-Encoding:gzip, and infers MIME from fileName (not fileName.gz).
				debug_i(ANSI_COLOR_GREEN "sending file %s with gzip check" ANSI_COLOR_RESET, fileName.c_str());
				response.sendFile(fileName, true);
			}
			return;
		}
	}

	debug_i(ANSI_COLOR_BLUE "found " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " in fileMap" ANSI_COLOR_RESET, String(v.key()).c_str());
	auto stream = std::make_unique<FSTR::Stream>(v.content());
	response.sendDataStream(stream.release(), ContentType::fromFullFileName(fileName));

}
void ApplicationWebserver::onWebapp(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "http onWebapp" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, {HttpMethod::GET})) return;

	response.headers[HTTP_HEADER_LOCATION] = F("/index.html");
	setCorsHeaders(response);

	response.code = HTTP_STATUS_PERMANENT_REDIRECT;
	response.sendString(F("Redirecting to /index.html"));
}

void ApplicationWebserver::onIndex(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "http onIndex" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, {HttpMethod::GET})) return;
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		response.setContentType(MIME_TEXT);
		response.code = HTTP_STATUS_SERVICE_UNAVAILABLE;
		response.sendString(F("OTA in progress"));
		return;
		void publishTransitionFinished(const String& name, bool requeued = false);
	}
#endif

	bool hasLfsIndex = app.isFilesystemMounted() &&
	     (fileExist(F("index.html")) || fileExist(F("index.html.gz")));

	// Case 1: AP active with no WiFi credentials → serve captive portal
	if(WifiAccessPoint.isEnabled() && !WifiStation.isConnected() && !hasLfsIndex) {
		debug_i(ANSI_COLOR_BLUE "onIndex: serving captive portal" ANSI_COLOR_RESET);
		auto v = fileMap[F("captive.html")];
		if(v) {
			setCorsHeaders(response);
			response.headers[HTTP_HEADER_CACHE_CONTROL] = F("no-store");
			auto stream = std::make_unique<FSTR::Stream>(v.content());
			response.sendDataStream(stream.release(), MIME_HTML);
			return;
		}
	}

	// Case 2: WiFi connected but webapp not yet in LFS → show progress page
	if(WifiStation.isConnected() && !hasLfsIndex) {
		debug_i(ANSI_COLOR_BLUE "onIndex: serving updating page" ANSI_COLOR_RESET);
		// Kick off webapp OTA if not already running
		if(!app.webappOta.isActive()) {
			app.webappOta.checkForUpdate();
		}
		auto v = fileMap[F("updating.html")];
		if(v) {
			setCorsHeaders(response);
			// Must not be cached — the page content changes with firmware and its
			// poll interval is baked in. Heuristic caching would serve a stale copy.
			response.headers[HTTP_HEADER_CACHE_CONTROL] = F("no-store");
			auto stream = std::make_unique<FSTR::Stream>(v.content());
			response.sendDataStream(stream.release(), MIME_HTML);
			return;
		}
	}

	// Normal: LFS webapp is present, redirect to index.html
	response.headers[HTTP_HEADER_LOCATION] = F("/index.html");
	setCorsHeaders(response);
	response.code = HTTP_STATUS_PERMANENT_REDIRECT;
	response.sendString(F("Redirecting to /index.html"));
}

void ApplicationWebserver::onWebappCheck(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "http onWebappCheck" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, {HttpMethod::GET, HttpMethod::POST})) return;
	if(!checkHeap(response)) return;

	if(request.method == HttpMethod::POST) {
		if(!app.webappOta.isActive()) {
			app.webappOta.checkForUpdate(true /* ignoreEnabled: manual trigger */);
		}
	}

	// Return current OTA status (same as /webapp_status but cache-busted)
	DynamicJsonDocument doc(512);
	JsonObject json = doc.to<JsonObject>();
	app.webappOta.fillStatusJson(json);
	String body;
	serializeJson(doc, body);

	setCorsHeaders(response);
	response.headers[HTTP_HEADER_CACHE_CONTROL] = F("no-store");
	response.headers[HTTP_HEADER_CONTENT_TYPE] = F("application/json");
	response.sendString(body);
}

void ApplicationWebserver::onWebappStatus(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "http onWebappStatus" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, {HttpMethod::GET}, 12000)) return;

	unsigned long now = millis();
	// Bypass cache when OTA is idle (terminal state) so updating.html sees the final result immediately.
	bool otaActive = app.webappOta.isActive();
	if(_webappStatusCache.length() == 0 || !otaActive || (now - _webappStatusCacheTime) >= WEBAPP_STATUS_CACHE_MS) {
		DynamicJsonDocument doc(512);
		JsonObject json = doc.to<JsonObject>();
		app.webappOta.fillStatusJson(json);
		_webappStatusCache = String();
		serializeJson(doc, _webappStatusCache);
		_webappStatusCacheTime = now;
	}

	setCorsHeaders(response);
	response.headers[HTTP_HEADER_CONTENT_TYPE] = F("application/json");
	response.sendString(_webappStatusCache);
}

bool ApplicationWebserver::checkHeap(HttpResponse& response)
{
	return checkHeap(response, MINIMUM_HEAP);
}

bool ApplicationWebserver::checkHeap(HttpResponse& response, int minHeap)
{
	if(!app.checkHeap(minHeap) ) {
		setCorsHeaders(response);
		response.code = HTTP_STATUS_TOO_MANY_REQUESTS;
		
		// Smart backoff: scale based on how far we are below threshold
		// If OTA is active, wait longer to avoid hammering during download
		// Otherwise, backoff increases with severity of heap shortage
		const char* retryAfterHeader;
		if(app.webappOta.isActive()) {
			retryAfterHeader = "30";  // OTA download: give 30 seconds
		} else {
			// Scale backoff: 4s for slightly low, 10s for critically low
			uint32_t freeHeap = app.getFreeHeapSize();
			if(freeHeap < (minHeap / 2)) {
				retryAfterHeader = "10";  // Critical: wait 10 seconds
			} else if(freeHeap < (minHeap * 3 / 4)) {
				retryAfterHeader = "5";   // Moderate: wait 6 seconds
			} else {
				retryAfterHeader = "2";   // Light: wait 4 seconds
			}
		}
		
		response.setHeader(F("Retry-After"), retryAfterHeader);
		debug_e(ANSI_COLOR_RED "Not enough heap free, rejecting request. Free heap: " ANSI_COLOR_CYAN "%u" ANSI_COLOR_RED " bytes" ANSI_COLOR_RESET, app.getFreeHeapSize());
		return false;
	}
	return true;
}

/**
 * @brief Helper to handle standard checks: Heap, CORS(Options), Method allow-list, Global Auth
 * 
 * @param minHeap Optional minimum heap required (default 0 loops back to _minimumHeap)
 * @return true if request is valid and should proceed. false if response handled (e.g. error or options)
 */
bool ApplicationWebserver::preflightRequest(HttpRequest& request, HttpResponse& response, std::initializer_list<HttpMethod> allowedMethods, int minHeap)
{
    // Default to no-cache for API/dynamic checks. 
    // Static file handler (onFile) will override this if caching is desired.
	debug_i(ANSI_COLOR_BLUE "preflightRequest: %d %s" ANSI_COLOR_RESET, (int)request.method, request.uri.Path.c_str());
    response.setHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
    response.setHeader(F("Pragma"), F("no-cache"));
    response.setHeader(F("Expires"), F("0"));

    // 1. Heap Check
    if(!checkHeap(response, minHeap)) {
		debug_i(ANSI_COLOR_RED "preflightRequest: %d %s - Not enough heap, rejecting request" ANSI_COLOR_RESET, (int)request.method, request.uri.Path.c_str());
		return false;
    }

   // 2. CORS Preflight (OPTIONS) - Must handle this before method check or Auth
    if(request.method == HttpMethod::OPTIONS) {
        setCorsHeaders(response);
        sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
        debug_i(ANSI_COLOR_BLUE "Handled OPTIONS preflight (generic)" ANSI_COLOR_RESET);
        return false; // Handled, stop processing
    }

    // 3. Method validation
    bool methodAllowed = false;

	char allowedMethodsStr[64] = {0};
	size_t allowedPos = 0;
	for(auto m : allowedMethods) {
		int written = snprintf(allowedMethodsStr + allowedPos, sizeof(allowedMethodsStr) - allowedPos,
						   (allowedPos == 0) ? "%d" : ", %d", (int)m);
		if(written <= 0) {
			break;
		}
		if((size_t)written >= (sizeof(allowedMethodsStr) - allowedPos)) {
			allowedPos = sizeof(allowedMethodsStr) - 1;
			break;
		}
		allowedPos += (size_t)written;
	}
    
    for(auto m : allowedMethods) {
        if(request.method == m) {
            methodAllowed = true;
            break;
        }
    }

    if(!methodAllowed) {
        setCorsHeaders(response);
		char msg[128];
		snprintf(msg, sizeof(msg), "Method not allowed. Allowed: %s. Current: %d", allowedMethodsStr,
				 (int)request.method);
		debug_i(ANSI_COLOR_RED "preflightRequest: %d %s - %s" ANSI_COLOR_RESET, (int)request.method,
				request.uri.Path.c_str(), msg);
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
        return false;
    }

    // 4. Global Authentication
    // Responds with 401 if security is enabled and auth fails
	
    if(!authenticated(request, response)) {
		debug_i(ANSI_COLOR_RED "preflightRequest: %d %s - Authentication failed" ANSI_COLOR_RESET, (int)request.method, request.uri.Path.c_str());
        return false;
    }

    // 5. Ensure CORS headers for actual response

    setCorsHeaders(response);
    return true;
}

void ApplicationWebserver::onConfig(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "onConfig" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 12000)) return;

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif



	if(request.method == HttpMethod::POST) {
		debug_i(ANSI_COLOR_BLUE "======================\nHTTP POST request received, " ANSI_COLOR_RESET);
		app.telemetryClient.log(F("onConfig POST"));
		// Invalidate the cached security flag so any password/secured changes take effect immediately.
		_apiSecuredCache = -1;

		/* ConfigDB importFomStream */
		String oldIP, oldSSID, oldDeviceName, oldCurrentPinConfigName, oldSyslogHost;
		bool mqttEnabled, dhcpEnabled,oldSyslogEnabled,oldTelemetryEnabled;
		int oldColorMode,oldSyslogPort;
		{
			debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig storing old settings" ANSI_COLOR_RESET);
			app.telemetryClient.log(F("onConfig storing old settings"));
			AppConfig::Network network(*app.cfg);
			oldIP = network.connection.getIp();
			oldSSID = network.ap.getSsid();
			mqttEnabled = network.mqtt.getEnabled();
			oldSyslogEnabled=network.rsyslog.getEnabled();
			oldTelemetryEnabled=network.telemetry.getStatsEnabled();
			oldSyslogPort=network.rsyslog.getPort();
			oldSyslogHost=network.rsyslog.getHost();
			dhcpEnabled=network.connection.getDhcp();
		}
		{
			AppConfig::General general(*app.cfg);
			oldDeviceName=general.getDeviceName();
			oldCurrentPinConfigName=general.getCurrentPinConfigName();
		}
		
		{
			AppConfig::Color color(*app.cfg);
			oldColorMode=color.getColorMode();
			// TODO: Store other color settings if needed		
		}

		auto bodyStream = request.getBodyStream();
		if(bodyStream) {
			ConfigDB::Status status = app.cfg->importFromStream(ConfigDB::Json::format, *bodyStream);

			/*********************************
             * TODO
             * - if network settings changed (ip config, default gateway, netmask, ssid, hostname(?) ) -> reboot 
             * - if mqtt settings changed to enabled -> start mqtt
             *   - if mqtt broker changed -> reconnect
             *   - if mqtt topic changed -> resubscribe
             *   - if mqtt master/secondary changed -> resubscribe to master/secondary topics where necessary
             * - if mqtt settings changed to disabled -> stop mqtt if possile, otherwise reboot
             * - if color setttings changed - reconfigure controller (see below)
             **********************************/

			// bool restart = root[F("restart")] | false;

			
			String newIP, newSSID, newDeviceName, newCurrentPinConfigName, newSyslogHost;
			bool newMqttEnabled,newDhcpEnabled,newSyslogEnabled, newTelemetryEnabled;
			int newColorMode,newSyslogPort;
			{
				debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig getting new settings" ANSI_COLOR_RESET);
				app.telemetryClient.log(F("onConfig getting new settings"));
				AppConfig::Network network(*app.cfg);
				newIP = network.connection.getIp();
				newSSID = network.ap.getSsid();
				newMqttEnabled = network.mqtt.getEnabled();
				newDhcpEnabled=network.connection.getDhcp();
				newSyslogEnabled=network.rsyslog.getEnabled();
				newTelemetryEnabled=network.telemetry.getStatsEnabled();
				newSyslogHost=network.rsyslog.getHost();
				newSyslogPort=network.rsyslog.getPort();
			}
			{
				AppConfig::General general(*app.cfg);
				newDeviceName=general.getDeviceName();
				newCurrentPinConfigName=general.getCurrentPinConfigName();
			}
			{
				AppConfig::Color color(*app.cfg);
				newColorMode=color.getColorMode();
			}
			
			/*
			*
			* handle ip address change - this will require the controller to reboot 
			* this only happens if the user configures a new fixed ip address, so they
			* are expectd to know that and reconnect to the new address (or the old
			* dns / mDNS name, once updated )
			* 
			*/
			if(oldIP != newIP) {
				//if (restart) {
				debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig ip settings changed - rebooting" ANSI_COLOR_RESET);
				app.telemetryClient.log(F("onConfig ip settings changed - rebooting"));
				String msg = F("new IP, ")+newIP;
				app.wsBroadcast(F("notification"), msg);
				app.telemetryClient.log(msg);
				app.delayedCMD(F("restart"), 3000); // wait 3s to first send response
			}

			/*
			*
			* handle wifi ssid change - this will require the controller to reboot 
			* this is a bit more tricky than the ip address change as we don't know
			* what new ip address the controller will get once connecting to a new 
			* wifi network.
			* not sure if much can be done about it - maybe
			* todo: see if we can connect to the new wifi, record the ip -address, go back to
			* the old wifi, send the new address to the frontend using websocket and then reboot. This would allow the frontend to automatically reconnect to the new address after the controller has rebooted and connected to the new wifi. But it is a bit of a hassle to implement and test, so for now we just reboot and expect the user to reconnect manually using the new ip address or dns / mDNS name.
			* 
			*/
			if(oldSSID != newSSID) {
				//
				if(WifiAccessPoint.isEnabled()) {
					debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig wifiap settings changed - rebooting" ANSI_COLOR_RESET);
					app.telemetryClient.log(F("onConfig wifiap settings changed - rebooting"));
					// report the fact that the system will restart to the frontend
					String msg = F("new SSID, ")+newSSID;
					app.wsBroadcast(F("notification"), msg);
					app.telemetryClient.log(msg);
					app.delayedCMD(F("restart"), 3000); // wait 3s to first send response
				}
			}

			/*
			*
			* mqtt changes - those will be handled on the fly 
			*
			*/
			if(mqttEnabled != newMqttEnabled) {
				if(newMqttEnabled) {
					if(!app.mqttclient.isRunning()) {
						debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig mqtt settings changed - starting mqtt" ANSI_COLOR_RESET);
						app.telemetryClient.log(F("onConfig mqtt settings changed - starting mqtt"));
						app.mqttclient.start();
					}
				} else {
					if(app.mqttclient.isRunning()) {
						debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig mqtt settings changed - stopping mqtt" ANSI_COLOR_RESET);
						app.telemetryClient.log(F("onConfig mqtt settings changed - stopping mqtt"));
						app.mqttclient.stop();
					} else {
						debug_i(ANSI_COLOR_BLUE "mqttclient was not running, no need to stop" ANSI_COLOR_RESET);
					}
				}
			
			}
			
			/*
			* 
			* telemetry changes - those will be handled on the fly 
			*
			*/
			if(newTelemetryEnabled!=oldTelemetryEnabled){
				if(newTelemetryEnabled){
					debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig telemetry settings changed - starting telemetry" ANSI_COLOR_RESET);
					app.telemetryClient.start();
				}else{
					debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig telemetry settings changed - stopping telemetry" ANSI_COLOR_RESET);
					app.telemetryClient.stop();
				}
			}
			
			/*
			* 
			* DHCP changes - if DHCP is enabled we can just switch to DHCP, 
			* if DHCP is disabled we need to reboot to apply the new static ip settings
			*
			*/
			if(newDhcpEnabled!=dhcpEnabled){
				if(newDhcpEnabled){
					WifiStation.enableDHCP(true);
				}else{
					debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig ip settings changed - rebooting" ANSI_COLOR_RESET);
					app.telemetryClient.log(F("onConfig ip settings changed - rebooting"));
					String msg = F("new IP, ")+newIP;
					app.wsBroadcast(F("notification"), msg);
					app.telemetryClient.log(msg);
					app.delayedCMD(F("restart"), 3000); // wait 3s to first send response
				}
			}
			
			/*
			*
			* pin config changes - those will require a reboot
			*
			*/
			if(oldCurrentPinConfigName!=newCurrentPinConfigName){
				String msg = F("Channel config has changed - rebooting ");
				app.wsBroadcast(F("notification"), msg);
				app.telemetryClient.log(msg);
				app.delayedCMD(F("restart"),1000);
			}

			/*
			*
			* device name changes - change all necessary services on the fly and send notification 
			* to frontend, no reboot required
			*
			*/
			if(oldDeviceName!=newDeviceName){
				String msg = F("device name change, old Device Name: ")+oldDeviceName+F(", new Device Name: ")+newDeviceName;
				AppConfig::Network::OuterUpdater network(*app.cfg);
				network.mdns.setName(app.sanitizeName(newDeviceName));
				app.wsBroadcast(F("notification"), msg);
				app.telemetryClient.log(msg);
				app.mdnsService.setHostname(newDeviceName);
				//app.delayedCMD(F("restart"),1000);
			}

			/*
			*
			* syslog changes - those will be handled on the fly 
			*
			*/
			if(oldSyslogHost!=newSyslogHost || oldSyslogPort!=newSyslogPort){
#ifndef SMING_RELEASE
				app.udpSyslogStream.begin(newSyslogHost,newSyslogPort);
#endif
			}

			/*
			*
			* syslog enable/disable changes - those will be handled on the fly 
			*
			*/
			if(oldSyslogEnabled!=newSyslogEnabled){
#ifndef SMING_RELEASE
				app.udpSyslogStream.setStatus(newSyslogEnabled);
#endif
			}

			debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE ", " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,newColorMode,oldColorMode);
			if (newColorMode!=oldColorMode){
				// color Mode has been updated, requires reconfiguration, will restart for now
				debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onConfig color settings changed - restarting" ANSI_COLOR_RESET);
				app.telemetryClient.log(F("onConfig color settings changed - restarting"));
				String msg=F("Color Mode changed");
				app.wsBroadcast(F("notification"), msg);
				app.telemetryClient.log(msg);
				app.delayedCMD(F("restart"), 1000); // wait 1s to first send response
			}


			//bodyStream->seekOrigin(0,SeekOrigin::Start);
			//app.wsBroadcast(F("config"),bodyStream->moveString());
			/* ConfigDB ToDo
            if (color_updated) {
                debug_d("ApplicationWebserver::onConfig color settings changed - refreshing");

                //refresh settings
                app.rgbwwctrl.setup();

                //refresh current output
                app.rgbwwctrl.refresh();

            }
            */

			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		} else {
			//CofigDB provide correct error message

			//debug_i(ANSI_COLOR_BLUE "config api error " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,error_msg.c_str());
			//JsonObject root = doc.as<JsonObject>();
			//sendApiCode(response, API_CODES::API_MISSING_PARAM, error_msg);
			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_MISSING_PARAM);
		}

	} else {
		/*
         * /config GET
         */
		setCorsHeaders(response);
		app.telemetryClient.log(F("onConfig GET"));

		auto configStream = app.cfg->createExportStream(ConfigDB::Json::format);
		response.sendDataStream(configStream.release(), MIME_JSON);
	}
}

void ApplicationWebserver::onInfo(HttpRequest& request, HttpResponse& response){
	debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onInfo" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, { HttpMethod::GET },app.ota.isProccessing() ? 8000 : 0)) return;

	// Build params from query string
	StaticJsonDocument<64> paramsDoc;
	JsonObject params = paramsDoc.to<JsonObject>();
	String versionParam = request.getQueryParameter(F("V"));
	if(!versionParam.length()) {
		versionParam = request.getQueryParameter(F("v"));
	}
	if(versionParam.length()) {
		params[F("V")] = versionParam;
	}
	const uint32_t infoHeapSnapshot = app.getFreeHeapSize();

	const bool isV2 = versionParam == "2";
	auto stream = std::make_unique<JsonObjectStream>(isV2 ? INFO_DOC_CAPACITY_V2 : INFO_DOC_CAPACITY_V1);
	JsonObject data = stream->getRoot();
	
	// Call the shared handler
	app.api->handleInfo(params, data, infoHeapSnapshot);
	
	sendApiResponse(response, stream.release());
}


void ApplicationWebserver::onColorGet(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "onColorGet" ANSI_COLOR_RESET);
    /*
	if(!checkHeap(response,2000))
		return;
    */

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	JsonObject raw = json.createNestedObject("raw");
	ChannelOutput output = app.rgbwwctrl.getCurrentOutput();
	raw[F("r")] = output.r;
	raw[F("g")] = output.g;
	raw[F("b")] = output.b;
	raw[F("ww")] = output.ww;
	raw[F("cw")] = output.cw;

	JsonObject hsv = json.createNestedObject("hsv");
	float h, s, v;
	int ct;
	HSVCT c = app.rgbwwctrl.getCurrentColor();
	c.asRadian(h, s, v, ct);
	hsv[F("h")] = h;
	hsv[F("s")] = s;
	hsv[F("v")] = v;
	hsv[F("ct")] = ct;

	setCorsHeaders(response);

	sendApiResponse(response, stream.release());

}

/**
 * @brief Handles the HTTP POST request for updating the color.
 * 
 * This function is responsible for processing the HTTP POST request and updating the color based on the received body.
 * If the body is empty, it sends a bad request response with the message "no body".
 * If the color update is successful, it sends a success response.
 * If the color update fails, it sends a bad request response with the corresponding error message.
 * 
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onColorPost(HttpRequest& request, HttpResponse& response)
{
	StaticJsonDocument<1024> doc;
	if(!parseJsonBody(request, response, doc, F("no body"))) {
		return;
	}

	debug_i(ANSI_COLOR_BLUE "received color update with body length" ANSI_COLOR_CYAN " %i " ANSI_COLOR_RESET, request.getBody().length());
	String msg;
	const bool ok = app.api->dispatchCommand(F("color"), doc.as<JsonObject>(), msg, true);

	if(!ok) {
		debug_i(ANSI_COLOR_BLUE "received color update with message " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, msg.c_str());
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	} else {
		debug_i(ANSI_COLOR_BLUE "received color update with message " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, msg.c_str());
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	}
}

/**
 * @brief Handles the color request from the client.
 *
 * This function is responsible for handling the color request from the client.
 * It checks for authentication, handles OTA update in progress, sets the necessary headers,
 * and delegates the request to the appropriate handler based on the HTTP method.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onColor(HttpRequest& request, HttpResponse& response){
	if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET})) return;
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif
	debug_i(ANSI_COLOR_BLUE "received /color request" ANSI_COLOR_RESET);

	bool error = false;
	if(request.method == HttpMethod::POST) {
		debug_i(ANSI_COLOR_BLUE "POST" ANSI_COLOR_RESET);
		ApplicationWebserver::onColorPost(request, response);
	} else if(request.method == HttpMethod::GET) {
		debug_i(ANSI_COLOR_BLUE "GET" ANSI_COLOR_RESET);
		ApplicationWebserver::onColorGet(request, response);
	} else {
		debug_i(ANSI_COLOR_BLUE "found unimplementd http_method " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, (int)request.method);
	}
}

/**
 * @brief Checks if a string is printable.
 *
 * This function checks if a given string is printable, i.e., if all characters in the string
 * have ASCII values greater than or equal to 0x20 (space character).
 *
 * @param str The string to be checked.
 * @return True if the string is printable, false otherwise.
 */
bool ApplicationWebserver::isPrintable(const String& str)
{
	for(unsigned int i = 0; i < str.length(); ++i) {
		char c = str[i];
		if(c < 0x20)
			return false;
	}
	return true;
}

/**
 * @brief Handles the HTTP request for retrieving network information.
 *
 * This function is responsible for handling the HTTP GET request for retrieving network information.
 * It checks if the request is authenticated and if OTA update is in progress. If not, it returns the
 * available networks along with their details such as SSID, signal strength, and encryption method.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onNetworks(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "onNetworks" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, {HttpMethod::GET})) return;
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	bool error = false;

	if(app.network.isScanning()) {
		json[F("scanning")] = true;
	} else {
		json[F("scanning")] = false;
		JsonArray netlist = json.createNestedArray(F("available"));
		BssList networks = app.network.getAvailableNetworks();
		for(unsigned int i = 0; i < networks.count(); i++) {
			if(networks[i].hidden)
				continue;

			// SSIDs may contain any byte values. Some are not printable and will cause the javascript client to fail
			// on parsing the message. Try to filter those here
			if(!ApplicationWebserver::isPrintable(networks[i].ssid)) {
				debug_w(ANSI_COLOR_YELLOW "Filtered SSID due to unprintable characters: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_YELLOW "" ANSI_COLOR_RESET, networks[i].ssid.c_str());
				continue;
			}

			JsonObject item = netlist.createNestedObject();
			item[F("id")] = (int)networks[i].getHashId();
			item[F("ssid")] = networks[i].ssid;
			item[F("signal")] = networks[i].rssi;
			item[F("encryption")] = networks[i].getAuthorizationMethodName();
			//limit to max 25 networks
			if(i >= 25)
				break;
		}
	}
	setCorsHeaders(response);
	sendApiResponse(response, stream.release());

}

/**
 * @brief Handles the "onScanNetworks" request from the webserver.
 *
 * This function is responsible for handling the "onScanNetworks" request from the webserver.
 * It checks if the request is authenticated, if OTA update is in progress, and if the request method is HTTP POST.
 * If all conditions are met, it initiates a network scan and sends a success response.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onScanNetworks(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

    /*
	if(!checkHeap(response)) {
		return;
	}
	if(!authenticated(request, response)) {
		return;
	}
    */

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

    /*
	if(request.method != HttpMethod::POST) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP POST"));
		return;
	}
    */
	if(!app.network.isScanning()) {
		app.network.scan(false);
	}

	sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
}

/**
 * @brief Handles the HTTP connection event.
 *
 * This function is called when a client connects to the web server.
 * It performs authentication, checks for ongoing OTA updates, and handles HTTP requests.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onConnect(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "onConnect" ANSI_COLOR_RESET);
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET})) return;

    /*
	if(!checkHeap(response)) {
		return;
	}
	debug_i(ANSI_COLOR_BLUE "onConnect request.method: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, request.method);
	if(request.method == HttpMethod::OPTIONS) {
		setCorsHeaders(response);
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		return;
	}
	
	if(!authenticated(request, response)) {
		return;
	}
    */

	debug_i(ANSI_COLOR_BLUE "passed checks" ANSI_COLOR_RESET);
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

    /*
	if(request.method != HttpMethod::POST && request.method != HttpMethod::GET) {
		debug_i(ANSI_COLOR_BLUE "not HTTP POST or GET" ANSI_COLOR_RESET);
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP POST or GET"));
		return;
	}
    */

	if(request.method == HttpMethod::POST) {
		debug_i(ANSI_COLOR_BLUE "is POST" ANSI_COLOR_RESET);
		StaticJsonDocument<512> doc;
		if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
			return;
		}
		String ssid;
		String password;
		if(Json::getValue(doc[F("ssid")], ssid)) {
			password = doc[F("password")].as<const char*>();
			debug_d("ssid %s - pass %s", ssid.c_str(), password.c_str());
			app.network.connect(ssid, password, true);
			sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
			return;

		} else {
			sendApiCode(response, API_CODES::API_MISSING_PARAM);
			return;
		}
	} else {
		auto stream = std::make_unique<JsonObjectStream>();
		JsonObject json = stream->getRoot();

		CONNECTION_STATUS status = app.network.get_con_status();
		json[F("status")] = int(status);
		if(status == CONNECTION_STATUS::ERROR) {
			json[F("error")] = app.network.get_con_err_msg();
		} else if(status == CONNECTION_STATUS::CONNECTED) {
			// return connected
			debug_i(ANSI_COLOR_BLUE "wifi connected, checking if dhcp enabled" ANSI_COLOR_RESET);
			AppConfig::Network network(*app.cfg);

			if(network.connection.getDhcp()) {
				json[F("ip")] = WifiStation.getIP().toString();
			} else {
				String ip = network.connection.getIp();
				json[F("ip")] = ip;
			}
			json[F("dhcp")] = network.connection.getDhcp() ? F("True") : F("False");
			json[F("ssid")] = WifiStation.getSSID();
		}
		sendApiResponse(response, stream.release());
	}
}

/**
 * @brief Handles the system request from the client.
 *
 * This function is responsible for handling the system request from the client. It performs the following tasks:
 * - Checks if the client is authenticated.
 * - Checks if an OTA update is in progress (only for ESP8266 architecture).
 * - Handles HTTP OPTIONS request by setting the cross-domain origin header and sending a success API code.
 * - Handles HTTP POST request by processing the request body and executing the specified command.
 * - Sends the appropriate API code response based on the success or failure of the request.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onSystemReq(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
    
/*
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif
*/
	if(!app.api) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("api not initialized"));
		return;
	}

	StaticJsonDocument<512> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onSystemReq" ANSI_COLOR_RESET);
	String errorMsg;
	const bool ok = app.api->dispatchCommand(F("system"), doc.as<JsonObject>(), errorMsg, false);

	setCorsHeaders(response);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_MISSING_PARAM, errorMsg);
	}
}

/**
 * @brief Handles the update request from the client.
 *
 * This function is responsible for handling the update request from the client.
 * It performs authentication, checks the request method, and processes the update request.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onUpdate(HttpRequest& request, HttpResponse& response)
{
	if(!checkHeap(response)) {
		return;
	}
	if(!authenticated(request, response)) {
		return;
	}

#ifdef ARCH_HOST
	sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not supported on Host"));
	return;
#else
	if(request.method == HttpMethod::OPTIONS) {
		// probably a CORS request
		setCorsHeaders(response);
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		debug_i(ANSI_COLOR_BLUE "/update HttpMethod::OPTIONS Request, sent API_SUCCSSS" ANSI_COLOR_RESET);
		return;
	}
	if(request.method != HttpMethod::POST && request.method != HttpMethod::GET) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP POST or GET"));
		return;
	}

	if(request.method == HttpMethod::POST) {
		if(app.ota.isProccessing()) {
			sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
			return;
		}

		StaticJsonDocument<512> doc;
		if(!parseJsonBody(request, response, doc, F("could not parse HTTP body"))) {
			return;
		}
		String romurl;
		Json::getValue(doc[F("rom")][F("url")], romurl);

		//String spiffsurl;
		//Json::getValue(doc[F("spiffs")][F("url")],spiffsurl);

		debug_i(ANSI_COLOR_BLUE "starting update process with \n    romurl: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, romurl.c_str());
		if(romurl == "") {
			debug_i(ANSI_COLOR_BLUE "missing rom url" ANSI_COLOR_RESET);
			sendApiCode(response, API_CODES::API_MISSING_PARAM);
		} else {
			app.ota.start(romurl);
			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		}
		return;
	}
	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();
	json[F("status")] = int(app.ota.getStatus());
	sendApiResponse(response, stream.release());

#endif
}

/**
 * @brief Handles the HTTP GET request for the ping endpoint.
 *
 * simple call-response to check if we can reach server
 * 
 * This function is responsible for handling the HTTP GET request for the ping endpoint.
 * It checks if the request method is GET, and if not, it sends a bad request response.
 * If the request method is GET, it creates a JSON object with the key "ping" and value "pong",
 * and sends the JSON response using the sendApiResponse function.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onPing(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::GET})) return;

    /*
	if(!checkHeap(response)) {
		return;
	}
	if(request.method != HttpMethod::GET) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP GET"));
		return;
	}
    */
	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();
	json[F("ping")] = "pong";
	sendApiResponse(response, stream.release());
}

void ApplicationWebserver::onStop(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
    
    /*
	if(!checkHeap(response)) {
		return;
	}
	if(request.method != HttpMethod::POST) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP POST"));
		return;
	}
    */

	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	const bool ok = app.api->dispatchCommand(F("stop"), doc.as<JsonObject>(), msg, true);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::onSkip(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
    
	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	const bool ok = app.api->dispatchCommand(F("skip"), doc.as<JsonObject>(), msg, true);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::onPause(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
    
	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	const bool ok = app.api->dispatchCommand(F("pause"), doc.as<JsonObject>(), msg, true);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::onContinue(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
    
	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	const bool ok = app.api->dispatchCommand(F("continue"), doc.as<JsonObject>(), msg, true);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::onBlink(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	const bool ok = app.api->dispatchCommand(F("blink"), doc.as<JsonObject>(), msg, true);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::onToggle(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
    
	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	const bool ok = app.api->dispatchCommand(F("toggle"), doc.as<JsonObject>(), msg, true);

	if(ok) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, msg);
	}
}


void ApplicationWebserver::onHosts(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::GET})) return;

    if(!app.controllers) {
        setCorsHeaders(response);
		debug_i(ANSI_COLOR_BLUE "Controllers not initialized" ANSI_COLOR_RESET);
        sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Controllers not initialized"));
        return;
    }

    bool showAll = request.getQueryParameter(F("all")) == "1" || request.getQueryParameter(F("all")) == "true";
	bool showDebug= request.getQueryParameter(F("debug"))== "1" || request.getQueryParameter(F("debug")) == "true";

    Controllers::JsonFilter filter;
    if (showAll || showDebug) {
        filter = Controllers::ALL_ENTRIES;  // Show all controllers including incomplete ones
    } else {
        filter = Controllers::VISIBLE_ONLY; // Show only visible/online controllers
    }

    setCorsHeaders(response);
    response.setContentType(MIME_JSON);

    // Use the JsonStream for automatic streaming
    auto stream = app.controllers->createJsonStream(filter, false); // Compact format for HTTP
    response.sendDataStream(stream.release(), MIME_JSON);

//todo 
}

void ApplicationWebserver::onData(HttpRequest& request, HttpResponse& response){
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 12000)) return;
    
	if(request.method==HttpMethod::GET){	
		setCorsHeaders(response);

		response.setContentType(F("application/json"));

		auto dataStream = app.data->createExportStream(ConfigDB::Json::format);
		response.sendDataStream(dataStream.release(), MIME_JSON);

	} else if (request.method==HttpMethod::POST){

		auto bodyStream = request.getBodyStream();
		if(bodyStream) {
			debug_i(ANSI_COLOR_BLUE "received Data bodyStream" ANSI_COLOR_RESET);
			ConfigDB::Status status = app.data->importFromStream(ConfigDB::Json::format, *bodyStream);
			String statusMsg = status.toString();
			if(status){
				debug_i(ANSI_COLOR_BLUE "successfully updated app-data" ANSI_COLOR_RESET);
				sendApiCode(response, API_CODES::API_SUCCESS, statusMsg);
			}else{
				debug_i(ANSI_COLOR_BLUE "could not update app-data" ANSI_COLOR_RESET);
				sendApiCode(response, API_CODES::API_BAD_REQUEST, statusMsg);
			}
		}else{
			debug_i(ANSI_COLOR_BLUE "could not get bodyStream" ANSI_COLOR_RESET);
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("could not get bodyStream"));
		}
	}

	return;
}

void ApplicationWebserver::onSetOn(HttpRequest &request, HttpResponse &response) {
    if(!preflightRequest(request, response, {HttpMethod::POST}, 4000)) return;
    

	debug_i(ANSI_COLOR_BLUE "onSetOn" ANSI_COLOR_RESET);

	if(!app.api) {
		sendApiCode(response, API_BAD_REQUEST, F("api not initialized"));
		return;
	}

	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	if(app.api->dispatchCommand(F("setOn"), doc.as<JsonObject>(), msg, true)) {
		sendApiCode(response, API_SUCCESS, F("SetOn OK"));
	} else {
		sendApiCode(response, API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::onSetOff(HttpRequest &request, HttpResponse &response) {
    if(!preflightRequest(request, response, {HttpMethod::POST}, 4000)) return;
    
	debug_i(ANSI_COLOR_BLUE "onSetOff" ANSI_COLOR_RESET);

	if(!app.api) {
		sendApiCode(response, API_BAD_REQUEST, F("api not initialized"));
		return;
	}

	StaticJsonDocument<256> doc;
	if(!parseJsonBody(request, response, doc, F("could not get HTTP body"))) {
		return;
	}

	String msg;
	if(app.api->dispatchCommand(F("setOff"), doc.as<JsonObject>(), msg, true)) {
		sendApiCode(response, API_SUCCESS, F("SetOff OK"));
	} else {
		sendApiCode(response, API_BAD_REQUEST, msg);
	}
}

void ApplicationWebserver::setCorsHeaders(HttpResponse& response)
{
	response.setAllowCrossDomainOrigin("*");
	response.setHeader(F("Access-Control-Allow-Headers"), F("Content-Type, Cache-Control"));
}
