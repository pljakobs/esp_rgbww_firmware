#include <ArduinoJson.h>

/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
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
#include <fileMap.h>

//#define NOCACHE

// ToDo: think about implementing a parameterized API to read objects from appData by id

ApplicationWebserver::ApplicationWebserver()
{
	_running = false;
	// keep some heap space free
	// value is a good guess and tested to not crash when issuing multiple parallel requests
	HttpServerSettings settings;
	settings.maxActiveConnections = 5;
	settings.minHeapSize = _minimumHeapAccept;
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
	// Abort any in-progress config stream for this socket
	if(_wsOutSocket == &socket) {
		_wsStreamTimer.stop();
		_wsOutStream.reset();
		_wsOutSocket = nullptr;
	}
}

void ApplicationWebserver::wsMessageReceived(WebsocketConnection& socket, const String& message)
{
	debug_i("WS received: %s", message.c_str());

	// Intercept "config" method — ConfigDB export size is unknown upfront so we cannot
	// fit it in a single WS frame.  Use a two-phase streaming protocol instead:
	//   Phase 1: TEXT {"jsonrpc":"2.0","id":N,"result":{"type":"stream_start","content-type":"application/json"}}
	//   Phase 2: one or more BINARY frames containing raw config JSON bytes (512-byte chunks)
	//   Phase 3: TEXT {"jsonrpc":"2.0","method":"stream_end","params":{"id":N}}
	{
		StaticJsonDocument<200> doc;
		if (!deserializeJson(doc, message)) {
			String method = doc[F("method")] | F("");
			if ((method == F("config") || method == F("data")) && doc.containsKey(F("id"))) {
				int id = doc[F("id")];
				String startMsg = String(F("{\"jsonrpc\":\"2.0\",\"id\":")) + String(id) +
				                  F(",\"result\":{\"type\":\"stream_start\",\"content-type\":\"application/json\"}}");
				socket.sendString(startMsg);
				_wsOutSocket = &socket;
				_wsOutRequestId = id;
				if (method == F("data")) {
					_wsOutStream = app.data->createExportStream(ConfigDB::Json::format);
				} else {
					_wsOutStream = app.cfg->createExportStream(ConfigDB::Json::format);
				}
				_wsStreamTimer.initializeMs(10, TimerDelegate(&ApplicationWebserver::wsStreamNextChunk, this)).startOnce();
				return;
			}
		}
	}

	String response;

	// Process the JSON-RPC message using JsonProcessor
	app.jsonproc.onJsonRpc(message, response);

	// If a response was generated, send it back
	if (response.length() > 0) {
		socket.sendString(response);
	}
}

void ApplicationWebserver::wsStreamNextChunk()
{
	if (!_wsOutSocket || !_wsOutStream) {
		return;
	}

	// Pack as much data as possible into one frame before sending.
	// ReadStream produces one tiny printer-step per readBytes() call (often 1-3 bytes),
	// so we loop until the buffer is full or the source runs dry.
	constexpr size_t CHUNK_SIZE = 512;
	char buf[CHUNK_SIZE];
	size_t total = 0;

	while (total < CHUNK_SIZE) {
		int n = _wsOutStream->readBytes(buf + total, CHUNK_SIZE - total);
		if (n <= 0) {
			break;
		}
		total += n;
	}

	if (total > 0) {
		_wsOutSocket->send(buf, total, WS_FRAME_BINARY);
		_wsStreamTimer.startOnce(); // yield to event loop then send next chunk
	} else if (_wsOutStream->isFinished()) {
		// Stream truly exhausted — signal end of stream to client
		String endMsg = String(F("{\"jsonrpc\":\"2.0\",\"method\":\"stream_end\",\"params\":{\"id\":")) +
		                String(_wsOutRequestId) + F("}}");
		_wsOutSocket->sendString(endMsg);
		_wsOutStream.reset();
		_wsOutSocket = nullptr;
	} else {
		// Source temporarily dry but not finished — yield and retry
		_wsStreamTimer.startOnce();
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
	return checkHeap(response, 0);
}

bool ApplicationWebserver::checkHeap(HttpResponse& response, int minHeap)
{
	if (minHeap==0) {
		minHeap=_minimumHeap;
	}
	unsigned fh = system_get_free_heap_size();
	if(fh < minHeap) {
		setCorsHeaders(response);
		response.code = HTTP_STATUS_TOO_MANY_REQUESTS;
		response.setHeader(F("Retry-After"), "1");
		debug_i("Not enough heap free, rejecting request. Free heap: %u", fh);
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
    response.setHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
    response.setHeader(F("Pragma"), F("no-cache"));
    response.setHeader(F("Expires"), F("0"));

    // 1. Heap Check
    if(!checkHeap(response, minHeap)) {
        return false;
    }

	debug_i("received method: %i", request.method);
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
    debug_i("preflightRequest: Request Method=%d, Allowed={%s}", (int)request.method, allowedMethodsStr.c_str());

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
	enum class ConfigFlag {
		ipChanged,
		dhcpChanged,
		apChanged,
		pinConfigChanged,
		colorModeChanged,
		mqttConnChanged,   // broker or clientId changed
		mqttTopicChanged,  // topic or master changed
		mqttEnabledChanged,
		deviceNameChanged,
	};
	using ConfigFlags = BitSet<uint16_t, ConfigFlag>;
	ConfigFlags flags;

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

        String oldIP, oldSSID, oldDeviceName, oldPinConfig;
        String oldMqttServer, oldMqttPassword, oldMqttUsername, oldMqttTopicBase;
        int32_t oldMqttPort;
        bool oldDhcp, oldMqttEnabled;
        int oldColorMode;

        {
            AppConfig::Network net(*app.cfg);
            oldIP            = net.connection.getIp();
            oldDhcp          = net.connection.getDhcp();
            oldSSID          = net.ap.getSsid();
            oldMqttEnabled   = net.mqtt.getEnabled();
            oldMqttServer    = net.mqtt.getServer();
            oldMqttPort      = net.mqtt.getPort();
            oldMqttUsername  = net.mqtt.getUsername();
            oldMqttPassword  = net.mqtt.getPassword();
            oldMqttTopicBase = net.mqtt.getTopicBase();
        }
        {
            AppConfig::General gen(*app.cfg);
            oldDeviceName = gen.getDeviceName();
            oldPinConfig  = gen.getCurrentPinConfigName();
        }
        {
            AppConfig::Color col(*app.cfg);
            oldColorMode = col.getColorMode();
        }

        // --- Import ---
		auto bodyStream = request.getBodyStream();
		if(bodyStream) {
			ConfigDB::Status status = app.cfg->importFromStream(ConfigDB::Json::format, *bodyStream);

			/*********************************
			 * Handle changes:
             * - if network settings changed (ip config, default gateway, netmask, ssid, hostname(?) ) -> reboot 
             * - if mqtt settings changed to enabled -> start mqtt
             *   - if mqtt broker changed -> reconnect
             *   - if mqtt topic changed -> resubscribe
             *   - if mqtt master/secondary changed -> resubscribe to master/secondary topics where necessary
             * - if mqtt settings changed to disabled -> stop mqtt if possile, otherwise reboot
             * - if color setttings changed - reconfigure controller (see below)
             **********************************/

			// bool restart = root[F("restart")] | false;

			app.telemetryClient.log(F("Config import finished"));
        {
            AppConfig::Network net(*app.cfg);
            if(net.connection.getDhcp() != oldDhcp)
                flags += ConfigFlag::dhcpChanged;
            if(!net.connection.getDhcp() && net.connection.getIp() != oldIP)
                flags += ConfigFlag::ipChanged;
            if(net.ap.getSsid() != oldSSID)
                flags += ConfigFlag::apChanged;
            if(net.mqtt.getEnabled() != oldMqttEnabled)
                flags += ConfigFlag::mqttEnabledChanged;
            if(net.mqtt.getServer()   != oldMqttServer  ||
               net.mqtt.getPort()     != oldMqttPort    ||
               net.mqtt.getUsername() != oldMqttUsername ||
               net.mqtt.getPassword() != oldMqttPassword)
                flags += ConfigFlag::mqttConnChanged;
            if(net.mqtt.getTopicBase() != oldMqttTopicBase)
                flags += ConfigFlag::mqttTopicChanged;
        }
        {
            AppConfig::General gen(*app.cfg);
            if(gen.getDeviceName()           != oldDeviceName) flags += ConfigFlag::deviceNameChanged;
            if(gen.getCurrentPinConfigName() != oldPinConfig)  flags += ConfigFlag::pinConfigChanged;
        }
        {
            AppConfig::Color col(*app.cfg);
            if(col.getColorMode() != oldColorMode) flags += ConfigFlag::colorModeChanged;
        }
					
			bool needReboot = false;

			// Handle changes that require reboot
			if(flags[ConfigFlag::dhcpChanged] || flags[ConfigFlag::ipChanged]) {
				debug_i("onConfig: network address changed — rebooting");
				app.telemetryClient.log(F("Network config changed, rebooting"));
				needReboot = true;
			}
			if(flags[ConfigFlag::apChanged] && WifiAccessPoint.isEnabled()) {
				debug_i("onConfig: AP SSID changed — rebooting");
				app.telemetryClient.log(F("AP config changed, rebooting"));
				needReboot = true;
			}
			if(flags[ConfigFlag::pinConfigChanged]) {
				debug_i("onConfig: pin config changed — rebooting");
				app.telemetryClient.log(F("Pin config changed, rebooting"));
				needReboot = true;
			}
			if(flags[ConfigFlag::colorModeChanged]) {
				debug_i("onConfig: color mode changed — rebooting");
				app.telemetryClient.log(F("Color mode changed, rebooting"));
				needReboot = true;
			}

			if(needReboot) {
				app.delayedCMD(F("restart"), 3000);
			}

			// restart mqtt if necessary (try to avoid reboot if we can just restart mqtt client)
			if(flags[ConfigFlag::mqttEnabledChanged] || flags[ConfigFlag::mqttConnChanged]) {
				debug_i("onConfig: MQTT connection settings changed or MQTT enabled/disabled — reconfiguring MQTT client");
				app.telemetryClient.log(F("MQTT config changed, reconfiguring client"));
				if(app.mqttclient.isRunning()) app.mqttclient.stop();
				AppConfig::Network net(*app.cfg);
				if(net.mqtt.getEnabled()) app.mqttclient.start();
			} else if(flags[ConfigFlag::mqttTopicChanged] && app.mqttclient.isRunning()) {
				debug_i("onConfig: MQTT topic changed — reconfiguring MQTT client");
				app.telemetryClient.log(F("MQTT topic changed, reconfiguring client"));
				app.mqttclient.stop();
				app.mqttclient.start();
			}

			// reconfigure mDNS if device name changed
			if(flags[ConfigFlag::deviceNameChanged]) {
				debug_i("onConfig: device name changed — reconfiguring mDNS");
				app.telemetryClient.log(F("Device name changed, reconfiguring mDNS"));
				AppConfig::General gen(*app.cfg);
				app.mdnsService.setHostname(gen.getDeviceName());
			}

			setCorsHeaders(response);
			sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
		} else {
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

void ApplicationWebserver::onInfo(HttpRequest& request, HttpResponse& response)
{
	debug_i("onInfo");
	debug_i("request method: %i",request.method);
	if(!preflightRequest(request, response, { HttpMethod::GET })) return;

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif
	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject data = stream->getRoot();
	
	JsonProcessor::ApiResponse apiResp;
	apiResp.data = data;
	app.jsonproc.handleRequest(F("info"), JsonProcessor::RequestType::Query, JsonObject(), apiResp);

	if (apiResp.code == 200) {
		sendApiResponse(response, stream.release());
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}

}

/**
 * @brief Handles the HTTP GET request for retrieving the current color information.
 *
 * This function is responsible for handling the HTTP GET request and returning the current color information
 * in JSON format. The color information includes the raw RGBWW values and the corresponding HSV values.
 *
 * @param request The HTTP request object.
 * @param response The HTTP response object.
 */
void ApplicationWebserver::onColorGet(HttpRequest& request, HttpResponse& response)
{
	debug_i("onColorGet");

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();

	JsonProcessor::ApiResponse apiResp;
	apiResp.data = json;
	app.jsonproc.handleRequest(F("color"), JsonProcessor::RequestType::Query, JsonObject(), apiResp);

	if (apiResp.code == 200) {
		setCorsHeaders(response);
		sendApiResponse(response, stream.release());
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}

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

	if(body == NULL) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("no body"));
		return;
	}

	debug_i("received color update with body legth %i and content %s", body.length(),body.c_str());
	
	StaticJsonDocument<512> doc;
	DeserializationError err = deserializeJson(doc, body);
	if (err) {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Invalid JSON"));
		return;
	}

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("color"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		debug_i("received color update success");
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		debug_i("received color update error: %s", apiResp.message.c_str());
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
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
	debug_i("received /color request");

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

	auto stream = std::make_unique<JsonObjectStream>();
    JsonProcessor::ApiResponse resp;
    resp.data = stream->getRoot();
    
    StaticJsonDocument<10> doc;
    JsonObject payload = doc.to<JsonObject>();

    app.jsonproc.handleRequest(F("networks"), JsonProcessor::RequestType::Query, payload, resp);

	setCorsHeaders(response);
    if(resp.code == 200) {
	    sendApiResponse(response, stream.release());
    } else {
        sendApiCode(response, (API_CODES)resp.code, resp.message.c_str());
    }

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

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

    JsonProcessor::ApiResponse resp;
    
    // Empty payload
    StaticJsonDocument<10> doc;
    JsonObject payload = doc.to<JsonObject>();

    app.jsonproc.handleRequest(F("networks"), JsonProcessor::RequestType::Command, payload, resp);

	sendApiCode(response, resp.code == 200 ? API_CODES::API_SUCCESS : API_CODES::API_BAD_REQUEST, resp.message.c_str());
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

	debug_i("passed checks");
#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

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
    if(!preflightRequest(request, response, {HttpMethod::GET, HttpMethod::POST})) return;

#ifdef ARCH_ESP8266
	if(app.ota.isProccessing()) {
		sendApiCode(response, API_CODES::API_UPDATE_IN_PROGRESS);
		return;
	}
#endif

    JsonProcessor::RequestType type = (request.method == HttpMethod::GET) 
        ? JsonProcessor::RequestType::Query 
        : JsonProcessor::RequestType::Command;
    
    StaticJsonDocument<512> doc;
    JsonObject payload;
    
    if (type == JsonProcessor::RequestType::Command) {
        String body = request.getBody();
        if (body.length() > 0) {
             if (!Json::deserialize(doc, body)) {
                 sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Invalid JSON"));
                 return;
             }
             payload = doc.as<JsonObject>();
        } else {
             sendApiCode(response, API_CODES::API_BAD_REQUEST, F("could not get HTTP body"));
             return;
        }
    }

    StaticJsonDocument<512> respDoc;
    JsonProcessor::ApiResponse resp;
    resp.data = respDoc.to<JsonObject>();

    app.jsonproc.handleRequest(F("system"), type, payload, resp);

    setCorsHeaders(response);

    if (resp.code == 200) {
        if (resp.data.size() > 0) {
             auto stream = std::make_unique<JsonObjectStream>();
             JsonObject root = stream->getRoot();
             root.set(resp.data);
             sendApiResponse(response, stream.release());
        } else {
             sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
        }
    } else {
         if (resp.code == 400) sendApiCode(response, API_CODES::API_BAD_REQUEST, resp.message);
         else sendApiCode(response, API_CODES::API_BAD_REQUEST, resp.message);
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

	auto stream = std::make_unique<JsonObjectStream>();
	JsonObject json = stream->getRoot();
	json[F("ping")] = "pong";
	sendApiResponse(response, stream.release());
}

void ApplicationWebserver::onStop(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

	StaticJsonDocument<256> doc;
	deserializeJson(doc, request.getBody());

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("stop"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::onSkip(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

	StaticJsonDocument<256> doc;
	deserializeJson(doc, request.getBody());

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("skip"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::onPause(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

	StaticJsonDocument<256> doc;
	deserializeJson(doc, request.getBody());

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("pause"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::onContinue(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

	StaticJsonDocument<256> doc;
	deserializeJson(doc, request.getBody());

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("continue"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::onBlink(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;


	StaticJsonDocument<512> doc;
	deserializeJson(doc, request.getBody());

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("blink"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::onToggle(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::POST})) return;

	StaticJsonDocument<256> doc;
	deserializeJson(doc, request.getBody());

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("toggle"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, (const char*)nullptr);
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}


void ApplicationWebserver::onHosts(HttpRequest& request, HttpResponse& response)
{
    if(!preflightRequest(request, response, {HttpMethod::GET})) return;

    if(!app.controllers) {
        setCorsHeaders(response);
		debug_i("Controllers not initialized");
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

    debug_i("show all controllers %s", showAll ? "true" : "false");

    setCorsHeaders(response);
    response.setContentType(MIME_JSON);

    // Use the JsonStream for automatic streaming
    auto stream = app.controllers->createJsonStream(filter, false); // Compact format for HTTP
    response.sendDataStream(stream.release(), MIME_JSON);

//todo 
}

void ApplicationWebserver::onData(HttpRequest& request, HttpResponse& response){
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 12000)) return;
    
	debug_i("http onData request.method %i", request.method);

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
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 4000)) return;

	debug_i("onSetOn");

	StaticJsonDocument<512> doc;
	String body = request.getBody();
	if (body.length() > 0) {
		DeserializationError err = deserializeJson(doc, body);
		if (err) {
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Invalid JSON"));
			return;
		}
	}

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("on"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, F("SetOn OK"));
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::onSetOff(HttpRequest &request, HttpResponse &response) {
    if(!preflightRequest(request, response, {HttpMethod::POST, HttpMethod::GET}, 4000)) return;

	debug_i("onSetOff");

	StaticJsonDocument<512> doc;
	String body = request.getBody();
	if (body.length() > 0) {
		DeserializationError err = deserializeJson(doc, body);
		if (err) {
			sendApiCode(response, API_CODES::API_BAD_REQUEST, F("Invalid JSON"));
			return;
		}
	}

	JsonProcessor::ApiResponse apiResp;
	app.jsonproc.handleRequest(F("off"), JsonProcessor::RequestType::Command, doc.as<JsonObject>(), apiResp);

	if (apiResp.code == 200) {
		sendApiCode(response, API_CODES::API_SUCCESS, F("SetOff OK"));
	} else {
		sendApiCode(response, API_CODES::API_BAD_REQUEST, apiResp.message);
	}
}

void ApplicationWebserver::setCorsHeaders(HttpResponse& response)
{
	response.setAllowCrossDomainOrigin("*");
	response.setHeader(F("Access-Control-Allow-Headers"), F("Content-Type, Cache-Control"));
}
