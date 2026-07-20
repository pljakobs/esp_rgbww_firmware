#include <apihandler.h>

#include <application.h>
#include <cstring>

// Stringify an integer macro so it can be embedded in a compile-time JSON literal.
#ifndef RGBWW_STRINGIFY
#define RGBWW_STRINGIFY_(x) #x
#define RGBWW_STRINGIFY(x) RGBWW_STRINGIFY_(x)
#endif

#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
extern "C" {
#include <lwip/tcp.h>
}
#endif

#define NO_INLINE __attribute__((noinline))

namespace {
enum class CommandMethodId : uint8_t {
	Unknown,
	Color,
	Stop,
	Skip,
	Pause,
	Continue,
	Blink,
	Toggle,
	Direct,
	SetOn,
	SetOff,
	KeepAlive,
	ScanNetworks,
	System,
	WebappCheck,
};

enum class DataMethodId : uint8_t {
	Unknown,
	Info,
	Color,
	Networks,
	Hosts,
	Config,
};

CommandMethodId getCommandMethodId(const char* method)
{
	if(method == nullptr || method[0] == '\0') {
		return CommandMethodId::Unknown;
	}

	if(std::strcmp(method, "color") == 0) {
		return CommandMethodId::Color;
	}
	if(std::strcmp(method, "stop") == 0) {
		return CommandMethodId::Stop;
	}
	if(std::strcmp(method, "skip") == 0) {
		return CommandMethodId::Skip;
	}
	if(std::strcmp(method, "pause") == 0) {
		return CommandMethodId::Pause;
	}
	if(std::strcmp(method, "continue") == 0) {
		return CommandMethodId::Continue;
	}
	if(std::strcmp(method, "blink") == 0) {
		return CommandMethodId::Blink;
	}
	if(std::strcmp(method, "toggle") == 0) {
		return CommandMethodId::Toggle;
	}
	if(std::strcmp(method, "direct") == 0) {
		return CommandMethodId::Direct;
	}
	if(std::strcmp(method, "setOn") == 0 || std::strcmp(method, "on") == 0) {
		return CommandMethodId::SetOn;
	}
	if(std::strcmp(method, "setOff") == 0 || std::strcmp(method, "off") == 0) {
		return CommandMethodId::SetOff;
	}
	if(std::strcmp(method, "keep_alive") == 0) {
		return CommandMethodId::KeepAlive;
	}
	if(std::strcmp(method, "scan_networks") == 0) {
		return CommandMethodId::ScanNetworks;
	}
	if(std::strcmp(method, "system") == 0) {
		return CommandMethodId::System;
	}
	if(std::strcmp(method, "webapp_check") == 0) {
		return CommandMethodId::WebappCheck;
	}

	return CommandMethodId::Unknown;
}

DataMethodId getDataMethodId(const char* method)
{
	if(method == nullptr || method[0] == '\0') {
		return DataMethodId::Unknown;
	}

	if(std::strcmp(method, "info") == 0 || std::strcmp(method, "getInfo") == 0) {
		return DataMethodId::Info;
	}
	if(std::strcmp(method, "color") == 0 || std::strcmp(method, "getColor") == 0) {
		return DataMethodId::Color;
	}
	if(std::strcmp(method, "networks") == 0 || std::strcmp(method, "getNetworks") == 0) {
		return DataMethodId::Networks;
	}
	if(std::strcmp(method, "hosts") == 0 || std::strcmp(method, "getHosts") == 0) {
		return DataMethodId::Hosts;
	}
	if(std::strcmp(method, "config") == 0 || std::strcmp(method, "getConfig") == 0) {
		return DataMethodId::Config;
	}

	return DataMethodId::Unknown;
}

bool isPrintableSsid(const String& str)
{
	for(unsigned int i = 0; i < str.length(); ++i) {
		if(str[i] < 0x20) {
			return false;
		}
	}
	return true;
}

} // namespace

bool Api::dispatch(const String& method, const JsonObject& params, JsonObject& out)
{
	return dispatch(method.c_str(), params, out);
}

bool Api::dispatch(const char* method, const JsonObject& params, JsonObject& out)
{
	const char* methodName = (method != nullptr) ? method : "";
	debug_i(ANSI_COLOR_BLUE "Api::dispatch: method=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
			methodName);
	String errorMsg;
	if(dispatchDataRequest(methodName, params, &out, nullptr, errorMsg)) {
		return true;
	}

	out[F("error")] = errorMsg;
	out[F("method")] = methodName;
	debug_i(ANSI_COLOR_BLUE "Api::dispatch failed: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
			errorMsg.c_str());
	return false;
}

bool Api::dispatchCommand(const String& method, const JsonObject& params, String& errorMsg, bool relay)
{
	return dispatchCommand(method.c_str(), params, errorMsg, relay);
}

bool Api::dispatchCommand(const char* method, const JsonObject& params, String& errorMsg, bool relay)
{
	const char* methodName = (method != nullptr) ? method : "";
	debug_i(ANSI_COLOR_BLUE "Api::dispatchCommand: method=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, methodName);

	switch(getCommandMethodId(method)) {
	case CommandMethodId::Color:
		return app.jsonproc.onColor(params, errorMsg, relay);
	case CommandMethodId::Stop:
		return app.jsonproc.onStop(params, errorMsg, relay);
	case CommandMethodId::Skip:
		return app.jsonproc.onSkip(params, errorMsg, relay);
	case CommandMethodId::Pause:
		return app.jsonproc.onPause(params, errorMsg, relay);
	case CommandMethodId::Continue:
		return app.jsonproc.onContinue(params, errorMsg, relay);
	case CommandMethodId::Blink:
		return app.jsonproc.onBlink(params, errorMsg, relay);
	case CommandMethodId::Toggle:
		return app.jsonproc.onToggle(params, errorMsg, relay);
	case CommandMethodId::Direct:
		return app.jsonproc.onDirect(params, errorMsg, relay);
	case CommandMethodId::SetOn:
		return app.jsonproc.onSetOn(params, errorMsg, relay);
	case CommandMethodId::SetOff:
		return app.jsonproc.onSetOff(params, errorMsg, relay);
	case CommandMethodId::KeepAlive:
		// No-op ping from webapp to keep the WebSocket connection alive.
		return true;
	case CommandMethodId::ScanNetworks:
		if(!app.network.isScanning()) {
			app.network.scan(false);
		}
		return true;
	case CommandMethodId::System: {
		String cmd = params[F("cmd")] | String::nullstr;
		if(cmd == String::nullstr) {
			errorMsg = F("missing cmd");
			return false;
		}

		if(cmd.equals(F("debug"))) {
			bool enable = false;
			if(!Json::getValue(params[F("enable")], enable)) {
				errorMsg = F("missing enable");
				return false;
			}
			Serial.systemDebugOutput(enable);
			return true;
		}

		if(cmd.equals(F("restart"))) {
			bool clearOta = false;
			Json::getValue(params[F("clearOTA")], clearOta);
			if(clearOta) {
				if(!app.delayedCMD(F("clear_ota_restart"), 1500)) {
					errorMsg = F("system command failed");
					return false;
				}
			} else {
				if(!app.delayedCMD(F("restart"), 1500)) {
					errorMsg = F("system command failed");
					return false;
				}
			}
			return true;
		}

		if(!app.delayedCMD(cmd, 1500)) {
			errorMsg = F("system command failed");
			return false;
		}

		return true;
	}
	case CommandMethodId::WebappCheck:
		if(!app.webappOta.isActive()) {
			app.webappOta.checkForUpdate(true /* ignoreEnabled: manual trigger */);
		}
		return true;
	case CommandMethodId::Unknown:
	default:
		break;
	}

	errorMsg = F("method not implemented: ");
	errorMsg.concat(methodName);
	debug_e(ANSI_COLOR_RED "Api::dispatchCommand failed: %s" ANSI_COLOR_RESET, errorMsg.c_str());
	return false;
}

bool Api::dispatchCommand(const String& method, const String& params, String& errorMsg, bool relay)
{
	debug_i(ANSI_COLOR_BLUE "Api::dispatchCommand(str): method=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ", params=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, method.c_str(), params.c_str());
	const auto methodId = getCommandMethodId(method.c_str());
	if(methodId == CommandMethodId::KeepAlive) {
		return true;
	}

	if(methodId == CommandMethodId::Unknown) {
		errorMsg = F("method not implemented: ");
		errorMsg.concat(method.c_str());
		return false;
	}

	StaticJsonDocument<512> doc;
	DeserializationError err = deserializeJson(doc, params);
	if(err) {
		if(err == DeserializationError::NoMemory) {
			errorMsg = F("params too large for parse buffer");
			debug_e(ANSI_COLOR_RED "Api::dispatchCommand: params exceeded %u byte buffer" ANSI_COLOR_RESET,
					(unsigned)doc.capacity());
		} else {
			errorMsg = F("malformed json");
		}
		return false;
	}

	return dispatchCommand(method.c_str(), doc.as<JsonObject>(), errorMsg, relay);
}

bool Api::handleColor(const JsonObject& params, JsonObject& out)
{
	(void)params;

	JsonObject raw = out.createNestedObject(F("raw"));
	ChannelOutput output = app.rgbwwctrl.getCurrentOutput();
	raw[F("r")] = output.r;
	raw[F("g")] = output.g;
	raw[F("b")] = output.b;
	raw[F("ww")] = output.ww;
	raw[F("cw")] = output.cw;

	JsonObject hsv = out.createNestedObject(F("hsv"));
	float h, s, v;
	int ct;
	HSVCT c = app.rgbwwctrl.getCurrentColor();
	c.asRadian(h, s, v, ct);
	hsv[F("h")] = h;
	hsv[F("s")] = s;
	hsv[F("v")] = v;
	hsv[F("ct")] = ct;

	return true;
}

bool Api::handleNetworks(const JsonObject& params, JsonObject& out)
{
	(void)params;

	if(app.network.isScanning()) {
		out[F("scanning")] = true;
		return true;
	}

	out[F("scanning")] = false;
	JsonArray netlist = out.createNestedArray(F("available"));
	BssList networks = app.network.getAvailableNetworks();
	for(unsigned int i = 0; i < networks.count(); i++) {
		if(networks[i].hidden) {
			continue;
		}
		if(!isPrintableSsid(networks[i].ssid)) {
			continue;
		}

		JsonObject item = netlist.createNestedObject();
		item[F("id")] = (int)networks[i].getHashId();
		item[F("ssid")] = networks[i].ssid;
		item[F("signal")] = networks[i].rssi;
		item[F("encryption")] = networks[i].getAuthorizationMethodName();

		if(i >= 25) {
			break;
		}
	}

	return true;
}

bool Api::dispatchJsonRpc(const String& json, String& errorMsg, bool relay)
{
	JsonRpcMessageIn rpc(json);
	if(!rpc.isValid()) {
		errorMsg = rpc.getError().length() ? rpc.getError() : String(F("malformed json"));
		return false;
	}

	const char* method = rpc.getMethod();
	if(method == nullptr || method[0] == '\0') {
		errorMsg = F("missing method");
		return false;
	}

	return dispatchCommand(method, rpc.getParams(), errorMsg, relay);
}

bool Api::dispatchStream(const String& method, const JsonObject& params, std::unique_ptr<IDataSourceStream>& out,
					 String& errorMsg)
{
	debug_i(ANSI_COLOR_BLUE "Api::dispatchStream: method=" ANSI_COLOR_RED "%s" ANSI_COLOR_RESET, method.c_str());
	return dispatchDataRequest(method, params, nullptr, &out, errorMsg);
}

bool Api::dispatchDataRequest(const String& method, const JsonObject& params, JsonObject* outObject,
						 std::unique_ptr<IDataSourceStream>* outStream, String& errorMsg)
{
	return dispatchDataRequest(method.c_str(), params, outObject, outStream, errorMsg);
}

bool Api::dispatchDataRequest(const char* method, const JsonObject& params, JsonObject* outObject,
						 std::unique_ptr<IDataSourceStream>* outStream, String& errorMsg)
{
	const char* methodName = (method != nullptr) ? method : "";
	const auto dataMethodId = getDataMethodId(methodName);

	if(outObject != nullptr) {
		debug_i(ANSI_COLOR_BLUE "Api::dispatchDataRequest: method=" ANSI_COLOR_RED "%s" ANSI_COLOR_RESET, methodName);
		if(dataMethodId == DataMethodId::Info) {
			return handleInfo(params, *outObject);
		}
		if(dataMethodId == DataMethodId::Color) {
			return handleColor(params, *outObject);
		}
		if(dataMethodId == DataMethodId::Networks) {
			return handleNetworks(params, *outObject);
		}
	}

	if(outStream != nullptr) {
		if(dataMethodId == DataMethodId::Hosts) {
			return handleHosts(params, *outStream, errorMsg);
		}
		if(dataMethodId == DataMethodId::Config) {
			return handleConfig(params, *outStream, errorMsg);
		}
	}

	errorMsg = F("method not implemented");
	return false;
}

NO_INLINE void buildAppInfo(JsonObject& data) {
    JsonObject application = data.createNestedObject(F("app"));
    AppConfig::Root::Webapp webappCfg(*app.cfg);
    String installedVer = webappCfg.getInstalledVersion();
    application[F("webapp_version")] = installedVer.length() > 0 ? installedVer : String(WEBAPP_VERSION);
    application[F("git_version")] = fw_git_version;
    application[F("build_type")] = BUILD_TYPE;
    application[F("git_date")] = fw_git_date;
}

NO_INLINE void buildFsInfo(JsonObject& data) {
    JsonObject fs = data.createNestedObject(F("filesystem"));
    IFS::FileSystem::Info fsInfo;
    if (fileGetSystemInfo(fsInfo) != FS_OK) {
        fs[F("error")] = F("failed to get filesystem info");
    } else {
        fs[F("total_bytes")] = fsInfo.volumeSize;
        fs[F("free_bytes")] = fsInfo.freeSpace;
        fs[F("used_bytes")] = fsInfo.volumeSize - fsInfo.freeSpace;
    }
}

NO_INLINE void buildNetworkInfo(JsonObject& data) {
    JsonObject con = data.createNestedObject(F("connection"));
    con[F("connected")] = WifiStation.isConnected();
    if(WifiStation.isConnected()) {
        con[F("ssid")] = WifiStation.getSSID();
        con[F("dhcp")] = WifiStation.isEnabledDHCP();
        // The temporary String objects generated here are isolated to this stack frame
        con[F("ip")] = WifiStation.getIP().toString();
        con[F("netmask")] = WifiStation.getNetworkMask().toString();
        con[F("gateway")] = WifiStation.getNetworkGateway().toString();
        con[F("mac")] = WifiStation.getMAC();
        con[F("rssi")] = WifiStation.getRssi();
    }
}

NO_INLINE void buildMqttInfo(JsonObject& data) {
    JsonObject mqtt = data.createNestedObject(F("mqtt"));
    JsonObject ha = data.createNestedObject(F("homeassistant"));
    
    if(!app.ota.isProccessing()) {
        AppConfig::Network network(*app.cfg);
        bool enabled = network.mqtt.getEnabled();
        
        if(enabled && !app.mqttclient.isRunning()) {
            mqtt[F("status")] = F("configured but not running");
        } else if(enabled && app.mqttclient.isRunning()) {
            mqtt[F("status")] = F("running");
        } else {
            mqtt[F("status")] = F("disabled");
        }
        mqtt[F("enabled")] = enabled;
        mqtt[F("broker")] = network.mqtt.getServer();
        mqtt[F("topic")] = network.mqtt.getTopicBase();

        ha[F("enabled")] = network.mqtt.homeassistant.getEnable();
        ha[F("discovery_prefix")] = network.mqtt.homeassistant.getDiscoveryPrefix();
        ha[F("Node ID")] = network.mqtt.homeassistant.getNodeId();
    } else {
        mqtt[F("status")] = F("ota in progress");
        mqtt[F("enabled")] = false;
        mqtt[F("broker")] = String::nullstr;
        mqtt[F("topic")] = String::nullstr;

        ha[F("enabled")] = false;
        ha[F("discovery_prefix")] = String::nullstr;
        ha[F("Node ID")] = String::nullstr;
    }
}

// Cleaned up main handler
bool Api::handleInfo(const JsonObject& params, JsonObject& data, uint32_t heapFreeSnapshot)
{
    debug_i(ANSI_COLOR_BLUE "Api::handleInfo called" ANSI_COLOR_RESET);
    const uint32_t heapFreeReported = (heapFreeSnapshot != 0) ? heapFreeSnapshot : app.getFreeHeapSize();

    JsonVariantConst version = params[F("V")];
    if(version.isNull()) {
        version = params[F("v")];
    }

    bool isV2 = false;
    if(!version.isNull()) {
        if(version.is<long>() || version.is<int>() || version.is<unsigned long>() || version.is<unsigned int>()) {
            isV2 = (version.as<long>() == 2);
        } else {
            const char* v = version.as<const char*>();
            if(v != nullptr) {
                isV2 = (v[0] == '2' && v[1] == '\0');
            }
        }
    }

    if(isV2) {
        debug_i(ANSI_COLOR_BLUE "Api::handleInfo: version 2 detected" ANSI_COLOR_RESET);

        static char s_infoDeviceV2[80];
        if(s_infoDeviceV2[0] == '\0') {
    #if defined(ARCH_ESP8266)
            m_snprintf(s_infoDeviceV2, sizeof(s_infoDeviceV2),
                "{\"deviceid\":%u,\"soc\":\"" SOC "\",\"current_rom\":\"%s\"}",
                (unsigned)system_get_chip_id(), app.ota.getRomPartition().name().c_str());
    #elif defined(ARCH_ESP32)
            m_snprintf(s_infoDeviceV2, sizeof(s_infoDeviceV2),
                "{\"deviceid\":0,\"soc\":\"" SOC "\",\"current_rom\":\"%s\"}",
                app.ota.getRomPartition().name().c_str());
    #else
            m_snprintf(s_infoDeviceV2, sizeof(s_infoDeviceV2),
                "{\"deviceid\":0,\"soc\":\"" SOC "\"}");
    #endif
        }

        static const char kInfoSmingV2[] PROGMEM = "{\"version\":\"" SMING_VERSION "\"}";
        static const char kInfoRgbwwV2[] PROGMEM =
            "{\"version\":\"" RGBWW_VERSION "\",\"queuesize\":" RGBWW_STRINGIFY(RGBWW_ANIMATIONQSIZE) "}";

        data[F("version")] = 2;
        data[F("device")] = serialized((const char*)s_infoDeviceV2);

        // Isolated sub-function calls execute sequentially, drastically flattening peak stack usage
        buildAppInfo(data);

        data[F("sming")] = serialized(FPSTR(kInfoSmingV2));

        buildFsInfo(data);

        {
            JsonObject run = data.createNestedObject(F("runtime"));
            run[F("uptime")] = app.getUptime();
            run[F("heap_free")] = heapFreeReported;
            run[F("minfreeHeapRuntime")] = app.getMinimumHeapUptime();
            run[F("minfreeHeap10min")] = app.getMinimumHeap10min();
            run[F("heapLowErrUptime")] = app.getHeapLowErrUptime();
            run[F("heapLowErr10min")] = app.getHeapLowErr10min();
        }   

        {
            JsonObject debug = data.createNestedObject(F("debug"));
            debug[F("http_active_connections")] = app.webserver.getHttpActiveConnections();
            debug[F("websocket_connections")] = app.webserver.getWebsocketConnectionCount();
            debug[F("eventserver_clients")] = app.eventserver.activeClients;
            debug[F("tcp_pcb_size")] = 0;
            debug[F("tcp_active_estimated_bytes")] = 0;
        }

		#ifdef RGBWW_ANIMATIONQSIZE
        data[F("rgbww")] = serialized(FPSTR(kInfoRgbwwV2));
        #endif 
		
        buildNetworkInfo(data);
        buildMqttInfo(data);

        if(app.ota.isProccessing()) {
            JsonObject ota = data.createNestedObject(F("ota"));
            ota[F("status")] = F("in progress");
        }

        return true;
    }else{
		// legacy payload shape
	#if defined(ARCH_ESP8266)
		data[F("deviceid")] = system_get_chip_id();
	#else
		data[F("deviceid")] = 0;
	#endif
		data[F("soc")] = SOC;
	#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
		data[F("current_rom")] = String(app.ota.getRomPartition().name());
	#endif
		data[F("git_version")] = fw_git_version;
		data[F("build_type")] = BUILD_TYPE;
		data[F("git_date")] = fw_git_date;
		{
			AppConfig::Root::Webapp webappCfg(*app.cfg);
			String installedVer = webappCfg.getInstalledVersion();
			data[F("webapp_version")] = installedVer.length() > 0 ? installedVer : String(WEBAPP_VERSION);
		}
		data[F("sming")] = SMING_VERSION;
		data[F("event_num_clients")] = app.eventserver.activeClients;
		data[F("uptime")] = app.getUptime();
		data[F("heap_free")] = heapFreeReported;

		JsonObject rgbww = data.createNestedObject(F("rgbww"));
		rgbww[F("version")] = RGBWW_VERSION;
		rgbww[F("queuesize")] = RGBWW_ANIMATIONQSIZE;

		JsonObject con = data.createNestedObject(F("connection"));
		con[F("connected")] = WifiStation.isConnected();
		con[F("ssid")] = WifiStation.getSSID();
		con[F("dhcp")] = WifiStation.isEnabledDHCP();
		con[F("ip")] = WifiStation.getIP().toString();
		con[F("netmask")] = WifiStation.getNetworkMask().toString();
		con[F("gateway")] = WifiStation.getNetworkGateway().toString();
		con[F("mac")] = WifiStation.getMAC();

		return true;
	}

    return false;
}

bool Api::handleHosts(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg)
{
	if(!app.controllers) {
		errorMsg = F("Controllers not initialized");
		return false;
	}

	const bool showAll = (params[F("all")] == "1") || (params[F("all")] == "true");
	const bool showDebug = (params[F("debug")] == "1") || (params[F("debug")] == "true");

	Controllers::JsonFilter filter = (showAll || showDebug) ? Controllers::ALL_ENTRIES : Controllers::VISIBLE_ONLY;

	out = app.controllers->createJsonStream(filter, false);
	if(!out) {
		errorMsg = F("could not create hosts stream");
		return false;
	}

	return true;
}

bool Api::handleConfig(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg)
{
	(void)params;

	if(!app.cfg) {
		errorMsg = F("ConfigDB not initialized");
		return false;
	}

	out = app.cfg->createExportStream(ConfigDB::Json::format);
	if(!out) {
		errorMsg = F("could not create config response");
		return false;
	}

	return true;
}
