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
	struct HeapTestRequest {
		String cmd;
		uint32_t size = 0;
		uint64_t address = 0;
	};

	Api() = default;
	~Api() = default;

	bool dispatchCommand(const String& method, const JsonObject& params, String& errorMsg, bool relay = true);
	bool dispatchCommand(const char* method, const JsonObject& params, String& errorMsg, bool relay = true);
	bool dispatchSystemCommand(const String& cmd, const String& enable, const String& clearOta, String& errorMsg);
	bool dispatchJsonRpc(const String& json, String& errorMsg, bool relay = false);
	bool parseJsonRpcRequest(Stream& input, String& method, String& params, int& requestId, String& authHash,
															 bool& sparse, bool& paramsEmpty, String& errorMsg, HeapTestRequest* heapTest = nullptr,
											 String* notificationChannel = nullptr, int32_t* lightSelector = nullptr,
											 String* extensionId = nullptr, bool* hasRequestId = nullptr);
	bool renderData(const String& method, const JsonObject& params, String& out);
	bool renderData(const String& method, const JsonObject& params, String& out, int requestId);
	bool renderData(const String& method, bool sparse, String& out, int requestId, int32_t lightSelector = -1,
				String* errorMsg = nullptr);
	std::unique_ptr<IDataSourceStream> renderDataStream(const String& method, bool sparse, int requestId,
													 int32_t lightSelector = -1, String* errorMsg = nullptr);
#if defined(ARCH_HOST) && !defined(SMING_RELEASE)
	bool handleHostHeapTest(const HeapTestRequest& request, char* result, size_t resultSize);
#endif

	// ConfigDB-backed command dispatch for HTTP request bodies.
	bool dispatchCommandFromStream(const String& method, Stream& body, String& errorMsg, bool relay = true,
									   int32_t lightSelector = -1, bool rawUnionNormalized = false);

private:
	bool renderDataImpl(const String& method, bool sparse, String* out,
						std::unique_ptr<IDataSourceStream>* streamOut, int requestId, int32_t lightSelector, String* errorMsg);
	bool handleHosts(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg);
	bool handleConfig(const JsonObject& params, std::unique_ptr<IDataSourceStream>& out, String& errorMsg);
};
