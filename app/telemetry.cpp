
/**
 * @file
 * @author  Peter Jakobs http://github.com/pljakobs
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
	String telemetryURL = network.telemetry.getUrl();
	String telemetryUser = network.telemetry.getUser();
	String telemetryPass = network.telemetry.getPassword();
	_telemetryStats=network.telemetry.getStatsEnabled();
	_telemetryLog=network.telemetry.getLogEnabled();

	if((_telemetryStats  or _telemetryLog ) && telemetryURL.length() > 0){
		debug_i(ANSI_COLOR_BLUE "Application::startServices - starting remote telemetry" ANSI_COLOR_RESET);

		debug_i(ANSI_COLOR_BLUE "Application::startServices - telemetry mqtt server: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, telemetryURL.c_str());
		connect(telemetryURL, telemetryUser, telemetryPass);
	}
	else {
		debug_i(ANSI_COLOR_BLUE "Application::startServices - mqtt telemetry disabled" ANSI_COLOR_RESET);
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
        debug_i(ANSI_COLOR_BLUE "Telemetry MQTT connecting to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, url);
        char clientId[64];
        snprintf(clientId, sizeof(clientId), "telemetry_client_%s", _chipId);
		mqtt->connect(url, clientId);
		mqtt->setCompleteDelegate([this](TcpClient& client, bool success) { this->onComplete(client, success); });
		mqtt->setConnectedHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onConnected(client, message); });
		mqtt->setMessageHandler([this](MqttClient& client, mqtt_message_t* message) { return this->onMessageReceived(client, message); });
	} else {
		debug_i(ANSI_COLOR_BLUE "Telemetry MQTT not configured properly" ANSI_COLOR_RESET);
	}
}

void TelemetryClient::connect(const String& telemetryURL, const String& telemetryUser, const String& telemetryPass) {
    connect(telemetryURL.c_str(), telemetryUser.c_str(), telemetryPass.c_str());
}

void TelemetryClient::reconnect() {
    // Schedule the actual stop+connect from the top of the event loop via a
    // 1-second timer.  Calling stop() (which deletes the MqttClient) and then
    // delay(1000) here would yield the event loop while stale TCP callbacks
    // still reference the deleted object → use-after-free crash.
    _reconnectTimer.initializeMs<1000>(TimerDelegate(&TelemetryClient::doReconnect, this)).startOnce();
}

void TelemetryClient::doReconnect() {
    stop();
    AppConfig::Network network(*app.cfg);
    connect(network.telemetry.getUrl(), network.telemetry.getUser(), network.telemetry.getPassword());
}

void TelemetryClient::onComplete(TcpClient& client, bool success) {
	if (!success) {
		debug_i(ANSI_COLOR_BLUE "Telemetry MQTT connection failed" ANSI_COLOR_RESET);
		_isRunning = false;
	}
}

int TelemetryClient::onConnected(MqttClient& client, mqtt_message_t* message) {
	debug_i(ANSI_COLOR_BLUE "Telemetry MQTT connected" ANSI_COLOR_RESET);
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
				debug_i(ANSI_COLOR_BLUE "TelemetryClient: attempting reconnect" ANSI_COLOR_RESET);
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
	debug_i(ANSI_COLOR_BLUE "Telemetry MQTT publishing " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " to topic: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, payload.c_str(), fullTopic);
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
        return false;
    }
    debug_i(ANSI_COLOR_BLUE "Telemetry MQTT publishing " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " to topic: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, payload, topic);
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