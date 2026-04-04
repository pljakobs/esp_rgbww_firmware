#include <RGBWWCtrl.h>
#include <arduinojson.h>
#include <SimpleTimer.h>


TelemetryClient::TelemetryClient() {
	snprintf(_chipId, TELEMETRY_CHIPID_MAX_SIZE, "%u", system_get_chip_id());
	mqtt = new MqttClient();
	snprintf(_id, TELEMETRY_ID_MAX_SIZE, "rgbww_%s", _chipId);
	_lastReconnectAttempt = 0;
	_reconnectPending = false;
}

void TelemetryClient::reconnectGateTimeoutCb(void* arg) {
	TelemetryClient* self = static_cast<TelemetryClient*>(arg);
	self->_reconnectPending = false;
}

TelemetryClient::~TelemetryClient() {
	if (mqtt) {
		delete mqtt;
		mqtt = nullptr;
	}
}

void TelemetryClient::start() {
	
	AppConfig::Network network(*app.cfg);
	strncpy(_telemetryURL, network.telemetry.getUrl().c_str(), TELEMETRY_URL_MAX_SIZE);
    _telemetryURL[TELEMETRY_URL_MAX_SIZE - 1] = '\0';
	strncpy(_telemetryUser, network.telemetry.getUser().c_str(), TELEMETRY_USER_MAX_SIZE);
    _telemetryUser[TELEMETRY_USER_MAX_SIZE - 1] = '\0';
	strncpy(_telemetryPass, network.telemetry.getPassword().c_str(), TELEMETRY_PASS_MAX_SIZE);
    _telemetryPass[TELEMETRY_PASS_MAX_SIZE - 1] = '\0';
	_telemetryStats=network.telemetry.getStatsEnabled();
	_telemetryLog=network.telemetry.getLogEnabled();

	if((_telemetryStats  or _telemetryLog ) && strlen(_telemetryURL) > 0){
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
	// Replace the MqttClient instance so the next connect() gets a fresh TCP PCB.
	// MqttClient::close() is inaccessible (protected base), so we recreate instead.
	delete mqtt;
	mqtt = new MqttClient();
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
		// If telemetry is enabled, try to reconnect with gating
		if ((_telemetryStats || _telemetryLog ) && !_reconnectPending) {
			unsigned long now = millis();
			if (now - _lastReconnectAttempt > 10000) { // 10s gate
				debug_i("TelemetryClient: attempting reconnect");
				_reconnectPending = true;
				_lastReconnectAttempt = now;
				reconnect();
				_reconnectGateTimer.initializeMs(10000, TelemetryClient::reconnectGateTimeoutCb, this).startOnce();
			}
		}
		return false;
	}
	char fullTopic[TELEMETRY_TOPIC_MAX_SIZE];
	buildTopic(topic, fullTopic, sizeof(fullTopic));
	String payload;
	serializeJson(doc, payload);
	debug_i("Telemetry MQTT publishing %s to topic: %s", payload.c_str(), fullTopic);
	return mqtt->publish(fullTopic, payload);
}
// Add to TelemetryClient class definition in telemetry.h:
// Timer _reconnectGateTimer;
// static void reconnectGateTimeoutCb(void* arg);
// Add to TelemetryClient class definition in telemetry.h:
// unsigned long _lastReconnectAttempt;
// bool _reconnectPending;

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
	if(!_telemetryStats ){
		return false; //stats disabled
	}
	return publish("monitor", doc);
}

bool TelemetryClient::log(const char* message) {
    if(!_telemetryLog ){
		return false; //logging disabled
	}
    return publish("log", message);
}

bool TelemetryClient::log(const String& message) {
    return log(message.c_str());
}