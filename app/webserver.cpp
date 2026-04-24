#include <ArduinoJson.h>

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
#include <Data/WebHelpers/base64.h>
#include <memory>

#include <Network/Http/Websocket/WebsocketResource.h>
#include <Storage.h>
#include <config.h>
#include <apihandler.h>
#include <jsonrpcmessage.h>
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
extern "C" {
#include <lwip/tcp.h>
}
#endif

#include <fileMap.h>

//#define NOCACHE


ApplicationWebserver::ApplicationWebserver()
{
	_running = false;
	// keep some heap space free
	// value is a good guess and tested to not crash when issuing multiple parallel requests
	HttpServerSettings settings;
	settings.maxActiveConnections = HTTP_MAX_CONNECTIONS;
	settings.minHeapSize = MINIMUM_HEAP_ACCEPT;
	settings.keepAliveSeconds = HTTP_KEEP_ALIVE_SECONDS; // do not close instantly when no transmission occurs. some clients are a bit slow (like FHEM)
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
		this->wsMessageReceived(socket, message);
	});
	wsResource->setDisconnectionHandler([this](WebsocketConnection& socket) { this->wsDisconnected(socket); });
	paths.set("/ws", wsResource);

	_init = true;
}

void ApplicationWebserver::wsConnected(WebsocketConnection& socket)
{
	debug_i("===>wsConnected");
	webSockets.addElement(&socket);
	debug_i("===>nr of websockets: %i", webSockets.size());
}

void ApplicationWebserver::wsDisconnected(WebsocketConnection& socket)
{
	debug_i("<===wsDisconnected");
	webSockets.removeElement(&socket);
	debug_i("===>nr of websockets: %i", webSockets.size());
}

void ApplicationWebserver::wsMessageReceived(WebsocketConnection& socket, const String& message)
{
	if(!app.api) {
		socket.sendString(F("{\"error\":\"api not initialized\"}"));
		return;
	}
	debug_i("Websocket message received: %s", message.c_str());

	JsonRpcMessageIn rpc(message);
	JsonObject requestRoot = rpc.getRoot();
	JsonVariant requestId = requestRoot[F("id")];
	const bool hasRequestId = !requestId.isNull();
	String method = rpc.getMethod();

	auto sendWsError = [&](const String& errorText) {
		if(!hasRequestId) {
			socket.sendString(String(F("{\"error\":\"")) + errorText + F("\"}"));
			return;
		}

		DynamicJsonDocument responseDoc(512);
		JsonObject responseRoot = responseDoc.to<JsonObject>();
		responseRoot[F("jsonrpc")] = F("2.0");
		responseRoot[F("id")] = requestId;
		responseRoot[F("error")] = errorText;
		String serialized;
		serializeJson(responseDoc, serialized);
		socket.sendString(serialized);
	};

	if(!rpc.isValid()) {
		debug_w("ws request rejected: malformed json (%s)", rpc.getError().c_str());
		sendWsError(F("malformed json"));
		return;
	}

	if(!method.length()) {
		debug_w("ws request rejected: missing method");
		sendWsError(F("missing method"));
		return;
	}

	if(method == F("keep_alive")) {
		return;
	}

	JsonObject params = rpc.getParams();

	if(method == F("info") || method == F("getInfo") || method == F("color") ||
	   method == F("getColor") || method == F("networks") || method == F("getNetworks")) {
		DynamicJsonDocument resultDoc(4096);
		JsonObject result = resultDoc.to<JsonObject>();
		if(!app.api->dispatch(method, params, result)) {
			debug_w("ws getter rejected (%s)", method.c_str());
			sendWsError(F("unsupported api method"));
			return;
		}

		DynamicJsonDocument responseDoc(4608);
		JsonObject responseRoot = responseDoc.to<JsonObject>();
		responseRoot[F("jsonrpc")] = F("2.0");
		if(hasRequestId) {
			responseRoot[F("id")] = requestId;
		}
		responseRoot[F("result")] = result;
		String serialized;
		serializeJson(responseDoc, serialized);
		socket.sendString(serialized);
		return;
	}

	if(method == F("hosts") || method == F("getHosts") || method == F("config") || method == F("getConfig")) {
		std::unique_ptr<IDataSourceStream> stream;
		String errorMsg;
		if(!app.api->dispatchStream(method, params, stream, errorMsg) || !stream) {
			debug_w("ws stream request rejected (%s): %s", method.c_str(), errorMsg.c_str());
			sendWsError(errorMsg.length() ? errorMsg : String(F("could not create stream response")));
			return;
		}

		String hostsPayload;
		while(!stream->isFinished()) {
			hostsPayload += stream->readString(512);
		}

		DynamicJsonDocument responseDoc(4096 + hostsPayload.length());
		JsonObject responseRoot = responseDoc.to<JsonObject>();
		responseRoot[F("jsonrpc")] = F("2.0");
		if(hasRequestId) {
			responseRoot[F("id")] = requestId;
		}
		responseRoot[F("result")] = serialized(hostsPayload);
		String serializedResponse;
		serializeJson(responseDoc, serializedResponse);
		socket.sendString(serializedResponse);
		return;
	}

	String errorMsg;
	if(!app.api->dispatchCommand(method, params, errorMsg, false)) {
		debug_w("ws command rejected: %s", errorMsg.c_str());
		sendWsError(errorMsg.length() ? errorMsg : String(F("command failed")));
		return;
	}

	if(hasRequestId) {
		DynamicJsonDocument responseDoc(256);
		JsonObject responseRoot = responseDoc.to<JsonObject>();
		responseRoot[F("jsonrpc")] = F("2.0");
		responseRoot[F("id")] = requestId;
		JsonObject result = responseRoot.createNestedObject(F("result"));
		result[F("success")] = true;
		String serialized;
		serializeJson(responseDoc, serialized);
		socket.sendString(serialized);
	}
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
		debug_i("ApplicationWebserver::authenticated - checking general context");
		AppConfig::Root config(*app.cfg);
		if(!config.security.getApiSecured())
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
		debug_i("ApplicationWebserver::authenticated - getting password");
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

bool ApplicationWebserver::ensureApiInitialized(HttpResponse& response)
{
	if(app.api) {
		return true;
	}

	setCorsHeaders(response);
	response.code = HTTP_STATUS_SERVICE_UNAVAILABLE;
	response.setContentType(MIME_TEXT);
	response.sendString(F("api not initialized"));
	return false;
}

bool ApplicationWebserver::parseJsonBody(const String& body, HttpResponse& response, JsonDocument& doc, bool allowEmptyBody)
{
	if(body.length() == 0) {
		if(allowEmptyBody) {
			doc.to<JsonObject>();
			return true;
		}
		sendApiCode(response, API_BAD_REQUEST, F("no body"));
		return false;
	}

	if(deserializeJson(doc, body)) {
		sendApiCode(response, API_BAD_REQUEST, F("Invalid JSON"));
		return false;
	}

	return true;
}

bool ApplicationWebserver::dispatchApiCommand(HttpResponse& response, const String& method, const JsonObject& params,
									  const String& fallbackError, bool relay)
{
	String errorMsg;
	if(app.api->dispatchCommand(method, params, errorMsg, relay)) {
		return true;
	}

	sendApiCode(response, API_CODES::API_BAD_REQUEST, errorMsg.length() ? errorMsg : fallbackError);
	return false;
}

bool ApplicationWebserver::handleApiCommandPost(HttpRequest& request, HttpResponse& response, const String& method,
									const String& fallbackError)
{
	if(!ensureApiInitialized(response)) {
		return false;
	}

	StaticJsonDocument<512> doc;
	if(!parseJsonBody(request.getBody(), response, doc, true)) {
		return false;
	}

	if(!dispatchApiCommand(response, method, doc.as<JsonObject>(), fallbackError, true)) {
		return false;
	}

	sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	return true;
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
	if(msg == nullptr) {
		sendApiCode(response, code, getApiCodeMsg(code));
		return;
	}
	sendApiCode(response, code, String(msg).c_str());
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
	application[F("webapp_version")] = WEBAPP_VERSION;
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
}

void ApplicationWebserver::sendApiCode(HttpResponse& response, API_CODES code, const char* msg)
{
	if(msg == nullptr) {
		msg = getApiCodeMsg(code);
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
			debug_i("API update in progress, adding info to response");
			JsonObject data = json.createNestedObject(F("info"));
			addInfoFields(data);
		}

		json[F("error")] = msg;
		sendApiResponse(response, stream.release(), HTTP_STATUS_BAD_REQUEST);
	}
}

void ApplicationWebserver::onFile(HttpRequest& request, HttpResponse& response)
{
	debug_i("http onFile");
	if(!preflightRequest(request, response, {HttpMethod::GET, HttpMethod::HEAD})) return;

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		response.setContentType(MIME_TEXT);
		response.code = HTTP_STATUS_SERVICE_UNAVAILABLE;
		response.sendString("OTA in progress");
		return;
	}
#endif
// Use client caching for better performance.
#ifndef NOCACHE
	//response.setCache(86400, true);
	response.setHeader(F("Cache-Control"),F("public, max-age=604800, immutable"));
#endif

	String fileName = request.uri.Path;
	debug_i("ApplicationWebserver::onFile with uri path=%s",fileName.c_str());
	if(fileName[0] == '/')
		fileName = fileName.substring(1);
	if(fileName[0] == '.') {
		response.code = HTTP_STATUS_FORBIDDEN;
		return;
	}

	String compressed = fileName + ".gz";
	debug_i("searching file name %s", compressed.c_str());
	auto v = fileMap[compressed];
	if(v) {
		debug_i("found");
		response.headers[HTTP_HEADER_CONTENT_ENCODING] = _F("gzip");
	} else {
		debug_i("searching file name %s", fileName.c_str());
		v = fileMap[fileName];
		if(!v) {
			debug_i("file %s not found in filemap", fileName.c_str());
			if(!app.isFilesystemMounted()) {
				response.setContentType(MIME_TEXT);
				response.code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
				response.sendString(F("No filesystem mounted"));
				return;
			}
			if(!fileExist(fileName) && !fileExist(fileName + ".gz") && WifiAccessPoint.isEnabled()) {
				//if accesspoint is active and we couldn`t find the file - redirect to index
				debug_d("ApplicationWebserver::onFile redirecting");
				response.headers[HTTP_HEADER_LOCATION] = F("http://") + WifiAccessPoint.getIP().toString() + "/";
			} else {
#ifndef NOCACHE
				//response.setCache(604800, true); // It's important to use cache for better performance.
				response.setHeader(F("Cache-Control"),F("public, max-age=604800, immutable"));
#endif
				response.code = HTTP_STATUS_OK;
				response.sendFile(fileName);
			}
			return;
		}
	}

	debug_i("found %s in fileMap", String(v.key()).c_str());
	auto stream = std::make_unique<FSTR::Stream>(v.content());
	response.sendDataStream(stream.release(), ContentType::fromFullFileName(fileName));

}
void ApplicationWebserver::onWebapp(HttpRequest& request, HttpResponse& response)
{
	debug_i("http onWebapp");
	if(!preflightRequest(request, response, {HttpMethod::GET})) return;

	response.headers[HTTP_HEADER_LOCATION] = F("/index.html");
	setCorsHeaders(response);

	response.code = HTTP_STATUS_PERMANENT_REDIRECT;
	response.sendString(F("Redirecting to /index.html"));
}

void ApplicationWebserver::onIndex(HttpRequest& request, HttpResponse& response)
{
	debug_i("http onIndex");
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

	response.headers[HTTP_HEADER_LOCATION] = F("/index.html");
	setCorsHeaders(response);

	response.code = HTTP_STATUS_PERMANENT_REDIRECT;
	response.sendString(F("Redirecting to /index.html"));
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
		response.setHeader(F("Retry-After"), "4");
		debug_e("Not enough heap free, rejecting request. Free heap: %u", app.getFreeHeapSize());
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
    debug_i("preflightRequest: %s %s", String((int)request.method).c_str(), request.uri.Path.c_str());
    response.setHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
    response.setHeader(F("Pragma"), F("no-cache"));
    response.setHeader(F("Expires"), F("0"));

    // 1. Heap Check
    if(!checkHeap(response, minHeap)) {
        return false;
    }

   // 2. CORS Preflight (OPTIONS) - Must handle this before method check or Auth
    if(request.method == HttpMethod::OPTIONS) {
        setCorsHeaders(response);
        sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
        debug_i("Handled OPTIONS preflight (generic)");
        return false; // Handled, stop processing
    }

    // 3. Method validation
    bool methodAllowed = false;

    String allowedMethodsStr;
    bool first = true;
    for(auto m : allowedMethods) {
        if (!first) {
             allowedMethodsStr += ", ";
        }
        allowedMethodsStr += String((int)m);
        first = false;
    }
    
    for(auto m : allowedMethods) {
        if(request.method == m) {
            methodAllowed = true;
            break;
        }
    }

    if(!methodAllowed) {
        setCorsHeaders(response);
        String msg = F("Method not allowed. Allowed: ");
        msg += allowedMethodsStr;
        msg += F(". Current: ");
        msg += String((int)request.method);
        sendApiCode(response, API_CODES::API_BAD_REQUEST, msg.c_str());
        return false;
    }

    // 4. Global Authentication
    // Responds with 401 if security is enabled and auth fails
    if(!authenticated(request, response)) {
        return false;
    }

    // 5. Ensure CORS headers for actual response
    setCorsHeaders(response);
    return true;
}

void ApplicationWebserver::onConfig(HttpRequest& request, HttpResponse& response)
{
	debug_i("onConfig");
	if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 12000)) return;

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif



	if(request.method == HttpMethod::POST) {
		debug_i("======================\nHTTP POST request received, ");
		app.telemetryClient.log(F("onConfig POST"));

		/* ConfigDB importFomStream */
		String oldIP, oldSSID, oldDeviceName, oldCurrentPinConfigName, oldSyslogHost;
		bool mqttEnabled, dhcpEnabled,oldSyslogEnabled,oldTelemetryEnabled;
		int oldColorMode,oldSyslogPort;
		{
			debug_i("ApplicationWebserver::onConfig storing old settings");
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
				debug_i("ApplicationWebserver::onConfig getting new settings");
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
				debug_i("ApplicationWebserver::onConfig ip settings changed - rebooting");
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
					debug_i("ApplicationWebserver::onConfig wifiap settings changed - rebooting");
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
						debug_i("ApplicationWebserver::onConfig mqtt settings changed - starting mqtt");
						app.telemetryClient.log(F("onConfig mqtt settings changed - starting mqtt"));
						app.mqttclient.start();
					}
				} else {
					if(app.mqttclient.isRunning()) {
						debug_i("ApplicationWebserver::onConfig mqtt settings changed - stopping mqtt");
						app.telemetryClient.log(F("onConfig mqtt settings changed - stopping mqtt"));
						app.mqttclient.stop();
					} else {
						debug_i("mqttclient was not running, no need to stop");
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
					debug_i("ApplicationWebserver::onConfig telemetry settings changed - starting telemetry");
					app.telemetryClient.start();
				}else{
					debug_i("ApplicationWebserver::onConfig telemetry settings changed - stopping telemetry");
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
					debug_i("ApplicationWebserver::onConfig ip settings changed - rebooting");
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

			debug_i("ApplicationWebserver::onConfig %i, %i",newColorMode,oldColorMode);
			if (newColorMode!=oldColorMode){
				// color Mode has been updated, requires reconfiguration, will restart for now
				debug_i("ApplicationWebserver::onConfig color settings changed - restarting");
				app.telemetryClient.log(F("onConfig color settings changed - restarting"));
				String msg=F("Color Mode changed");
				app.wsBroadcast(F("notification"), msg);
				app.telemetryClient.log(msg);
				app.delayedCMD(F("restart"), 1000); // wait 1s to first send response
			}

			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		} else {
			//CofigDB provide correct error message

			//debug_i("config api error %s",error_msg.c_str());
			//JsonObject root = doc.as<JsonObject>();
			//sendApiCode(response, API_CODES::API_MISSING_PARAM, error_msg);
			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_MISSING_PARAM);
		}

	} else {
		/*
         * /config GET
         */
		app.telemetryClient.log(F("onConfig GET"));

		if(!ensureApiInitialized(response)) {
			return;
		}

		StaticJsonDocument<16> paramsDoc;
		JsonObject params = paramsDoc.to<JsonObject>();
		std::unique_ptr<IDataSourceStream> configStream;
		String errorMsg;
		if(!app.api->dispatchStream(F("config"), params, configStream, errorMsg)) {
			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_BAD_REQUEST,
						errorMsg.length() ? errorMsg : F("could not create config response"));
			return;
		}

		setCorsHeaders(response);
		response.sendDataStream(configStream.release(), MIME_JSON);
	}
}

void ApplicationWebserver::onInfo(HttpRequest& request, HttpResponse& response){
	debug_i("onInfo");
	if(!preflightRequest(request, response, { HttpMethod::GET },app.ota.isProccessing() ? 4000 : 0)) return;

	auto stream = std::make_unique<JsonObjectStream>(2048);
	JsonObject data = stream->getRoot();

	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<64> paramsDoc;
	JsonObject params = paramsDoc.to<JsonObject>();
	params[F("V")] = request.getQueryParameter(F("V"));
	params[F("v")] = request.getQueryParameter(F("v"));

	if(!app.api->dispatch(F("info"), params, data)) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("unsupported api method"));
		return;
	}

		sendApiResponse(response, stream.release());

}

void ApplicationWebserver::onColorGet(HttpRequest& request, HttpResponse& response)
{
	debug_i("onColorGet");
    /*
	if(!checkHeap(response,2000))
		return;
    */

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<16> paramsDoc;
	JsonObject params = paramsDoc.to<JsonObject>();
	if(!app.api->dispatch(F("color"), params, json)) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("unsupported api method"));
		return;
	}

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
    /*
	if(!checkHeap(response)) {
		return;
	}
    */
	String body = request.getBody();

    /*
	setCorsHeaders(response);
	response.setHeader(F("Access-Control-Allow-Methods"), F("GET, PUT, POST, OPTIONS"));
	response.setHeader(F("Access-Control-Allow-Credentials"), F("true"));
    */

	debug_i("received color update with body legth %i and content %s", body.length(),body.c_str());
	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<1024> doc;
	if(!parseJsonBody(body, response, doc, false)) {
		return;
	}

	if(!dispatchApiCommand(response, F("color"), doc.as<JsonObject>(), F("color command failed"), true)) {
		return;
	}

	sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
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
	debug_i("received /color request");
    /*
	setCorsHeaders(response);
	response.setHeader(F("Access-Control-Allow-Origin"), F("*"));

	if(request.method == HttpMethod::OPTIONS) {
		response.setHeader(F("Access-Control-Allow-Methods"), F("GET, PUT, POST, OPTIONS"));

		debug_i("OPTIONS");
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		return;
	}
    */

	bool error = false;
	if(request.method == HttpMethod::POST) {
		debug_i("POST");
		ApplicationWebserver::onColorPost(request, response);
	} else if(request.method == HttpMethod::GET) {
		debug_i("GET");
		ApplicationWebserver::onColorGet(request, response);
	} else {
		debug_i("found unimplementd http_method %i", (int)request.method);
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
	debug_i("onNetworks");
	if(!preflightRequest(request, response, {HttpMethod::GET})) return;
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif
    /*
	if(request.method == HttpMethod::OPTIONS) {
		setCorsHeaders(response);

		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		return;
	}
	if(request.method != HttpMethod::GET) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP GET"));
		return;
	}
    */

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<16> paramsDoc;
	JsonObject params = paramsDoc.to<JsonObject>();
	if(!app.api->dispatch(F("networks"), params, json)) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("unsupported api method"));
		return;
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
	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<16> paramsDoc;
	JsonObject params = paramsDoc.to<JsonObject>();
	if(!dispatchApiCommand(response, F("scan_networks"), params, F("scan networks failed"), false)) {
		return;
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
	debug_i("onConnect");
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET})) return;

    /*
	if(!checkHeap(response)) {
		return;
	}
	debug_i("onConnect request.method: %i", request.method);
	if(request.method == HttpMethod::OPTIONS) {
		setCorsHeaders(response);
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		return;
	}
	
	if(!authenticated(request, response)) {
		return;
	}
    */

	debug_i("passed checks");
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

    /*
	if(request.method != HttpMethod::POST && request.method != HttpMethod::GET) {
		debug_i("not HTTP POST or GET");
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("not HTTP POST or GET"));
		return;
	}
    */

	if(request.method == HttpMethod::POST) {
		String body = request.getBody();
		debug_i("is POST");
		if(body == NULL) {
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("could not get HTTP body"));
			return;
		}
		// ConfigDB - CONFIG_MAX_LENGTH was no longer defined, what's the right size here?
		StaticJsonDocument<512> doc;
		Json::deserialize(doc, body);
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
			debug_i("wifi connected, checking if dhcp enabled");
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
	if(!handleApiCommandPost(request, response, F("system"), F("system command failed"))) {
		return;
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
		debug_i("/update HttpMethod::OPTIONS Request, sent API_SUCCSSS");
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

		String body = request.getBody();
		if(body == NULL) {
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("could not parse HTTP body"));
			return;
		}

		debug_i("body: %s", body.c_str());
		// ConfigDB - CONFIG_MAX_LENGTH was no longer defined, what's the right size here?
		StaticJsonDocument<512> doc;
		Json::deserialize(doc, body);
		String romurl;
		Json::getValue(doc[F("rom")][F("url")], romurl);

		//String spiffsurl;
		//Json::getValue(doc[F("spiffs")][F("url")],spiffsurl);

		debug_i("starting update process with \n    romurl: %s", romurl.c_str());
		if(romurl == "") {
			debug_i("missing rom url");
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

	handleApiCommandPost(request, response, F("stop"), F("stop failed"));
}

void ApplicationWebserver::onSkip(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
	handleApiCommandPost(request, response, F("skip"), F("skip failed"));
}

void ApplicationWebserver::onPause(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
	handleApiCommandPost(request, response, F("pause"), F("pause failed"));
}

void ApplicationWebserver::onContinue(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
	handleApiCommandPost(request, response, F("continue"), F("continue failed"));
}

void ApplicationWebserver::onBlink(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
	handleApiCommandPost(request, response, F("blink"), F("blink failed"));
}

void ApplicationWebserver::onToggle(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;
	handleApiCommandPost(request, response, F("toggle"), F("toggle failed"));
}


void ApplicationWebserver::onHosts(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::GET})) return;

	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<96> paramsDoc;
	JsonObject params = paramsDoc.to<JsonObject>();
	params[F("all")] = request.getQueryParameter(F("all"));
	params[F("debug")] = request.getQueryParameter(F("debug"));

	std::unique_ptr<IDataSourceStream> stream;
	String errorMsg;
	if(!app.api->dispatchStream(F("hosts"), params, stream, errorMsg)) {
		setCorsHeaders(response);
		sendApiCode(response, API_CODES::API_BAD_REQUEST, errorMsg.length() ? errorMsg : F("could not create hosts response"));
		return;
	}

    setCorsHeaders(response);
    response.setContentType(MIME_JSON);

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
			debug_i("received Data bodyStream");
			ConfigDB::Status status = app.data->importFromStream(ConfigDB::Json::format, *bodyStream);
			if(status){
				debug_i("successfully updated app-data");
				sendApiCode(response, API_CODES::API_SUCCESS, status.toString().c_str());
			}else{
				debug_i("could not update app-data");
				sendApiCode(response, API_CODES::API_BAD_REQUEST, status.toString().c_str());
			}
		}else{
			debug_i("could not get bodyStream");
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("could not get bodyStream"));
		}
	}

	return;
}

void ApplicationWebserver::onSetOn(HttpRequest &request, HttpResponse &response) {
    if(!preflightRequest(request, response, {HttpMethod::POST}, 4000)) return;
    

	debug_i("onSetOn");
		
	String body = request.getBody();
	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<512> doc;
	if(!parseJsonBody(body, response, doc, false)) {
		return;
	}
	if(dispatchApiCommand(response, F("setOn"), doc.as<JsonObject>(), F("setOn failed"), true)) {
		sendApiCode(response, API_SUCCESS, F("SetOn OK"));
	} else {
		return;
	}
}

void ApplicationWebserver::onSetOff(HttpRequest &request, HttpResponse &response) {
    if(!preflightRequest(request, response, {HttpMethod::POST}, 4000)) return;
    
	debug_i("onSetOff");

	String body = request.getBody();
	if(!ensureApiInitialized(response)) {
		return;
	}

	StaticJsonDocument<512> doc;
	if(!parseJsonBody(body, response, doc, false)) {
		return;
	}
	if(dispatchApiCommand(response, F("setOff"), doc.as<JsonObject>(), F("setOff failed"), true)) {
		sendApiCode(response, API_SUCCESS, F("SetOff OK"));
	} else {
		return;
	}
}

void ApplicationWebserver::setCorsHeaders(HttpResponse& response)
{
	response.setAllowCrossDomainOrigin("*");
	response.setHeader(F("Access-Control-Allow-Headers"), F("Content-Type, Cache-Control"));
}
