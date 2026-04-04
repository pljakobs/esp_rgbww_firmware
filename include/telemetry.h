/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 * 
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
 *
 */

#pragma once
#include "app-config.h"
#include <ArduinoJson.h>
#include <Network/MqttClient.h> // Include for MqttClient, TcpClient, and mqtt_message_t

#define TELEMETRY_URL_MAX_SIZE 128
#define TELEMETRY_USER_MAX_SIZE 64
#define TELEMETRY_PASS_MAX_SIZE 64
#define TELEMETRY_CHIPID_MAX_SIZE 16
#define TELEMETRY_ID_MAX_SIZE 32
#define TELEMETRY_TOPIC_MAX_SIZE 128

class TelemetryClient {
public:
    TelemetryClient();
    virtual ~TelemetryClient();

    void start();
    void stop();
    bool stat(const JsonDocument& doc);
    bool log(const char* message);
    bool log(const String& message);
    void connect(const char* debugServer, const char* debugUser, const char* debugPass);
    void connect(const String& debugServer, const String& debugUser, const String& debugPass);
    void reconnect();
    bool isRunning() const { return _isRunning; }
private:
    void onComplete(TcpClient& client, bool success);
    int onConnected(MqttClient& client, mqtt_message_t* message);
    int onMessageReceived(MqttClient& client, mqtt_message_t* message);
    bool publish(const char* topic, const JsonDocument& doc);
    bool publish(const String& topic, const JsonDocument& doc);
    bool publish(const char* topic, const char* payload);
    bool publish(const String& topic, const String& payload);

    void buildTopic(const char* suffix, char* dest, size_t size);

    char _telemetryURL[TELEMETRY_URL_MAX_SIZE];
    char _telemetryUser[TELEMETRY_USER_MAX_SIZE];
    char _telemetryPass[TELEMETRY_PASS_MAX_SIZE];
    bool _telemetryStats;
    bool _telemetryLog;

    int _lastReconnectAttempt = 0;
    bool _reconnectPending = false;
    SimpleTimer _reconnectGateTimer;
    static void reconnectGateTimeoutCb(void* arg);

    bool _isRunning = false;
    MqttClient* mqtt = nullptr;
    char _chipId[TELEMETRY_CHIPID_MAX_SIZE];
    char _id[TELEMETRY_ID_MAX_SIZE];
};