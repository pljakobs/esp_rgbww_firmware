#include <apihandler.h>

#include <application.h>

#if defined(ARCH_ESP8266) || defined(ARCH_ESP32)
extern "C" {
#include <lwip/tcp.h>
}
#endif

namespace {
struct TcpPcbStats
{
	uint16_t active_total{0};
	uint16_t established{0};
	uint16_t syn_sent{0};
	uint16_t syn_rcvd{0};
	uint16_t fin_wait_1{0};
	uint16_t fin_wait_2{0};
	uint16_t close_wait{0};
	uint16_t closing{0};
	uint16_t last_ack{0};
	uint16_t time_wait{0};
	uint16_t closed{0};
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
	if(method == F("info") || method == F("getInfo")) {
		return handleInfo(params, out);
	}

	out[F("error")] = F("method not implemented");
	out[F("method")] = method;
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
