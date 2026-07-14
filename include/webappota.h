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
 * WebappOta — background webapp file-fetch and LittleFS staging/activation.
 */
#pragma once

#include <Network/Http/HttpClient.h>
#include <Timer.h>
#include <ArduinoJson.h>
#include <vector>

#define FS_MIN_FREE_SPACE 358400UL  
#define FS_EMERGENCY_FREE_SPACE 102400UL  
#define FS_DOWNLOAD_MARGIN 32768UL  

class WebappOta
{
public:
    void checkForUpdate(bool ignoreEnabled = false);
    bool isActive() const { return _state != State::IDLE; }
    bool wasInterrupted() const;
    void fillStatusJson(JsonObject& obj) const;
    void broadcastStatus() const;

private:
    enum class State {
        IDLE,
        QUERYING_API,
        DOWNLOADING,
        ACTIVATING,
    };

    void setState(State newState);

    struct FileEntry {
        String path;        
        String expectedMd5; 
        String url;         
        size_t size{0};     
    };

    // --- API query ---
    void queryApi(const String& branch, const String& firmwareVersion, const String& apiBaseUrl);
    int onApiResponse(HttpConnection& client, bool successful);

    // --- File download ---
    void startNextDownload();
    int onFileDownloaded(HttpConnection& client, bool successful);
    void verifyAndContinue();
    void activateStagingDeferred();
    void retryFromScratchDeferred();

    // --- Activation ---
    bool activateStaging();
    void purgeOldWebapp();
    static bool moveTree(const String& srcDir, const String& dstDir);

    // --- Helpers ---
    bool verifyFileMd5(const String& filePath, const String& expectedMd5);
    void failAttempt(const char* status);
    void saveState(const String& version, const String& md5, const char* status);
    void cleanupStaging();
    void listDirectory(const String& path, int depth=0);
    void printFileSystemUsage();
    void printIndent(int depth) {
        for (int i = 0; i < depth; ++i) {
            Serial.print(F("  │"));
        }
    }
    static String extractBranch(const String& firmwareVersion);
    static String stagingPath(const String& relPath);
    static bool ensureParentDir(const String& path);

    // --- State ---
    State _state{State::IDLE};
    String _pendingVersion;
    std::vector<FileEntry> _files;
    unsigned _fileIndex{0};
    unsigned _totalFiles{0};  
    bool _resumingInterrupted{false};
    bool _retryAfterCleanupDone{false};
    String _lastBranch;
    String _lastFirmwareVersion;
    String _lastApiBaseUrl;
    HttpClient _httpClient;
    Timer _retryTimer; 
};