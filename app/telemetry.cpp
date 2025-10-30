#include <RGBWWCtrl.h>

using telemetryStats = AppConfig::ContainedRoot::telemetryStats;
using telemetryLog = AppConfig::ContainedRoot::telemetryLog;

TelemetryClient::TelemetryClient() {
	_chipId = String(system_get_chip_id());
	mqtt = new MqttClient();
    _id = String("rgbww_") + system_get_chip_id();
}

TelemetryClient::~TelemetryClient() {
	if (mqtt) {
		delete mqtt;
		mqtt = nullptr;
	}
}

void TelemetryClient::start() {
	
	AppConfig::Root::Telemetry telemetryCfg(*app.cfg);
	_telemetryURL=telemetryCfg.getUrl();
	_telemetryUser=telemetryCfg.getUser();
	_telemetryPass=telemetryCfg.getPassword();
	_telemetryStats=telemetryCfg.getStatsEnabled();
	_telemetryLog=telemetryCfg.getLogEnabled();

	// set defaults if undefined
	// this will only happen once upon first start

	String _buildType=BUILD_TYPE;

	//for this one version, set full debug to ON to allow troubleshooting
	_telemetryStats = telemetryStats::ON; //enable stats in debug builds by default
	_telemetryLog = telemetryLog::ON; //enable log in debug builds by default

/*
	if(_telemetryStats == telemetryStats::UNDEF and _buildType == "debug"){
		debug_i("TelemetryClient::start - enabling telemetry stats by default for debug build");
		_telemetryStats = telemetryStats::ON; //enable stats in debug builds by default
	}else if(_telemetryStats == telemetryStats::UNDEF){
		debug_i("TelemetryClient::start - disabling telemetry stats by default for release build");
		_telemetryStats = telemetryStats::OFF; //disable stats in release builds by default
	}

	if(_telemetryLog == telemetryLog::UNDEF){
		_telemetryLog = telemetryLog::OFF; //disable log by default
	}
*/
	{
	auto telemetryUpdate=telemetryCfg.update();
	telemetryUpdate.setStatsEnabled(_telemetryStats);
	telemetryUpdate.setLogEnabled(_telemetryLog);
	}

	if(_telemetryStats == telemetryStats::ON or _telemetryLog == telemetryLog::ON){
		debug_i("Application::startServices - starting remote telemetry");

		debug_i("Application::startServices - telemetry mqtt server: %s", _telemetryURL.c_str());
		connect(_telemetryURL, _telemetryUser, _telemetryPass);
	}
	else {
		debug_i("Application::startServices - mqtt telemetry disabled");
		stop();
	}
}

void TelemetryClient::stop() {
	_isRunning = false;
}

void TelemetryClient::connect(String telemetryURL, String telemetryUser, String telemetryPass) {
	// Connect to public MQTT server (example: test.mosquitto.org)

	if(telemetryURL.length()>0 && telemetryUser.length()>0 && telemetryPass.length()>0){
		// Build URL: mqtt://user:pass@server:port
		String url = "mqtt://" + telemetryUser + ":" + telemetryPass + "@" + telemetryURL;
        debug_i("Telemetry MQTT connecting to %s", url.c_str());
		mqtt->connect(url, "telemetry_client_" + _chipId);
		mqtt->setCompleteDelegate([this](TcpClient& client, bool success) { this->onComplete(client, success); });
		mqtt->setConnectedHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onConnected(client, message); });
		mqtt->setMessageHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onMessageReceived(client, message); });
	} else {
		Serial.println("Telemetry MQTT not configured properly");
	}
}

void TelemetryClient::reconnect() {
    stop();
    delay(1000); // brief delay before reconnecting
    connect(_telemetryURL, _telemetryUser, _telemetryPass);
 }

void TelemetryClient::onComplete(TcpClient& client, bool success) {
	if (!success) {
		Serial.println("Telemetry MQTT connection failed");
		_isRunning = false;
	}
}

int TelemetryClient::onConnected(MqttClient& client, mqtt_message_t* message) {
	Serial.println("Telemetry MQTT connected");
    _isRunning = true;
	return 0;
}

int TelemetryClient::onMessageReceived(MqttClient& client, mqtt_message_t* message) {
	// Not used for publishing only
	return 0;
}

String TelemetryClient::buildTopic(const String& suffix) {
    String topic = String("rgbww/") + _chipId + "/" + suffix;
    topic.trim();
    return topic;
}

bool TelemetryClient::publish(const String& topic, const JsonDocument& doc) {
	//if (!_isRunning || !mqtt || mqtt->getConnectionState() != eTCS_Connected) {
    if (!_isRunning) {
		Serial.println("Telemetry MQTT not connected");
		return false;
	}
	String fullTopic = buildTopic(topic);
	String payload;
	serializeJson(doc, payload);
    debug_i("Telemetry MQTT publishing %s to topic: %s", payload.c_str(), fullTopic.c_str());
	return mqtt->publish(fullTopic, payload);
}

bool TelemetryClient::publish(const String& topic, const String& payload) {
    if (!_isRunning || !mqtt || mqtt->getConnectionState() != eTCS_Connected) {
        Serial.println("Telemetry MQTT not connected");
        return false;
    }
    debug_i("Telemetry MQTT publishing %s to topic: %s", payload.c_str(), topic.c_str());
    String fullTopic = buildTopic(topic);
    return mqtt->publish(fullTopic, payload);
}
bool TelemetryClient::stat(const JsonDocument& doc) {
	if(_telemetryStats != telemetryStats::ON){
		return false; //stats disabled
	}
	return publish(F("monitor"), doc);
}

bool TelemetryClient::log(const String& message) {
    if(_telemetryLog != telemetryLog::ON){
		return false; //logging disabled
	}
    return publish(F("log"), message);
}