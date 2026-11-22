#include "texture_replacement.hpp"

#include "../io.hpp"
#include "../gx/gx.hpp"
#include "../gx/texture.hpp"
#include "../internal.hpp"
#include "../thread.hpp"
#include "../webgpu/gpu.hpp"
#include "dds_io.hpp"
#include "png_io.hpp"
#include "texture_convert.hpp"

#include <aurora/texture.hpp>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__SWITCH__)
extern "C" int sfaSwitchMenuIniValue(const char* key, int def) __attribute__((weak));
#endif

namespace aurora::texture {
namespace {
constexpr Module Log{"aurora::texture"};

constexpr uint64_t kReplacementCacheBudgetBytes = 4294967296; // 4GB
constexpr uint64_t kReplacementWildcardTextureHash = kWildcardTextureHash;
constexpr uint64_t kReplacementWildcardTlutHash = kWildcardTlutHash;

enum class EntryKind {
  Raw,
  File,
  Virtual,
};

struct VirtualReadState {
  std::mutex mutex;
  std::condition_variable cv;
  bool cancelled = false;
  uint32_t inFlight = 0;
};

struct ReplacementEntry {
  uint64_t id = 0;
  int32_t priority = 0;
  uint64_t sequence = 0;
  EntryKind kind = EntryKind::Raw;
  std::span<const uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 1;
  uint32_t gxFormat = 0;
  std::string label;
  std::filesystem::path path;
  std::string virtualPath;
  VirtualFileSource source;
  std::shared_ptr<VirtualReadState> virtualReadState;
  std::optional<gfx::ConvertedTexture> thumbnail;
  bool thumbnailIncludesBase = false;
};

enum class Tier {
  Thumbnail,
  Full,
};

struct SelectedCache {
  gfx::TextureHandle handle;
  uint64_t id = 0;
  uint64_t bytes = 0;
  std::list<ReplacementKey>::iterator lruIt;
  Tier tier = Tier::Full;
};

struct EntryLoadSnapshot {
  ReplacementKey key;
  uint64_t id = 0;
  EntryKind kind = EntryKind::File;
  std::string label;
  std::filesystem::path path;
  std::string virtualPath;
  VirtualFileSource source;
  std::shared_ptr<VirtualReadState> virtualReadState;
};

struct LoadJob {
  Tier tier = Tier::Full;
  EntryLoadSnapshot entry;
};

struct LoadCompletion {
  Tier tier = Tier::Full;
  EntryLoadSnapshot entry;
  std::optional<gfx::ConvertedTexture> texture;
  bool thumbnailStored = false;
};

struct SourceKeyHash {
  size_t operator()(const TextureSourceKey& key) const noexcept {
    return absl::HashOf(key.textureHash, key.tlutHash, key.width, key.height, key.format, key.hasTlut);
  }
};

struct ReplacementKeyHash {
  size_t operator()(const ReplacementKey& key) const noexcept {
    if (const auto* ptrKey = std::get_if<TexturePointerKey>(&key)) {
      return absl::HashOf(0u, ptrKey->data);
    }
    const auto& sourceKey = std::get<TextureSourceKey>(key);
    return absl::HashOf(1u, sourceKey.textureHash, sourceKey.tlutHash, sourceKey.width, sourceKey.height,
                        sourceKey.format, sourceKey.hasTlut);
  }
};

std::mutex s_registryMutex;
absl::flat_hash_map<ReplacementKey, std::vector<ReplacementEntry>, ReplacementKeyHash> s_entriesByKey;
absl::flat_hash_map<ReplacementKey, SelectedCache, ReplacementKeyHash> s_cacheByKey;
absl::flat_hash_set<uint64_t> s_failedIds;
absl::flat_hash_set<TextureSourceKey, SourceKeyHash> s_reportedMisses;
std::list<ReplacementKey> s_replacementLru;
uint64_t s_replacementCacheBytes = 0;
uint64_t s_nextRegistrationId = 1;
uint64_t s_nextSequence = 1;
uint32_t s_sourceEntryCount = 0;

std::mutex s_jobMutex;
std::condition_variable s_jobCv;
std::deque<LoadJob> s_highPriorityJobs;
std::deque<LoadJob> s_lowPriorityJobs;
std::deque<LoadCompletion> s_workerCompletions;
std::vector<LoadCompletion> s_readyPublishes;
std::unordered_map<uint64_t, uint64_t> s_pendingFullLoads;
std::unordered_set<uint64_t> s_pendingThumbnailLoads;
std::vector<thread::Thread> s_workers;
uint64_t s_requestSequence = 0;
bool s_workersPaused = false;
uint32_t s_workerCountOverride = 0;

const ReplacementEntry* find_selected_entry_locked(const ReplacementKey& key) noexcept;
ReplacementEntry* find_entry_locked(const ReplacementKey& key, uint64_t id) noexcept;

unsigned char ascii_lower(unsigned char ch) noexcept {
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<unsigned char>(ch - 'A' + 'a');
  }
  return ch;
}

bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower(static_cast<unsigned char>(lhs[i])) != ascii_lower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

int compare_ascii_ci(std::string_view lhs, std::string_view rhs) noexcept {
  const size_t count = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < count; ++i) {
    const auto lhsCh = ascii_lower(static_cast<unsigned char>(lhs[i]));
    const auto rhsCh = ascii_lower(static_cast<unsigned char>(rhs[i]));
    if (lhsCh < rhsCh) {
      return -1;
    }
    if (lhsCh > rhsCh) {
      return 1;
    }
  }
  if (lhs.size() < rhs.size()) {
    return -1;
  }
  if (lhs.size() > rhs.size()) {
    return 1;
  }
  return 0;
}

std::vector<std::string> path_components(const std::filesystem::path& root, const std::filesystem::path& path) {
  std::vector<std::string> components;
  auto relative = path.lexically_relative(root);
  if (relative.empty()) {
    relative = path.filename();
  }
  for (const auto& component : relative) {
    components.push_back(io::fs_path_to_string(component));
  }
  return components;
}

struct ReplacementCandidate {
  std::filesystem::path path;
  std::vector<std::string> components;
};

bool compare_replacement_candidates(const ReplacementCandidate& lhs, const ReplacementCandidate& rhs) noexcept {
  const size_t count = std::min(lhs.components.size(), rhs.components.size());
  for (size_t i = 0; i < count; ++i) {
    const auto cmp = compare_ascii_ci(lhs.components[i], rhs.components[i]);
    if (cmp != 0) {
      return cmp < 0;
    }
    if (lhs.components[i] != rhs.components[i]) {
      return lhs.components[i] < rhs.components[i];
    }
  }
  return lhs.components.size() < rhs.components.size();
}

bool is_relative_to(const std::filesystem::path& path, const std::filesystem::path& root) noexcept {
  if (root.empty()) {
    return false;
  }
  auto pathIt = path.begin();
  auto rootIt = root.begin();
  for (; rootIt != root.end(); ++rootIt, ++pathIt) {
    if (pathIt == path.end() ||
        !iequals_ascii(io::fs_path_to_string(*pathIt), io::fs_path_to_string(*rootIt))) {
      return false;
    }
  }
  return true;
}

bool is_sidecar_mip(std::string_view stem) noexcept {
  constexpr std::string_view tag = "_mip";
  size_t i = stem.size();
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') {
    --i;
  }

  if (i == stem.size() || i < tag.size()) {
    return false;
  }

  return stem.substr(i - tag.size(), tag.size()) == tag;
}

std::optional<uint64_t> parse_hex(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char ch : text) {
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<uint64_t>(ch - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

std::optional<uint32_t> parse_u32(std::string_view text, int base = 10) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }

  uint32_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value, base);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::pair<uint32_t, uint32_t>> parse_dimensions(std::string_view text) noexcept {
  const size_t sep = text.find('x');
  if (sep == std::string_view::npos) {
    return std::nullopt;
  }

  const auto width = parse_u32(text.substr(0, sep));
  const auto height = parse_u32(text.substr(sep + 1));
  if (!width.has_value() || !height.has_value()) {
    return std::nullopt;
  }
  return std::pair{*width, *height};
}

uint32_t texture_base_level_size(const GXTexObj_& obj) noexcept {
  switch (obj.format()) {
  case GX_TF_R8_PC:
    return obj.width() * obj.height();
  case GX_TF_RG8_PC:
    return obj.width() * obj.height() * 2;
  case GX_TF_RGBA8_PC:
    return obj.width() * obj.height() * 4;
  case GX_TF_BC1_PC:
    return ((obj.width() + 3) / 4) * ((obj.height() + 3) / 4) * 8;
  default:
    return GXGetTexBufferSize(obj.width(), obj.height(), obj.format(), false, 0);
  }
}

ArrayRef<uint8_t> tlut_bytes(const GXTlutObj_& tlut) noexcept {
  return {static_cast<const uint8_t*>(tlut.data), static_cast<size_t>(tlut.numEntries) * sizeof(uint16_t)};
}

std::optional<uint64_t> compute_referenced_tlut_hash(const GXTexObj_& obj, ArrayRef<uint8_t> tlutData) noexcept {
  const uint32_t textureSize = texture_base_level_size(obj);
  const auto* textureData = static_cast<const uint8_t*>(obj.data);
  if (!gx::is_palette_format(obj.format()) || !obj.has_data() || textureSize == 0 || tlutData.empty()) {
    return std::nullopt;
  }

  uint32_t minIndex = 0xffff;
  uint32_t maxIndex = 0;
  switch (obj.format()) {
  case GX_TF_C4:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t lowNibble = textureData[i] & 0xf;
      const uint32_t highNibble = textureData[i] >> 4;
      minIndex = std::min({minIndex, lowNibble, highNibble});
      maxIndex = std::max({maxIndex, lowNibble, highNibble});
    }
    break;
  case GX_TF_C8:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t index = textureData[i];
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  case GX_TF_C14X2:
    for (uint32_t i = 0; i + sizeof(uint16_t) <= textureSize; i += sizeof(uint16_t)) {
      uint16_t value = 0;
      std::memcpy(&value, textureData + i, sizeof(value));
      const uint32_t index = bswap(value) & 0x3fff;
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  default:
    return std::nullopt;
  }

  size_t tlutSize = 2 * (static_cast<size_t>(maxIndex) + 1 - minIndex);
  const size_t tlutOffset = 2 * static_cast<size_t>(minIndex);
  if (tlutOffset + tlutSize > tlutData.size()) {
    return std::nullopt;
  }
  return XXH64(tlutData.data() + tlutOffset, tlutSize, 0);
}

std::optional<uint64_t> compute_referenced_tlut_hash(const GXTexObj_& obj) noexcept {
  if (!gx::is_palette_format(obj.format()) || obj.tlut >= gx::g_gxState.loadedTluts.size()) {
    return std::nullopt;
  }

  const auto& tlut = gx::g_gxState.loadedTluts[obj.tlut];
  if (tlut.data == nullptr) {
    return std::nullopt;
  }

  return compute_referenced_tlut_hash(obj, tlut_bytes(tlut));
}

const GXTlutObj_* get_loaded_tlut(const GXTexObj_& obj) noexcept {
  if (!gx::is_palette_format(obj.format()) || obj.tlut >= gx::g_gxState.loadedTluts.size()) {
    return nullptr;
  }

  const auto& tlut = gx::g_gxState.loadedTluts[obj.tlut];
  return tlut.data != nullptr ? &tlut : nullptr;
}

TextureSourceKey build_source_key_base(const GXTexObj_& obj) noexcept {
  TextureSourceKey key{
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
      .hasTlut = gx::is_palette_format(obj.format()),
  };

  const uint32_t textureSize = texture_base_level_size(obj);
  if (obj.has_data() && textureSize != 0) {
    key.textureHash = XXH64(obj.data, textureSize, 0);
  }
  return key;
}

TextureSourceKey build_source_key(const GXTexObj_& obj) noexcept {
  auto key = build_source_key_base(obj);
  if (key.hasTlut) {
    key.tlutHash = compute_referenced_tlut_hash(obj).value_or(0);
  }
  return key;
}

TextureSourceKey build_source_key(const GXTexObj_& obj, const GXTlutObj_& tlut) noexcept {
  auto key = build_source_key_base(obj);
  if (key.hasTlut && tlut.data != nullptr) {
    key.tlutHash = compute_referenced_tlut_hash(obj, tlut_bytes(tlut)).value_or(0);
  }
  return key;
}

std::string format_replacement_filename(const TextureSourceKey& key) {
  if (key.hasTlut) {
    return fmt::format("tex1_{}x{}_{:016x}_{:016x}_{}.dds", key.width, key.height, key.textureHash, key.tlutHash,
                       key.format);
  }
  return fmt::format("tex1_{}x{}_{:016x}_{}.dds", key.width, key.height, key.textureHash, key.format);
}

std::string format_source_key_for_log(const TextureSourceKey& key) {
  const auto textureHash =
      key.textureHash == kReplacementWildcardTextureHash ? std::string{"$"} : fmt::format("{:016x}", key.textureHash);
  if (!key.hasTlut) {
    return fmt::format("{}x{} tex={} fmt={}", key.width, key.height, textureHash, key.format);
  }

  const auto tlutHash =
      key.tlutHash == kReplacementWildcardTlutHash ? std::string{"$"} : fmt::format("{:016x}", key.tlutHash);
  return fmt::format("{}x{} tex={} tlut={} fmt={}", key.width, key.height, textureHash, tlutHash, key.format);
}

std::optional<gfx::ConvertedTexture> load_texture_file(const std::filesystem::path& path) {
  if (iequals_ascii(io::fs_path_to_string(path.extension()), ".png")) {
    return gfx::png::load_png_file(path);
  }
  return gfx::dds::load_dds_file(path);
}

bool remove_mipmaps(gfx::ConvertedTexture& texture) noexcept {
  if (texture.mips <= 1) {
    return true;
  }

  const uint64_t size = gfx::calc_texture_size(texture.format, texture.width, texture.height, 1);
  if (size == 0 || size > texture.data.size()) {
    return false;
  }

  ByteBuffer data{size};
  std::memcpy(data.data(), texture.data.data(), size);
  texture.mips = 1;
  texture.data = std::move(data);
  return true;
}

// SFA-NX: BC1/BC2/BC3 replacements whose size is not a multiple of 4 (e.g. 62x31 in the GameBanana HD pack).
// WebGPU refuses a compressed texture whose base size is not whole blocks, but the DDS data already holds
// whole blocks (ceil(w/4) x ceil(h/4) per level), so these are decoded to RGBA8 at their real size instead.
// They are small, so four times the memory costs little. Other compressed formats are still refused.

// The block kind of a format this can decode (1, 2, 3 for BC1, BC2, BC3), 0 for any other format.
int bc_kind(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
    return 1;
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
    return 2;
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
    return 3;
  default:
    return 0;
  }
}

// An RGB565 colour as 8-bit RGB (bits repeated into the low bits) with alpha 255.
void expand_565(uint16_t c, uint8_t out[4]) noexcept {
  const uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  out[0] = uint8_t(r << 3 | r >> 2);
  out[1] = uint8_t(g << 2 | g >> 4);
  out[2] = uint8_t(b << 3 | b >> 2);
  out[3] = 255;
}

// One 4x4 block to 16 RGBA8 texels in row order (the D3D BC1-3 rules: BC1's 3-colour mode with transparent
// black when c0 <= c1; BC2/BC3 colour always 4-colour; BC2 4-bit alpha; BC3 interpolated 8- or 6-step alpha).
void decode_bc_block(const uint8_t* block, int kind, uint8_t out[16][4]) noexcept {
  const uint8_t* color = kind == 1 ? block : block + 8;
  const uint16_t c0 = uint16_t(color[0] | color[1] << 8);
  const uint16_t c1 = uint16_t(color[2] | color[3] << 8);
  const uint32_t indices = uint32_t(color[4]) | uint32_t(color[5]) << 8 | uint32_t(color[6]) << 16 |
                           uint32_t(color[7]) << 24;
  const bool fourColour = kind != 1 || c0 > c1;
  uint8_t pal[4][4];
  expand_565(c0, pal[0]);
  expand_565(c1, pal[1]);
  for (int ch = 0; ch < 3; ++ch) {
    if (fourColour) {
      pal[2][ch] = uint8_t((2 * pal[0][ch] + pal[1][ch]) / 3);
      pal[3][ch] = uint8_t((pal[0][ch] + 2 * pal[1][ch]) / 3);
    } else {
      pal[2][ch] = uint8_t((pal[0][ch] + pal[1][ch]) / 2);
      pal[3][ch] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = fourColour ? 255 : 0;
  for (int i = 0; i < 16; ++i) {
    std::memcpy(out[i], pal[(indices >> (2 * i)) & 3], 4);
  }
  if (kind == 2) {
    for (int i = 0; i < 16; ++i) {
      const uint8_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 15;
      out[i][3] = uint8_t(nibble * 17);
    }
  } else if (kind == 3) {
    const uint32_t a0 = block[0], a1 = block[1];
    uint8_t alpha[8] = {uint8_t(a0), uint8_t(a1)};
    if (a0 > a1) {
      for (uint32_t k = 1; k < 7; ++k)
        alpha[k + 1] = uint8_t(((7 - k) * a0 + k * a1) / 7);
    } else {
      for (uint32_t k = 1; k < 5; ++k)
        alpha[k + 1] = uint8_t(((5 - k) * a0 + k * a1) / 5);
      alpha[6] = 0;
      alpha[7] = 255;
    }
    uint64_t bits = 0;
    for (int b = 0; b < 6; ++b)
      bits |= uint64_t(block[2 + b]) << (8 * b);
    for (int i = 0; i < 16; ++i)
      out[i][3] = alpha[(bits >> (3 * i)) & 7];
  }
}

// Decodes a BC1-3 texture (every level it holds) to RGBA8 (sRGB kept). False for other formats or data
// shorter than its levels need; the texture is then unchanged.
bool decode_bc_to_rgba8(gfx::ConvertedTexture& tex) noexcept {
  const int kind = bc_kind(tex.format);
  if (kind == 0 || tex.width == 0 || tex.height == 0 || tex.mips == 0) {
    return false;
  }
  const uint32_t blockBytes = kind == 1 ? 8 : 16;
  const uint64_t srcSize = gfx::calc_texture_size(tex.format, tex.width, tex.height, tex.mips);
  if (srcSize == 0 || srcSize > tex.data.size()) {
    return false;
  }
  uint64_t dstSize = 0;
  for (uint32_t mip = 0; mip < tex.mips; ++mip) {
    dstSize += uint64_t(std::max(tex.width >> mip, 1u)) * std::max(tex.height >> mip, 1u) * 4;
  }
  ByteBuffer out{dstSize};
  const uint8_t* src = tex.data.data();
  uint8_t* dst = out.data();
  for (uint32_t mip = 0; mip < tex.mips; ++mip) {
    const uint32_t w = std::max(tex.width >> mip, 1u), h = std::max(tex.height >> mip, 1u);
    for (uint32_t by = 0; by < h; by += 4) {
      for (uint32_t bx = 0; bx < w; bx += 4) {
        uint8_t texels[16][4];
        decode_bc_block(src, kind, texels);
        src += blockBytes;
        // Only the texels inside the level: the block's padding beyond w x h is dropped.
        for (uint32_t y = 0; y < 4 && by + y < h; ++y)
          for (uint32_t x = 0; x < 4 && bx + x < w; ++x)
            std::memcpy(dst + ((by + y) * size_t(w) + bx + x) * 4, texels[y * 4 + x], 4);
      }
    }
    dst += size_t(w) * h * 4;
  }
  const bool srgb = tex.format == wgpu::TextureFormat::BC1RGBAUnormSrgb ||
                    tex.format == wgpu::TextureFormat::BC2RGBAUnormSrgb ||
                    tex.format == wgpu::TextureFormat::BC3RGBAUnormSrgb;
  tex.format = srgb ? wgpu::TextureFormat::RGBA8UnormSrgb : wgpu::TextureFormat::RGBA8Unorm;
  tex.data = std::move(out);
  return true;
}

// ---- SFA-NX: mipmaps for replacements that ship without them.
// 2716 of the GameBanana pack's 4029 DDS files have a single level (1024x576, 1024x528, 256x256, ...), so
// every minified draw sampled the full-size level: shimmering at a distance and the costliest draws of
// docked 1080p (a 1024x1024 texture on a far wall reads texels spread over the whole image). The missing
// levels are made on the texture worker at load: level 0 is kept as the file has it, each lower level is
// a 2x2 box filter of the one above (colour weighted by alpha, so transparent texels' colour does not
// bleed into edges) and is encoded back to the file's BC1/BC3 format. Alpha-tested textures (foliage,
// fences) keep their level 0 coverage at alpha 128 in every level, so they do not thin out in the
// distance. switch_menu.ini hd_mips=0 turns it off (read once).

// An RGBA8 image for mip generation.
struct MipImage {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

// The next level of `src`: half its size (at least 1), each texel the alpha-weighted average of up to
// 2x2 texels (an odd last row or column is dropped, as the GPU's level sizes floor).
MipImage downsample_rgba8(const MipImage& src) {
  MipImage dst;
  dst.width = std::max(src.width >> 1, 1u);
  dst.height = std::max(src.height >> 1, 1u);
  dst.rgba.resize(size_t(dst.width) * dst.height * 4);
  for (uint32_t y = 0; y < dst.height; ++y) {
    const uint32_t y0 = std::min(y * 2, src.height - 1), y1 = std::min(y * 2 + 1, src.height - 1);
    for (uint32_t x = 0; x < dst.width; ++x) {
      const uint32_t x0 = std::min(x * 2, src.width - 1), x1 = std::min(x * 2 + 1, src.width - 1);
      const uint8_t* p[4] = {&src.rgba[(size_t(y0) * src.width + x0) * 4], &src.rgba[(size_t(y0) * src.width + x1) * 4],
                             &src.rgba[(size_t(y1) * src.width + x0) * 4], &src.rgba[(size_t(y1) * src.width + x1) * 4]};
      uint32_t sumA = 0;
      uint32_t sumC[3] = {}, sumCA[3] = {};
      for (const uint8_t* t : p) {
        sumA += t[3];
        for (int c = 0; c < 3; ++c) {
          sumC[c] += t[c];
          sumCA[c] += uint32_t(t[c]) * t[3];
        }
      }
      uint8_t* o = &dst.rgba[(size_t(y) * dst.width + x) * 4];
      for (int c = 0; c < 3; ++c) {
        // Fully transparent texels: plain average (their colour is still what a blend would show).
        o[c] = uint8_t(sumA > 0 ? (sumCA[c] + sumA / 2) / sumA : (sumC[c] + 2) / 4);
      }
      o[3] = uint8_t((sumA + 2) / 4);
    }
  }
  return dst;
}

// Texels whose alpha, multiplied by `scale`, reaches the alpha-test threshold (128).
size_t alpha_coverage(const std::vector<uint8_t>& rgba, float scale) {
  size_t n = 0;
  for (size_t i = 3; i < rgba.size(); i += 4) {
    n += float(rgba[i]) * scale >= 127.5f;
  }
  return n;
}

// Scales a level's alpha so the share of texels at or above 128 matches level 0's (`fraction`): binary
// search over the scale, as in Castano's "coverage-preserving mipmaps". The scale stays within 0.5-4.
void preserve_alpha_coverage(std::vector<uint8_t>& rgba, double fraction) {
  const size_t texels = rgba.size() / 4;
  const double target = fraction * double(texels);
  float lo = 0.5f, hi = 4.f, best = 1.f;
  double bestErr = std::abs(double(alpha_coverage(rgba, 1.f)) - target);
  for (int i = 0; i < 12; ++i) {
    const float mid = (lo + hi) * 0.5f;
    const double n = double(alpha_coverage(rgba, mid));
    if (std::abs(n - target) < bestErr) {
      bestErr = std::abs(n - target);
      best = mid;
    }
    if (n < target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  if (best == 1.f) {
    return;
  }
  for (size_t i = 3; i < rgba.size(); i += 4) {
    rgba[i] = uint8_t(std::min(255.f, float(rgba[i]) * best + 0.5f));
  }
}

// RGB888 to RGB565 with rounding.
uint16_t pack_565(const float c[3]) noexcept {
  const auto q = [](float v, int max) { return uint32_t(std::clamp(v, 0.f, 255.f) * float(max) / 255.f + 0.5f); };
  return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

// One BC1 colour block (also BC3's colour half) from 16 RGBA8 texels in row order. The endpoints lie on
// the block's principal colour axis (power iteration on the covariance) through its mean, at the
// extreme projections, refitted once by least squares; each texel takes the palette entry its
// projection onto the endpoint line is nearest to. `punch`: BC1's 3-colour mode, texels below alpha 128 get index 3 (transparent black).
void encode_bc_colour(const uint8_t px[16][4], bool punch, uint8_t out[8]) noexcept {
  const auto skip = [&](int i) { return punch && px[i][3] < 128; };
  float mean[3] = {};
  int n = 0;
  for (int i = 0; i < 16; ++i) {
    if (skip(i)) {
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      mean[c] += px[i][c];
    }
    ++n;
  }
  if (n == 0) {
    // All transparent: c0 = c1 = 0 (3-colour mode), every index 3.
    std::memset(out, 0, 4);
    std::memset(out + 4, 0xFF, 4);
    return;
  }
  for (float& m : mean) {
    m /= float(n);
  }
  float cov[6] = {}; // rr rg rb gg gb bb
  for (int i = 0; i < 16; ++i) {
    if (skip(i)) {
      continue;
    }
    const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
    cov[0] += d[0] * d[0];
    cov[1] += d[0] * d[1];
    cov[2] += d[0] * d[2];
    cov[3] += d[1] * d[1];
    cov[4] += d[1] * d[2];
    cov[5] += d[2] * d[2];
  }
  float axis[3] = {1.f, 1.f, 1.f};
  for (int it = 0; it < 4; ++it) {
    const float v[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                        cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                        cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
    const float len = std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
    if (len < 1e-6f) {
      break; // a flat block: the endpoints collapse onto the mean whatever the axis
    }
    for (int c = 0; c < 3; ++c) {
      axis[c] = v[c] / len;
    }
  }
  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < 16; ++i) {
    if (skip(i)) {
      continue;
    }
    const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  float e0[3], e1[3];
  for (int c = 0; c < 3; ++c) {
    e0[c] = mean[c] + axis[c] * tmax;
    e1[c] = mean[c] + axis[c] * tmin;
  }
  // Indices for a pair of 565 endpoints, by projecting each texel onto the line between the decoded
  // endpoints (the palette's entries lie on it), and the squared error that gives. 4-colour mode orders
  // the palette c0, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1, c1 (indices 0, 2, 3, 1); 3-colour mode c0, the
  // midpoint, c1 (0, 2, 1).
  const auto assign = [&](uint16_t a0, uint16_t a1, uint32_t& indices) -> float {
    uint8_t p0[4], p1[4];
    expand_565(a0, p0);
    expand_565(a1, p1);
    const float d[3] = {float(p1[0]) - p0[0], float(p1[1]) - p0[1], float(p1[2]) - p0[2]};
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const int steps = punch ? 2 : 3;
    static constexpr uint32_t kOrder4[4] = {0, 2, 3, 1}, kOrder3[3] = {0, 2, 1};
    indices = 0;
    float err = 0.f;
    for (int i = 0; i < 16; ++i) {
      if (skip(i)) {
        indices |= 3u << (2 * i);
        continue;
      }
      const float q[3] = {float(px[i][0]) - p0[0], float(px[i][1]) - p0[1], float(px[i][2]) - p0[2]};
      int k = 0;
      if (dd > 0.f) {
        const float t = (q[0] * d[0] + q[1] * d[1] + q[2] * d[2]) / dd;
        k = std::clamp(int(t * float(steps) + 0.5f), 0, steps);
      }
      indices |= (punch ? kOrder3[k] : kOrder4[k]) << (2 * i);
      const float w = float(k) / float(steps);
      for (int c = 0; c < 3; ++c) {
        const float e = q[c] - w * d[c];
        err += e * e;
      }
    }
    return err;
  };
  // Orders the endpoints for the mode (BC1's 4-colour mode needs c0 > c1, its 3-colour mode c0 <= c1;
  // BC3 colour is always 4-colour) and assigns indices. Equal endpoints: every index 0 (that colour in
  // either mode), or 3 for transparent texels.
  const auto encode = [&](uint16_t a0, uint16_t a1, uint32_t& indices) -> float {
    if (punch ? a0 > a1 : a0 < a1) {
      std::swap(a0, a1);
    }
    return assign(a0, a1, indices);
  };
  uint16_t c0 = pack_565(e0), c1 = pack_565(e1);
  if (punch ? c0 > c1 : c0 < c1) {
    std::swap(c0, c1);
  }
  uint32_t indices = 0;
  float err = encode(c0, c1, indices);
  // One least-squares refit: with each texel's weight between the endpoints fixed by its index, the
  // endpoints that minimise the error (a 2x2 system per channel); kept if it lowers the error.
  if (c0 != c1) {
    float a = 0.f, b = 0.f, c = 0.f, x0[3] = {}, x1[3] = {};
    for (int i = 0; i < 16; ++i) {
      if (skip(i)) {
        continue;
      }
      const uint32_t idx = (indices >> (2 * i)) & 3;
      // the index's position along c0 -> c1
      const float w = punch ? (idx == 0 ? 0.f : idx == 1 ? 1.f : 0.5f)
                            : (idx == 0 ? 0.f : idx == 1 ? 1.f : idx == 2 ? 1.f / 3.f : 2.f / 3.f);
      a += (1.f - w) * (1.f - w);
      b += w * w;
      c += w * (1.f - w);
      for (int ch = 0; ch < 3; ++ch) {
        x0[ch] += (1.f - w) * px[i][ch];
        x1[ch] += w * px[i][ch];
      }
    }
    const float det = a * b - c * c;
    if (std::abs(det) > 1e-4f) {
      float r0[3], r1[3];
      for (int ch = 0; ch < 3; ++ch) {
        r0[ch] = (b * x0[ch] - c * x1[ch]) / det;
        r1[ch] = (a * x1[ch] - c * x0[ch]) / det;
      }
      uint32_t refined = 0;
      uint16_t n0 = pack_565(r0), n1 = pack_565(r1);
      if (punch ? n0 > n1 : n0 < n1) {
        std::swap(n0, n1);
      }
      if (n0 != n1) {
        const float refinedErr = encode(n0, n1, refined);
        if (refinedErr < err) {
          c0 = n0;
          c1 = n1;
          indices = refined;
          err = refinedErr;
        }
      }
    }
  }
  if (c0 == c1) {
    // (see encode: index 0, or 3 for transparent texels)
    uint32_t flat = 0;
    for (int i = 0; i < 16; ++i) {
      flat |= (skip(i) ? 3u : 0u) << (2 * i);
    }
    indices = flat;
  }
  out[0] = uint8_t(c0);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1);
  out[3] = uint8_t(c1 >> 8);
  for (int b4 = 0; b4 < 4; ++b4) {
    out[4 + b4] = uint8_t(indices >> (8 * b4));
  }
}

// BC3's alpha half: the block's alpha range in 8-step mode (a0 > a1), each texel the nearest step.
void encode_bc3_alpha(const uint8_t px[16][4], uint8_t out[8]) noexcept {
  uint8_t amin = 255, amax = 0;
  for (int i = 0; i < 16; ++i) {
    amin = std::min(amin, px[i][3]);
    amax = std::max(amax, px[i][3]);
  }
  out[0] = amax;
  out[1] = amin;
  uint64_t bits = 0;
  if (amax != amin) {
    uint8_t steps[8] = {amax, amin};
    for (uint32_t k = 1; k < 7; ++k) {
      steps[k + 1] = uint8_t(((7 - k) * amax + k * amin) / 7);
    }
    for (int i = 0; i < 16; ++i) {
      uint64_t idx = 0;
      int bestErr = 256;
      for (int k = 0; k < 8; ++k) {
        const int err = std::abs(int(px[i][3]) - steps[k]);
        if (err < bestErr) {
          bestErr = err;
          idx = uint64_t(k);
        }
      }
      bits |= idx << (3 * i);
    }
  }
  for (int b = 0; b < 6; ++b) {
    out[2 + b] = uint8_t(bits >> (8 * b));
  }
}

// Appends one level, encoded as BC1 (kind 1) or BC3 (kind 3), to `dst`. Blocks past the level's edge
// repeat its last row and column.
void encode_bc_level(const MipImage& img, int kind, std::vector<uint8_t>& dst) {
  for (uint32_t by = 0; by < img.height; by += 4) {
    for (uint32_t bx = 0; bx < img.width; bx += 4) {
      uint8_t px[16][4];
      bool transparent = false;
      for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t sx = std::min(bx + x, img.width - 1), sy = std::min(by + y, img.height - 1);
          std::memcpy(px[y * 4 + x], &img.rgba[(size_t(sy) * img.width + sx) * 4], 4);
          transparent |= px[y * 4 + x][3] < 128;
        }
      }
      uint8_t block[16];
      if (kind == 1) {
        encode_bc_colour(px, transparent, block);
        dst.insert(dst.end(), block, block + 8);
      } else {
        encode_bc3_alpha(px, block);
        encode_bc_colour(px, false, block + 8);
        dst.insert(dst.end(), block, block + 16);
      }
    }
  }
}

// Whether hd_mips is on (switch_menu.ini, default on; read once).
bool hd_mips_enabled() noexcept {
#if defined(__SWITCH__)
  static const bool on = sfaSwitchMenuIniValue == nullptr || sfaSwitchMenuIniValue("hd_mips", 1) != 0;
  return on;
#else
  return true;
#endif
}

// Gives a single-level BC1, BC3 or RGBA8 replacement of 64 px or more its full mip chain (see above).
// False (texture unchanged) for other formats, textures that already have levels, small ones, or data
// shorter than level 0 needs.
bool generate_mips(gfx::ConvertedTexture& tex) noexcept {
  if (!hd_mips_enabled() || tex.mips != 1 || std::max(tex.width, tex.height) < 64) {
    return false;
  }
  const int kind = bc_kind(tex.format);
  const bool rgba8 = tex.format == wgpu::TextureFormat::RGBA8Unorm || tex.format == wgpu::TextureFormat::RGBA8UnormSrgb;
  if (kind != 1 && kind != 3 && !rgba8) {
    return false;
  }
  const uint64_t level0 = gfx::calc_texture_size(tex.format, tex.width, tex.height, 1);
  if (level0 == 0 || level0 > tex.data.size()) {
    return false;
  }
  // Level 0 as RGBA8.
  MipImage img;
  img.width = tex.width;
  img.height = tex.height;
  img.rgba.resize(size_t(tex.width) * tex.height * 4);
  if (rgba8) {
    std::memcpy(img.rgba.data(), tex.data.data(), img.rgba.size());
  } else {
    const uint8_t* src = tex.data.data();
    const uint32_t blockBytes = kind == 1 ? 8 : 16;
    for (uint32_t by = 0; by < tex.height; by += 4) {
      for (uint32_t bx = 0; bx < tex.width; bx += 4) {
        uint8_t texels[16][4];
        decode_bc_block(src, kind, texels);
        src += blockBytes;
        for (uint32_t y = 0; y < 4 && by + y < tex.height; ++y)
          for (uint32_t x = 0; x < 4 && bx + x < tex.width; ++x)
            std::memcpy(&img.rgba[((by + y) * size_t(tex.width) + bx + x) * 4], texels[y * 4 + x], 4);
      }
    }
  }
  // Alpha-test coverage of level 0, kept in the levels below for cut-out textures: some transparency and
  // under 10% of texels between alpha 16 and 240. Soft alpha (smoke, glows) keeps the plain box filter.
  size_t transparentTexels = 0, softTexels = 0;
  for (size_t i = 3; i < img.rgba.size(); i += 4) {
    transparentTexels += img.rgba[i] < 255;
    softTexels += img.rgba[i] > 16 && img.rgba[i] < 240;
  }
  const size_t texelCount = img.rgba.size() / 4;
  const bool cutout = transparentTexels > 0 && softTexels * 10 < texelCount;
  const double coverage = cutout ? double(alpha_coverage(img.rgba, 1.f)) / double(texelCount) : 1.0;

  std::vector<uint8_t> out(tex.data.data(), tex.data.data() + level0);
  uint32_t mips = 1;
  while (img.width > 1 || img.height > 1) {
    img = downsample_rgba8(img); // from the unscaled level above
    MipImage level = img;
    if (cutout) {
      preserve_alpha_coverage(level.rgba, coverage);
    }
    if (rgba8) {
      out.insert(out.end(), level.rgba.begin(), level.rgba.end());
    } else {
      encode_bc_level(level, kind, out);
    }
    ++mips;
  }
  if (gfx::calc_texture_size(tex.format, tex.width, tex.height, mips) != out.size()) {
    return false; // (the sizes disagree: keep the single level)
  }
  ByteBuffer data{out.size()};
  std::memcpy(data.data(), out.data(), out.size());
  tex.data = std::move(data);
  tex.mips = mips;
  return true;
}

constexpr bool is_unsupported_texture_format(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
  case wgpu::TextureFormat::BC4RUnorm:
  case wgpu::TextureFormat::BC4RSnorm:
  case wgpu::TextureFormat::BC5RGUnorm:
  case wgpu::TextureFormat::BC5RGSnorm:
  case wgpu::TextureFormat::BC6HRGBUfloat:
  case wgpu::TextureFormat::BC6HRGBFloat:
  case wgpu::TextureFormat::BC7RGBAUnorm:
  case wgpu::TextureFormat::BC7RGBAUnormSrgb:
    return !webgpu::g_bcTexturesSupported;
  case wgpu::TextureFormat::ASTC4x4Unorm:
  case wgpu::TextureFormat::ASTC4x4UnormSrgb:
  case wgpu::TextureFormat::ASTC5x4Unorm:
  case wgpu::TextureFormat::ASTC5x4UnormSrgb:
  case wgpu::TextureFormat::ASTC5x5Unorm:
  case wgpu::TextureFormat::ASTC5x5UnormSrgb:
  case wgpu::TextureFormat::ASTC6x5Unorm:
  case wgpu::TextureFormat::ASTC6x5UnormSrgb:
  case wgpu::TextureFormat::ASTC6x6Unorm:
  case wgpu::TextureFormat::ASTC6x6UnormSrgb:
  case wgpu::TextureFormat::ASTC8x5Unorm:
  case wgpu::TextureFormat::ASTC8x5UnormSrgb:
  case wgpu::TextureFormat::ASTC8x6Unorm:
  case wgpu::TextureFormat::ASTC8x6UnormSrgb:
  case wgpu::TextureFormat::ASTC8x8Unorm:
  case wgpu::TextureFormat::ASTC8x8UnormSrgb:
  case wgpu::TextureFormat::ASTC10x5Unorm:
  case wgpu::TextureFormat::ASTC10x5UnormSrgb:
  case wgpu::TextureFormat::ASTC10x6Unorm:
  case wgpu::TextureFormat::ASTC10x6UnormSrgb:
  case wgpu::TextureFormat::ASTC10x8Unorm:
  case wgpu::TextureFormat::ASTC10x8UnormSrgb:
  case wgpu::TextureFormat::ASTC10x10Unorm:
  case wgpu::TextureFormat::ASTC10x10UnormSrgb:
  case wgpu::TextureFormat::ASTC12x10Unorm:
  case wgpu::TextureFormat::ASTC12x10UnormSrgb:
  case wgpu::TextureFormat::ASTC12x12Unorm:
  case wgpu::TextureFormat::ASTC12x12UnormSrgb:
    return !webgpu::g_astcTexturesSupported;
  default:
    return false;
  }
}

bool validate_texture_size(wgpu::TextureFormat format, uint32_t width, uint32_t height,
                           std::string_view label) noexcept {
  if (gfx::is_block_aligned(format, width, height)) {
    return true;
  }

  const auto info = gfx::format_info(format);
  Log.warn(
      "texture_replacement: failed to load texture {} because {}x{} is not aligned to {}x{} texel blocks for "
      "format {}",
      label, width, height, info.blockWidth, info.blockHeight, static_cast<uint32_t>(format));
  return false;
}

struct FileTextureSource {
  const std::filesystem::path& path;
  std::filesystem::path mipPath;

  std::string name() const { return io::fs_path_to_string(path); }
  std::optional<gfx::ConvertedTexture> load_base() { return load_texture_file(path); }
  bool open_mip(uint32_t mipLevel) {
    mipPath = path.parent_path() /
              fmt::format("{}_mip{}{}", io::fs_path_to_string(path.stem()), mipLevel,
                          io::fs_path_to_string(path.extension()));
    std::error_code ec;
    return std::filesystem::is_regular_file(mipPath, ec);
  }
  std::string mip_name() const { return io::fs_path_to_string(mipPath); }
  std::optional<gfx::ConvertedTexture> load_mip() { return load_texture_file(mipPath); }
};

bool guarded_virtual_read(const std::shared_ptr<VirtualReadState>& state, const VirtualFileSource& source,
                          const char* path, std::vector<uint8_t>& outBytes) {
  if (!state || source.read == nullptr) {
    return false;
  }

  {
    std::unique_lock lock{state->mutex};
    state->cv.wait(lock, [&] { return state->cancelled || state->inFlight == 0; });
    if (state->cancelled) {
      return false;
    }
    state->inFlight = 1;
  }

  bool read = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
#endif
    read = source.read(source.userData, path, outBytes);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  } catch (...) { Log.warn("texture_replacement: virtual read callback threw for {}", path); }
#endif
  bool cancelled = false;
  {
    std::lock_guard lock{state->mutex};
    cancelled = state->cancelled;
    state->inFlight = 0;
  }
  state->cv.notify_all();
  return read && !cancelled;
}

std::string derive_virtual_mip_name(std::string_view path, uint32_t mipLevel) {
  const size_t slash = path.rfind('/');
  const size_t nameStart = slash == std::string_view::npos ? 0 : slash + 1;
  size_t dot = path.rfind('.');
  if (dot == std::string_view::npos || dot < nameStart) {
    dot = path.size();
  }
  return fmt::format("{}_mip{}{}", path.substr(0, dot), mipLevel, path.substr(dot));
}

struct VirtualTextureSource {
  std::string_view path;
  VirtualFileSource source;
  std::shared_ptr<VirtualReadState> readState;
  std::vector<uint8_t> bytes;
  std::string mipPath;

  std::optional<gfx::ConvertedTexture> decode() const {
    const ArrayRef data{bytes.data(), bytes.size()};
    const size_t dot = path.rfind('.');
    if (dot != std::string_view::npos && iequals_ascii(path.substr(dot), ".png")) {
      return gfx::png::parse_png_bytes(data);
    }
    return gfx::dds::parse_dds_bytes(data);
  }
  std::string name() const { return std::string{path}; }
  std::optional<gfx::ConvertedTexture> load_base() {
    bytes.clear();
    const auto pathString = std::string{path};
    if (!guarded_virtual_read(readState, source, pathString.c_str(), bytes)) {
      return std::nullopt;
    }
    return decode();
  }
  bool open_mip(uint32_t mipLevel) {
    mipPath = derive_virtual_mip_name(path, mipLevel);
    bytes.clear();
    return guarded_virtual_read(readState, source, mipPath.c_str(), bytes);
  }
  std::string mip_name() const { return mipPath; }
  std::optional<gfx::ConvertedTexture> load_mip() { return decode(); }
};

template <typename Source>
std::optional<gfx::ConvertedTexture> load_encoded_replacement(Source&& src) noexcept {
  auto base = src.load_base();
  if (!base.has_value()) {
    Log.warn("texture_replacement: failed to load texture {}", src.name());
    return std::nullopt;
  }
  if (is_unsupported_texture_format(base->format)) {
    Log.warn("texture_replacement: failed to load texture {} due to unsupported format: {}", src.name(),
             static_cast<uint32_t>(base->format));
    return std::nullopt;
  }
  // SFA-NX: a BC1-3 texture whose size is not whole blocks is decoded to RGBA8 (decode_bc_to_rgba8 above);
  // any other unaligned compressed texture is still refused with the warning.
  const wgpu::TextureFormat fileFormat = base->format;
  bool decoded = false;
  if (!gfx::is_block_aligned(base->format, base->width, base->height) && decode_bc_to_rgba8(*base)) {
    decoded = true;
    Log.info("texture_replacement: {} is {}x{}, not whole 4x4 blocks: decoded to RGBA8", src.name(), base->width,
             base->height);
  }
  if (!validate_texture_size(base->format, base->width, base->height, src.name())) {
    return std::nullopt;
  }

  if (base->mips > 1) {
    return base;
  }

  std::vector<gfx::ConvertedTexture> more;
  for (uint32_t mipLevel = 1;; ++mipLevel) {
    if (!src.open_mip(mipLevel)) {
      break;
    }

    auto lvl = src.load_mip();
    // SFA-NX: a decoded base's sidecar mips are decoded the same way, so their format matches it
    if (decoded && lvl.has_value() && lvl->format == fileFormat) {
      decode_bc_to_rgba8(*lvl);
    }
    const uint32_t ew = std::max(base->width >> mipLevel, 1u);
    const uint32_t eh = std::max(base->height >> mipLevel, 1u);
    const bool ok = lvl.has_value() && lvl->format == base->format && lvl->width == ew && lvl->height == eh;
    if (!ok) {
      if (!lvl.has_value()) {
        Log.warn("texture_replacement: could not load mip {}", src.mip_name());
      } else {
        Log.warn("texture_replacement: expected {}x{} for mip {}, got {}x{}", ew, eh, src.mip_name(), lvl->width,
                 lvl->height);
      }

      break;
    }
    // If a sidecar mip file contains mipmaps, keep only the top level mip.
    if (!remove_mipmaps(*lvl)) {
      Log.warn("texture_replacement: could not slice first mip {}", src.mip_name());
      break;
    }
    more.push_back(std::move(*lvl));
  }

  if (more.empty()) {
    // SFA-NX: no levels in the file or beside it: make them (generate_mips above). Counted and timed;
    // logged every 200 textures.
    static std::atomic<uint32_t> s_generated{0};
    static std::atomic<uint64_t> s_generatedUs{0};
    const auto t0 = std::chrono::steady_clock::now();
    if (generate_mips(*base)) {
      const auto us = uint64_t(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
      const uint64_t totalUs = s_generatedUs.fetch_add(us) + us;
      const uint32_t n = s_generated.fetch_add(1) + 1;
      if (n == 1 || n % 200 == 0) {
        Log.info("texture_replacement: mipmaps made for {} textures without them, {:.1f} ms each on average "
                 "(latest {} {}x{}, {} levels)",
                 n, double(totalUs) / 1000.0 / n, src.name(), base->width, base->height, base->mips);
      }
    }
    return base;
  }

  const uint32_t mips = 1u + static_cast<uint32_t>(more.size());
  const uint64_t n = gfx::calc_texture_size(base->format, base->width, base->height, mips);
  if (n == 0) {
    return std::nullopt;
  }

  ByteBuffer blob{n};
  uint8_t* const dst = blob.data();
  uint64_t o = 0;
  const auto append = [&](const ByteBuffer& d) noexcept -> bool {
    if (o + d.size() > n) {
      return false;
    }
    std::memcpy(dst + o, d.data(), d.size());
    o += d.size();
    return true;
  };
  if (!append(base->data)) {
    return std::nullopt;
  }
  for (const auto& mip : more) {
    if (!append(mip.data)) {
      return std::nullopt;
    }
  }
  if (o != n) {
    return std::nullopt;
  }

  return gfx::ConvertedTexture{
      .format = base->format,
      .width = base->width,
      .height = base->height,
      .mips = mips,
      .data = std::move(blob),
  };
}

std::optional<gfx::ConvertedTexture> load_file_replacement(const EntryLoadSnapshot& entry) noexcept {
  return load_encoded_replacement(FileTextureSource{.path = entry.path});
}

std::optional<gfx::ConvertedTexture> load_virtual_replacement(const EntryLoadSnapshot& entry) noexcept {
  return load_encoded_replacement(VirtualTextureSource{
      .path = entry.virtualPath,
      .source = entry.source,
      .readState = entry.virtualReadState,
  });
}

EntryLoadSnapshot snapshot_entry(const ReplacementKey& key, const ReplacementEntry& entry) {
  return EntryLoadSnapshot{
      .key = key,
      .id = entry.id,
      .kind = entry.kind,
      .label = entry.label,
      .path = entry.path,
      .virtualPath = entry.virtualPath,
      .source = entry.source,
      .virtualReadState = entry.virtualReadState,
  };
}

bool is_dds_entry(const EntryLoadSnapshot& entry) {
  const std::string extension = entry.kind == EntryKind::File
                                    ? io::fs_path_to_string(entry.path.extension())
                                    : io::fs_path_to_string(std::filesystem::path{entry.virtualPath}.extension());
  return iequals_ascii(extension, ".dds");
}

std::optional<gfx::dds::MipTail> load_thumbnail(const EntryLoadSnapshot& entry) noexcept {
  if (!is_dds_entry(entry)) {
    return std::nullopt;
  }
  if (entry.kind == EntryKind::File) {
    return gfx::dds::load_dds_mip_tail(entry.path, gx::texture::ReplacementThumbnailDim);
  }
  if (entry.kind != EntryKind::Virtual) {
    return std::nullopt;
  }

  std::vector<uint8_t> bytes;
  if (!guarded_virtual_read(entry.virtualReadState, entry.source, entry.virtualPath.c_str(), bytes)) {
    return std::nullopt;
  }
  return gfx::dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, gx::texture::ReplacementThumbnailDim);
}

void worker_main(std::stop_token token) {
  std::stop_callback notifyOnStop{token, [] { s_jobCv.notify_all(); }};
  while (true) {
    LoadJob job;
    {
      std::unique_lock lock{s_jobMutex};
      s_jobCv.wait(lock, [&] {
        return token.stop_requested() ||
               (!s_workersPaused && (!s_highPriorityJobs.empty() || !s_lowPriorityJobs.empty()));
      });
      if (token.stop_requested()) {
        return;
      }
      if (!s_highPriorityJobs.empty()) {
        job = std::move(s_highPriorityJobs.front());
        s_highPriorityJobs.pop_front();
      } else {
        job = std::move(s_lowPriorityJobs.front());
        s_lowPriorityJobs.pop_front();
      }
    }

    std::optional<gfx::ConvertedTexture> texture;
    bool thumbnailIncludesBase = false;
    bool thumbnailStored = false;
    if (job.tier == Tier::Thumbnail) {
      if (auto thumbnail = load_thumbnail(job.entry); thumbnail.has_value()) {
        texture = std::move(thumbnail->texture);
        thumbnailIncludesBase = thumbnail->includesBase;
        // SFA-NX: the preview is the first mip level of 64 px or less, which for a non-power-of-two texture
        // is often not whole 4x4 blocks (1984x992 gives 62x31) and was refused, so the original texture
        // showed until the full one loaded. A BC1-3 preview like that is decoded to RGBA8 (it is tiny).
        if (!gfx::is_block_aligned(texture->format, texture->width, texture->height)) {
          decode_bc_to_rgba8(*texture);
        }
      }
    } else if (job.entry.kind == EntryKind::File) {
      texture = load_file_replacement(job.entry);
    } else if (job.entry.kind == EntryKind::Virtual) {
      texture = load_virtual_replacement(job.entry);
    }

    if (job.tier == Tier::Thumbnail && texture.has_value() && !is_unsupported_texture_format(texture->format) &&
        validate_texture_size(texture->format, texture->width, texture->height, job.entry.label)) {
      std::lock_guard lock{s_registryMutex};
      if (auto* entry = find_entry_locked(job.entry.key, job.entry.id); entry != nullptr) {
        entry->thumbnail = std::move(texture);
        entry->thumbnailIncludesBase = thumbnailIncludesBase;
        thumbnailStored = true;
      }
    }

    {
      std::lock_guard lock{s_jobMutex};
      if (!token.stop_requested()) {
        if (job.tier == Tier::Thumbnail) {
          s_pendingThumbnailLoads.erase(job.entry.id);
        }
        s_workerCompletions.push_back({
            .tier = job.tier,
            .entry = std::move(job.entry),
            .texture = job.tier == Tier::Full ? std::move(texture) : std::nullopt,
            .thumbnailStored = thumbnailStored,
        });
      }
    }
    s_jobCv.notify_all();
  }
}

void start_worker_pool() {
  if constexpr (!gx::texture::AsyncTextureReplacements) {
    return;
  }
  std::lock_guard lock{s_jobMutex};
  if (!s_workers.empty()) {
    return;
  }
  const uint32_t hardwareThreads = std::max(std::thread::hardware_concurrency(), 1u);
  const uint32_t workerCount =
      s_workerCountOverride != 0 ? s_workerCountOverride : std::clamp(hardwareThreads / 2, 1u, 4u);
  for (uint32_t i = 0; i < workerCount; ++i) {
    s_workers.emplace_back(
        thread::Options{
            .name = fmt::format("Aurora texture worker {}", i),
            .priority = thread::Priority::Low,
        },
        worker_main);
  }
}

void stop_worker_pool() {
  if constexpr (!gx::texture::AsyncTextureReplacements) {
    return;
  }
  {
    std::lock_guard lock{s_jobMutex};
    s_highPriorityJobs.clear();
    s_lowPriorityJobs.clear();
  }
  for (auto& worker : s_workers) {
    worker.request_stop();
  }
  s_jobCv.notify_all();
  for (auto& worker : s_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  {
    std::lock_guard lock{s_jobMutex};
    s_workers.clear();
    s_workerCompletions.clear();
    s_pendingFullLoads.clear();
    s_pendingThumbnailLoads.clear();
    s_requestSequence = 0;
    s_workersPaused = false;
  }
  s_readyPublishes.clear();
}

void queue_full_load(const EntryLoadSnapshot& entry) {
  if constexpr (!gx::texture::AsyncTextureReplacements) {
    return;
  }
  start_worker_pool();
  std::lock_guard lock{s_jobMutex};
  const uint64_t priority = ++s_requestSequence;
  const auto [it, inserted] = s_pendingFullLoads.emplace(entry.id, priority);
  if (!inserted) {
    it->second = priority;
    return;
  }
  s_highPriorityJobs.push_back({.tier = Tier::Full, .entry = entry});
  s_jobCv.notify_one();
}

void queue_thumbnail_load(const EntryLoadSnapshot& entry) {
  if constexpr (!gx::texture::AsyncTextureReplacements || gx::texture::ReplacementThumbnailDim == 0) {
    return;
  }
  if (!is_dds_entry(entry)) {
    return;
  }
  start_worker_pool();
  std::lock_guard lock{s_jobMutex};
  if (!s_pendingThumbnailLoads.insert(entry.id).second) {
    return;
  }
  s_lowPriorityJobs.push_back({.tier = Tier::Thumbnail, .entry = entry});
  s_jobCv.notify_one();
}

void finish_full_load(uint64_t id) {
  std::lock_guard lock{s_jobMutex};
  s_pendingFullLoads.erase(id);
}

uint64_t pending_full_load_count() {
  std::lock_guard lock{s_jobMutex};
  return s_pendingFullLoads.size();
}

uint64_t converted_upload_bytes(const gfx::ConvertedTexture& texture) noexcept {
  const auto info = gfx::format_info(texture.format);
  uint64_t total = 0;
  for (uint32_t mip = 0; mip < texture.mips; ++mip) {
    const uint64_t width = std::max(texture.width >> mip, 1u);
    const uint64_t height = std::max(texture.height >> mip, 1u);
    const uint64_t widthBlocks = (width + info.blockWidth - 1) / info.blockWidth;
    const uint64_t heightBlocks = (height + info.blockHeight - 1) / info.blockHeight;
    const uint64_t bytesPerRow = widthBlocks * info.blockSize;
    const uint64_t alignedBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
    if (heightBlocks != 0 && alignedBytesPerRow > UINT64_MAX / heightBlocks) {
      return UINT64_MAX;
    }
    const uint64_t mipBytes = alignedBytesPerRow * heightBlocks;
    if (mipBytes > UINT64_MAX - total) {
      return UINT64_MAX;
    }
    total += mipBytes;
  }
  return total;
}

bool publish_fits_budget(uint64_t publishedCount, uint64_t publishedBytes, uint64_t nextBytes) noexcept {
  if (publishedCount == 0) {
    return true;
  }
  return nextBytes <= gx::texture::ReplacementPublishBudgetBytes &&
         publishedBytes <= gx::texture::ReplacementPublishBudgetBytes - nextBytes;
}

std::string entry_path_for_log(const ReplacementEntry& entry) {
  return entry.kind == EntryKind::Virtual ? entry.virtualPath : io::fs_path_to_string(entry.path);
}

gfx::TextureHandle create_converted_texture_handle(const EntryLoadSnapshot& entry,
                                                   const gfx::ConvertedTexture& replacement) noexcept {
  const auto label = entry.label.empty() ? fmt::format("TextureReplacement {}", entry.id) : entry.label;
  const wgpu::Extent3D size{
      .width = replacement.width,
      .height = replacement.height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label.c_str(),
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = replacement.format,
      .mipLevelCount = replacement.mips,
      .sampleCount = 1,
  };
  auto texture = webgpu::g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = replacement.format,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = replacement.mips,
  };
  auto textureView = texture.CreateView(&textureViewDescriptor);
  auto handle = std::make_shared<gfx::TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size,
                                                  replacement.format, replacement.mips, gfx::InvalidTextureFormat);
  handle->isReplacement = true;
  aurora::gfx::write_texture(*handle, replacement.data);
  return handle;
}

gfx::TextureHandle create_raw_texture_handle(const ReplacementEntry& entry) noexcept {
  if (entry.bytes.empty() || entry.width == 0 || entry.height == 0 || entry.mipCount == 0) {
    return {};
  }

  const auto label = entry.label.empty() ? fmt::format("{}", entry.id) : entry.label;
  const auto format = gfx::to_wgpu(entry.gxFormat);
  if (is_unsupported_texture_format(format)) {
    Log.warn("texture_replacement: failed to load raw replacement {} due to unsupported format: {}", label,
             static_cast<uint32_t>(format));
    return {};
  }
  if (!validate_texture_size(format, entry.width, entry.height, label)) {
    return {};
  }

  const auto textureLabel = entry.label.empty() ? fmt::format("TextureReplacement {}", entry.id) : entry.label;
  auto handle = gfx::new_static_texture_2d(entry.width, entry.height, entry.mipCount, entry.gxFormat,
                                           {entry.bytes.data(), entry.bytes.size()}, false, textureLabel.c_str());
  if (handle) {
    handle->isReplacement = true;
  }
  return handle;
}

void erase_cache_locked(const ReplacementKey& key) noexcept {
  const auto it = s_cacheByKey.find(key);
  if (it == s_cacheByKey.end()) {
    return;
  }
  s_replacementCacheBytes -= std::min(s_replacementCacheBytes, it->second.bytes);
  s_replacementLru.erase(it->second.lruIt);
  s_cacheByKey.erase(it);
}

void touch_cached_replacement(decltype(s_cacheByKey)::iterator it) noexcept {
  if (it->second.lruIt != s_replacementLru.begin()) {
    s_replacementLru.splice(s_replacementLru.begin(), s_replacementLru, it->second.lruIt);
    it->second.lruIt = s_replacementLru.begin();
  }
}

// SFA-NX: memory for decoded HD textures; the least recently used are dropped beyond it. On Switch it comes
// from switch_menu.ini hd_texture_mb (default 512 MiB, 64 to 1536).
uint64_t replacement_cache_budget() noexcept {
#if defined(__SWITCH__)
  static const uint64_t budget = [] {
    const int mb = sfaSwitchMenuIniValue != nullptr ? sfaSwitchMenuIniValue("hd_texture_mb", 512) : 512;
    return static_cast<uint64_t>(std::clamp(mb, 64, 1536)) << 20;
  }();
  return budget;
#else
  return kReplacementCacheBudgetBytes;
#endif
}

void evict_replacement_cache_if_needed() noexcept {
  while (s_replacementCacheBytes > replacement_cache_budget() && !s_replacementLru.empty()) {
    const auto key = s_replacementLru.back();
    const auto cache = s_cacheByKey.find(key);
    const uint64_t id = cache == s_cacheByKey.end() ? 0 : cache->second.id;
    erase_cache_locked(key);
    if (id != 0) {
      gx::texture::invalidate_replacement(id);
      gx::texture::invalidate_bindings();
    }
  }
}

void cache_replacement_locked(const ReplacementKey& key, uint64_t id, gfx::TextureHandle handle, Tier tier) noexcept {
  erase_cache_locked(key);
  if (!handle) {
    return;
  }
  const uint64_t replacementBytes =
      gfx::calc_texture_size(handle->format, handle->size.width, handle->size.height, handle->mipCount);
  s_replacementLru.push_front(key);
  s_cacheByKey.emplace(key, SelectedCache{
                                .handle = std::move(handle),
                                .id = id,
                                .bytes = replacementBytes,
                                .lruIt = s_replacementLru.begin(),
                                .tier = tier,
                            });
  s_replacementCacheBytes += replacementBytes;
  evict_replacement_cache_if_needed();
}

const ReplacementEntry* select_entry(const std::vector<ReplacementEntry>& entries) noexcept {
  const ReplacementEntry* selected = nullptr;
  for (const auto& entry : entries) {
    if (selected == nullptr || entry.priority > selected->priority ||
        (entry.priority == selected->priority && entry.sequence > selected->sequence)) {
      selected = &entry;
    }
  }
  return selected;
}

const ReplacementEntry* find_selected_entry_locked(const ReplacementKey& key) noexcept {
  const auto it = s_entriesByKey.find(key);
  if (it == s_entriesByKey.end()) {
    return nullptr;
  }
  return select_entry(it->second);
}

ReplacementEntry* find_entry_locked(const ReplacementKey& key, uint64_t id) noexcept {
  const auto it = s_entriesByKey.find(key);
  if (it == s_entriesByKey.end()) {
    return nullptr;
  }
  const auto entry = std::find_if(it->second.begin(), it->second.end(),
                                  [id](const ReplacementEntry& candidate) { return candidate.id == id; });
  return entry == it->second.end() ? nullptr : &*entry;
}

std::optional<ReplacementKey> find_source_replacement_key_locked(const TextureSourceKey& key) noexcept {
  ReplacementKey exactKey{key};
  if (s_entriesByKey.contains(exactKey)) {
    return exactKey;
  }

  if (key.hasTlut) {
    auto tlutWildcard = key;
    tlutWildcard.tlutHash = kReplacementWildcardTlutHash;
    ReplacementKey tlutWildcardKey{tlutWildcard};
    if (s_entriesByKey.contains(tlutWildcardKey)) {
      return tlutWildcardKey;
    }
  }

  auto textureWildcard = key;
  textureWildcard.textureHash = kReplacementWildcardTextureHash;
  ReplacementKey textureWildcardKey{textureWildcard};
  if (s_entriesByKey.contains(textureWildcardKey)) {
    return textureWildcardKey;
  }

  return std::nullopt;
}

gfx::TextureHandle load_entry_handle(const ReplacementKey& key, const ReplacementEntry& entry) noexcept {
  if (s_failedIds.contains(entry.id)) {
    return {};
  }

  gfx::TextureHandle handle;
  if (entry.kind == EntryKind::File || entry.kind == EntryKind::Virtual) {
    const auto snapshot = snapshot_entry(key, entry);
    const auto replacement =
        entry.kind == EntryKind::File ? load_file_replacement(snapshot) : load_virtual_replacement(snapshot);
    if (!replacement.has_value()) {
      s_failedIds.insert(entry.id);
      return {};
    }
    handle = create_converted_texture_handle(snapshot, *replacement);
  } else {
    handle = create_raw_texture_handle(entry);
    if (!handle) {
      s_failedIds.insert(entry.id);
      return {};
    }
  }
  return handle;
}

std::optional<gfx::texture_replacement::ReplacementResult>
find_replacement_for_key_locked(const ReplacementKey& key) noexcept {
  const auto* entry = find_selected_entry_locked(key);
  if (entry == nullptr) {
    return std::nullopt;
  }

  if (const auto cache = s_cacheByKey.find(key); cache != s_cacheByKey.end() && cache->second.id == entry->id) {
    touch_cached_replacement(cache);
    if (cache->second.tier == Tier::Thumbnail && !s_failedIds.contains(entry->id)) {
      queue_full_load(snapshot_entry(key, *entry));
    }
    return gfx::texture_replacement::ReplacementResult{.handle = cache->second.handle, .id = entry->id};
  }

  erase_cache_locked(key);
  if constexpr (gx::texture::AsyncTextureReplacements) {
    // Raw entry data is borrowed, so they must take the synchronous path.
    if (entry->kind != EntryKind::Raw) {
      gfx::TextureHandle thumbnailHandle;
      Tier tier = Tier::Thumbnail;
      if (entry->thumbnail.has_value() && !is_unsupported_texture_format(entry->thumbnail->format) &&
          validate_texture_size(entry->thumbnail->format, entry->thumbnail->width, entry->thumbnail->height,
                                entry->label)) {
        const auto snapshot = snapshot_entry(key, *entry);
        thumbnailHandle = create_converted_texture_handle(snapshot, *entry->thumbnail);
        tier = entry->thumbnailIncludesBase ? Tier::Full : Tier::Thumbnail;
        cache_replacement_locked(key, entry->id, thumbnailHandle, tier);
      }
      if (tier != Tier::Full && !s_failedIds.contains(entry->id)) {
        queue_full_load(snapshot_entry(key, *entry));
      }
      return gfx::texture_replacement::ReplacementResult{.handle = std::move(thumbnailHandle), .id = entry->id};
    }
  }

  auto handle = load_entry_handle(key, *entry);
  if (!handle) {
    return gfx::texture_replacement::ReplacementResult{.id = entry->id};
  }
  cache_replacement_locked(key, entry->id, handle, Tier::Full);
  return gfx::texture_replacement::ReplacementResult{.handle = std::move(handle), .id = entry->id};
}

bool dump_editable_texture_dds(const TextureSourceKey& key, const GXTexObj_& obj) noexcept {
  const ArrayRef texData{static_cast<const uint8_t*>(obj.data), UINT32_MAX};
  const uint32_t texWidth = obj.width();
  const uint32_t texHeight = obj.height();

  gfx::ConvertedTexture pixels;
  if (gx::is_palette_format(obj.format())) {
    const GXTlutObj_* tlut = get_loaded_tlut(obj);
    if (tlut == nullptr) {
      return false;
    }
    pixels = gfx::convert_texture_palette(obj.format(), texWidth, texHeight, 1, texData, tlut->format, tlut->numEntries,
                                          tlut_bytes(*tlut));
  } else {
    pixels = gfx::convert_texture(obj.format(), texWidth, texHeight, 1, texData);
  }

  const uint64_t rgbaBytes = gfx::calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, texWidth, texHeight, 1);
  if (pixels.data.empty() || pixels.format != wgpu::TextureFormat::RGBA8Unorm || pixels.data.size() != rgbaBytes) {
    return false;
  }

  const auto dumpRoot = io::fs_path_from_string(g_config.cachePath) / "texture_dumps";
  const auto path = dumpRoot / format_replacement_filename(key);
  return gfx::dds::write_rgba8_dds(path, texWidth, texHeight, pixels.data);
}

bool report_missing_key(const TextureSourceKey& key, const GXTexObj_& obj) noexcept {
  if (!s_reportedMisses.insert(key).second) {
    return false;
  }

  Log.warn("texture_replacement: missing runtime key {}", format_source_key_for_log(key));

  size_t loggedCandidates = 0;
  size_t omittedCandidates = 0;
  for (const auto& [replacementKey, entries] : s_entriesByKey) {
    const auto* candidate = std::get_if<TextureSourceKey>(&replacementKey);
    if (candidate == nullptr || candidate->format != key.format || candidate->hasTlut != key.hasTlut) {
      continue;
    }

    const bool sameDimensions = candidate->width == key.width && candidate->height == key.height;
    const bool sameTextureHash = candidate->textureHash == key.textureHash;
    const bool sameWidth = candidate->width == key.width;
    if (!sameDimensions && !sameTextureHash && !sameWidth) {
      continue;
    }

    std::string_view reason = "same width/format";
    if (sameDimensions && sameTextureHash) {
      reason = "same texture/dimensions";
    } else if (sameDimensions) {
      reason = "same dimensions";
    } else if (sameTextureHash) {
      reason = "same texture hash";
    }

    const auto* selected = select_entry(entries);
    if (loggedCandidates < 8) {
      Log.warn("texture_replacement: candidate ({}) {} path={}", reason, format_source_key_for_log(*candidate),
               selected != nullptr ? entry_path_for_log(*selected) : std::string{});
      ++loggedCandidates;
    } else {
      ++omittedCandidates;
    }
  }
  if (omittedCandidates != 0) {
    Log.warn("texture_replacement: omitted {} additional candidate(s) for missing key {}", omittedCandidates,
             format_source_key_for_log(key));
  }

  if (g_config.allowTextureDumps) {
    dump_editable_texture_dds(key, obj);
  }
  return true;
}

bool is_source_key(const ReplacementKey& key) noexcept;

void cancel_virtual_entry_locked(const ReplacementEntry& entry,
                                 std::vector<std::shared_ptr<VirtualReadState>>& waitStates) {
  if (!entry.virtualReadState) {
    return;
  }
  {
    std::lock_guard lock{entry.virtualReadState->mutex};
    entry.virtualReadState->cancelled = true;
  }
  entry.virtualReadState->cv.notify_all();
  waitStates.push_back(entry.virtualReadState);
}

void wait_for_virtual_reads(const std::vector<std::shared_ptr<VirtualReadState>>& states) {
  for (const auto& state : states) {
    std::unique_lock lock{state->mutex};
    state->cv.wait(lock, [&] { return state->inFlight == 0; });
  }
}

void cancel_queued_jobs(uint64_t id) {
  std::lock_guard lock{s_jobMutex};
  const auto removeId = [id](const LoadJob& job) { return job.entry.id == id; };
  const auto oldHighSize = s_highPriorityJobs.size();
  const auto oldLowSize = s_lowPriorityJobs.size();
  s_highPriorityJobs.erase(std::remove_if(s_highPriorityJobs.begin(), s_highPriorityJobs.end(), removeId),
                           s_highPriorityJobs.end());
  s_lowPriorityJobs.erase(std::remove_if(s_lowPriorityJobs.begin(), s_lowPriorityJobs.end(), removeId),
                          s_lowPriorityJobs.end());
  if (s_highPriorityJobs.size() != oldHighSize) {
    s_pendingFullLoads.erase(id);
  }
  if (s_lowPriorityJobs.size() != oldLowSize) {
    s_pendingThumbnailLoads.erase(id);
  }
}

bool unregister_replacement_locked(const ReplacementRegistration& registration,
                                   std::vector<std::shared_ptr<VirtualReadState>>& waitStates) {
  const auto it = s_entriesByKey.find(registration.key);
  if (it == s_entriesByKey.end()) {
    return false;
  }

  auto& entries = it->second;
  const auto entry = std::find_if(entries.begin(), entries.end(),
                                  [&](const ReplacementEntry& candidate) { return candidate.id == registration.id; });
  if (entry == entries.end()) {
    return false;
  }
  cancel_virtual_entry_locked(*entry, waitStates);
  cancel_queued_jobs(entry->id);
  if (is_source_key(registration.key)) {
    --s_sourceEntryCount;
  }
  s_failedIds.erase(registration.id);
  erase_cache_locked(registration.key);
  entries.erase(entry);
  if (entries.empty()) {
    s_entriesByKey.erase(it);
  }
  return true;
}

void clear_replacement_runtime_state_locked(std::vector<std::shared_ptr<VirtualReadState>>& waitStates) noexcept {
  for (const auto& [_, entries] : s_entriesByKey) {
    for (const auto& entry : entries) {
      cancel_virtual_entry_locked(entry, waitStates);
      cancel_queued_jobs(entry.id);
    }
  }
  s_entriesByKey.clear();
  s_cacheByKey.clear();
  s_failedIds.clear();
  s_reportedMisses.clear();
  s_replacementLru.clear();
  s_replacementCacheBytes = 0;
  s_sourceEntryCount = 0;
}

bool is_source_key(const ReplacementKey& key) noexcept { return std::holds_alternative<TextureSourceKey>(key); }

ReplacementRegistration register_file_replacement(TextureSourceKey key, std::filesystem::path path,
                                                  ReplacementOptions options) {
  std::lock_guard lk(s_registryMutex);
  ReplacementKey replacementKey{key};
  ReplacementRegistration registration{
      .id = s_nextRegistrationId++,
      .key = replacementKey,
  };

  auto& entries = s_entriesByKey[replacementKey];
  entries.push_back({
      .id = registration.id,
      .priority = options.priority,
      .sequence = s_nextSequence++,
      .kind = EntryKind::File,
      .label = fmt::format("TextureReplacement {}", io::fs_path_to_string(path.filename())),
      .path = std::move(path),
  });
  ++s_sourceEntryCount;
  const auto snapshot = snapshot_entry(replacementKey, entries.back());
  queue_thumbnail_load(snapshot);
  erase_cache_locked(replacementKey);
  gx::clear_static_texture_cache();
  return registration;
}
} // namespace

std::optional<TextureSourceKey> parse_replacement_filename(std::string_view filename) noexcept {
  const size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }

  if (!iequals_ascii(filename.substr(dot), ".dds") && !iequals_ascii(filename.substr(dot), ".png")) {
    return std::nullopt;
  }

  const std::string_view stem = filename.substr(0, dot);
  constexpr std::string_view prefix = "tex1_";
  if (!stem.starts_with(prefix)) {
    return std::nullopt;
  }

  std::array<std::string_view, 6> parts{};
  size_t partCount = 0;
  size_t offset = 0;
  bool consumedAll = false;
  while (offset <= stem.size() && partCount < parts.size()) {
    const size_t next = stem.find('_', offset);
    parts[partCount++] = stem.substr(offset, next == std::string_view::npos ? stem.size() - offset : next - offset);
    if (next == std::string_view::npos) {
      consumedAll = true;
      break;
    }
    offset = next + 1;
  }
  if (!consumedAll || partCount < 4 || partCount > 6 || parts[0] != "tex1") {
    return std::nullopt;
  }

  const auto dimensions = parse_dimensions(parts[1]);
  if (!dimensions.has_value()) {
    return std::nullopt;
  }

  size_t index = 2;
  if (parts[index] == "m") {
    ++index;
  }

  size_t remaining = partCount - index;
  if (remaining != 2 && remaining != 3) {
    return std::nullopt;
  }

  uint64_t textureHash = 0;
  if (parts[index] == "$") {
    textureHash = kReplacementWildcardTextureHash;
  } else {
    const auto parsedTex = parse_hex(parts[index]);
    if (!parsedTex.has_value()) {
      return std::nullopt;
    }
    textureHash = *parsedTex;
  }

  auto formatPart = parts[partCount - 1];
  if (formatPart == "arb") {
    formatPart = parts[partCount - 2];
    remaining -= 1;
  }
  const auto format = parse_u32(formatPart);
  if (!format.has_value()) {
    return std::nullopt;
  }

  uint64_t tlutHash = 0;
  const bool hasTlut = remaining == 3;
  if (hasTlut) {
    const std::string_view tlutPart = parts[index + 1];
    if (tlutPart == "$") {
      tlutHash = kReplacementWildcardTlutHash;
    } else {
      const auto parsedTlutHash = parse_hex(tlutPart);
      if (!parsedTlutHash.has_value()) {
        return std::nullopt;
      }
      tlutHash = *parsedTlutHash;
    }
  }

  return TextureSourceKey{
      .textureHash = textureHash,
      .tlutHash = tlutHash,
      .width = dimensions->first,
      .height = dimensions->second,
      .format = *format,
      .hasTlut = hasTlut,
  };
}

ReplacementRegistration register_replacement(ReplacementKey key, RawTextureReplacement replacement,
                                             ReplacementOptions options) {
  if (std::holds_alternative<TexturePointerKey>(key) && std::get<TexturePointerKey>(key).data == nullptr) {
    return {};
  }
  if (replacement.bytes.empty() || replacement.width == 0 || replacement.height == 0 || replacement.mipCount == 0) {
    return {};
  }

  std::lock_guard lk(s_registryMutex);
  ReplacementRegistration registration{
      .id = s_nextRegistrationId++,
      .key = key,
  };

  auto& entries = s_entriesByKey[key];
  entries.push_back({
      .id = registration.id,
      .priority = options.priority,
      .sequence = s_nextSequence++,
      .kind = EntryKind::Raw,
      .bytes = replacement.bytes,
      .width = replacement.width,
      .height = replacement.height,
      .mipCount = replacement.mipCount,
      .gxFormat = replacement.gxFormat,
      .label = std::string(replacement.label),
  });
  if (is_source_key(key)) {
    ++s_sourceEntryCount;
  }
  erase_cache_locked(key);
  gx::clear_static_texture_cache();
  return registration;
}

void unregister_replacement(const ReplacementRegistration& registration) {
  if (registration.id == 0) {
    return;
  }

  std::vector<std::shared_ptr<VirtualReadState>> waitStates;
  bool removed = false;
  {
    std::lock_guard lk(s_registryMutex);
    removed = unregister_replacement_locked(registration, waitStates);
    if (removed) {
      gx::clear_static_texture_cache();
    }
  }
  wait_for_virtual_reads(waitStates);
}

void unregister_replacements(std::span<const ReplacementRegistration> registrations) {
  std::vector<std::shared_ptr<VirtualReadState>> waitStates;
  bool removed = false;
  {
    std::lock_guard lk(s_registryMutex);
    for (const auto& registration : registrations) {
      if (registration.id != 0) {
        removed |= unregister_replacement_locked(registration, waitStates);
      }
    }
    if (removed) {
      gx::clear_static_texture_cache();
    }
  }
  wait_for_virtual_reads(waitStates);
}

void unregister_replacements(const ReplacementGroup& group) { unregister_replacements(group.registrations); }

void unregister_replacements(const ReplacementKey& key) {
  std::vector<std::shared_ptr<VirtualReadState>> waitStates;
  bool removed = false;
  {
    std::lock_guard lk(s_registryMutex);
    const auto it = s_entriesByKey.find(key);
    if (it != s_entriesByKey.end()) {
      if (is_source_key(key)) {
        s_sourceEntryCount -= std::min<uint32_t>(s_sourceEntryCount, static_cast<uint32_t>(it->second.size()));
      }
      for (const auto& entry : it->second) {
        cancel_virtual_entry_locked(entry, waitStates);
        cancel_queued_jobs(entry.id);
        s_failedIds.erase(entry.id);
      }
      erase_cache_locked(key);
      s_entriesByKey.erase(it);
      gx::clear_static_texture_cache();
      removed = true;
    }
  }
  if (removed) {
    wait_for_virtual_reads(waitStates);
  }
}

void clear_replacements() {
  std::vector<std::shared_ptr<VirtualReadState>> waitStates;
  {
    std::lock_guard lk(s_registryMutex);
    clear_replacement_runtime_state_locked(waitStates);
    gx::clear_static_texture_cache();
  }
  wait_for_virtual_reads(waitStates);
}

ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,
                                                     ReplacementOptions options) {
  if (source.read == nullptr) {
    return {};
  }

  const size_t slash = path.rfind('/');
  const auto filename = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto parsed = parse_replacement_filename(filename);
  if (!parsed.has_value()) {
    return {};
  }

  std::lock_guard lk(s_registryMutex);
  ReplacementKey replacementKey{*parsed};
  ReplacementRegistration registration{
      .id = s_nextRegistrationId++,
      .key = replacementKey,
  };

  auto& entries = s_entriesByKey[replacementKey];
  entries.push_back({
      .id = registration.id,
      .priority = options.priority,
      .sequence = s_nextSequence++,
      .kind = EntryKind::Virtual,
      .label = fmt::format("TextureReplacement {}", filename),
      .virtualPath = std::string{path},
      .source = source,
      .virtualReadState = std::make_shared<VirtualReadState>(),
  });
  ++s_sourceEntryCount;
  const auto snapshot = snapshot_entry(replacementKey, entries.back());
  queue_thumbnail_load(snapshot);
  erase_cache_locked(replacementKey);
  gx::clear_static_texture_cache();
  return registration;
}

ReplacementGroup load_replacement_directory(const std::filesystem::path& root, ReplacementOptions options) {
  ReplacementGroup group;
  if (root.empty() || !io::create_directories(root)) {
    return group;
  }

  const auto dumpRoot = io::fs_path_from_string(g_config.cachePath) / "texture_dumps";
  if (g_config.allowTextureDumps && !io::create_directories(dumpRoot)) {
    return group;
  }

  std::vector<ReplacementCandidate> candidates;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(
           root,
           std::filesystem::directory_options::skip_permission_denied |
               std::filesystem::directory_options::follow_directory_symlink,
           ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }

    if (!it->is_regular_file()) {
      continue;
    }

    const auto& path = it->path();
    if (is_relative_to(path, dumpRoot)) {
      continue;
    }

    const auto extension = io::fs_path_to_string(path.extension());
    if (!iequals_ascii(extension, ".dds") && !iequals_ascii(extension, ".png")) {
      continue;
    }

    if (is_sidecar_mip(io::fs_path_to_string(path.stem()))) {
      continue;
    }

    // SFA-NX: the caller's filter (ReplacementOptions::accept) may skip files by name.
    if (options.accept != nullptr && !options.accept(io::fs_path_to_string(path.filename()))) {
      continue;
    }

    candidates.push_back({
        .path = path,
        .components = path_components(root, path),
    });
  }

  std::sort(candidates.begin(), candidates.end(), compare_replacement_candidates);

  absl::flat_hash_set<TextureSourceKey, SourceKeyHash> registeredKeys;
  for (const auto& candidate : candidates) {
    const auto parsed = parse_replacement_filename(io::fs_path_to_string(candidate.path.filename()));
    if (!parsed.has_value() || registeredKeys.contains(*parsed)) {
      continue;
    }
    registeredKeys.insert(*parsed);
    group.registrations.push_back(register_file_replacement(*parsed, candidate.path, options));
  }

  Log.info("Loaded {} texture replacement registrations from {}", group.registrations.size(),
           io::fs_path_to_string(root));
  return group;
}

void reload_replacement_directory(const std::filesystem::path& root, ReplacementGroup& group,
                                  ReplacementOptions options) {
  unregister_replacements(group);
  group = load_replacement_directory(root, options);
}

bool has_replacement(const GXTexObj* obj, const GXTlutObj* tlut) {
  const auto* obj_ = reinterpret_cast<const GXTexObj_*>(obj);
  if (tlut != nullptr) {
    const auto* tlut_ = reinterpret_cast<const GXTlutObj_*>(tlut);
    return gfx::texture_replacement::has_replacement(*obj_, *tlut_);
  }
  return gfx::texture_replacement::has_replacement(*obj_);
}
} // namespace aurora::texture

namespace aurora::gfx::texture_replacement {
using namespace aurora::texture;

void shutdown() noexcept {
  clear_replacements();
  stop_worker_pool();
}

StreamingStats process_streaming() noexcept {
  if constexpr (!gx::texture::AsyncTextureReplacements) {
    return {};
  }

  std::vector<LoadCompletion> thumbnails;
  {
    std::lock_guard lock{s_jobMutex};
    while (!s_workerCompletions.empty()) {
      auto completion = std::move(s_workerCompletions.front());
      s_workerCompletions.pop_front();
      if (completion.tier == Tier::Thumbnail) {
        thumbnails.push_back(std::move(completion));
      } else {
        s_readyPublishes.push_back(std::move(completion));
      }
    }
  }

  for (auto& completion : thumbnails) {
    if (!completion.thumbnailStored) {
      continue;
    }

    bool invalidate = false;
    {
      std::lock_guard lock{s_registryMutex};
      auto* entry = find_entry_locked(completion.entry.key, completion.entry.id);
      if (entry == nullptr) {
        continue;
      }
      const auto* selected = find_selected_entry_locked(completion.entry.key);
      const auto cache = s_cacheByKey.find(completion.entry.key);
      invalidate = selected != nullptr && selected->id == completion.entry.id &&
                   (cache == s_cacheByKey.end() || cache->second.id != completion.entry.id);
    }
    if (invalidate) {
      gx::texture::invalidate_replacement(completion.entry.id);
      gx::texture::invalidate_bindings();
    }
  }

  {
    std::lock_guard lock{s_jobMutex};
    const auto priority = [](uint64_t id) {
      const auto it = s_pendingFullLoads.find(id);
      return it == s_pendingFullLoads.end() ? 0 : it->second;
    };
    std::stable_sort(s_readyPublishes.begin(), s_readyPublishes.end(),
                     [&](const auto& lhs, const auto& rhs) { return priority(lhs.entry.id) > priority(rhs.entry.id); });
  }

  StreamingStats stats;
  std::vector<LoadCompletion> deferred;
  for (auto& completion : s_readyPublishes) {
    bool selected = false;
    {
      std::lock_guard lock{s_registryMutex};
      const auto* entry = find_selected_entry_locked(completion.entry.key);
      selected = entry != nullptr && entry->id == completion.entry.id;
      if (selected && !completion.texture.has_value()) {
        s_failedIds.insert(completion.entry.id);
      }
    }
    if (!selected || !completion.texture.has_value()) {
      finish_full_load(completion.entry.id);
      continue;
    }

    const uint64_t bytes = converted_upload_bytes(*completion.texture);
    if (!publish_fits_budget(stats.publishes, stats.publishBytes, bytes)) {
      deferred.push_back(std::move(completion));
      continue;
    }

    auto handle = create_converted_texture_handle(completion.entry, *completion.texture);
    bool published = false;
    {
      std::lock_guard lock{s_registryMutex};
      const auto* entry = find_selected_entry_locked(completion.entry.key);
      if (entry != nullptr && entry->id == completion.entry.id) {
        cache_replacement_locked(completion.entry.key, completion.entry.id, handle, Tier::Full);
        s_failedIds.erase(completion.entry.id);
        published = true;
      }
    }
    finish_full_load(completion.entry.id);
    if (!published) {
      continue;
    }

    gx::texture::invalidate_replacement(completion.entry.id);
    gx::texture::invalidate_bindings();
    ++stats.publishes;
    stats.publishBytes += bytes;
  }
  s_readyPublishes = std::move(deferred);
  stats.pendingLoads = pending_full_load_count();
  return stats;
}

std::optional<ReplacementResult> find_source_replacement_locked(const GXTexObj_& obj,
                                                                const TextureSourceKey& sourceKey) noexcept {
  const auto replacementKey = find_source_replacement_key_locked(sourceKey);
  if (!replacementKey.has_value()) {
    const bool alwaysReportMissingKey = false; // Enable for debugging
    if (g_config.allowTextureDumps || alwaysReportMissingKey) {
      report_missing_key(sourceKey, obj);
    }
    return std::nullopt;
  }

  return find_replacement_for_key_locked(*replacementKey);
}

std::optional<ReplacementResult> find_pointer_replacement(const GXTexObj_& obj) noexcept {
  ZoneScoped;
  if (obj.data == nullptr) {
    return std::nullopt;
  }

  std::lock_guard lk(s_registryMutex);
  ReplacementKey pointerKey{TexturePointerKey{.data = obj.data}};
  if (!s_entriesByKey.contains(pointerKey)) {
    return std::nullopt;
  }
  return find_replacement_for_key_locked(pointerKey);
}

std::optional<ReplacementResult> find_source_replacement(const GXTexObj_& obj,
                                                         const TextureSourceKey& sourceKey) noexcept {
  ZoneScoped;
  std::lock_guard lk(s_registryMutex);
  if (s_sourceEntryCount == 0 && !g_config.allowTextureDumps) {
    return std::nullopt;
  }
  return find_source_replacement_locked(obj, sourceKey);
}

bool should_build_source_key() noexcept {
  std::lock_guard lk(s_registryMutex);
  return s_sourceEntryCount != 0 || g_config.allowTextureDumps;
}

bool has_replacement(const GXTexObj_& obj) noexcept {
  std::lock_guard lk(s_registryMutex);
  if (s_entriesByKey.empty()) {
    return false;
  }

  if (obj.data != nullptr) {
    ReplacementKey pointerKey{TexturePointerKey{.data = obj.data}};
    if (s_entriesByKey.contains(pointerKey)) {
      return true;
    }
  }

  if (s_sourceEntryCount == 0) {
    return false;
  }

  return find_source_replacement_key_locked(build_source_key(obj)).has_value();
}

bool has_replacement(const GXTexObj_& obj, const GXTlutObj_& tlut) noexcept {
  std::lock_guard lk(s_registryMutex);
  if (s_entriesByKey.empty()) {
    return false;
  }

  if (obj.data != nullptr) {
    ReplacementKey pointerKey{TexturePointerKey{.data = obj.data}};
    if (s_entriesByKey.contains(pointerKey)) {
      return true;
    }
  }

  if (s_sourceEntryCount == 0) {
    return false;
  }

  return find_source_replacement_key_locked(build_source_key(obj, tlut)).has_value();
}

std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept {
  return format_replacement_filename(build_source_key(obj));
}

std::string build_texture_replacement_name(const TextureSourceKey& sourceKey) noexcept {
  return format_replacement_filename(sourceKey);
}

namespace testing {
void set_workers_paused(bool paused) noexcept {
  {
    std::lock_guard lock{s_jobMutex};
    s_workersPaused = paused;
  }
  s_jobCv.notify_all();
}

void set_worker_count(uint32_t count) noexcept {
  std::lock_guard lock{s_jobMutex};
  if (s_workers.empty()) {
    s_workerCountOverride = count;
  }
}

bool wait_for_completions(uint64_t id, uint32_t count, uint32_t timeoutMs) noexcept {
  std::unique_lock lock{s_jobMutex};
  return s_jobCv.wait_for(lock, std::chrono::milliseconds{timeoutMs}, [id, count] {
    return std::count_if(s_workerCompletions.begin(), s_workerCompletions.end(),
                         [id](const LoadCompletion& completion) { return completion.entry.id == id; }) >= count;
  });
}
} // namespace testing
} // namespace aurora::gfx::texture_replacement
