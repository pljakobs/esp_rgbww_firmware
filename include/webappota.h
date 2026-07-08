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
 *
 * Flow:
 *   checkForUpdate()
 *     → queryApi()               (HTTP GET /api/webapp/latest?branch=…&firmware_version=…)
 *     → onApiResponse()          (parse JSON; compare versions; populate _files)
 *     → startNextDownload()      (per file: makedirs, HttpClient::downloadFile)
 *     → onFileDownloaded()       (verify MD5; advance index or activate)
 *     → activateStaging()        (move files from staging/ to root, update ConfigDB)
 *
 * File layout on LittleFS:
 *   staging/<path>  — downloaded, verified files awaiting activation
 *   <path>          — active webapp files served by the webserver
 *
 * ConfigDB state (AppConfig::Root::Webapp):
 *   enabled            — master switch; checkForUpdate() is a no-op when false
 *   api_base_url       — e.g. "https://lightinator.de/api"
 *   installed_version  — persisted after successful activation
 *   installed_md5      — persisted after successful activation (last file md5)
 *   last_check_status  — "ok" | "no_update" | "api_error" | "download_error" | "md5_error"
 */
#pragma once

#include <Network/Http/HttpClient.h>
#include <Timer.h>
#include <ArduinoJson.h>
#include <vector>


#define FS_MIN_FREE_SPACE 358400UL  // informational: a full webapp bundle is ~350KB
#define FS_EMERGENCY_FREE_SPACE 102400UL  // below this free space, force-clear staging/ to recover from a stuck, full filesystem
#define FS_DOWNLOAD_MARGIN 32768UL  // extra headroom (bytes) required on top of the reported bundle size to allow for filesystem overhead

class WebappOta
{
public:
    /**
     * @brief Trigger a check for a newer webapp from the version API.
     *
     * Call this once after the WiFi station has obtained an IP address.
     * Re-entrant: a second call while a check/download is in progress is
     * silently ignored.
     *
     * @param ignoreEnabled  When true, bypasses the enabled flag.
     *   Use for: bootstrap fetch (no webapp present) or manual UI trigger.
     *   Default false — respects the auto-update enabled flag.
     */
    void checkForUpdate(bool ignoreEnabled = false);

    bool isActive() const
    {
        return _state != State::IDLE;
    }

    // Returns true if a previous download was interrupted and not yet completed.
    // Persisted in ConfigDB; survives reboots. Checked by checkForUpdate() on boot.
    bool wasInterrupted() const;

    /**
     * @brief Fill @p obj with current OTA state for the /webapp_status endpoint.
     *
     * Keys: state (string), file (int, 1-based current), total (int),
     *       file_path (string), version (string), last_status (string from ConfigDB).
     */
    void fillStatusJson(JsonObject& obj) const;

    // Broadcast current state as a 'webapp_ota_status' WebSocket message.
    // Called at each state transition so the updating.html page can react
    // without polling /webapp_status over HTTP.
    void broadcastStatus() const;

private:
    enum class State {
        IDLE,
        QUERYING_API,
        DOWNLOADING,
        ACTIVATING,
    };

    // Single choke point for all _state changes. Keeps the inbound HTTP
    // connection limit in sync with OTA activity (clamped while active,
    // restored when returning to IDLE) so the download cannot be starved of
    // heap by concurrent browser polling on ESP8266.
    void setState(State newState);

    struct FileEntry {
        String path;        ///< relative path, e.g. "assets/index.js.gz"
        String expectedMd5; ///< lowercase hex MD5 from API response
        String url;         ///< absolute download URL
        size_t size{0};     ///< file size in bytes from API (0 if not provided)
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
            Serial.print("  │");
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
    unsigned _totalFiles{0};  ///< total files in this version (including already-verified ones)
    bool _resumingInterrupted{false};
    bool _retryAfterCleanupDone{false};
    String _lastBranch;
    String _lastFirmwareVersion;
    String _lastApiBaseUrl;
    HttpClient _httpClient;
    Timer _retryTimer; ///< backoff timer for failed checks
};
