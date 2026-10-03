/**
 * @file
 * @author  Peter Jakobs http://github.com/pljakobs
 *
 * Background webapp OTA — fetch webapp files from lightinator.de version API,
 * stage in LittleFS, verify MD5, then atomically activate.
 *
 * See include/webappota.h for the full design description.
 */

#include <webappota.h>
#include <application.h>
#include <ArduinoJson.h>
#include <FileSystem.h>
#include <Crypto/Md5.h>
#include <Data/HexString.h>
#include <Data/Stream/MemoryDataStream.h>
#include <Data/Stream/FileStream.h>
#include <cstring>
#include <vector>


// ToDo: before update, read free file system space and compare against total size of files to download. If insufficient, skip update and log error (e.g. "not enough free space for update") instead of starting download and failing midway with "download error" or "md5 error". This is especially important for ESP8266 with its smaller flash sizes and more fragmented free space after OTA firmware updates.
/*
 * File-system layout:
 *   staging/<path>   — files being downloaded / just verified
 *   <path>           — active files served by the webserver
 */
static constexpr const char STAGING_ROOT[] = "staging";

// Retry interval on transient failures (ms)
static constexpr uint32_t RETRY_INTERVAL_MS = 5 * 60 * 1000; // 5 minutes

namespace {
constexpr const char* kStatusOk = "ok";
constexpr const char* kStatusNoUpdate = "no_update";
constexpr const char* kStatusApiError = "api_error";
constexpr const char* kStatusDownloadError = "download_error";
constexpr const char* kStatusMd5Error = "md5_error";
constexpr const char* kStatusActivationError = "activation_error";
constexpr const char* kStatusLowHeap = "low_heap";
constexpr const char* kStatusLowSpace = "low_space";
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

String WebappOta::stagingPath(const String& relPath)
{
    return String(STAGING_ROOT) + "/" + relPath;
}

/**
 * @brief Create all parent directories for @p path if they don't exist.
 * @param path  e.g. "staging/assets/index.js.gz"
 * @retval true on success or if no parent dir needed
 */
bool WebappOta::ensureParentDir(const String& path)
{
    int sep = path.lastIndexOf('/');
    if(sep <= 0) {
        return true; // root level, no parent needed
    }
    String dir = path.substring(0, sep);
    // createDirectories (makedirs) only creates intermediate components, not
    // the final directory itself (stops before the last non-slash segment).
    // Call it first for any intermediate dirs, then mkdir the final dir.
    int res = createDirectories(dir);
    if(res < 0 && res != IFS::Error::Exists) {
        cdebug_e(WEBAPPOTA, "WebappOta::ensureParentDir: " ANSI_COLOR_RED "makedirs('" ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "') = " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET, dir.c_str(), res);
        return false;
    }
    res = createDirectory(dir);
    if(res < 0 && res != IFS::Error::Exists) {
        cdebug_e(WEBAPPOTA, "WebappOta::ensureParentDir: " ANSI_COLOR_RED "mkdir('" ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "') = " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET, dir.c_str(), res);
        return false;
    }
    return true;
}

/**
 * @brief Extract the branch segment from a firmware version string.
 *
 * CI format:   "V5.0-599-testing"              → "testing"
 * Local format: "V5.0-847-experimental-extra"  → "experimental"
 *
 * Splits on '-', skips segment 0 (e.g. "V5.0") and segment 1 if numeric
 * (build number), returns segment 2 as the branch name.
 * Falls back to "experimental" if the version string doesn't match.
 */
String WebappOta::extractBranch(const String& firmwareVersion)
{
#ifdef ARCH_HOST
    // On the Host emulator the git-describe version carries the local working
    // branch (e.g. "rollback/979"), for which no webapp artifacts are published.
    // Force the branch that Host testing tracks so OTA queries resolve.
    return F("experimental");
#endif
    // Find the branch after the numeric build number. Local git-describe
    // versions can contain a tag such as "ci/feature/name/..."; those do not
    // identify a published webapp branch and must use the experimental feed.
    int firstDash = firmwareVersion.indexOf('-');
    if(firstDash < 0) {
        return F("experimental");
    }
    int secondDash = firmwareVersion.indexOf('-', firstDash + 1);
    if(secondDash < 0) {
        return F("experimental");
    }
    String buildNumber = firmwareVersion.substring(firstDash + 1, secondDash);
    if(buildNumber.length() == 0) {
        return F("experimental");
    }
    for(unsigned i = 0; i < buildNumber.length(); ++i) {
        if(buildNumber[i] < '0' || buildNumber[i] > '9') {
            return F("experimental");
        }
    }

    int thirdDash = firmwareVersion.indexOf('-', secondDash + 1);
    String branch = (thirdDash > 0)
        ? firmwareVersion.substring(secondDash + 1, thirdDash)
        : firmwareVersion.substring(secondDash + 1);
    return branch.length() > 0 ? branch : F("experimental");
}

// ─── Public API ──────────────────────────────────────────────────────────────

bool WebappOta::wasInterrupted() const
{
    AppConfig::Webapp webapp(*app.cfg);
    return webapp.getInProgress();
}

void WebappOta::checkForUpdate(bool ignoreEnabled)
{
    cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "ignoreEnabled=" ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, ignoreEnabled);

    if (app.getFreeHeapSize() < WEBAPP_OTA_MIN_UPDATE_HEAP) {
        cdebug_w(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_YELLOW "low heap (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW ", should be " ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW " bytes), backing off 5s" ANSI_COLOR_RESET, app.getFreeHeapSize(), WEBAPP_OTA_MIN_UPDATE_HEAP);
        
        // Statically allocated CallbackTimer avoids heap allocation.
        static Timer heapRetryTimer;
        heapRetryTimer.initializeMs(5000, TimerDelegate([this, ignoreEnabled]() {
            this->checkForUpdate(ignoreEnabled);
        })).startOnce();
        
        return;
    }

    cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "| file system size and usage |" ANSI_COLOR_RESET);
    cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    printFileSystemUsage();
    
    /*
    debug_i(ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    debug_i(ANSI_COLOR_BLUE "|   current directory layout |" ANSI_COLOR_RESET);
    debug_i(ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    #ifndef ARCH_HOST
    listDirectory("/", 0);
    #endif
    */
    IFS::FileSystem::Info fsInfo;
    int result = fileGetSystemInfo(fsInfo);
    if(result != FS_OK) {
        cdebug_e(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_RED "failed to get filesystem info" ANSI_COLOR_RESET);
        return;
    }

    // (c) Emergency reclaim: a previous interrupted download can leave a partial
    // staging/ tree that fills LittleFS and blocks every future update (and even
    // config writes).  If free space is critically low, drop staging first so we
    // can recover on this boot instead of being stuck forever.
    if(fsInfo.freeSpace < FS_EMERGENCY_FREE_SPACE) {
        cdebug_w(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_YELLOW "emergency low space (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW " bytes), clearing staging" ANSI_COLOR_RESET, fsInfo.freeSpace);
        cleanupStaging();
        fileGetSystemInfo(fsInfo);
    }
    // Note: we no longer bail out on low free space here.  Old webapp assets are
    // purged before the new version is downloaded (see onApiResponse), so the
    // active bundle is only replaced once we know a different version exists.

    if(_state != State::IDLE) {
        cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "already active, skipping" ANSI_COLOR_RESET);
        return;
    }

    AppConfig::Webapp webapp(*app.cfg);
    if(!ignoreEnabled && !webapp.getEnabled()) {
        cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "disabled in config" ANSI_COLOR_RESET);
        return;
    }

    bool interrupted = webapp.getInProgress();
    if(interrupted) {
        cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "resuming interrupted download" ANSI_COLOR_RESET);
    }

    String apiBaseUrl = webapp.getApiBaseUrl();
    String branch = extractBranch(fw_git_version);

    _resumingInterrupted = interrupted;
    _retryAfterCleanupDone = false;
    _lastBranch = branch;
    _lastFirmwareVersion = fw_git_version;
    _lastApiBaseUrl = apiBaseUrl;

    cdebug_i(WEBAPPOTA, "WebappOta::checkForUpdate: " ANSI_COLOR_BLUE "branch=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " fw=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, branch.c_str(), fw_git_version);
    queryApi(branch, fw_git_version, apiBaseUrl);
}

// ─── API query ───────────────────────────────────────────────────────────────

void WebappOta::setState(State newState)
{
    const bool wasActive = (_state != State::IDLE);
    _state = newState;
    const bool nowActive = (_state != State::IDLE);

    // Only touch the webserver on genuine active<->idle transitions to avoid
    // reconfiguring the TCP accept limit on every intermediate state change.
    if(wasActive != nowActive) {
        app.webserver.applyOtaLoadShedding(nowActive);
    }
}

void WebappOta::queryApi(const String& branch, const String& firmwareVersion, const String& apiBaseUrl)
{
    setState(State::QUERYING_API);
    broadcastStatus();
    _pendingFileIndices.clear();
    _fileIndex = 0;

    String url = apiBaseUrl + F("/webapp/latest?branch=") + branch
               + F("&firmware_version=") + firmwareVersion;

    cdebug_i(WEBAPPOTA, "WebappOta::queryApi: " ANSI_COLOR_BLUE "GET " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, url.c_str());

    if(!_httpClient.downloadString(url,
            RequestCompletedDelegate(&WebappOta::onApiResponse, this), 4096)) {
        cdebug_e(WEBAPPOTA, "WebappOta::queryApi: " ANSI_COLOR_RED "failed to queue request" ANSI_COLOR_RESET);
        setState(State::IDLE);
        saveState(String::nullstr, String::nullstr, kStatusApiError);
    }
}

int WebappOta::onApiResponse(HttpConnection& client, bool successful)
{
    auto* response = client.getResponse();
    if(!response) {
        cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "no response object" ANSI_COLOR_RESET);
        setState(State::IDLE);
        saveState(String::nullstr, String::nullstr, kStatusApiError);
        return 0;
    }

    int code = (int)response->code;

    if(!successful || (code != 200 && code != 0 /* 0 = no code set */)) {
        cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "HTTP " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ", no update available" ANSI_COLOR_RESET, code);
        failAttempt((code == 404) ? kStatusNoUpdate : kStatusApiError);
        return 0;
    }

    String body = response->getBody();
    if(body.length() == 0) {
        cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "empty body" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }

    cdebug_d(WEBAPPOTA, "WebappOta::onApiResponse: " "body: %s", body.c_str());

    /*
    | Import the typed response — the /webapp/latest endpoint with a fixed
    | branch returns a single version object (not an array).
    | Expected shape:
    |   { "version": "5.2.0", "branch": "testing", "files": [
    |       { "path": "index.html.gz", "md5": "abc123" },
    |       ...
    |   ] }
    | The response is imported into the persistent webapp.current ConfigDB
    | object, then read through generated accessors.
    */
    MemoryDataStream input(std::move(body));
    ConfigDB::Status importStatus;
    {
        AppConfig::Webapp webapp(*app.cfg);
        if(auto update = webapp.update()) {
            importStatus = update.current.importFromStream(ConfigDB::Json::format, input);
        } else {
            importStatus = ConfigDB::Status{ConfigDB::Error::UpdateConflict};
        }
    }
    if(!importStatus) {
        cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "ConfigDB import error: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, importStatus.toString().c_str());
        failAttempt(kStatusApiError);
        return 0;
    }

    AppConfig::Webapp webapp(*app.cfg);
    auto current = webapp.current;
    _pendingVersion = current.getVersion();
    if(_pendingVersion.length() == 0) {
        cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "missing 'version' field" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }

    // Compare against installed version
    if(webapp.getInstalledVersion() == _pendingVersion) {
        cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "already up to date (" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, _pendingVersion.c_str());
        setState(State::IDLE);
        saveState(_pendingVersion, webapp.getInstalledMd5(), kStatusNoUpdate);
        return 0;
    }

    // Populate file list
    if(current.files.getItemCount() == 0) {
        cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "no files in response" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }

    if(current.getBasepath().length() == 0) {
        cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "missing basepath in response" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }

    for(unsigned i = 0; i < current.files.getItemCount(); ++i) {
        auto file = current.files[i];
        if(file.getFilename().length() == 0 || file.getMd5().length() == 0) {
            cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "file entry missing filename/md5, skipping version" ANSI_COLOR_RESET);
            failAttempt(kStatusApiError);
            return 0;
        }
    }

    // Optional size hints from the API: a top-level "total_size" and/or a per-file
    // "size".  Used below to verify the bundle can fit before we touch the active
    // webapp.  Older servers omit these → bundleTotalSize stays 0 and the space
    // check is skipped (backward compatible).
    size_t bundleTotalSize = current.getTotalSize();
    if(bundleTotalSize == 0) {
        for(unsigned i = 0; i < current.files.getItemCount(); ++i) {
            bundleTotalSize += current.files[i].getSize();
        }
    }

    _totalFiles = current.files.getItemCount();
    cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "will download " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " files for version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            _totalFiles, _pendingVersion.c_str());

    // Resume support: if staging already has a correctly-verified file from a
    // previous (interrupted) download attempt, skip re-downloading it. Only the
    // index is kept - the file's own data stays in ConfigDB.
    _pendingFileIndices.clear();
    _pendingFileIndices.reserve(_totalFiles);
    for(unsigned i = 0; i < current.files.getItemCount(); ++i) {
        auto file = current.files[i];
        String filename = file.getFilename();
        String sp = stagingPath(filename);
        if(fileExist(sp) && verifyFileMd5(sp, file.getMd5())) {
            cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "resume: skipping already-verified " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, filename.c_str());
        } else {
            _pendingFileIndices.push_back((uint16_t)i);
        }
    }

    if(_pendingFileIndices.empty()) {
        // All files already staged and verified — go straight to activation.
        cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "all files already staged, activating" ANSI_COLOR_RESET);
        setState(State::ACTIVATING);
        _fileIndex = 0;
        broadcastStatus();
        _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::activateStagingDeferred, this));
        _retryTimer.startOnce();
        return 0;
    }

    cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " files to download (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " already staged)" ANSI_COLOR_RESET,
            (unsigned)_pendingFileIndices.size(), _totalFiles - (unsigned)_pendingFileIndices.size());

    // Verify the whole bundle can fit before we purge the currently-active
    // webapp.  We compare against the total volume size (not free space) because
    // purgeOldWebapp() reclaims the old bundle first.  Skipping here — rather than
    // purging and then failing mid-download — keeps the working webapp intact when
    // a bundle is simply too large for this partition.
    if(bundleTotalSize > 0) {
        IFS::FileSystem::Info fsInfo;
        if(fileGetSystemInfo(fsInfo) == FS_OK) {
            size_t needed = bundleTotalSize + FS_DOWNLOAD_MARGIN;
            cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "bundle " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes (+" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " margin), volume " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes, free " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET,
                    (unsigned)bundleTotalSize, (unsigned)FS_DOWNLOAD_MARGIN, (unsigned)fsInfo.volumeSize, (unsigned)fsInfo.freeSpace);
            if(needed > fsInfo.volumeSize) {
                cdebug_e(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_RED "bundle too large for filesystem (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_RED " > " ANSI_COLOR_CYAN "%u" ANSI_COLOR_RED "), skipping update" ANSI_COLOR_RESET,
                        (unsigned)needed, (unsigned)fsInfo.volumeSize);
                failAttempt(kStatusLowSpace);
                return 0;
            }
        }
    }

    // Mark download as in-progress in persistent config so a reboot can resume.
    if(auto update = webapp.update()) {
        update.setInProgress(true);
    }

    // (b) A different version is available: free the old webapp assets NOW so the
    // new bundle has room to download on the small LittleFS partition (old and new
    // content-hashed bundles cannot coexist).  The update UI is served from a
    // firmware-embedded page, so the device stays reachable while downloading.
    cdebug_i(WEBAPPOTA, "WebappOta::onApiResponse: " ANSI_COLOR_BLUE "purging old webapp assets before download" ANSI_COLOR_RESET);
    purgeOldWebapp();

    setState(State::DOWNLOADING);
    _fileIndex = 0;
    broadcastStatus();
    // Defer startNextDownload out of the HTTP callback context.
    // Calling downloadFile() from inside onMessageComplete (via
    // requestCompletedDelegate) re-enters the HttpClientConnection state
    // machine before it has finished cleaning up the current request, which
    // can cause a hang/WDT reset.  A 1 ms timer breaks us out of that context.
    _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::startNextDownload, this));
    _retryTimer.startOnce();
    return 0;
}

// ─── File download ────────────────────────────────────────────────────────────

void WebappOta::startNextDownload()
{
    if(_fileIndex >= (unsigned)_pendingFileIndices.size()) {
        // All files downloaded; move to activation
        setState(State::ACTIVATING);
        broadcastStatus();
        if(!activateStaging()) {
            failAttempt(kStatusActivationError);
        }
        return;
    }

    // Require at least 12 KB free heap before starting a download.
    // The HttpClient + lwIP TCP buffers + FileStream need headroom.
    // Back off briefly and retry — each retry re-checks heap availability.
    if(app.getFreeHeapSize() < WEBAPP_OTA_MIN_UPDATE_HEAP) {
        cdebug_w(WEBAPPOTA, "WebappOta::startNextDownload: " ANSI_COLOR_YELLOW "low heap (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW ", should be " ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW "), backing off 5s" ANSI_COLOR_RESET,
                app.getFreeHeapSize(), WEBAPP_OTA_MIN_UPDATE_HEAP);
        saveState(String::nullstr, String::nullstr, kStatusLowHeap);
        // Don't broadcastStatus here — we already checked heap is low and broadcastStatus
        // itself allocates.  The updating page will get the next push when download resumes.
        _retryTimer.initializeMs(5000, TimerDelegate(&WebappOta::startNextDownload, this));
        _retryTimer.startOnce();
        return;
    }

    AppConfig::Webapp webapp(*app.cfg);
    auto current = webapp.current;
    auto file = current.files[_pendingFileIndices[_fileIndex]];
    String filename = file.getFilename();
    String destPath = stagingPath(filename);

    if(!ensureParentDir(destPath)) {
        cdebug_e(WEBAPPOTA, "WebappOta::startNextDownload: " ANSI_COLOR_RED "makedirs failed for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, destPath.c_str());
        failAttempt(kStatusDownloadError);
        return;
    }

    String url = current.getBasepath() + filename;
    cdebug_i(WEBAPPOTA, "WebappOta::startNextDownload: " ANSI_COLOR_BLUE "[" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "/" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " → " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            _fileIndex + 1, (unsigned)_pendingFileIndices.size(), url.c_str(), destPath.c_str());
    broadcastStatus();

    if(!_httpClient.downloadFile(url, destPath,
            RequestCompletedDelegate(&WebappOta::onFileDownloaded, this))) {
        cdebug_e(WEBAPPOTA, "WebappOta::startNextDownload: " ANSI_COLOR_RED "failed to queue download" ANSI_COLOR_RESET);
        failAttempt(kStatusDownloadError);
    }
}

int WebappOta::onFileDownloaded(HttpConnection& client, bool successful)
{
    auto* response = client.getResponse();
    int code = response ? (int)response->code : 0;

    if(!successful || (code != 200 && code != 0)) {
        AppConfig::Webapp webapp(*app.cfg);
        String filename = webapp.current.files[_pendingFileIndices[_fileIndex]].getFilename();
        String destPath = stagingPath(filename);
        cdebug_e(WEBAPPOTA, "WebappOta::onFileDownloaded: " ANSI_COLOR_RED "HTTP " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED " for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, code, destPath.c_str());
        failAttempt(kStatusDownloadError);
        return 0;
    }

    // Defer MD5 verification to the next event-loop tick.
    // The HttpClientConnection destroys the FileStream (response buffer) after
    // the callback returns, which is when the file is actually flushed and
    // closed on LittleFS.  Reading the file inside this callback would see an
    // empty or partial file.  A 1 ms one-shot timer defers execution until
    // after the connection cleanup completes.
    _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::verifyAndContinue, this));
    _retryTimer.startOnce();

    return 0;
}

void WebappOta::verifyAndContinue()
{
    AppConfig::Webapp webapp(*app.cfg);
    auto file = webapp.current.files[_pendingFileIndices[_fileIndex]];
    String destPath = stagingPath(file.getFilename());

    if(!verifyFileMd5(destPath, file.getMd5())) {
        cdebug_e(WEBAPPOTA, "WebappOta::verifyAndContinue: " ANSI_COLOR_RED "MD5 mismatch for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, destPath.c_str());
        failAttempt(kStatusMd5Error);
        return;
    }

    cdebug_i(WEBAPPOTA, "WebappOta::verifyAndContinue: " ANSI_COLOR_BLUE "[" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "/" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "] OK: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            _fileIndex + 1, (unsigned)_pendingFileIndices.size(), destPath.c_str());

    ++_fileIndex;
    broadcastStatus();
    startNextDownload();
}

// ─── MD5 verification ────────────────────────────────────────────────────────

bool WebappOta::verifyFileMd5(const String& filePath, const String& expectedMd5)
{
    FileStream fs;
    if(!fs.open(filePath, File::ReadOnly)) {
        cdebug_e(WEBAPPOTA, "WebappOta::verifyFileMd5: " ANSI_COLOR_RED "cannot open " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, filePath.c_str());
        return false;
    }

    Crypto::Md5 md5;
    uint8_t buf[256];
    while(true) {
        int n = fs.readBytes(reinterpret_cast<char*>(buf), sizeof(buf));
        if(n <= 0) break;
        md5.update(buf, n);
    }
    fs.close();

    String computed = Crypto::toString(md5.getHash());
    computed.toLowerCase();

    bool match = (computed == expectedMd5);
    if(!match) {
        cdebug_e(WEBAPPOTA, "WebappOta::verifyFileMd5: " ANSI_COLOR_RED "expected " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " got " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET,
                expectedMd5.c_str(), computed.c_str(), filePath.c_str());
    }
    return match;
}

// ─── Activation ──────────────────────────────────────────────────────────────

/**
 * @brief Recursively delete everything under @p dir (files and subdirectories),
 *        then remove @p dir itself.
 *
 * Directory type is taken from the entry's stat attribute rather than probing
 * with Directory::open(), which is both faster and avoids misclassifying files.
 * LittleFS' lfs_remove() deletes empty directories, so once a subtree has been
 * emptied the directory node is removed as well — otherwise stale empty dirs
 * would accumulate across repeated updates.
 */
static void deleteTree(const String& dir)
{
    Directory d;
    if(!d.open(dir)) {
        return;
    }
    struct Entry {
        String path;
        bool isDir;
    };
    std::vector<Entry> entries;
    while(d.next()) {
        const auto& stat = d.stat();
        entries.push_back({dir + "/" + stat.name.c_str(), stat.attr[FileAttribute::Directory]});
    }
    d.close();
    for(auto& e : entries) {
        if(e.isDir) {
            deleteTree(e.path);
        } else {
            fileDelete(e.path);
        }
    }
    // Remove the now-empty directory node itself.
    fileDelete(dir);
}

/**
 * @brief Delete old webapp files from the active filesystem before activating
 *        a new version.  Only removes known webapp-owned directories/files so
 *        that config/, captive.html, updating.html and other firmware files are
 *        preserved.
 *
 * Content-hashed asset filenames change every build, so old chunks would
 * otherwise accumulate and fill LittleFS.
 */
void WebappOta::purgeOldWebapp()
{
    // Directories that belong entirely to the webapp bundle
    static const char* const WEBAPP_DIRS[] = {"assets", "icons", nullptr};
    for(int i = 0; WEBAPP_DIRS[i] != nullptr; ++i) {
        deleteTree(String(WEBAPP_DIRS[i]));
    }

    // Root-level gzip files from the webapp (index.html.gz etc.)
    Directory root;
    if(root.open("")) {
        std::vector<String> toDelete;
        while(root.next()) {
            auto& stat = root.stat();
            if(stat.attr[FileAttribute::Directory]) continue;
            String name = stat.name.c_str();
            if(name.endsWith(F(".gz"))) {
                toDelete.push_back(name);
            }
        }
        root.close();
        for(auto& f : toDelete) {
            cdebug_d(WEBAPPOTA, "WebappOta::purgeOldWebapp: " "deleting %s", f.c_str());
            fileDelete(f);
        }
    }
}

/**
 * @brief Recursively move all files from @p srcDir to @p dstDir.
 *
 * For each file found in @p srcDir (recursively), the corresponding file in
 * @p dstDir is deleted (if present) and the staged file is renamed into place.
 * The source file's parent path relative to @p srcDir is re-created under
 * @p dstDir as needed.
 *
 * @note LittleFS rename() supports moving a file across directories as long
 *       as the destination parent directory exists.
 */
bool WebappOta::moveTree(const String& srcDir, const String& dstDir)
{
    Directory dir;
    if(!dir.open(srcDir)) {
        cdebug_e(WEBAPPOTA, "WebappOta::moveTree: " ANSI_COLOR_RED "cannot open " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, srcDir.c_str());
        return false;
    }

    bool ok = true;
    while(dir.next()) {
        auto& stat = dir.stat();
        String name = stat.name.c_str();
        String src = srcDir + "/" + name;
        String dst = dstDir + "/" + name;

        if(stat.attr[FileAttribute::Directory]) {
            if(!ensureParentDir(dst + "/_")) { // ensure dstDir/<subdir> exists
                createDirectories(dst);
            }
            if(!moveTree(src, dst)) {
                ok = false;
            }
        } else {
            // Delete destination file if it exists (ignore errors)
            if(fileExist(dst)) {
                fileDelete(dst);
            }
            if(!ensureParentDir(dst)) {
                cdebug_e(WEBAPPOTA, "WebappOta::moveTree: " ANSI_COLOR_RED "makedirs failed for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, dst.c_str());
                ok = false;
                continue;
            }
            int res = fileRename(src, dst);
            if(res < 0) {
                cdebug_e(WEBAPPOTA, "WebappOta::moveTree: " ANSI_COLOR_RED "rename " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " → " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " failed (" ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED ")" ANSI_COLOR_RESET, src.c_str(), dst.c_str(), res);
                ok = false;
            } else {
                cdebug_d(WEBAPPOTA, "WebappOta::moveTree: " "%s → %s", src.c_str(), dst.c_str());
            }
        }
    }
    return ok;
}

void WebappOta::activateStagingDeferred()
{
    if(!activateStaging()) {
        failAttempt(kStatusActivationError);
    }
}

void WebappOta::retryFromScratchDeferred()
{
    if(_state != State::IDLE) {
        return;
    }
    if(_lastBranch.length() == 0 || _lastFirmwareVersion.length() == 0 || _lastApiBaseUrl.length() == 0) {
        failAttempt(kStatusApiError);
        return;
    }

    cdebug_i(WEBAPPOTA, "WebappOta::retryFromScratchDeferred: " ANSI_COLOR_BLUE "restarting OTA from scratch" ANSI_COLOR_RESET);
    queryApi(_lastBranch, _lastFirmwareVersion, _lastApiBaseUrl);
}

void WebappOta::failAttempt(const char* status)
{
    // If we resumed an interrupted update (e.g. chip reboot mid-download) and this
    // attempt now fails, clear staging and restart once from scratch automatically.
    if(_resumingInterrupted && !_retryAfterCleanupDone
            && status != nullptr
            && std::strcmp(status, kStatusNoUpdate) != 0
            && std::strcmp(status, kStatusOk) != 0
            && std::strcmp(status, kStatusLowHeap) != 0) {
        cdebug_w(WEBAPPOTA, "WebappOta::failAttempt: " ANSI_COLOR_YELLOW "resumed OTA failed, clearing staging and retrying once" ANSI_COLOR_RESET);
        cleanupStaging();
        setState(State::IDLE);
        _resumingInterrupted = false;
        _retryAfterCleanupDone = true;
        _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::retryFromScratchDeferred, this));
        _retryTimer.startOnce();
        return;
    }

    setState(State::IDLE);
    saveState(String::nullstr, String::nullstr, status);
}

bool WebappOta::activateStaging()
{
    cdebug_i(WEBAPPOTA, "WebappOta::activateStaging: " ANSI_COLOR_BLUE "activating version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _pendingVersion.c_str());

    // Purge stale webapp files (content-hashed assets change names each build)
    // before moving in the new version to free up space first.
    purgeOldWebapp();

    // Move staged files to the filesystem root (where the webserver serves from)
    if(!moveTree(STAGING_ROOT, "")) {
        cdebug_e(WEBAPPOTA, "WebappOta::activateStaging: " ANSI_COLOR_RED "moveTree failed" ANSI_COLOR_RESET);
        return false;
    }

    // Cleanup empty staging directories
    cleanupStaging();

    // Use the last downloaded file's MD5 as the overall bundle fingerprint for now
    String bundleMd5;
    if(!_pendingFileIndices.empty()) {
        AppConfig::Webapp webapp(*app.cfg);
        bundleMd5 = webapp.current.files[_pendingFileIndices.back()].getMd5();
    }

    setState(State::IDLE);
    saveState(_pendingVersion, bundleMd5, kStatusOk);

    cdebug_i(WEBAPPOTA, "WebappOta::activateStaging: " ANSI_COLOR_BLUE "webapp updated to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _pendingVersion.c_str());
    app.wsBroadcast(F("notification"),
                    F("Webapp updated to ") + _pendingVersion);

    // Reboot to reclaim heap used during download before serving the webapp.
    // The updating.html page polls /webapp_status; when it sees last_status=="ok"
    // it reloads — the reload will land on a freshly booted device.
#ifdef ARCH_HOST
    // On the Host emulator there is no heap pressure to reclaim and a restart
    // would tear down the emulator process, so skip the reboot.
    cdebug_i(WEBAPPOTA, "WebappOta::activateStaging: " ANSI_COLOR_BLUE "skipping reboot on host" ANSI_COLOR_RESET);
#else
    cdebug_i(WEBAPPOTA, "WebappOta::activateStaging: " ANSI_COLOR_BLUE "webapp update complete; rebooting in 5 seconds to activate version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RESET, _pendingVersion.c_str());
    System.restart(5000); // Allow the terminal status to reach the frontend before reboot
#endif

    return true;
}

// ─── Persistence ─────────────────────────────────────────────────────────────

void WebappOta::saveState(const String& version, const String& md5, const char* status)
{
    if(status == nullptr) {
        status = "";
    }

    AppConfig::Webapp webapp(*app.cfg);
    if(auto update = webapp.update()) {
        if(version.length() > 0) {
            update.setInstalledVersion(version);
        }
        if(md5.length() > 0) {
            update.setInstalledMd5(md5);
        }
        update.setLastCheckStatus(status);
        // Clear in_progress whenever we reach a terminal state.
        // It is set to true by checkForUpdate() when a download begins.
                    if(strcmp(status, kStatusOk) == 0 || strcmp(status, kStatusNoUpdate) == 0 || strcmp(status, kStatusApiError) == 0 ||
                            strcmp(status, kStatusDownloadError) == 0 || strcmp(status, kStatusMd5Error) == 0 || strcmp(status, kStatusActivationError) == 0) {
            update.setInProgress(false);
        }
    }
    cdebug_d(WEBAPPOTA, "WebappOta::saveState: " "version=%s md5=%s status=%s",
                        version.c_str(), md5.c_str(), status);
    // Push updated state to all websocket clients so the updating page
    // reacts immediately without waiting for the next HTTP poll.
    // Skip for transient backoff states where heap is already known to be low.
    if(strcmp(status, kStatusLowHeap) != 0) {
        broadcastStatus();
    }
}

void WebappOta::cleanupStaging()
{
    // Best-effort recursive delete of staging directory contents.
    // This must remove nested files, otherwise repeated failed/interrupted
    // updates can slowly fill LittleFS and block future OTA attempts.
    Directory dir;
    if(!dir.open(STAGING_ROOT)) {
        return;
    }
    dir.close();

    cdebug_i(WEBAPPOTA, "WebappOta::cleanupStaging: " ANSI_COLOR_BLUE "recursively clearing staging/" ANSI_COLOR_RESET);
    deleteTree(STAGING_ROOT);
}

void WebappOta::listDirectory(const String& path, int depth)
{
    Directory dir;
    
    // Open the current directory path scope
    if (!dir.open(path)) {
        return; 
    }

    while (dir.next()) {
        String name = String(dir.stat().name.c_str());
        
        // Skip hidden/special files if applicable (e.g., "." or "..")
        if (name == "." || name == "..") {
            continue;
        }

        printIndent(depth);
        if(dir.stat().attr[FileAttribute::Directory])
        {
            cdebug_i(WEBAPPOTA, "WebappOta::listDirectory: " ANSI_COLOR_BLUE "  ├── [d] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, name.c_str());
            
            // Construct the next path branch
            String nextPath = path;
            if (!nextPath.endsWith("/")) {
                nextPath += "/";
            }
            nextPath += name;

            // Recurse into the sub-directory
            listDirectory(nextPath, depth + 1);
        } else {
            cdebug_i(WEBAPPOTA, "WebappOta::listDirectory: " ANSI_COLOR_BLUE "  ├── [f] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes)" ANSI_COLOR_RESET, name.c_str(), dir.stat().size);
        }
    }
    
    dir.close();
}

void WebappOta::printFileSystemUsage() {
    IFS::FileSystem::Info fsInfo;
    
    // Populate the structure with the active filesystem's data
    int result = fileGetSystemInfo(fsInfo);
    
    if (result == FS_OK) {
        // Compute metrics directly from the returned architecture fields
        size_t totalBytes = fsInfo.volumeSize;
        size_t freeBytes  = fsInfo.freeSpace;
        size_t usedBytes  = totalBytes - freeBytes;
        
        cdebug_i(WEBAPPOTA, "WebappOta::printFileSystemUsage: " ANSI_COLOR_BLUE "Total FS Size: " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET, totalBytes);
        cdebug_i(WEBAPPOTA, "WebappOta::printFileSystemUsage: " ANSI_COLOR_BLUE "Used Space:    " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET, usedBytes);
        cdebug_i(WEBAPPOTA, "WebappOta::printFileSystemUsage: " ANSI_COLOR_BLUE "Free Space:    " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET, freeBytes);
    } else {
        cdebug_e(WEBAPPOTA, "WebappOta::printFileSystemUsage: " ANSI_COLOR_RED "Failed to retrieve filesystem information. Error code: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET, result);
    }
}
// ─── Status JSON ─────────────────────────────────────────────────────────────

void WebappOta::fillStatusJson(JsonObject& obj) const
{
    static const char* stateNames[] = {
        "idle",         // IDLE
        "querying_api", // QUERYING_API
        "downloading",  // DOWNLOADING
        "activating",   // ACTIVATING
    };
    obj[F("state")] = stateNames[static_cast<int>(_state)];
    // "file" = number of files fully processed (skipped + downloaded so far)
    unsigned done = (_totalFiles > (unsigned)_pendingFileIndices.size())
                    ? _totalFiles - (unsigned)_pendingFileIndices.size()
                    : 0;
    obj[F("file")]  = (int)(done + _fileIndex);
    obj[F("total")] = (int)(_totalFiles > 0 ? _totalFiles : _pendingFileIndices.size());

    if(_state == State::DOWNLOADING && _fileIndex < (unsigned)_pendingFileIndices.size()) {
        AppConfig::Webapp webapp(*app.cfg);
        obj[F("file_path")] = webapp.current.files[_pendingFileIndices[_fileIndex]].getFilename();
    }
    if(_pendingVersion.length() > 0) {
        obj[F("version")] = _pendingVersion;
    }

    // Read persisted fields from ConfigDB
    AppConfig::Webapp webapp(*app.cfg);
    obj[F("last_status")] = webapp.getLastCheckStatus();
    obj[F("in_progress")] = webapp.getInProgress();
}

void WebappOta::broadcastStatus() const
{
    // Don't broadcast if heap is too tight — wsBroadcast allocates a JsonRpcMessage,
    // a serialised String, and a char[] frame buffer.  Attempting this in a low-heap
    // situation (the very condition we're trying to report) can itself crash the device.
    static constexpr size_t MIN_BROADCAST_HEAP = 8192;
    if(app.getFreeHeapSize() < MIN_BROADCAST_HEAP) {
        cdebug_w(WEBAPPOTA, "WebappOta::broadcastStatus: " ANSI_COLOR_YELLOW "skipping, low heap (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW ")" ANSI_COLOR_RESET, app.getFreeHeapSize());
        return;
    }
    DynamicJsonDocument doc(256);
    JsonObject params = doc.to<JsonObject>();
    fillStatusJson(params);
    app.wsBroadcast(F("webapp_ota_status"), params);
}
