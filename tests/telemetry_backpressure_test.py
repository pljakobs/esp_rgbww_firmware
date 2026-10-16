from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest


CPP_SUPPORT = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
using String = std::string;
struct JsonDocument {};
size_t serializations = 0;
void serializeJson(const JsonDocument&, String& payload) { ++serializations; payload = "{}"; }
constexpr size_t TELEMETRY_CHIPID_MAX_SIZE = 16;
constexpr size_t TELEMETRY_ID_MAX_SIZE = 32;
constexpr size_t TELEMETRY_TOPIC_MAX_SIZE = 128;
constexpr int MQTT_TYPE_PUBACK = 4;
constexpr int MQTT_QOS_AT_LEAST_ONCE = 1;
enum TcpClientState { eTCS_Ready, eTCS_Connected };
struct TcpClient {};
struct mqtt_message_t {
    struct { int type = MQTT_TYPE_PUBACK; } common;
    struct { int return_code = 0; } connack;
};
uint32_t nowMs = 100;
uint32_t millis() { return nowMs; }
template<typename Owner>
auto TimerDelegate(void (Owner::*method)(), Owner* owner) {
    return [owner, method]() { (owner->*method)(); };
}
struct Timer {
    bool armed = false;
    uint32_t delay = 0;
    std::function<void()> callback;
    Timer& initializeMs(uint32_t value, std::function<void()> handler) {
        delay = value; callback = std::move(handler); return *this;
    }
    template<uint32_t Delay> Timer& initializeMs(std::function<void()> handler) {
        return initializeMs(Delay, std::move(handler));
    }
    void startOnce() { armed = true; }
    void stop() { armed = false; }
    void fire() { assert(armed); armed = false; callback(); }
};
struct MqttClient : TcpClient {
    static int alive;
    size_t publishes = 0;
    bool accepts = true;
    uint8_t lastFlags = 0;
    TcpClientState state = eTCS_Connected;
    std::function<void()> synchronousAck;
    MqttClient() { ++alive; }
    ~MqttClient() { --alive; }
    TcpClientState getConnectionState() { return state; }
    static uint8_t getFlags(int qos) { return qos << 1; }
    bool publish(const char*, const char*, uint8_t flags) {
        ++publishes; lastFlags = flags;
        if(synchronousAck) synchronousAck();
        return accepts;
    }
};
int MqttClient::alive = 0;
struct {
    size_t heap = 32000;
    size_t checks = 0;
    bool checkHeap(uint32_t threshold) { ++checks; return heap >= threshold; }
} app;
#define cdebug_i(...) ((void)0)
#define cdebug_w(...) ((void)0)
"""

CPP_TESTS = r"""
TelemetryClient::TelemetryClient() {
    mqtt = new MqttClient();
    std::strcpy(_chipId, "1234");
}
TelemetryClient::~TelemetryClient() { delete mqtt; }
void TelemetryClient::doReconnect() {}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    {
        TelemetryClient client;
        client._telemetryStats = true;
        client._isRunning = true;
        mqtt_message_t ack;
        if(scenario == "ack_gate") {
            assert(client.publishPayload("monitor", "first"));
            assert(client._publishPending && !client.canPublishStats());
            assert(client.mqtt->lastFlags == 2 && client._publishTimer.delay == 15000);
            for(size_t index = 0; index < 100; ++index) {
                assert(!client.publishPayload("log", "drop"));
            }
            assert(client.mqtt->publishes == 1);
            client.onPublished(*client.mqtt, &ack);
            assert(!client._publishPending && !client._publishTimer.armed);
            assert(client.canPublishStats());
            assert(client.publishPayload("monitor", "second"));
            assert(client.mqtt->publishes == 2);
        } else if(scenario == "disconnected") {
            client.mqtt->state = eTCS_Ready;
            assert(!client.publishPayload("monitor", "drop"));
            assert(client.mqtt->publishes == 0 && !client.isRunning());
            assert(client._reconnectPending && client._reconnectTimer.armed);
        } else if(scenario == "timeout") {
            assert(client.publishPayload("monitor", "first"));
            nowMs += 15000;
            client._publishTimer.fire();
            assert(!client.isRunning() && !client._publishPending && !client.mqtt);
            assert(MqttClient::alive == 0);
            assert(client._reconnectPending && client._reconnectTimer.armed);
        } else if(scenario == "rejected_publish") {
            client.mqtt->accepts = false;
            assert(!client.publishPayload("monitor", "drop"));
            assert(!client.isRunning() && !client._publishPending && !client._publishTimer.armed);
            assert(client._reconnectTimer.armed);
        } else if(scenario == "low_heap") {
            app.heap = 8192;
            assert(!client.publishPayload("monitor", "drop"));
            assert(app.checks == 1 && client.mqtt->publishes == 0 && !client._publishPending);
        } else if(scenario == "broker_rejection") {
            ack.connack.return_code = 5;
            client.onConnected(*client.mqtt, &ack);
            assert(!client.isRunning() && client._reconnectTimer.armed);
        } else if(scenario == "backoff") {
            client._lastReconnectAttempt = 100;
            nowMs = 1100;
            client.requestReconnect("test");
            assert(client._reconnectTimer.armed && client._reconnectTimer.delay == 9000);
            assert(client._reconnectBackoffMs == 20000);
            client.requestReconnect("duplicate");
            assert(client._reconnectBackoffMs == 20000);
        } else if(scenario == "disconnect_success") {
            client.onComplete(*client.mqtt, true);
            assert(!client.isRunning() && client._reconnectTimer.armed);
        } else if(scenario == "serialization_gate") {
            JsonDocument doc;
            client._isRunning = false;
            assert(!client.publish("monitor", doc) && serializations == 0);
            client._isRunning = true;
            client._publishPending = true;
            assert(!client.publish("monitor", doc) && serializations == 0);
        } else if(scenario == "disabled_stats") {
            client._telemetryStats = false;
            assert(!client.stat(String("disabled")) && client.mqtt->publishes == 0);
        } else if(scenario == "synchronous_ack") {
            client.mqtt->synchronousAck = [&]() { client.onPublished(*client.mqtt, &ack); };
            assert(client.publishPayload("monitor", "first"));
            assert(!client._publishPending && !client._publishTimer.armed);
        } else {
            return 2;
        }
        client.stop();
    }
    assert(MqttClient::alive == 0);
}
"""


@pytest.fixture(scope="module")
def telemetry_binary(tmp_path_factory: pytest.TempPathFactory) -> Path:
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is required for isolated telemetry backpressure tests")
    root = Path(__file__).resolve().parents[1]
    header = (root / "include/telemetry.h").read_text()
    declaration = header[header.index("class TelemetryClient {"):].replace("private:", "public:")
    source = (root / "app/telemetry.cpp").read_text()
    signatures = [
        "void TelemetryClient::stop()",
        "void TelemetryClient::onComplete(",
        "int TelemetryClient::onConnected(",
        "int TelemetryClient::onPublished(",
        "void TelemetryClient::onPublishTimeout()",
        "void TelemetryClient::buildTopic(",
        "void TelemetryClient::requestReconnect(",
        "bool TelemetryClient::publishPayload(",
        "bool TelemetryClient::publish(const char* topic, const JsonDocument& doc)",
        "bool TelemetryClient::publish(const char* topic, const char* payload)",
        "bool TelemetryClient::publish(const String& topic, const String& payload)",
        "bool TelemetryClient::stat(const String& payload)",
    ]
    methods = []
    for signature in signatures:
        start = source.index(signature)
        end = source.index("\n}", start) + 2
        methods.append(source[start:end])
    binary = tmp_path_factory.mktemp("telemetry-backpressure") / "test"
    result = subprocess.run(
        [compiler, "-std=c++17", "-D_GLIBCXX_ASSERTIONS", "-x", "c++", "-", "-o", str(binary)],
        input=CPP_SUPPORT + declaration + "\n".join(methods) + CPP_TESTS,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


@pytest.mark.parametrize(
    "scenario",
    [
        "ack_gate", "disconnected", "timeout", "rejected_publish", "low_heap",
        "broker_rejection", "backoff", "disconnect_success", "serialization_gate",
        "disabled_stats", "synchronous_ack",
    ],
)
def test_telemetry_backpressure(telemetry_binary: Path, scenario: str) -> None:
    result = subprocess.run(
        [str(telemetry_binary), scenario],
        text=True,
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr