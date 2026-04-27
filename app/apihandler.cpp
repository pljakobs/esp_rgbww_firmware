#include <apihandler.h>

#include <application.h>

#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
extern "C" {
#include <lwip/tcp.h>
}
#endif

namespace {
bool isPrintableSsid(const String& str)
{
	for(unsigned int i = 0; i < str.length(); ++i) {
		if(str[i] < 0x20) {
			return false;
		}
	}
	return true;
}

struct TcpPcbStats
{
	uint8_t active_total{0};
	uint8_t established{0};
	uint8_t syn_sent{0};
	uint8_t syn_rcvd{0};
	uint8_t fin_wait_1{0};
	uint8_t fin_wait_2{0};
	uint8_t close_wait{0};
	uint8_t closing{0};
	uint8_t last_ack{0};
	uint8_t time_wait{0};
	uint8_t closed{0};
};

TcpPcbStats getTcpPcbStats()
{
	TcpPcbStats stats;
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
	for(const tcp_pcb* pcb = tcp_active_pcbs; pcb != nullptr; pcb = pcb->next) {
		++stats.active_total;
		switch(pcb->state) {
		case ESTABLISHED:
			++stats.established;
			break;
		case SYN_SENT:
			++stats.syn_sent;
			break;
		case SYN_RCVD:
			++stats.syn_rcvd;
			break;
		case FIN_WAIT_1:
			++stats.fin_wait_1;
			break;
		case FIN_WAIT_2:
			++stats.fin_wait_2;
			break;
		case CLOSE_WAIT:
			++stats.close_wait;
			break;
		case CLOSING:
			++stats.closing;
			break;
		case LAST_ACK:
			++stats.last_ack;
			break;
		case TIME_WAIT:
			++stats.time_wait;
			break;
		case CLOSED:
			++stats.closed;
			break;
		default:
			break;
		}
	}
#endif
	return stats;
}
} // namespace

bool Api::dispatch(const String& method, const JsonObject& params, JsonObject& out)
{
	debug_i("Api::dispatch: method=%s, params=%s", method.c_str(), Json::serialize(params).c_str());
	String errorMsg;
	if(dispatchDataRequest(method, params, &out, nullptr, errorMsg)) {
		return true;
	}

	out[F("error")] = errorMsg;
	out[F("method")] = method;
	debug_i("Api::dispatch failed: %s", errorMsg.c_str());
	return false;
}

bool Api::dispatchCommand(const String& method, const JsonObject& params, String& errorMsg, bool relay)
{
    debug_i("Api::dispatchCommand: method=%s, params=%s", method.c_str(), Json::serialize(params).c_str());
	if(method == F("color")) {
		return app.jsonproc.onColor(params, errorMsg, relay);
	}
	if(method == F("stop")) {
		return app.jsonproc.onStop(params, errorMsg, relay);
	}
	if(method == F("skip")) {
		return app.jsonproc.onSkip(params, errorMsg, relay);
	}
	if(method == F("pause")) {
		return app.jsonproc.onPause(params, errorMsg, relay);
	}
	if(method == F("continue")) {
		return app.jsonproc.onContinue(params, errorMsg, relay);
	}
	if(method == F("blink")) {
		return app.jsonproc.onBlink(params, errorMsg, relay);
	}
	if(method == F("toggle")) {
		return app.jsonproc.onToggle(params, errorMsg, relay);
	}
	if(method == F("direct")) {
		return app.jsonproc.onDirect(params, errorMsg, relay);
	}
	if(method == F("setOn") || method == F("on")) {
		return app.jsonproc.onSetOn(params, errorMsg, relay);
	}
	if(method == F("setOff") || method == F("off")) {
		return app.jsonproc.onSetOff(params, errorMsg, relay);
	}
	if(method == F("scan_networks")) {
		if(!app.network.isScanning()) {
			app.network.scan(false);
		}
		return true;
	}
	if(method == F("system")) {
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

		if(cmd.equals(F("debug_log"))) {
			JsonObject logParams;
			if(params[F("param")].is<JsonObject>()) {
				logParams = params[F("param")].as<JsonObject>();
			} else {
				// Allow legacy flat payloads where lvl/text are at top level.
				logParams = params;
			}

			int level = 0;
			if(!Json::getValue(logParams[F("lvl")], level)) {
				errorMsg = F("missing lvl");
				return false;
			}

			String text = String::nullstr;
			if(!Json::getValue(logParams[F("text")], text) || text == String::nullstr) {
				errorMsg = F("missing text");
				return false;
			}

			switch(level) {
			case 1:
				debug_i("WEBAPP: api debug_log: %s", text.c_str());
				break;
			case 2:
				debug_w("WEBAPP: api debug_log: %s", text.c_str());
				break;
			case 3:
				debug_e("WEBAPP: api debug_log: %s", text.c_str());
				break;
			default:
				errorMsg = F("invalid lvl (use 1, 2 or 3)");
				return false;
			}

			return true;
		}

		if(cmd.equals(F("restart"))) {
			bool clearOta = false;
			Json::getValue(params[F("clearOTA")], clearOta);
			String restartCmd = clearOta ? F("clear_ota_restart") : F("restart");
			if(!app.delayedCMD(restartCmd, 1500)) {
				errorMsg = F("system command failed");
				return false;
			}
			return true;
		}

		if(!app.delayedCMD(cmd, 1500)) {
			errorMsg = F("system command failed");
			return false;
		}

		return true;
	}

	errorMsg = F("method not implemented");
    debug_e("Api::dispatchCommand failed: %s", errorMsg.c_str());
	return false;
}

bool Api::dispatchCommand(const String& method, const String& params, String& errorMsg, bool relay)
{
	debug_i("Api::dispatchCommand(str): method=%s, params=%s", method.c_str(), params.c_str());
	if(method == F("color")) {
		return app.jsonproc.onColor(params, errorMsg, relay);
	}
	if(method == F("stop")) {
		return app.jsonproc.onStop(params, errorMsg, relay);
	}
	if(method == F("skip")) {
		return app.jsonproc.onSkip(params, errorMsg, relay);
	}
	if(method == F("pause")) {
		return app.jsonproc.onPause(params, errorMsg, relay);
	}
	if(method == F("continue")) {
		return app.jsonproc.onContinue(params, errorMsg, relay);
	}
	if(method == F("blink")) {
		return app.jsonproc.onBlink(params, errorMsg, relay);
	}
	if(method == F("toggle")) {
		return app.jsonproc.onToggle(params, errorMsg, relay);
	}
	if(method == F("direct")) {
		return app.jsonproc.onDirect(params, errorMsg, relay);
	}
	if(method == F("setOn") || method == F("on")) {
		return app.jsonproc.onSetOn(params, errorMsg, relay);
	}
	if(method == F("setOff") || method == F("off")) {
		return app.jsonproc.onSetOff(params, errorMsg, relay);
	}

	StaticJsonDocument<512> doc;
	DeserializationError err = deserializeJson(doc, params);
	if(err) {
		errorMsg = F("malformed json");
		return false;
	}

	return dispatchCommand(method, doc.as<JsonObject>(), errorMsg, relay);
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
		errorMsg = F("malformed json");
		return false;
	}

	String method = rpc.getMethod();
	if(!method.length()) {
		errorMsg = F("missing method");
		return false;
	}

	return dispatchCommand(method, rpc.getParams(), errorMsg, relay);
}

bool Api::dispatchStream(const String& method, const JsonObject& params, std::unique_ptr<IDataSourceStream>& out,
					 String& errorMsg)
{
	debug_i("Api::dispatchStream: method=%s, params=%s", method.c_str(), Json::serialize(params).c_str());
	return dispatchDataRequest(method, params, nullptr, &out, errorMsg);
}

bool Api::dispatchDataRequest(const String& method, const JsonObject& params, JsonObject* outObject,
						 std::unique_ptr<IDataSourceStream>* outStream, String& errorMsg)
{
	if(outObject != nullptr) {
		if(method == F("info") || method == F("getInfo")) {
			return handleInfo(params, *outObject);
		}
		if(method == F("color") || method == F("getColor")) {
			return handleColor(params, *outObject);
		}
		if(method == F("networks") || method == F("getNetworks")) {
			return handleNetworks(params, *outObject);
		}
	}

	if(outStream != nullptr) {
		if(method == F("hosts") || method == F("getHosts")) {
			return handleHosts(params, *outStream, errorMsg);
		}
		if(method == F("config") || method == F("getConfig")) {
			return handleConfig(params, *outStream, errorMsg);
		}
	}

	errorMsg = F("method not implemented");
	return false;
}

bool Api::handleInfo(const JsonObject& params, JsonObject& data)
{
	const String versionUpper = params[F("V")] | String::nullstr;
	const String versionLower = params[F("v")] | String::nullstr;
	const bool isV2 = (versionUpper == "2" || versionLower == "2");

	const auto tcpStats = getTcpPcbStats();

	if(isV2) {
		JsonObject dev = data.createNestedObject(F("device"));
#if defined(ARCH_ESP8266)
		dev[F("deviceid")] = system_get_chip_id();
#else
		dev[F("deviceid")] = 0;
#endif
		dev[F("soc")] = SOC;
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
		dev[F("current_rom")] = String(app.ota.getRomPartition().name());
#endif

		JsonObject application = data.createNestedObject(F("app"));
		application[F("webapp_version")] = WEBAPP_VERSION;
		application[F("git_version")] = fw_git_version;
		application[F("build_type")] = BUILD_TYPE;
		application[F("git_date")] = fw_git_date;

		JsonObject sming = data.createNestedObject(F("sming"));
		sming[F("version")] = SMING_VERSION;

		JsonObject run = data.createNestedObject(F("runtime"));
		run[F("uptime")] = app.getUptime();
		run[F("heap_free")] = app.getFreeHeapSize();
		run[F("minimumfreeHeapRuntime")] = app.getMinimumHeapUptime();
		run[F("minimumfreeHeap10min")] = app.getMinimumHeap10min();
		run[F("heapLowErrUptime")] = app.getHeapLowErrUptime();
		run[F("heapLowErr10min")] = app.getHeapLowErr10min();

		JsonObject debug = data.createNestedObject(F("debug"));
#ifndef SMING_RELEASE
		const char* preNetState = "unknown";
		switch(app.syslogPreNetState()) {
		case UdpSyslogStream::PreNetState::Buffering:
			preNetState = "buffering";
			break;
		case UdpSyslogStream::PreNetState::Draining:
			preNetState = "draining";
			break;
		case UdpSyslogStream::PreNetState::Done:
			preNetState = "done";
			break;
		}
		debug[F("syslog_pre_net_state")] = preNetState;
		debug[F("syslog_pre_net_buffer_allocated")] = app.udpSyslogStream.preNetBufferAllocated();
		debug[F("syslog_pre_net_encoder_allocated")] = app.udpSyslogStream.preNetEncoderAllocated();
		debug[F("syslog_pre_net_buffer_capacity")] = app.udpSyslogStream.preNetBufferCapacity();
		debug[F("syslog_pre_net_buffer_used")] = app.udpSyslogStream.preNetBufferUsed();
		debug[F("syslog_pre_net_buffer_frames")] = app.udpSyslogStream.preNetBufferedFrames();
		debug[F("syslog_pre_net_buffer_evicted")] = app.udpSyslogStream.preNetEvictedFrames();
#else
		debug[F("syslog_pre_net_state")] = "release";
		debug[F("syslog_pre_net_buffer_allocated")] = false;
		debug[F("syslog_pre_net_encoder_allocated")] = false;
		debug[F("syslog_pre_net_buffer_capacity")] = 0;
		debug[F("syslog_pre_net_buffer_used")] = 0;
		debug[F("syslog_pre_net_buffer_frames")] = 0;
		debug[F("syslog_pre_net_buffer_evicted")] = 0;
#endif

		debug[F("http_active_connections")] = app.webserver.getHttpActiveConnections();
		debug[F("websocket_connections")] = app.webserver.getWebsocketConnectionCount();
		debug[F("eventserver_clients")] = app.eventserver.activeClients;
#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
		debug[F("tcp_pcb_size")] = sizeof(tcp_pcb);
		debug[F("tcp_active_estimated_bytes")] = static_cast<uint32_t>(tcpStats.active_total) * sizeof(tcp_pcb);
#else
		debug[F("tcp_pcb_size")] = 0;
		debug[F("tcp_active_estimated_bytes")] = 0;
#endif

		JsonObject rgbww = data.createNestedObject(F("rgbww"));
		rgbww[F("version")] = RGBWW_VERSION;
		rgbww[F("queuesize")] = RGBWW_ANIMATIONQSIZE;

		JsonObject con = data.createNestedObject(F("connection"));
		con[F("connected")] = WifiStation.isConnected();
		if(WifiStation.isConnected()) {
			con[F("ssid")] = WifiStation.getSSID();
			con[F("dhcp")] = WifiStation.isEnabledDHCP();
			con[F("ip")] = WifiStation.getIP().toString();
			con[F("netmask")] = WifiStation.getNetworkMask().toString();
			con[F("gateway")] = WifiStation.getNetworkGateway().toString();
			con[F("mac")] = WifiStation.getMAC();
			con[F("rssi")] = WifiStation.getRssi();

			JsonObject net = data.createNestedObject(F("network"));
			net[F("tcp_connections")] = tcpStats.active_total;
			net[F("tcp_active")] = tcpStats.active_total;
			net[F("tcp_established")] = tcpStats.established;
			net[F("tcp_syn_sent")] = tcpStats.syn_sent;
			net[F("tcp_syn_rcvd")] = tcpStats.syn_rcvd;
			net[F("tcp_fin_wait_1")] = tcpStats.fin_wait_1;
			net[F("tcp_fin_wait_2")] = tcpStats.fin_wait_2;
			net[F("tcp_close_wait")] = tcpStats.close_wait;
			net[F("tcp_closing")] = tcpStats.closing;
			net[F("tcp_last_ack")] = tcpStats.last_ack;
			net[F("tcp_time_wait")] = tcpStats.time_wait;
			net[F("tcp_closed")] = tcpStats.closed;
		}

		if(!app.ota.isProccessing()) {
			AppConfig::Network network(*app.cfg);
			JsonObject mqtt = data.createNestedObject(F("mqtt"));
			if(network.mqtt.getEnabled() && !app.mqttclient.isRunning()) {
				mqtt[F("status")] = F("configured but not running");
			} else if(network.mqtt.getEnabled() && app.mqttclient.isRunning()) {
				mqtt[F("status")] = F("running");
			} else {
				mqtt[F("status")] = F("disabled");
			}
			mqtt[F("enabled")] = network.mqtt.getEnabled();
			mqtt[F("broker")] = network.mqtt.getServer();
			mqtt[F("topic")] = network.mqtt.getTopicBase();

			if(network.mqtt.homeassistant.getEnable()) {
				JsonObject ha = data.createNestedObject(F("homeassistant"));
				ha[F("enabled")] = network.mqtt.homeassistant.getEnable();
				ha[F("discovery_prefix")] = network.mqtt.homeassistant.getDiscoveryPrefix();
				ha[F("Node ID")] = network.mqtt.homeassistant.getNodeId();
			}
		}

		if(app.ota.isProccessing()) {
			JsonObject ota = data.createNestedObject(F("ota"));
			ota[F("status")] = F("in progress");
		}

		return true;
	}

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
	data[F("webapp_version")] = WEBAPP_VERSION;
	data[F("sming")] = SMING_VERSION;
	data[F("event_num_clients")] = app.eventserver.activeClients;
	data[F("uptime")] = app.getUptime();
	data[F("heap_free")] = app.getFreeHeapSize();

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
