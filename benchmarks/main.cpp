#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

#ifndef LINKERBOT_PERF_COUNT_ALLOCATIONS
#define LINKERBOT_PERF_COUNT_ALLOCATIONS 0
#endif

#if LINKERBOT_PERF_COUNT_ALLOCATIONS
namespace {
    // Global allocation instrumentation belongs only to this executable.
    // Requested bytes are counted, not allocator metadata or resident memory.
    std::atomic<bool> counting{false};
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> allocated_bytes{0};

    void record_allocation(std::size_t bytes) noexcept {
        if (counting.load(std::memory_order_relaxed)) {
            allocations.fetch_add(1, std::memory_order_relaxed);
            allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void *allocate(std::size_t bytes) {
        if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) {
            record_allocation(bytes);
            return memory;
        }
        throw std::bad_alloc();
    }

    void *allocate_aligned(std::size_t bytes, std::size_t alignment) {
        void *memory = nullptr;
        if (posix_memalign(&memory, alignment, bytes == 0 ? alignment : bytes) != 0) {
            throw std::bad_alloc();
        }
        record_allocation(bytes);
        return memory;
    }
} // namespace

void *operator new(std::size_t bytes) {
    return allocate(bytes);
}
void *operator new[](std::size_t bytes) {
    return allocate(bytes);
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
void *operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void *memory, std::align_val_t) noexcept {
    std::free(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
    std::free(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}
#endif

namespace {
    using Clock = std::chrono::steady_clock;
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;

    std::uint64_t process_cpu_ns() {
        timespec time{};
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) {
            throw std::runtime_error("clock_gettime failed");
        }
        return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
               static_cast<std::uint64_t>(time.tv_nsec);
    }

    rusage usage() {
        rusage result{};
        if (getrusage(RUSAGE_SELF, &result) != 0) {
            throw std::runtime_error("getrusage failed");
        }
        return result;
    }

    struct Measurement {
        Clock::time_point wall_start;
        std::uint64_t cpu_start;
        rusage usage_start;
        Measurement()
            : wall_start(Clock::now()), cpu_start(process_cpu_ns()), usage_start(usage()) {
#if LINKERBOT_PERF_COUNT_ALLOCATIONS
            allocations.store(0, std::memory_order_relaxed);
            allocated_bytes.store(0, std::memory_order_relaxed);
            counting.store(true, std::memory_order_relaxed);
#endif
        }
        void print(const char *scenario, std::size_t subjects, std::size_t repetition,
                   std::uint64_t operations, std::uint64_t reads, std::uint64_t checksum) const {
#if LINKERBOT_PERF_COUNT_ALLOCATIONS
            counting.store(false, std::memory_order_relaxed);
#endif
            const auto wall =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall_start)
                    .count();
            const auto cpu = process_cpu_ns() - cpu_start;
            const auto current = usage();
            std::cout << scenario << ',' << subjects << ',' << repetition << ',' << operations
                      << ',' << wall << ',' << cpu << ',';
#if LINKERBOT_PERF_COUNT_ALLOCATIONS
            std::cout << "1," << allocations.load() << ',' << allocated_bytes.load() << ',';
#else
            std::cout << "0,NA,NA,";
#endif
            std::cout << current.ru_nvcsw - usage_start.ru_nvcsw << ','
                      << current.ru_nivcsw - usage_start.ru_nivcsw << ',' << reads << ','
                      << checksum << '\n';
        }
    };

    template <typename Entries> const Entries &entries_of(const Entries &entries) {
        return entries;
    }
    template <typename Entries> const Entries &entries_of(const std::shared_ptr<Entries> &entries) {
        return *entries;
    }

    // Preserve the exact benchmark source across revisions. Prefer the borrowed
    // immutable view when implemented, falling back to the original copy API.
    template <typename RegistryType>
    auto acquire_snapshot(const RegistryType &registry, int) -> decltype(registry.snapshot_view()) {
        return registry.snapshot_view();
    }
    template <typename RegistryType>
    auto acquire_snapshot(const RegistryType &registry, long) -> decltype(registry.snapshot()) {
        return registry.snapshot();
    }
    template <typename RegistryType>
    auto snapshot_path(const RegistryType &registry,
                       int) -> decltype(registry.snapshot_view(),
                                        static_cast<const char *>(nullptr)) {
        return "snapshot_view";
    }
    template <typename RegistryType> const char *snapshot_path(const RegistryType &, long) {
        return "snapshot_copy";
    }

    void snapshot_case(std::size_t subscribers, std::uint64_t iterations, std::size_t repetition) {
        Registry registry;
        std::uint64_t checksum = 0;
        for (std::size_t index = 0; index < subscribers; ++index) {
            registry.subscribe([&checksum] { ++checksum; });
        }
        const auto invoke = [&] {
            const auto snapshot = acquire_snapshot(registry, 0);
            for (const auto &entry : entries_of(snapshot)) {
                Registry::invoke(*entry);
            }
        };
        for (std::size_t index = 0; index < 1000; ++index) {
            invoke();
        }
        checksum = 0;
        const Measurement measurement;
        for (std::uint64_t index = 0; index < iterations; ++index) {
            invoke();
        }
        measurement.print(snapshot_path(registry, 0), subscribers, repetition, iterations, 0,
                          checksum);
        if (checksum != iterations * subscribers) {
            throw std::runtime_error("snapshot callback count mismatch");
        }
    }

    void mutation_case(std::size_t subscribers, std::uint64_t iterations, std::size_t repetition,
                       bool snapshot_after_mutation) {
        Registry registry;
        std::uint64_t checksum = 0;
        for (std::size_t index = 0; index < subscribers; ++index) {
            registry.subscribe([] {});
        }
        const auto mutate = [&] {
            const auto id = registry.subscribe([] {});
            if (snapshot_after_mutation) {
                const auto snapshot = acquire_snapshot(registry, 0);
                checksum += entries_of(snapshot).size();
            }
            if (!registry.unsubscribe(id)) {
                throw std::runtime_error("unsubscribe failed");
            }
            if (snapshot_after_mutation) {
                const auto snapshot = acquire_snapshot(registry, 0);
                checksum += entries_of(snapshot).size();
            }
        };
        for (std::size_t index = 0; index < 1000; ++index) {
            mutate();
        }
        checksum = 0;
        const Measurement measurement;
        for (std::uint64_t index = 0; index < iterations; ++index) {
            mutate();
        }
        measurement.print(snapshot_after_mutation ? "mutation_snapshot" : "mutation", subscribers,
                          repetition, iterations, 0, checksum);
        if (registry.size() != subscribers) {
            throw std::runtime_error("mutation registry size mismatch");
        }
        if (snapshot_after_mutation && checksum != iterations * (2 * subscribers + 1)) {
            throw std::runtime_error("mutation snapshot size mismatch");
        }
    }

    class PassiveTransport final : public linkerbot::can::CANTransport {
      public:
        explicit PassiveTransport(std::shared_ptr<std::atomic<std::uint64_t>> reads)
            : reads_(std::move(reads)) {}
        std::optional<linkerbot::can::Message> read() override {
            reads_->fetch_add(1, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10), [&] { return closed_; });
            return std::nullopt;
        }
        void write(const linkerbot::can::Message &) override {}
        void close() override {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            cv_.notify_all();
        }

      private:
        std::shared_ptr<std::atomic<std::uint64_t>> reads_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool closed_ = false;
    };

    void idle_case(std::size_t sessions, std::chrono::milliseconds idle_duration,
                   std::size_t repetition, bool routed) {
        auto reads = std::make_shared<std::atomic<std::uint64_t>>(0);
        std::vector<std::shared_ptr<linkerbot::transport::Session>> owners;
        std::vector<std::shared_ptr<linkerbot::transport::RouteInbox>> inboxes;
        owners.reserve(sessions);
        inboxes.reserve(sessions);
        for (std::size_t index = 0; index < sessions; ++index) {
            owners.push_back(
                linkerbot::transport::Session::own(std::make_unique<PassiveTransport>(reads)));
            if (routed) {
                inboxes.push_back(owners.back()->subscribe_routes({{0x28, false}}));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto reads_start = reads->load(std::memory_order_relaxed);
        const Measurement measurement;
        std::this_thread::sleep_for(idle_duration);
        measurement.print(routed ? "idle_routed" : "idle_unrouted", sessions, repetition, 0,
                          reads->load(std::memory_order_relaxed) - reads_start, owners.size());
        for (auto &owner : owners) {
            owner->close();
        }
    }

    std::uint64_t parse_positive(const char *text) {
        const std::string value(text);
        if (value.empty() || value.front() == '-') {
            throw std::runtime_error("expected a positive integer");
        }
        std::size_t consumed = 0;
        const auto result = std::stoull(value, &consumed);
        if (result == 0 || consumed != value.size()) {
            throw std::runtime_error("expected a positive integer");
        }
        return result;
    }
} // namespace

int main(int argc, char **argv) {
    try {
        std::uint64_t iterations = 200000;
        std::uint64_t idle_ms = 500;
        std::size_t repetitions = 5;
        for (int index = 1; index < argc; index += 2) {
            const std::string option(argv[index]);
            if (index + 1 >= argc) {
                throw std::runtime_error("each option requires a value");
            }
            const auto value = parse_positive(argv[index + 1]);
            if (option == "--iterations") {
                iterations = value;
            } else if (option == "--idle-ms") {
                idle_ms = value;
            } else if (option == "--repetitions") {
                repetitions = static_cast<std::size_t>(value);
            } else {
                throw std::runtime_error("unknown option: " + option);
            }
        }
        std::cout << "scenario,subjects,repetition,operations,wall_ns,cpu_ns,allocation_tracking,"
                     "allocations,"
                     "allocated_bytes,voluntary_switches,involuntary_switches,transport_reads,"
                     "checksum\n";
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
            for (const std::size_t subscribers : {0U, 1U, 8U, 64U}) {
                snapshot_case(subscribers, iterations, repetition);
                mutation_case(subscribers, iterations / 10 + 1, repetition, false);
                mutation_case(subscribers, iterations / 10 + 1, repetition, true);
            }
            for (const std::size_t sessions : {0U, 1U, 8U, 32U}) {
                idle_case(sessions, std::chrono::milliseconds(idle_ms), repetition, false);
                idle_case(sessions, std::chrono::milliseconds(idle_ms), repetition, true);
            }
        }
    } catch (const std::exception &error) {
#if LINKERBOT_PERF_COUNT_ALLOCATIONS
        counting.store(false, std::memory_order_relaxed);
#endif
        std::cerr << error.what() << '\n';
        return 1;
    }
}
