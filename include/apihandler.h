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
#include <Data/Stream/DataSourceStream.h>
#include <memory>

class Api {
public:
	Api() = default;
	~Api() = default;

	bool dispatch(const String& method, const JsonObject& params, JsonObject& out);
	bool dispatchCommand(const String& method, const JsonObject& params, String& errorMsg, bool relay = true);
	bool dispatchCommand(const String& method, const String& params, String& errorMsg, bool relay = true);
	bool dispatchJsonRpc(const String& json, String& errorMsg, bool relay = false);
	bool dispatchStream(const String& method, const JsonObject& params, std::unique_ptr<IDataSourceStream>& out,
					 String& errorMsg);

private:
	bool dispatchDataRequest(const String& method, const JsonObject& params, JsonObject* outObject,
						 std::unique_ptr<IDataSourceStream>* outStream, String& errorMsg);
	bool handleInfo(const JsonObject& params, JsonObject& out);
	bool handleColor(const JsonObject& params, JsonObject& out);
	bool handleNetworks(const JsonObject& params, JsonObject& out);
	bool handleHosts(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg);
	bool handleConfig(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg);
};