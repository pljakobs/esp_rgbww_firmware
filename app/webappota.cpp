/**
 * @file
 * @author  Peter Jakobs http://github.com/pljakobs
 *
 * Background webapp OTA — fetch webapp files from lightinator.de version API,
 * stage in LittleFS, verify MD5, then atomically activate.
 */

#include <webappota.h>
#include <application.h>
#include <ArduinoJson.h>
#include <FileSystem.h>
#include <IFS/FileCopier.h>
#include <Crypto/Md5.h>
#include <Data/HexString.h>
#include <Data/Stream/FileStream.h>
#include <cstring>
#include <vector>

static constexpr const char STAGING_ROOT[] = "staging";
static constexpr uint32_t RETRY_INTERVAL_MS = 5 * 60 * 1000; 

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

bool WebappOta::ensureParentDir(const String& path)
{
    int sep = path.lastIndexOf('/');
    if(sep <= 0) {
        return true; 
    }
    String dir = path.substring(0, sep);
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

String WebappOta::extractBranch(const String& firmwareVersion)
{
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
    
    IFS::FileSystem::Info fsInfo;
    int result = fileGetSystemInfo(fsInfo);
    if(result != FS_OK) {
        debug_e(ANSI_COLOR_RED "WebappOta::checkForUpdate - failed to get filesystem info" ANSI_COLOR_RESET);
        return;
    }

    if(fsInfo.freeSpace < FS_EMERGENCY_FREE_SPACE) {
        debug_w(ANSI_COLOR_YELLOW "WebappOta::checkForUpdate - emergency low space (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW " bytes), clearing staging" ANSI_COLOR_RESET, fsInfo.freeSpace);
        cleanupStaging();
        fileGetSystemInfo(fsInfo);
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

void WebappOta::setState(State newState)
{
    const bool wasActive = (_state != State::IDLE);
    _state = newState;
    const bool nowActive = (_state != State::IDLE);

    if(wasActive != nowActive) {
        app.webserver.applyOtaLoadShedding(nowActive);
    }
}

void WebappOta::queryApi(const String& branch, const String& firmwareVersion, const String& apiBaseUrl)
{
    setState(State::QUERYING_API);
    broadcastStatus();
    _files.clear();
    _fileIndex = 0;

    String url = apiBaseUrl + F("/webapp/latest?branch=") + branch + F("&firmware_version=") + firmwareVersion;
    debug_i(ANSI_COLOR_BLUE "WebappOta::queryApi - GET " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, url.c_str());

    if(!_httpClient.downloadString(url, RequestCompletedDelegate(&WebappOta::onApiResponse, this), 4096)) {
        debug_e(ANSI_COLOR_RED "WebappOta::queryApi - failed to queue request" ANSI_COLOR_RESET);
        setState(State::IDLE);
        saveState(String::nullstr, String::nullstr, kStatusApiError);
    }
}

int WebappOta::onApiResponse(HttpConnection& client, bool successful)
{
    auto* response = client.getResponse();
    if(!response) {
        debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - no response object" ANSI_COLOR_RESET);
        setState(State::IDLE);
        saveState(String::nullstr, String::nullstr, kStatusApiError);
        return 0;
    }

    int code = (int)response->code;
    if(!successful || (code != 200 && code != 0)) {
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

    {
        AppConfig::Root::Webapp webapp(*app.cfg);
        if(webapp.getInstalledVersion() == _pendingVersion) {
            debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - already up to date (" ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE ")" ANSI_COLOR_RESET, _pendingVersion.c_str());
            setState(State::IDLE);
            saveState(_pendingVersion, webapp.getInstalledMd5(), kStatusNoUpdate);
            return 0;
        }
    }

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
        entry.size        = f["size"].as<size_t>();
        _files.push_back(entry);
    }

    size_t bundleTotalSize = doc["total_size"].as<size_t>();
    if(bundleTotalSize == 0) {
        for(const auto& f : _files) {
            bundleTotalSize += f.size;
        }
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - will download " ANSI_COLOR_CYAN "%d" ANSI_COLOR_BLUE " files for version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET,
            (int)_files.size(), _pendingVersion.c_str());

    _totalFiles = (unsigned)_files.size();

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
        debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - all files already staged, activating" ANSI_COLOR_RESET);
        setState(State::ACTIVATING);
        _fileIndex = 0;
        broadcastStatus();
        _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::activateStagingDeferred, this));
        _retryTimer.startOnce();
        return 0;
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " files to download (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " already staged)" ANSI_COLOR_RESET,
            (unsigned)_files.size(), _totalFiles - (unsigned)_files.size());

    if(bundleTotalSize > 0) {
        IFS::FileSystem::Info fsInfo;
        if(fileGetSystemInfo(fsInfo) == FS_OK) {
            size_t needed = bundleTotalSize + FS_DOWNLOAD_MARGIN;
            debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - bundle " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes (+" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " margin), volume " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes, free " ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes" ANSI_COLOR_RESET,
                    (unsigned)bundleTotalSize, (unsigned)FS_DOWNLOAD_MARGIN, (unsigned)fsInfo.volumeSize, (unsigned)fsInfo.freeSpace);
            if(needed > fsInfo.volumeSize) {
                debug_e(ANSI_COLOR_RED "WebappOta::onApiResponse - bundle too large for filesystem (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_RED " > " ANSI_COLOR_CYAN "%u" ANSI_COLOR_RED "), skipping update" ANSI_COLOR_RESET,
                        (unsigned)needed, (unsigned)fsInfo.volumeSize);
                failAttempt(kStatusLowSpace);
                return 0;
            }
        }
    }

    {
        AppConfig::Root root(*app.cfg);
        if(auto update = root.update()) {
            update.webapp.setInProgress(true);
        }
    }

    debug_i(ANSI_COLOR_BLUE "WebappOta::onApiResponse - purging old webapp assets before download" ANSI_COLOR_RESET);
    purgeOldWebapp();

    setState(State::DOWNLOADING);
    _fileIndex = 0;
    broadcastStatus();
    _retryTimer.initializeMs<1>(TimerDelegate(&WebappOta::startNextDownload, this));
    _retryTimer.startOnce();
    return 0;
}

// ─── File download ────────────────────────────────────────────────────────────

void WebappOta::startNextDownload()
{
    if(_fileIndex >= (unsigned)_files.size()) {
        setState(State::ACTIVATING);
        broadcastStatus();
        if(!activateStaging()) {
            failAttempt(kStatusActivationError);
        }
        return;
    }

    static constexpr size_t MIN_DOWNLOAD_HEAP = 12000;
    if(app.getFreeHeapSize() < MIN_DOWNLOAD_HEAP) {
        debug_w(ANSI_COLOR_YELLOW "WebappOta::startNextDownload - low heap (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_YELLOW "), backing off 5s" ANSI_COLOR_RESET, app.getFreeHeapSize());
        saveState(String::nullstr, String::nullstr, kStatusLowHeap);
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

    if(!_httpClient.downloadFile(entry.url, destPath, RequestCompletedDelegate(&WebappOta::onFileDownloaded, this))) {
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
    fileDelete(dir);
}

void WebappOta::purgeOldWebapp()
{
    static const char* const WEBAPP_DIRS[] = {"assets", "icons", nullptr};
    for(int i = 0; WEBAPP_DIRS[i] != nullptr; ++i) {
        deleteTree(String(WEBAPP_DIRS[i]));
    }

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
/**
 * @brief Recursively copy files from @p srcDir to @p dstDir using IFS::FileCopier.
 */
bool WebappOta::moveTree(const String& srcDir, const String& dstDir)
{
    // Resolve the default file system reference. 
    // (Update this reference if your configuration uses a custom partition instance)
    IFS::FileSystem& fs = IFS::defaultFileSystem();

    // The constructor requires references to both source and destination filesystems
    IFS::FileCopier copier(fs, fs);

    // Execute the built-in recursive directory copy utility
    if(!copier.copyDir(srcDir, dstDir)) {
        debug_e(ANSI_COLOR_RED "WebappOta::moveTree - copyDir from " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_RED " failed" ANSI_COLOR_RESET, srcDir.c_str(), dstDir.c_str());
        return false;
    }

    debug_d("WebappOta::moveTree - successfully copied %s to %s", srcDir.c_str(), dstDir.c_str());
    return true;
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
    if(_resumingInterrupted && !_retryAfterCleanupDone
            && status != nullptr
            && std::strcmp(status, kStatusNoUpdate) != 0
            && std::strcmp(status, kStatusOk) != 0
            && std::strcmp(status, kStatusLowHeap) != 0) {
        debug_w(ANSI_COLOR_YELLOW "WebappOta::failAttempt - resumed OTA failed, clearing staging and retrying once" ANSI_COLOR_RESET);
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
    debug_i(ANSI_COLOR_BLUE "WebappOta::activateStaging - activating version " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _pendingVersion.c_str());

    purgeOldWebapp();

    if(!moveTree(STAGING_ROOT, "")) {
        debug_e(ANSI_COLOR_RED "WebappOta::activateStaging - moveTree failed" ANSI_COLOR_RESET);
        return false;
    }

    cleanupStaging();

    String bundleMd5;
    if(_files.size() > 0) {
        bundleMd5 = _files[_files.size() - 1].expectedMd5;
    }

    setState(State::IDLE);
    saveState(_pendingVersion, bundleMd5, kStatusOk);

    debug_i(ANSI_COLOR_BLUE "WebappOta::activateStaging - webapp updated to " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, _pendingVersion.c_str());
    app.wsBroadcast(F("notification"), F("Webapp updated to ") + _pendingVersion);

    debug_i(ANSI_COLOR_BLUE "WebappOta::activateStaging - rebooting to reclaim heap" ANSI_COLOR_RESET);
    System.restart(2000); 

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
        if(strcmp(status, kStatusOk) == 0 || strcmp(status, kStatusNoUpdate) == 0 || strcmp(status, kStatusApiError) == 0 ||
           strcmp(status, kStatusDownloadError) == 0 || strcmp(status, kStatusMd5Error) == 0 || strcmp(status, kStatusActivationError) == 0) {
            update.webapp.setInProgress(false);
        }
    }
    debug_d("WebappOta::saveState - version=%s md5=%s status=%s", version.c_str(), md5.c_str(), status);
    if(strcmp(status, kStatusLowHeap) != 0) {
        broadcastStatus();
    }
}

void WebappOta::cleanupStaging()
{
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
    if (!dir.open(path)) {
        return; 
    }

    while (dir.next()) {
        String name = String(dir.stat().name.c_str());
        if (name == "." || name == "..") {
            continue;
        }

        printIndent(depth);
        if(dir.stat().attr[FileAttribute::Directory]) {
            debug_i(ANSI_COLOR_BLUE "  ├── [d] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE "" ANSI_COLOR_RESET, name.c_str());
            String nextPath = path;
            if (!nextPath.endsWith("/")) {
                nextPath += "/";
            }
            nextPath += name;
            listDirectory(nextPath, depth + 1);
        } else {
            debug_i(ANSI_COLOR_BLUE "  ├── [f] " ANSI_COLOR_CYAN "%s" ANSI_COLOR_BLUE " (" ANSI_COLOR_CYAN "%u" ANSI_COLOR_BLUE " bytes)" ANSI_COLOR_RESET, name.c_str(), dir.stat().size);
        }
    }
    dir.close();
}

void WebappOta::printFileSystemUsage() 
{
    IFS::FileSystem::Info fsInfo;
    int result = fileGetSystemInfo(fsInfo);
    
    if (result == FS_OK) {
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
        "idle",         
        "querying_api", 
        "downloading",  
        "activating",   
    };
    obj[F("state")] = stateNames[static_cast<int>(_state)];
    unsigned done = (_totalFiles > (unsigned)_files.size()) ? _totalFiles - (unsigned)_files.size() : 0;
    obj[F("file")]  = (int)(done + _fileIndex);
    obj[F("total")] = (int)(_totalFiles > 0 ? _totalFiles : _files.size());

    if(_state == State::DOWNLOADING && _fileIndex < (unsigned)_files.size()) {
        obj[F("file_path")] = _files[_fileIndex].path;
    }
    if(_pendingVersion.length() > 0) {
        obj[F("version")] = _pendingVersion;
    }

    AppConfig::Root::Webapp webapp(*app.cfg);
    obj[F("last_status")] = webapp.getLastCheckStatus();
    obj[F("in_progress")] = webapp.getInProgress();
}

void WebappOta::broadcastStatus() const
{
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