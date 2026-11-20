// Diagnostic: the game code is built with -fsanitize=float-cast-overflow (CMakeLists.txt,
// SFA_NX_FCAST_TRACE). A float -> integer conversion whose value does not fit the target type is
// undefined in C and gives different results per CPU: AArch64 saturates (negative -> unsigned = 0,
// too large -> max), x86 and the GameCube's PowerPC wrap through a 32-bit integer. The decomp was
// written against the latter, so each such cast is a candidate for Switch-only behaviour. This
// handler logs every source location once ("[fcast] ..." in stdout.log); the conversion itself
// still happens natively.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Data layouts the compiler passes to UBSan handlers (they match compiler-rt's ubsan_handlers.h).
typedef struct {
    const char* file;
    uint32_t line;
    uint32_t column;
} UbsanSourceLocation;

typedef struct {
    uint16_t kind; // 0 integer, 1 float
    uint16_t info; // integer: (bit width log2 << 1) | signed; float: bit width
    char name[1];
} UbsanTypeDescriptor;

typedef struct {
    UbsanSourceLocation loc;
    const UbsanTypeDescriptor* from;
    const UbsanTypeDescriptor* to;
} UbsanFloatCastOverflowData;

// Source locations already reported (each site's data pointer is unique), and the total count.
#define FCAST_MAX_SITES 1024
static const void* sSeen[FCAST_MAX_SITES];
static unsigned sSeenCount;
static unsigned long long sTotal;

// Logs a site the first time it overflows, with the float value that did not fit. `value` carries the
// raw bits of a float or double argument.
static void fcast_report(UbsanFloatCastOverflowData* data, uintptr_t value) {
    ++sTotal;
    for (unsigned i = 0; i < sSeenCount; ++i) {
        if (sSeen[i] == data) {
            return;
        }
    }
    if (sSeenCount >= FCAST_MAX_SITES) {
        return;
    }
    sSeen[sSeenCount++] = data;
    double v = 0.0;
    if (data->from != NULL && data->from->info == 32) {
        float f;
        uint32_t bits = (uint32_t)value;
        memcpy(&f, &bits, sizeof(f));
        v = f;
    } else if (data->from != NULL && data->from->info == 64) {
        uint64_t bits = (uint64_t)value;
        memcpy(&v, &bits, sizeof(v));
    }
    const char* file = data->loc.file != NULL ? data->loc.file : "?";
    const char* slash = strstr(file, "/src/");
    printf("[fcast] #%u %s:%u:%u %s -> %s value %g (call %llu)\n", sSeenCount, slash != NULL ? slash + 5 : file,
           data->loc.line, data->loc.column, data->from != NULL ? data->from->name : "?",
           data->to != NULL ? data->to->name : "?", v, sTotal);
}

// The handler names the compiler calls; the _abort variant is used with -fno-sanitize-recover, and here
// it logs and continues too.
void __ubsan_handle_float_cast_overflow(void* data, uintptr_t value) {
    fcast_report((UbsanFloatCastOverflowData*)data, value);
}

void __ubsan_handle_float_cast_overflow_abort(void* data, uintptr_t value) {
    fcast_report((UbsanFloatCastOverflowData*)data, value);
}
