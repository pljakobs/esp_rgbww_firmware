/**
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
 */

#pragma once

#include <ArduinoJson.h>
#include <rpccodec.h>
#include <Data/Stream/DataSourceStream.h>
#include <memory>

class Api {
public:
	Api() = default;
	~Api() = default;

	bool dispatchCommand(const String& method, const JsonObject& params, String& errorMsg, bool relay = true);
	bool dispatchCommand(const char* method, const JsonObject& params, String& errorMsg, bool relay = true);
	bool dispatchSystemCommand(const String& cmd, const String& enable, const String& clearOta, String& errorMsg);
	bool dispatchJsonRpc(const String& json, String& errorMsg, bool relay = false);
	bool parseJsonRpcRequest(Stream& input, String& method, String& params, int& requestId, String& authHash,
							 bool& sparse, bool& paramsEmpty, String& errorMsg);
	bool renderData(const String& method, const JsonObject& params, String& out);
	bool renderData(const String& method, const JsonObject& params, String& out, int requestId);
	bool renderData(const String& method, bool sparse, String& out, int requestId);

	// ConfigDB-backed command dispatch for HTTP request bodies (see CONFIGDB_JSON_INBOUND_PLAN.md Phase B)
	bool dispatchCommandFromStream(const String& method, Stream& body, String& errorMsg, bool relay = true);

private:
	bool handleHosts(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg);
	bool handleConfig(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg);
};