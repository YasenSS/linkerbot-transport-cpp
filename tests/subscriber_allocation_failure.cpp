#include <linkerbot/can/dispatcher.hpp>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <utility>

namespace {
    // This executable uses no worker threads. Arm only the allocation made by
    // unsubscribe(), after registry setup has completed.
    auto failure_armed() noexcept -> bool & {
        static bool armed = false;
        return armed;
    }
} // namespace

// Replacement allocation functions deliberately use malloc/free to inject a
// failure without recursively invoking operator new.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
auto operator new(std::size_t bytes) -> void * {
    if (std::exchange(failure_armed(), false)) {
        throw std::bad_alloc();
    }
    if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) {
        return memory;
    }
    throw std::bad_alloc();
}

auto operator new[](std::size_t bytes) -> void * {
    return ::operator new(bytes);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}
// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

auto main() -> int {
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;
    Registry registry;
    bool invoked = false;
    const auto id = registry.subscribe([&] { invoked = true; });

    failure_armed() = true;
    bool allocation_failed = false;
    try {
        (void)registry.unsubscribe(id);
    } catch (const std::bad_alloc &) {
        allocation_failed = true;
    }
    failure_armed() = false;
    if (!allocation_failed) {
        return 1;
    }
    // An allocation failure must not remove or cancel the live subscription.
    if (registry.size() != 1) {
        return 2;
    }
    const auto snapshot = registry.snapshot();
    if (snapshot.size() != 1 || !Registry::invoke(*snapshot.front()) || !invoked) {
        return 3;
    }
    if (!registry.unsubscribe(id) || registry.size() != 0 || Registry::invoke(*snapshot.front())) {
        return 4;
    }
    return 0;
}
