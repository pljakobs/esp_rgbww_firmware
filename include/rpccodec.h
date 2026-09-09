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

#include <ConfigDB/JsonRPC/JsonRPC.h>
#include "jsonrpc.h"

/**
 * @brief Transient workspace for building outbound JSON-RPC messages.
 *
 * Unlike app.cfg / app.data this database is never persisted: a commit callback
 * clears the dirty flag before ConfigDB can save it. Only one message may be
 * under construction at a time, so a body must be fully rendered before the
 * next message is started.
 */
class RpcCodec
{
public:
	using Message = JsonRPC::Message;

	RpcCodec();

	Jsonrpc& db()
	{
		return _db;
	}

	/**
	 * @brief Serialize a complete JSON-RPC envelope around @a body into @a out.
	 *
	 * For the buffer-only sinks: MqttClient::publish() and
	 * WebsocketConnection::broadcast().
	 */
	bool render(const Message& msg, const ConfigDB::Object& body, String& out);

	/**
	 * @brief Serialize @a body on its own, with no JSON-RPC envelope.
	 *
	 * For transports that carry a bare payload, such as the MQTT state topics.
	 */
	bool renderPayload(const ConfigDB::Object& body, String& out);

private:
	Jsonrpc _db;
};

RpcCodec& rpcCodec();
