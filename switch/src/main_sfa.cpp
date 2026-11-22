// Switch startup for Foxhollow (Star Fox Adventures): SD folders, logs, disc lookup, then the
// desktop entry point (src/main.c, whose main is renamed aurora_main by aurora/main.h).
//
// SD layout (sdmc:/switch/SFA-NX/):
//   SFA-NX.nro            this program (the shader and pipeline seeds are packed in its RomFS)
//   <any>.iso/.gcm/.rvz   the user's own Star Fox Adventures disc image (GSAE01/GSAJ01/GSAP01)
//   logs/                 everything needed for debugging, in one folder:
//     boot.txt            boot markers, committed one by one (survives a crash before logs open)
//     log.txt             runtime log of the last 10 runs, each starting with a "===== SFA-NX run" line
//     profile-NNN.txt     profiler reports (Debug tab), the newest 10 kept
//     deko-fail-*         shaders the GPU backend could not compile
//     crash_reports/      copies of the newest Atmosphere crash reports (copied at the next start)
//   config/               port settings: switch_menu.ini (- menu), perf.cfg (tuning knobs), uber.tripped
//   user/                 Foxhollow's saves and settings
//   cache/                shader and pipeline caches, SQLite temp files, the updater's download
//
// Startup order: create folders, check the launch mode, finish or clean up an update, mount RomFS, find the
// disc, open the log, then hand over to Foxhollow's own main (aurora_main) with the disc path as argv[1].

#include "log_switch.h"
#include "perf_switch.h"

#include <switch.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" int aurora_main(int argc, char** argv); // src/main.c (aurora/main.h renames its main)
extern "C" void melee_nx_ensure_controller(void); // input_switch.cpp
extern "C" void sfaUpdaterSetNroPath(const char* path);
extern "C" void sfaUpdaterApplyPending(void);
extern "C" int sfaSwitchMenuIniValue(const char* key, int def);  // switch_menu.cpp
// True when the NRO's RomFS mounted; the shader seeds are then read from romfs: instead of the SD card.
static bool g_romfs = false;
// True when the disc is the PAL one (GSAP01), the only one with French, German, Italian and Spanish text.
static bool g_disc_pal = false;
// For the - menu's Language line.
extern "C" int sfaDiscIsPal(void) { return g_disc_pal ? 1 : 0; }

#define SFA_DIR "sdmc:/switch/SFA-NX"
#define LOG_DIR SFA_DIR "/logs"
#define CONFIG_DIR SFA_DIR "/config"

namespace {
// Compile time of this file; build.sh touches main_sfa.cpp before every build, so it identifies the build.
constexpr const char* kBuildStamp = __DATE__ " " __TIME__;
}
// The profiler's report header (prof_switch.cpp); build.sh touches this file, so it names the build.
extern "C" const char sfa_nx_build_stamp[] = __DATE__ " " __TIME__;

// One open/write/close per marker so a crash leaves every earlier line on the card.
// (Named melee_boot_trace because the shared Switch layer, clang_tls_switch.c, calls it.)
extern "C" void melee_boot_trace(const char* fmt, ...);

// Logs how many sessions, events, threads and transfer memories the process holds against its limits.
// After the menu's Restart, hbloader's process carries over whatever the previous NRO left open, so
// numbers that grow from one relaunch to the next point at a leak (one leaked apm session per run made
// the second relaunch abort in applet init, 2345-0031).
static void trace_resource_limits() {
    Handle limit = INVALID_HANDLE;
    u64 info = 0;
    if (R_FAILED(svcGetInfo(&info, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
        return;
    }
    limit = static_cast<Handle>(info);
    if (limit == INVALID_HANDLE) {
        return;
    }
    // Current and maximum value of one resource; -1 when the kernel refuses the query.
    auto value = [limit](LimitableResource r, bool max) -> long long {
        s64 v = -1;
        Result rc = max ? svcGetResourceLimitLimitValue(&v, limit, r) : svcGetResourceLimitCurrentValue(&v, limit, r);
        return R_SUCCEEDED(rc) ? static_cast<long long>(v) : -1;
    };
    melee_boot_trace("   limits sessions %lld/%lld events %lld/%lld threads %lld/%lld tmem %lld/%lld",
                     value(LimitableResource_Sessions, false), value(LimitableResource_Sessions, true),
                     value(LimitableResource_Events, false), value(LimitableResource_Events, true),
                     value(LimitableResource_Threads, false), value(LimitableResource_Threads, true),
                     value(LimitableResource_TransferMemories, false),
                     value(LimitableResource_TransferMemories, true));
    svcCloseHandle(limit);  // svcGetInfo handed us a new handle; closing it keeps this probe leak-free
}

extern "C" void melee_boot_trace(const char* fmt, ...) {
    static bool truncated = false;
    const int fd = open(LOG_DIR "/boot.txt", O_WRONLY | O_CREAT | (truncated ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) {
        return;
    }
    truncated = true;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = static_cast<size_t>(n) < sizeof(buf) - 1 ? static_cast<size_t>(n) : sizeof(buf) - 2;
        buf[len] = '\n';
        (void)write(fd, buf, len + 1);
    }
    fsync(fd);
    close(fd);
}

namespace {

// Disc image kinds the port can read: plain GameCube images (.iso/.gcm) and Dolphin's compressed RVZ
// (decoded on the fly by switch/src/nod/rvz.c). .ciso/.wbfs/.wia must be converted with Dolphin.
enum class DiscKind { None, Plain, Rvz };

DiscKind disc_kind(const char* name) {
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) {
        return DiscKind::None;
    }
    if (strcasecmp(dot, ".iso") == 0 || strcasecmp(dot, ".gcm") == 0) {
        return DiscKind::Plain;
    }
    return strcasecmp(dot, ".rvz") == 0 ? DiscKind::Rvz : DiscKind::None;
}

// Reads a disc image's 6-byte game ID into id (not NUL-terminated); false when the file can't be read or an
// .rvz lacks its magic. A GameCube disc header starts with the game ID: "GSA" + region letter + "01" for
// this game. In an RVZ file the disc header is copied at offset 0x58 (after the 0x48-byte file head and
// 0x10 bytes of the disc struct), behind "RVZ\x01". Used both to pick the image and to spot the PAL disc.
bool read_game_id(const std::string& path, DiscKind kind, char id[6]) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char head[0x58 + 6] = {};
    const size_t want = kind == DiscKind::Rvz ? sizeof(head) : 6;
    const bool read_ok = std::fread(head, 1, want, f) == want;
    std::fclose(f);
    const bool magic_ok = kind != DiscKind::Rvz || std::memcmp(head, "RVZ\x01", 4) == 0;
    std::memcpy(id, kind == DiscKind::Rvz ? head + 0x58 : head, 6);
    return read_ok && magic_ok;
}

// The Star Fox Adventures image next to the NRO (or in SFA_DIR): the first file whose game ID is GSA?01.
// An .iso/.gcm wins over an .rvz when both are there, because it needs no decompression.
std::string find_disc(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return {};
    }
    std::string found;
    std::string found_rvz;
    while (dirent* e = readdir(d)) {
        const DiscKind kind = disc_kind(e->d_name);
        if (kind == DiscKind::None) {
            continue;
        }
        std::string path = dir + "/" + e->d_name;
        char id[6] = {};
        if (read_game_id(path, kind, id) && std::memcmp(id, "GSA", 3) == 0 && std::memcmp(id + 4, "01", 2) == 0) {
            if (kind == DiscKind::Plain) {
                found = path; // an ISO is the best case: stop looking
                break;
            }
            if (found_rvz.empty()) {
                found_rvz = path; // keep the first RVZ, but keep scanning for an ISO
            }
            continue;
        }
        melee_boot_trace("   skip %s (id %.6s)", path.c_str(), id);
    }
    closedir(d);
    return found.empty() ? found_rvz : found;
}

// Folder the NRO was started from (argv[0] is its full sdmc: path); falls back to the default folder when
// the loader passed no path.
std::string nro_directory(int argc, char** argv) {
    if (argc > 0 && argv[0] != nullptr) {
        std::string p = argv[0];
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) {
            return p.substr(0, slash);
        }
    }
    return SFA_DIR;
}

// One numeric `key value` line of config/perf.cfg, read before perf_switch.cpp parses the file (its parse
// logs, so it runs after stdout is redirected).
int early_cfg(const char* want, int fallback) {
    FILE* f = std::fopen(CONFIG_DIR "/perf.cfg", "r");
    if (f == nullptr) {
        return fallback;
    }
    char line[128], key[32];
    int value = fallback, v;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, " %31s %i", key, &v) == 2 && std::strcmp(key, want) == 0) {
            value = v;
            break;
        }
    }
    std::fclose(f);
    return value;
}

// ---- logs/log.txt: one file holding the last kKeepRuns runs ----------------------------------------------
// Each run appends to the same file, starting with a kRunMarker line. At start, before this run's header,
// the oldest runs are cut so at most kKeepRuns - 1 earlier runs stay, and the file stays under kMaxLogBytes
// (a profiler session can write several MiB; normal play writes far less). Cutting copies the kept tail to
// log.tmp and renames it over log.txt, so a crash mid-cut leaves either the old file or the new one.
constexpr const char* kRunLog = LOG_DIR "/log.txt";
constexpr const char* kRunLogTmp = LOG_DIR "/log.tmp";
constexpr const char kRunMarker[] = "===== SFA-NX run ";
constexpr int kKeepRuns = 10;
constexpr long kMaxLogBytes = 16L << 20;

// Settles a cut interrupted by a crash: log.tmp alone is the finished copy (log.txt was already removed);
// next to log.txt it is an unfinished one.
void recover_run_log() {
    struct stat st;
    if (stat(kRunLogTmp, &st) != 0) {
        return;
    }
    if (stat(kRunLog, &st) != 0) {
        rename(kRunLogTmp, kRunLog);
    } else {
        unlink(kRunLogTmp);
    }
}

// Drops the oldest runs from log.txt (see above). Any failure leaves the file as it was.
void trim_run_log() {
    recover_run_log();
    FILE* in = std::fopen(kRunLog, "rb");
    if (in == nullptr) {
        return;
    }
    static char inBuf[64 * 1024];
    std::setvbuf(in, inBuf, _IOFBF, sizeof(inBuf));
    // Offsets of every run header; a marker counts only at the start of a line.
    std::vector<long> runs;
    char line[1024];
    bool lineStart = true;
    long at = 0;
    while (std::fgets(line, sizeof(line), in) != nullptr) {
        const size_t n = std::strlen(line);
        if (lineStart && std::strncmp(line, kRunMarker, sizeof(kRunMarker) - 1) == 0) {
            runs.push_back(at);
        }
        at += long(n);
        lineStart = n > 0 && line[n - 1] == '\n';
    }
    const long total = at;
    // Keep the newest kKeepRuns - 1 runs (this run is the next one), then drop whole runs while over the
    // size cap. If the newest run alone is too big, keep its last kMaxLogBytes from a line start.
    size_t first = runs.size() >= size_t(kKeepRuns) ? runs.size() - (kKeepRuns - 1) : 0;
    long keep = runs.empty() ? 0 : runs[first];
    while (total - keep > kMaxLogBytes && first + 1 < runs.size()) {
        keep = runs[++first];
    }
    bool partial = false;
    if (total - keep > kMaxLogBytes) {
        keep = total - kMaxLogBytes;
        partial = true;
    }
    if (keep <= 0) {
        std::fclose(in);
        return;
    }
    FILE* out = std::fopen(kRunLogTmp, "wb");
    bool ok = out != nullptr && std::fseek(in, keep, SEEK_SET) == 0;
    if (ok && partial) {
        // resume at the next line start and say that the run's beginning was cut
        int c;
        while ((c = std::fgetc(in)) != EOF && c != '\n') {
        }
        std::fputs("[log] the beginning of this run was cut to keep log.txt under 16 MiB\n", out);
    }
    static char copyBuf[64 * 1024];
    size_t n;
    while (ok && (n = std::fread(copyBuf, 1, sizeof(copyBuf), in)) > 0) {
        ok = std::fwrite(copyBuf, 1, n, out) == n;
    }
    std::fclose(in);
    if (out != nullptr) {
        ok = std::fclose(out) == 0 && ok;
    }
    // FAT cannot rename over an existing file: remove log.txt first (recover_run_log covers a crash here).
    if (ok && unlink(kRunLog) == 0) {
        rename(kRunLogTmp, kRunLog);
    } else {
        unlink(kRunLogTmp);
    }
}

// Older builds wrote one logs/stdout-NNN.log per run; log.txt replaces them, so they are removed.
void remove_numbered_logs() {
    std::vector<std::string> old;
    if (DIR* d = opendir(LOG_DIR)) {
        while (dirent* e = readdir(d)) {
            int n = 0;
            char tail[8] = {};
            if (std::sscanf(e->d_name, "stdout-%d.%7s", &n, tail) == 2 && std::strcmp(tail, "log") == 0) {
                old.emplace_back(std::string(LOG_DIR "/") + e->d_name);
            }
        }
        closedir(d);
    }
    for (const std::string& path : old) {
        unlink(path.c_str());
    }
}

// Keeps only the newest kKeepRuns profiler reports (logs/profile-NNN.txt, prof_switch.cpp numbers them
// upwards), so a tester's card does not fill with them.
void prune_profiles() {
    std::vector<std::pair<int, std::string>> files;
    if (DIR* d = opendir(LOG_DIR)) {
        while (dirent* e = readdir(d)) {
            int n = 0;
            char tail[8] = {};
            if (std::sscanf(e->d_name, "profile-%d.%7s", &n, tail) == 2 && std::strcmp(tail, "txt") == 0) {
                files.emplace_back(n, std::string(LOG_DIR "/") + e->d_name);
            }
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + kKeepRuns < files.size(); ++i) {
        unlink(files[i].second.c_str());
    }
}

// Trims logs/log.txt, appends this run's header to it and sends stdout/stderr there.
bool redirect_stdio() {
    std::fflush(stdout);
    std::fflush(stderr);
    remove_numbered_logs();
    prune_profiles();
    trim_run_log();
    const int fd = open(kRunLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }
    lseek(fd, 0, SEEK_END);  // O_APPEND already writes at the end; this makes sure the first write is too
    {
        // The header starts on a new line even when the previous run died mid-line.
        // The console's local time (newlib's localtime has no time zone here and would give UTC).
        char header[160];
        char when[40];
        const time_t now = time(nullptr);
        TimeCalendarTime local{};
        TimeCalendarAdditionalInfo zone{};
        if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(u64(now), &local, &zone))) {
            std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", local.year, local.month,
                          local.day, local.hour, local.minute, local.second);
        } else {
            struct tm utc{};
            gmtime_r(&now, &utc);
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &utc);
        }
        const int n = std::snprintf(header, sizeof(header), "\n%s%s  build %s =====\n", kRunMarker, when, kBuildStamp);
        if (n > 0 && write(fd, header, size_t(n)) != n) {
            close(fd);
            return false;
        }
    }
    // Unbuffered: lines reach the writer (or the card) immediately, so nothing is lost if the game crashes.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // log_capture 1: printf lands in the log writer's RAM buffer and its thread does the SD writes, instead
    // of the game thread writing each line to the card (~1 ms/frame in the 2026-11-17 profiles). fd stays
    // open when on.
    const int capture = early_cfg("log_capture", 1);
    if (capture == 0 || !melee_nx_log_capture_stdio(fd)) {
        if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            close(fd);
            return false;
        }
        close(fd);
    }
    std::printf("[sfa-nx] build %s  log_capture %d\n", kBuildStamp, capture);
    return true;
}

// Atmosphere writes crash reports to sdmc:/atmosphere/crash_reports/<timestamp>_<program id>.log (CPU
// crashes) and fatal_reports/ (aborts and GPU timeouts such as 2348-0006). The newest kKeep of each are
// copied into logs/ so the logs folder holds everything needed for a bug report. File names start with a
// timestamp, so sorting them by name sorts them by time. Reports already copied are skipped.
void copy_reports(const char* kSrc, const char* kDst) {
    constexpr int kKeep = 3;
    std::vector<std::string> names;
    if (DIR* d = opendir(kSrc)) {
        while (dirent* e = readdir(d)) {
            const char* dot = std::strrchr(e->d_name, '.');
            if (dot != nullptr && strcasecmp(dot, ".log") == 0) {
                names.emplace_back(e->d_name);
            }
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    if (names.size() > kKeep) {
        names.erase(names.begin(), names.end() - kKeep);
    }
    if (names.empty()) {
        return;
    }
    mkdir(kDst, 0777);
    for (const std::string& name : names) {
        const std::string dst = std::string(kDst) + "/" + name;
        struct stat st;
        if (stat(dst.c_str(), &st) == 0) {
            continue;
        }
        FILE* in = std::fopen((std::string(kSrc) + "/" + name).c_str(), "rb");
        FILE* out = in != nullptr ? std::fopen(dst.c_str(), "wb") : nullptr;
        char buf[16384];
        size_t n;
        while (out != nullptr && (n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
            std::fwrite(buf, 1, n, out);
        }
        if (out != nullptr) {
            std::fclose(out);
        }
        if (in != nullptr) {
            std::fclose(in);
        }
    }
}

// Copies the newest crash and fatal reports into logs/ (see copy_reports).
void copy_crash_reports() {
    copy_reports("sdmc:/atmosphere/crash_reports", LOG_DIR "/crash_reports");
    copy_reports("sdmc:/atmosphere/fatal_reports", LOG_DIR "/fatal_reports");
}

// Text-console message for problems found before the game takes the screen (wrong launch mode, no disc,
// no log file). Shows the message and the build stamp, waits for + and returns the exit code for main.
int fail_screen(const char* fmt, ...) {
    consoleInit(nullptr);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("\n  SFA-NX (Star Fox Adventures)  build %s\n\n%s\n\n  Press + to exit.\n", kBuildStamp, buf);
    melee_boot_trace("FAIL %s", buf);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return 1;
}

} // namespace

extern "C" int main(int argc, char** argv) {
    mkdir(SFA_DIR, 0777);
    mkdir(LOG_DIR, 0777);
    // logs used to sit next to the .nro: drop those so nobody sends stale ones
    unlink(SFA_DIR "/boot.txt");
    unlink(SFA_DIR "/stdout.log");
    unlink(SFA_DIR "/stdout.prev.log");
    // and in logs/ from builds before the numbered logs (logs/log.txt replaces both)
    unlink(LOG_DIR "/stdout.log");
    unlink(LOG_DIR "/stdout.prev.log");
    // All port settings live in config/; move files from older builds (rename keeps an existing new copy).
    mkdir(CONFIG_DIR, 0777);
    for (const char* name : {"switch_menu.ini", "perf.cfg", "uber.tripped"}) {
        const std::string from = std::string(SFA_DIR "/") + name;
        const std::string to = std::string(CONFIG_DIR "/") + name;
        struct stat st;
        if (stat(from.c_str(), &st) == 0 && stat(to.c_str(), &st) != 0) {
            rename(from.c_str(), to.c_str());
        }
    }
    melee_boot_trace("01 main  build %s  argv0=%s", kBuildStamp, argc > 0 && argv[0] ? argv[0] : "<null>");
    trace_resource_limits();

    // Applet mode (started from the Album) gets far less memory than title takeover (a full application),
    // not enough for the game, so refuse to start there with an explanation instead of crashing later.
    const AppletType applet = appletGetAppletType();
    if (applet != AppletType_Application && applet != AppletType_SystemApplication) {
        return fail_screen("  Started in applet mode (album), which has too little memory.\n"
                           "  Hold R while starting a game from the HOME menu, then open SFA-NX\n"
                           "  from the Homebrew Menu (title takeover).");
    }

    // The updater needs the NRO's own path to replace it. ApplyPending removes leftovers from earlier
    // versions and from a finished update before anything else opens files in the app folder.
    sfaUpdaterSetNroPath(argc > 0 ? argv[0] : nullptr);
    sfaUpdaterApplyPending();
    // RomFS holds the shipped shader and pipeline seeds (switch/seed, packed by CMake). A missing RomFS is
    // not fatal: the seeds then come from the SD folder, and without them the first start is just slower.
    g_romfs = R_SUCCEEDED(romfsInit());
    melee_boot_trace("02 romfs %s", g_romfs ? "ok" : "missing");
    // Look next to the NRO first, then in the default folder.
    std::string disc = find_disc(nro_directory(argc, argv));
    if (disc.empty()) {
        disc = find_disc(SFA_DIR);
    }
    melee_boot_trace("02 disc  %s", disc.empty() ? "<none>" : disc.c_str());
    if (disc.empty()) {
        return fail_screen("  No Star Fox Adventures disc image found.\n"
                           "  Put your own .iso or .rvz (GSAE01, GSAJ01 or GSAP01) in\n"
                           "  " SFA_DIR "/");
    }

    if (!redirect_stdio()) {
        return fail_screen("  Could not open a log file in " LOG_DIR " for writing.");
    }
    melee_boot_trace("03 logs  ok");
    // The - menu's Extra logging (log_switch.h), from switch_menu.ini before anything that checks it runs.
    // sfa_log_set_extra logs only a change (here: "on"), so the default is stated here.
    sfa_log_set_extra(sfaSwitchMenuIniValue("extra_logging", 0));
    if (!sfa_log_extra()) {
        std::printf("[sfa-nx] extra logging off\n");
    }
    // Foxhollow reads these to find its folders; setenv's last argument 0 keeps a value set elsewhere.
    copy_crash_reports();

    mkdir(SFA_DIR "/user", 0777);
    mkdir(SFA_DIR "/cache", 0777);
    // Empty mods/hd so players see where the HD texture pack goes (hd_textures_switch.cpp reads it).
    mkdir(SFA_DIR "/mods", 0777);
    mkdir(SFA_DIR "/mods/hd", 0777);
    setenv("FOXHOLLOW_USER_DIR", SFA_DIR "/user", 0);
    setenv("FOXHOLLOW_CACHE_DIR", SFA_DIR "/cache", 0);
    // Aurora's resources path, where it looks for the shipped pipeline seed initial_pipeline_cache.db. With
    // RomFS mounted that is romfs: (the file is read in place, never copied to the SD card); otherwise the
    // app folder, so a seed copied there by hand still works.
    setenv("FOXHOLLOW_SHADER_CACHE", g_romfs ? "romfs:" : SFA_DIR, 0);
    setenv("FOXHOLLOW_DISC", disc.c_str(), 1);
    // Game text language. Foxhollow runs the USA code, which has no language menu and reads the language
    // only from FOXHOLLOW_LANGUAGE (port/src/foxhollow_config.c), so the PAL disc's other languages need
    // the - menu's Language setting (switch_menu.ini language=, the game's numbering) passed on here.
    // The ID comes from read_game_id, which also looks inside .rvz files (their first bytes are the RVZ
    // header, not the disc: reading those made an RVZ of the PAL disc look like another region).
    {
        char id[6] = {};
        g_disc_pal = read_game_id(disc, disc_kind(disc.c_str()), id) && std::memcmp(id, "GSAP01", 6) == 0;
        melee_boot_trace("   disc id %.6s%s", id, g_disc_pal ? " (PAL)" : "");
    }
    if (g_disc_pal) {
        static const char* const kCodes[6] = {"en", "fr", "de", "it", nullptr, "es"};
        const int lang = sfaSwitchMenuIniValue("language", 0);
        if (lang >= 0 && lang < 6 && kCodes[lang] != nullptr) {
            setenv("FOXHOLLOW_LANGUAGE", kCodes[lang], 0);
            melee_boot_trace("   language %s", kCodes[lang]);
        }
    }
    // Test runs (Ryujinx) only: switch_menu.ini screen_style=1 starts in the game's 16:9 mode, 2 in 4:3, without
    // its Options menu (there is no one to press buttons). Read, never written; forcing it hides the game's own
    // Widescreen option (port/src/foxhollow_config.c FOXHOLLOW_SCREEN_STYLE).
    {
        const int style = sfaSwitchMenuIniValue("screen_style", 0);
        if (style == 1 || style == 2) {
            setenv("FOXHOLLOW_SCREEN_STYLE", style == 1 ? "wide" : "narrow", 0);
            melee_boot_trace("   screen style %s (test)", style == 1 ? "wide" : "narrow");
        }
    }
    // 1024-frame SDL audio buffers leave more slack before the sound underruns when a frame runs long.
    setenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "1024", 0);

    // Memory available to the process, logged to spot a wrong launch mode or a memory leak between versions.
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    std::printf("[sfa-nx] memory %llu MiB total, %llu MiB used; disc %s\n",
                (unsigned long long)(total >> 20), (unsigned long long)(used >> 20), disc.c_str());

    // Log writer thread, perf.cfg settings and the controller check (the melee_nx_ names come from the
    // Melee-NX Switch layer these files were taken from).
    melee_nx_log_init();
    melee_nx_perf_init();
    melee_nx_ensure_controller();

    // Foxhollow's command line is "foxhollow <disc image>".
    char prog[] = "foxhollow";
    std::string discArg = disc;
    char* fhArgv[] = {prog, discArg.data(), nullptr};
    melee_boot_trace("04 aurora_main");
    const int rc = aurora_main(2, fhArgv);
    melee_boot_trace("05 aurora_main returned %d", rc);
    return rc;
}
