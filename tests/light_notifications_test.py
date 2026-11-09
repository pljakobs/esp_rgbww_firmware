"""Execute the firmware's event publisher and identity guard with isolated I/O."""

from pathlib import Path
import shutil
import subprocess

import pytest


SUPPORT = r"""
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#define F(value) value
#define cdebug_e(...) ((void)0)
#define EVENTSERVER 0
struct String : std::string {
    using std::string::string;
    unsigned length() const { return size(); }
};
struct ChannelOutput {
    int r=0, g=0, b=0, ww=0, cw=0;
    bool operator==(const ChannelOutput& o) const {
        return r==o.r && g==o.g && b==o.b && ww==o.ww && cw==o.cw;
    }
};
struct HSVCT {
    float h=0, s=0, v=0;
    int ct=0;
    bool operator==(const HSVCT& o) const {
        return h==o.h && s==o.s && v==o.v && ct==o.ct;
    }
    void asRadian(float& oh, float& os, float& ov, int& oct) const {
        oh=h; os=s; ov=v; oct=ct;
    }
};
enum class CtrlChannel { Red, Green, Blue, WarmWhite, ColdWhite };
struct RGBWWLed {
    enum class ColorMode { Hsv, Raw };
    using ChannelList = std::vector<CtrlChannel>;
    ChannelOutput output;
    HSVCT hsv;
    ColorMode mode=ColorMode::Raw;
    bool named=false;
    ChannelList channels{CtrlChannel::WarmWhite};
    auto getCurrentOutput() const { return output; }
    auto getCurrentColor() const { return hsv; }
    auto getMode() const { return mode; }
    bool usesNamedRawChannels() const { return named; }
    void getChannels(ChannelList& result) const { result=channels; }
};
struct VirtualLight {
    String id;
    std::unique_ptr<RGBWWLed> led=std::make_unique<RGBWWLed>();
    uint16_t index=0;
    ChannelOutput lastEventRaw;
    HSVCT lastEventHsv;
    bool lastEventHasHsv=false, eventStateKnown=false;
    uint32_t lastEventTime=0;
};
struct Frame {
    int light=-1, revision=0;
    String id, method;
    ChannelOutput raw;
    HSVCT hsv;
    std::vector<int> indexed;
} frame;
struct Builder {
    Frame* frame;
    void setH(float v) { frame->hsv.h=v; }
    void setS(float v) { frame->hsv.s=v; }
    void setV(float v) { frame->hsv.v=v; }
    void setCt(int v) { frame->hsv.ct=v; }
    void setR(int v) { frame->raw.r=v; }
    void setG(int v) { frame->raw.g=v; }
    void setB(int v) { frame->raw.b=v; }
    void setWw(int v) { frame->raw.ww=v; }
    void setCw(int v) { frame->raw.cw=v; }
    void addItem(int v) { frame->indexed.push_back(v); }
    Builder toRawNamed() { return *this; }
    Builder toRawIndexed() { return *this; }
};
struct StateBuilder {
    Builder toRaw() { return {&frame}; }
    Builder toHsv() { return {&frame}; }
};
struct EventBuilder {
    StateBuilder state;
    void setLight(int v) { frame.light=v; }
    void setLightId(const String& v) { frame.id=v; }
    void setRevision(int v) { frame.revision=v; }
};
namespace JsonRPC {
struct Message {
    enum class Kind { notification };
    int id;
    Kind kind;
    const char* method;
};
}
namespace Jsonrpc {
struct Root {
    Root(int) { frame=Frame{}; }
    struct Update {
        explicit operator bool() const { return true; }
        EventBuilder toLightState() { return {}; }
        EventBuilder toLightsChanged() { return {}; }
    };
    Update update() { return {}; }
    int asLightState() { return 0; }
    int asLightsChanged() { return 0; }
};
}
struct Codec {
    bool fail=false;
    int db() { return 0; }
    bool render(JsonRPC::Message message, int, String& payload) {
        frame.method=message.method;
        payload=message.method;
        return !fail;
    }
} codec;
Codec& rpcCodec() { return codec; }
bool normalizeRawUnionForWire(String&) { return true; }
uint32_t now=1000;
uint32_t millis() { return now; }
struct ApplicationWebserver {
    bool subscribed=true;
    std::vector<Frame> frames;
    bool hasLightSubscribers() const { return subscribed; }
    void wsSendLightEvent(const char*, unsigned) { frames.push_back(frame); }
};
struct Application {
    int config=0;
    int* cfg=&config;
    std::vector<VirtualLight> lights;
    RGBWWLed* getLight(int index, String&) const {
        for(const auto& light: lights)
            if(light.index==index) return light.led.get();
        return nullptr;
    }
} app;
namespace AppConfig {
struct Hardware {
    Hardware(int) {}
    struct Configuration { int getRevision() const { return 42; } } configuration;
};
}
struct EventServer {
    ApplicationWebserver* webServer;
    bool publishLightState(VirtualLight&);
    void publishLightsChanged();
};
struct JsonProcessor {
    struct RequestParameters { int32_t light=-1; String lightId; };
    RGBWWLed* resolveLight(const RequestParameters&, String&) const;
};
"""

TESTS = r"""
int main() {
    ApplicationWebserver socket;
    EventServer server{&socket};
    VirtualLight first, second;
    first.id="first"; first.index=3; first.led->output.ww=512;
    second.id="second"; second.index=7; second.led->output.ww=123;
    second.led->output.cw=789;
    second.led->channels.push_back(CtrlChannel::ColdWhite);
    assert(server.publishLightState(first));
    assert(server.publishLightState(second));
    assert(socket.frames.size()==2);
    assert(socket.frames[0].id=="first" && socket.frames[0].light==3);
    assert(socket.frames[0].indexed==std::vector<int>({512}));
    assert(socket.frames[1].id=="second" && socket.frames[1].light==7);
    assert(socket.frames[1].indexed==std::vector<int>({123,789}));
    assert(!server.publishLightState(first));
    second.led->output.cw=800;
    assert(!server.publishLightState(second));
    now+=200;
    assert(server.publishLightState(second));
    assert(socket.frames.back().indexed==std::vector<int>({123,800}));
    now+=200;
    first.led->mode=RGBWWLed::ColorMode::Hsv;
    first.led->hsv={3.14f,0.5f,0.75f,4000};
    assert(server.publishLightState(first));
    assert(socket.frames.back().hsv.ct==4000);
    first.led->named=true; first.led->mode=RGBWWLed::ColorMode::Raw;
    first.led->output.r=1023;
    now+=200;
    assert(server.publishLightState(first));
    assert(socket.frames.back().raw.r==1023);
    assert(socket.frames.back().indexed.empty());
    server.publishLightsChanged();
    assert(socket.frames.back().method=="lights_changed");
    assert(socket.frames.back().revision==42);
    socket.subscribed=false;
    now+=200;
    second.led->output.cw=900;
    assert(!server.publishLightState(second));
    const auto count=socket.frames.size();
    server.publishLightsChanged();
    assert(socket.frames.size()==count);
    socket.subscribed=true;
    codec.fail=true;
    assert(!server.publishLightState(second));
    codec.fail=false;
    assert(server.publishLightState(second));
    app.lights.push_back(std::move(first));
    JsonProcessor processor;
    JsonProcessor::RequestParameters params{3,"first"};
    String error;
    assert(processor.resolveLight(params,error)==app.lights[0].led.get());
    params.lightId="deleted";
    assert(processor.resolveLight(params,error)==nullptr);
    assert(error=="light_id does not match the selected light");
    params.lightId="";
    assert(processor.resolveLight(params,error)==app.lights[0].led.get());
    params.light=-2;
    assert(processor.resolveLight(params,error)==nullptr);
}
"""


def test_light_notifications_and_uuid_guard(tmp_path: Path) -> None:
    """All lights share one output, with independent throttles and UUID guards."""
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is required")
    root = Path(__file__).resolve().parents[1]
    source = (root / "app/eventserver.cpp").read_text()
    start = source.index("bool EventServer::publishLightState(")
    end = source.index("void EventServer::publishCurrentState(", start)
    publisher = source[start:end]
    source = (root / "app/jsonprocessor.cpp").read_text()
    start = source.index("RGBWWLed* JsonProcessor::resolveLight(")
    end = source.index("bool JsonProcessor::resolveRawChannels(", start)
    binary = tmp_path / "light-notifications"
    result = subprocess.run(
        [compiler, "-std=c++17", "-D_GLIBCXX_ASSERTIONS", "-x", "c++", "-", "-o", str(binary)],
        input=SUPPORT + publisher + source[start:end] + TESTS,
        text=True, capture_output=True, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
