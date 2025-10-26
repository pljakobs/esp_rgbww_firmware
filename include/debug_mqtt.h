#pragma once

class DebugMqttClient {
public:
    DebugMqttClient();
    virtual ~DebugMqttClient();

    void start(String debugServer, String debugUser, String debugPass);
    
    void stop();
    bool publish(const String& topic, const JsonDocument& doc);
    bool publish(const String& topic, const String& payload);
    void connect(String debugServer, String debugUser, String debugPass);
    void reconnect();
private:
    void onComplete(TcpClient& client, bool success);
    int onConnected(MqttClient& client, mqtt_message_t* message);
    int onMessageReceived(MqttClient& client, mqtt_message_t* message);

    String buildTopic(const String& suffix);

    String _debugServer, _debugUser, _debugPass;
    bool _isRunning = false;
    MqttClient* mqtt = nullptr;
    String _chipId;
    String _id;
};