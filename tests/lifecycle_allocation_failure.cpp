#include <atomic>
#include <chrono>
#include <cstdio>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/pool.hpp>
#include <linkerbot/can/vendor_canfd_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <thread>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <utility>

namespace {
    // Only the calling thread fails allocations. Background receive/cleanup
    // threads retain their allocator, so each sweep tests one synchronous API.
    // Allocation replacement needs process-wide gates and thread-local faults.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    const auto kMainThread = std::this_thread::get_id();
    std::atomic<bool> pause_worker{false}, allocation_paused{false}, resume_allocation{false};
    thread_local int countdown = -1;
    thread_local bool unknown_failure = false;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

// Replacement allocation functions deliberately use malloc/free to inject a
// failure without recursively invoking operator new.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
auto operator new(std::size_t bytes) -> void * {
    if (std::this_thread::get_id() != kMainThread && pause_worker.exchange(false)) {
        allocation_paused = true;
        while (!resume_allocation.load()) {
            std::this_thread::yield();
        }
    }
    if (countdown == 0) {
        countdown = -1;
        if (std::exchange(unknown_failure, false)) {
            throw 42;
        }
        throw std::bad_alloc();
    }
    if (countdown > 0) {
        --countdown;
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

namespace {
    using namespace linkerbot;
    void require(bool condition) {
        if (!condition) {
            (void)std::fputs("allocation rollback invariant failed\n", stderr);
            std::abort();
        }
    }
    template <typename Operation> void sweep(Operation operation) {
        bool failed = false;
        bool succeeded = false;
        for (int index = 0; index < 100; ++index) {
            if (operation(index)) {
                succeeded = true;
                break;
            }
            failed = true;
        }
        require(failed && succeeded);
    }
    class CloseFailure : public can::FakeTransport {
      public:
        bool *fail;
        explicit CloseFailure(bool *value) : fail(value) {}
        void close() override {
            if (*fail) {
                throw can::TransportIOError("injected close failure");
            }
            can::FakeTransport::close();
        }
    };
    bool await_closed(const std::shared_ptr<transport::Session> &session) {
        const auto until = transport::Clock::now() + std::chrono::seconds(2);
        while (transport::Clock::now() < until) {
            if (!session->health().open) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }
    class DiagnosticError : public can::TransportIOError {
      public:
        mutable unsigned calls = 0;
        unsigned fail_at;
        bool unknown;
        DiagnosticError(unsigned at, bool value)
            : can::TransportIOError("diagnostic"), fail_at(at), unknown(value) {}
        const char *what() const noexcept override {
            if (++calls == fail_at) {
                countdown = 0;
                unknown_failure = unknown;
            }
            return "a deliberately long backend diagnostic that requires allocation";
        }
    };
    class DiagnosticTransport : public can::CANTransport {
      public:
        std::exception_ptr failure;
        bool arm_after_success = false;
        std::optional<can::Message> read() override {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }
        void write(const can::Message &) override {
            if (failure) {
                std::rethrow_exception(failure);
            }
            if (arm_after_success) {
                countdown = 0;
                arm_after_success = false;
            }
        }
        void close() override {}
    };

} // namespace
auto main() -> int {
    using namespace linkerbot;
    auto factory = [] { return std::make_unique<can::FakeTransport>(); };
    // Initialize singleton cleanup before arming allocation faults.
    transport::Session::drain_deferred_cleanup();
    sweep([&](int index) {
        auto session = transport::Session::create(factory);
        const std::vector<transport::RouteKey> old_keys{{0x20, false}};
        const std::vector<transport::RouteKey> new_keys{
            {0x21, false}, {0x22, false}, {0x23, false}};
        auto inbox = session->subscribe_routes(old_keys);
        bool success = false;
        countdown = index;
        try {
            session->update_routes(inbox, new_keys);
            success = true;
        } catch (const std::bad_alloc &) {
        }
        countdown = -1;
        // After a failed update all newly inserted nodes must be gone. The
        // original route remains reserved, and can be updated normally.
        if (!success) {
            auto other = session->subscribe_routes(new_keys);
            require(other != nullptr);
            session->update_routes(inbox, old_keys);
        }
        session->close();
        return success;
    });
    sweep([&](int index) {
        can::SharedCANBusPool pool;
        const can::CANBusKey key{"allocation", "rollback"};
        bool success = false;
        countdown = index;
        try {
            auto lease = pool.acquire(key, factory);
            countdown = -1;
            lease.close();
            success = true;
        } catch (const std::bad_alloc &) {
        }
        countdown = -1;
        // A failed acquire must not leave a physical-channel reservation or
        // opening flag that prevents retrying the exact same key.
        auto retry = pool.acquire(key, factory);
        retry.close();
        return success;
    });
    sweep([&](int index) {
        can::ManagedCANBus bus(factory);
        bool success = false;
        countdown = index;
        try {
            bus.start();
            success = true;
        } catch (const std::bad_alloc &) {
        }
        countdown = -1;
        bus.stop();
        bus.start();
        require(bus.get_health().connected());
        bus.stop();
        return success;
    });
    sweep([&](int index) {
        can::CANDispatcher dispatcher(factory());
        bool success = false;
        countdown = index;
        try {
            auto session = transport::Session::borrow(dispatcher);
            countdown = -1;
            session->close();
            success = true;
        } catch (const std::bad_alloc &) {
        }
        countdown = -1;
        auto retry = transport::Session::borrow(dispatcher);
        retry->close();
        return success;
    });
    sweep([&](int index) {
        auto bus = std::make_shared<can::ManagedCANBus>(factory);
        bus->start();
        bool success = false;
        countdown = index;
        try {
            auto session = transport::Session::share(bus);
            countdown = -1;
            session->close();
            success = true;
        } catch (const std::bad_alloc &) {
        }
        countdown = -1;
        auto retry = transport::Session::share(bus);
        retry->close();
        bus->stop();
        return success;
    });
    sweep([&](int index) {
        bool close_fails = true;
        can::ManagedCANBus bus([&] { return std::make_unique<CloseFailure>(&close_fails); });
        bool success = false;
        countdown = index;
        try {
            bus.start();
            success = true;
        } catch (const std::bad_alloc &) {
        }
        countdown = -1;
        close_fails = false;
        bus.stop();
        bus.start();
        require(bus.get_health().connected());
        bus.stop();
        return success;
    });
    for (bool unknown : {false, true}) {
        auto backend = std::make_unique<can::FakeTransport>();
        auto *raw = backend.get();
        auto bus = std::make_shared<can::ManagedCANBus>(
            [&]() -> std::unique_ptr<can::CANTransport> { return std::move(backend); });
        bus->start();
        // Arm only after dispatcher's snapshot is allocated, just before the
        // Session callback. Reset in the trailing subscriber when no allocation
        // was needed by a particular deque insertion.
        bus->subscribe([=](const can::Message &) {
            countdown = 0;
            unknown_failure = unknown;
        });
        auto session = transport::Session::share(bus);
        auto inbox = session->subscribe_routes({{0x20, false}}, 256);
        bus->subscribe([](const can::Message &) {
            countdown = -1;
            unknown_failure = false;
        });
        can::Message frame;
        frame.id = 0x20;
        for (unsigned i = 0; i < 100; ++i) {
            raw->inject(frame);
        }
        require(await_closed(session));
        require(!session->health().open);
        require(inbox->health().closed);
        try {
            session->recover(session->health().generation);
            require(false);
        } catch (const can::FaultedError &) {
        }
        require(!session->health().last_error.empty());
        session->close();
        bus->stop();
    }
    for (unsigned fail_at : {1U, 2U}) {
        auto backend = std::make_unique<DiagnosticTransport>();
        backend->failure = std::make_exception_ptr(DiagnosticError(fail_at, fail_at == 1));
        auto session = transport::Session::own(std::move(backend));
        can::Message frame;
        frame.id = 0x20;
        auto receipt = session->submit({{frame}, {}, {}});
        try {
            receipt.wait();
            require(false);
        } catch (const DiagnosticError &) {
            require(fail_at == 2);
        } catch (int value) {
            require(fail_at == 1 && value == 42);
        }
        require(receipt.result().current_frame_uncertain);
        session->close();
    }
    {
        auto backend = std::make_unique<DiagnosticTransport>();
        auto *raw = backend.get();
        raw->failure = std::make_exception_ptr(can::TransportIOError(
            "a long prior send diagnostic retained by the physical dispatcher"));
        can::CANDispatcher dispatcher(std::move(backend));
        can::Message frame;
        frame.id = 0x20;
        try {
            dispatcher.send(frame);
        } catch (const can::TransportIOError &) {
        }
        raw->failure = {};
        auto session = transport::Session::borrow(dispatcher);
        raw->arm_after_success = true;
        auto receipt = session->submit({{frame}, {}, {}});
        receipt.wait();
        require(await_closed(session));
        require(!session->health().open);
        require(session->health().last_error == "send worker failed");
        try {
            session->submit({{frame}, {}, {}});
            require(false);
        } catch (const can::ClosedError &) {
        }
        session->close();
    }
    {
        bool close_fails = false;
        const auto close_failure =
            std::make_exception_ptr(can::TransportIOError("injected close failure"));
        bool quarantined = false;
        std::vector<std::shared_ptr<can::VendorCanFdApi>> owners;
        for (unsigned i = 0; i < 64 && !quarantined; ++i) {
            auto api = std::make_shared<can::VendorCanFdApi>();
            api->lifecycle_key = "allocation-cleanup-" + std::to_string(i);
            api->scan_device = [] { return 1; };
            api->open_device = [](unsigned, unsigned) { return 0; };
            api->close_device = [&](unsigned, unsigned) {
                if (close_fails) {
                    std::rethrow_exception(close_failure);
                }
                return 0;
            };
            api->canfd_init = [](unsigned, unsigned, can::CanFDConfig *) { return 0; };
            api->transmit = [](unsigned, unsigned, can::CanFDMsg *, unsigned n, int) {
                return static_cast<int>(n);
            };
            api->receive = [](unsigned, unsigned, can::CanFDMsg *, unsigned, int) { return 0; };
            auto device = std::make_unique<can::VendorCanFdTransport>(
                can::VendorCanFdTransport::Config{}, api);
            close_fails = true;
            countdown = 0;
            device.reset();
            const bool allocation_failed = countdown < 0;
            countdown = -1;
            close_fails = false;
            if (allocation_failed) {
                try {
                    can::vendor_canfd_lifecycle::acquire(api);
                    require(false);
                } catch (const can::FaultedError &) {
                    quarantined = true;
                }
            }
            owners.push_back(api);
        }
        require(quarantined);
        // No hardware handles exist in this fixture. Reset the test registry
        // after proving that unrecorded cleanup ownership blocks reuse.
        can::vendor_canfd_lifecycle::reset_for_test();
    }
    {
        can::ManagedCANBus bus([] { return std::make_unique<DiagnosticTransport>(); });
        bus.start();
        pause_worker = true;
        can::Message frame;
        frame.id = 0x20;
        auto receipt = bus.queue_send(frame);
        const auto until = transport::Clock::now() + std::chrono::seconds(2);
        while (!allocation_paused && transport::Clock::now() < until) {
            std::this_thread::yield();
        }
        require(allocation_paused);
        std::thread stopper([&] { bus.stop(); });
        bool stopping = false;
        while (!stopping && transport::Clock::now() < until) {
            try {
                bus.send(frame);
            } catch (const can::ClosedError &) {
                stopping = true;
            }
        }
        resume_allocation = true;
        stopper.join();
        require(stopping);
        try {
            receipt.wait();
            require(false);
        } catch (const can::ClosedError &) {
        }
        require(bus.get_health().state == can::TransportState::kClosed);
    }
    return 0;
}
