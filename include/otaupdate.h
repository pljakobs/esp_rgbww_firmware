/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
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
#ifndef OTAUPDATE_H_
#define OTAUPDATE_H_
#define OTA_STATUS_FILE ".ota"

#include <Ota/Network/HttpUpgrader.h>
#include <Storage/PartitionStream.h>
#include <Storage/SpiFlash.h>
#include <Ota/Upgrader.h>
#include <Storage.h>

#ifdef ARCH_ESP8266
#include <Crypto/Md5.h>
#include <Storage/partition_info.h>
#include <Storage/Debug.h>
#endif


enum class OTASTATUS {
    OTA_NOT_UPDATING = 0,
    OTA_PROCESSING = 1,
    OTA_SUCCESS_REBOOT = 2,
    OTA_SUCCESS = 3,
    OTA_FAILED = 4
};

class Application;

class ApplicationOTA {
public:

    void start(String romurl);                   // v2 partition layout only needs rom as the webapp is wrapped into the rom
    void checkAtBoot();
    inline OTASTATUS getStatus() { 
        return status; 
    };
    inline bool isProccessing() { 
        return status == OTASTATUS::OTA_PROCESSING; 
    };
    Storage::Partition getRomPartition() { 
        return ota.getRunningPartition();
    }
    Storage::Partition getSpiffsPartition() { 
        return findSpiffsPartition(ota.getBootPartition());
    }

#ifdef ARCH_ESP8266
    /*
    * functions to manipulate the partition table
    * those are only a transitional requirement and should be dropped in a future version 
    * when most users have migrated to the new partition layout
    */
    std::vector<Storage::esp_partition_info_t> getEditablePartitionTable();
    bool addPartition(std::vector<Storage::esp_partition_info_t>& partitionTable, String partitionName,uint8_t type, uint8_t subType, uint32_t start, uint32_t size, uint8_t flags);
    bool delPartition(std::vector<Storage::esp_partition_info_t>& partitionTable, String partitionName);
    bool savePartitionTable(std::vector<Storage::esp_partition_info_t>& partitionTable);
#endif

protected:
    std::unique_ptr<Ota::Network::HttpUpgrader> otaUpdater;
    uint8 rom_slot;
    OTASTATUS status = OTASTATUS::OTA_NOT_UPDATING;

#ifdef ARCH_ESP8266
    uint8_t getPartitionIndex(std::vector<Storage::esp_partition_info_t>& partitionTable, String partitionName);
#endif 
protected:
    Storage::Partition spiffsPartition;
    OtaUpgrader ota;
    void upgradeCallback(Ota::Network::HttpUpgrader& client, bool result);
    void reset();
    void beforeOTA();
    void afterOTA();
    void doSwitch();
    void saveStatus(OTASTATUS status);
    OTASTATUS loadStatus();
    Storage::Partition findSpiffsPartition(Storage::Partition appPart);

    friend Application;
};

#endif // OTAUPDATE_H_
