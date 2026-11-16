#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class String : public std::string
{
public:
	using std::string::string;
	String(const std::string& value) : std::string(value) {}
};
String operator+(const char* left, const String& right) { return String(left) += right; }
String operator+(const String& left, const char* right) { return String(left) += right; }
String operator+(const String& left, const String& right) { return String(left) += right; }

#define F(value) value
#define ARCH_ESP8266
#define cdebug_i(...) ((void)0)
constexpr int HTTP_STATUS_OK = 200;
constexpr int HTTP_STATUS_PERMANENT_REDIRECT = 308;
constexpr int HTTP_STATUS_SERVICE_UNAVAILABLE = 503;
constexpr int HTTP_HEADER_LOCATION = 1;
constexpr int HTTP_HEADER_CACHE_CONTROL = 2;
constexpr int MIME_TEXT = 1;
constexpr int MIME_HTML = 2;
enum class HttpMethod { GET };
struct HttpRequest {};

class IpAddress
{
public:
	IpAddress() = default;
	IpAddress(const char* value) : address(value) {}
	bool isNull() const { return address.empty(); }
	bool operator==(const IpAddress& other) const { return address == other.address; }
	bool operator!=(const IpAddress& other) const { return !(*this == other); }
	String toString() const { return address; }
private:
	String address;
};

uint32_t localId = 10964360;
uint32_t system_get_chip_id() { return localId; }
struct {
	IpAddress address{"192.168.29.101"};
	bool connected = true;
	IpAddress getIP() const { return address; }
	bool isConnected() const { return connected; }
} WifiStation;
struct {
	bool enabled = false;
	bool isEnabled() const { return enabled; }
} WifiAccessPoint;

struct Controllers {
	enum ControllerState { INCOMPLETE, OFFLINE, ONLINE, LOCALHOST };
	enum HostType { HOST_TYPE_CONTROLLER, HOST_TYPE_WALLPANEL, HOST_TYPE_ALIAS };
	struct ControllerInfo {
		uint32_t id;
		const char* ipAddress;
		ControllerState state = ONLINE;
		HostType hostType = HOST_TYPE_CONTROLLER;
	};
	std::vector<ControllerInfo> data;
	auto begin() { return data.begin(); }
	auto end() { return data.end(); }
};

namespace FSTR
{
struct Stream {
	explicit Stream(void*) {}
	virtual ~Stream() = default;
};
struct TemplateStream : Stream {
	using Stream::Stream;
	std::map<String, String> variables;
	bool doubleBraces = false;
	void setDoubleBraces(bool value) { doubleBraces = value; }
	void setVar(const String& name, const String& value) { variables[name] = value; }
};
}
struct Asset {
	explicit operator bool() const { return false; }
	void* content() const { return nullptr; }
};
struct {
	Asset operator[](const char*) const { return {}; }
} fileMap;
bool fileExist(const char*) { return true; }

struct {
	Controllers* controllers = nullptr;
	bool mounted = true;
	bool isFilesystemMounted() const { return mounted; }
	struct {
		bool processing = false;
		bool isProccessing() const { return processing; }
	} ota;
	struct {
		unsigned checks = 0;
		bool isActive() const { return false; }
		void checkForUpdate() { ++checks; }
	} webappOta;
} app;

struct HttpResponse {
	int code = 0;
	std::map<int, String> headers;
	String body;
	std::map<String, String> variables;
	bool doubleBraces = false;
	bool cors = false;
	void setContentType(int) {}
	void sendString(const String& value) { body = value; }
	void sendDataStream(FSTR::Stream* stream, int type) {
		assert(type == MIME_HTML);
		if(auto page = dynamic_cast<FSTR::TemplateStream*>(stream)) {
			variables = page->variables;
			doubleBraces = page->doubleBraces;
		}
		delete stream;
	}
};
struct ApplicationWebserver {
	bool _redirectToLeader = false;
	IpAddress _webappLeaderAddress;
	bool preflight = true;
	bool preflightRequest(HttpRequest&, HttpResponse&, std::initializer_list<HttpMethod>) { return preflight; }
	void setCorsHeaders(HttpResponse& response) { response.cors = true; }
	void updateWebappLeader();
	void onIndex(HttpRequest&, HttpResponse&);
};

#include "webapp_redirect_methods.inc"

int main()
{
	ApplicationWebserver server;
	HttpRequest request;
	HttpResponse local;
	server.onIndex(request, local);
	assert(local.headers[HTTP_HEADER_LOCATION] == "/index.html");

	Controllers controllers;
	app.controllers = &controllers;
	controllers.data = {{0xFFFFFF, "192.168.29.69"}};
	server.updateWebappLeader();
	assert(!server._redirectToLeader);
	controllers.data.push_back({0x1000000, "192.168.29.70"});
	server.updateWebappLeader();
	assert(server._redirectToLeader);
	assert(server._webappLeaderAddress.toString() == "192.168.29.70");

	controllers.data.push_back({0xFFFFFFFF, "192.168.29.69"});
	app.mounted = false;
	HttpResponse redirect;
	server.onIndex(request, redirect);
	assert(redirect.code == HTTP_STATUS_OK);
	assert(redirect.headers.count(HTTP_HEADER_LOCATION) == 0);
	assert(redirect.variables.at("leader") == "192.168.29.69");
	assert(redirect.variables.at("home") == "192.168.29.101");
	assert(redirect.doubleBraces);
	assert(redirect.headers[HTTP_HEADER_CACHE_CONTROL] == "no-store");
	assert(redirect.cors);
	assert(app.webappOta.checks == 0);
	app.mounted = true;
	std::reverse(controllers.data.begin(), controllers.data.end());
	server.updateWebappLeader();
	assert(server._webappLeaderAddress.toString() == "192.168.29.69");

	controllers.data[0].state = Controllers::OFFLINE;
	server.updateWebappLeader();
	assert(server._webappLeaderAddress.toString() == "192.168.29.70");
	controllers.data[1].state = Controllers::INCOMPLETE;
	server.updateWebappLeader();
	assert(!server._redirectToLeader);

	controllers.data = {
		{0xFFFFFFFF, "192.168.29.69", Controllers::ONLINE, Controllers::HOST_TYPE_WALLPANEL},
		{0xFFFFFFFE, "192.168.29.70", Controllers::ONLINE, Controllers::HOST_TYPE_ALIAS},
		{0xFFFFFFFD, ""},
	};
	server.updateWebappLeader();
	assert(!server._redirectToLeader);

	controllers.data = {{0x1000000, "192.168.29.69"}};
	localId = 0x1000000;
	server.updateWebappLeader();
	assert(!server._redirectToLeader);
	localId = 0x1000001;
	server.updateWebappLeader();
	assert(!server._redirectToLeader);
	localId = 10964360;
	controllers.data[0].ipAddress = "192.168.29.101";
	server.updateWebappLeader();
	assert(!server._redirectToLeader);
	controllers.data[0].ipAddress = "192.168.29.69";

	WifiStation.connected = false;
	WifiAccessPoint.enabled = true;
	HttpResponse disconnected;
	server.onIndex(request, disconnected);
	assert(!server._redirectToLeader);
	assert(disconnected.headers[HTTP_HEADER_LOCATION] == "/index.html");
	WifiStation.connected = true;
	WifiAccessPoint.enabled = false;
	app.ota.processing = true;
	HttpResponse updating;
	server.onIndex(request, updating);
	assert(updating.code == HTTP_STATUS_SERVICE_UNAVAILABLE);
	assert(updating.headers.count(HTTP_HEADER_LOCATION) == 0);
	app.ota.processing = false;

	controllers.data.clear();
	HttpResponse fallback;
	server.onIndex(request, fallback);
	assert(!server._redirectToLeader && server._webappLeaderAddress.isNull());
	assert(fallback.headers[HTTP_HEADER_LOCATION] == "/index.html");
}
