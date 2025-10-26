#include <RGBWWCtrl.h>

DebugMqttClient::DebugMqttClient() {
	_chipId = String(system_get_chip_id());
	mqtt = new MqttClient();
    _id = String("rgbww_") + system_get_chip_id();
}

DebugMqttClient::~DebugMqttClient() {
	if (mqtt) {
		delete mqtt;
		mqtt = nullptr;
	}
}

void DebugMqttClient::start(String debugServer, String debugUser, String debugPass) {
    _debugServer=debugServer;
    _debugUser=debugUser;
    _debugPass=debugPass;
    connect(debugServer, debugUser, debugPass);
	_isRunning = true;
}

void DebugMqttClient::stop() {
	_isRunning = false;
}

void DebugMqttClient::connect(String debugServer, String debugUser, String debugPass) {
	// Connect to public MQTT server (example: test.mosquitto.org)

	if(debugServer.length()>0 && debugUser.length()>0 && debugPass.length()>0){
		// Build URL: mqtt://user:pass@server:port
		String url = "mqtt://" + debugUser + ":" + debugPass + "@" + debugServer;
        debug_i("Debug MQTT connecting to %s", url.c_str());
		mqtt->connect(url, "debug_client_" + _chipId);
		mqtt->setCompleteDelegate([this](TcpClient& client, bool success) { this->onComplete(client, success); });
		mqtt->setConnectedHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onConnected(client, message); });
		mqtt->setMessageHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onMessageReceived(client, message); });
	} else {
		Serial.println("Debug MQTT not configured properly");
	}
}

void DebugMqttClient::reconnect() {
    stop();
    delay(1000); // brief delay before reconnecting
    connect(_debugServer, _debugUser, _debugPass);
    _isRunning = true;
}

void DebugMqttClient::onComplete(TcpClient& client, bool success) {
	if (!success) {
		Serial.println("Debug MQTT connection failed");
		_isRunning = false;
	}
}

int DebugMqttClient::onConnected(MqttClient& client, mqtt_message_t* message) {
	Serial.println("Debug MQTT connected");
	return 0;
}

int DebugMqttClient::onMessageReceived(MqttClient& client, mqtt_message_t* message) {
	// Not used for publishing only
	return 0;
}

String DebugMqttClient::buildTopic(const String& suffix) {
    String topic = String("rgbww/") + _chipId + "/" + suffix;
    topic.trim();
    return topic;
}

bool DebugMqttClient::publish(const String& topic, const JsonDocument& doc) {
	if (!_isRunning || !mqtt || mqtt->getConnectionState() != eTCS_Connected) {
		Serial.println("Debug MQTT not connected");
		return false;
	}
	String fullTopic = buildTopic(topic);
	String payload;
	serializeJson(doc, payload);
    debug_i("Debug MQTT publishing %s to topic: %s", payload.c_str(), fullTopic.c_str());
	return mqtt->publish(fullTopic, payload);
}

bool DebugMqttClient::publish(const String& topic, const String& payload) {
    if (!_isRunning || !mqtt || mqtt->getConnectionState() != eTCS_Connected) {
        Serial.println("Debug MQTT not connected");
        return false;
    }
    debug_i("Debug MQTT publishing %s to topic: %s", payload.c_str(), topic.c_str());
    String fullTopic = buildTopic(topic);
    return mqtt->publish(fullTopic, payload);
}