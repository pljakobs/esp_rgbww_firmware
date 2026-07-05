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
        debug_e(ANSI_COLOR_RED "WebappOta::ensureParentDir - makedirs('" ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "') = " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET, dir.c_str(), res);
        return false;
    }
    res = createDirectory(dir);
    if(res < 0 && res != IFS::Error::Exists) {
        debug_e(ANSI_COLOR_RED "WebappOta::ensureParentDir - mkdir('" ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "') = " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET, dir.c_str(), res);
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
    // Find the third hyphen-separated token
    int firstDash = firmwareVersion.indexOf('-');
    if(firstDash < 0) {
        return F("experimental");
    }
    int secondDash = firmwareVersion.indexOf('-', firstDash + 1);
    if(secondDash < 0) {
        return F("experimental");
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
    AppConfig::Root::Webapp webapp(*app.cfg);
    return webapp.getInProgress();
}

void WebappOta::checkForUpdate(bool ignoreEnabled)
{
    debug_i(ANSI_COLOR_BLUE "WebappOta::checkForUpdate - ignoreEnabled=" ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, ignoreEnabled);
    
    debug_i(ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    debug_i(ANSI_COLOR_BLUE "| file system size and usage |" ANSI_COLOR_RESET);
    debug_i(ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    printFileSystemUsage();
    
    debug_i(ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    debug_i(ANSI_COLOR_BLUE "|   current directory layout |" ANSI_COLOR_RESET);
    debug_i(ANSI_COLOR_BLUE "==============================" ANSI_COLOR_RESET);
    #ifndef ARCH_HOST
    listDirectory("/", 0);
    #endif

    IFS::FileSystem::Info fsInfo;
    int result=fileGetSystemInfo(fsInfo);
    // ToDo: this is just checking for a plain 350kB free space condition. In future, the update server shall provide
    // the proper size of the update package and the firmware can check against that
    if (result != FS_OK || fsInfo.freeSpace <= FS_MIN_FREE_SPACE) {
        debug_e(ANSI_COLOR_RED "WebappOta::checkForUpdate - failed to get filesystem info or no free space" ANSI_COLOR_RESET);
        return;
    }

    if(_state != State::IDLE) {
        debug_i(ANSI_COLOR_BLUE "WebappOta::checkForUpdate - already active, skipping" ANSI_COLOR_RESET);
        return;
    }

    AppConfig::Root::Webapp webapp(*app.cfg);
    if(!ignoreEnabled && !webapp.getEnabled()) {
        debug_i(ANSI_COLOR_BLUE "WebappOta::checkForUpdate - disabled in config" ANSI_COLOR_RESET);
        return;
    }

    bool interrupted = webapp.getInProgress();
    if(interrupted) {
        debug_i(ANSI_COLOR_BLUE "WebappOta::checkForUpdate - resuming interrupted download" ANSI_COLOR_RESET);
    }

    String apiBaseUrl = webapp.getApiBaseUrl();
    String branch = extractBranch(fw_git_version);

    _resumingInterrupted = interrupted;
    _retryAfterCleanupDone = false;
    _lastBranch = branch;
    _lastFirmwareVersion = fw_git_version;
    _lastApiBaseUrl = apiBaseUrl;

    debug_i(ANSI_COLOR_BLUE "WebappOta::checkForUpdate - branch=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " fw=" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, branch.c_str(), fw_git_version);
    queryApi(branch, fw_git_version, apiBaseUrl);
}

// ─── API query ───────────────────────────────────────────────────────────────

void WebappOta::queryApi(const String& branch, const String& firmwareVersion, const String& apiBaseUrl)
{
    _state = State::QUERYING_API;
    broadcastStatus();
    _files.clear();
    _fileIndex = 0;

    String url = apiBaseUrl + F("/webapp/latest?branch=") + branch
               + F("&firmware_version=") + firmwareVersion;

    debug_i(ANSI_COLOR_BLUE "WebappOta::queryApi - GET " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, url.c_str());

    if(!_httpClient.downloadString(url,
            RequestCompletedDelegate(&WebappOta::onApiResponse, this), 4096)) {
        debug_e(ANSI_COLOR_RED "WebappOta::queryApi - failed to queue request" ANSI_COLOR_RESET);
        _state = State::IDLE;
        saveState(String::nullstr, String::nullstr, kStatusApiError);
    }
}

int WebappOta::onApiResponse(HttpConnection& client, bool successful)
{
    auto* response = client.getResponse();
    if(!response) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - no response object" ANSI_COLOR_RESET);
        _state = State::IDLE;
        saveState(String::nullstr, String::nullstr, kStatusApiError);
        return 0;
    }

    int code = (int)response->code;

    if(!successful || (code != 200 && code != 0 /* 0 = no code set */)) {
        debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - HTTP " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE ", no update available" ANSI_COLOR_RESET, code);
        failAttempt((code == 404) ? kStatusNoUpdate : kStatusApiError);
        return 0;
    }

    String body = response->getBody();
    if(body.length() == 0) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - empty body" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }

    debug_d("WebappOta::onApiResponse - body: %s", body.c_str());

    /*
    | Parse JSON — the /webapp/latest endpoint with a fixed branch returns a
    | single version object (not an array).
    | Expected shape:
    |   { "version": "5.2.0", "branch": "testing", "files": [
    |       { "path": "index.html.gz", "md5": "abc123" },
    |       ...
    |   ] }
    | Response is ~1 KB raw JSON; ArduinoJson needs ~2-3x that internally.
    | Use DynamicJsonDocument on the heap to avoid stack overflow on ESP8266.
    */
    DynamicJsonDocument doc(3072);
    DeserializationError err = deserializeJson(doc, body);
    if(err) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - JSON parse error: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, err.c_str());
        failAttempt(kStatusApiError);
        return 0;
    }

    const char* version = doc["version"];
    if(version == nullptr) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - missing 'version' field" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }
    _pendingVersion = version;

    // Compare against installed version
    {
        AppConfig::Root::Webapp webapp(*app.cfg);
        if(webapp.getInstalledVersion() == _pendingVersion) {
            debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - already up to date (" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, _pendingVersion.c_str());
            _state = State::IDLE;
            saveState(_pendingVersion, webapp.getInstalledMd5(), kStatusNoUpdate);
            return 0;
        }
    }

    // Populate file list
    JsonArray files = doc["files"];
    if(files.isNull() || files.size() == 0) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - no files in response" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }

    const char* basepath = doc["basepath"];
    if(basepath == nullptr) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - missing basepath in response" ANSI_COLOR_RESET);
        failAttempt(kStatusApiError);
        return 0;
    }
    String base(basepath);

    for(JsonObject f : files) {
        const char* filename = f["filename"];
        const char* md5      = f["md5"];
        if(filename == nullptr || md5 == nullptr) {
            debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - file entry missing filename/md5, skipping version" ANSI_COLOR_RESET);
            failAttempt(kStatusApiError);
            return 0;
        }
        FileEntry entry;
        entry.path        = filename;
        entry.expectedMd5 = md5;
        entry.url         = base + filename;
        _files.push_back(entry);
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - will download " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE " files for version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            (int)_files.size(), _pendingVersion.c_str());

    _totalFiles = (unsigned)_files.size();

    // Resume support: if staging already has a correctly-verified file from a
    // previous (interrupted) download attempt, skip re-downloading it.
    {
        std::vector<FileEntry> pending;
        for(auto& f : _files) {
            String sp = stagingPath(f.path);
            if(fileExist(sp) && verifyFileMd5(sp, f.expectedMd5)) {
                debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - resume: skipping already-verified " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, f.path.c_str());
            } else {
                pending.push_back(std::move(f));
            }
        }
        _files = std::move(pending);
    }

    if(_files.empty()) {
        // All files already staged and verified — go straight to activation.
        debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - all files already staged, activating" ANSI_COLOR_RESET);
        _state = State::ACTIVATING;
        _fileIndex = 0;
        broadcastStatus();
        _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::activateStagingDeferred, this));
        _retryTimer.startOnce();
        return 0;
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " files to download (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " already staged)" ANSI_COLOR_RESET,
            (unsigned)_files.size(), _totalFiles - (unsigned)_files.size());

    // Mark download as in-progress in persistent config so a reboot can resume.
    {
        AppConfig::Root root(*app.cfg);
        if(auto update = root.update()) {
            update.webapp.setInProgress(true);
        }
    }

    _state = State::DOWNLOADING;
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
    if(_fileIndex >= (unsigned)_files.size()) {
        // All files downloaded; move to activation
        _state = State::ACTIVATING;
        broadcastStatus();
        if(!activateStaging()) {
            failAttempt(kStatusActivationError);
        }
        return;
    }

    // Require at least 16 KB free heap before starting a download.
    // The HttpClient + lwIP TCP buffers + FileStream need headroom.
    // Back off briefly and retry — each retry re-checks heap availability.
    static constexpr size_t MIN_DOWNLOAD_HEAP = 12000;
    if(app.getFreeHeapSize() < MIN_DOWNLOAD_HEAP) {
        debug_w(ANSI_COLOR_YELLOW "WebappOta::startNextDownload - low heap (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW "), backing off 5s" ANSI_COLOR_RESET,
                app.getFreeHeapSize());
        saveState(String::nullstr, String::nullstr, kStatusLowHeap);
        // Don't broadcastStatus here — we already checked heap is low and broadcastStatus
        // itself allocates.  The updating page will get the next push when download resumes.
        _retryTimer.initializeMs(5000, TimerDelegate(&WebappOta::startNextDownload, this));
        _retryTimer.startOnce();
        return;
    }

    const FileEntry& entry = _files[_fileIndex];
    String destPath = stagingPath(entry.path);

    if(!ensureParentDir(destPath)) {
        debug_e(ANSI_COLOR_RED "WebappOta::startNextDownload - makedirs failed for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, destPath.c_str());
        failAttempt(kStatusDownloadError);
        return;
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::startNextDownload - [" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "/" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " → " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            _fileIndex + 1, (unsigned)_files.size(), entry.url.c_str(), destPath.c_str());
    broadcastStatus();

    if(!_httpClient.downloadFile(entry.url, destPath,
            RequestCompletedDelegate(&WebappOta::onFileDownloaded, this))) {
        debug_e(ANSI_COLOR_RED "WebappOta::startNextDownload - failed to queue download" ANSI_COLOR_RESET);
        failAttempt(kStatusDownloadError);
    }
}

int WebappOta::onFileDownloaded(HttpConnection& client, bool successful)
{
    auto* response = client.getResponse();
    int code = response ? (int)response->code : 0;

    if(!successful || (code != 200 && code != 0)) {
        const FileEntry& entry = _files[_fileIndex];
        String destPath = stagingPath(entry.path);
        debug_e(ANSI_COLOR_RED "WebappOta::onFileDownloaded - HTTP " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED " for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, code, destPath.c_str());
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
    const FileEntry& entry = _files[_fileIndex];
    String destPath = stagingPath(entry.path);

    if(!verifyFileMd5(destPath, entry.expectedMd5)) {
        debug_e(ANSI_COLOR_RED "WebappOta::verifyAndContinue - MD5 mismatch for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, destPath.c_str());
        failAttempt(kStatusMd5Error);
        return;
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::verifyAndContinue - [" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "/" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE "] OK: " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            _fileIndex + 1, (unsigned)_files.size(), destPath.c_str());

    ++_fileIndex;
    broadcastStatus();
    startNextDownload();
}

// ─── MD5 verification ────────────────────────────────────────────────────────

bool WebappOta::verifyFileMd5(const String& filePath, const String& expectedMd5)
{
    FileStream fs;
    if(!fs.open(filePath, File::ReadOnly)) {
        debug_e(ANSI_COLOR_RED "WebappOta::verifyFileMd5 - cannot open " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, filePath.c_str());
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
        debug_e(ANSI_COLOR_RED "WebappOta::verifyFileMd5 - expected " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " got " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET,
                expectedMd5.c_str(), computed.c_str(), filePath.c_str());
    }
    return match;
}

// ─── Activation ──────────────────────────────────────────────────────────────

/**
 * @brief Recursively delete all files under @p dir (non-recursive subdirs only).
 */
static void deleteTree(const String& dir)
{
    Directory d;
    if(!d.open(dir)) {
        return;
    }
    std::vector<String> entries;
    while(d.next()) {
        entries.push_back(dir + "/" + d.stat().name.c_str());
    }
    d.close();
    for(auto& e : entries) {
        // Try as directory first; if it opens, recurse
        Directory sub;
        if(sub.open(e)) {
            sub.close();
            deleteTree(e);
        } else {
            fileDelete(e);
        }
    }
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
            debug_d("WebappOta::purgeOldWebapp - deleting %s", f.c_str());
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
        debug_e(ANSI_COLOR_RED "WebappOta::moveTree - cannot open " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, srcDir.c_str());
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
                debug_e(ANSI_COLOR_RED "WebappOta::moveTree - makedirs failed for " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED "" ANSI_COLOR_RESET, dst.c_str());
                ok = false;
                continue;
            }
            int res = fileRename(src, dst);
            if(res < 0) {
                debug_e(ANSI_COLOR_RED "WebappOta::moveTree - rename " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " → " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " failed (" ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED ")" ANSI_COLOR_RESET, src.c_str(), dst.c_str(), res);
                ok = false;
            } else {
                debug_d("WebappOta::moveTree - %s → %s", src.c_str(), dst.c_str());
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

    debug_i(ANSI_COLOR_BLUE "WebappOta::retryFromScratchDeferred - restarting OTA from scratch" ANSI_COLOR_RESET);
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
        debug_w(ANSI_COLOR_YELLOW "WebappOta::failAttempt - resumed OTA failed, clearing staging and retrying once" ANSI_COLOR_RESET);
        cleanupStaging();
        _state = State::IDLE;
        _resumingInterrupted = false;
        _retryAfterCleanupDone = true;
        _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::retryFromScratchDeferred, this));
        _retryTimer.startOnce();
        return;
    }

    _state = State::IDLE;
    saveState(String::nullstr, String::nullstr, status);
}

bool WebappOta::activateStaging()
{
    debug_i(ANSI_COLOR_BLUE "WebappOta::activateStaging - activating version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _pendingVersion.c_str());

    // Purge stale webapp files (content-hashed assets change names each build)
    // before moving in the new version to free up space first.
    purgeOldWebapp();

    // Move staged files to the filesystem root (where the webserver serves from)
    if(!moveTree(STAGING_ROOT, "")) {
        debug_e(ANSI_COLOR_RED "WebappOta::activateStaging - moveTree failed" ANSI_COLOR_RESET);
        return false;
    }

    // Cleanup empty staging directories
    cleanupStaging();

    // Use the last file's MD5 as the overall bundle fingerprint for now
    String bundleMd5;
    if(_files.size() > 0) {
        bundleMd5 = _files[_files.size() - 1].expectedMd5;
    }

    _state = State::IDLE;
    saveState(_pendingVersion, bundleMd5, kStatusOk);

    debug_i(ANSI_COLOR_BLUE "WebappOta::activateStaging - webapp updated to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _pendingVersion.c_str());
    app.wsBroadcast(F("notification"),
                    F("Webapp updated to ") + _pendingVersion);

    // Reboot to reclaim heap used during download before serving the webapp.
    // The updating.html page polls /webapp_status; when it sees last_status=="ok"
    // it reloads — the reload will land on a freshly booted device.
    debug_i(ANSI_COLOR_BLUE "WebappOta::activateStaging - rebooting to reclaim heap" ANSI_COLOR_RESET);
    System.restart(2000); // 2 s grace period for the status response to reach the browser

    return true;
}

// ─── Persistence ─────────────────────────────────────────────────────────────

void WebappOta::saveState(const String& version, const String& md5, const char* status)
{
    if(status == nullptr) {
        status = "";
    }

    AppConfig::Root root(*app.cfg);
    if(auto update = root.update()) {
        if(version.length() > 0) {
            update.webapp.setInstalledVersion(version);
        }
        if(md5.length() > 0) {
            update.webapp.setInstalledMd5(md5);
        }
        update.webapp.setLastCheckStatus(status);
        // Clear in_progress whenever we reach a terminal state.
        // It is set to true by checkForUpdate() when a download begins.
                    if(strcmp(status, kStatusOk) == 0 || strcmp(status, kStatusNoUpdate) == 0 || strcmp(status, kStatusApiError) == 0 ||
                            strcmp(status, kStatusDownloadError) == 0 || strcmp(status, kStatusMd5Error) == 0 || strcmp(status, kStatusActivationError) == 0) {
            update.webapp.setInProgress(false);
        }
    }
    debug_d("WebappOta::saveState - version=%s md5=%s status=%s",
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

    debug_i(ANSI_COLOR_BLUE "WebappOta::cleanupStaging - recursively clearing staging/" ANSI_COLOR_RESET);
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
            debug_i(ANSI_COLOR_BLUE "  ├── [d] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, name.c_str());
            
            // Construct the next path branch
            String nextPath = path;
            if (!nextPath.endsWith("/")) {
                nextPath += "/";
            }
            nextPath += name;

            // Recurse into the sub-directory
            listDirectory(nextPath, depth + 1);
        } else {
            debug_i(ANSI_COLOR_BLUE "  ├── [f] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes)" ANSI_COLOR_RESET, name.c_str(), dir.stat().size);
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
        
        debug_i(ANSI_COLOR_BLUE "Total FS Size: " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET, totalBytes);
        debug_i(ANSI_COLOR_BLUE "Used Space:    " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET, usedBytes);
        debug_i(ANSI_COLOR_BLUE "Free Space:    " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET, freeBytes);
    } else {
        debug_e(ANSI_COLOR_RED "Failed to retrieve filesystem information. Error code: " ANSI_COLOR_CYAN "%d" ANSI_COLOR_RED "" ANSI_COLOR_RESET, result);
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
    unsigned done = (_totalFiles > (unsigned)_files.size())
                    ? _totalFiles - (unsigned)_files.size()
                    : 0;
    obj[F("file")]  = (int)(done + _fileIndex);
    obj[F("total")] = (int)(_totalFiles > 0 ? _totalFiles : _files.size());

    if(_state == State::DOWNLOADING && _fileIndex < (unsigned)_files.size()) {
        obj[F("file_path")] = _files[_fileIndex].path;
    }
    if(_pendingVersion.length() > 0) {
        obj[F("version")] = _pendingVersion;
    }

    // Read persisted fields from ConfigDB
    AppConfig::Root::Webapp webapp(*app.cfg);
    obj[F("last_status")] = webapp.getLastCheckStatus();
    obj[F("in_progress")] = webapp.getInProgress();
}

void WebappOta::broadcastStatus() const
{
    // Don't broadcast if heap is too tight — wsBroadcast allocates a JsonRpcMessage,
    // a serialised String, and a char[] frame buffer.  Attempting this in a low-heap
    // situation (the very condition we're trying to report) can itself crash the device.
    static constexpr size_t MIN_BROADCAST_HEAP = 10240;
    if(app.getFreeHeapSize() < MIN_BROADCAST_HEAP) {
        debug_w(ANSI_COLOR_YELLOW "WebappOta::broadcastStatus - skipping, low heap (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW ")" ANSI_COLOR_RESET, app.getFreeHeapSize());
        return;
    }
    StaticJsonDocument<256> doc;
    JsonObject params = doc.to<JsonObject>();
    fillStatusJson(params);
    app.wsBroadcast(F("webapp_ota_status"), params);
}
