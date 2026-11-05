#include <atomic>
#include <chrono>
#include <cstdio>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/pool.hpp>
#include <linkerbot/can/vendor_canfd_transport.hpp>
#include <linkerbot/transport/session.hpp>
#ifdef LINKERBOT_TEST_MODBUS
#include "rtu_test_stream.hpp"
#endif
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
    bool recoverable_allocation(std::size_t bytes) noexcept {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        // MSVC's noexcept empty-container constructors allocate iterator proxies.
        // A proxy failure terminates inside the STL, rather than reaching the
        // production rollback path. Keep this size class out of Debug injection;
        // this also leaves any same-size payload allocation unarmed on MSVC Debug.
        return bytes != sizeof(std::_Container_proxy);
#else
        (void)bytes;
        return true;
#endif
    }
} // namespace

// Replacement allocation functions deliberately use malloc/free to inject a
// failure without recursively invoking operator new.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
auto operator new(std::size_t bytes) -> void * {
    // Debug standard libraries allocate small iterator bookkeeping objects.
    // Pause queue storage, not bookkeeping which may hold a CRT-wide lock.
    if (recoverable_allocation(bytes) && bytes >= sizeof(linkerbot::can::Message) &&
        std::this_thread::get_id() != kMainThread && pause_worker.exchange(false)) {
        allocation_paused = true;
        while (!resume_allocation.load()) {
            std::this_thread::yield();
        }
    }
    if (recoverable_allocation(bytes) && countdown == 0) {
        countdown = -1;
        if (std::exchange(unknown_failure, false)) {
            throw 42;
        }
        throw std::bad_alloc();
    }
    if (recoverable_allocation(bytes) && countdown > 0) {
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
    template <typename Operation> void sweep(const char *name, Operation operation) {
        bool failed = false;
        bool succeeded = false;
        for (int index = 0; index < 100; ++index) {
            (void)std::fprintf(stderr, "%s: allocation %d\n", name, index);
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
namespace {
    auto run_failure_cases() -> int {
        using namespace linkerbot;
        auto factory = [] { return std::make_unique<can::FakeTransport>(); };
        // Initialize singleton cleanup before arming allocation faults.
        transport::Session::drain_deferred_cleanup();
        sweep("route update", [&](int index) {
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
                require(countdown == -1);
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
        sweep("latest replacement admission", [&](int index) {
            auto backend = std::make_unique<can::FakeTransport>();
            auto *raw = backend.get();
            auto session = transport::Session::own(std::move(backend));
            struct ReleaseGate {
                can::FakeTransport *backend;
                ~ReleaseGate() {
                    backend->release_tx_gate();
                }
            } release{raw};
            transport::SessionOptions settings;
            settings.separate_policy_capacity = true;
            settings.fifo_unit = transport::CapacityUnit::kFrames;
            settings.fifo_capacity = 29;
            settings.latest_capacity = 1;
            session->configure(settings);
            can::Message frame;
            frame.id = 0x20;
            const transport::MessageBatch request{{frame}};
            raw->arm_tx_gate();
            transport::SendOptions blocking;
            blocking.endpoint = "blocker";
            auto blocker = session->submit(request, blocking);
            require(raw->wait_until_tx_entered(std::chrono::seconds(2)));
            transport::SendOptions latest;
            latest.endpoint = "hand";
            latest.family = "angles";
            latest.policy = transport::SendPolicy::latest;
            auto old = session->submit(request, latest);
            // Fill a deque block without running its tasks. A replacement must
            // preserve the old slot even if growing queue storage fails.
            transport::SendOptions fifo;
            fifo.endpoint = "arm";
            for (unsigned i = 0; i < 29; ++i) {
                (void)session->submit(request, fifo);
            }
            const auto before = session->health();
            auto signal = session->notification();
            const auto revision = signal->revision();
            bool success = false;
            std::optional<transport::SendReceipt> replacement;
            countdown = index;
            try {
                replacement = session->replace_latest_if_pending(request, latest);
                success = true;
            } catch (const std::bad_alloc &) {
                require(countdown == -1);
            }
            countdown = -1;
            const auto after = session->health();
            require(after.pending_fifo == before.pending_fifo);
            require(after.pending_latest == before.pending_latest);
            require(after.accepted == before.accepted + (success ? 1 : 0));
            require(after.superseded == before.superseded + (success ? 1 : 0));
            if (success) {
                require(replacement.has_value());
            } else {
                require(old.result().outcome == transport::SendOutcome::pending);
                require(!old.completed());
                require(signal->revision() == revision);
                require(raw->sent().empty());
                replacement = session->replace_latest_if_pending(request, latest);
                require(replacement.has_value());
            }
            require(old.result().outcome == transport::SendOutcome::superseded);
            require(replacement->result().outcome == transport::SendOutcome::pending);
            require(signal->revision() != revision);
            raw->release_tx_gate();
            blocker.wait();
            replacement->wait();
            session->close();
            require(session->health().accepted == before.accepted + 1);
            return success;
        });
        sweep("pool acquire", [&](int index) {
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
                require(countdown == -1);
            }
            countdown = -1;
            // A failed acquire must not leave a physical-channel reservation or
            // opening flag that prevents retrying the exact same key.
            auto retry = pool.acquire(key, factory);
            retry.close();
            return success;
        });
        sweep("managed start", [&](int index) {
            can::ManagedCANBus bus(factory);
            bool success = false;
            countdown = index;
            try {
                bus.start();
                success = true;
            } catch (const std::bad_alloc &) {
                require(countdown == -1);
            }
            countdown = -1;
            bus.stop();
            bus.start();
            require(bus.get_health().connected());
            bus.stop();
            return success;
        });
        sweep("borrowed Session", [&](int index) {
            can::CANDispatcher dispatcher(factory());
            bool success = false;
            countdown = index;
            try {
                auto session = transport::Session::borrow(dispatcher);
                countdown = -1;
                session->close();
                success = true;
            } catch (const std::bad_alloc &) {
                require(countdown == -1);
            }
            countdown = -1;
            auto retry = transport::Session::borrow(dispatcher);
            retry->close();
            return success;
        });
        sweep("shared Session", [&](int index) {
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
                require(countdown == -1);
            }
            countdown = -1;
            auto retry = transport::Session::share(bus);
            retry->close();
            bus->stop();
            return success;
        });
        sweep("failed-start cleanup", [&](int index) {
            bool close_fails = true;
            can::ManagedCANBus bus([&] { return std::make_unique<CloseFailure>(&close_fails); });
            bool success = false;
            countdown = index;
            try {
                bus.start();
                success = true;
            } catch (const std::bad_alloc &) {
                require(countdown == -1);
            }
            countdown = -1;
            close_fails = false;
            bus.stop();
            bus.start();
            require(bus.get_health().connected());
            bus.stop();
            return success;
        });
        (void)std::fputs("receive callback allocation failures\n", stderr);
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
            std::atomic<unsigned> received{0};
            auto session = transport::Session::share(bus);
            auto inbox = session->subscribe_routes({{0x20, false}}, 256);
            bus->subscribe([&](const can::Message &) {
                countdown = -1;
                unknown_failure = false;
                received.fetch_add(1, std::memory_order_release);
            });
            can::Message frame;
            frame.id = 0x20;
            for (unsigned i = 0; i < 100; ++i) {
                raw->inject(frame);
            }
            // Finish the trailing reset for every injected frame before starting
            // lifecycle operations. stop() may otherwise skip the remaining
            // subscribers and leave a fault armed for dispatcher teardown.
            const auto received_until = transport::Clock::now() + std::chrono::seconds(2);
            while (received.load(std::memory_order_acquire) != 100 &&
                   transport::Clock::now() < received_until) {
                std::this_thread::yield();
            }
            require(received.load(std::memory_order_acquire) == 100);
            require(await_closed(session));
            require(!session->health().open);
            require(inbox->health().closed);
            try {
                session->recover(session->health().generation);
                require(false);
            } catch (const can::FaultedError &) {
                require(!session->health().open);
            }
            require(!session->health().last_error.empty());
            session->close();
            bus->stop();
        }
        (void)std::fputs("send diagnostic allocation failures\n", stderr);
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
        (void)std::fputs("send worker terminal failure\n", stderr);
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
                require(dispatcher.get_health().last_error_code == can::ErrorCode::kTransportIo);
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
                require(!session->health().open);
            }
            session->close();
        }
        (void)std::fputs("vendor cleanup ownership allocation failure\n", stderr);
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
        (void)std::fputs("queued send stopped during allocation\n", stderr);
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
                require(bus.get_health().state == can::TransportState::kClosed);
            }
            require(bus.get_health().state == can::TransportState::kClosed);
        }
#ifdef LINKERBOT_TEST_MODBUS
        sweep("RTU encode", [&](int index) {
            const modbus::Request request{1, 16, 0, 123, std::vector<uint16_t>(123, 0x1234)};
            bool success = false;
            countdown = index;
            try {
                const auto bytes = modbus::encode_request(request);
                require(bytes.size() == 255);
                success = true;
            } catch (const std::bad_alloc &) {
                require(countdown == -1);
            }
            countdown = -1;
            const auto retry = modbus::encode_request(request);
            require(modbus::crc16(retry.data(), retry.size()) == 0);
            return success;
        });
        sweep("RTU create", [&](int index) {
            auto wire = std::make_shared<test_support::RtuWire>();
            bool success = false;
            countdown = index;
            try {
                auto session = test_support::make_session(wire);
                countdown = -1;
                session->close();
                success = true;
            } catch (const std::bad_alloc &) {
                require(countdown == -1);
            }
            countdown = -1;
            auto retry = test_support::make_session(std::make_shared<test_support::RtuWire>());
            require(retry->health().open);
            retry->close();
            return success;
        });
#endif
        return 0;
    }

} // namespace

auto main() -> int {
    try {
        return run_failure_cases();
    } catch (const std::exception &error) {
        countdown = -1;
        unknown_failure = false;
        (void)std::fprintf(stderr, "Unexpected fixture exception: %s\n", error.what());
    } catch (...) {
        countdown = -1;
        unknown_failure = false;
        (void)std::fputs("Unexpected nonstandard fixture exception\n", stderr);
    }
    return EXIT_FAILURE;
}
