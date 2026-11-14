from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest


CPP_SUPPORT = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#define F(value) value
#define HAS_INA219_EXTENSION 1
#define FADE_TIMING_ONLY(...)

class String : public std::string {
public:
    using std::string::string;
    bool concat(const char* data, size_t length) { append(data, length); return true; }
};

struct IDataSourceStream {
    static int alive;
    IDataSourceStream() { ++alive; }
    virtual ~IDataSourceStream() { --alive; }
};
int IDataSourceStream::alive = 0;

enum { eWSCS_Open, eWSCS_Closed };

struct WebsocketConnection {
    void* state = nullptr;
    bool open = true;
    bool disconnectOnSend = false;
    size_t closes = 0;
    std::vector<std::string> sent;
    void* getUserData() { return state; }
    int getState() const { return open ? eWSCS_Open : eWSCS_Closed; }
    bool send(IDataSourceStream* stream) { delete stream; return true; }
    bool sendString(const String& payload) {
        sent.emplace_back(payload);
        if(disconnectOnSend) close();
        return true;
    }
    void close();
};

struct RpcCodec {
    struct StreamTask {};
    bool disconnectOnStart = false;
    std::weak_ptr<StreamTask> lastTask;
    std::deque<std::function<void(bool)>> completions;
    std::shared_ptr<StreamTask> sendWebsocketStream(
        WebsocketConnection& socket, std::unique_ptr<IDataSourceStream> source,
        std::function<void(bool)> callback) {
        auto task = std::make_shared<StreamTask>();
        lastTask = task;
        if(disconnectOnStart) socket.close();
        else completions.push_back(std::move(callback));
        return task;
    }
} codec;
RpcCodec& rpcCodec() { return codec; }
"""

CPP_LIGHT_INTERFACE = r"""
struct ApplicationWebserver {
    std::vector<WebsocketConnection*> webSockets;
    bool hasLightSubscribers() const;
    bool hasRuntimeInfoSubscribers() const;
    void wsSendLightEvent(const char*, size_t, uint32_t = 0);
    void wsSendIna219(const char*, size_t);
};
"""

CPP_TESTS = r"""
void WebsocketConnection::close() {
    ++closes;
    open = false;
    delete static_cast<WsAuthState*>(state);
    state = nullptr;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    WebsocketConnection socket;
    socket.state = new WsAuthState();
    auto* state = static_cast<WsAuthState*>(socket.state);
    static_assert(std::tuple_size<decltype(state->pendingOutput)>::value == 4);

    if(scenario == "fifo") {
        size_t expectedCount = 0;
        for(size_t round = 0; round < 100; ++round) {
            state->activeStream = std::make_shared<RpcCodec::StreamTask>();
            for(size_t index = 0; index < 4; ++index) {
                const auto text = std::to_string(round * 4 + index);
                queueWsPayload(socket, text.data(), text.size());
            }
            assert(state->pendingCount == 4);
            WsQueuedOutput dropped;
            assert(!queueWsOutput(socket, std::move(dropped), false));
            assert(state->pendingCount == 4 && socket.closes == 0);
            state->activeStream.reset();
            flushWsOutput(socket);
            assert(state->pendingCount == 0);
            for(size_t index = 0; index < 4; ++index) {
                assert(socket.sent[expectedCount] == std::to_string(expectedCount));
                ++expectedCount;
            }
        }
        assert(socket.sent.size() == 400);
    } else if(scenario == "overflow") {
        state->activeStream = std::make_shared<RpcCodec::StreamTask>();
        for(size_t index = 0; index < 4; ++index) {
            queueWsStream(socket, std::make_unique<IDataSourceStream>());
        }
        assert(IDataSourceStream::alive == 4);
        queueWsStream(socket, std::make_unique<IDataSourceStream>());
        assert(!socket.state && socket.closes == 1);
    } else if(scenario == "stream_completion") {
        state->activeStream = std::make_shared<RpcCodec::StreamTask>();
        for(size_t index = 0; index < 4; ++index) {
            queueWsStream(socket, std::make_unique<IDataSourceStream>());
        }
        state->activeStream.reset();
        flushWsOutput(socket);
        size_t completed = 0;
        while(!codec.completions.empty()) {
            auto callback = std::move(codec.completions.front());
            codec.completions.pop_front();
            callback(true);
            ++completed;
        }
        assert(completed == 4 && state->pendingCount == 0 && !state->activeStream);
    } else if(scenario == "disconnect_text") {
        socket.disconnectOnSend = true;
        state->activeStream = std::make_shared<RpcCodec::StreamTask>();
        queueWsPayload(socket, "first", 5);
        queueWsPayload(socket, "second", 6);
        state->activeStream.reset();
        flushWsOutput(socket);
        assert(!socket.state && socket.closes == 1 && socket.sent.size() == 1);
    } else if(scenario == "disconnect_start") {
        codec.disconnectOnStart = true;
        queueWsStream(socket, std::make_unique<IDataSourceStream>());
        assert(!socket.state && socket.closes == 1 && codec.lastTask.expired());
    } else if(scenario == "late_completion") {
        queueWsStream(socket, std::make_unique<IDataSourceStream>());
        socket.close();
        auto callback = std::move(codec.completions.front());
        codec.completions.pop_front();
        callback(false);
        assert(!socket.state && socket.closes == 1);
    } else if(scenario == "runtime_subscription") {
        ApplicationWebserver server;
        server.webSockets = {nullptr, &socket};
        assert(!server.hasRuntimeInfoSubscribers());
        state->lightsSubscribed = true;
        assert(!server.hasRuntimeInfoSubscribers());
        state->runtimeInfoSubscribed = true;
        assert(server.hasRuntimeInfoSubscribers());
        socket.open = false;
        assert(!server.hasRuntimeInfoSubscribers());
        socket.open = true;
        state->runtimeInfoSubscribed = false;
        assert(!server.hasRuntimeInfoSubscribers());
        socket.close();
        assert(!server.hasRuntimeInfoSubscribers());
    } else if(scenario == "light_subscription") {
        WebsocketConnection runtime;
        runtime.state = new WsAuthState();
        static_cast<WsAuthState*>(runtime.state)->runtimeInfoSubscribed = true;
        ApplicationWebserver server;
        server.webSockets = {nullptr, &socket, &runtime};
        assert(!server.hasLightSubscribers());
        state->lightsSubscribed = true;
        assert(server.hasLightSubscribers());
        server.wsSendLightEvent("light-3", 7);
        server.wsSendLightEvent("light-7", 7);
        assert(socket.sent == std::vector<std::string>({"light-3", "light-7"}));
        assert(runtime.sent.empty());
        state->lightsSubscribed = false;
        assert(!server.hasLightSubscribers());
        server.wsSendLightEvent("unsubscribed", 12);
        assert(socket.sent.size() == 2);
        state->lightsSubscribed = true;
        socket.close();
        assert(!server.hasLightSubscribers());
        server.wsSendLightEvent("closed", 6);
        assert(socket.sent.size() == 2);
        runtime.close();
    } else if(scenario == "current_subscription") {
        WebsocketConnection other;
        other.state = new WsAuthState();
        ApplicationWebserver server;
        server.webSockets = {nullptr, &socket, &other};
        state->currentSubscribed = true;
        server.wsSendIna219("reading", 7);
        assert(socket.sent == std::vector<std::string>({"reading"}));
        assert(other.sent.empty());
        state->currentSubscribed = false;
        static_cast<WsAuthState*>(other.state)->currentSubscribed = true;
        server.wsSendIna219("invalid", 7);
        assert(socket.sent.size() == 1);
        assert(other.sent == std::vector<std::string>({"invalid"}));
        other.close();
        server.wsSendIna219("closed", 6);
        assert(other.sent.size() == 1);
    } else {
        return 2;
    }
    if(socket.state) socket.close();
    assert(IDataSourceStream::alive == 0);
}
"""


@pytest.fixture(scope="module")
def websocket_output_binary(tmp_path_factory: pytest.TempPathFactory) -> Path:
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is required for isolated WebSocket output tests")
    root = Path(__file__).resolve().parents[1]
    source = (root / "app/webserver.cpp").read_text()
    start = source.index("constexpr size_t WS_MAX_QUEUED_OUTPUTS")
    end = source.index("// Generate a 16-byte random nonce", start)
    queue_types_end = source.index("bool parseLightSelector(", start)
    queue_state_start = source.index("struct WsAuthState {", queue_types_end)
    implementation = source[start:queue_types_end] + source[queue_state_start:end]
    start = source.index("bool ApplicationWebserver::hasRuntimeInfoSubscribers()")
    end = source.index("unsigned ApplicationWebserver::getHttpActiveConnections()", start)
    light_output = source[start:end]
    start = source.index("void ApplicationWebserver::wsSendIna219(")
    end = source.index("void ApplicationWebserver::publishIna219(", start)
    current_output = source[start:end]
    binary = tmp_path_factory.mktemp("websocket-output") / "test"
    result = subprocess.run(
        [compiler, "-std=c++17", "-D_GLIBCXX_ASSERTIONS", "-x", "c++", "-", "-o", str(binary)],
        input=CPP_SUPPORT + implementation + CPP_LIGHT_INTERFACE + light_output + current_output + CPP_TESTS,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


@pytest.mark.parametrize(
    "scenario",
    ["fifo", "overflow", "stream_completion", "disconnect_text", "disconnect_start", "late_completion",
     "light_subscription", "runtime_subscription", "current_subscription"],
)
def test_websocket_output(websocket_output_binary: Path, scenario: str) -> None:
    result = subprocess.run(
        [str(websocket_output_binary), scenario],
        text=True,
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr