
#pragma once
#include "app-config.h"

using telemetryStats = AppConfig::ContainedRoot::telemetryStats;
using telemetryLog = AppConfig::ContainedRoot::telemetryLog;

class TelemetryClient {
public:
    TelemetryClient();
    virtual ~TelemetryClient();

    void start();
    void stop();
    bool stat( const JsonDocument& doc);
    bool log(const String& message);
    void connect(String debugServer, String debugUser, String debugPass);
    void reconnect();
    bool isRunning() const { return _isRunning; }
private:
    void onComplete(TcpClient& client, bool success);
    int onConnected(MqttClient& client, mqtt_message_t* message);
    int onMessageReceived(MqttClient& client, mqtt_message_t* message);
    bool publish(const String& topic, const JsonDocument& doc);
    bool publish(const String& topic, const String& payload);

    String buildTopic(const String& suffix);

    String _telemetryURL, _telemetryUser, _telemetryPass;
    telemetryStats _telemetryStats;
    telemetryLog _telemetryLog;

    bool _isRunning = false;
    MqttClient* mqtt = nullptr;
    String _chipId;
    String _id;

};