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

#include <Wiring/WVector.h>

#include "jsonrpcmessage.h"

class EventServer : public TcpServer{
public:
    EventServer() : webServer(nullptr) {} // Empty constructor
	EventServer(ApplicationWebserver& webServer) : webServer(&webServer){} ;
	virtual ~EventServer();

    void start(ApplicationWebserver& webServer); // Add this line
	void stop();

	void publishCurrentState(const ChannelOutput& raw, const HSVCT* pColor = NULL);
	void publishTransitionFinished(const String& name, bool requeued = false);
	void publishKeepAlive();
	void publishClockSlaveStatus(int offset, uint32_t interval);
	bool isEnabled() const { return enabled; }
	void setEnabled(bool enabled) { this->enabled = enabled; }

private:
	virtual void onClient(TcpClient *client) override;
	virtual void onClientComplete(TcpClient& client, bool succesfull) override;

	void sendToClients(JsonRpcMessage& rpcMsg);

	static const int _tcpPort = 9090;
	static const int _connectionTimeout = 120;
	static const int _keepAliveInterval = 60;
	
    Timer _keepAliveTimer;
	int _nextId = 1;

	bool enabled;

	// event throtteling parameters
	unsigned long _lastEventTime = 0;      // Timestamp of last event sent
    const unsigned long _minEventInterval = 500; // 500ms = 2 per second max
	
	ChannelOutput _lastRaw;
	const HSVCT* _lastpHsv = nullptr;
	// websocket interface
    ApplicationWebserver* webServer;
	};
