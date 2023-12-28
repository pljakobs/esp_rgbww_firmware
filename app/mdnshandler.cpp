#include <ArduinoJson.h>
#include <RGBWWCtrl.h>
// #include <cctype> //for debugging only


// ...

mdnsHandler::mdnsHandler(){};


void mdnsHandler::start()
{
    static mDNS::Responder responder;

    searchService = F("esprgbwwAPI._http._tcp.local");

    static LEDControllerAPIService ledControllerAPIService;
    static LEDControllerWebAppService ledControllerWebAppService;  
    static LEDControllerWSService ledControllerWSService;

    debug_i("staring mDNS responder with hostname %s", app.cfg.network.connection.mdnshostname.c_str());
	responder.begin(app.cfg.network.connection.mdnshostname.c_str());
	responder.addService(ledControllerAPIService);
    responder.addService(ledControllerWebAppService);
    responder.addService(ledControllerWSService);

    _mdnsSearchTimer.setCallback(mdnsHandler::sendSearchCb, this);
    _mdnsSearchTimer.setIntervalMs(_mdnsTimerInterval);
    //_mdnsSearchTimer.startOnce();
    debug_i("starting mDNS search timer for service %s", searchService.c_str());
    sendSearch();
    
    mDNS::server.addHandler(*this);
}

/**
 * @brief Handles an mDNS message.
 *
 * This function is responsible for processing an mDNS message and extracting the required information from it.
 * It checks if the message is a reply and if it contains a PTR record with a matching name.
 * If the message contains TXT and A records, it extracts the instance, service, and IP address information.
 * It then creates a JSON object with the extracted information and serializes it.
 *
 * @param message The mDNS message to be handled.
 * @return True if the message was successfully handled, false otherwise.
 */
bool mdnsHandler::onMessage(mDNS::Message& message)
{
    debug_i("onMessage handler called");
    using namespace mDNS;

    // Check if we're interested in this message
    if(!message.isReply()) {
        debug_i("received query, ignoring");
        return false;
    }
    
    mDNS::printMessage(Serial, message);

    //auto answer = message[mDNS::ResourceType::PTR];
    //if(answer == nullptr) {
    //    debug_i("Ignoring message: no PTR record");
    //    return false;
    //}
String hostName="";
String address="";
for(auto& myAnswer : message.answers) {
    String name = myAnswer.getName();
    mDNS::ResourceType type = myAnswer.getType();
    debug_i("name ==> checking %s <==", name.c_str());
    if(type==mDNS::ResourceType::SRV){
        debug_i("is SRV record, checking if name is %s",searchName.c_str());
        if(name==searchName ){
            debug_i("name matches, getting hostname");
            records=parseMdnsRecordString(myAnswer.getRecordString());
    
            for (const auto& record : records) {
                String key = record[0];
                String value = record[1];
                // Do something with key and value
            }    
        }
    }
    if(type==mDNS::ResourceType::A){
        debug_i("is A record");
        if (name==hostName){
        address=myAnswer.getRecordString();
        debug_i("address ==> %s <==", address.c_str());
        }
    }
}

    //debug_i("...for Name (%s) of type %s",answer->getName());
    //debug_i("Address: %#010x", uint32_t(answer));
    //debug_i("going to compare with %s of size %i", searchName.c_str(), sizeof(searchName.c_str()));

    /*
    if(answer->getName() != searchName) {
        debug_i("Ignoring message: Name %s doesn't match %s", answer->getName().toString(),searchName.c_str());
        return false;
    }else{debug_i("Name matches");}
    
    // Extract our required information from the message
    */
    struct {
        String instance;
        String service;
        IpAddress ipaddr;
    } info;

    debug_i("...extracting info");
    /*
    answer = message[mDNS::ResourceType::TXT];
    if(answer != nullptr) {
        mDNS::Resource::TXT txt(*answer);
        info.instance = txt["md"];
        info.service = txt["fn"];
    }

    answer = message[mDNS::ResourceType::A];
    if(answer != nullptr) {
        mDNS::Resource::A a(*answer);
        info.ipaddr = a.getAddress();
    }
    */
    // Create a JSON object
    /*
    debug_i("...creating JSON object\nSize of component data: %i\n", sizeof(info.instance)+sizeof(info.service)+sizeof(info.ipaddr));
    StaticJsonDocument<200> doc;
    doc["hostname"] = info.instance;
    doc["ip_address"] = info.ipaddr.toString();
    doc["service"] = info.service;

    debug_i("=== Found service: %s at address %s ===", info.service.c_str(), info.ipaddr.toString().c_str());
*/
    // Serialize JSON document
  /*  String output;
    serializeJson(doc, output);
*/
    return true;
}

std::vector<std::vector<String>> mdnsHandler::parseMdnsRecordString(const String& recordString) {
    std::vector<std::vector<String>> result;
    std::istringstream iss(recordString.c_str());
    std::string item;

    while (std::getline(iss, item, ';')) {
        std::istringstream iss_item(item);
        std::string key;
        std::string value;

        if (std::getline(std::getline(iss_item, key, '='), value)) {
            result.push_back({String(key.c_str()), String(value.c_str())});
        }
    }

    return result;
}

/**
 * @brief Sends a search request for the specified services using mDNS.
 *
 * This function sends a search request for the services specified in the `services` array using mDNS.
 * It iterates through each service in the array and calls the `mDNS::server.search()` function to perform the search.
**/
void mdnsHandler::sendSearch()
{
    // Search for the service
        bool ok = mDNS::server.search(searchService);
        debug_i("########################");
        debug_i("search('%s'): %s", searchService.c_str(), ok ? "OK" : "FAIL");

	    setSearchName(searchService);

        //restart the timer
        _mdnsSearchTimer.startOnce();
}

void mdnsHandler::sendSearchCb(void* pTimerArg) {
    debug_i("sendSearchCb called");
    mdnsHandler* pThis = static_cast<mdnsHandler*>(pTimerArg);
    pThis->sendSearch();
}