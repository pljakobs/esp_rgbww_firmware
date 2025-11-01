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

            if ((unsigned char)read_ptr[0] < 0x80) {
                cp = read_ptr[0];
                len = 1;
            } else if ((unsigned char)read_ptr[0] >= 0xC2 && (unsigned char)read_ptr[0] <= 0xDF) {
                if ((unsigned char)read_ptr[1] >= 0x80 && (unsigned char)read_ptr[1] <= 0xBF) {
                    cp = ((read_ptr[0] & 0x1F) << 6) | (read_ptr[1] & 0x3F);
                    len = 2;
                }
            } else if ((unsigned char)read_ptr[0] == 0xE1 && (unsigned char)read_ptr[1] == 0xBA && (unsigned char)read_ptr[2] == 0x9E) {
                cp = 0x1E9E; // ẞ
                len = 3;
            }


            if (len == 0) { // Invalid UTF-8 sequence
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

// Define service classes FIRST before using them
class LEDControllerAPIService : public mDNS::Service {
    public:
        void setLeader(bool isLeader) {
            _isLeader = isLeader;
        }
        
        void setGroups(const Vector<String>& groups) {
            _groups = groups;
        }
        
        void setLeadingGroups(const Vector<String>& groups) {
            _leadingGroups = groups;
        }
        
        String getInstance() override { return F("esprgbwwAPI"); }
        String getName() override { return F("http"); }
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
            txt.add(F("fn=LED Controller API"));
            char idStr[16];
            snprintf(idStr, sizeof(idStr), "id=%u", system_get_chip_id());
            txt.add(idStr);
            
            // Add leader status if this is the leader
            if (_isLeader) {
                txt.add(F("isLeader=1"));
            }
            
            // Add groups information
            if (_groups.size() > 0) {
                String groupList = "";
                for (size_t i = 0; i < _groups.size(); i++) {
                    if (i > 0) groupList += ",";
                    groupList += _groups[i];
                }
                txt.add(F("groups=") + groupList);
            }
            
            // Add leading groups
            for (size_t i = 0; i < _leadingGroups.size(); i++) {
                txt.add(F("leads_") + _leadingGroups[i] + "=1");
            }
        }
        
    private:
        bool _isLeader = false;
        Vector<String> _groups;
        Vector<String> _leadingGroups;
    };


class LEDControllerWebService : public mDNS::Service {
    public:
        enum class HostType {
            Device,  // Regular device hostname
            Leader,  // Global leader (lightinator)
            Group    // Group hostname
        };
    
        LEDControllerWebService(const String& instance = "lightinator", HostType type = HostType::Device) 
            : _instance(instance), _hostType(type) {}
        
        void setInstance(const String& instance) {
            _instance = instance;
        }
        
        String getInstance() override { return _instance; }
        String getName() override { return F("http"); }
        Protocol getProtocol() override { return Protocol::Tcp; }
        uint16_t getPort() override { return 80; }
        
        void addText(mDNS::Resource::TXT& txt) override {
            txt.add(F("fn=LED Controller"));
            txt.add(F("instance=") + _instance);
            char idStr[16];
            snprintf(idStr, sizeof(idStr), "id=%u", system_get_chip_id());
            txt.add(idStr);
            
            // Add type indicator
            switch (_hostType) {
                case HostType::Device:
                    txt.add(F("type=host"));
                    break;
                case HostType::Leader:
                    txt.add(F("type=leader"));
                    break;
                case HostType::Group:
                    txt.add(F("type=group"));
                    break;
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
    const char* service = "_http._tcp.local";
    int _mdnsTimerInterval = 10000;
    int _mdnsPingInterval = 10000; // Ping every minute
    int conntrack = 0;

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
    LEDControllerAPIService ledControllerAPIService; // For API discovery
    std::unique_ptr<LEDControllerWebService> deviceWebService;   // For hostname.local
    std::unique_ptr<LEDControllerWebService> leaderWebService;   // For lightinator.local
    // Hostname resolution handling
    std::map<String, String> _pendingHostnameResolutions;

    // Process different types of mDNS responses
    bool processApiServiceResponse(mDNS::Message& message);
    bool processHostnameARecord(mDNS::Message& message, mDNS::Answer* a_answer);
    bool processHostnameResponse(mDNS::Message& message, const char* hostname);

    void pingController(const char* ipAddress);
    int pingCallback(HttpConnection& connection, bool successful);

};


