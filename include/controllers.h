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
        int ttl;
        HostType hostType = HOST_TYPE_UNKNOWN;
        ControllerState state;
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
    void removeExpired(int elapsedSeconds);

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

    // Iterator support
    Iterator begin();
    Iterator end();
    
    // JSON output: renders the hosts list via the ConfigDB-generated jsonrpc "hosts" schema (see jsonrpc.cfgdb / params.cfgdb).
    std::unique_ptr<IDataSourceStream> createJsonStream(JsonFilter filter = VALID_ONLY, bool pretty = false);

private:
    static const size_t INVALID_INDEX = SIZE_MAX;
    
    std::vector<VisibleController> visibleControllers;
    
    // Helper methods
    size_t findVisibleControllerIndex(unsigned int id);
    ControllerInfo findById(unsigned int id);
    ControllerInfo findByIpAddress(const char* ipAddress);
    ControllerInfo findByIpAddress(const String& ipAddress);
    ControllerInfo findByHostname(const char* hostname);
    ControllerInfo findByHostname(const String& hostname);
    static bool shouldIncludeController(JsonFilter filter, const ControllerInfo& info);
};