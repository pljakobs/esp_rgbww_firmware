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

#include <app-data.h>
#include <Data/Stream/DataSourceStream.h>
#include <vector>
#include <algorithm>
#include <memory>


#define CONTROLLER_HOSTNAME_MAX_SIZE 64
#define CONTROLLER_IP_MAX_SIZE 16

// String::toInt() saturates at INT_MAX, but chip IDs can use the full 32 bits
inline uint32_t parseControllerId(const String& id)
{
    return strtoul(id.c_str(), nullptr, 10);
}

class Controllers {
public:
    enum HostType {
        HOST_TYPE_UNKNOWN,
        HOST_TYPE_ALIAS,
        HOST_TYPE_CONTROLLER,
        HOST_TYPE_WALLPANEL,
    };

    enum ControllerState {
        NOT_FOUND, INCOMPLETE, OFFLINE, ONLINE, LOCALHOST
    };

    enum JsonFilter {
        ALL_ENTRIES, VALID_ONLY, VISIBLE_ONLY
    };

    struct ControllerInfo {
        unsigned int id = 0;
        char hostname[CONTROLLER_HOSTNAME_MAX_SIZE] = {0};
        char ipAddress[CONTROLLER_IP_MAX_SIZE] = {0};
        HostType hostType = HOST_TYPE_UNKNOWN;
        ControllerState state = NOT_FOUND;
        int ttl = 0;
    };

    struct VisibleController {
        unsigned int id;
        // RFC 6762 cache record: ttl is the advertised lifetime (seconds) and
        // lastSeenMs is when we last heard it. ONLINE/OFFLINE is derived live
        // from (now - lastSeenMs) vs ttl, never decremented on a fixed cadence.
        // state only stores the sticky LOCALHOST marker; it is ignored otherwise.
        int ttl;
        uint32_t lastSeenMs = 0;
        HostType hostType = HOST_TYPE_UNKNOWN;
        ControllerState state = OFFLINE;
    };

    class Iterator {
    private:
        Controllers& manager;
        AppData::Root::Controllers configControllers;
        size_t currentIndex;
        size_t totalCount;

    public:
        Iterator(Controllers& mgr, bool atEnd = false);
        ControllerInfo operator*();
        Iterator& operator++();
        bool operator==(const Iterator& other) const;
        bool operator!=(const Iterator& other) const;
    };

    // Constructor/Destructor
    Controllers();
    ~Controllers();

    // Core methods
    void addOrUpdate(unsigned int id, const char* hostname, const char* ipAddress, int ttl, HostType hostType = HOST_TYPE_UNKNOWN);
    void addOrUpdate(unsigned int id, const String& hostname, const String& ipAddress, int ttl, HostType hostType = HOST_TYPE_UNKNOWN);

    // mDNS fragment handlers. The id is the immutable controller identity and is
    // only ever carried by a TXT record; an A record carries only hostname + ip.
    // These let discovery complete across separate packets instead of requiring
    // SRV + A + TXT to arrive together in one message.
    void noteIdentity(unsigned int id, const char* hostname, int ttl, HostType hostType = HOST_TYPE_UNKNOWN);
    void noteAddress(const char* hostname, const char* ipAddress, int ttl);

    // Prune records whose advertised TTL (plus grace) has elapsed since they were
    // last heard. Derives expiry from the wall clock, not the caller's cadence.
    void removeExpired();

    static HostType hostTypeFromString(const String& type);
    static const char* hostTypeToString(HostType type);
    
    // Query methods
    ControllerInfo getController(unsigned int id);
    const char* getIpAddress(unsigned int id);
    String getIpAddressString(unsigned int id);
    const char* getHostname(unsigned int id);
    String getHostnameString(unsigned int id);
    unsigned int getIdByHostname(const char* hostname);
    unsigned int getIdByHostname(const String& hostname);
    unsigned int getIdByIpAddress(const char* ipAddress);
    unsigned int getIdByIpAddress(const String& ipAddress);
    uint32_t getHighestId();
    
    // State checks
    bool isVisible(unsigned int id);
    bool isVisibleByHostname(const char* hostname);
    bool isVisibleByHostname(const String& hostname);
    bool isVisibleByIpAddress(const char* ipAddress);
    bool isVisibleByIpAddress(const String& ipAddress);
    int getTTL(unsigned int id);
    
    // Counts
    size_t getVisibleCount();
    size_t getTotalCount();
    
    // Utility
    void update();
    void forgetControllers();

    // Number of times a known controller ID was seen with a different hostname / IP address
    uint32_t hostnameChanges = 0;
    uint32_t ipChanges = 0;
    // Number of brand-new controllers refused because the inventory bound (count
    // ceiling or free-heap floor) was hit. Existing controllers still update.
    uint32_t controllersDropped = 0;

    // Iterator support
    Iterator begin();
    Iterator end();
    
    // JSON output: renders the hosts list via the ConfigDB-generated jsonrpc "hosts" schema (see jsonrpc.cfgdb / params.cfgdb).
    std::unique_ptr<IDataSourceStream> createJsonStream(JsonFilter filter = VALID_ONLY, bool pretty = false);

private:
    static const size_t INVALID_INDEX = SIZE_MAX;
    
    std::vector<VisibleController> visibleControllers;

    // Address fragments (hostname + ip) seen before the controller's id is known
    // (its TXT record hasn't arrived yet). Held here, bounded and TTL-reaped,
    // until a TXT reveals the id and the record can enter the id-keyed inventory.
    struct PendingAddress {
        char hostname[CONTROLLER_HOSTNAME_MAX_SIZE];
        char ipAddress[CONTROLLER_IP_MAX_SIZE];
        int ttl;
        uint32_t lastSeenMs = 0;
    };
    std::vector<PendingAddress> pendingAddresses;

    // Latches the "inventory bound reached" warning so it logs once per episode
    // instead of on every refused mDNS response.
    bool _inventoryFullReported = false;
    
    // Grace period beyond the advertised TTL before a silent record is dropped
    // (RFC 6762 cache-flush tolerance), so a single missed refresh never evicts.
    static constexpr int VISIBLE_CONTROLLER_GRACE_SECONDS = 300;

    // Helper methods
    // Derives the live ControllerState (and remaining ttl, seconds) for a cached
    // record from the last-seen timestamp per RFC 6762 §10.
    static ControllerState liveState(const VisibleController& controller, int& remainingTtl);
    size_t findVisibleControllerIndex(unsigned int id);
    size_t findPendingAddressIndex(const char* hostname);
    void writePartialIdentity(unsigned int id, const char* hostname);
    ControllerInfo findById(unsigned int id);
    ControllerInfo findByIpAddress(const char* ipAddress);
    ControllerInfo findByIpAddress(const String& ipAddress);
    ControllerInfo findByHostname(const char* hostname);
    ControllerInfo findByHostname(const String& hostname);
    static bool shouldIncludeController(JsonFilter filter, const ControllerInfo& info);
};