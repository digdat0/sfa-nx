// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWakerHD (https://github.com/centollOS/SwitchWakerHD) at df8fbde:
// runtime/src/gfx/deko/shader_files.cpp.
// Reading and writing the SWDK cache file format described in dksh_file.h (initial_dksh_cache.bin and
// dksh_local.bin): a 16-byte header, then CRC-protected records.
#include "dksh_file.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace swdk {
namespace {

// Record kinds: compiled shader, module (vertex + fragment pair), named stage (old layout), and stage
// with full binding info (kind 4, used for everything written now).
constexpr uint8_t kKindShader = 1, kKindModule = 2, kKindNamed = 3;
constexpr uint8_t kKindStage = 4;
constexpr size_t kRecordHeaderSize = 9;  // kind, payload size, crc

// Raw little-endian append / unaligned read of a trivially copyable value.
template <class T> void put(std::vector<uint8_t>* out, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out->insert(out->end(), p, p + sizeof v);
}
template <class T> T get(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof v);
    return v;
}
// 64-bit FNV-1a over a C string, continuing from `h`.
uint64_t fnv(const char* s, uint64_t h = 0xCBF29CE484222325ull) {
    for (; *s; ++s) h = (h ^ uint8_t(*s)) * 0x100000001B3ull;
    return h;
}

// Appends one record: kind byte, payload size, CRC-32 over kind and payload, then the payload.
void finish(std::vector<uint8_t>* out, uint8_t kind, const std::vector<uint8_t>& payload) {
    uLong crc = crc32(0, &kind, 1);
    crc = crc32(crc, payload.data(), uInt(payload.size()));
    put(out, kind);
    put(out, uint32_t(payload.size()));
    put(out, uint32_t(crc));
    out->insert(out->end(), payload.begin(), payload.end());
}

}  // namespace

// (dksh_file.h) Anything that changes generated shader code changes this id, so caches from older
// builds are recognised as stale instead of loading wrong shaders.
uint64_t dksh_compiler_id(uint32_t gxConfigVersion) {
    const std::string tail = "|translate " + std::to_string(kTranslateRevision) + "|gx config " +
                             std::to_string(gxConfigVersion) + "|SWDK " + std::to_string(kDkshFileVersion);
    return fnv(tail.c_str(), fnv(kDkshCompilerName));
}

// "SWDK", the format version and the compiler id.
std::vector<uint8_t> dksh_file_header(uint64_t compilerId) {
    std::vector<uint8_t> h;
    h.insert(h.end(), {'S', 'W', 'D', 'K'});
    put(&h, kDkshFileVersion);
    put(&h, compilerId);
    return h;
}

// Record writers, one per kind; the layouts are mirrored by read_dksh_file below and by
// switch/builder/seed-caches.py.
void append_record(std::vector<uint8_t>* out, const DkshShaderRecord& r) {
    std::vector<uint8_t> p;
    put(&p, uint8_t(r.stage));
    put(&p, r.glslHash);
    p.insert(p.end(), r.dksh.begin(), r.dksh.end());
    finish(out, kKindShader, p);
}

void append_record(std::vector<uint8_t>* out, const DkshModuleRecord& r) {
    std::vector<uint8_t> p;
    put(&p, r.moduleHash);
    put(&p, r.vertexGlslHash);
    put(&p, r.fragmentGlslHash);
    finish(out, kKindModule, p);
}

void append_record(std::vector<uint8_t>* out, const DkshNamedRecord& r) {
    std::vector<uint8_t> p;
    put(&p, uint8_t(r.stage));
    put(&p, uint8_t(r.name.size()));
    p.insert(p.end(), r.name.begin(), r.name.begin() + std::min<size_t>(r.name.size(), 255));
    put(&p, r.glslHash);
    put(&p, uint8_t(r.bindings.size()));
    for (const SlotBinding& b : r.bindings) {
        put(&p, uint8_t(b.kind));
        put(&p, b.slot);
        put(&p, uint8_t(b.group));
        put(&p, uint8_t(b.binding));
    }
    finish(out, kKindNamed, p);
}

// (dksh_file.h) Records with a bad CRC are counted and skipped; a record cut short ends the file, and
// validBytes tells the caller where the intact part ends (shaders.cpp then truncates the file there).
bool read_dksh_file(const std::vector<uint8_t>& data, DkshFile* out, std::string* error) {
    if (data.size() < kDkshFileHeaderSize || memcmp(data.data(), "SWDK", 4) != 0) {
        *error = "not a DKSH cache file (no SWDK magic)";
        return false;
    }
    const uint8_t* d = data.data();
    if (get<uint32_t>(d + 4) != kDkshFileVersion) {
        *error = "SWDK version " + std::to_string(get<uint32_t>(d + 4)) + ", this build reads " +
                 std::to_string(kDkshFileVersion);
        return false;
    }
    out->compilerId = get<uint64_t>(d + 8);
    size_t i = kDkshFileHeaderSize;
    while (i + kRecordHeaderSize <= data.size()) {
        const uint8_t kind = d[i];
        const uint32_t size = get<uint32_t>(d + i + 1), crc = get<uint32_t>(d + i + 5);
        if (i + kRecordHeaderSize + size > data.size()) break;  // cut short
        const uint8_t* p = d + i + kRecordHeaderSize;
        i += kRecordHeaderSize + size;
        out->validBytes = i;
        uLong c = crc32(0, &kind, 1);
        c = crc32(c, p, size);
        if (uint32_t(c) != crc) {
            out->badCrc++;
            continue;
        }
        if (kind == kKindShader && size >= 9 && (p[0] == 0 || p[0] == 4)) {
            DkshShaderRecord r;
            r.stage = ShaderStage(p[0]);
            r.glslHash = get<uint64_t>(p + 1);
            r.dksh.assign(p + 9, p + size);
            out->shaders.push_back(std::move(r));
        } else if (kind == kKindModule && size == 24) {
            out->modules.push_back({get<uint64_t>(p), get<uint64_t>(p + 8), get<uint64_t>(p + 16)});
        } else if ((kind == kKindNamed || kind == kKindStage) && size >= 2 && (p[0] == 0 || p[0] == 4)) {
            DkshNamedRecord r;
            r.stage = ShaderStage(p[0]);
            const size_t n = p[1];
            if (size < 2 + n + 9) continue;
            r.name.assign(reinterpret_cast<const char*>(p + 2), n);
            r.glslHash = get<uint64_t>(p + 2 + n);
            const size_t count = p[2 + n + 8];
            const size_t stride=kind==kKindStage?18:4;
            if (size != 2 + n + 9 + stride * count) continue;
            const uint8_t* b = p + 2 + n + 9;
            for (size_t k = 0; k < count; k++, b += stride) {
                if (kind==kKindStage)
                    r.bindings.push_back({SlotBinding::Kind(b[0]),b[1],get<uint32_t>(b+2),get<uint32_t>(b+6),get<uint32_t>(b+10),get<uint32_t>(b+14)});
                else r.bindings.push_back({SlotBinding::Kind(b[0]), b[1], b[2], b[3]});
            }
            out->named.push_back(std::move(r));
        }
        // unknown kinds: skipped (a later version's additions)
    }
    return true;
}

// Kind-4 stage record: like a named record, with the sampler group and binding of each slot as well.
void append_stage_record(std::vector<uint8_t>* out, const DkshNamedRecord& r) {
    std::vector<uint8_t> p;
    const size_t length=std::min<size_t>(r.name.size(),255);
    put(&p,uint8_t(r.stage));put(&p,uint8_t(length));
    p.insert(p.end(),r.name.begin(),r.name.begin()+length);
    put(&p,r.glslHash);put(&p,uint8_t(r.bindings.size()));
    for (const auto& b:r.bindings) {
        put(&p,uint8_t(b.kind));put(&p,b.slot);put(&p,b.group);put(&p,b.binding);
        put(&p,b.samplerGroup);put(&p,b.samplerBinding);
    }
    finish(out,kKindStage,p);
}

// Counts for the startup log line: shaders per stage, failures, and how many modules and named stages
// have all their shaders present.
DkshFileStats dksh_file_stats(const DkshFile& file, uint64_t expectedCompilerId) {
    DkshFileStats s;
    s.idMatches = file.compilerId == expectedCompilerId;
    s.badCrc = file.badCrc;
    std::set<std::pair<uint8_t, uint64_t>> compiled;
    for (const DkshShaderRecord& r : file.shaders) {
        if (r.dksh.empty()) {
            s.failed++;
            continue;
        }
        (r.stage == ShaderStage::Vertex ? s.vertex : s.fragment)++;
        s.dkshBytes += r.dksh.size();
        compiled.insert({uint8_t(r.stage), r.glslHash});
    }
    s.modules = file.modules.size();
    for (const DkshModuleRecord& m : file.modules) {
        s.modulesComplete += compiled.count({uint8_t(ShaderStage::Vertex), m.vertexGlslHash}) &&
                             compiled.count({uint8_t(ShaderStage::Fragment), m.fragmentGlslHash});
    }
    s.named = file.named.size();
    for (const DkshNamedRecord& n : file.named) s.namedComplete += compiled.count({uint8_t(n.stage), n.glslHash});
    return s;
}

// The stats as one readable line.
std::string dksh_file_summary(const DkshFileStats& s) {
    char line[320];
    snprintf(line, sizeof line,
             "%zu shaders (%zu vertex, %zu fragment, %zu failed), %.2f MiB of DKSH; %zu / %zu modules and %zu / %zu "
             "named shaders complete; compiler id %s; %zu bad CRC",
             s.vertex + s.fragment + s.failed, s.vertex, s.fragment, s.failed, double(s.dkshBytes) / 1048576.0,
             s.modulesComplete, s.modules, s.namedComplete, s.named, s.idMatches ? "matches" : "DIFFERS (stale file)",
             s.badCrc);
    return line;
}

// Key of a shader in the registry: a hash of its GLSL text.
uint64_t glsl_hash(const std::string& glsl) { return XXH3_64bits(glsl.data(), glsl.size()); }

}  // namespace swdk
