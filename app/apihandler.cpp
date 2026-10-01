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
	if (method == nullptr || method[0] == '\0') {
		return CommandMethodId::Unknown;
	}

	struct CommandMapping {
		PGM_P name;
		CommandMethodId id;
	};

	static const char s_color[] PROGMEM = "color";
	static const char s_stop[] PROGMEM = "stop";
	static const char s_skip[] PROGMEM = "skip";
	static const char s_pause[] PROGMEM = "pause";
	static const char s_continue[] PROGMEM = "continue";
	static const char s_blink[] PROGMEM = "blink";
	static const char s_toggle[] PROGMEM = "toggle";
	static const char s_direct[] PROGMEM = "direct";
	static const char s_setOn[] PROGMEM = "setOn";
	static const char s_on[] PROGMEM = "on";
	static const char s_setOff[] PROGMEM = "setOff";
	static const char s_off[] PROGMEM = "off";
	static const char s_scan[] PROGMEM = "scan_networks";
	static const char s_system[] PROGMEM = "system";
	static const char s_webapp[] PROGMEM = "webapp_check";

	static const CommandMapping commands[] PROGMEM = {
		{ s_color,    CommandMethodId::Color },
		{ s_stop,     CommandMethodId::Stop },
		{ s_skip,     CommandMethodId::Skip },
		{ s_pause,    CommandMethodId::Pause },
		{ s_continue, CommandMethodId::Continue },
		{ s_blink,    CommandMethodId::Blink },
		{ s_toggle,   CommandMethodId::Toggle },
		{ s_direct,   CommandMethodId::Direct },
		{ s_setOn,    CommandMethodId::SetOn },
		{ s_on,       CommandMethodId::SetOn },
		{ s_setOff,   CommandMethodId::SetOff },
		{ s_off,      CommandMethodId::SetOff },
		{ s_scan,     CommandMethodId::ScanNetworks },
		{ s_system,   CommandMethodId::System },
		{ s_webapp,   CommandMethodId::WebappCheck },
	};

	for (size_t i = 0; i < ARRAY_SIZE(commands); ++i) {
		PGM_P pName = reinterpret_cast<PGM_P>(pgm_read_ptr(&commands[i].name));
		if (strcmp_P(method, pName) == 0) {
			return static_cast<CommandMethodId>(pgm_read_byte(&commands[i].id));
		}
	}

	return CommandMethodId::Unknown;
}

DataMethodId getDataMethodId(const char* method)
{
	if (method == nullptr || method[0] == '\0') {
		return DataMethodId::Unknown;
	}

	struct DataMapping {
		PGM_P name;
		DataMethodId id;
	};

	static const char s_info[] PROGMEM = "info";
	static const char s_getInfo[] PROGMEM = "getInfo";
	static const char s_color[] PROGMEM = "color";
	static const char s_getColor[] PROGMEM = "getColor";
	static const char s_networks[] PROGMEM = "networks";
	static const char s_getNetworks[] PROGMEM = "getNetworks";
	static const char s_hosts[] PROGMEM = "hosts";
	static const char s_getHosts[] PROGMEM = "getHosts";
	static const char s_config[] PROGMEM = "config";
	static const char s_getConfig[] PROGMEM = "getConfig";

	static const DataMapping methods[] PROGMEM = {
		{ s_info,        DataMethodId::Info },
		{ s_getInfo,     DataMethodId::Info },
		{ s_color,       DataMethodId::Color },
		{ s_getColor,    DataMethodId::Color },
		{ s_networks,    DataMethodId::Networks },
		{ s_getNetworks, DataMethodId::Networks },
		{ s_hosts,       DataMethodId::Hosts },
		{ s_getHosts,    DataMethodId::Hosts },
		{ s_config,      DataMethodId::Config },
		{ s_getConfig,   DataMethodId::Config },
	};

	for (size_t i = 0; i < ARRAY_SIZE(methods); ++i) {
		PGM_P pName = reinterpret_cast<PGM_P>(pgm_read_ptr(&methods[i].name));
		if (strcmp_P(method, pName) == 0) {
			return static_cast<DataMethodId>(pgm_read_byte(&methods[i].id));
		}
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
	if(methodId == CommandMethodId::Unknown) {
		errorMsg = F("method not implemented: ");
		errorMsg.concat(method.c_str());
		return false;
	}

	DynamicJsonDocument doc(512);
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

bool Api::renderData(const String& method, const JsonObject& params, String& out)
{
	return renderData(method, params, out, -1);
}

bool Api::renderData(const String& method, const JsonObject& params, String& out, int requestId)
{
	auto& codec = rpcCodec();
	Jsonrpc::Root root(codec.db());
	const auto dataMethodId = getDataMethodId(method.c_str());

	if(dataMethodId == DataMethodId::Info) {
		JsonVariantConst sparseParam = params[F("sparse")];
		if(sparseParam.isNull()) {
			sparseParam = params[F("S")];
		}
		const bool sparse = sparseParam.isNull() ? true :
			(sparseParam.is<bool>() ? sparseParam.as<bool>() :
			 !(sparseParam.as<String>() == F("0") || sparseParam.as<String>() == F("false") || sparseParam.as<String>() == F("off")));

		if(auto update = root.update()) {
			auto info = update.toInfo();
		auto fillCommon = [&](auto& value) {
			#if defined(ARCH_ESP8266)
			value.device.setDeviceid(system_get_chip_id());
			#else
			value.device.setDeviceid(0);
			#endif
			value.device.setSoc(SOC);
			#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
			value.device.setCurrentRom(String(app.ota.getRomPartition().name()));
			#endif
			value.app.setGitVersion(fw_git_version);
			value.app.setBuildType(BUILD_TYPE);
			value.app.setGitDate(fw_git_date);
			AppConfig::Webapp webappCfg(*app.cfg);
			String installedVer = webappCfg.getInstalledVersion();
			value.app.setWebappVersion(installedVer.length() > 0 ? installedVer : String(WEBAPP_VERSION));
			value.sming.setVersion(SMING_VERSION);
			IFS::FileSystem::Info fsInfo;
			if(fileGetSystemInfo(fsInfo) == FS_OK) {
				value.filesystem.setTotalBytes(fsInfo.volumeSize);
				value.filesystem.setFreeBytes(fsInfo.freeSpace);
				value.filesystem.setUsedBytes(fsInfo.volumeSize - fsInfo.freeSpace);
			}
			value.rgbww.setVersion(RGBWW_VERSION);
			value.rgbww.setQueuesize(RGBWW_ANIMATIONQSIZE);
			value.connection.setConnected(WifiStation.isConnected());
			if(WifiStation.isConnected()) {
				value.connection.setSsid(WifiStation.getSSID());
				value.connection.setDhcp(WifiStation.isEnabledDHCP());
				value.connection.setIp(WifiStation.getIP().toString());
				value.connection.setNetmask(WifiStation.getNetworkMask().toString());
				value.connection.setGateway(WifiStation.getNetworkGateway().toString());
				value.connection.setMac(WifiStation.getMAC());
				value.connection.setRssi(WifiStation.getRssi());
			}
			AppConfig::Network network(*app.cfg);
			const bool mqttEnabled = !app.ota.isProccessing() && network.mqtt.getEnabled();
			value.mqtt.setEnabled(mqttEnabled);
			value.mqtt.setBroker(mqttEnabled ? network.mqtt.getServer() : String::nullstr);
			value.mqtt.setTopic(mqttEnabled ? network.mqtt.getTopicBase() : String::nullstr);
			value.mqtt.setStatus(app.ota.isProccessing() ? F("ota in progress") :
				(mqttEnabled ? (app.mqttclient.isRunning() ? F("running") : F("configured but not running")) : F("disabled")));
			value.homeassistant.setEnabled(mqttEnabled && network.mqtt.homeassistant.getEnable());
			value.homeassistant.setDiscoveryPrefix(mqttEnabled ? network.mqtt.homeassistant.getDiscoveryPrefix() : String::nullstr);
			value.homeassistant.setNodeID(mqttEnabled ? network.mqtt.homeassistant.getNodeId() : String::nullstr);
			if(app.ota.isProccessing()) {
				value.ota.setStatus(F("in progress"));
			}
		};
			if(!sparse) {
				auto full = info.toInfoFullParams();
				full.setVersion(2);
				fillCommon(full);
				auto& runtime = full.runtime;
				runtime.setUptime(app.getUptime());
				runtime.setHeapFree(app.getFreeHeapSize());
				runtime.setMinfreeHeapRuntime(app.getMinimumHeapUptime());
				runtime.setMinfreeHeap10min(app.getMinimumHeap10min());
				runtime.setHeapLowErrUptime(app.getHeapLowErrUptime());
				runtime.setHeapLowErr10min(app.getHeapLowErr10min());
				auto& debug = full.debug;
				debug.setHttpActiveConnections(app.webserver.getHttpActiveConnections());
				debug.setWebsocketConnections(app.webserver.getWebsocketConnectionCount());
				debug.setEventserverClients(app.eventserver.activeClients);
			}
			else {
				auto stat = info.toInfoStaticParams();
				stat.setVersion(2);
				fillCommon(stat);
			}
		}
		const auto infoBody = root.asInfo();
		if(!sparse) {
			if(requestId >= 0) {
				return codec.render({requestId, JsonRPC::Message::Kind::result, method},
					infoBody.asInfoFullParams(), out);
			}
			return codec.renderPayload(infoBody.asInfoFullParams(), out);
		}
		if(requestId >= 0) {
			return codec.render({requestId, JsonRPC::Message::Kind::result, method},
				infoBody.asInfoStaticParams(), out);
		}
		return codec.renderPayload(infoBody.asInfoStaticParams(), out);
	}

	if(dataMethodId == DataMethodId::Color) {
		if(auto update = root.update()) {
			auto color = update.toColor();
			ChannelOutput output = app.rgbwwctrl.getCurrentOutput();
			{
				auto raw = color.toRaw();
				raw.setR(output.r);
				raw.setG(output.g);
				raw.setB(output.b);
				raw.setWw(output.ww);
				raw.setCw(output.cw);
			}

			float h, s, v;
			int ct;
			HSVCT current = app.rgbwwctrl.getCurrentColor();
			current.asRadian(h, s, v, ct);
			{
				auto hsv = color.toHsv();
				hsv.setH(h);
				hsv.setS(s);
				hsv.setV(v);
				hsv.setCt(ct);
			}
		}
		if(requestId >= 0) {
			return codec.render({requestId, JsonRPC::Message::Kind::result, method}, root.asColor(), out);
		}
		return codec.renderPayload(root.asColor(), out);
	}

	if(dataMethodId == DataMethodId::Networks) {
		if(auto update = root.update()) {
			auto networks = update.toNetworks().toNetworksParams();
			const bool scanning = app.network.isScanning();
			networks.setScanning(scanning);
			if(!scanning) {
				BssList available = app.network.getAvailableNetworks();
				for(unsigned int i = 0; i < available.count(); ++i) {
					if(available[i].hidden || !isPrintableSsid(available[i].ssid)) {
						continue;
					}
					auto item = networks.available.addItem();
					item.setId(String(available[i].getHashId()));
					item.setSsid(available[i].ssid);
					item.setSignal(available[i].rssi);
					item.setEncryption(available[i].getAuthorizationMethodName());
					if(i >= 25) {
						break;
					}
				}
			}
		}
		if(requestId >= 0) {
			return codec.render({requestId, JsonRPC::Message::Kind::result, method},
				root.asNetworks().asNetworksParams(), out);
		}
		return codec.renderPayload(root.asNetworks().asNetworksParams(), out);
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
