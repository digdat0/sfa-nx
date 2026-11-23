// The shader database, sfa-shaderdb.bin (written by switch/builder/make-shaderdb.py, which documents the
// file layout). It holds the shaders and pipeline recipes we collected (consoles, Ryujinx save tours) and
// is shipped beside the NRO and from the fixed "shaderdb" GitHub release, not inside the NRO, so it can be
// updated without a new NRO.
//
// Files (all under sdmc:/switch/SFA-NX/):
//   sfa-shaderdb.bin                    a copy the player put next to the NRO (first install, or by hand)
//   cache/shaderdb/sfa-shaderdb.bin     the installed database (ours, replaced whole by updates)
//   cache/shaderdb/sfa-shaderdb.bin.new a download from the updater, installed at the next start
//   cache/shaderdb/initial_dksh_cache.bin
//                                       its shaders, unpacked once per version (extracted: the stamp)
//   cache/deko/dksh_local.bin, cache/pipeline_cache_deko.db
//                                       the player's own shaders: never touched here
//
// At start (sfaShaderDbPrepare, main_sfa.cpp, before the renderer): a newer .new or app-folder copy is
// moved in, its shaders are unpacked if the stamp differs, and the renderer is pointed at them
// (SFA_SHADER_SEED for switch/deko/shader_cache.cpp). Our compiled shaders are loaded beside the player's
// (a shader in both loads once). Our recipes stay packed: Aurora's pipeline cache asks for them
// (sfaShaderDbRecipes) and reads them from memory, beside the player's database, without writing them
// to it. Writing them (39 MB of SQLite rows on the SD card) took ~32 s at the first start and ~6 s at
// every start after it (2026-11-22 hardware logs). With the − menu's "Shader Pack" off
// (switch_menu.ini shader_pack=0) only the player's own shaders and recipes are used.
#include <switch.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>

extern "C" void melee_boot_trace(const char* fmt, ...);  // main_sfa.cpp: boot.txt + log
extern "C" int sfaSwitchMenuIniValue(const char* key, int def);  // switch_menu.cpp

namespace {

constexpr const char* kDir = "sdmc:/switch/SFA-NX/cache/shaderdb";
constexpr const char* kInstalled = "sdmc:/switch/SFA-NX/cache/shaderdb/sfa-shaderdb.bin";
constexpr const char* kPending = "sdmc:/switch/SFA-NX/cache/shaderdb/sfa-shaderdb.bin.new";
constexpr const char* kAppCopy = "sdmc:/switch/SFA-NX/sfa-shaderdb.bin";
constexpr const char* kStamp = "sdmc:/switch/SFA-NX/cache/shaderdb/extracted";
constexpr const char* kDksh = "sdmc:/switch/SFA-NX/cache/shaderdb/initial_dksh_cache.bin";
// The recipes as earlier builds unpacked them: deleted when found (39 MB nothing reads any more).
constexpr const char* kPipe = "sdmc:/switch/SFA-NX/cache/shaderdb/initial_pipeline_cache.db";
// Where Aurora looks for its pipeline seed while the pack is off: a folder that never holds one.
constexpr const char* kOffDir = "sdmc:/switch/SFA-NX/cache/shaderdb/off";

// The newest file format this build reads (make-shaderdb.py FORMAT).
constexpr uint32_t kFormat = 1;
constexpr uint32_t kCodecStored = 0, kCodecZlib = 1;

// On-disk header and section table entry (make-shaderdb.py HEADER / SECTION, little endian; the Switch is
// little endian too, so they are read in place).
struct Header {
    char magic[8];
    uint32_t format;
    uint32_t headerSize;
    uint32_t version;
    uint32_t sectionCount;
    uint64_t compilerId;
    uint32_t shaders;
    uint32_t recipes;
    uint32_t pipelineSchema;
    uint32_t headerCrc;
    uint8_t reserved[16];
};
struct Section {
    char tag[4];
    uint32_t codec;
    uint64_t offset;
    uint64_t packedSize;
    uint64_t size;
    uint8_t sha256[32];
};
static_assert(sizeof(Header) == 64 && sizeof(Section) == 64, "layout must match make-shaderdb.py");
constexpr uint32_t kMaxSections = 16;

// A database's header and section table, as read and checked by read_info.
struct Info {
    Header header{};
    Section sections[kMaxSections]{};
    long fileSize = 0;
};

// What the installed database is (version 0: none), for the menu and the updater.
Info g_installed;
bool g_enabled = true;

// Reads and checks a database's header and section table: magic, a format this build reads, the header
// CRC, and every section inside the file. False (with *why) for anything else.
bool read_info(const char* path, Info* out, std::string* why) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        *why = "missing";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    out->fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = std::fread(&out->header, sizeof(Header), 1, f) == 1;
    const Header& h = out->header;
    if (!ok || std::memcmp(h.magic, "SFASHDB", 8) != 0) {
        *why = "not a shader database";
    } else if (h.format > kFormat) {
        *why = "made for a newer SFA-NX (format " + std::to_string(h.format) + ")";
        ok = false;
    } else if (h.sectionCount == 0 || h.sectionCount > kMaxSections ||
               h.headerSize != sizeof(Header) + h.sectionCount * sizeof(Section)) {
        *why = "bad section table";
        ok = false;
    } else if (std::fread(out->sections, sizeof(Section), h.sectionCount, f) != h.sectionCount) {
        *why = "cut short";
        ok = false;
    }
    std::fclose(f);
    if (!ok) {
        return false;
    }
    // CRC of header + table with the CRC field zero.
    Header zeroed = h;
    zeroed.headerCrc = 0;
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(&zeroed), sizeof(Header));
    crc = crc32(crc, reinterpret_cast<const Bytef*>(out->sections), h.sectionCount * sizeof(Section));
    if (crc != h.headerCrc) {
        *why = "header checksum mismatch";
        return false;
    }
    for (uint32_t i = 0; i < h.sectionCount; ++i) {
        const Section& s = out->sections[i];
        if (s.offset + s.packedSize > uint64_t(out->fileSize) || s.offset < h.headerSize) {
            *why = "cut short";
            return false;
        }
    }
    return true;
}

// The section with this tag, or nullptr.
const Section* find_section(const Info& info, const char* tag) {
    for (uint32_t i = 0; i < info.header.sectionCount; ++i) {
        if (std::memcmp(info.sections[i].tag, tag, 4) == 0) {
            return &info.sections[i];
        }
    }
    return nullptr;
}

// Reads one section of `db`, unpacked, and passes it to `sink` in pieces (false from it stops); checks
// the unpacked size and SHA-256. `progress` (may be empty) gets the share of the packed bytes read, 0..1.
bool read_section(const char* db, const Section& s, const std::function<bool(const unsigned char*, size_t)>& sink,
                  const std::function<void(float)>& progress, std::string* why) {
    FILE* in = std::fopen(db, "rb");
    if (in == nullptr) {
        *why = "cannot open the database";
        return false;
    }
    std::fseek(in, long(s.offset), SEEK_SET);
    Sha256Context sha;
    sha256ContextCreate(&sha);
    static unsigned char inBuf[64 * 1024], outBuf[256 * 1024];
    z_stream z{};
    bool ok = s.codec == kCodecStored || (s.codec == kCodecZlib && inflateInit(&z) == Z_OK);
    if (!ok) {
        *why = "unknown packing";
    }
    uint64_t left = s.packedSize, written = 0;
    int zr = Z_OK;
    while (ok && left > 0) {
        const size_t want = left < sizeof(inBuf) ? size_t(left) : sizeof(inBuf);
        if (std::fread(inBuf, 1, want, in) != want) {
            *why = "read error";
            ok = false;
            break;
        }
        left -= want;
        if (progress) {
            progress(float(s.packedSize - left) / float(s.packedSize));
        }
        if (s.codec == kCodecStored) {
            sha256ContextUpdate(&sha, inBuf, want);
            ok = sink(inBuf, want);
            written += want;
            continue;
        }
        z.next_in = inBuf;
        z.avail_in = uInt(want);
        do {
            z.next_out = outBuf;
            z.avail_out = sizeof(outBuf);
            zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) {
                *why = "damaged (unpack failed)";
                ok = false;
                break;
            }
            const size_t n = sizeof(outBuf) - z.avail_out;
            sha256ContextUpdate(&sha, outBuf, n);
            ok = sink(outBuf, n);
            written += n;
        } while (ok && z.avail_out == 0 && zr != Z_STREAM_END);
    }
    if (s.codec == kCodecZlib) {
        inflateEnd(&z);
    }
    std::fclose(in);
    if (ok && written != s.size) {
        *why = "damaged (wrong size)";
        ok = false;
    }
    if (ok) {
        unsigned char hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&sha, hash);
        if (std::memcmp(hash, s.sha256, SHA256_HASH_SIZE) != 0) {
            *why = "damaged (checksum)";
            ok = false;
        }
    }
    return ok;
}

// Unpacks one section of `db` into `dest` (via dest.tmp, renamed when complete).
bool extract(const char* db, const Section& s, const char* dest, std::string* why) {
    const std::string tmp = std::string(dest) + ".tmp";
    FILE* out = std::fopen(tmp.c_str(), "wb");
    if (out == nullptr) {
        *why = "cannot write to the SD card";
        return false;
    }
    bool writeFailed = false;
    bool ok = read_section(
        db, s,
        [&](const unsigned char* data, size_t n) {
            writeFailed = std::fwrite(data, 1, n, out) != n;
            return !writeFailed;
        },
        {}, why);
    if (writeFailed) {
        *why = "cannot write to the SD card";
    }
    ok = std::fclose(out) == 0 && ok;
    // Horizon's rename does not overwrite: remove the old file first.
    if (ok) {
        std::remove(dest);
        ok = std::rename(tmp.c_str(), dest) == 0;
        if (!ok) {
            *why = "cannot rename on the SD card";
        }
    }
    if (!ok) {
        std::remove(tmp.c_str());
    }
    return ok;
}

// Moves `candidate` in as the installed database when it is valid and newer than the installed one (or
// there is none); deletes it otherwise, so it is not checked again at every start.
void consider(const char* candidate, const char* what) {
    Info info;
    std::string why;
    if (!read_info(candidate, &info, &why)) {
        if (why != "missing") {
            melee_boot_trace("   shaderdb: %s %s: %s, deleted", what, candidate, why.c_str());
            std::remove(candidate);
        }
        return;
    }
    std::string installedWhy;
    Info installed;
    const bool have = read_info(kInstalled, &installed, &installedWhy);
    if (have && info.header.version <= installed.header.version) {
        melee_boot_trace("   shaderdb: %s v%u is not newer than the installed v%u, deleted", what,
                         info.header.version, installed.header.version);
        std::remove(candidate);
        return;
    }
    std::remove(kInstalled);
    if (std::rename(candidate, kInstalled) != 0) {
        melee_boot_trace("   shaderdb: could not move %s into cache/shaderdb", candidate);
        return;
    }
    melee_boot_trace("   shaderdb: installed v%u from %s", info.header.version, what);
}

// The stamp names the unpacked version and header CRC; a different database means unpack again.
std::string stamp_text(const Info& info) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u %08x\n", info.header.version, info.header.headerCrc);
    return buf;
}

bool stamp_matches(const Info& info) {
    FILE* f = std::fopen(kStamp, "r");
    if (f == nullptr) {
        return false;
    }
    char buf[64] = {};
    std::fgets(buf, sizeof(buf), f);
    std::fclose(f);
    struct stat st;
    return stamp_text(info) == buf && stat(kDksh, &st) == 0;
}

}  // namespace

// Start-up step (see the top of the file). Sets SFA_SHADER_SEED and FOXHOLLOW_SHADER_CACHE.
extern "C" void sfaShaderDbPrepare(void) {
    mkdir("sdmc:/switch/SFA-NX/cache", 0777);
    mkdir(kDir, 0777);
    std::remove(kPipe);  // unpacked recipes of earlier builds (see kPipe)
    consider(kPending, "download");
    consider(kAppCopy, "app folder copy");
    g_enabled = sfaSwitchMenuIniValue("shader_pack", 1) != 0;
    std::string why;
    if (!read_info(kInstalled, &g_installed, &why)) {
        g_installed = Info{};
        melee_boot_trace("   shaderdb: none installed (%s): shaders compile as the game needs them", why.c_str());
        setenv("FOXHOLLOW_SHADER_CACHE", kOffDir, 1);
        return;
    }
    const Header& h = g_installed.header;
    melee_boot_trace("   shaderdb: v%u, %u shaders (compiler %016llx), %u recipes%s", h.version, h.shaders,
                     (unsigned long long)h.compilerId, h.recipes, g_enabled ? "" : "; off in the - menu");
    if (!g_enabled) {
        setenv("FOXHOLLOW_SHADER_CACHE", kOffDir, 1);
        return;
    }
    if (!stamp_matches(g_installed)) {
        std::remove(kStamp);
        // Only the shaders: the recipes are read from the database itself (sfaShaderDbRecipes).
        const Section* dksh = find_section(g_installed, "DKSH");
        bool ok = true;
        if (dksh != nullptr) {
            ok = extract(kInstalled, *dksh, kDksh, &why);
        }
        if (!ok) {
            melee_boot_trace("   shaderdb: unpacking failed (%s); not used this run", why.c_str());
            setenv("FOXHOLLOW_SHADER_CACHE", kOffDir, 1);
            return;
        }
        FILE* f = std::fopen(kStamp, "w");
        if (f != nullptr) {
            std::fputs(stamp_text(g_installed).c_str(), f);
            std::fclose(f);
        }
        melee_boot_trace("   shaderdb: unpacked v%u", h.version);
    }
    setenv("SFA_SHADER_SEED", kDksh, 1);
    // Aurora's own seed file lookup finds nothing there now (no initial_pipeline_cache.db): the recipes
    // come from sfaShaderDbRecipes.
    setenv("FOXHOLLOW_SHADER_CACHE", kDir, 1);
}

// The installed database's recipes (its PIPE section: an SQLite database in Aurora's pipeline cache
// format), unpacked into memory for Aurora's pipeline cache (pipeline_cache.cpp load_pack_recipes), which
// opens it read-only with sqlite3_deserialize and frees it with free(). nullptr (size 0) when the pack is
// off, not installed, has no recipes or fails its checks. `progress` gets 0..1 while it unpacks (the boot
// screen's bar).
extern "C" void* sfaShaderDbRecipes(size_t* size, void (*progress)(float fraction, void* user), void* user) {
    *size = 0;
    if (!g_enabled || g_installed.header.version == 0) {
        return nullptr;
    }
    const Section* pipe = find_section(g_installed, "PIPE");
    if (pipe == nullptr || pipe->size == 0) {
        return nullptr;
    }
    auto* buffer = static_cast<unsigned char*>(std::malloc(size_t(pipe->size)));
    if (buffer == nullptr) {
        melee_boot_trace("   shaderdb: no memory for %llu bytes of recipes", (unsigned long long)pipe->size);
        return nullptr;
    }
    size_t used = 0;
    std::string why;
    const bool ok = read_section(
        kInstalled, *pipe,
        [&](const unsigned char* data, size_t n) {
            if (used + n > pipe->size) {
                return false;  // larger than the table says: read_section reports the wrong size
            }
            std::memcpy(buffer + used, data, n);
            used += n;
            return true;
        },
        [&](float fraction) {
            if (progress != nullptr) {
                progress(fraction, user);
            }
        },
        &why);
    if (!ok) {
        melee_boot_trace("   shaderdb: recipes not used (%s)", why.c_str());
        std::free(buffer);
        return nullptr;
    }
    *size = used;
    return buffer;
}

// Installed database version (0: none) and whether the − menu's Shader Pack setting uses it.
extern "C" unsigned sfaShaderDbVersion(void) { return g_installed.header.version; }
extern "C" int sfaShaderDbEnabled(void) { return g_enabled ? 1 : 0; }

// The newest file format this build reads, and a file's database version (0: missing or not valid), for
// the updater's "is the release newer" check.
extern "C" unsigned sfaShaderDbFormat(void) { return kFormat; }
extern "C" unsigned sfaShaderDbFileVersion(const char* path) {
    Info info;
    std::string why;
    return read_info(path, &info, &why) ? info.header.version : 0;
}

// 1 when a downloaded database waits in cache/shaderdb/ for the next start (the menu then offers a restart,
// not a download).
extern "C" int sfaShaderDbPending(void) { return sfaShaderDbFileVersion(kPending) != 0 ? 1 : 0; }

// Checks a downloaded database (the updater's cache/shaderdb/sfa-shaderdb.bin.part) before it is kept.
// 1 valid, 0 not (with the reason in why).
extern "C" int sfaShaderDbCheckFile(const char* path, char* why, size_t whySize) {
    Info info;
    std::string reason;
    if (!read_info(path, &info, &reason)) {
        std::snprintf(why, whySize, "Shader database %s", reason.c_str());
        return 0;
    }
    return 1;
}
