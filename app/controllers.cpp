/**
 * @file
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
 *
 * @section DESCRIPTION
 *
 *
 */

#include <RGBWWCtrl.h>
#include <controllers.h>
#include <application.h>
#include <Data/Stream/MemoryDataStream.h>

extern Application app;

Controllers::HostType Controllers::hostTypeFromString(const String& type)
{
    if(type.equalsIgnoreCase(F("ALIAS")) || type.equalsIgnoreCase(F("leader")) || type.equalsIgnoreCase(F("group"))) {
        return HOST_TYPE_ALIAS;
    }
    if(type.equalsIgnoreCase(F("CONTROLLER")) || type.equalsIgnoreCase(F("host"))) {
        return HOST_TYPE_CONTROLLER;
    }
    if(type.equalsIgnoreCase(F("WALLPANEL")) || type.equalsIgnoreCase(F("wall_panel"))) {
        return HOST_TYPE_WALLPANEL;
    }
    return HOST_TYPE_UNKNOWN;
}

const char* Controllers::hostTypeToString(HostType type)
{
    switch(type) {
    case HOST_TYPE_ALIAS:
        return "ALIAS";
    case HOST_TYPE_CONTROLLER:
        return "CONTROLLER";
    case HOST_TYPE_WALLPANEL:
        return "WALLPANEL";
    default:
        return "UNKNOWN";
    }
}

// Constructor
Controllers::Controllers() {
    cdebug_i(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_BLUE "Controllers constructor called" ANSI_COLOR_RESET);
    if (!app.data) {
        cdebug_e(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_RED "app.data is NULL in Controllers constructor!" ANSI_COLOR_RESET);
        return;
    }
    cdebug_i(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_BLUE "Controllers constructor: accessing ConfigDB..." ANSI_COLOR_RESET);
    AppData::Root::Controllers controllers(*app.data);
    if (auto controllersUpdate = controllers.update()) {
        for (unsigned configIndex = 0; configIndex < controllersUpdate.getItemCount();) {
            uint32_t id;
            {
                auto controllerItem = controllersUpdate[configIndex];
                id = parseControllerId(controllerItem.getId());
            }
            bool duplicate = false;
            for (unsigned otherIndex = controllersUpdate.getItemCount(); otherIndex > configIndex + 1;) {
                --otherIndex;
                bool sameId;
                {
                    auto controllerItem = controllersUpdate[otherIndex];
                    sameId = parseControllerId(controllerItem.getId()) == id;
                }
                if (sameId) {
                    if (!controllersUpdate.removeItem(otherIndex)) {
                        cdebug_e(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_RED "error: failed to remove duplicate stored controller" ANSI_COLOR_RESET);
                        return;
                    }
                    duplicate = true;
                }
            }
            if (duplicate) {
                if (!controllersUpdate.removeItem(configIndex)) {
                    cdebug_e(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_RED "error: failed to remove duplicate stored controller" ANSI_COLOR_RESET);
                    return;
                }
                cdebug_w(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_YELLOW "Removed duplicate controller ID %u from ConfigDB; awaiting discovery" ANSI_COLOR_RESET, id);
            } else {
                ++configIndex;
            }
        }
    } else {
        cdebug_e(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_RED "error: failed to open stored controllers for cleanup" ANSI_COLOR_RESET);
        return;
    }
    size_t count = 0;
    for (auto it = controllers.begin(); it != controllers.end(); ++it) {
        count++;
        cdebug_i(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_BLUE "Found controller ID: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, (*it).getId().c_str());
    }
    cdebug_i(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_BLUE "Controllers constructor: found " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE " controllers in DB" ANSI_COLOR_RESET, count);
    visibleControllers.reserve(std::max(count, static_cast<size_t>(10)));

    // Ensure local controller is always present
    unsigned int localId = (unsigned int)system_get_chip_id();
    bool foundLocal = false;
    for (const auto& ctrl : visibleControllers) {
        if (ctrl.id == localId) {
            foundLocal = true;
            break;
        }
    }
    if (!foundLocal) {
        VisibleController localCtrl;
        localCtrl.id = localId;
        localCtrl.ttl = 0;
        localCtrl.hostType = HOST_TYPE_CONTROLLER;
        localCtrl.state = LOCALHOST;
        visibleControllers.push_back(localCtrl);
    }
    cdebug_i(CONTROLLERS, "Controllers::Controllers: " ANSI_COLOR_BLUE "Controllers constructor completed" ANSI_COLOR_RESET);
}

// Destructor
Controllers::~Controllers() {
}

// Core methods
void Controllers::addOrUpdate(unsigned int id, const char* hostname, const char* ipAddress, int ttl, HostType hostType) {
    #ifdef DEBUG_MDNS
        cdebug_i(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_BLUE "Controllers::addOrUpdate id=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE ", hostname=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ", ip=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ", ttl=" ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, id, hostname, ipAddress, ttl);
    #endif
    if(hostname == nullptr || hostname[0] == '\0' || ipAddress == nullptr || ipAddress[0] == '\0') {
        cdebug_w(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_YELLOW "Empty hostname or IP address provided, skipping addOrUpdate" ANSI_COLOR_RESET);
        return;
    }
    // Find existing visible controller
    size_t index = findVisibleControllerIndex(id);

    if (index != INVALID_INDEX) {
        // Update existing: record the advertised TTL and stamp "last heard" now.
        // State is derived live, so we never clobber the sticky LOCALHOST marker.
        visibleControllers[index].ttl = ttl;
        visibleControllers[index].lastSeenMs = millis();
        if (hostType != HOST_TYPE_UNKNOWN) {
            visibleControllers[index].hostType = hostType;
        }
    } else {
        // Brand-new controller: admit only while we stay within the inventory
        // ceiling and above the free-heap floor. Existing controllers (handled
        // above) always keep updating; we never drop what we already track.
        // checkHeap() also records the min-heap / low-heap-error counters.
        const bool heapOk = app.checkHeap(CONTROLLERS_MIN_FREE_HEAP);
        if (visibleControllers.size() >= MAX_VISIBLE_CONTROLLERS || !heapOk) {
            ++controllersDropped;
            if (!_inventoryFullReported) {
                _inventoryFullReported = true;
                cdebug_w(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_YELLOW "inventory bound reached (count=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW "/" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW ", free heap=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW ", floor=" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW "), refusing new controller " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RESET,
                        (unsigned)visibleControllers.size(), (unsigned)MAX_VISIBLE_CONTROLLERS, (unsigned)app.getFreeHeapSize(), (unsigned)CONTROLLERS_MIN_FREE_HEAP, hostname);
            }
            return;
        }
        _inventoryFullReported = false;

        // Add new visible controller
        VisibleController newController;
        newController.id = id;
        newController.ttl = ttl;
        newController.lastSeenMs = millis();
        newController.hostType = hostType;
        visibleControllers.push_back(newController);
    }

    AppData::Root::Controllers controllers(*app.data);
    bool foundInConfig = false;
    
    if (auto controllersUpdate = controllers.update()) {
        for (unsigned configIndex = 0; configIndex < controllersUpdate.getItemCount();) {
            bool duplicate = false;
            {
                auto controllerItem = controllersUpdate[configIndex];
                if (parseControllerId(controllerItem.getId()) == id && foundInConfig) {
                    duplicate = true;
                } else if (parseControllerId(controllerItem.getId()) == id) {
                    foundInConfig = true;
                    #ifdef DEBUG_MDNS
                    cdebug_i(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_BLUE "Hostname " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " already in list" ANSI_COLOR_RESET, hostname);
                    #endif

                    // Always update IP address
                    if (controllerItem.getIpAddress() != ipAddress) {
                        cdebug_i(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_BLUE "IP address changed from " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
                               controllerItem.getIpAddress().c_str(), ipAddress);
                        controllerItem.setIpAddress(ipAddress);
                        ++ipChanges;
                    }

                    // Only update hostname if this is NOT a group or leader hostname
                    if ( controllerItem.getName() != hostname) {
                        cdebug_i(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_BLUE "Hostname changed from " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
                               controllerItem.getName().c_str(), hostname);
                        controllerItem.setName(hostname);
                        ++hostnameChanges;
                    }
                }
            }
            if (duplicate) {
                if (!controllersUpdate.removeItem(configIndex)) {
                    cdebug_e(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_RED "error: failed to remove duplicate host" ANSI_COLOR_RESET);
                    return;
                }
            } else {
                ++configIndex;
            }
        }

        if(!foundInConfig) {
            cdebug_i(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_BLUE "Hostname " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " not in list adding to hostname db" ANSI_COLOR_RESET, hostname);
            auto newController = controllersUpdate.addItem();
            newController.setName(hostname);
            newController.setIpAddress(ipAddress);
            newController.setId(String(id));
        }
    } else {
        cdebug_e(CONTROLLERS, "Controllers::addOrUpdate: " ANSI_COLOR_RED "error: failed to open hosts db for update" ANSI_COLOR_RESET);
    }
}

void Controllers::addOrUpdate(unsigned int id, const String& hostname, const String& ipAddress, int ttl, HostType hostType) {
    addOrUpdate(id, hostname.c_str(), ipAddress.c_str(), ttl, hostType);
}

void Controllers::noteAddress(const char* hostname, const char* ipAddress, int ttl) {
    if (hostname == nullptr || hostname[0] == '\0' || ipAddress == nullptr || ipAddress[0] == '\0') {
        return;
    }

    // If we already know this hostname's id, the record is complete: hand it to
    // the normal upsert path so it becomes a visible (ONLINE) controller.
    unsigned int id = getIdByHostname(hostname);
    if (id != 0) {
        addOrUpdate(id, hostname, ipAddress, ttl);
        return;
    }

    // id still unknown (no TXT seen yet): stage the address until one arrives.
    size_t index = findPendingAddressIndex(hostname);
    if (index != INVALID_INDEX) {
        strncpy(pendingAddresses[index].ipAddress, ipAddress, CONTROLLER_IP_MAX_SIZE - 1);
        pendingAddresses[index].ipAddress[CONTROLLER_IP_MAX_SIZE - 1] = '\0';
        pendingAddresses[index].ttl = ttl;
        pendingAddresses[index].lastSeenMs = millis();
        return;
    }

    const bool heapOk = app.checkHeap(CONTROLLERS_MIN_FREE_HEAP);
    if (pendingAddresses.size() >= MAX_VISIBLE_CONTROLLERS || !heapOk) {
        ++controllersDropped;
        if (!_inventoryFullReported) {
            _inventoryFullReported = true;
            cdebug_w(CONTROLLERS, "Controllers::noteAddress: " ANSI_COLOR_YELLOW "staging bound reached, dropping address for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RESET, hostname);
        }
        return;
    }

    PendingAddress pending;
    strncpy(pending.hostname, hostname, CONTROLLER_HOSTNAME_MAX_SIZE - 1);
    pending.hostname[CONTROLLER_HOSTNAME_MAX_SIZE - 1] = '\0';
    strncpy(pending.ipAddress, ipAddress, CONTROLLER_IP_MAX_SIZE - 1);
    pending.ipAddress[CONTROLLER_IP_MAX_SIZE - 1] = '\0';
    pending.ttl = ttl;
    pending.lastSeenMs = millis();
    pendingAddresses.push_back(pending);
}

void Controllers::noteIdentity(unsigned int id, const char* hostname, int ttl, HostType hostType) {
    // The id is the immutable identity of the chip; we only ever learn it from a
    // TXT record. Without it (or a hostname label) there is nothing to bind.
    if (id == 0 || hostname == nullptr || hostname[0] == '\0') {
        return;
    }

    // If an address for this hostname was staged before we knew the id, the
    // record is now complete.
    size_t pendingIndex = findPendingAddressIndex(hostname);
    if (pendingIndex != INVALID_INDEX) {
        char ip[CONTROLLER_IP_MAX_SIZE];
        strncpy(ip, pendingAddresses[pendingIndex].ipAddress, CONTROLLER_IP_MAX_SIZE);
        ip[CONTROLLER_IP_MAX_SIZE - 1] = '\0';
        int stagedTtl = pendingAddresses[pendingIndex].ttl;
        pendingAddresses.erase(pendingAddresses.begin() + pendingIndex);
        addOrUpdate(id, hostname, ip, (ttl > 0) ? ttl : stagedTtl, hostType);
        return;
    }

    // If we already have an address for this id, refresh the known record. The
    // hostname may have changed; the id is the stable key, so this keeps the
    // contract that the id always identifies the chip.
    ControllerInfo info = findById(id);
    if (info.state != NOT_FOUND && info.ipAddress[0] != '\0') {
        addOrUpdate(id, hostname, info.ipAddress, ttl, hostType);
        return;
    }

    // Otherwise record an id -> hostname binding (no ip yet) so a later A record
    // can complete it. This entry surfaces as INCOMPLETE and stays hidden from
    // the visible/valid controller views until it gains an address.
    writePartialIdentity(id, hostname);
}

void Controllers::writePartialIdentity(unsigned int id, const char* hostname) {
    AppData::Root::Controllers controllers(*app.data);
    auto controllersUpdate = controllers.update();
    if (!controllersUpdate) {
        cdebug_e(CONTROLLERS, "Controllers::writePartialIdentity: " ANSI_COLOR_RED "error: failed to open hosts db for update" ANSI_COLOR_RESET);
        return;
    }

    // Refresh the hostname label if we already track this id.
    for (unsigned i = 0; i < controllersUpdate.getItemCount(); ++i) {
        auto item = controllersUpdate[i];
        if (parseControllerId(item.getId()) == id) {
            if (item.getName() != hostname) {
                item.setName(hostname);
                ++hostnameChanges;
            }
            return;
        }
    }

    // Brand-new partial: admit only within the inventory ceiling / heap floor.
    const bool heapOk = app.checkHeap(CONTROLLERS_MIN_FREE_HEAP);
    if (controllersUpdate.getItemCount() >= MAX_VISIBLE_CONTROLLERS || !heapOk) {
        ++controllersDropped;
        if (!_inventoryFullReported) {
            _inventoryFullReported = true;
            cdebug_w(CONTROLLERS, "Controllers::writePartialIdentity: " ANSI_COLOR_YELLOW "inventory bound reached, dropping new controller id " ANSI_COLOR_CYAN "%u" ANSI_COLOR_RESET, id);
        }
        return;
    }
    _inventoryFullReported = false;

    cdebug_i(CONTROLLERS, "Controllers::writePartialIdentity: " ANSI_COLOR_BLUE "recording INCOMPLETE controller id " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " (" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, id, hostname);
    auto newController = controllersUpdate.addItem();
    newController.setName(hostname);
    newController.setId(String(id));
    // ip intentionally left empty -> INCOMPLETE until an A record arrives.
}

Controllers::ControllerState Controllers::liveState(const VisibleController& controller, int& remainingTtl) {
    if (controller.state == LOCALHOST || controller.id == (unsigned int)system_get_chip_id()) {
        remainingTtl = controller.ttl;
        return LOCALHOST;
    }
    // RFC 6762 §10: the record is valid for its full advertised TTL measured from
    // the moment it was last heard. millis() subtraction is wrap-safe.
    uint32_t ageSeconds = (millis() - controller.lastSeenMs) / 1000u;
    int remaining = controller.ttl - (int)ageSeconds;
    remainingTtl = (remaining > 0) ? remaining : 0;
    return (remaining > 0) ? ONLINE : OFFLINE;
}

void Controllers::removeExpired() {
    const unsigned int localId = (unsigned int)system_get_chip_id();

    // Drop records silent for longer than their advertised TTL plus the grace
    // period. State is derived live elsewhere, so expiry is purely a prune here
    // and is driven by the wall clock, not by how often this is called.
    visibleControllers.erase(
        std::remove_if(visibleControllers.begin(), visibleControllers.end(),
            [localId](const VisibleController& c) {
                if (c.id == localId || c.state == LOCALHOST) return false;
                uint32_t ageSeconds = (millis() - c.lastSeenMs) / 1000u;
                return ageSeconds > (uint32_t)(c.ttl + VISIBLE_CONTROLLER_GRACE_SECONDS);
            }),
        visibleControllers.end()
    );

    // Drop staged address fragments whose advertised TTL elapsed before a TXT
    // revealed the owning controller's id.
    pendingAddresses.erase(
        std::remove_if(pendingAddresses.begin(), pendingAddresses.end(),
            [](const PendingAddress& p) {
                uint32_t ageSeconds = (millis() - p.lastSeenMs) / 1000u;
                return ageSeconds > (uint32_t)p.ttl;
            }),
        pendingAddresses.end()
    );
}

// Query methods
Controllers::ControllerInfo Controllers::getController(unsigned int id) {
    return findById(id);
}

const char* Controllers::getIpAddress(unsigned int id) {
    auto info = findById(id);
    if (info.state != NOT_FOUND) {
        static char ip[CONTROLLER_IP_MAX_SIZE];
        strncpy(ip, info.ipAddress, CONTROLLER_IP_MAX_SIZE);
        return ip;
    }
    return nullptr;
}

String Controllers::getIpAddressString(unsigned int id) {
    const char* ip = getIpAddress(id);
    return ip ? String(ip) : String();
}

const char* Controllers::getHostname(unsigned int id) {
    auto info = findById(id);
    if (info.state != NOT_FOUND) {
        static char hostname[CONTROLLER_HOSTNAME_MAX_SIZE];
        strncpy(hostname, info.hostname, CONTROLLER_HOSTNAME_MAX_SIZE);
        return hostname;
    }
    return nullptr;
}

String Controllers::getHostnameString(unsigned int id) {
    const char* hostname = getHostname(id);
    return hostname ? String(hostname) : String();
}

unsigned int Controllers::getIdByHostname(const char* hostname) {
    auto info = findByHostname(hostname);
    return (info.state != NOT_FOUND) ? info.id : 0;
}

unsigned int Controllers::getIdByHostname(const String& hostname) {
    return getIdByHostname(hostname.c_str());
}

unsigned int Controllers::getIdByIpAddress(const char* ipAddress) {
    auto info = findByIpAddress(ipAddress);
    return (info.state != NOT_FOUND) ? info.id : 0;
}

unsigned int Controllers::getIdByIpAddress(const String& ipAddress) {
    return getIdByIpAddress(ipAddress.c_str());
}

uint32_t Controllers::getHighestId() {
    uint32_t highest = 0;
    AppData::Root::Controllers controllers(*app.data);
    for (auto& controller : controllers) {
        uint32_t id = parseControllerId(controller.getId());
        if (id > highest) {
            highest = id;
        }
    }
    return highest;
}

// State checks
bool Controllers::isVisible(unsigned int id) {
    size_t index = findVisibleControllerIndex(id);
    if (index == INVALID_INDEX) {
        return false;
    }
    int remaining = 0;
    return liveState(visibleControllers[index], remaining) == ONLINE;
}

bool Controllers::isVisibleByHostname(const char* hostname) {
    unsigned int id = getIdByHostname(hostname);
    return id != 0 && isVisible(id);
}

bool Controllers::isVisibleByHostname(const String& hostname) {
    return isVisibleByHostname(hostname.c_str());
}

bool Controllers::isVisibleByIpAddress(const char* ipAddress) {
    unsigned int id = getIdByIpAddress(ipAddress);
    return id != 0 && isVisible(id);
}

bool Controllers::isVisibleByIpAddress(const String& ipAddress) {
    return isVisibleByIpAddress(ipAddress.c_str());
}

int Controllers::getTTL(unsigned int id) {
    size_t index = findVisibleControllerIndex(id);
    if (index == INVALID_INDEX) {
        return 0;
    }
    int remaining = 0;
    liveState(visibleControllers[index], remaining);
    return remaining;
}

// Counts
size_t Controllers::getVisibleCount() {
    size_t count = 0;
    int remaining = 0;
    for (const auto& controller : visibleControllers) {
        if (liveState(controller, remaining) == ONLINE) {
            count++;
        }
    }
    return count;
}

size_t Controllers::getTotalCount() {
    AppData::Root::Controllers controllers(*app.data);
    size_t count = 0;
    for (auto it = controllers.begin(); it != controllers.end(); ++it) {
        count++;
    }
    return count;
}

// Utility
void Controllers::update() {
    // Update logic if needed
}

void Controllers::forgetControllers(){
    visibleControllers.clear();
    if (auto controllersUpdate = AppData::Root::Controllers(*app.data).update()) {
        controllersUpdate.clear();
        cdebug_i(CONTROLLERS, "Controllers::forgetControllers: " ANSI_COLOR_BLUE "Cleared all controllers from ConfigDB" ANSI_COLOR_RESET);
    } else {
        cdebug_e(CONTROLLERS, "Controllers::forgetControllers: " ANSI_COLOR_RED "error: failed to open hosts db for clearing, now " ANSI_COLOR_CYAN "%i" ANSI_COLOR_RED " controllers known" ANSI_COLOR_RESET, getTotalCount());
    }
}

// Iterator implementation
Controllers::Iterator::Iterator(Controllers& mgr, bool atEnd) 
    : manager(mgr), configControllers(*app.data), currentIndex(0), totalCount(0) {
    
    // Count total controllers
    for (auto it = configControllers.begin(); it != configControllers.end(); ++it) {
        totalCount++;
    }
    
    if (atEnd) {
        currentIndex = totalCount;
    }
}

Controllers::ControllerInfo Controllers::Iterator::operator*() {
    AppData::Root::Controllers controllers(*app.data);
    size_t index = 0;
    for (auto it = controllers.begin(); it != controllers.end(); ++it, ++index) {
        if (index == currentIndex) {
            auto& configItem = *it;
            Controllers::ControllerInfo info;
            info.id = parseControllerId(configItem.getId());
            strncpy(info.hostname, configItem.getName().c_str(), CONTROLLER_HOSTNAME_MAX_SIZE);
            strncpy(info.ipAddress, configItem.getIpAddress().c_str(), CONTROLLER_IP_MAX_SIZE);
            info.state = OFFLINE;
            info.ttl = 0;
            info.hostType = HOST_TYPE_UNKNOWN;
            
            // Check if controller is visible
            size_t visibleIndex = manager.findVisibleControllerIndex(info.id);
            if (visibleIndex != Controllers::INVALID_INDEX) {
                info.hostType = manager.visibleControllers[visibleIndex].hostType;
                info.state = liveState(manager.visibleControllers[visibleIndex], info.ttl);
            } else if (strlen(info.hostname) == 0 || strlen(info.ipAddress) == 0) {
                info.state = INCOMPLETE;
            }
            
            return info;
        }
    }
    
    // Return empty info if not found
    return ControllerInfo();
}

Controllers::Iterator& Controllers::Iterator::operator++() {
    currentIndex++;
    return *this;
}

bool Controllers::Iterator::operator==(const Iterator& other) const {
    return currentIndex == other.currentIndex;
}

bool Controllers::Iterator::operator!=(const Iterator& other) const {
    return !(*this == other);
}

Controllers::Iterator Controllers::begin() {
    return Iterator(*this, false);
}

Controllers::Iterator Controllers::end() {
    return Iterator(*this, true);
}

// Helper methods
size_t Controllers::findVisibleControllerIndex(unsigned int id) {
    for (size_t i = 0; i < visibleControllers.size(); i++) {
        if (visibleControllers[i].id == id) {
            return i;
        }
    }
    return INVALID_INDEX;
}

size_t Controllers::findPendingAddressIndex(const char* hostname) {
    if (hostname == nullptr) {
        return INVALID_INDEX;
    }
    for (size_t i = 0; i < pendingAddresses.size(); i++) {
        if (strcmp(pendingAddresses[i].hostname, hostname) == 0) {
            return i;
        }
    }
    return INVALID_INDEX;
}

Controllers::ControllerInfo Controllers::findById(unsigned int id) {
    AppData::Root::Controllers controllers(*app.data);
    for (auto& controller : controllers) {
        if (parseControllerId(controller.getId()) == id) {
            ControllerInfo info;
            info.id = id;
            strncpy(info.hostname, controller.getName().c_str(), CONTROLLER_HOSTNAME_MAX_SIZE);
            strncpy(info.ipAddress, controller.getIpAddress().c_str(), CONTROLLER_IP_MAX_SIZE);
            info.state = OFFLINE;
            info.ttl = 0;
            info.hostType = HOST_TYPE_UNKNOWN;
            
            // Check if visible
            size_t visibleIndex = findVisibleControllerIndex(id);
            if (visibleIndex != INVALID_INDEX) {
                info.hostType = visibleControllers[visibleIndex].hostType;
                info.state = liveState(visibleControllers[visibleIndex], info.ttl);
            } else if (strlen(info.hostname) == 0 || strlen(info.ipAddress) == 0) {
                info.state = INCOMPLETE;
            }
            
            return info;
        }
    }
    
    return ControllerInfo(); // NOT_FOUND
}

Controllers::ControllerInfo Controllers::findByIpAddress(const char* ipAddress) {
    AppData::Root::Controllers controllers(*app.data);
    for (auto& controller : controllers) {
        if (strcmp(controller.getIpAddress().c_str(), ipAddress) == 0) {
            return findById(parseControllerId(controller.getId()));
        }
    }
    return ControllerInfo(); // NOT_FOUND
}

Controllers::ControllerInfo Controllers::findByIpAddress(const String& ipAddress) {
    return findByIpAddress(ipAddress.c_str());
}

Controllers::ControllerInfo Controllers::findByHostname(const char* hostname) {
    AppData::Root::Controllers controllers(*app.data);
    for (auto& controller : controllers) {
        if (strcmp(controller.getName().c_str(), hostname) == 0) {
            return findById(parseControllerId(controller.getId()));
        }
    }
    return ControllerInfo(); // NOT_FOUND
}

Controllers::ControllerInfo Controllers::findByHostname(const String& hostname) {
    return findByHostname(hostname.c_str());
}

// JSON output methods
bool Controllers::shouldIncludeController(JsonFilter filter, const Controllers::ControllerInfo& info) {
    bool result;
    switch (filter) {
        case ALL_ENTRIES:
            result = true;
            break;

        case VALID_ONLY:
            result = info.id != 0 &&
                   strlen(info.hostname) > 0 &&
                   strlen(info.ipAddress) > 0;
            break;

        case VISIBLE_ONLY:
            result = info.state == ONLINE || info.state == LOCALHOST;
            break;

        default:
            result = false;
    }
    cdebug_i(CONTROLLERS, "Controllers::shouldIncludeController: " ANSI_COLOR_BLUE "" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " controller: " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE ", hostname: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ", ip: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ", state: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ", ttl: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, result?"return": "skip", info.id, info.hostname, info.ipAddress, info.state, info.ttl);

    return result;
}

std::unique_ptr<IDataSourceStream> Controllers::createJsonStream(JsonFilter filter, bool pretty) {
    (void)pretty; // ConfigDB-rendered JSON is always compact.

    auto& codec = rpcCodec();
    Jsonrpc::Root root(codec.db());
    const unsigned int localId = (unsigned int)system_get_chip_id();
    bool foundLocal = false;

    if(auto update = root.update()) {
        auto hostsParams = update.toHosts().toHostsParams();

        AppData::Root::Controllers controllers(*app.data);
        for(auto configItem : controllers) {
            ControllerInfo info;
            info.id = parseControllerId(configItem.getId());
            strncpy(info.hostname, configItem.getName().c_str(), CONTROLLER_HOSTNAME_MAX_SIZE);
            strncpy(info.ipAddress, configItem.getIpAddress().c_str(), CONTROLLER_IP_MAX_SIZE);
            info.state = OFFLINE;
            info.ttl = 0;
            info.hostType = HOST_TYPE_UNKNOWN;

            size_t visibleIndex = findVisibleControllerIndex(info.id);
            if(visibleIndex != INVALID_INDEX) {
                info.hostType = visibleControllers[visibleIndex].hostType;
                info.state = liveState(visibleControllers[visibleIndex], info.ttl);
            } else if(strlen(info.hostname) == 0 || strlen(info.ipAddress) == 0) {
                info.state = INCOMPLETE;
            }

            if(info.id == localId) {
                foundLocal = true;
                info.state = LOCALHOST;
            }

            if(!shouldIncludeController(filter, info)) {
                continue;
            }

            auto item = hostsParams.hosts.addItem();
            item.setId(info.id);
            item.setHostname(info.hostname);
            item.setIpAddress(info.ipAddress);
            item.setHostType(hostTypeToString(info.hostType));
            item.setVisible(info.state == ONLINE || info.state == LOCALHOST);
            item.setState((int)info.state);
        }

        if(!foundLocal) {
            ControllerInfo info;
            info.id = localId;
            info.state = LOCALHOST;
            String localHostname = WifiStation.getHostname();
            String localIp = WifiStation.getIP().toString();
            strncpy(info.hostname, localHostname.c_str(), CONTROLLER_HOSTNAME_MAX_SIZE);
            strncpy(info.ipAddress, localIp.c_str(), CONTROLLER_IP_MAX_SIZE);

            if(shouldIncludeController(filter, info)) {
                auto item = hostsParams.hosts.addItem();
                item.setId(localId);
                item.setHostname(localHostname);
                item.setIpAddress(localIp);
                item.setHostType(hostTypeToString(HOST_TYPE_CONTROLLER));
                item.setVisible(true);
                item.setState((int)LOCALHOST);
            }
        }
    }

    String json;
    if(!codec.renderPayload(root.asHosts().asHostsParams(), json)) {
        return nullptr;
    }

    return std::make_unique<MemoryDataStream>(std::move(json));
}
