// In-app updater, driven from the - menu's Updates row (switch_menu.cpp).
//
// Each press of A advances one step:
//   idle/up to date/failed -> check: ask the GitHub API for the latest release of digdat0/sfa-nx, and for
//                             the fixed "shaderdb" release (the shader database, shaderdb_switch.cpp)
//   available              -> download: fetch the release's SFA-NX.nro to cache/update.nro and/or a newer
//                             sfa-shaderdb.bin to cache/shaderdb/sfa-shaderdb.bin.new, and verify them
//   ready                  -> install: swap the new NRO in (if one came), ask the Homebrew loader to start
//                             it next, quit; the shader database is installed by the next start
//
// The shaderdb release holds sfa-shaderdb.bin and its manifest sfa-shaderdb.json (version, format, sha256;
// switch/builder/make-shaderdb.py). Only the small manifest is read to decide: its version must be newer
// than the installed (or already downloaded) database and its format one this build reads. The same NRO can
// so get newer shaders without an NRO release. sfaUpdaterOfferShaders runs the same steps for the shader
// database only (the first-start offer in switch_menu.cpp).
//
// Network work runs on a detached thread so the game keeps rendering; the menu polls sfaUpdaterStatus each
// frame. The release tag (e.g. "v0.2.0") is compared with SFA_NX_VERSION, which CMake sets from
// PROJECT_VERSION. While the repository is private a GitHub token can be put in config/github_token.
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <switch.h>

#include <algorithm>
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

// Shader database helpers (shaderdb_switch.cpp).
extern "C" unsigned sfaShaderDbVersion(void);
extern "C" unsigned sfaShaderDbFileVersion(const char* path);
extern "C" unsigned sfaShaderDbFormat(void);
extern "C" int sfaShaderDbCheckFile(const char* path, char* why, size_t whySize);

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
// The shader database's release (a fixed tag whose assets are replaced) and where its download goes: .part
// while downloading, renamed to .new once verified, installed at the next start (shaderdb_switch.cpp).
constexpr const char* kShaderDbUrl = "https://api.github.com/repos/digdat0/sfa-nx/releases/tags/shaderdb";
constexpr const char* kShaderDbAsset = "sfa-shaderdb.bin";
constexpr const char* kShaderDbManifest = "sfa-shaderdb.json";
constexpr const char* kShaderPart = "sdmc:/switch/SFA-NX/cache/shaderdb/sfa-shaderdb.bin.part";
constexpr const char* kShaderPending = "sdmc:/switch/SFA-NX/cache/shaderdb/sfa-shaderdb.bin.new";

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
// Set by sfaUpdaterOfferShaders: this check looks at the shader database only (no NRO update).
std::atomic<bool> g_shadersOnly{false};
// What the last download fetched: an NRO to swap in at install.
bool g_gotNro;

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

// GETs a GitHub API URL into *body. Returns the curl result; *http gets the status code.
CURLcode fetch(const std::string& url, const char* accept, std::string* body, long* http) {
    curl_slist* list = headers(accept);
    CURL* curl = make_handle(url.c_str(), list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    const CURLcode rc = curl_easy_perform(curl);
    *http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http);
    curl_easy_cleanup(curl);
    curl_slist_free_all(list);
    return rc;
}

// The asset called `name` in a release's JSON (empty url when it is missing).
Asset find_asset(const nlohmann::json& release, const char* name) {
    Asset asset;
    if (!release.contains("assets") || !release["assets"].is_array()) {
        return asset;
    }
    for (const auto& item : release["assets"]) {
        if (!item.is_object() || strcasecmp(item.value("name", "").c_str(), name) != 0) {
            continue;
        }
        asset.name = name;
        // The API URL of the asset (not browser_download_url): it works with a token on a private
        // repository when requested with Accept: application/octet-stream.
        asset.url = item.value("url", "");
        asset.size = item.value("size", static_cast<curl_off_t>(0));
        if (item.contains("digest") && item["digest"].is_string()) {
            asset.digest = item["digest"].get<std::string>();
        }
        break;
    }
    return asset;
}

// Result of one release check: 1 something newer (added to *assets), 0 nothing newer, -1 failed (*why).
// The NRO: the latest release's tag against SFA_NX_VERSION; *label gets the tag.
int check_nro(std::vector<Asset>* assets, std::string* label, std::string* why) {
    std::string body;
    long http = 0;
    const CURLcode rc = fetch(kLatestUrl, "Accept: application/vnd.github+json", &body, &http);
    if (rc != CURLE_OK) {
        *why = http == 404 ? "No release found" : std::string("Check failed: ") + curl_easy_strerror(rc);
        return -1;
    }
    nlohmann::json release = nlohmann::json::parse(body, nullptr, false);
    if (!release.is_object() || !release.contains("tag_name")) {
        *why = "Bad reply from GitHub";
        return -1;
    }
    const std::string tag = release.value("tag_name", "");
    if (!newer(tag, SFA_NX_VERSION)) {
        return 0;
    }
    Asset asset = find_asset(release, kAssetName);
    if (asset.url.empty()) {
        *why = tag + " has no " + kAssetName;
        return -1;
    }
    asset.target = g_nroPath;
    assets->push_back(asset);
    *label = tag;
    return 1;
}

// The shader database: the shaderdb release's manifest against the installed (or already downloaded)
// database. No shaderdb release at all counts as nothing newer. *label gets "Shaders <version>".
int check_shaders(std::vector<Asset>* assets, std::string* label, std::string* why) {
    std::string body;
    long http = 0;
    CURLcode rc = fetch(kShaderDbUrl, "Accept: application/vnd.github+json", &body, &http);
    if (rc != CURLE_OK) {
        if (http == 404) {
            return 0;
        }
        *why = std::string("Shader check failed: ") + curl_easy_strerror(rc);
        return -1;
    }
    nlohmann::json release = nlohmann::json::parse(body, nullptr, false);
    Asset bin = release.is_object() ? find_asset(release, kShaderDbAsset) : Asset{};
    const Asset manifestAsset = release.is_object() ? find_asset(release, kShaderDbManifest) : Asset{};
    if (bin.url.empty() || manifestAsset.url.empty()) {
        return 0;
    }
    std::string text;
    rc = fetch(manifestAsset.url, "Accept: application/octet-stream", &text, &http);
    nlohmann::json manifest = nlohmann::json::parse(text, nullptr, false);
    if (rc != CURLE_OK || !manifest.is_object()) {
        *why = "Bad shader manifest";
        return -1;
    }
    const unsigned version = manifest.value("version", 0u);
    const unsigned format = manifest.value("format", 0u);
    const unsigned have = std::max(sfaShaderDbVersion(), sfaShaderDbFileVersion(kShaderPending));
    if (format == 0 || format > sfaShaderDbFormat() || version <= have) {
        return 0;  // nothing newer, or made for a newer SFA-NX (that NRO update brings it)
    }
    if (bin.digest.empty() && manifest.contains("sha256") && manifest["sha256"].is_string()) {
        bin.digest = "sha256:" + manifest["sha256"].get<std::string>();
    }
    bin.target = kShaderPending;
    assets->push_back(bin);
    *label = "Shaders " + std::to_string(version);
    return 1;
}

// Worker thread for "check": the NRO release (unless only shaders were asked for) and the shader database;
// decides between up to date, available (with a label such as "v1.0.2 + shaders") or failed.
void check_worker() {
    if (!net_init()) {
        set_state(kFailed, "No network");
        g_busy.store(false);
        return;
    }
    std::vector<Asset> assets;
    std::string nroLabel, nroWhy, shaderLabel, shaderWhy;
    const int nro = g_shadersOnly.load() ? 0 : check_nro(&assets, &nroLabel, &nroWhy);
    const int shaders = check_shaders(&assets, &shaderLabel, &shaderWhy);
    std::string label = nro > 0 && shaders > 0 ? nroLabel + " + shaders" : nro > 0 ? nroLabel : shaderLabel;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = label;
        g_assets = assets;
    }
    if (!assets.empty()) {
        set_state(kAvailable, label + " available");
    } else if (nro < 0 || shaders < 0) {
        set_state(kFailed, nro < 0 ? nroWhy : shaderWhy);
    } else {
        set_state(kUpToDate, g_shadersOnly.load() ? "No newer shaders" : "Up to date (v" SFA_NX_VERSION ")");
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

// Downloads one asset and verifies it: the NRO to cache/update.nro (swapped in by install), the shader
// database to its .part file, renamed to .new once it checks out. A failed or bad download is deleted.
bool download_one(const Asset& asset, std::string* why) {
    const bool nro = asset.target == g_nroPath;
    const std::string part = nro ? kPartPath : kShaderPart;
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
    if (!verify(part, asset.size, asset.digest, nro, why)) {
        std::remove(part.c_str());
        return false;
    }
    if (!nro) {
        char reason[96];
        if (sfaShaderDbCheckFile(part.c_str(), reason, sizeof(reason)) == 0) {
            std::remove(part.c_str());
            *why = reason;
            return false;
        }
        // Horizon's rename does not overwrite: drop an older download first.
        std::remove(kShaderPending);
        if (std::rename(part.c_str(), kShaderPending) != 0) {
            std::remove(part.c_str());
            *why = "Cannot write to the SD card";
            return false;
        }
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
    g_gotNro = false;
    for (const Asset& asset : assets) {
        if (!download_one(asset, &why)) {
            std::remove(kPartPath);
            g_gotNro = false;
            set_state(kFailed, why);
            g_busy.store(false);
            return;
        }
        g_doneSize += asset.size;
        g_gotNro = g_gotNro || asset.target == g_nroPath;
    }
    set_state(kReady, tag + " downloaded");
    g_busy.store(false);
}

// Swaps the verified download in: the running NRO becomes cache/previous.nro and the download takes its
// name. If the second rename fails the first is undone, so the user is never left without an NRO.
// (The NRO has no RomFS mounted, which would keep the running NRO open: Horizon cannot rename an open file.)
bool install() {
    const std::string part = kPartPath;
    const std::string old = kOldPath;
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
    std::remove(kShaderPart);
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

// The state number (State above), download percentage and release tag, for the menu to word the Updates row
// in the game's language itself.
extern "C" int sfaUpdaterState(int* percent, char* tag, size_t tagSize) {
    const int state = g_state.load();
    *percent = g_percent.load();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::snprintf(tag, tagSize, "%s", g_latest.c_str());
    return state;
}

// The menu's Restart row: quits and asks hbloader to start this NRO again, so settings read at start take
// effect. Loaders without next-load support just quit; the settings apply the next time SFA-NX is started.
// Set by sfaRelaunch; sfaRelaunchPending tells vi_shim.c's shutdown_and_exit to return to hbloader instead of
// ending the process (ending it shows "the software was closed" under title takeover and starts nothing).
static bool g_relaunch = false;
extern "C" int sfaRelaunchPending(void) { return g_relaunch ? 1 : 0; }

// Shared by the Restart row and the updater: asks hbloader to start this NRO again and sets g_relaunch, so
// shutdown_and_exit takes the clean path back to hbloader. Returns false on loaders without next-load support.
static bool request_relaunch() {
    if (!envHasNextLoad()) {
        return false;
    }
    envSetNextLoad(g_nroPath.c_str(), g_nroPath.c_str());
    g_relaunch = true;
    return true;
}

extern "C" void sfaRelaunch(void) {
    if (request_relaunch()) {
        pc_log_line("[sfa-nx] restart: relaunching %s", g_nroPath.c_str());
    } else {
        pc_log_line("[sfa-nx] restart: no relaunch available, quitting");
    }
    fhRequestQuit();
}

// Restart path (vi_shim.c's shutdown_and_exit): closes curl and the socket service if the updater opened them.
// Under hbloader the process outlives this NRO, so an open bsd session and its transfer memory (heap pages
// locked to the service) would carry over into the relaunched NRO.
extern "C" void sfaUpdaterReleaseNet(void) {
    if (!g_netReady) {
        return;
    }
    curl_global_cleanup();
    socketExit();
    g_netReady = false;
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
        if (g_gotNro && !install()) {
            set_state(kFailed, "Could not replace SFA-NX.nro");
            return;
        }
        // Ask hbloader to start the new NRO when this one exits. Without that support (some loaders) the
        // update is still installed and takes effect the next time SFA-NX is started.
        // request_relaunch also sets g_relaunch: without it shutdown_and_exit ended the whole process
        // (title takeover showed "the software was closed") instead of returning to hbloader (v1.0.0 bug).
        if (request_relaunch()) {
            pc_log_line("[sfa-nx] updater: relaunching %s", g_nroPath.c_str());
        } else {
            pc_log_line("[sfa-nx] updater: installed; no relaunch available");
        }
        fhRequestQuit();
    } else {
        g_shadersOnly.store(false);
        g_busy.store(true);
        set_state(kChecking, "Checking");
        std::thread(check_worker).detach();
    }
}

// The first-start offer's "Download": checks the shader database only. The offer then calls
// sfaUpdaterActivate for the download and the restart, as the Updates row does.
extern "C" void sfaUpdaterOfferShaders(void) {
    if (g_busy.load()) {
        return;
    }
    g_shadersOnly.store(true);
    g_busy.store(true);
    set_state(kChecking, "Checking");
    std::thread(check_worker).detach();
}
