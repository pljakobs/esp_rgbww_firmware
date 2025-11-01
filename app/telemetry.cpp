#include <RGBWWCtrl.h>
#include <arduinojson.h>

using telemetryStats = AppConfig::ContainedRoot::telemetryStats;
using telemetryLog = AppConfig::ContainedRoot::telemetryLog;

TelemetryClient::TelemetryClient() {
	snprintf(_chipId, TELEMETRY_CHIPID_MAX_SIZE, "%u", system_get_chip_id());
	mqtt = new MqttClient();
    snprintf(_id, TELEMETRY_ID_MAX_SIZE, "rgbww_%s", _chipId);
}

TelemetryClient::~TelemetryClient() {
	if (mqtt) {
		delete mqtt;
		mqtt = nullptr;
	}
}

void TelemetryClient::start() {
	
	AppConfig::Root::Telemetry telemetryCfg(*app.cfg);
	strncpy(_telemetryURL, telemetryCfg.getUrl().c_str(), TELEMETRY_URL_MAX_SIZE);
    _telemetryURL[TELEMETRY_URL_MAX_SIZE - 1] = '\0';
	strncpy(_telemetryUser, telemetryCfg.getUser().c_str(), TELEMETRY_USER_MAX_SIZE);
    _telemetryUser[TELEMETRY_USER_MAX_SIZE - 1] = '\0';
	strncpy(_telemetryPass, telemetryCfg.getPassword().c_str(), TELEMETRY_PASS_MAX_SIZE);
    _telemetryPass[TELEMETRY_PASS_MAX_SIZE - 1] = '\0';
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

	if((_telemetryStats == telemetryStats::ON or _telemetryLog == telemetryLog::ON) && strlen(_telemetryURL) > 0){
		debug_i("Application::startServices - starting remote telemetry");

		debug_i("Application::startServices - telemetry mqtt server: %s", _telemetryURL);
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

void TelemetryClient::connect(const char* telemetryURL, const char* telemetryUser, const char* telemetryPass) {
	// Connect to public MQTT server (example: test.mosquitto.org)

	if(strlen(telemetryURL)>0 && strlen(telemetryUser)>0 && strlen(telemetryPass)>0){
		// Build URL: mqtt://user:pass@server:port
		char url[256];
        snprintf(url, sizeof(url), "mqtt://%s:%s@%s", telemetryUser, telemetryPass, telemetryURL);
        debug_i("Telemetry MQTT connecting to %s", url);
        char clientId[64];
        snprintf(clientId, sizeof(clientId), "telemetry_client_%s", _chipId);
		mqtt->connect(url, clientId);
		mqtt->setCompleteDelegate([this](TcpClient& client, bool success) { this->onComplete(client, success); });
		mqtt->setConnectedHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onConnected(client, message); });
		mqtt->setMessageHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onMessageReceived(client, message); });
	} else {
		Serial.println("Telemetry MQTT not configured properly");
	}
}

void TelemetryClient::connect(const String& telemetryURL, const String& telemetryUser, const String& telemetryPass) {
    connect(telemetryURL.c_str(), telemetryUser.c_str(), telemetryPass.c_str());
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

void TelemetryClient::buildTopic(const char* suffix, char* dest, size_t size) {
    snprintf(dest, size, "rgbww/%s/%s", _chipId, suffix);
    dest[size - 1] = '\0';
}

bool TelemetryClient::publish(const char* topic, const JsonDocument& doc) {
    if (!_isRunning || !mqtt || mqtt->getConnectionState() != eTCS_Connected) {
        // Serial.println("Telemetry MQTT not connected");
        return false;
    }
	char fullTopic[TELEMETRY_TOPIC_MAX_SIZE];
    buildTopic(topic, fullTopic, sizeof(fullTopic));
	String payload;
	serializeJson(doc, payload);
    debug_i("Telemetry MQTT publishing %s to topic: %s", payload.c_str(), fullTopic);
	return mqtt->publish(fullTopic, payload);
}

bool TelemetryClient::publish(const String& topic, const JsonDocument& doc) {
    return publish(topic.c_str(), doc);
}

bool TelemetryClient::publish(const char* topic, const char* payload) {
    if (!_isRunning || !mqtt || mqtt->getConnectionState() != eTCS_Connected) {
        // Serial.println("Telemetry MQTT not connected");
        return false;
    }
    debug_i("Telemetry MQTT publishing %s to topic: %s", payload, topic);
    char fullTopic[TELEMETRY_TOPIC_MAX_SIZE];
    buildTopic(topic, fullTopic, sizeof(fullTopic));
    return mqtt->publish(fullTopic, payload);
}

bool TelemetryClient::publish(const String& topic, const String& payload) {
    return publish(topic.c_str(), payload.c_str());
}
bool TelemetryClient::stat(const JsonDocument& doc) {
	if(_telemetryStats != telemetryStats::ON){
		return false; //stats disabled
	}
	return publish("monitor", doc);
}

bool TelemetryClient::log(const char* message) {
    if(_telemetryLog != telemetryLog::ON){
		return false; //logging disabled
	}
    return publish("log", message);
}

bool TelemetryClient::log(const String& message) {
    return log(message.c_str());
}