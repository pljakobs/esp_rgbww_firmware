/**
 * @file
 * @author  Peter Jakobs http://github.com/pljakobs
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
 *
 */

#include <ArduinoJson.h>
#include <RGBWWCtrl.h>
#include <mdnsHandler.h>
#include "app-data.h"
#include <application.h>

extern Application app;

//ToDo: verify if mDNS with group names can be implemented with a single handler instance and multiple responders, or if we need to create separate handler instances for each group (potentially with shared responder logic) to properly manage group-specific state and avoid conflicts in service registration and message handling.


// No global pointer needed — swarm state is managed via the
// ledControllerSwarmService member of mdnsHandler directly.

namespace
{
// One reply can carry records for many instances, so records must be matched by owner name.
mDNS::Answer* findAnswer(mDNS::Message& message, mDNS::ResourceType type, const String& name)
{
    for(auto& ans : message.answers) {
        if(ans.getType() == type && ans.getName() == name) {
            return &ans;
        }
    }
    return nullptr;
}

bool hasSuffix(const char* name, const char* suffix)
{
    const char* p = strstr(name, suffix);
    return p != nullptr && p[strlen(suffix)] == '\0';
}
} // namespace

mdnsHandler::mdnsHandler() {
    // Initialize with default values
    _currentMdnsTimerInterval = _mdnsTimerInterval;
}

mdnsHandler::~mdnsHandler() {
    // Clean up primary responder
    if (primaryResponder) {
        mDNS::server.removeHandler(*primaryResponder);
    }

    // Clean up global leader responder
    if (leaderResponder) {
        mDNS::server.removeHandler(*leaderResponder);
    }
    
    // Clean up all group responders
    for (auto it = _groupResponders.begin(); it != _groupResponders.end(); ++it) {
        mDNS::server.removeHandler(*it->second);
    }
}

void mdnsHandler::setHostname(const char* newHostname)
{
    using namespace mDNS;

    // Relinquish all leadership roles before changing hostname
    relinquishLeadership();

    // Create a copy of the group IDs to avoid iterator invalidation only if needed
    std::vector<GroupId> groupsToRelinquish = _leadingGroups;
    for (const auto& g : groupsToRelinquish) {
        relinquishGroupLeadership(g.value);
    }

    // Sanitize the hostname
    char san_buf[128];
    strncpy(san_buf, newHostname, sizeof(san_buf));
    Util::sanitizeHostname(san_buf, sizeof(san_buf));

    // If there's an existing primary responder, remove it first
    if (primaryResponder) {
        server.removeHandler(*primaryResponder);
    }

    // Create device web service with proper hostname
    deviceWebService =
        std::make_unique<LEDControllerWebService>(san_buf, LEDControllerWebService::HostType::Device);

    // Update API and swarm service instance names so they match the hostname
    ledControllerAPIService.setInstance(san_buf);
    ledControllerSwarmService.setInstance(san_buf);

    // Create and configure the new primary responder
    primaryResponder = std::make_unique<Responder>();
    primaryResponder->begin(san_buf);

#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::setHostname: " ANSI_COLOR_BLUE "Registered hostname: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, san_buf);
#endif

    // Register all three services on the primary responder:
    //   _lightinator-api._tcp  — for HA / log-service / FHEM
    //   _lightinator._tcp      — for controller-to-controller swarm gossip
    //   _http._tcp             — for browser access via hostname.local
    primaryResponder->addService(ledControllerAPIService);
    primaryResponder->addService(ledControllerSwarmService);
    primaryResponder->addService(*deviceWebService);

    // Register the new handler with the mDNS server
    server.addHandler(*primaryResponder);
}

void mdnsHandler::setSearchName(const char* name)
{
    cdebug_i(MDNSHANDLER, "mdnsHandler::setSearchName: " ANSI_COLOR_BLUE "setting searchName to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, name);
    searchName = String(name);
}

void mdnsHandler::start()
{
    using namespace mDNS;

    cdebug_i(MDNSHANDLER, "mdnsHandler::start: " ANSI_COLOR_BLUE "########################################################" ANSI_COLOR_RESET);
    cdebug_i(MDNSHANDLER, "mdnsHandler::start: " ANSI_COLOR_BLUE "# mdns Handler initialized, Source Port: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ", TARGET Port: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, MDNS_SOURCE_PORT, MDNS_TARGET_PORT);
    cdebug_i(MDNSHANDLER, "mdnsHandler::start: " ANSI_COLOR_BLUE "########################################################" ANSI_COLOR_RESET);
    
    // Get device hostname from configuration and set it
    String hostName;
    {
        AppConfig::Network network(*app.cfg);
        hostName = network.mdns.getName();
        if (hostName.length() == 0) {
            // Generate hostname from MAC if not set
			char hostName_buf[64];
			snprintf(hostName_buf, sizeof(hostName_buf), "lightinator-%u", system_get_chip_id());
			hostName = hostName_buf;
        }
    }
    setHostname(hostName);

    checkGroupLeadership();
    
    // Store global reference for API service (kept for external callers)
    // Swarm service state is managed directly via ledControllerSwarmService member.

    // Set up leadership election with delay
    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::start: " ANSI_COLOR_BLUE "starting leader election timer timer" ANSI_COLOR_RESET);
    #endif 
    _leaderElectionTimer.setCallback(mdnsHandler::checkForLeadershipCb, this);
    _leaderElectionTimer.setIntervalMs(_mdnsTimerInterval * LEADER_ELECTION_DELAY);
    _leaderElectionTimer.startOnce();
    
    // Set search name for discovering other controllers via the swarm service type
    setSearchName(F("_lightinator._tcp.local"));
    
    // Set up timer for periodic mDNS searches
    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::start: " ANSI_COLOR_BLUE "starting mDNS search timer" ANSI_COLOR_RESET);
    #endif 
    _mdnsSearchTimer.setCallback(mdnsHandler::sendSearchCb, this);
    _mdnsSearchTimer.setIntervalMs(_currentMdnsTimerInterval);
    _mdnsSearchTimer.startOnce();

    // Register the main handler
    mDNS::server.addHandler(*this); 
    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::start: " ANSI_COLOR_BLUE "mDNS server started" ANSI_COLOR_RESET);
    #endif
}

bool mdnsHandler::onMessage(mDNS::Message& message)
{
    // update debug counter
    app._mDNS_received++;
    bool msgHasA = false, msgHasTXT = false;

    // Adaptive mDNS interval logic
    unsigned long now = millis();
    _messageCount++;
    if (now - _lastMessageTime > 1000) { // Check every second
        if (_messageCount > 30) { // High traffic
            _currentMdnsTimerInterval = min(_mdnsTimerInterval * 4, 300000); // Increase interval, max 300s
        } else if (_messageCount < 10) { // Low traffic
            _currentMdnsTimerInterval = max(_mdnsTimerInterval, _currentMdnsTimerInterval - 1000); // Decrease interval
        }
        _messageCount = 0;
        _lastMessageTime = now;
    }

#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::onMessage: " ANSI_COLOR_BLUE "onMessage handler called" ANSI_COLOR_RESET);
#endif
    using namespace mDNS;

    // Check if we're interested in this message
    if (!message.isReply()) {
#ifdef DEBUG_MDNS
        cdebug_i(MDNSHANDLER, "mdnsHandler::onMessage: " ANSI_COLOR_BLUE "Ignoring query" ANSI_COLOR_RESET);
#endif
        return false;
    }
    // update debug counter
    app._mDNS_replies++;

    bool hasSrv = false;
    bool handled = false;
    for(auto& srv_answer : message.answers) {
        if(srv_answer.getType() != mDNS::ResourceType::SRV) {
            continue;
        }
        hasSrv = true;
        const String answerNameString = String(srv_answer.getName());
        const char* answerName = answerNameString.c_str();

        if(hasSuffix(answerName, "._lightinator._tcp.local") || hasSuffix(answerName, "._wall-panel-api._tcp.local")) {
            handled |= processSwarmServiceResponse(message, srv_answer);
            continue;
        }
        const char* http_tcp_local = "._http._tcp.local";
        if(hasSuffix(answerName, http_tcp_local)) {
            const char* p = strstr(answerName, http_tcp_local);
            size_t hostname_len = p - answerName;
            // Bound the copy to a fixed buffer: answerName comes straight off
            // the network, so a VLA sized from it would let a remote peer decide
            // how much stack this frame consumes (unbounded alloca, CWE-789) —
            // dangerous on the shared SYS/network stack this runs on.  A single
            // mDNS label never exceeds 63 bytes; 64 covers the label + NUL.
            char hostname[64];
            if (hostname_len >= sizeof(hostname)) {
                hostname_len = sizeof(hostname) - 1;
            }
            memcpy(hostname, answerName, hostname_len);
            hostname[hostname_len] = '\0';

#ifdef DEBUG_MDNS
            cdebug_i(MDNSHANDLER, "mdnsHandler::onMessage: " ANSI_COLOR_BLUE "Processing hostname response for: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, hostname);
#endif
            handled |= processHostnameResponse(message, srv_answer, hostname);
        }
    }

    if(!hasSrv) {
        // Plain A record responses: each A record is resolved independently by its own name.
        for(auto& a_answer : message.answers) {
            if(a_answer.getType() == mDNS::ResourceType::A) {
                handled |= processHostnameARecord(message, &a_answer);
            }
        }
    }
    return handled;
}

// Process swarm service responses (_lightinator._tcp)
bool mdnsHandler::processSwarmServiceResponse(mDNS::Message& message, mDNS::Answer& srv)
{
    using namespace mDNS;

    const String instanceName = String(srv.getName());
    const String target = String(Resource::SRV(srv).getHost());

    // Hostname label = SRV target with the trailing ".local" removed. This is
    // available from the SRV answer alone, even when this packet carries no A
    // record, so it bridges TXT (id) and A (ip) fragments across packets.
    char hostName[64];
    strncpy(hostName, target.c_str(), sizeof(hostName) - 1);
    hostName[sizeof(hostName) - 1] = '\0';
    if (char* dot = strstr(hostName, ".local")) {
        *dot = '\0';
    }

    bool handled = false;

    // TXT fragment carries the immutable id (plus type and leader flag). It binds
    // the id to the hostname label so a later A record can complete the record.
    if (auto txt_answer = findAnswer(message, ResourceType::TXT, instanceName)) {
        mDNS::Resource::TXT txt(*txt_answer);
        unsigned int id = parseControllerId(txt["id"]);
        if (id != 0) {
            if (txt[F("isLeader")] == "1") {
                _leaderDetected = true;
#ifdef DEBUG_MDNS
                cdebug_i(MDNSHANDLER, "mdnsHandler::processSwarmServiceResponse: " ANSI_COLOR_BLUE "Detected leader: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RESET, hostName);
#endif
            }
            String hostnameType = txt[F("host_type")];
            if (hostnameType.length() == 0) {
                hostnameType = txt[F("type")];
            }
            const Controllers::HostType hostType = Controllers::hostTypeFromString(hostnameType);
            app.controllers->noteIdentity(id, hostName, txt_answer->getTtl(), hostType);
            handled = true;
        }
    }

    // A fragment carries the address; it completes the record once the id for
    // this hostname is known (or is staged until then).
    if (auto a_answer = findAnswer(message, ResourceType::A, target)) {
        String ip = a_answer->getRecordString();
        app.controllers->noteAddress(hostName, ip.c_str(), a_answer->getTtl());
        handled = true;
    }

    return handled;
}

// Process hostname A record responses
bool mdnsHandler::processHostnameARecord(mDNS::Message& message, mDNS::Answer* a_answer)
{
    using namespace mDNS;
    (void)message;

    // Extract hostname from A record
	String hostname_local = String(a_answer->getName());
    char hostname[64];
    strncpy(hostname, hostname_local.c_str(), sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';

    // Remove .local suffix if present
    char* dot = strstr(hostname, ".local");
    if (dot) {
        *dot = '\0';
    }

    // Get IP address from A record
    String ipAddress = a_answer->getRecordString();
    unsigned int ttl = a_answer->getTtl();
#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::processHostnameARecord: " ANSI_COLOR_BLUE "Got A record for hostname: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ", IP: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, hostname, ipAddress.c_str());
#endif

    // Feed the address fragment. If a TXT already bound this hostname to an id
    // the record completes immediately; otherwise the address is staged until a
    // TXT reveals the id.
    app.controllers->noteAddress(hostname, ipAddress.c_str(), ttl);
    return true;
}

// Process hostname responses with potential SRV records
bool mdnsHandler::processHostnameResponse(mDNS::Message& message, mDNS::Answer& srv, const char* hostname)
{
    using namespace mDNS;

    bool handled = false;

    // TXT fragment carries the immutable id (plus type).
    if (auto txt_answer = findAnswer(message, ResourceType::TXT, String(srv.getName()))) {
        mDNS::Resource::TXT txt(*txt_answer);
        unsigned int controllerId = parseControllerId(txt["id"]);
        if (controllerId != 0) {
            Controllers::HostType hostType = Controllers::hostTypeFromString(txt["host_type"]);
            if (hostType == Controllers::HOST_TYPE_UNKNOWN) {
                hostType = Controllers::hostTypeFromString(txt["type"]);
            }
            app.controllers->noteIdentity(controllerId, hostname, txt_answer->getTtl(), hostType);
            handled = true;
        }
    }

    // A fragment carries the address; completes the record once the id is known.
    if (auto a_answer = findAnswer(message, ResourceType::A, String(Resource::SRV(srv).getHost()))) {
        String ipAddress = a_answer->getRecordString();
        app.controllers->noteAddress(hostname, ipAddress.c_str(), a_answer->getTtl());
        handled = true;
    }

    return handled;
}

void mdnsHandler::sendSearch()
{
    static unsigned long lastLeaderCheck = 0;
    static uint8_t queryIndex = 0;
    unsigned long now = millis();

    // Search for the service
    bool ok = mDNS::server.search(service);
    bool wallPanelOk = mDNS::server.search(wallPanelService);
#ifndef DEBUG_MDNS
    (void)wallPanelOk;
#endif
#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::sendSearch: " ANSI_COLOR_BLUE "search('" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "'): " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, service, ok ? "OK" : "FAIL");
    cdebug_i(MDNSHANDLER, "mdnsHandler::sendSearch: " ANSI_COLOR_BLUE "search('" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "'): " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, wallPanelService, wallPanelOk ? "OK" : "FAIL");
#endif

    // Periodically check if there is still a leader in the network
    if (now - lastLeaderCheck > (_mdnsTimerInterval * LEADER_ELECTION_DELAY)) {
        lastLeaderCheck = now;

        // Reset leader detection status
        _leaderDetected = false;
        // If we're currently not a leader, schedule a check after the next search cycle
        if (!_isLeader) {
            _leaderElectionTimer.startOnce();
        }
    }

    static unsigned long lastGroupLeaderCheck = 0;
    if (now - lastGroupLeaderCheck > (2 * 60 * 1000)) { // 2 minutes
        lastGroupLeaderCheck = now;

        // Check group leadership again
        checkGroupLeadership();
    }
    // Restart the timer
    _mdnsSearchTimer.startOnce();
    app.controllers->removeExpired();
}

void mdnsHandler::sendSearchCb(void* pTimerArg) {
#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::sendSearchCb: " ANSI_COLOR_BLUE "sendSearchCb called" ANSI_COLOR_RESET);
#endif
    mdnsHandler* pThis = static_cast<mdnsHandler*>(pTimerArg);
    pThis->sendSearch();
}

void mdnsHandler::sendWsUpdate(const char* type, JsonObject host)
{
    String hostString;

    if (serializeJsonPretty(host, hostString)) {
        app.wsBroadcast(type, hostString.c_str());
    }
}

void mdnsHandler::checkForLeadership() {
    if (_leaderDetected) {
        #ifdef DEBUG_MDNS
        cdebug_i(MDNSHANDLER, "mdnsHandler::checkForLeadership: " ANSI_COLOR_BLUE "Leader already exists in network, not becoming leader" ANSI_COLOR_RESET);
        #endif
        _leaderCheckCounter = 0;  // Reset counter when a leader is detected
        return;
    }

    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::checkForLeadership: " ANSI_COLOR_BLUE "No leader detected (check round " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, _leaderCheckCounter + 1);
    #endif
    
    // Increment leader check counter
    _leaderCheckCounter++;
    
    // Check if this controller has highest ID
    unsigned int myId = system_get_chip_id();
    bool hasHighestId = true;
    
    // Check all visible controllers
    (myId>app.controllers->getHighestId()) ? hasHighestId=true : hasHighestId=false;
    
    // Become leader if we have highest ID OR we've checked max times with no leader
    if (hasHighestId || _leaderCheckCounter >= LEADERSHIP_MAX_FAIL_COUNT) {
        if (hasHighestId) {
            #ifdef DEBUG_MDNS
            cdebug_i(MDNSHANDLER, "mdnsHandler::checkForLeadership: " ANSI_COLOR_BLUE "No leader detected and we have highest ID, becoming leader" ANSI_COLOR_RESET);
            #endif
        } else {
            #ifdef DEBUG_MDNS
            cdebug_i(MDNSHANDLER, "mdnsHandler::checkForLeadership: " ANSI_COLOR_BLUE "No leader detected after " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE " checks, becoming leader as a failsafe" ANSI_COLOR_RESET, LEADERSHIP_MAX_FAIL_COUNT);
            #endif
        }
        
        becomeLeader();
        _leaderCheckCounter = 0;  // Reset counter
    } else {
        #ifdef DEBUG_MDNS
        cdebug_i(MDNSHANDLER, "mdnsHandler::checkForLeadership: " ANSI_COLOR_BLUE "Not becoming leader, another controller has higher ID (check " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "/" ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, 
                _leaderCheckCounter, LEADERSHIP_MAX_FAIL_COUNT);
        #endif

        // Start another check after a delay if we haven't reached the limit
        if (_leaderCheckCounter < LEADERSHIP_MAX_FAIL_COUNT) {
            _leaderElectionTimer.startOnce();
        }
    }
}

void mdnsHandler::checkForLeadershipCb(void* pTimerArg) {
    mdnsHandler* pThis = static_cast<mdnsHandler*>(pTimerArg);
    pThis->checkForLeadership();
}

void mdnsHandler::becomeLeader() {
    if (_isLeader) return;
    
    _isLeader = true;
    ledControllerSwarmService.setLeader(true);

    // Create new web service for the leader
    leaderWebService = std::make_unique<LEDControllerWebService>("lightinator", 
        LEDControllerWebService::HostType::Leader);
    // Create new responder for "lightinator.local"
    leaderResponder = std::make_unique<mDNS::Responder>();
    leaderResponder->begin("lightinator");
    
    // _http._tcp only: browsers resolve lightinator.local via this
    leaderResponder->addService(*leaderWebService);
    
    // Register the leader responder with mDNS server
    mDNS::server.addHandler(*leaderResponder);

    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::becomeLeader: " ANSI_COLOR_BLUE "This controller is now the global leader (lightinator.local)" ANSI_COLOR_RESET);
    #endif
}

void mdnsHandler::relinquishLeadership() {
    if (!_isLeader) return;
    
    _isLeader = false;
    ledControllerSwarmService.setLeader(false);

    // Remove leader responder from mDNS server
    if (leaderResponder) {
        mDNS::server.removeHandler(*leaderResponder);
        leaderResponder.reset();
    }
    
    // Clean up leader web service
    leaderWebService.reset();
    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::relinquishLeadership: " ANSI_COLOR_BLUE "This controller is no longer the global leader" ANSI_COLOR_RESET);
    #endif
}

void mdnsHandler::checkGroupLeadership() {
    // Step 1: Identify which groups we belong to (already implemented)
    uint32_t myId = system_get_chip_id();
    Vector<String> memberGroups;
    Vector<String> groupsToLead;
    
    // Get access to all groups and track our memberships
    AppData::Root::Groups groups(*app.data);

    // We can lead at most as many groups as exist; reserve up front so the
    // push_back()s in becomeGroupLeader() don't trigger reallocations.
    _leadingGroups.reserve(groups.getItemCount());

    // Build map of group ID -> group name for easier reference (avoid redundant String copies)
    std::map<String, String> groupNames;
    
    // Scan for our group memberships
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        auto& currentGroup = *it;
        String groupId = currentGroup.getId();
        String groupName = currentGroup.getName();
        groupNames[groupId] = groupName;
        
        // Check if we're a member of this group
        bool isMember = false;
        
        for (auto controllerIt = currentGroup.controllerIds.begin(); 
             controllerIt != currentGroup.controllerIds.end(); ++controllerIt) {
            if (String(*controllerIt).toInt() == myId) {
                isMember = true;
                memberGroups.add(groupId);
                break;
            }
        }
        
        if (isMember) {
            // Step 2: For each group we're a member of, check if we should be leader
            
            // Determine if we have the highest ID in the group
            bool hasHighestId = true;
            
            for (auto controllerIt = currentGroup.controllerIds.begin(); 
                 controllerIt != currentGroup.controllerIds.end(); ++controllerIt) {
                String controllerId = *controllerIt;
                
                // Skip ourselves
                if (controllerId.toInt() == myId) continue;
                
                // Convert string IDs to integers for comparison
                unsigned int theirId = controllerId.toInt();
                
                if (theirId > myId) {
                    hasHighestId = false;
                    break;
                }
            }
            
            // If we have the highest ID in this group, we should be the leader
            if (hasHighestId) {
                groupsToLead.add(groupId);
                #ifdef DEBUG_MDNS
                cdebug_i(MDNSHANDLER, "mdnsHandler::checkGroupLeadership: " ANSI_COLOR_BLUE "This device should be the leader for group: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, groupName.c_str());
                #endif
            }
        }
    }
    
    // Step 3: Set up leadership for groups where we should be leader
    for (size_t i = 0; i < groupsToLead.size(); i++) {
        const String& groupId = groupsToLead[i];
        String groupName = groupNames[groupId];

        // Only set up leadership if we're not already leader for this group
        bool alreadyLeader = false;
        for (const auto& g : _leadingGroups) {
            if (groupId == g.value) {
                alreadyLeader = true;
                break;
            }
        }
        if (!alreadyLeader) {
            becomeGroupLeader(groupId.c_str(), groupName.c_str());
        }
    }

    // Step 4: Relinquish leadership for groups where we no longer should be leader
    std::vector<GroupId> groupsToRelinquish;

    for (const auto& g : _leadingGroups) {
        const char* groupId = g.value;

        // If we're no longer a member or shouldn't be leader, relinquish
        bool stillMember = false;
        for (int j = 0; j < memberGroups.size(); j++) {
            if (memberGroups[j] == groupId) {
                stillMember = true;
                break;
            }
        }
        bool stillLeader = false;
        if (stillMember) {
            for (int j = 0; j < groupsToLead.size(); j++) {
                if (groupsToLead[j] == groupId) {
                    stillLeader = true;
                    break;
                }
            }
        }
        if (!stillMember || !stillLeader) {
            groupsToRelinquish.push_back(g);
        }
    }

    for (const auto& g : groupsToRelinquish) {
        relinquishGroupLeadership(g.value);
    }
    
    // Step 5: Update our service TXT records with current group info
    updateServiceTxtRecords();
}

void mdnsHandler::updateServiceTxtRecords() {
    // Group membership is no longer advertised via mDNS. Leadership is computed
    // locally from the synced config DB, so only the leader flag is published.
    ledControllerSwarmService.setLeader(_isLeader);

    #ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::updateServiceTxtRecords: " ANSI_COLOR_BLUE "Updated service TXT records (leader=" ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, _isLeader);
    #endif
}

void mdnsHandler::becomeGroupLeader(const char* groupId, const char* groupName)
{
#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::becomeGroupLeader: " ANSI_COLOR_BLUE "Becoming leader for group: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " (ID: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, groupName, groupId);
#endif

    // Sanitize the group name for use as a hostname
    char san_buf[128];
    strncpy(san_buf, groupName, sizeof(san_buf));
    Util::sanitizeHostname(san_buf, sizeof(san_buf));

#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::becomeGroupLeader: " ANSI_COLOR_BLUE "Sanitized group name: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, san_buf);
#endif

    // Create responder for this group's hostname
    auto responder = std::make_unique<mDNS::Responder>();
    responder->begin(san_buf);

    // Create and set up web service for this group
    auto webService =
        std::make_unique<LEDControllerWebService>(san_buf, LEDControllerWebService::HostType::Group);
    // Add services to the responder
    responder->addService(*webService);

    // Register with mDNS server
    mDNS::server.addHandler(*responder);

    // Store in our maps
    _groupResponders[groupId] = std::move(responder);
    _groupWebServices[groupId] = std::move(webService);

    // Track that we're now leading this group
    GroupId g;
    strncpy(g.value, groupId, sizeof(g.value) - 1);
    g.value[sizeof(g.value) - 1] = '\0';
    _leadingGroups.push_back(g);

#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::becomeGroupLeader: " ANSI_COLOR_BLUE "This controller is now leader for group: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " (" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ".local)" ANSI_COLOR_RESET, groupName, san_buf);
#endif
}

void mdnsHandler::relinquishGroupLeadership(const char* groupId)
{
    // Find the group name for logging
    AppData::Root::Groups groups(*app.data);
    String groupName = "unknown";

    for (auto it = groups.begin(); it != groups.end(); ++it) {
        if ((*it).getId() == groupId) {
            groupName = (*it).getName();
            break;
        }
    }

#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::relinquishGroupLeadership: " ANSI_COLOR_BLUE "Relinquishing leadership for group: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " (ID: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, groupName.c_str(), groupId);
#endif

    // Remove the responder from mDNS server
    if (_groupResponders.find(groupId) != _groupResponders.end()) {
        mDNS::server.removeHandler(*_groupResponders[groupId]);
        _groupResponders.erase(groupId);
    }

    // Clean up the web service
    if (_groupWebServices.find(groupId) != _groupWebServices.end()) {
        _groupWebServices.erase(groupId);
    }

    // Remove from our list of led groups
    for (auto it = _leadingGroups.begin(); it != _leadingGroups.end(); ++it) {
        if (strcmp(it->value, groupId) == 0) {
            _leadingGroups.erase(it);
            break;
        }
    }


#ifdef DEBUG_MDNS
    cdebug_i(MDNSHANDLER, "mdnsHandler::relinquishGroupLeadership: " ANSI_COLOR_BLUE "This controller is no longer leader for group: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, groupName);
#endif
}
