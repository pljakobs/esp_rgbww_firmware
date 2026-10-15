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

#include <functional>
#include <memory>
#include <ConfigDB/JsonRPC/JsonRPC.h>
#include "jsonrpc.h"

class WebsocketConnection;

/**
 * @brief Transient workspace for building outbound JSON-RPC messages.
 *
 * Unlike app.cfg / app.data this database is never persisted: a commit callback
 * clears the dirty flag before ConfigDB can save it. Streaming responses retain
 * an ObjectRef to their body store; ConfigDB copy-on-write keeps that snapshot
 * stable while later messages are constructed.
 */
class RpcCodec
{
public:
	using Message = JsonRPC::Message;

	class StreamTask
	{
	public:
		~StreamTask();
		void cancel();

	private:
		struct State;
		explicit StreamTask(std::shared_ptr<State> state) : state(std::move(state))
		{
		}
		std::shared_ptr<State> state;
		friend class RpcCodec;
	};

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
	 * @brief Create a lazy JSON-RPC envelope source.
	 *
	 * The source does not know its total length; use sendWebsocketStream() for
	 * transports which require finite frame lengths.
	 */
	std::unique_ptr<IDataSourceStream> renderStream(const Message& msg, const ConfigDB::Object& body);

	/**
	 * @brief Send a lazy source as bounded, finite WebSocket fragments.
	 *
	 * The caller owns the returned task and must cancel it when the connection
	 * is disconnected. The completion callback is deferred until all fragments
	 * have drained from the TCP stream.
	 */
	std::shared_ptr<StreamTask> sendWebsocketStream(WebsocketConnection& socket,
												 std::unique_ptr<IDataSourceStream> source,
												 std::function<void(bool)> onComplete);

	/**
	 * @brief Serialize @a body on its own, with no JSON-RPC envelope.
	 *
	 * For transports that carry a bare payload, such as the MQTT state topics.
	 */
	bool renderPayload(const ConfigDB::Object& body, String& out);

private:
	Jsonrpc _db;
	// Guards synchronous serialization and stream-size measurement against
	// re-entrant use; it does not cover field population before those calls.
	bool _rendering = false;
};

RpcCodec& rpcCodec();
