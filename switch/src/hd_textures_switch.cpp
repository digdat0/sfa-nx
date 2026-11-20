// HD texture packs on Switch. Foxhollow's desktop mod loader (native mod DLLs) cannot run here, so this
// file provides its fhModsInit / fhModsShutdown entry points and only does the texture part: it registers
// replacement textures with Aurora, which swaps them in when the game uploads a texture with a matching hash.
//
// Folders, in priority order (later folders win when two contain the same texture):
//   sdmc:/switch/SFA-NX/mods/hd/             the community pack (GameBanana's GSA folder contents)
//   sdmc:/switch/SFA-NX/mods/<name>/textures  any other packs, in alphabetical order
#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern "C" void pc_log_line(const char* fmt, ...);
extern "C" int sfaSwitchMenuIniValue(const char* key, int def);

namespace {

namespace fs = std::filesystem;

// Registered folders (kept so they can be unregistered at shutdown) and the skip count of the folder being
// loaded.
std::vector<aurora::texture::ReplacementGroup> g_groups;
unsigned g_skipped;

// Texture hashes in the GameBanana pack that Foxhollow's own curated copy of the pack leaves out (fur,
// sky and one Dinosaur Planet texture; 34 files). They are skipped so the Switch version looks like the
// desktop one.
constexpr const char* kSkipped[] = {
    "1695e148ba2cc6f9", "9eacd6ca017679da", "d0e43143c96ed693", "878290f3c6945402", "03691e7df483a532",
    "08a71716e956c396", "1e38dadbceb381f9", "24720a1c21cce6ba", "2d44c1d0adbfc21f", "3bfada4130b49a77",
    "4cd2e3d25cfec504", "50631bbc0627cbf5", "57330f3ea9e3d092", "5a1275c32f48b12b", "5b39a8136bd9bb6c",
    "60de20bca4265a6a", "6336d7aca44965bf", "7414cf0651403711", "7cda5483bf42a85e", "8a69f0d59a4fbedf",
    "93334a30167e06a1", "95cc865706adaa2d", "a6816835cbfb4188", "a8037c56bad7c516", "a8ee0549afd2c8df",
    "af732d5c3bb4ea7b", "b44d49c3ebbbbe63", "b88b70b2d292a4f8", "bb0ae4d0bc1cd5fe", "bd4b75f88808b3f0",
    "c2654573c7af59a0", "cbfa514664e04fa5", "f3a925346e69b5d7", "fc6681e95c5eb5ef",
};

// Aurora's per-file filter: rejects any file whose name contains one of the skipped hashes (any case).
bool accept(std::string_view name) {
    std::string lower(name);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* hash : kSkipped) {
        if (lower.find(hash) != std::string::npos) {
            ++g_skipped;
            return false;
        }
    }
    return true;
}

} // namespace

// Called once at startup by Foxhollow. Does nothing when HD textures are turned off in the - menu
// (switch_menu.ini hd_textures 0; the setting applies at the next start). Logs the count and load time
// of each folder.
extern "C" void fhModsInit(int, char**, const char* userPath) {
    if (userPath == nullptr || sfaSwitchMenuIniValue("hd_textures", 1) == 0) {
        pc_log_line("[sfa-nx] HD textures off");
        return;
    }
    const fs::path app = "sdmc:/switch/SFA-NX";
    std::error_code ec;
    std::vector<fs::path> dirs;
    if (fs::is_directory(app / "mods" / "hd", ec)) {
        dirs.push_back(app / "mods" / "hd");
    }
    std::vector<fs::path> mods;
    for (fs::directory_iterator it(app / "mods", ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path textures = it->path() / "textures";
        if (fs::is_directory(textures, ec)) {
            mods.push_back(textures);
        }
    }
    std::sort(mods.begin(), mods.end());
    dirs.insert(dirs.end(), mods.begin(), mods.end());
    if (dirs.empty()) {
        pc_log_line("[sfa-nx] HD textures: none in %s/mods/hd", app.string().c_str());
        return;
    }
    for (size_t i = 0; i < dirs.size(); ++i) {
        const u64 t0 = armTicksToNs(armGetSystemTick());
        aurora::texture::ReplacementOptions options{};
        // Higher priority wins, so mods/hd (i = 0) is overridden by the extra packs.
        options.priority = static_cast<int32_t>(i + 1);
        options.accept = accept;
        g_skipped = 0;
        auto group = aurora::texture::load_replacement_directory(dirs[i], options);
        pc_log_line("[sfa-nx] HD textures: %zu from %s (%u skipped) in %.0f ms", group.registrations.size(),
                    dirs[i].string().c_str(), g_skipped,
                    static_cast<double>(armTicksToNs(armGetSystemTick()) - t0) / 1e6);
        if (!group.registrations.empty()) {
            g_groups.push_back(std::move(group));
        }
    }
}

// Removes all registered replacements at exit.
extern "C" void fhModsShutdown(void) {
    for (const auto& group : g_groups) {
        aurora::texture::unregister_replacements(group);
    }
    g_groups.clear();
}
