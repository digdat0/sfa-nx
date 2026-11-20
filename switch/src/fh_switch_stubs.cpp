// Small pieces the shared Switch layer (from Melee-NX) expects from its desktop port.
#include <cstdarg>
#include <cstdio>

// perf_switch.cpp / prof_switch.cpp log through melee-pc's line logger: one line to stdout.
extern "C" void pc_log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
}

// Foxhollow mods patch game functions at run time (-fpatchable-function-entry + writable code).
// Switch code pages are read-only to homebrew without JIT mappings, so mods are off on Switch.
extern "C" void fhModsUpdate(void) {}
extern "C" void fhModsDraw(void) {}

// Desktop crash handler (borealis) is replaced on Switch by Atmosphère's crash reports and the
// boot.txt markers; a libnx exception handler can come later.
extern "C" void fhInstallCrashHandler(void) {}

// clang_tls_switch.c flushes Aurora's caches on exit; Foxhollow's Aurora writes them as it goes.
extern "C" __attribute__((weak)) void aurora_flush_caches(void) {}
