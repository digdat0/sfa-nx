// In-app updater, driven from the - menu's Updates row (switch_menu.cpp).
//
// Each press of A advances one step:
//   idle/up to date/failed -> check: ask the GitHub API for the latest release of digdat0/sfa-nx
//   available              -> download: fetch the release's SFA-NX.nro to cache/update.nro and verify it
//   ready                  -> install: swap the new NRO in, ask the Homebrew loader to start it next, quit
//
// Network work runs on a detached thread so the game keeps rendering; the menu polls sfaUpdaterStatus each
// frame. The release tag (e.g. "v0.2.0") is compared with SFA_NX_VERSION, which CMake sets from
// PROJECT_VERSION. While the repository is private a GitHub token can be put in config/github_token.
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <switch.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Foxhollow's logger, and its request to leave the main loop cleanly (saves settings, shuts the GPU down).
extern "C" void pc_log_line(const char* fmt, ...);
extern "C" void fhRequestQuit(void);

// Set by CMake from PROJECT_VERSION; the fallback only keeps the file compiling on its own.
#ifndef SFA_NX_VERSION
#define SFA_NX_VERSION "0.0.0"
#endif

namespace {

constexpr const char* kLatestUrl = "https://api.github.com/repos/digdat0/sfa-nx/releases/latest";
constexpr const char* kAssetName = "SFA-NX.nro";
constexpr const char* kAppDir = "sdmc:/switch/SFA-NX/";
// The download goes to cache/ so the app folder only ever holds the NRO, the disc image and folders.
// kOldPath keeps the replaced NRO until the next start, when sfaUpdaterApplyPending deletes it.
constexpr const char* kPartPath = "sdmc:/switch/SFA-NX/cache/update.nro";
constexpr const char* kOldPath = "sdmc:/switch/SFA-NX/cache/previous.nro";
constexpr const char* kTokenPath = "sdmc:/switch/SFA-NX/config/github_token";
constexpr const char* kDefaultNro = "sdmc:/switch/SFA-NX/SFA-NX.nro";

// Updater state machine (see the top of the file). g_state is read by the menu thread and written by the
// worker thread, so it and the progress percentage are atomics; g_message is guarded by g_mutex.
enum State { kIdle, kChecking, kUpToDate, kAvailable, kDownloading, kReady, kFailed };

std::atomic<int> g_state{kIdle};
std::atomic<int> g_percent{0};
// True while a worker thread runs; further presses of A are ignored until it finishes.
std::atomic<bool> g_busy{false};
std::mutex g_mutex;
std::string g_message;
// One downloadable file of a release, as listed in the GitHub API reply. digest is "sha256:<hex>" when
// GitHub provides it; target is where the file is installed (the running NRO's path).
struct Asset {
    std::string name;
    std::string url;
    std::string digest;
    curl_off_t size = 0;
    std::string target;
};

// Result of the last check: the release tag and its usable assets. g_totalSize/g_doneSize turn per-file
// curl progress into one overall percentage.
std::string g_latest;
std::vector<Asset> g_assets;
curl_off_t g_totalSize;
curl_off_t g_doneSize;
// Path of the running NRO (from argv[0]), replaced on install. Falls back to the default location.
std::string g_nroPath = kDefaultNro;
bool g_netReady;

// Publishes a new state and status text, and logs it.
void set_state(int state, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_message = message;
    }
    g_state.store(state);
    pc_log_line("[sfa-nx] updater: %s", message.c_str());
}

// Starts the socket service and curl once, on first use; the game never needs the network otherwise.
bool net_init() {
    if (g_netReady) {
        return true;
    }
    if (R_FAILED(socketInitializeDefault())) {
        return false;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socketExit();
        return false;
    }
    g_netReady = true;
    return true;
}

// Optional GitHub personal access token from config/github_token (needed only while the repository is
// private). Trailing whitespace and newlines from a text editor are trimmed.
std::string read_token() {
    FILE* f = std::fopen(kTokenPath, "r");
    if (f == nullptr) {
        return {};
    }
    char buf[256] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string token(buf, n);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
        token.pop_back();
    }
    return token;
}

// Request headers for the GitHub REST API: the Accept type, the pinned API version and, when present, the
// token.
curl_slist* headers(const char* accept) {
    curl_slist* list = curl_slist_append(nullptr, accept);
    list = curl_slist_append(list, "X-GitHub-Api-Version: 2022-11-28");
    const std::string token = read_token();
    if (!token.empty()) {
        list = curl_slist_append(list, ("Authorization: Bearer " + token).c_str());
    }
    return list;
}

// A curl handle with the shared options: follow GitHub's redirect to the download host, treat HTTP errors
// as failures, and give up on a connection that stalls (no data for 30 s) instead of hanging forever.
CURL* make_handle(const char* url, curl_slist* list) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return nullptr;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SFA-NX-updater");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    return curl;
}

// curl write callbacks: collect the API reply in a string, or stream a download into a file.
size_t to_string(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

size_t to_file(char* data, size_t size, size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<FILE*>(user)) * size;
}

// curl progress callback: bytes of earlier files plus this file's bytes so far, as a percentage of all.
int on_progress(void*, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) {
    if (g_totalSize > 0) {
        g_percent.store(static_cast<int>((g_doneSize + now) * 100 / g_totalSize));
    }
    return 0;
}

// Reads "major.minor.patch" from a tag such as "v0.2.0" or "SFA-NX 0.2" (missing parts count as 0).
void parse_version(const std::string& text, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    const char* p = text.c_str();
    while (*p != '\0' && !std::isdigit(static_cast<unsigned char>(*p))) {
        ++p;
    }
    std::sscanf(p, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

// True when version `latest` is strictly greater than `current`.
bool newer(const std::string& latest, const std::string& current) {
    int a[3], b[3];
    parse_version(latest, a);
    parse_version(current, b);
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

// Worker thread for "check": fetches the latest release, keeps its SFA-NX.nro asset, and decides between
// up to date, available, or failed.
void check_worker() {
    if (!net_init()) {
        set_state(kFailed, "No network");
        g_busy.store(false);
        return;
    }
    std::string body;
    curl_slist* list = headers("Accept: application/vnd.github+json");
    CURL* curl = make_handle(kLatestUrl, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    curl_slist_free_all(list);
    if (rc != CURLE_OK) {
        set_state(kFailed, http == 404 ? "No release found" : std::string("Check failed: ") + curl_easy_strerror(rc));
        g_busy.store(false);
        return;
    }
    nlohmann::json release = nlohmann::json::parse(body, nullptr, false);
    if (!release.is_object() || !release.contains("tag_name")) {
        set_state(kFailed, "Bad reply from GitHub");
        g_busy.store(false);
        return;
    }
    const std::string tag = release.value("tag_name", "");
    std::vector<Asset> assets;
    bool hasNro = false;
    if (release.contains("assets") && release["assets"].is_array()) {
        for (const auto& item : release["assets"]) {
            if (!item.is_object()) {
                continue;
            }
            Asset asset;
            asset.name = item.value("name", "");
            if (strcasecmp(asset.name.c_str(), kAssetName) != 0) {
                continue;
            }
            asset.target = g_nroPath;
            hasNro = true;
            // The API URL of the asset (not browser_download_url): it works with a token on a private
            // repository when requested with Accept: application/octet-stream.
            asset.url = item.value("url", "");
            asset.size = item.value("size", static_cast<curl_off_t>(0));
            if (item.contains("digest") && item["digest"].is_string()) {
                asset.digest = item["digest"].get<std::string>();
            }
            assets.push_back(asset);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = tag;
        g_assets = assets;
    }
    if (!newer(tag, SFA_NX_VERSION)) {
        set_state(kUpToDate, "Up to date (v" SFA_NX_VERSION ")");
    } else if (!hasNro) {
        set_state(kFailed, tag + " has no " + kAssetName);
    } else {
        set_state(kAvailable, tag + " available");
    }
    g_busy.store(false);
}

// Checks a finished download before it may replace the running NRO: an NRO has the magic "NRO0" at offset
// 0x10, the size must match the release listing, and the SHA-256 must match GitHub's digest when one was
// given. On failure *why gets the reason shown in the menu.
bool verify(const std::string& path, curl_off_t size, const std::string& digest, bool nro, std::string* why) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        *why = "Download missing";
        return false;
    }
    Sha256Context sha;
    sha256ContextCreate(&sha);
    static unsigned char buf[64 * 1024];
    curl_off_t total = 0;
    char magic[4] = {};
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        if (total == 0 && n >= 0x14) {
            std::memcpy(magic, buf + 0x10, 4);
        }
        sha256ContextUpdate(&sha, buf, n);
        total += static_cast<curl_off_t>(n);
    }
    std::fclose(f);
    if (nro && std::memcmp(magic, "NRO0", 4) != 0) {
        *why = "Download is not an NRO";
        return false;
    }
    if (size > 0 && total != size) {
        *why = "Download incomplete";
        return false;
    }
    if (digest.rfind("sha256:", 0) == 0) {
        unsigned char hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&sha, hash);
        char hex[SHA256_HASH_SIZE * 2 + 1];
        for (int i = 0; i < SHA256_HASH_SIZE; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }
        if (strcasecmp(hex, digest.c_str() + 7) != 0) {
            *why = "Checksum mismatch";
            return false;
        }
    }
    return true;
}

// Downloads one asset to cache/update.nro and verifies it. A failed or bad download is deleted.
bool download_one(const Asset& asset, std::string* why) {
    const std::string part = kPartPath;
    FILE* f = std::fopen(part.c_str(), "wb");
    if (f == nullptr) {
        *why = "Cannot write to the SD card";
        return false;
    }
    curl_slist* list = headers("Accept: application/octet-stream");
    CURL* curl = make_handle(asset.url.c_str(), list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(list);
    const bool written = std::fclose(f) == 0;
    if (rc != CURLE_OK || !written) {
        std::remove(part.c_str());
        *why = std::string("Download failed: ") + (rc != CURLE_OK ? curl_easy_strerror(rc) : "SD write");
        return false;
    }
    if (!verify(part, asset.size, asset.digest, asset.target == g_nroPath, why)) {
        std::remove(part.c_str());
        return false;
    }
    return true;
}

// Worker thread for "download": copies the asset list under the lock, then downloads each asset.
void download_worker() {
    std::vector<Asset> assets;
    std::string tag;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        assets = g_assets;
        tag = g_latest;
    }
    g_percent.store(0);
    g_totalSize = 0;
    g_doneSize = 0;
    for (const Asset& asset : assets) {
        g_totalSize += asset.size;
    }
    std::string why;
    for (const Asset& asset : assets) {
        if (!download_one(asset, &why)) {
            std::remove(kPartPath);
            set_state(kFailed, why);
            g_busy.store(false);
            return;
        }
        g_doneSize += asset.size;
    }
    set_state(kReady, tag + " downloaded");
    g_busy.store(false);
}

// Swaps the verified download in: the running NRO becomes cache/previous.nro and the download takes its
// name. If the second rename fails the first is undone, so the user is never left without an NRO.
// RomFS is closed first because it keeps the running NRO open, and Horizon cannot rename an open file.
bool install() {
    const std::string part = kPartPath;
    const std::string old = kOldPath;
    romfsExit();
    std::remove(old.c_str());
    if (std::rename(g_nroPath.c_str(), old.c_str()) != 0) {
        return false;
    }
    if (std::rename(part.c_str(), g_nroPath.c_str()) != 0) {
        std::rename(old.c_str(), g_nroPath.c_str());
        return false;
    }
    return true;
}

} // namespace

// Called at startup, before anything opens files in the app folder. Deletes files older versions put next
// to the NRO (the shader seeds are now inside the NRO's RomFS, and earlier updaters staged downloads as
// .new/.old there), plus the previous NRO and any half-finished download from cache/.
extern "C" void sfaUpdaterApplyPending(void) {
    for (const char* name : {"initial_pipeline_cache.db", "initial_dksh_cache.bin", "initial_pipeline_cache.db.new",
                             "initial_dksh_cache.bin.new", "SFA-NX.nro.new", "SFA-NX.nro.old"}) {
        std::remove((std::string(kAppDir) + name).c_str());
    }
    std::remove(kPartPath);
    std::remove(kOldPath);
}

// Records where the running NRO lives (argv[0]); only full SD card paths are accepted.
extern "C" void sfaUpdaterSetNroPath(const char* path) {
    if (path != nullptr && std::strncmp(path, "sdmc:/", 6) == 0) {
        g_nroPath = path;
    }
}

// The version this build reports (shown in the menu).
extern "C" const char* sfaUpdaterVersion(void) { return SFA_NX_VERSION; }

// Status text for the menu row, including which action A performs next.
extern "C" void sfaUpdaterStatus(char* out, size_t size) {
    const int state = g_state.load();
    std::lock_guard<std::mutex> lock(g_mutex);
    switch (state) {
    case kIdle: std::snprintf(out, size, "v%s  A: check", SFA_NX_VERSION); break;
    case kChecking: std::snprintf(out, size, "Checking..."); break;
    case kAvailable: std::snprintf(out, size, "%s  A: download", g_message.c_str()); break;
    case kDownloading: std::snprintf(out, size, "Downloading %d%%", g_percent.load()); break;
    case kReady: std::snprintf(out, size, "%s  A: restart", g_message.c_str()); break;
    default: std::snprintf(out, size, "%s", g_message.c_str()); break;
    }
}

// The menu's A press: starts the next step for the current state (see the top of the file).
extern "C" void sfaUpdaterActivate(void) {
    if (g_busy.load()) {
        return;
    }
    const int state = g_state.load();
    if (state == kAvailable) {
        g_busy.store(true);
        set_state(kDownloading, "Downloading");
        std::thread(download_worker).detach();
    } else if (state == kReady) {
        if (!install()) {
            set_state(kFailed, "Could not replace SFA-NX.nro");
            return;
        }
        // Ask hbloader to start the new NRO when this one exits. Without that support (some loaders) the
        // update is still installed and takes effect the next time SFA-NX is started.
        if (envHasNextLoad()) {
            envSetNextLoad(g_nroPath.c_str(), g_nroPath.c_str());
            pc_log_line("[sfa-nx] updater: relaunching %s", g_nroPath.c_str());
        } else {
            pc_log_line("[sfa-nx] updater: installed; no relaunch available");
        }
        fhRequestQuit();
    } else {
        g_busy.store(true);
        set_state(kChecking, "Checking");
        std::thread(check_worker).detach();
    }
}
