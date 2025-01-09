#include <ArduinoJson.h>
#include <mdnshandler.h>
#include <RGBWWCtrl.h>
#include "application.h"


// ...

mdnsHandler::mdnsHandler(){};

void mdnsHandler::start()
{
	using namespace mDNS;
	static mDNS::Responder responder;

	static LEDControllerAPIService ledControllerAPIService;
	//static LEDControllerWebAppService ledControllerWebAppService;
	///static LEDControllerWSService ledControllerWSService;

	//start the mDNS responder with the configured services, using the configured hostname
	{
		AppConfig::Network network(*app.cfg);
		responder.begin(network.mdns.getName().c_str());
	} // end of ConfigDB network context

	  //responder.begin(app.cfg.network.connection.mdnshostname.c_str());
	responder.addService(ledControllerAPIService);
	//responder.addService(ledControllerWebAppService);
	//responder.addService(ledControllerWSService);

	//create an empty hosts array to store recieved host entries
	//hosts = hostsDoc.createNestedArray("hosts");
	
	{
		AppData::Controllers controllers(*app.data);
		auto controllersUpdate = controllers.update();
		controllersUpdate.clear();
		
	} // end of ConfigDB controllers context

	//serch for the esprgbwwAIP service. This is used in the onMessage handler to filter out unwanted messages.
	//to fulter for a number of services, this would have to be adapted a bit.
	setSearchName(F("esprgbwwAPI.") + service);

	//query mDNS at regular intervals
	_mdnsSearchTimer.setCallback(mdnsHandler::sendSearchCb, this);
	_mdnsSearchTimer.setIntervalMs(_mdnsTimerInterval);
	_mdnsSearchTimer.startOnce();
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
#ifdef DEBUG_MDNS
	debug_i("onMessage handler called");
#endif
	using namespace mDNS;

	// Check if we're interested in this message
	if(!message.isReply()) {
#ifdef DEBUG_MDNS
		debug_i("Ignoring query");
#endif
		return false;
	}

	#ifdef MDNS_DEBUG
		mDNS::printMessage(Serial, message);
	#endif

	auto answer = message[mDNS::ResourceType::SRV];
	if(answer == nullptr) {
#ifdef DEBUG_MDNS
		debug_i("Ignoring message: no SRV record");
#endif
		return false;
	}
	#ifdef MDNS_DEBUG
		mDNS::printMessage(Serial, message);
	#endif
	String answerName = String(answer->getName());
#ifdef DEBUG_MDNS
	debug_i("\nanswerName: %s\nsearchName: %s", answerName.c_str(), searchName.c_str());
#endif
	if(answerName != searchName) {
		//debug_i("Ignoring message: Name doesn't match");
		return false;
	}
#ifdef DEBUG_MDNS
	debug_i("Found matching SRV record");
	debug_i(">>>> db writes / s: %f", ((db_writes*100)/app.getUptime())/(float)100);
#endif
	// Extract our required information from the message
	struct {
		String hostName;
		IpAddress ipAddr;
		String Model;
		unsigned int	id;
		int ttl;
	} info;

	answer = message[mDNS::ResourceType::A];
	if(answer != nullptr) {
		info.hostName = String(answer->getName());
		info.hostName = info.hostName.substring(0, info.hostName.lastIndexOf(".local"));
		info.ipAddr = String(answer->getRecordString());
		info.ttl = answer->getTtl();
		
	}
	answer = message[mDNS::ResourceType::TXT];
	if(answer!=nullptr){
		debug_i("mDNS ressource type TXT");
		mDNS::Resource::TXT txt(*answer);
		//mDNS::printAnswer(Serial, *answer);
		info.id=txt["id"].toInt();
		info.Model=txt["mo"];
	}

#ifdef DEBUG_MDNS
	debug_i("found Host %s with IP %s and TTL %i", info.hostName.c_str(), info.ipAddr.toString().c_str(), info.ttl);
#endif

	if(info.Model!="" && info.id >0) 
		addHost(info.hostName, info.ipAddr.toString(), info.Model, info.id, info.ttl); //add host to list
	return true;
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
	bool ok = mDNS::server.search(service);
#ifdef DEBUG_MDNS
	debug_i("search('%s'): %s", service.c_str(), debug_i("known host: %s", hostname.c_str());ok ? "OK" : "FAIL");
#endif

	//restart the timer
	_mdnsSearchTimer.startOnce();
	{
		AppData::Controllers controllers(*app.data);
		auto controllersUpdate=controllers.update();
		for (auto it = controllersUpdate.begin(); it; ++it) {
			auto controller = *it;
			if(controller.getTtl()>0){
				int lastSeen=controller.getLastSeen();
				if(app.getUptime()-lastSeen>controller.getTtl()){
					debug_i("+---------------------------------+");
					debug_i("removing controller %s, last seen %is ago, ttl %is", controller.getName().c_str(),app.getUptime()-lastSeen, controller.getTtl());
					debug_i(">>>> db writes / s: %f", ((db_writes*100)/app.getUptime())/(float)100);
					debug_i("+---------------------------------+");
					
		
					JsonRpcMessage msg(F("removed_host"));
					JsonObject root = msg.getParams();
					root[F("hostname")] = controller.getName();
					String jsonStr = Json::serialize(msg.getRoot());
					app.wsBroadcast(jsonStr);
					
					controllersUpdate.removeItem(it.index());
					db_writes++;
					continue;
				}
			}
		}
	}
}

void mdnsHandler::sendSearchCb(void* pTimerArg)
{
#ifdef DEBUG_MDNS
	debug_i("sendSearchCb called");
#endif
	mdnsHandler* pThis = static_cast<mdnsHandler*>(pTimerArg);
	pThis->sendSearch();
}

void mdnsHandler::addHost(const String& hostname, const String& ip_address, const String& model, unsigned int id, int ttl)
{
    AppData::Controllers controllers(*app.data);
    auto controllersUpdate = controllers.update();
    bool knownHost = false;

    for (auto controller : controllersUpdate) {
        if (controller.getCid() == id) {
            knownHost = true;
            break;
        }
    }

    if (!knownHost) {
        debug_i("+---------------------------------+");
        debug_i("new host: %s", hostname.c_str());
		debug_i("adding to database");
		debug_i(">>>> db writes / s: %f", ((db_writes*100)/app.getUptime())/(float)100);
        debug_i("+---------------------------------+");
		if (id != 0 && hostname != "" ){
			auto controller = controllersUpdate.addItem();
			controller.setName(hostname);
			controller.setIpAddress(ip_address);
			controller.setModel(model);
			controller.setCid(id);
			controller.setTtl(ttl);
			controller.setLastSeen(app.getUptime());

			StaticJsonDocument<200> hosts;
			JsonObject newHost = hosts.createNestedObject();

			newHost[F("hostname")] = hostname;
			newHost[F("ip_address")] = ip_address;
			newHost[F("ttl")] = ttl;
			String newHostString;
			serializeJsonPretty(newHost, newHostString);
	#ifdef DEBUG_MDNS
			debug_i("new host: %s", newHostString.c_str());
	#endif
		
        JsonRpcMessage msg(F("new_host"));
        JsonObject root = msg.getParams();
        root.set(newHost);
        String jsonStr = Json::serialize(msg.getRoot());

        app.wsBroadcast(jsonStr);

		db_writes++;
		}
    } else {
        // Update the existing controller's information
        for (auto controller : controllersUpdate) {
            if (controller.getCid() == id) {
				if(app.getUptime()-controller.getLastSeen()>controller.getTtl()/2) {// only update if half ttl has elapsed to minimize flash writes
					debug_i("+---------------------------------+");
					debug_i("known host: %s", hostname.c_str());
					debug_i("updating last seen");
					debug_i(">>>> db writes / s: %f", ((db_writes*100)/app.getUptime())/(float)100);
					debug_i("+---------------------------------+");
					controller.setLastSeen(app.getUptime());
					db_writes++;
					break;
				}
            }
        }
    }
}

