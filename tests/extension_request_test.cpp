#include <SmingCore.h>
#include <ConfigDB/Json/Format.h>
#include <Data/Stream/MemoryDataStream.h>
#include <jsonrpc.h>
#include <cstdlib>

namespace
{
void require(bool condition, const char* message)
{
	if(!condition) {
		Serial << "FAIL: " << message << endl;
		std::exit(1);
	}
}
}

void init()
{
	Serial.begin(115200);
	fileSetFileSystem(&IFS::Host::getFileSystem());
	Jsonrpc database("rpc");
	Jsonrpc::Root root(database);
	{
		auto update = root.update();
		require(bool(update), "create request updater");
		auto request = update.toWsRequest();
		MemoryDataStream input;
		input << F("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"extensions.start_measurement\","
				   "\"params\":{\"extension_id\":\"spectrum-1\"}}");
		const auto status = request.importFromStream(ConfigDB::Json::format, input);
		require(bool(status), status.toString().c_str());
		require(request.getMethod() == F("extensions.start_measurement"), "method imported");
		require(request.params.getExtensionId() == F("spectrum-1"), "extension ID imported");
		MemoryDataStream output;
		ConfigDB::ExportOptions options;
		options.asObject = false;
		require(ConfigDB::Json::format.exportToStream(request.params, output, options) != 0, "export params");
		String json = output.readString(output.available());
		require(json.indexOf(F("\"extension_id\":\"spectrum-1\"")) >= 0, "extension ID retained on export");
	}
	Serial << "PASS: extension request parameters round-trip" << endl;
	std::exit(0);
}
