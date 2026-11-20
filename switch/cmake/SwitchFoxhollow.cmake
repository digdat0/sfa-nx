# Switch toolchain for Foxhollow: devkitA64 GCC for C, C++ and ASM (no Clang needed).
# Used for Dawn (Null backend + Tint) and for the game. Run through switch/build.sh, which starts
# devkitPro's msys2 cmake with DEVKITPRO set.

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO not set in the environment")
endif()
set(DEVKITPRO "$ENV{DEVKITPRO}")

# devkitPro's toolchain: compilers, libnx/portlibs prefixes, elf2nro/nacptool/uam.
include("${DEVKITPRO}/cmake/Switch.cmake")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Archive through ar-rsp.sh: Dawn's monolithic archive passes thousands of objects, over the 32 KB
# limit of a Windows command line (devkitPro's msys2 runs native .exe tools).
if(CMAKE_HOST_SYSTEM_NAME MATCHES "MSYS|MINGW|CYGWIN|Windows" OR EXISTS "/opt/devkitpro/msys2")
  set(_FH_AR "${CMAKE_CURRENT_LIST_DIR}/ar-rsp.sh")
  foreach(_lang C CXX ASM)
    set(CMAKE_${_lang}_ARCHIVE_CREATE "${_FH_AR} qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_${_lang}_ARCHIVE_APPEND "${_FH_AR} q <TARGET> <LINK_FLAGS> <OBJECTS>")
  endforeach()
endif()

# Every object must be position-independent for an NRO.
set(_FH_COMMON_FLAGS
  "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE")

set(CMAKE_C_FLAGS   "${_FH_COMMON_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_FH_COMMON_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "-march=armv8-a+crc+crypto" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO   "-O2 -g -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "" FORCE)
