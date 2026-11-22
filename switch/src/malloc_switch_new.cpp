// Global operator new replacements, so the heap statistics in malloc_switch.c see who allocates.
// libstdc++'s own operator new calls malloc from inside libstdc++, which would make every C++ allocation
// look like it came from operator new. These do the same job (malloc, new_handler loop, bad_alloc) and
// pass their caller's address to the sampler. operator delete stays libstdc++'s (it calls free, which is
// malloc_switch.c's). Aligned new also stays libstdc++'s (it calls aligned_alloc, also ours).
#include <cstdint>
#include <cstdlib>
#include <new>

extern "C" void sfa_heap_note_alloc(std::uintptr_t caller);  // malloc_switch.c
extern "C" void* dlmalloc(std::size_t n);                    // dlmalloc 2.8.6, built into malloc_switch.c
extern "C" void sfa_heap_track(void* p, std::size_t n, std::uintptr_t caller);  // malloc_switch.c leak finder

namespace {
// Allocates n bytes for `caller`; nullptr only when the heap is exhausted and no new_handler helps.
inline void* allocate(std::size_t n, std::uintptr_t caller) noexcept {
  if (n == 0) n = 1;  // operator new must return a unique pointer for 0 bytes
  sfa_heap_note_alloc(caller);
  for (;;) {
    if (void* p = dlmalloc(n)) {
      sfa_heap_track(p, n, caller);  // offered to the leak finder like malloc's blocks
      return p;
    }
    std::new_handler handler = std::get_new_handler();
    if (handler == nullptr) return nullptr;
    handler();
  }
}

// The throwing forms: out of memory crashes on the spot (Atmosphere report) rather than throwing through
// game code that is not built for exceptions.
inline void* allocate_or_trap(std::size_t n, std::uintptr_t caller) noexcept {
  void* p = allocate(n, caller);
  if (p == nullptr) __builtin_trap();
  return p;
}
} // namespace

#define SFA_CALLER reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))

void* operator new(std::size_t n) { return allocate_or_trap(n, SFA_CALLER); }
void* operator new[](std::size_t n) { return allocate_or_trap(n, SFA_CALLER); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate(n, SFA_CALLER); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate(n, SFA_CALLER); }
