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
#include <Crypto/Sha2.h>
#include <cstring>
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
constexpr size_t INFO_DOC_CAPACITY_V1 = 512;
constexpr size_t INFO_DOC_CAPACITY_V2 = 1224;
constexpr size_t WS_INFO_RESPONSE_OVERHEAD = 300;
constexpr size_t WS_INFO_RESPONSE_CAPACITY = INFO_DOC_CAPACITY_V1 + WS_INFO_RESPONSE_OVERHEAD;

// Per-connection WebSocket authentication state, attached via setUserData().
// Allocated in wsConnected(), released in wsDisconnected(). The challenge is a
// one-shot nonce handed to the client in an "authentication required" reply and
// consumed by the next "authenticate" message.
struct WsAuthState {
	bool authenticated = false;
	String challenge;
};

// Generate a 16-byte random nonce as a 32-char lowercase hex string.
String wsMakeChallenge()
{
	static const char hex[] = "0123456789abcdef";
	char buf[33];
	for(int i = 0; i < 16; i++) {
		uint8_t b = static_cast<uint8_t>(os_random() & 0xFF);
		buf[i * 2] = hex[b >> 4];
		buf[i * 2 + 1] = hex[b & 0x0F];
	}
	buf[32] = '\0';
	return String(buf);
}

// Shared-secret challenge-response digest used by the WebSocket auth handshake.
// Canonical input is "<challenge>:<password>" (SHA-256, lowercase hex).
// The client must compute the identical string to authenticate.
String wsComputeAuthHash(const String& challenge, const String& password)
{
	String canonical;
	canonical.reserve(challenge.length() + password.length() + 1);
	canonical += challenge;
	canonical += ':';
	canonical += password;
	Crypto::Sha256 ctx;
	ctx.update(canonical.c_str(), canonical.length());
	return Crypto::toString(ctx.getHash());
}
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
	settings.keepAliveSeconds = 5; // do not close instantly when no transmission occurs. some clients are a bit slow (like FHEM)
#ifdef ARCH_ESP8266
	// Stability workaround: reduce overlap pressure without starving browser traffic.
	// Keep enough concurrent HTTP slots for page/API usage, but disable keepalive
	// reuse on ESP8266 so sockets close immediately after each response.
	settings.maxActiveConnections = WEBSERVER_MAX_CONN_DEFAULT;
	settings.keepAliveSeconds = 0;
#endif
	// Retain a copy so setMaxActiveConnections() can re-configure the limit at
	// runtime without losing the heap/keep-alive settings established here.
	_serverSettings = settings;
	configure(settings);

	// Only JSON POST endpoints need the request body buffered into a String.
	// The old wildcard parser caused heap pressure for every POST request.
	setBodyParser(MIME_JSON, bodyToStringParser);
}

void ApplicationWebserver::setMaxActiveConnections(uint16_t n)
{
	if(_serverSettings.maxActiveConnections == n) {
		return;
	}
	debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::setMaxActiveConnections " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " -> " ANSI_COLOR_CYAN "%u" ANSI_COLOR_RESET,
			_serverSettings.maxActiveConnections, n);
	_serverSettings.maxActiveConnections = n;
	// configure() reassigns the live limit read by TcpServer::onAccept; it only
	// adds body parsers, so the JSON body parser set in the constructor is kept.
	configure(_serverSettings);
}

void ApplicationWebserver::applyOtaLoadShedding(bool otaActive)
{
#ifdef ARCH_ESP8266
	// During the OTA download the outbound HTTP client + LittleFS writes hold most
	// of the free heap. Clamp inbound connections to a single slot so a burst of
	// browser polls (/info, /webapp_status) cannot allocate the device into OOM;
	// restore the normal limit once the download completes. setMaxActiveConnections
	// no-ops when the value is unchanged, so repeated calls are cheap.
	setMaxActiveConnections(otaActive ? WEBAPP_OTA_MAX_CONN : WEBSERVER_MAX_CONN_DEFAULT);
#else
	(void)otaActive;
#endif
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
	paths.set(F("/canonical.html"), HttpPathDelegate(&ApplicationWebserver::onRedirector, this)); 
	paths.set(F("/generate_204"), HttpPathDelegate(&ApplicationWebserver::onRedirector, this)); //android
	paths.set(F("/static/hotspot.txt"), HttpPathDelegate(&ApplicationWebserver::onRedirector, this));
	paths.set(F("/connecttest.txt"), HttpPathDelegate(&ApplicationWebserver::onRedirector, this)); //Windows
	paths.set(F("/hotspot-detect.html"), HttpPathDelegate(&ApplicationWebserver::onRedirector, this)); //iOS/macOS
	paths.set(F("/nmcheck.gnome.org"), HttpPathDelegate(&ApplicationWebserver::onRedirector, this)); //Linux (NetworkManager)

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
	// Attach per-connection auth state. A fresh connection starts unauthenticated;
	// it must complete the challenge-response handshake before mutating commands
	// are accepted when the API is secured.
	socket.setUserData(new WsAuthState());
	webSockets.addElement(&socket);
	debug_i(ANSI_COLOR_BLUE "===>nr of websockets: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, webSockets.size());

	// If a webapp OTA is in progress, push the current state immediately so
	// the updating page doesn't have to wait for the next timed broadcast.
	if(app.webappOta.isActive()) {
		DynamicJsonDocument doc(256);
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
	// Release the per-connection auth state allocated in wsConnected().
	delete static_cast<WsAuthState*>(socket.getUserData());
	socket.setUserData(nullptr);
	webSockets.removeElement(&socket);
	debug_i(ANSI_COLOR_BLUE "===>nr of websockets: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, webSockets.size());
}

void ApplicationWebserver::wsMessage(WebsocketConnection& socket, const String& message)
{
    debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::wsMessage: " ANSI_COLOR_GREEN " %s" ANSI_COLOR_RESET, message.c_str());

    // Size the parse buffer from the incoming message so large payloads don't
    // overflow a fixed capacity. Heap-allocated to keep it off the small stack.
    const size_t requestCapacity = std::max<size_t>(1024, message.length() * 2);
    DynamicJsonDocument requestDoc(requestCapacity);
	String errorMsg;
	int errorCode = 0;

    if(!Json::deserialize(requestDoc, message)) {
		socket.sendString(F("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"},\"id\":null}"));
        return;
    }

    JsonObject requestRoot = requestDoc.as<JsonObject>();
	const char* method = requestRoot[F("method")] | "";
    JsonVariant requestId = requestRoot[F("id")];

	debug_i(ANSI_COLOR_BLUE "Websocket message: method= " ANSI_COLOR_GREEN "%s" ANSI_COLOR_RESET, method);

	// Determine target stream capacity based on the specific method requested
	size_t responseCapacity = 512; // Default for simple getters/commands
	const bool isInfoMethod = (strcmp_P(method, PSTR("info")) == 0) || (strcmp_P(method, PSTR("getInfo")) == 0);
	if(isInfoMethod) {
		responseCapacity = WS_INFO_RESPONSE_CAPACITY;
	}
	const uint32_t infoHeapSnapshot = app.getFreeHeapSize();

	auto responseStream = std::make_unique<JsonObjectStream>(responseCapacity);
	if(!responseStream) {
		socket.sendString(F("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"internal error: low memory\"},\"id\":null}"));
		return;
	}
	JsonObject responseRoot = responseStream->getRoot();
	responseRoot[F("jsonrpc")] = F("2.0");

	if(!requestId.isNull()) {
		responseRoot[F("id")] = requestId;
	}

	// ---- WebSocket authentication gate ----------------------------------
	// When the API is secured, a connection must complete the challenge-
	// response handshake before any state-changing method is accepted. The
	// "authenticate" method and "keep_alive" pings are always allowed through.
	ensureSecurityCache();
	const bool wsSecured = (_apiSecuredCache == 1);
	WsAuthState* wsAuth = static_cast<WsAuthState*>(socket.getUserData());
	const bool isAuthMethod = strcmp_P(method, PSTR("authenticate")) == 0;
	const bool isKeepAlive = strcmp_P(method, PSTR("keep_alive")) == 0;

	if(isAuthMethod) {
		JsonObject params = requestRoot[F("params")];
		const String clientHash = params[F("hash")] | "";
		if(!wsSecured) {
			// Nothing to prove when security is disabled.
			if(wsAuth) {
				wsAuth->authenticated = true;
			}
			JsonObject result = responseRoot.createNestedObject(F("result"));
			result[F("authenticated")] = true;
		} else if(wsAuth == nullptr) {
			errorCode = -32603;
			errorMsg = F("internal error: no auth state");
		} else if(wsAuth->challenge.length() == 0) {
			// No outstanding challenge — issue one and ask the client to retry.
			wsAuth->challenge = wsMakeChallenge();
			errorCode = -32001;
			errorMsg = F("authentication required");
			responseRoot[F("challenge")] = wsAuth->challenge;
		} else {
			const String expected = wsComputeAuthHash(wsAuth->challenge, _apiPasswordCache);
			if(expected.length() && clientHash.equalsIgnoreCase(expected)) {
				wsAuth->authenticated = true;
				wsAuth->challenge = ""; // consume the nonce
				JsonObject result = responseRoot.createNestedObject(F("result"));
				result[F("authenticated")] = true;
			} else {
				// Wrong hash — hand out a fresh challenge for the next attempt.
				wsAuth->challenge = wsMakeChallenge();
				errorCode = -32001;
				errorMsg = F("authentication failed");
				responseRoot[F("challenge")] = wsAuth->challenge;
			}
		}
	} else if(wsSecured && !isKeepAlive && (wsAuth == nullptr || !wsAuth->authenticated)) {
		// Unauthenticated request on a secured API: refuse and issue a challenge.
		const String challenge = wsMakeChallenge();
		if(wsAuth) {
			wsAuth->challenge = challenge;
		}
		errorCode = -32001;
		errorMsg = F("authentication required");
		responseRoot[F("challenge")] = challenge;
	} else if(method[0] == '\0') {
		errorCode = -32600;
		errorMsg = F("missing method");
	} else if(!app.api) {
		errorCode = -32603;
		errorMsg = F("api not initialized");
    } else {
        JsonObject params = requestRoot[F("params")];
		const bool isColorGetter = (strcmp_P(method, PSTR("color")) == 0) && (params.isNull() || params.size() == 0);
		const bool isDataMethod = isColorGetter || (strcmp_P(method, PSTR("getColor")) == 0) || isInfoMethod ||
						(strcmp_P(method, PSTR("networks")) == 0) || (strcmp_P(method, PSTR("getNetworks")) == 0);

		if(isDataMethod) {
			JsonObject result = responseRoot.createNestedObject(F("result"));
			if(isInfoMethod) {
				if(!app.api->handleInfo(params, result, infoHeapSnapshot)) {
					errorCode = -32601;
					const char* resultError = result[F("error")] | nullptr;
					errorMsg = resultError ? String(resultError) : String(F("method not implemented"));
					responseRoot.remove(F("result"));
				}
			} else {
				if(!app.api->dispatch(method, params, result)) {
					errorCode = -32601;
					const char* resultError = result[F("error")] | nullptr;
					errorMsg = resultError ? String(resultError) : String(F("method not implemented"));
					responseRoot.remove(F("result"));
				}
			}
		} else {
			if(app.api->dispatchCommand(method, params, errorMsg, false)) {
				JsonObject result = responseRoot.createNestedObject(F("result"));
				result[F("success")] = true;
			} else {
				const bool methodMissing = errorMsg.length() == 0 || errorMsg.indexOf(F("method not implemented")) >= 0;
				if(methodMissing) {
					errorCode = -32601;
					if(errorMsg.length() == 0) {
						errorMsg = String(F("method not implemented: ")) + method;
					}
				} else {
					errorCode = -32000;
				}
			}
		}
    }

	if(errorMsg.length()) {
		JsonObject errorObj = responseRoot.createNestedObject(F("error"));
		errorObj[F("code")] = errorCode;
		errorObj[F("message")] = errorMsg;
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
	// Report the authoritative live connection count (added on accept, removed on
	// destroy — this is what enforces maxConnections). The inherited activeClients
	// counter can drift upward because onClientComplete is not guaranteed to fire
	// for every accepted connection (aborted/reset sockets), which made the debug
	// figure look like an unbounded "connection leak".
	return getConnections().count();
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

void ApplicationWebserver::ensureSecurityCache()
{
	if(_apiSecuredCache < 0) {
		AppConfig::Root config(*app.cfg);
		_apiSecuredCache = config.security.getApiSecured() ? 1 : 0;
		_apiPasswordCache = config.security.getApiPassword();
	}
}

bool ICACHE_FLASH_ATTR ApplicationWebserver::authenticateExec(HttpRequest& request, HttpResponse& response)
{
	{
		debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::authenticated - checking general context" ANSI_COLOR_RESET);
		ensureSecurityCache();
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

	debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::authenticated - getting password" ANSI_COLOR_RESET);
	userPass = base64_decode(userPass);
	if(userPass.endsWith(_apiPasswordCache)) {
		return true;
	}
	return false;
}

bool ICACHE_FLASH_ATTR ApplicationWebserver::authenticated(HttpRequest& request, HttpResponse& response)
{
	bool authenticated = authenticateExec(request, response);

	if(!authenticated) {
		response.code = HTTP_STATUS_UNAUTHORIZED;
		response.setHeader(F("WWW-Authenticate"), F("Basic realm=\"RGBWW Server\""));
		response.setHeader(F("401 wrong credentials"), F("wrong credentials"));
		response.setHeader(F("Connection"), F("close"));
		// CORS headers must be present on the 401 too, otherwise a cross-origin
		// browser blocks the response and fetch() rejects before the client can
		// see the 401 and prompt for credentials.
		setCorsHeaders(response);
	}
	return authenticated;
}

String ApplicationWebserver::getApiCodeMsg(API_CODES code)
{
	switch(code) {
	case API_CODES::API_MISSING_PARAM:
		return F("missing param");
	case API_CODES::API_UNAUTHORIZED:
		return F("authorization required");
	case API_CODES::API_UPDATE_IN_PROGRESS:
		return F("update in progress");
	default:
		return F("bad request");
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
	if(!stream) {
		setCorsHeaders(response);
		response.setHeader(F("accept"), F("GET, POST, OPTIONS"));
		response.setHeader(F("Connection"), F("close"));
		response.code = (code == API_CODES::API_SUCCESS) ? HTTP_STATUS_OK : HTTP_STATUS_BAD_REQUEST;
		response.setContentType(MIME_TEXT);
		if(!response.sendString(String(F("Invalid JSON")))) {
			response.headers[HTTP_HEADER_CONTENT_LENGTH] = "0";
		}
		return;
	}
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
	if(!stream) {
		setCorsHeaders(response);
		response.setHeader(F("accept"), F("GET, POST, OPTIONS"));
		response.setHeader(F("Connection"), F("close"));
		response.code = (code == API_CODES::API_SUCCESS) ? HTTP_STATUS_OK : HTTP_STATUS_BAD_REQUEST;
		response.setContentType(MIME_TEXT);
		if(!response.sendString(String(F("Invalid JSON")))) {
			response.headers[HTTP_HEADER_CONTENT_LENGTH] = "0";
		}
		return;
	}
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

		// ArduinoJson stores a const char* by reference (no copy). Callers may pass a
		// pointer to a stack buffer (e.g. parseJsonBody's parseError[]) that is gone by
		// the time JsonObjectStream is serialized asynchronously, yielding a dangling
		// read and a truncated response. Wrap in String to force a copy into the pool.
		json[F("error")] = String(msg);
		sendApiResponse(response, stream.release(), HTTP_STATUS_BAD_REQUEST);
	}
}

bool ApplicationWebserver::parseJsonBody(HttpRequest& request, HttpResponse& response, JsonDocument& doc,
											 const String& noBodyMessage)
{
	debug_i(ANSI_COLOR_BLUE "parseJsonBody: begin" ANSI_COLOR_RESET);
	DeserializationError err = DeserializationError::EmptyInput;
	auto bodyStream = request.getBodyStream();
	if(bodyStream != nullptr) {
		debug_i(ANSI_COLOR_BLUE "parseJsonBody: deserializeJson(stream), freeHeap=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_RESET,
				app.getFreeHeapSize());
		err = deserializeJson(doc, *bodyStream);
		debug_i(ANSI_COLOR_BLUE "parseJsonBody: deserializeJson(stream) done" ANSI_COLOR_RESET);
	} else {
		debug_i(ANSI_COLOR_BLUE "parseJsonBody: bodyStream unavailable, fallback to getBody" ANSI_COLOR_RESET);
		String body = request.getBody();
		debug_i(ANSI_COLOR_BLUE "parseJsonBody: body length=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE ", freeHeap=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_RESET,
				(unsigned)body.length(), app.getFreeHeapSize());

		if(body.length()) {
			debug_i(ANSI_COLOR_BLUE "parseJsonBody: deserializeJson(buffer)" ANSI_COLOR_RESET);
			err = deserializeJson(doc, body.c_str(), body.length());
			debug_i(ANSI_COLOR_BLUE "parseJsonBody: deserializeJson(buffer) done" ANSI_COLOR_RESET);
		} else {
			const String& contentLength = request.headers[HTTP_HEADER_CONTENT_LENGTH];
			const String& contentType = request.headers[HTTP_HEADER_CONTENT_TYPE];
			if(contentLength.length() && contentLength.toInt() > 0) {
				if(contentType.indexOf(F("application/json")) != 0) {
					sendApiCode(response, API_CODES::API_BAD_REQUEST,
							F("Invalid JSON: send Content-Type: application/json"));
				} else {
					sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Invalid JSON: body unavailable"));
				}
			} else {
				sendApiCode(response, API_CODES::API_BAD_REQUEST,
						noBodyMessage.length() ? noBodyMessage.c_str() : (const char*)nullptr);
			}
			return false;
		}
	}

	if(err) {
		char parseError[96];
		snprintf(parseError, sizeof(parseError), "Invalid JSON: %s", err.c_str());
		sendApiCode(response, API_CODES::API_BAD_REQUEST, parseError);
		return false;
	}

	return true;
}

void ApplicationWebserver::onFile(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "http onFile" ANSI_COLOR_RESET);
	// LittleFS file serving buffers through lwIP — require more free heap than API calls.
	if(!preflightRequest(request, response,true, {HttpMethod::GET, HttpMethod::HEAD}, 12000)) return;

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
			// file not found in fileMap, check if it exists in filesystem
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
				if(fileName != F("index.html")) {
					// never cache the index.html page. it's small and does not have a cache busting hash.
					response.setHeader(F("Cache-Control"),F("public, max-age=604800, immutable"));
				}
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

void ApplicationWebserver::onRedirector(HttpRequest& request, HttpResponse& response)
{
	// OS/browser connectivity probes (generate_204, hotspot-detect, connecttest,
	// nmcheck, ...) hit these paths repeatedly. While the webapp is downloading
	// the heap is scarce, so shed them with 429 to make the client back off
	// instead of driving the device into OOM. Kept deliberately allocation-light:
	// no stream, no body.
	if(app.webappOta.isActive()) {
		debug_i(ANSI_COLOR_YELLOW "onRedirector: webapp download active, shedding probe %s (429)" ANSI_COLOR_RESET, request.uri.Path.c_str());
		response.code = HTTP_STATUS_TOO_MANY_REQUESTS;
		response.setHeader(F("Retry-After"), F("10"));
		response.setHeader(F("Connection"), F("close"));
		return;
	}
	onIndex(request, response);
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

	// Return current OTA status (same as /webapp_status but cache-busted).
	// Stream directly from a JsonObjectStream to avoid a second serialized
	// String buffer (peak-heap reduction on the low-heap ESP8266 path).
	auto stream = std::make_unique<JsonObjectStream>(512);
	if(!stream) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
		return;
	}
	JsonObject json = stream->getRoot();
	app.webappOta.fillStatusJson(json);

	setCorsHeaders(response);
	response.headers[HTTP_HEADER_CACHE_CONTROL] = F("no-store");
	response.sendDataStream(stream.release(), MIME_JSON);
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

bool ApplicationWebserver::checkHeap(HttpResponse& response, uint32_t minHeap)
{
	// A zero request means "use the default floor". This matches the
	// documented preflightRequest(minHeap=0) contract and ensures every handler
	// — including those relying on the default — is actually heap-gated.
	if(minHeap == 0) {
		minHeap = MINIMUM_HEAP;
	}
	// While the webapp is downloading, the OTA client and filesystem writes hold
	// a large chunk of heap. Serving additional JSON endpoints (e.g. repeated
	// /info polls from the browser) on top of that pushes the device into OOM —
	// we've observed onInfo drop the connection at ~6.8 KB free and the immediate
	// client retry then crash the allocator. Raise the floor during the download
	// so non-essential requests are shed with 429 (client backs off via
	// Retry-After) instead of being processed into an out-of-memory crash.
	if(app.webappOta.isActive() && minHeap < WEBAPP_OTA_MIN_HEAP) {
		minHeap = WEBAPP_OTA_MIN_HEAP;
	}

	return app.checkHeap(minHeap);
}

/**
 * @brief Helper to handle standard checks: Heap, CORS(Options), Method allow-list, Global Auth
 * 
 * @param minHeap Optional minimum heap required (default 0 loops back to _minimumHeap)
 * @return true if request is valid and should proceed. false if response handled (e.g. error or options)
 */
bool ApplicationWebserver::preflightRequest(HttpRequest& request, HttpResponse& response, bool canRedirect, std::initializer_list<HttpMethod> allowedMethods,  uint32_t minHeap)
{
	debug_i(ANSI_COLOR_BLUE "preflightRequest: %d %s" ANSI_COLOR_RESET, (int)request.method, request.uri.Path.c_str());
	const HttpMethod reqMethod = request.method;

	debug_i(ANSI_COLOR_BLUE "checking heap..." ANSI_COLOR_RESET);
    // 1. Heap Check
	if (!checkHeap(response, minHeap)) {
    	setCorsHeaders(response);
		/*
		if (canRedirect) {
			auto filename = request.uri.Path;
			
			int dotIndex = filename.lastIndexOf('.');
			bool isJavaScript = false;

			if (dotIndex != -1 && dotIndex < (int)filename.length() - 1) {
				// Point directly into the existing string buffer instead of allocating a new String object
				const char* extPtr = filename.c_str() + dotIndex + 1;
				
				// Handle case-insensitivity using strcasecmp_P to protect against .JS uppercase variants
				if (strcasecmp_P(extPtr, PSTR("js")) == 0) {
					isJavaScript = true;
				}
				
				debug_i(ANSI_COLOR_BLUE "Request for %s with extension %s failed heap check" ANSI_COLOR_RESET, filename.c_str(), extPtr);
			} else {
				debug_i(ANSI_COLOR_BLUE "Request for %s (no extension) failed heap check" ANSI_COLOR_RESET, filename.c_str());
			}

			// Only redirect if it is NOT a JavaScript asset
			if (!isJavaScript) {
				response.code = HTTP_STATUS_TEMPORARY_REDIRECT;
				
				// Building location header
				String Location = F("http://") + app.controllers->getNextCompatibleWebappController().toString() + request.uri.Path;
				response.headers[HTTP_HEADER_LOCATION] = Location;
				
				debug_i(ANSI_COLOR_RED "Not enough heap free, redirecting request to %s. Free heap: " ANSI_COLOR_CYAN "%u" ANSI_COLOR_RED " bytes" ANSI_COLOR_RESET, Location.c_str(), app.getFreeHeapSize());
				return false;
			} 
		} else {
		*/
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
		//}
	}

	debug_i(ANSI_COLOR_BLUE "heap check passed, checking OPTIONS..." ANSI_COLOR_RESET);
   // 2. CORS Preflight (OPTIONS) - Must handle this before method check or Auth
    if(reqMethod == HttpMethod::OPTIONS) {
        setCorsHeaders(response);
        sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
        debug_i(ANSI_COLOR_BLUE "Handled OPTIONS preflight (generic)" ANSI_COLOR_RESET);
        return false; // Handled, stop processing
    }
	debug_i(ANSI_COLOR_BLUE "OPTIONS check passed, checking method..." ANSI_COLOR_RESET);
    // 3. Method validation
    bool methodAllowed = false;

    debug_i(ANSI_COLOR_BLUE "Method check passed, checking authentication..." ANSI_COLOR_RESET);
    for(auto m : allowedMethods) {
		if(reqMethod == m) {
            methodAllowed = true;
            break;
        }
    }

    if(!methodAllowed) {
        setCorsHeaders(response);
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Method not allowed"));
        return false;
    }

    // 4. Global Authentication
    // Responds with 401 if security is enabled and auth fails
	debug_i(ANSI_COLOR_BLUE "Checking authentication..." ANSI_COLOR_RESET);
    if(!authenticated(request, response)) {
		debug_i(ANSI_COLOR_RED "preflightRequest: %d %s - Authentication failed" ANSI_COLOR_RESET, (int)request.method, request.uri.Path.c_str());
        return false;
    }

    // 5. Set cache-control headers and CORS headers only if all checks pass
    // This prevents committed response headers from interfering with error responses downstream
    response.setHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
    response.setHeader(F("Pragma"), F("no-cache"));
    response.setHeader(F("Expires"), F("0"));
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

			// Security settings may have changed (apiSecured/password), refresh lazily on next request.
			_apiSecuredCache = -1;
			_apiPasswordCache = String::nullstr;

			sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		} else {
			//CofigDB provide correct error message

			//debug_i(ANSI_COLOR_BLUE "config api error " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,error_msg.c_str());
			//JsonObject root = doc.as<JsonObject>();
			//sendApiCode(response, API_CODES::API_MISSING_PARAM, error_msg);
			sendApiCode(response, API_CODES::API_MISSING_PARAM);
		}

	} else {
		/*
         * /config GET
         */
		app.telemetryClient.log(F("onConfig GET"));

		auto configStream = app.cfg->createExportStream(ConfigDB::Json::format);
		response.sendDataStream(configStream.release(), MIME_JSON);
	}
}

void ApplicationWebserver::onInfo(HttpRequest& request, HttpResponse& response){
	debug_i(ANSI_COLOR_BLUE "ApplicationWebserver::onInfo" ANSI_COLOR_RESET);
	if(!preflightRequest(request, response, { HttpMethod::GET },app.ota.isProccessing() ? 10000 : 0)) return;

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
	String* cachePayload = isV2 ? &_infoV2Cache : &_infoV1Cache;
	unsigned long* cacheTime = isV2 ? &_infoV2CacheTime : &_infoV1CacheTime;
	const unsigned long nowMs = millis();

	// Frequent UI polling can trigger repeated ConfigDB store opens;
	// use a short cache window to lower pressure on FS/event queue.
	if(!app.ota.isProccessing() && cachePayload->length() > 0 && (nowMs - *cacheTime) < INFO_CACHE_MS) {
		setCorsHeaders(response);
		response.setHeader(F("accept"), F("GET, POST, OPTIONS"));
		response.code = HTTP_STATUS_OK;
		response.setContentType(MIME_JSON);
		response.sendString(*cachePayload);
		return;
	}

	auto stream = std::make_unique<JsonObjectStream>(isV2 ? INFO_DOC_CAPACITY_V2 : INFO_DOC_CAPACITY_V1);
	if(!stream) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
		return;
	}
	JsonObject data = stream->getRoot();
	
	// Call the shared handler
	app.api->handleInfo(params, data, infoHeapSnapshot);

	String payload;
	if(!serializeJson(data, payload)) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("serialize failed"));
		return;
	}

	if(!app.ota.isProccessing()) {
		*cachePayload = payload;
		*cacheTime = nowMs;
	}

	if(!checkHeap(response)) {
		return;
	}
	setCorsHeaders(response);
	response.setHeader(F("accept"), F("GET, POST, OPTIONS"));
	response.code = HTTP_STATUS_OK;
	response.setContentType(MIME_JSON);
	response.sendString(payload);
}


void ApplicationWebserver::onColorGet(HttpRequest& request, HttpResponse& response)
{
	debug_i(ANSI_COLOR_BLUE "onColorGet" ANSI_COLOR_RESET);

	StaticJsonDocument<256> doc;
	JsonObject json = doc.to<JsonObject>();

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

	String payload;
	payload.reserve(128);
	serializeJson(doc, payload);

	response.code = HTTP_STATUS_OK;
	response.setContentType(MIME_JSON);
	response.sendString(payload);

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
	debug_i(ANSI_COLOR_BLUE "onColorPost" ANSI_COLOR_RESET);
	_colorPostDoc.clear();
	if(!parseJsonBody(request, response, _colorPostDoc, F("no body"))) {
		return;
	}

	debug_i(ANSI_COLOR_BLUE "received color update" ANSI_COLOR_RESET);
	String msg;
	debug_i(ANSI_COLOR_BLUE "dispatching color update" ANSI_COLOR_RESET);
	const bool ok = app.api->dispatchCommand(F("color"), _colorPostDoc.as<JsonObject>(), msg, true);

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
	if(!stream) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
		return;
	}
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

	debug_i(ANSI_COLOR_BLUE "passed checks" ANSI_COLOR_RESET);
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

	if(request.method == HttpMethod::POST) {
		debug_i(ANSI_COLOR_BLUE "is POST" ANSI_COLOR_RESET);
		DynamicJsonDocument doc(256);
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
		if(!stream) {
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
			return;
		}
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

	StaticJsonDocument<128> doc;
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
	if(!stream) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
		return;
	}
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
	if(!stream) {
		response.code = HTTP_STATUS_BAD_REQUEST;
		response.sendString(F("{\"error\":\"low memory\"}"));
		return;
	}
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
	if(!stream) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
		return;
	}
    response.sendDataStream(stream.release(), MIME_JSON);

//todo 
}

void ApplicationWebserver::onData(HttpRequest& request, HttpResponse& response){
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 12000)) return;
    
	if(request.method==HttpMethod::GET){	
		setCorsHeaders(response);

		response.setContentType(F("application/json"));

		auto dataStream = app.data->createExportStream(ConfigDB::Json::format);
		if(!dataStream) {
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("low memory"));
			return;
		}
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
	response.setHeader(F("Access-Control-Allow-Headers"), F("Content-Type, Cache-Control, Authorization"));
}
