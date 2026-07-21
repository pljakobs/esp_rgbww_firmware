/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 * 			Peter Jakobs http://github.com/pljakobs
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
 *
 */
#include <RGBWWCtrl.h>

#define DNS_PORT 53

DnsServer dnsServer;

/**
 * @brief Constructor for the AppWIFI class.
 * 
 * Initializes the member variables of the AppWIFI class.
 */
AppWIFI::AppWIFI()
{
	_ApIP = IpAddress(String(DEFAULT_AP_IP));
	_client_err_msg = "";
	_con_ctr = 0;
	_scanning = false;
	_new_connection = false;
	_client_status = CONNECTION_STATUS::IDLE;
}

/**
 * @brief Retrieves the available networks.
 * 
 * @return The list of available networks.
 */
BssList AppWIFI::getAvailableNetworks()
{
	return _networks;
}

/**
 * Scans for available Wi-Fi networks.
 * 
 * @param connectAfterScan Flag indicating whether to connect to a network after the scan is completed.
 */
void AppWIFI::scan(bool connectAfterScan)
{
	_scanning = true;
	_keepStaAfterScan = connectAfterScan;
	WifiStation.startScan(ScanCompletedDelegate(&AppWIFI::scanCompleted, this));
}

/**
 * Callback function called when WiFi scan is completed.
 * 
 * @param succeeded Indicates whether the scan succeeded or not.
 * @param list The list of available WiFi networks.
 */
void AppWIFI::scanCompleted(bool succeeded, BssList& list)
{
	if(succeeded) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::scanCompleted" ANSI_COLOR_RESET);
		_networks.clear();
		for(size_t i = 0; i < list.count(); i++) {
			if(!list[i].hidden && list[i].ssid.length() > 0) {
				_networks.add(list[i]);
			}
		}
	}else{
		debug_e(ANSI_COLOR_RED "wifi scan failed" ANSI_COLOR_RESET);
	}
	// TODO add wsBroadcast of available networks
	_networks.sort([](const BssInfo& a, const BssInfo& b) { return b.rssi - a.rssi; });
	_scanning = false;

	// make sure to trigger connect again cause otherwise the Wifi reconnect attempts may come to a stop
	if(_keepStaAfterScan)
		WifiStation.connect();
}

/**
 * @brief Clears the stored WiFi credentials and disconnects from the WiFi network.
 * 
 * This function resets the WiFi configuration to empty values and disconnects from the current WiFi network.
 * After calling this function, the connection status is set to IDLE.
 */
void AppWIFI::forgetWifi()
{
	debug_i(ANSI_COLOR_BLUE "AppWIFI::forget_wifi" ANSI_COLOR_RESET);
	WifiStation.config("", "");
	WifiStation.disconnect();
	if(!WifiAccessPoint.isEnabled()) {
		startAp();
	}
	_client_status = CONNECTION_STATUS::IDLE;
}

/**
 * @brief Initializes the AppWIFI class.
 * 
 * This function disables wifi sleep, enables WifiStation if it is not already enabled,
 * disables WifiAccessPoint if it is enabled, sets up the access point and station configurations,
 * registers callbacks for station disconnect, connect, and IP acquisition events,
 * and configures the WifiClient with static IP or DHCP based on the configuration.
 * 
 * If there is no access point to connect to, it starts its own access point and scans for available networks.
 * 
 * @note This function assumes that the app object is available and isFirstRun() returns the correct value.
 */
void AppWIFI::init()
{
	debug_i(ANSI_COLOR_BLUE "AppWIFI::init" ANSI_COLOR_RESET);
	// ESP SDK function to disable  sleep
	wifi_set_sleep_type(NONE_SLEEP_T);

	debug_i(ANSI_COLOR_BLUE "AppWIFI::init\n    station " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "\n    AP      " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, WifiStation.isEnabled()? "enabled" : "disabled", WifiAccessPoint.isEnabled()? "enabled" : "disabled");

	//don`t enable/disable again to save eeprom cycles
	if(!WifiStation.isEnabled()) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::init enable WifiStation" ANSI_COLOR_RESET);
		WifiStation.enable(true, true);
	}

	WifiStation.enable(true);
	if(WifiAccessPoint.isEnabled()) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::init WifiAccessPoint disabled" ANSI_COLOR_RESET);
		WifiAccessPoint.enable(false, true);
	}

	_con_ctr = 0;
	// ConfigDB adapt
	if(app.isFirstRun()) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::init initial run - setting up AP, ssid: " ANSI_COLOR_RESET);
		char ssid_buf[64];
		snprintf(ssid_buf, sizeof(ssid_buf), "%s%u", DEFAULT_AP_SSIDPREFIX, system_get_chip_id());
		String SSID = ssid_buf;
		debug_i(ANSI_COLOR_BLUE "" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, SSID.c_str());

		AppConfig::Network network(*app.cfg);
		if(auto networkUpdate = network.update()) {
			networkUpdate.mdns.setName(SSID);
			networkUpdate.ap.setSsid(SSID);
		} //this should never fail as it is during system startup, there are no asynchronous things here. I think
		WifiAccessPoint.setIP(_ApIP);
	}

	// register callbacks
	debug_i(ANSI_COLOR_BLUE "AppWIFI::init register callbacks" ANSI_COLOR_RESET);
	WifiEvents.onStationDisconnect(StationDisconnectDelegate(&AppWIFI::_STADisconnect, this));
	WifiEvents.onStationConnect(StationConnectDelegate(&AppWIFI::_STAConnected, this));
	WifiEvents.onStationGotIP(StationGotIPDelegate(&AppWIFI::_STAGotIP, this));

	if(WifiStation.getSSID() == "") {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::init no AP to connect to - start own AP" ANSI_COLOR_RESET);
		// No wifi to connect to - initialize AP
		startAp();

		// already scan for avaialble networks to speedup things later
		scan(false);

	} else {

		//configure WifiClient
		{
			AppConfig::Network network(*app.cfg);
			if(!network.connection.getDhcp() && !network.connection.getIp().length() == 0) {
				debug_i(ANSI_COLOR_BLUE "AppWIFI::init setting static ip" ANSI_COLOR_RESET);
				if(WifiStation.isEnabledDHCP()) {
					// dhcp is configured off but currently enabled - disable it
					debug_i(ANSI_COLOR_BLUE "AppWIFI::init disabled dhcp" ANSI_COLOR_RESET);
					WifiStation.enableDHCP(false);
				}
				if(!(WifiStation.getIP() == network.connection.getIp()) ||
				   !(WifiStation.getNetworkGateway() == network.connection.getGateway()) ||
				   !(WifiStation.getNetworkMask() == network.connection.getNetmask())) {
					debug_i(ANSI_COLOR_BLUE "AppWIFI::init updating ip configuration" ANSI_COLOR_RESET);
					WifiStation.setIP(network.connection.getIp(), network.connection.getNetmask(),
									  network.connection.getGateway());
				}
			} else {
				debug_i(ANSI_COLOR_BLUE "AppWIFI::init dhcp" ANSI_COLOR_RESET);
				if(!WifiStation.isEnabledDHCP()) {
					debug_i(ANSI_COLOR_BLUE "AppWIFI::init enabling dhcp" ANSI_COLOR_RESET);
					WifiStation.enableDHCP(true);
				}
			}
		} // end ConfigDB network context
		debug_i(ANSI_COLOR_BLUE "AppWifi::init - triggering wifi connect" ANSI_COLOR_RESET);
		WifiStation.connect();
	}
}
/*
 * 
 * @param ssid The SSID of the Wi-Fi network.
 * @param new_con Flag indicating whether it is a new connection or not.
 */
void AppWIFI::connect(String ssid, bool new_con /* = false */)
{
	connect(ssid, "", new_con);
}

/**
 * @brief Connects to a Wi-Fi network with the specified SSID and password.
 * 
 * @param ssid The SSID of the Wi-Fi network to connect to.
 * @param pass The password of the Wi-Fi network.
 * @param new_con Flag indicating whether it is a new connection or not. Default is false.
 */
void AppWIFI::connect(String ssid, String pass, bool new_con /* = false */)
{
	debug_i(ANSI_COLOR_BLUE "AppWIFI::connect ssid " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " newcon " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, ssid.c_str(), new_con);
	_con_ctr = 0;
	_new_connection = new_con;
	_client_status = CONNECTION_STATUS::CONNECTING;

	debug_i(ANSI_COLOR_BLUE "connecting to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " using " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, ssid.c_str(), pass.c_str());
	WifiStation.config(ssid, pass);
	WifiStation.connect();
	broadcastWifiStatus(F("Connecting to WiFi"));
}

/**
 * @brief Handles the disconnection of the station from the Wi-Fi network.
 *
 * This function is called when the station disconnects from the Wi-Fi network.
 * It checks the reason for disconnection and performs necessary actions based on the reason.
 * If the disconnection reason is either reaching the maximum connection retries or wrong password,
 * it sets the client status to ERROR and updates the client error message.
 * If a new connection is requested, it disconnects the station and configures it with empty SSID and password.
 * Otherwise, it scans for available networks and starts the access point.
 *
 * @param ssid The SSID of the Wi-Fi network.
 * @param bssid The MAC address of the Wi-Fi network.
 * @param reason The reason for disconnection.
 */
void AppWIFI::_STADisconnect(const String& ssid, MacAddress bssid, WifiDisconnectReason reason)
{
	debug_i(ANSI_COLOR_BLUE "AppWIFI::_STADisconnect reason - " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE " - counter " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, reason, _con_ctr);

	if(_con_ctr == DEFAULT_CONNECTION_RETRIES || WifiStation.getConnectionStatus() == eSCS_WrongPassword) {
		_client_status = CONNECTION_STATUS::ERROR;
		_client_err_msg = WifiStation.getConnectionStatusName();
		debug_i(ANSI_COLOR_BLUE "AppWIFI::_STADisconnect err " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " - new connection: " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _client_err_msg.c_str(), _new_connection);
		if(_new_connection) {
			debug_i(ANSI_COLOR_BLUE "AppWIFI::_STADisconnect - disconnecting station" ANSI_COLOR_RESET);
			WifiStation.disconnect();
			WifiStation.config("", "");
		} else {
			scan(true);
			startAp();
		}
	}
	debug_i(ANSI_COLOR_BLUE "AppWIFI::_STADisconnect - _client_err_msg: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _client_err_msg.c_str());
	broadcastWifiStatus(_client_err_msg);
	_con_ctr++;
}

/**
 * Callback function called when a WiFi connection is established.
 * 
 * primarily sets the hostname, if it's configured in ConfigDB, that is used
 * otherwise, the default hostname is set to "RGBWW-<chipid>"
 * the hostname is used for mDNS and other network services
 * 
 * the function also broadcasts the wifi status to the clients
 * 
 * @param ssid The SSID of the Wi-Fi network.
 * @param bssid The MAC address of the access point.
 * @param channel The Wi-Fi channel used for the connection.
 */
void AppWIFI::_STAConnected(const String& ssid, MacAddress bssid, uint8_t channel)
{
	debug_i(ANSI_COLOR_BLUE "AppWIFI::_STAConnected SSID - " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, ssid.c_str());
	{
		String device_name;
		{
			AppConfig::General general(*app.cfg);
			device_name = general.getDeviceName();
		} // end ConfigDB general context
		if(device_name == "") {
			char device_name_buf[64];
			snprintf(device_name_buf, sizeof(device_name_buf), "%s%u", DEFAULT_AP_SSIDPREFIX, system_get_chip_id());
			device_name = device_name_buf;
			debug_i(ANSI_COLOR_BLUE "no device name configured, building default name" ANSI_COLOR_RESET);
		}
		debug_i(ANSI_COLOR_BLUE "AppWIFI::connect setting hostname to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, device_name.c_str());
		WifiStation.setHostname(device_name);
		{
			AppConfig::Network::OuterUpdater network(*app.cfg);
			network.mdns.setName(app.sanitizeName(device_name));
		} // end ConfigDB network updater context
	}	 
	app.startNetworkServices();
	broadcastWifiStatus(F("Connected to WiFi"));
	app.telemetryClient.log(F("WiFi connected"));
	_con_ctr = 0;
	// wifi cstation connected
}

/**
 * @brief Callback function called when the WiFi station gets an IP address.
 * 
 * This function is called when the WiFi station successfully connects to a WiFi network
 * and obtains an IP address. It performs the necessary initialization steps and starts
 * the required services.
 * 
 * @param ip The IP address assigned to the WiFi station.
 * @param mask The subnet mask assigned to the WiFi station.
 * @param gateway The gateway IP address assigned to the WiFi station.
 */
void AppWIFI::_STAGotIP(IpAddress ip, IpAddress mask, IpAddress gateway)
{
	// Set hostname immediately so every debug_i() in this callback and onwards
	// carries the correct name — not the empty string from begin().
	{
		AppConfig::Network network(*app.cfg);
#ifndef SMING_RELEASE
		app.udpSyslogStream.setHostname(network.mdns.getName());
#endif
	}
	debug_i(ANSI_COLOR_BLUE "AppWIFI::_STAGotIP" ANSI_COLOR_RESET);

	if(_new_connection) {
		stopAp(90000);
	} else {
		stopAp(1000);
	}

	IP=ip.toString();
	{
		AppConfig::General general(*app.cfg);
		AppConfig::Network network(*app.cfg);
		debug_i(ANSI_COLOR_BLUE "AppWIFI::_STAGotIP - device_name " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " hostname " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, general.getDeviceName().c_str(),
				network.mdns.getName().c_str());
		if(network.mdns.getName().length() > 0) {
			debug_i(ANSI_COLOR_BLUE "AppWIFI::_STAGotIP - setting mdns hostname to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, network.mdns.getName().c_str());
		}
	} //end ConfigDB general and network context

	app.mdnsService.start();
	String ipAddress = ip.toString();

	{
		AppConfig::Network network(*app.cfg);
		uint32_t id;
		
		id = (uint32_t)system_get_chip_id();	

		debug_i(ANSI_COLOR_BLUE "adding mdns host " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " with ip " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " and id " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, network.mdns.getName().c_str(), ipAddress.c_str(), String(id).c_str());
		app.controllers->addOrUpdate(id, network.mdns.getName(), ipAddress,"",-1,  Controllers::HostType::HOST_TYPE_CONTROLLER);
		
		broadcastWifiStatus();

		if(network.mqtt.getEnabled()) {
			app.mqttclient.start();
		}
	} // end ConfigDB network context

	// After network is up, report any crash/watchdog reboot to the syslog target
	app.reportCrashDump();
	// Now that we have an IP address, drain any log messages that were compressed
	// into the pre-network ring buffer before the UDP socket was routable.
#ifndef SMING_RELEASE
	app.udpSyslogStream.drainPreNetBuffer();
#endif

	// Kick off background webapp update check with a 10s delay to allow system to stabilize[cite: 18].
    // Static SimpleTimer avoids dynamic memory allocation during the boot sequence.
    static Timer otaDelayTimer;
    otaDelayTimer.initializeMs(10000, TimerDelegate([]() {
        app.webappOta.checkForUpdate();
 	   })).startOnce();
	}

/**
 * Stops the access point (AP) if it is enabled.
 * 
 * @param delay The delay in milliseconds before stopping the AP. If delay is greater than 0, the AP will be stopped after the specified delay.
 */
void AppWIFI::stopAp(int delay)
{
	if(!WifiAccessPoint.isEnabled()) {
		return;
	}

	if(delay > 0) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::stopAp delay " ANSI_COLOR_CYAN "%i" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, delay);
		_timer.initializeMs(delay, std::bind(&AppWIFI::stopAp, this, 0)).startOnce();
		return;
	}

	debug_i(ANSI_COLOR_BLUE "AppWIFI::stopAp" ANSI_COLOR_RESET);
	debug_i(ANSI_COLOR_BLUE "Disabling AP" ANSI_COLOR_RESET);
	_timer.stop();

	// Don't shut down the AP while the webapp is still downloading — the user's
	// browser may be connected via the AP to watch the updating page.  Poll every
	// 10 s until the download is done, then disable the AP.
	if(app.webappOta.isActive()) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::stopAp - webapp OTA in progress, deferring AP stop by 10s" ANSI_COLOR_RESET);
		_timer.initializeMs(10000, std::bind(&AppWIFI::stopAp, this, 0)).startOnce();
		return;
	}

	if(WifiAccessPoint.isEnabled()) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI::stopAp WifiAP disable" ANSI_COLOR_RESET);
		WifiAccessPoint.enable(false, false);
	}
	broadcastWifiStatus(F("AP stopping"));
}

/**
 * @brief Starts the Access Point (AP) for the AppWIFI class.
 * 
 * This function enables the AP and configures it with the provided SSID and password.
 * If the AP is already enabled, it does nothing.
 * 
 * @note This function assumes that the necessary configurations are already set in the `app.cfg.network.ap` structure.
 */
void AppWIFI::startAp()
{
	//String ssid="rgbww test";
	debug_i(ANSI_COLOR_BLUE "AppWIFI::startAp" ANSI_COLOR_RESET);
	debug_i(ANSI_COLOR_BLUE "Enabling AP" ANSI_COLOR_RESET);
	if(!WifiAccessPoint.isEnabled()) {
		debug_i(ANSI_COLOR_BLUE "AppWIFI:: WifiAP enable" ANSI_COLOR_RESET);
		WifiAccessPoint.enable(true, false);
		debug_i(ANSI_COLOR_BLUE "AP enabled" ANSI_COLOR_RESET);
		//debug_i(ANSI_COLOR_BLUE "AP SSID: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, app.cfg.network.ap.ssid);
		{
			AppConfig::Network network(*app.cfg);
			if(network.ap.getSecured()) {
				WifiAccessPoint.config(network.ap.getSsid(), network.ap.getPassword(), AUTH_WPA2_PSK);
			} else {
				WifiAccessPoint.config(network.ap.getSsid(), "", AUTH_OPEN);
			}
		} // end AppConfig network context
	}

    // Wait for AP IP to be assigned before starting DNS server
    Timer* dnsStartTimer = new Timer();
    dnsStartTimer->initializeMs(500, [this, dnsStartTimer]() {
        IpAddress apIP = WifiAccessPoint.getIP();
        if (apIP.toString() != "0.0.0.0") {
            dnsServer.start(DNS_PORT, "*", apIP);
            debug_i(ANSI_COLOR_BLUE "DNS server started: with address " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, apIP.toString().c_str());
            dnsStartTimer->stop();
            delete dnsStartTimer;
        }
    }).start();
	broadcastWifiStatus(F("AP started"));
}

/**
 * @brief Broadcasts the WiFi status to all connected clients.
 * 
 * This function broadcasts the WiFi status to all connected clients.
 * It creates a JSON-RPC message with the WiFi status and broadcasts it to all connected clients.
 */
void AppWIFI::broadcastWifiStatus(String message)
{
	if(WifiStation.isConnected() || WifiAccessPoint.isEnabled()) {
		JsonRpcMessage msg(F("wifi_status"));
		JsonObject root = msg.getParams();

		if(message != "") {
			root[F("message")] = message;
		}

		JsonObject station = root.createNestedObject(F("station"));

		station[F("connected")] = WifiStation.isConnected();
		station[F("ssid")] = WifiStation.getSSID();
		station[F("dhcp")] = WifiStation.isEnabledDHCP();
		station[F("ip")] = WifiStation.getIP().toString();
		station[F("netmask")] = WifiStation.getNetworkMask().toString();
		station[F("gateway")] = WifiStation.getNetworkGateway().toString();
		station[F("mac")] = WifiStation.getMAC();

		JsonObject ap = root.createNestedObject("ap");

		ap[F("enabled")] = WifiAccessPoint.isEnabled();
		ap[F("ssid")] = WifiAccessPoint.getSSID();
		ap[F("ip")] = WifiAccessPoint.getIP().toString();

		debug_i(ANSI_COLOR_BLUE "rpc: root =" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, Json::serialize(root).c_str());

		String jsonStr;
		jsonStr.reserve(384); // Pre-allocate buffer space to avoid repeated heap reallocations
		Json::serialize(msg.getRoot(), jsonStr);

		// Single debug logging statement using the already serialized string
		debug_i(ANSI_COLOR_BLUE "rpc: root =" ANSI_COLOR_CYAN "%s" ANSI_COLOR_RESET, jsonStr.c_str());

		// Broadcast
    	app.wsBroadcast(jsonStr);
	}
}

void AppWIFI::broadcastWifiStatus()
{
	broadcastWifiStatus("");
}