/**
 * @file
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
 *
 * @section DESCRIPTION
 *
 * Protocol-agnostic API handler layer.
 * Decouples business logic from HTTP transport so it can be called from any transport (HTTP, WebSocket, etc).
 */

#ifndef APP_API_H_
#define APP_API_H_

#include <ArduinoJson.h>

class Application;

/**
 * @class AppApi
 * @brief Protocol-agnostic API handler
 *
 * Provides a single entry point for all API methods.
 * Business logic is extracted from HTTP handlers and called through dispatch().
 */
class AppApi
{
public:
	/**
	 * @brief Constructor
	 * @param app Reference to Application instance
	 */
	AppApi(Application& app) : app(app) {}

	/**
	 * @brief Dispatch API method
	 * @param method API method name (e.g., "getInfo", "setColor")
	 * @param params Request parameters as JsonObject
	 * @param result Output result as JsonObject (caller allocates)
	 * @return true if method exists and executed successfully; false if method not found or error occurred
	 */
	bool dispatch(const String& method, const JsonObject& params, JsonObject& result);

private:
	Application& app;

	// API method implementations
	bool getInfo(const JsonObject& params, JsonObject& result);
	bool getColor(const JsonObject& params, JsonObject& result);
	bool setColor(const JsonObject& params, JsonObject& result);
	bool getConfig(const JsonObject& params, JsonObject& result);
	bool setConfig(const JsonObject& params, JsonObject& result);
	bool getData(const JsonObject& params, JsonObject& result);
	bool setData(const JsonObject& params, JsonObject& result);
	bool getHosts(const JsonObject& params, JsonObject& result);
	bool ping(const JsonObject& params, JsonObject& result);
	bool systemCmd(const JsonObject& params, JsonObject& result);
	bool on(const JsonObject& params, JsonObject& result);
	bool off(const JsonObject& params, JsonObject& result);
	bool stop(const JsonObject& params, JsonObject& result);
	bool skip(const JsonObject& params, JsonObject& result);
	bool pause(const JsonObject& params, JsonObject& result);
	bool continue_cmd(const JsonObject& params, JsonObject& result);
	bool blink(const JsonObject& params, JsonObject& result);
	bool toggle(const JsonObject& params, JsonObject& result);
	bool networks(const JsonObject& params, JsonObject& result);
	bool scanNetworks(const JsonObject& params, JsonObject& result);
	bool connect(const JsonObject& params, JsonObject& result);
};

#endif /* APP_API_H_ */
