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

#include <ArduinoJson.h>
#include <Network/Mdns/Handler.h>
#include <Network/Mdns/Message.h>
#include <SimpleTimer.h>
#include <Network/Mdns/Resource.h>
#include <Network/Mdns/Responder.h>
#include <Network/Mdns/debug.h>
#include <map>
#include <memory>
#include <controllers.h>

#pragma once

#define JSON_SIZE 2048
#define LEADERSHIP_MAX_FAIL_COUNT 4
#define LEADER_ELECTION_DELAY 2

#define TTL_MDNS 60
#define TTL_HTTP_VERIFIED 600

namespace Util {
    inline void sanitizeHostname(char* str, const size_t bufferSize)
    {
        if (!str || bufferSize == 0) {
            return;
        }

        char* write_ptr = str;
        const char* read_ptr = str;
        const char* end_of_buffer = str + bufferSize - 1;

        bool last_char_was_hyphen = true;

        while (*read_ptr != '\0' && write_ptr < end_of_buffer) {
            uint32_t cp = 0;
            int len = 0;

            const unsigned char* p = (const unsigned char*)read_ptr;
            size_t remaining = (str + bufferSize) - read_ptr;

            if (remaining > 0 && p[0] < 0x80) { // 0xxxxxxx
                cp = p[0];
                len = 1;
            } else if (remaining > 1 && (p[0] & 0xE0) == 0xC0) { // 110xxxxx 10xxxxxx
                if ((p[1] & 0xC0) == 0x80) {
                    cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                    if (cp >= 0x80) { // Check for overlong encoding
                        len = 2;
                    }
                }
            } else if (remaining > 2 && (p[0] & 0xF0) == 0xE0) { // 1110xxxx 10xxxxxx 10xxxxxx
                if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                    cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                    if (cp >= 0x800) { // Check for overlong encoding
                        len = 3;
                    }
                }
            } else if (remaining > 3 && (p[0] & 0xF8) == 0xF0) { // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
                if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                    cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                    if (cp >= 0x10000) { // Check for overlong encoding
                        len = 4;
                    }
                }
            }

            if (len == 0) { // Invalid or incomplete UTF-8 sequence
                read_ptr++;
                continue;
            }

            const char* replacement = nullptr;

            switch (cp) {
                case 0xE4: case 0xC4: replacement = "ae"; break; // ä, Ä
                case 0xF6: case 0xD6: replacement = "oe"; break; // ö, Ö
                case 0xFC: case 0xDC: replacement = "ue"; break; // ü, Ü
                case 0xDF:           replacement = "ss"; break; // ß
                case 0x1E9E:         replacement = "ss"; break; // ẞ (U+1E9E) -> ss
                case 0xE9: case 0xE8: case 0xEA: case 0xEB: case 0xC9: case 0xC8: case 0xCA: case 0xCB: replacement = "e"; break;
                case 0xE1: case 0xE0: case 0xE2: case 0xE3: case 0xC1: case 0xC0: case 0xC2: case 0xC3: replacement = "a"; break;
                case 0xED: case 0xEC: case 0xEE: case 0xCD: case 0xCC: case 0xCE: replacement = "i"; break;
                case 0xF3: case 0xF2: case 0xF4: case 0xF5: case 0xD3: case 0xD2: case 0xD4: case 0xD5: replacement = "o"; break;
                case 0xFA: case 0xF9: case 0xFB: case 0xDA: case 0xD9: case 0xDB: replacement = "u"; break;
                case 0xF1: case 0xD1: replacement = "n"; break; // ñ, Ñ
                case 0xE7: case 0xC7: replacement = "c"; break; // ç, Ç
                case 0xE5: case 0xC5: replacement = "a"; break; // å, Å
                case 0xF8: case 0xD8: replacement = "o"; break; // ø, Ø
                case 0xE6: case 0xC6: replacement = "ae"; break; // æ, Æ
                case 0x142: case 0x141: replacement = "l"; break; // ł, Ł
                case 0x17C: case 0x17B: replacement = "z"; break; // ż, Ż
                case 0x17A: case 0x179: replacement = "z"; break; // ź, Ź
                case 0x107: case 0x106: replacement = "c"; break; // ć, Ć
            }

            if (replacement) {
                while (*replacement && write_ptr < end_of_buffer) {
                    *write_ptr++ = *replacement++;
                    last_char_was_hyphen = false;
                }
            } else if (cp >= 'a' && cp <= 'z' || cp >= '0' && cp <= '9') {
                *write_ptr++ = cp;
                last_char_was_hyphen = false;
            } else if (cp >= 'A' && cp <= 'Z') {
                *write_ptr++ = tolower(cp);
                last_char_was_hyphen = false;
            } else if (cp == ' ' || cp == '_' || cp == '-') {
                if (!last_char_was_hyphen) {
                    *write_ptr++ = '-';
                    last_char_was_hyphen = true;
                }
            }
            
            read_ptr += len;
        }

            if (write_ptr > str && *(write_ptr - 1) == '-') {
                write_ptr--;
            }

            *write_ptr = '\0';

            if (strlen(str) > 63) {
                str[63] = '\0';
                if (strlen(str) > 0 && str[strlen(str)-1] == '-') {
                    str[strlen(str)-1] = '\0';
                }
            }

            if (strlen(str) == 0) {
                if (bufferSize > strlen("lightinator")+12) {
                    snprintf(str, bufferSize, "lightinator-%d", system_get_chip_id());
            }
        }
    }
    inline String sanitizeHostname(const String& input) {
        char buffer[128];
        input.toCharArray(buffer, sizeof(buffer));
        sanitizeHostname(buffer, sizeof(buffer));
        return String(buffer);
    }
} // namespace Util

// ---------------------------------------------------------------------------
// Service class definitions
// ---------------------------------------------------------------------------

/**
 * _lightinator-api._tcp  —  machine-readable API endpoint
 * Browsed by: Home Assistant, FHEM, ioBroker, lightinator-log-service.
 * Carries only the minimum needed to reach the REST API; no swarm metadata.
 */
class LEDControllerAPIService : public mDNS::Service {
public:
    String getInstance() override { return F("esprgbwwAPI"); }
    String getName() override { return F("lightinator-api"); }
    Protocol getProtocol() override { return Protocol::Tcp; }
    uint16_t getPort() override { return 80; }

    void addText(mDNS::Resource::TXT& txt) override {
#ifdef ESP8266
        txt.add(F("mo=esp8266"));
#elif defined(ESP32)
        txt.add(F("mo=esp32"));
#elif defined(ESP32C3)
        txt.add(F("mo=esp32c3"));
#endif
        char idStr[16];
        snprintf(idStr, sizeof(idStr), "id=%u", system_get_chip_id());
        txt.add(idStr);
        txt.add(F("path=/"));
        txt.add(F("v=2"));
    }
};

/**
 * _lightinator._tcp  —  controller-to-controller swarm gossip
 * Browsed by: other Lightinator controllers only.
 * Carries swarm topology metadata (leader status, group membership).
 */
class LEDControllerSwarmService : public mDNS::Service {
public:
    void setInstance(const String& instance) { _instance = instance; }
    void setLeader(bool isLeader)             { _isLeader = isLeader; }
    void setGroups(const Vector<String>& g)   { _groups = g; }
    void setLeadingGroups(const Vector<String>& g) { _leadingGroups = g; }

    String getInstance() override { return _instance; }
    String getName() override { return F("lightinator"); }
    Protocol getProtocol() override { return Protocol::Tcp; }
    uint16_t getPort() override { return 80; }

    void addText(mDNS::Resource::TXT& txt) override {
        char idStr[16];
        snprintf(idStr, sizeof(idStr), "id=%u", system_get_chip_id());
        txt.add(idStr);
        txt.add(_isLeader ? F("isLeader=1") : F("isLeader=0"));
        if (_groups.size() > 0) {
            String groupList;
            for (size_t i = 0; i < _groups.size(); i++) {
                if (i > 0) groupList += ",";
                groupList += _groups[i];
            }
            txt.add(F("groups=") + groupList);
        }
        for (size_t i = 0; i < _leadingGroups.size(); i++) {
            txt.add(F("leads_") + _leadingGroups[i] + "=1");
        }
    }

private:
    String _instance;
    bool _isLeader = false;
    Vector<String> _groups;
    Vector<String> _leadingGroups;
};


/**
 * _http._tcp  —  human-facing web frontend
 * Browsed by: browsers, avahi-browse, webapp discovery UI.
 * One instance per role: device hostname, group alias, global "lightinator".
 * Carries only human-readable labels; no swarm topology.
 */
class LEDControllerWebService : public mDNS::Service {
public:
    enum class HostType {
        Device,  // <hostname>.local
        Leader,  // lightinator.local
        Group    // <groupname>.local
    };

    LEDControllerWebService(const String& instance = "lightinator", HostType type = HostType::Device)
        : _instance(instance), _hostType(type) {}

    void setInstance(const String& instance) { _instance = instance; }

    String getInstance() override { return _instance; }
    String getName() override { return F("http"); }
    Protocol getProtocol() override { return Protocol::Tcp; }
    uint16_t getPort() override { return 80; }

    void addText(mDNS::Resource::TXT& txt) override {
        txt.add(F("fn=") + _instance);
        char idStr[16];
        snprintf(idStr, sizeof(idStr), "id=%u", system_get_chip_id());
        txt.add(idStr);
        switch (_hostType) {
            case HostType::Device: txt.add(F("type=host"));   break;
            case HostType::Leader: txt.add(F("type=leader")); break;
            case HostType::Group:  txt.add(F("type=group"));  break;
        }
    }

private:
    String _instance;
    HostType _hostType;
};
/**
 * @class mdnsHandler
 * @brief A class that handles mDNS (Multicast DNS) functionality.
 * 
 * This class provides:
 * 1. Controller discovery through LEDControllerAPIService
 * 2. Web access via hostname.local for every controller
 * 3. Web access via lightinator.local for the global leader
 * 4. Web access via groupname.local for group leaders
 */
class mdnsHandler: public mDNS::Handler {  
public:
    mdnsHandler();
    virtual ~mdnsHandler() ;
    
    /**
     * @brief Initialize and start mDNS services

     */
    void start();
    
    /**
     * @brief Set the search name for discovering other controllers
     */
    void setSearchName(const char* name);
    void setSearchName(const String& name)
    {
        setSearchName(name.c_str());
    }

    /**
     * @brief Set the hostname for this device
     * @param newHostname The new hostname to use
     */
    void setHostname(const char* newHostname);
    void setHostname(const String& newHostname)
    {
        setHostname(newHostname.c_str());
    }

    /**
     * @brief Process incoming mDNS messages
     * @param message The incoming message
     * @return true if the message was handled, false otherwise
     */
    bool onMessage(mDNS::Message& message) override;

    /**
     * @brief Add a discovered host to the list
     */
    void addHost(const char* hostname, const char* ip_address, int ttl, unsigned int id);

    /**
     * @brief Send WebSocket update about discovered hosts
     */
    void sendWsUpdate(const char* type, JsonObject host);

    /**
     * @brief Set or clear group leadership status
     * @param enable true to become leader, false to relinquish leadership
     */
    void checkGroupLeadership();


private:
    // Responders - each handles a different hostname
    std::unique_ptr<mDNS::Responder> primaryResponder; // hostname.local
    std::unique_ptr<mDNS::Responder> leaderResponder;  // lightinator.local

    // Discovery
    SimpleTimer _mdnsSearchTimer;
    SimpleTimer _pingTimer;
    String searchName;
    // Swarm gossip service type — controllers browse this exclusively
    const char* service = "_lightinator._tcp.local";
    int _mdnsTimerInterval = 15000; // Increased from 10000
    int _mdnsPingInterval = 10000; // Ping every minute
    int conntrack = 0;
    int _currentMdnsTimerInterval;
    unsigned long _lastMessageTime = 0;
    int _messageCount = 0;

    // Global leadership
    bool _isLeader = false;
    bool _leaderDetected = false;
    SimpleTimer _leaderElectionTimer;
    uint8_t _leaderCheckCounter = 0;

    void updateServiceTxtRecords();
    void becomeGroupLeader(const char* groupId, const char* groupName);
    void relinquishGroupLeadership(const char* groupId);

    // Track group leadership
    Vector<String> _leadingGroups;
    std::map<String, std::unique_ptr<mDNS::Responder>> _groupResponders;
    std::map<String, std::unique_ptr<LEDControllerWebService>> _groupWebServices;

    // Leadership methods
    void checkForLeadership();
    static void checkForLeadershipCb(void* pTimerArg);
    void becomeLeader();
    void relinquishLeadership();

    // Discovery methods
    static void sendSearchCb(void* pTimerArg);
    void sendSearch();
    // void queryKnownControllers(uint8_t batchIndex);

    // Service instances
    LEDControllerAPIService  ledControllerAPIService;  // _lightinator-api._tcp: external tools
    LEDControllerSwarmService ledControllerSwarmService; // _lightinator._tcp: swarm gossip
    std::unique_ptr<LEDControllerWebService> deviceWebService;   // _http._tcp: hostname.local
    std::unique_ptr<LEDControllerWebService> leaderWebService;   // _http._tcp: lightinator.local
    // Hostname resolution handling
    //std::map<String, String> _pendingHostnameResolutions;

    // Process different types of mDNS responses
    bool processSwarmServiceResponse(mDNS::Message& message); // _lightinator._tcp replies
    bool processHostnameARecord(mDNS::Message& message, mDNS::Answer* a_answer);
    bool processHostnameResponse(mDNS::Message& message, const char* hostname);

    void pingController(const char* ipAddress);
    int pingCallback(HttpConnection& connection, bool successful);

};


