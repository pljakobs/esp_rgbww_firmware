#pragma once
#include <Network/Mdns/Handler.h>
#include <Network/Mdns/Message.h>
#include <Network/Mdns/Resource.h>
#include <Network/Mdns/Responder.h>
#include <Network/Mdns/debug.h>
#include <SimpleTimer.h>
#include "/opt/sming/Sming/Services/HexDump/HexDump.h"
#include <sstream>
//#include <vector>
class mdnsHandler: public mDNS::Responder {
    public:
        mdnsHandler();
        virtual ~mdnsHandler(){};
        void start();
        void setSearchName(const String name){
            debug_i("setSearchName called with %s", name.c_str());
            searchName=name;
        }
        bool onMessage(mDNS::Message& message);
        
        void printType(const std::string&) {
    debug_i("Type of answer->getName(): std::string");
    }

    void printType(const char*) {
        debug_i("Type of answer->getName(): const char*");
    }
    
    private:
        SimpleTimer _mdnsSearchTimer;        
        String searchName;
        String searchService;
        int _mdnsTimerInterval = 30000; //search every 10 seconds
        std::vector<std::vector<String>> records;
        
        HexDump hd;
        
        static void sendSearchCb(void* pTimerArg);
        std::vector<std::vector<String>> parseMdnsRecordString(const String& recordString);
        void sendSearch();
    
};

class LEDControllerAPIService : public mDNS::Service{
    public:

        String getInstance() override{
		    return F("esprgbwwAPI");
        }
        String getName() override{
		    return F("http");
        }
        Protocol getProtocol() override{
		    return Protocol::Tcp;
	    }
        uint16_t getPort() override{
		    return 80;
    	};
	    void addText(mDNS::Resource::TXT& txt) override{
            txt.add("ty=rgbwwctrl");
            #ifdef ESP8266
            txt.add("mo=esp8266");
            #elif defined(ESP32)
            txt.add("mo=esp32");
            #endif
            txt.add("fn=LED Controller API");
            txt.add("id=" + String(system_get_chip_id()));
            txt.add("path=/");
        }
    private:
};

class LEDControllerWebAppService : public mDNS::Service{
    public:

        String getInstance() override{
		    return F("esprgbwwWebApp");
        }
        String getName() override{
		    return F("http");
        }
        Protocol getProtocol() override{
		    return Protocol::Tcp;
	    }
        uint16_t getPort() override{
		    return 80;
    	};
	
    void addText(mDNS::Resource::TXT& txt) override{
            txt.add("ty=rgbwwctrl");
            #ifdef ESP8266
            txt.add("mo=esp8266");
            #elif defined(ESP32)
            txt.add("mo=esp32");
            #endif
            txt.add("fn=LED Controller WebApp");
            txt.add("id=" + String(system_get_chip_id()));
            txt.add("path=/webapp");
        }
    private:
};

class LEDControllerWSService : public mDNS::Service{
    public:

        String getInstance() override{
		    return F("esprgbwwWS");
        }
        String getName() override{
		    return F("ws");
        }
        Protocol getProtocol() override{
		    return Protocol::Tcp;
	    }
        uint16_t getPort() override{
		    return 80;
    	};
	
    void addText(mDNS::Resource::TXT& txt) override{
            txt.add("ty=rgbwwctrl");
            #ifdef ESP8266
            txt.add("mo=esp8266");
            #elif defined(ESP32)
            txt.add("mo=esp32");
            #endif
            txt.add("fn=LED Controller");
            txt.add("id=" + String(system_get_chip_id()));
            txt.add("path=/ws");
        }
    private:
};