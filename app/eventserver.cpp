/**
 * @file
 * @author  Robin http://github.com/verybadsoldier
 *          Peter Jakobs http://github.com/pljakobs
 * 
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
 * eventserver.cpp
 *
 */
#include <RGBWWCtrl.h>

EventServer::~EventServer()
{
	stop();
}

/**
 * @brief Starts the event server with the given web server reference.
 * 
 * This function initializes the event server with the provided web server reference
 * (in order to access the Websocket broadcast function,sets the connection timeout
 * and starts listening on the TCP port. It also starts a timer to periodically 
 * publish keep-alive messages.
 * 
 * @param webServer The reference to the ApplicationWebserver object.
 */
void EventServer::start(ApplicationWebserver& webServer)
{
	this->webServer = &webServer;
	debug_i(ANSI_COLOR_BLUE "Starting event server with webserver referal\n" ANSI_COLOR_RESET);
	setTimeOut(_connectionTimeout);
	if(not listen(_tcpPort)) {
		debug_e(ANSI_COLOR_RED "EventServer failed to open listening port!" ANSI_COLOR_RESET);
	}

	auto fnc = TimerDelegate(&EventServer::publishKeepAlive, this);
	_keepAliveTimer.initializeMs(_keepAliveInterval * 1000, fnc).start();
}

/**
 * @brief Stops the EventServer.
 *
 * This function stops the EventServer if it is currently active. If the EventServer is not active, the function does nothing.
 */
void EventServer::stop()
{
	_keepAliveTimer.stop();

	// TcpServer::shutdown() calls delete this when connections drain.
	// EventServer is an embedded Application member, so self-delete is invalid.
	if(tcp) {
		tcp_arg(tcp, nullptr);
		tcp_accept(tcp, nullptr);
		tcp_close(tcp);
		tcp = nullptr;
	}

	for(auto& connection : connections) {
		if(connection != nullptr) {
			connection->setTimeOut(1);
		}
	}
}

/**
 * @brief Handles the connection of a client to the event server.
 *
 * This function is called when a client connects to the event server. It is responsible for
 * performing any necessary setup or initialization for the client connection.
 *
 * @param client A pointer to the TcpClient object representing the connected client.
 */
void EventServer::onClient(TcpClient* client)
{
	TcpServer::onClient(client);
	debug_d("Client connected from: %s\n", client->getRemoteIp().toString().c_str());
}

/**
 * @brief Handles the completion of a client connection.
 *
 * This function is called when a client connection is completed, whether it was successful or not.
 * It is an override of the TcpServer::onClientComplete() function.
 *
 * @param client The TcpClient object representing the client connection.
 * @param successful A boolean indicating whether the client connection was successful or not.
 */
void EventServer::onClientComplete(TcpClient& client, bool succesfull)
{
	TcpServer::onClientComplete(client, succesfull);
	debug_d("Client removed: %x\n", &client);
}

/**
 * @brief Publishes the current state of the event server.
 *
 * This function is responsible for publishing the current state of the LED controller
 * by creating a JSON-RPC message with the appropriate parameters and sending it to
 * the connected clients.
 *
 * @param raw The ChannelOutput object representing the raw color values.
 * @param pHsv Pointer to the HSVCT object representing the HSV color values. If null,
 *             the mode will be set to "raw".
 */
void EventServer::publishCurrentState(const ChannelOutput& raw, const HSVCT* pHsv)
{
	//debug_i(ANSI_COLOR_BLUE "EventServer::publishCurrentState\n" ANSI_COLOR_RESET);
	const bool hasHsv = (pHsv != nullptr);
	const bool sameRaw = (raw == _lastRaw);
	const bool sameMode = (hasHsv == _lastHasHsv);
	const bool sameHsv = (!hasHsv || (*pHsv == _lastHsv));
	if(sameRaw && sameMode && sameHsv) // No change
		return;
	unsigned long currentTime = millis();
	if(currentTime - _lastEventTime < _minEventInterval) {
		debug_d("eventserver, dropping currentState event\n");
		return; // Silently discard this event
	}
	_lastRaw = raw;
	_lastHasHsv = hasHsv;
	if(hasHsv) {
		_lastHsv = *pHsv;
	}
	_lastEventTime = currentTime;

	// Option C: reuse the persistent _colorDoc for this high-frequency event so
	// no JSON document is heap-allocated per publish (can fire up to 50x/s under
	// MQTT sync). clear() keeps the static pool; we just repopulate it.
	_colorDoc.clear();
	JsonObject root = _colorDoc.to<JsonObject>();
	root[F("jsonrpc")] = "2.0";
	root[F("method")] = F("color_event");
	root[F("id")] = _nextId++;
	JsonObject params = root.createNestedObject(F("params"));

	params[F("mode")] = pHsv ? "hsv" : "raw";

	JsonObject rawJson = params.createNestedObject(F("raw"));
	rawJson[F("r")] = raw.r;
	rawJson[F("g")] = raw.g;
	rawJson[F("b")] = raw.b;
	rawJson[F("ww")] = raw.ww;
	rawJson[F("cw")] = raw.cw;

	if(pHsv) {
		float h, s, v;
		int ct;
		pHsv->asRadian(h, s, v, ct);

		JsonObject hsvJson = params.createNestedObject(F("hsv"));
		hsvJson[F("h")] = h;
		hsvJson[F("s")] = s;
		hsvJson[F("v")] = v;
		hsvJson[F("ct")] = ct;
	}

	debug_d("EventServer::publishCurrentColor\n");

	if(_colorDoc.overflowed()) {
		debug_e(ANSI_COLOR_RED "EventServer::publishCurrentState: color_event exceeded _colorDoc capacity (%u), event truncated" ANSI_COLOR_RESET,
				(unsigned)_colorDoc.capacity());
	}

	sendRoot(root);
}

/**
 * @brief Publishes the clock slave status to the clients.
 *
 * This function creates a JSON-RPC message with the clock slave status and sends it to the clients.
 *
 * @param offset The offset value.
 * @param interval The current interval value.
 */
void EventServer::publishClockSlaveStatus(int offset, uint32_t interval)
{
	debug_d("EventServer::publishClockSlaveStatus: offset: %d | interval :%d\n", offset, interval);

	JsonRpcMessage msg(F("clock_slave_status"));
	JsonObject root = msg.getParams();
	root[F("offset")] = offset;
	root[F("current_interval")] = interval;
	sendToClients(msg);
}

/**
 * @brief Publishes a keep-alive message to the clients.
 * 
 * This function creates a JSON-RPC message with the method "keep_alive" and sends it to all connected clients.
 */
void EventServer::publishKeepAlive()
{
	debug_d("EventServer::publishKeepAlive\n");

	JsonRpcMessage msg(F("keep_alive"));
	sendToClients(msg);
}

/**
 * @brief Publishes a transition finished event.
 *
 * This function creates a JSON-RPC message with the event type "transition_finished" and sends it to the clients.
 *
 * @param name The name of the transition.
 * @param requeued Indicates whether the transition was requeued or not.
 */
void EventServer::publishTransitionFinished(const String& name, bool requeued)
{
	debug_d("EventServer::publishTransitionComplete: %s\n", name.c_str());

	JsonRpcMessage msg(F("transition_finished"));
	JsonObject root = msg.getParams();
	root[F("name")] = name;
	root[F("requeued")] = requeued;

	sendToClients(msg);
}

/**
 * @brief Sends a JSON-RPC message to all connected clients.
 *
 * This function sends a JSON-RPC message to all clients that are currently connected to the server.
 * The message is serialized into a JSON string and then sent to each client individually using the
 * `sendString` method of the `TcpClient` class. Additionally, the message is broadcasted to all
 * clients using the `wsBroadcast` method of the `webServer` object.
 * 
 * @note this is a bit of a cludge right now. I assume that mid term, I will deprecate the pure tcp 
 *      connection and only use the websocket connection. I'm keeping it for now to maintain compatibility
 *      with the fhem module
 *
 * @param rpcMsg The JSON-RPC message to be sent to the clients.
 */
void EventServer::sendToClients(JsonRpcMessage& rpcMsg)
{
	rpcMsg.setId(_nextId++);
	sendRoot(rpcMsg.getRoot());
}

void EventServer::sendRoot(const JsonObject& root)
{
	// Serialize into the persistent buffer. setLength(0) keeps the already
	// allocated capacity, so repeated events reuse the same heap block instead
	// of allocating and freeing a fresh String every time.
	_txBuffer.setLength(0);
	Json::serialize(root, _txBuffer);
	debug_d("EventServer::sendToClients: %s\n", _txBuffer.c_str());

	for(unsigned i = 0; i < connections.size(); ++i) {
		auto pClient = reinterpret_cast<TcpClient*>(connections[i]);
		pClient->sendString(_txBuffer);
	}

	app.wsBroadcast(_txBuffer);
}
