# Foxhollow (Star Fox Adventures) for Nintendo Switch: an .nro built with devkitA64 GCC.
# Game C + Foxhollow's port layer + Aurora (staged with the deko3d seams) + Dawn Null/Tint + deko3d.
# Build with switch/build.sh (see switch/README.md); Dawn and SDL3 come from switch/build-dawn.sh and
# switch/build-sdl.sh. The renderer (switch/deko, switch/uam) is from Melee-NX/KartPadNX/SwitchWaker.
cmake_minimum_required(VERSION 3.25)
# VERSION is the release version: the NRO's metadata and the in-app updater (SFA_NX_VERSION) use it, so
# bump it for each GitHub release.
# 1.0.0: first SFA-NX release; the updater compares release tags against this.
project(sfa-nx LANGUAGES C CXX ASM VERSION 1.0.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(FH_ROOT "${_repo}")

# The heap allocator is dlmalloc 2.8.6 (switch/src/malloc_switch.c defines newlib's malloc entry points,
# so newlib's own malloc objects are never linked; build.sh checks that).

# Dawn, SDL3, Aurora (staged copy) and the deko3d renderer.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Graphics.cmake")

# Foxhollow's other dependencies.
find_package(ZLIB REQUIRED)
set(MUSYX_ENABLE_SDL OFF CACHE BOOL "" FORCE)
add_subdirectory("${FH_ROOT}/extern/musyx" musyx EXCLUDE_FROM_ALL)
find_package(nlohmann_json 3 CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(FH_C_DIALECT_FLAGS -std=gnu11 -fsigned-char -fcommon)
set(FH_NO_WARN_FLAGS -w -Wno-error=incompatible-pointer-types)
set(FH_FORCE_INCLUDE "SHELL:-include ${FH_ROOT}/port/include/foxhollow_compat.h")

# ---- game: the decompiled C (port/compiling.txt), USA 1.0 code as on the desktop build
file(STRINGS "${FH_ROOT}/port/compiling.txt" FH_GAME_SOURCES)
list(FILTER FH_GAME_SOURCES EXCLUDE REGEX "^ *(#|$)")
list(TRANSFORM FH_GAME_SOURCES PREPEND "${FH_ROOT}/game/src/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FH_ROOT}/port/compiling.txt")
add_library(game STATIC ${FH_GAME_SOURCES})
target_include_directories(game PRIVATE "${FH_ROOT}/port/include" "${FH_ROOT}/game/include" "${FH_ROOT}/game")
target_link_libraries(game PRIVATE ZLIB::ZLIB aurora::core musyx)
target_compile_definitions(game PRIVATE BUILD_VERSION=0 VERSION_GSAE01 NDEBUG=1
  # The game's padUpdate() would replace libnx's padUpdate (whole-archive + allow-multiple-definition):
  # the controller check before aurora_main then ran the game's pad code -> "PADRead before PADInit".
  padUpdate=sfaPadUpdate)
target_compile_options(game PRIVATE ${FH_C_DIALECT_FLAGS} ${FH_NO_WARN_FLAGS} ${FH_FORCE_INCLUDE}
  -fno-strict-aliasing -fwrapv
  # The decomp reads ~100 locals before setting them (gcc -Wmaybe-uninitialized). With GCC the stack
  # garbage moved with every codegen change (the float-cast trace build drew spiky geometry everywhere).
  # Zero makes them deterministic.
  -ftrivial-auto-var-init=zero)
# The game code is built with Clang like upstream Foxhollow (switch/cmake/clang-game.sh).
option(SFA_NX_GAME_CLANG "Compile the decompiled game code with Clang" ON)
if(SFA_NX_GAME_CLANG)
  set_target_properties(game PROPERTIES C_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/cmake/clang-game.sh")
endif()
# Build 1347 tried -fno-data-sections -fno-toplevel-reorder (keep globals in source order): models
# turned into spikes on hardware, so it was reverted.
# Diagnostic: log float -> integer casts that overflow (AArch64 saturates where x86/PowerPC wrap);
# the handler is switch/src/fcast_trace_switch.c. Run 2026-11-16: 12 sites, none of them visible.
option(SFA_NX_FCAST_TRACE "Log overflowing float->int casts in the game code" OFF)
if(SFA_NX_FCAST_TRACE)
  target_compile_options(game PRIVATE -fsanitize=float-cast-overflow -fsanitize-recover=float-cast-overflow)
endif()

# ---- port_shims: Foxhollow's platform layer (no mods/hot patching, no crash handler, no websocket
# client on Switch: those are stubbed in switch/src/fh_switch_stubs.cpp)
file(GLOB FH_SHIM_SOURCES "${FH_ROOT}/port/src/*.c")
list(APPEND FH_SHIM_SOURCES
  "${FH_ROOT}/port/src/foxhollow_ui.cpp"
  "${FH_ROOT}/port/src/foxhollow_controls.cpp"
  "${FH_ROOT}/port/src/foxhollow_splash.cpp")
# Splash font embedded from imgui (fetched by Aurora), as on the desktop build.
FetchContent_GetProperties(imgui)
set(FH_SPLASH_FONT "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf")
if(NOT EXISTS "${FH_SPLASH_FONT}")
  message(FATAL_ERROR "Splash font not found at ${FH_SPLASH_FONT}")
endif()
set(FH_SPLASH_FONT_SOURCE "${CMAKE_BINARY_DIR}/generated/foxhollow_splash_font.c")
if(NOT EXISTS "${FH_SPLASH_FONT_SOURCE}")
  file(READ "${FH_SPLASH_FONT}" _font_hex HEX)
  file(SIZE "${FH_SPLASH_FONT}" _font_size)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _font_bytes "${_font_hex}")
  file(WRITE "${FH_SPLASH_FONT_SOURCE}"
    "#include <stddef.h>\nconst unsigned char gFoxhollowSplashFont[] = {${_font_bytes}};\nconst size_t gFoxhollowSplashFontSize = ${_font_size};\n")
endif()
list(APPEND FH_SHIM_SOURCES "${FH_SPLASH_FONT_SOURCE}")
add_library(port_shims STATIC ${FH_SHIM_SOURCES})
target_compile_definitions(port_shims PRIVATE FOXHOLLOW_VERSION="${PROJECT_VERSION}-switch")
target_include_directories(port_shims PRIVATE "${FH_ROOT}/port/src" "${FH_ROOT}/port/include"
  "${FH_ROOT}/game/include" "${FH_ROOT}/game")
target_compile_options(port_shims PRIVATE
  $<$<COMPILE_LANGUAGE:C>:${FH_C_DIALECT_FLAGS}> $<$<COMPILE_LANGUAGE:CXX>:-fsigned-char>)
target_link_libraries(port_shims PRIVATE aurora::core nlohmann_json::nlohmann_json musyx)

# ---- the executable: Switch bootstrap + Foxhollow's main + the shared Switch layer
file(GLOB SWITCH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
  "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
# sqlite_vfs.cpp is compiled as part of the sqlite3 library instead (switch/cmake/Graphics.cmake), which
# calls its sqlite3_os_init.
list(REMOVE_ITEM SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/sqlite_vfs.cpp") # built into sqlite3
add_executable(sfa-nx "${FH_ROOT}/src/main.c" ${SWITCH_SOURCES}
  "${CMAKE_CURRENT_LIST_DIR}/deko/host/deko_host_switch.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/deko/host/gpu_submit_diagnostics.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/deko/host/posix_stubs_switch.c")
# src/main.c's main becomes aurora_main (aurora/main.h); switch/src/main_sfa.cpp calls it. aurora::main
# (its one-line main) is not linked.
target_include_directories(sfa-nx PRIVATE "${FH_ROOT}/port/include" "${CMAKE_CURRENT_LIST_DIR}/src")
target_compile_options(sfa-nx PRIVATE $<$<COMPILE_LANGUAGE:C>:${FH_C_DIALECT_FLAGS}>)
# The game links whole-archive: its objects are only reached through function tables and the DLL system,
# so the linker would otherwise drop them. libcurl is for the in-app updater.
target_link_libraries(sfa-nx PRIVATE
  -Wl,--whole-archive game -Wl,--no-whole-archive
  port_shims musyx ZLIB::ZLIB
  aurora::core aurora::gx aurora::vi aurora::os aurora::pad aurora::dvd aurora::card
  aurora::mtx aurora::si aurora::gd aurora::ms aurora::thp
  nlohmann_json::nlohmann_json "${DEVKITPRO}/portlibs/switch/lib/libcurl.a")
target_compile_definitions(sfa-nx PRIVATE SFA_NX_VERSION="${PROJECT_VERSION}" CURL_STATICLIB)
add_dependencies(sfa-nx game)

# Threads get 8 MB stacks and exit tears down cleanly (switch/src/clang_tls_switch.c).
target_link_options(sfa-nx PRIVATE
  "LINKER:--wrap=pthread_create" "LINKER:--wrap=pthread_join" "LINKER:--wrap=pthread_detach"
  "LINKER:--wrap=exit" "LINKER:--wrap=_exit" "LINKER:--wrap=abort")
# PADRead wrapper (switch/src/input_trace_switch.c): input diagnostics, and where the - menu filters the pads.
target_link_options(sfa-nx PRIVATE "LINKER:--wrap=PADRead")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/input_trace_switch.c"
  "${CMAKE_CURRENT_LIST_DIR}/src/shader_tour_switch.c" PROPERTIES
  INCLUDE_DIRECTORIES "${FH_ROOT}/game/include;${FH_ROOT}/game"
  COMPILE_DEFINITIONS "VERSION_GSAE01;BUILD_VERSION=0"
  COMPILE_OPTIONS "-include;${FH_ROOT}/port/include/foxhollow_compat.h;-w")
# The native renderer replaces every wgpu entry point in wrap-symbols.txt (switch/deko); the Null
# device's originals stay reachable as __real_<name>.
target_link_options(sfa-nx PRIVATE
  "LINKER:--wrap=nvGpuChannelAppendEntry" "LINKER:--wrap=nvGpuChannelKickoff")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/deko/wrap-symbols.txt" _deko_wraps)
foreach(_symbol IN LISTS _deko_wraps)
  target_link_options(sfa-nx PRIVATE "LINKER:--wrap=${_symbol}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/deko/wrap-symbols.txt")
# --allow-multiple-definition: the game defines a few names libraries also define (its own float trig);
# build.sh fails the build if any of them replaces a libnx or SDL function.
target_link_options(sfa-nx PRIVATE "LINKER:--gc-sections" "LINKER:--allow-multiple-definition")
# Dawn's archives and deko3d after everything else, in a group (they reference each other).
set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,--start-group ${MELEE_GFX_LINK_LIBS} -lstdc++ -lnx -lm -Wl,--end-group" CACHE STRING "" FORCE)

set_target_properties(sfa-nx PROPERTIES OUTPUT_NAME SFA-NX)
# Name and author shown in the homebrew menu's NRO details (the file and folder stay SFA-NX).
nx_generate_nacp(SFA-NX.nacp NAME "Starfox Adventures" AUTHOR "digdat0" VERSION "${PROJECT_VERSION}")
# RomFS inside the NRO: the shader and pipeline seeds (switch/seed), read in place at startup
# (romfs:/deko/initial_dksh_cache.bin, romfs:/initial_pipeline_cache.db), so players only copy the NRO.
# configure_file re-copies them whenever the seed files change.
set(SFA_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${SFA_ROMFS}/deko")
configure_file("${CMAKE_CURRENT_LIST_DIR}/seed/initial_dksh_cache.bin" "${SFA_ROMFS}/deko/initial_dksh_cache.bin" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/seed/initial_pipeline_cache.db" "${SFA_ROMFS}/initial_pipeline_cache.db" COPYONLY)
# switch/icon.jpg (256x256 JPEG) is not in git (fan art of the game's cover, see .gitignore); without it the
# NRO gets the default homebrew icon.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/icon.jpg")
  nx_create_nro(sfa-nx NACP SFA-NX.nacp ICON "${CMAKE_CURRENT_LIST_DIR}/icon.jpg" ROMFS "${SFA_ROMFS}")
else()
  nx_create_nro(sfa-nx NACP SFA-NX.nacp ROMFS "${SFA_ROMFS}")
endif()
