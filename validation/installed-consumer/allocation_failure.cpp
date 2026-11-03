#include <atomic>
#include <cstddef>
#include <new>

namespace {
    auto fail_next_allocation() -> std::atomic<bool> & {
        static std::atomic<bool> fail{false};
        return fail;
    }
} // namespace

// GNU ld's --wrap symbols and the Itanium operator-new symbol are fixed ABI
// labels. Ordinary C++ identifiers are kept separate from those linker labels.
// Only the first allocation after entering main is forced to fail;
// subsequent allocations use the original allocator, including sanitizer hooks.
extern "C" auto original_allocation(std::size_t bytes) -> void *__asm__("__real__Znwm");
extern "C" auto original_main() -> int __asm__("__real_main");
extern "C" auto failing_allocation(std::size_t bytes) -> void *__asm__("__wrap__Znwm");
extern "C" auto failing_main() -> int __asm__("__wrap_main");

extern "C" auto failing_allocation(std::size_t bytes) -> void * {
    if (fail_next_allocation().exchange(false, std::memory_order_relaxed)) {
        throw std::bad_alloc();
    }
    return original_allocation(bytes);
}

extern "C" auto failing_main() -> int {
    fail_next_allocation().store(true, std::memory_order_relaxed);
    return original_main();
}
