#include <catch2/catch_test_macros.hpp>

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/can/pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using linkerbot::can::CANBusKey;
using linkerbot::can::CANBusLease;
using linkerbot::can::CANError;
using linkerbot::can::ClosedError;
using linkerbot::can::ErrorCode;
using linkerbot::can::FakeTransport;
using linkerbot::can::FaultedError;
using linkerbot::can::ManagedCANBus;
using linkerbot::can::Message;
using linkerbot::can::QueueFullError;
using linkerbot::can::SharedCANBusPool;
using linkerbot::can::TransportIOError;
using linkerbot::can::TransportState;
using linkerbot::can::ValidationError;

namespace {

    constexpr auto kTimeout = std::chrono::milliseconds(500);

    auto make_frame(uint32_t id, std::byte first_byte) -> Message {
        Message msg;
        msg.id = id;
        msg.dlc = 1;
        msg.data[0] = std::to_integer<uint8_t>(first_byte);
        return msg;
    }

    template <typename Predicate>
    auto wait_until(Predicate predicate, std::chrono::milliseconds timeout = kTimeout) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    }

    class FrameCollector {
      public:
        auto callback() -> ManagedCANBus::FrameCallback {
            return [this](const Message &msg) {
                const std::scoped_lock lock(mutex_);
                frames_.push_back(msg);
                cv_.notify_all();
            };
        }

        auto wait_for(size_t count, std::chrono::milliseconds timeout) -> bool {
            std::unique_lock lock(mutex_);
            return cv_.wait_for(lock, timeout, [&] { return frames_.size() >= count; });
        }

        [[nodiscard]] auto frames() const -> std::vector<Message> {
            const std::scoped_lock lock(mutex_);
            return frames_;
        }

      private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::vector<Message> frames_;
    };

    class DestructionTrackingFakeTransport final : public FakeTransport {
      public:
        explicit DestructionTrackingFakeTransport(std::shared_ptr<std::atomic<bool>> destroyed)
            : destroyed_(std::move(destroyed)) {}

        ~DestructionTrackingFakeTransport() override {
            destroyed_->store(true, std::memory_order_release);
        }

      private:
        std::shared_ptr<std::atomic<bool>> destroyed_;
    };

} // namespace

TEST_CASE("constructing ManagedCANBus does not call the factory", "[managed_bus]") {
    int factory_calls = 0;
    ManagedCANBus bus([&factory_calls] {
        ++factory_calls;
        return std::make_unique<FakeTransport>();
    });

    REQUIRE(factory_calls == 0);
    const auto health = bus.get_health();
    REQUIRE(health.state == TransportState::kClosed);
    REQUIRE(health.generation == 0);
    REQUIRE_FALSE(health.connected());
}

TEST_CASE("start and stop are idempotent", "[managed_bus]") {
    int factory_calls = 0;
    ManagedCANBus bus([&factory_calls] {
        ++factory_calls;
        return std::make_unique<FakeTransport>();
    });

    bus.start();
    bus.start();
    REQUIRE(factory_calls == 1);
    REQUIRE(bus.get_health().generation == 1);
    REQUIRE(bus.get_health().state == TransportState::kOpen);

    bus.stop();
    bus.stop();
    REQUIRE(bus.get_health().state == TransportState::kClosed);
    REQUIRE(factory_calls == 1);
}

TEST_CASE("concurrent start creates one connection", "[managed_bus]") {
    std::atomic<int> factory_calls{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    ManagedCANBus bus([&factory_calls] {
        ++factory_calls;
        return std::make_unique<FakeTransport>();
    });

    std::vector<std::thread> callers;
    callers.reserve(16);
    for (int i = 0; i < 16; ++i) {
        callers.emplace_back([&] {
            ++ready;
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            bus.start();
        });
    }
    REQUIRE(wait_until([&] { return ready.load() == 16; }));
    go.store(true, std::memory_order_release);
    for (auto &caller : callers) {
        caller.join();
    }

    REQUIRE(factory_calls.load() == 1);
    REQUIRE(bus.get_health().state == TransportState::kOpen);
    bus.stop();
}

TEST_CASE("recover bumps generation and does not replay sent frames", "[managed_bus]") {
    std::vector<std::vector<Message>> sent_by_generation;
    FakeTransport *current = nullptr;
    int factory_calls = 0;
    ManagedCANBus bus([&] {
        ++factory_calls;
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });

    bus.start();
    bus.send(make_frame(0x28, std::byte{0x01}));
    REQUIRE(current != nullptr);
    sent_by_generation.push_back(current->sent());

    bus.recover();
    REQUIRE(factory_calls == 2);
    REQUIRE(bus.get_health().generation == 2);
    REQUIRE(bus.get_health().state == TransportState::kOpen);
    bus.send(make_frame(0x28, std::byte{0x02}));

    REQUIRE(sent_by_generation.size() == 1);
    REQUIRE(sent_by_generation[0].size() == 1);
    REQUIRE(sent_by_generation[0][0].data[0] == 0x01);
    REQUIRE(current->sent().size() == 1);
    REQUIRE(current->sent()[0].data[0] == 0x02);
    bus.stop();
}

TEST_CASE("old-generation callbacks are not delivered after recover", "[managed_bus]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });

    FrameCollector collector;
    bus.subscribe(collector.callback());
    bus.start();
    current->inject(make_frame(0x28, std::byte{0xA1}));
    REQUIRE(collector.wait_for(1, kTimeout));

    bus.recover();
    current->inject(make_frame(0x28, std::byte{0xA2}));
    REQUIRE(collector.wait_for(2, kTimeout));

    auto frames = collector.frames();
    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].data[0] == 0xA1);
    REQUIRE(frames[1].data[0] == 0xA2);
    bus.stop();
}

TEST_CASE("late errors from a closed generation are ignored", "[managed_bus]") {
    FakeTransport *current = nullptr;
    ManagedCANBus::Options options;
    options.dispatcher.max_consecutive_errors = 1;
    FrameCollector collector;
    ManagedCANBus bus(
        [&] {
            auto transport = std::make_unique<FakeTransport>();
            current = transport.get();
            return transport;
        },
        options);

    bus.subscribe(collector.callback());
    bus.start();
    current->fail_reads("unplugged");
    REQUIRE(wait_until([&] { return bus.get_health().state == TransportState::kFaulted; }));
    REQUIRE(bus.get_health().generation == 1);

    bus.recover();
    REQUIRE(bus.get_health().state == TransportState::kOpen);
    REQUIRE(bus.get_health().generation == 2);
    REQUIRE_FALSE(bus.get_health().last_error_code.has_value());

    current->inject(make_frame(0x28, std::byte{0xA9}));
    REQUIRE(collector.wait_for(1, kTimeout));
    REQUIRE(collector.frames().back().data[0] == 0xA9);
    bus.stop();
}

TEST_CASE("close failure is retryable and start requires recover", "[managed_bus]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });

    bus.start();
    current->fail_next_closes(1);
    REQUIRE_THROWS_AS(bus.stop(), TransportIOError);
    REQUIRE(bus.get_health().state == TransportState::kFaulted);
    REQUIRE_THROWS_AS(bus.start(), FaultedError);

    REQUIRE_NOTHROW(bus.recover());
    REQUIRE(bus.get_health().state == TransportState::kOpen);
    REQUIRE(bus.get_health().generation == 2);
    bus.stop();
}

TEST_CASE("sync send still means backend accepted the frame", "[managed_bus]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();
    bus.send(make_frame(0x28, std::byte{0x03}));
    REQUIRE(current->sent().size() == 1);
    current->fail_writes("USB transmit failed");
    REQUIRE_THROWS_AS(bus.send(make_frame(0x28, std::byte{0x04})), TransportIOError);
    REQUIRE(current->sent().size() == 1);
    bus.stop();
}

TEST_CASE("queue_send receipt completes after backend accept", "[managed_bus][queue]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();

    auto receipt = bus.queue_send(make_frame(0x28, std::byte{0x10}));
    REQUIRE_NOTHROW(receipt.wait_for(kTimeout));
    REQUIRE(receipt.completed());
    REQUIRE(current->sent().size() == 1);
    REQUIRE(current->sent()[0].data[0] == 0x10);
    bus.stop();
}

TEST_CASE("enqueue success is not physical send success", "[managed_bus][queue]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();
    current->arm_tx_gate();

    auto receipt = bus.queue_send(make_frame(0x28, std::byte{0x11}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    REQUIRE_FALSE(receipt.completed());
    REQUIRE(current->sent().empty());
    REQUIRE_THROWS_AS(receipt.wait_for(std::chrono::milliseconds(1)), linkerbot::can::TimeoutError);

    current->release_tx_gate();
    REQUIRE_NOTHROW(receipt.wait_for(kTimeout));
    REQUIRE(current->sent().size() == 1);
    bus.stop();
}

TEST_CASE("queue full is reported immediately", "[managed_bus][queue]") {
    FakeTransport *current = nullptr;
    ManagedCANBus::Options options;
    options.send_queue_size = 1;
    ManagedCANBus bus(
        [&] {
            auto transport = std::make_unique<FakeTransport>();
            current = transport.get();
            return transport;
        },
        options);
    bus.start();
    current->arm_tx_gate();

    auto primer = bus.queue_send(make_frame(0x28, std::byte{0x01}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    auto queued = bus.queue_send(make_frame(0x28, std::byte{0x02}));
    REQUIRE_THROWS_AS(bus.queue_send(make_frame(0x28, std::byte{0x03})), QueueFullError);

    current->release_tx_gate();
    REQUIRE_NOTHROW(primer.wait_for(kTimeout));
    REQUIRE_NOTHROW(queued.wait_for(kTimeout));
    bus.stop();
}

TEST_CASE("receipt distinguishes backend failure", "[managed_bus][queue]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();
    current->fail_writes("USB transmit failed");

    auto receipt = bus.queue_send(make_frame(0x28, std::byte{0x12}));
    try {
        receipt.wait_for(kTimeout);
        FAIL("backend failure should complete the receipt with an error");
    } catch (const TransportIOError &error) {
        REQUIRE(error.code() == ErrorCode::kTransportIo);
        REQUIRE(std::string(error.what()).find("USB transmit failed") != std::string::npos);
    }
    bus.stop();
}

TEST_CASE("stop completes pending receipts", "[managed_bus][queue]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();
    current->arm_tx_gate();

    auto in_flight = bus.queue_send(make_frame(0x28, std::byte{0x21}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    auto queued = bus.queue_send(make_frame(0x28, std::byte{0x22}));

    std::thread stopper([&] { bus.stop(); });
    REQUIRE_THROWS_AS(queued.wait_for(kTimeout), ClosedError);
    current->release_tx_gate();
    stopper.join();
    try {
        in_flight.wait_for(kTimeout);
    } catch (const ClosedError &) {
        CHECK(in_flight.completed());
    } catch (const TransportIOError &) {
        CHECK(in_flight.completed());
    }
    REQUIRE(in_flight.completed());
}

TEST_CASE("recover drops queued frames from the old generation", "[managed_bus][queue]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();
    current->arm_tx_gate();

    auto primer = bus.queue_send(make_frame(0x28, std::byte{0x31}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    auto stale = bus.queue_send(make_frame(0x28, std::byte{0x32}));

    std::thread recoverer([&] { bus.recover(); });
    REQUIRE_THROWS_AS(stale.wait_for(kTimeout), ClosedError);
    current->release_tx_gate();
    recoverer.join();
    try {
        primer.wait_for(kTimeout);
    } catch (const ClosedError &) {
        CHECK(primer.completed());
    } catch (const TransportIOError &) {
        CHECK(primer.completed());
    }
    REQUIRE(bus.get_health().generation == 2);
    REQUIRE(bus.get_health().state == TransportState::kOpen);

    auto fresh = bus.queue_send(make_frame(0x28, std::byte{0x33}));
    REQUIRE_NOTHROW(fresh.wait_for(kTimeout));
    REQUIRE(current->sent().size() == 1);
    REQUIRE(current->sent()[0].data[0] == 0x33);
    bus.stop();
}

TEST_CASE("queued sends coalesce into write_batch", "[managed_bus][queue][batch]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        transport->set_prefer_batch(true);
        current = transport.get();
        return transport;
    });
    bus.start();
    current->arm_tx_gate();

    auto primer = bus.queue_send(make_frame(0x10, std::byte{0}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    std::vector<linkerbot::can::SendReceipt> receipts;
    receipts.push_back(bus.queue_send(make_frame(0x11, std::byte{1})));
    receipts.push_back(bus.queue_send(make_frame(0x12, std::byte{2})));
    receipts.push_back(bus.queue_send(make_frame(0x13, std::byte{3})));
    current->release_tx_gate();

    REQUIRE_NOTHROW(primer.wait_for(kTimeout));
    for (auto &receipt : receipts) {
        REQUIRE_NOTHROW(receipt.wait_for(kTimeout));
    }
    REQUIRE(wait_until([&] { return current->batch_writes().size() == 1; }));
    REQUIRE(current->batch_writes().size() == 1);
    REQUIRE(current->batch_writes()[0].size() == 3);
    REQUIRE(current->batch_writes()[0][0].id == 0x11);
    REQUIRE(current->batch_writes()[0][2].id == 0x13);
    bus.stop();
}

TEST_CASE("failed batch fails every receipt", "[managed_bus][queue][batch]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        transport->set_prefer_batch(true);
        current = transport.get();
        return transport;
    });
    bus.start();
    current->arm_tx_gate();

    auto primer = bus.queue_send(make_frame(0x10, std::byte{0}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    current->fail_batches("batch outcome is unknown");
    auto first = bus.queue_send(make_frame(0x11, std::byte{1}));
    auto second = bus.queue_send(make_frame(0x12, std::byte{2}));
    auto third = bus.queue_send(make_frame(0x13, std::byte{3}));
    current->release_tx_gate();

    REQUIRE_NOTHROW(primer.wait_for(kTimeout));
    REQUIRE_THROWS_AS(first.wait_for(kTimeout), TransportIOError);
    REQUIRE_THROWS_AS(second.wait_for(kTimeout), TransportIOError);
    REQUIRE_THROWS_AS(third.wait_for(kTimeout), TransportIOError);
    bus.stop();
}

TEST_CASE("backends without batch fall back to per-frame send", "[managed_bus][queue][batch]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    });
    bus.start();
    current->arm_tx_gate();

    auto primer = bus.queue_send(make_frame(0x10, std::byte{0}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    auto a = bus.queue_send(make_frame(0x11, std::byte{1}));
    auto b = bus.queue_send(make_frame(0x12, std::byte{2}));
    current->release_tx_gate();

    REQUIRE_NOTHROW(primer.wait_for(kTimeout));
    REQUIRE_NOTHROW(a.wait_for(kTimeout));
    REQUIRE_NOTHROW(b.wait_for(kTimeout));
    REQUIRE(current->batch_writes().empty());
    REQUIRE(current->sent().size() == 3);
    bus.stop();
}

TEST_CASE("send_batch_max caps each backend call", "[managed_bus][queue][batch]") {
    FakeTransport *current = nullptr;
    ManagedCANBus::Options options;
    options.send_batch_max = 4;
    ManagedCANBus bus(
        [&] {
            auto transport = std::make_unique<FakeTransport>();
            transport->set_prefer_batch(true);
            current = transport.get();
            return transport;
        },
        options);
    bus.start();
    current->arm_tx_gate();

    auto primer = bus.queue_send(make_frame(0x10, std::byte{0}));
    REQUIRE(current->wait_until_tx_entered(kTimeout));
    std::vector<linkerbot::can::SendReceipt> receipts;
    for (uint8_t i = 1; i <= 10; ++i) {
        receipts.push_back(bus.queue_send(make_frame(0x20u + i, std::byte{i})));
    }
    current->release_tx_gate();

    REQUIRE_NOTHROW(primer.wait_for(kTimeout));
    for (auto &receipt : receipts) {
        REQUIRE_NOTHROW(receipt.wait_for(kTimeout));
    }
    REQUIRE(wait_until([&] {
        size_t total = 0;
        for (const auto &batch : current->batch_writes()) {
            total += batch.size();
        }
        return total == 10;
    }));
    for (const auto &batch : current->batch_writes()) {
        REQUIRE(batch.size() <= 4);
    }
    bus.stop();
}

TEST_CASE("single queued frame uses send not write_batch", "[managed_bus][queue][batch]") {
    FakeTransport *current = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        transport->set_prefer_batch(true);
        current = transport.get();
        return transport;
    });
    bus.start();
    auto receipt = bus.queue_send(make_frame(0x42, std::byte{1}));
    REQUIRE_NOTHROW(receipt.wait_for(kTimeout));
    REQUIRE(current->sent().size() == 1);
    REQUIRE(current->batch_writes().empty());
    bus.stop();
}

TEST_CASE("queue_send after stop fails fast", "[managed_bus][queue]") {
    ManagedCANBus bus([] { return std::make_unique<FakeTransport>(); });
    REQUIRE_THROWS_AS(bus.queue_send(make_frame(0x28, std::byte{0x01})), ClosedError);
    bus.start();
    bus.stop();
    REQUIRE_THROWS_AS(bus.queue_send(make_frame(0x28, std::byte{0x01})), ClosedError);
}

TEST_CASE("synchronous stop/start/recover from a subscriber callback is rejected, not fatal",
          "[managed_bus][lifecycle]") {
    // Regresses R02: calling stop()/recover() synchronously from the bus's own
    // receive callback would otherwise drop the last shared_ptr to the
    // still-running CANDispatcher on its own receive thread, destroying a
    // joinable std::thread that is currently executing -> std::terminate.
    FakeTransport *raw = nullptr;
    ManagedCANBus bus([&raw] {
        auto transport = std::make_unique<FakeTransport>();
        raw = transport.get();
        return transport;
    });
    bus.start();

    std::atomic<bool> saw_validation_error{false};
    std::atomic<bool> callback_finished{false};
    bus.subscribe([&](const Message &) {
        try {
            bus.stop();
        } catch (const ValidationError &) {
            saw_validation_error.store(true, std::memory_order_release);
        }
        callback_finished.store(true, std::memory_order_release);
    });

    raw->inject(make_frame(0x28, std::byte{0x50}));
    REQUIRE(wait_until([&] { return callback_finished.load(std::memory_order_acquire); }));
    REQUIRE(saw_validation_error.load(std::memory_order_acquire));
    // stop() was rejected rather than silently swallowed, so the bus is
    // still open and usable afterwards.
    REQUIRE(bus.get_health().state == TransportState::kOpen);
    bus.stop();
}

TEST_CASE("synchronous recover from the fault callback is rejected, not fatal",
          "[managed_bus][lifecycle]") {
    ManagedCANBus *bus_ptr = nullptr;
    std::atomic<bool> fault_seen{false};
    std::atomic<bool> saw_validation_error{false};

    ManagedCANBus::Options options;
    options.dispatcher.max_consecutive_errors = 1;
    options.on_fault = [&](const CANError &) {
        fault_seen.store(true, std::memory_order_release);
        try {
            bus_ptr->recover();
        } catch (const ValidationError &) {
            saw_validation_error.store(true, std::memory_order_release);
        }
        throw std::runtime_error("fault observer failure must be isolated");
    };

    FakeTransport *raw = nullptr;
    ManagedCANBus bus(
        [&raw] {
            auto transport = std::make_unique<FakeTransport>();
            raw = transport.get();
            return transport;
        },
        options);
    bus_ptr = &bus;
    bus.start();

    raw->fail_reads("boom");
    REQUIRE(wait_until([&] { return fault_seen.load(std::memory_order_acquire); }));
    REQUIRE(saw_validation_error.load(std::memory_order_acquire));
    REQUIRE_THROWS_AS(bus.send(make_frame(0x28, std::byte{0x52})), FaultedError);
    REQUIRE_THROWS_AS(bus.queue_send(make_frame(0x28, std::byte{0x53})), FaultedError);
}

TEST_CASE("an external stop is not blocked or terminated by a rejected self-stop callback",
          "[managed_bus][lifecycle]") {
    // Regresses R02 alongside a concurrent, legitimate close: the callback's
    // own stop() attempt must fail fast (not block), so it never contends
    // with the external thread's stop() for the receive-thread join.
    FakeTransport *raw = nullptr;
    ManagedCANBus bus([&raw] {
        auto transport = std::make_unique<FakeTransport>();
        raw = transport.get();
        return transport;
    });
    bus.start();

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release_now = false;
    std::atomic<bool> saw_validation_error{false};
    std::atomic<int> later_calls{0};

    bus.subscribe([&](const Message &) {
        try {
            bus.stop(); // must be rejected immediately, not block or terminate
        } catch (const ValidationError &) {
            saw_validation_error.store(true, std::memory_order_release);
        }
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_now; });
    });
    bus.subscribe([&](const Message &) { ++later_calls; });

    raw->inject(make_frame(0x28, std::byte{0x51}));
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    REQUIRE(saw_validation_error.load(std::memory_order_acquire));

    // The external stop() blocks joining the receive thread until the
    // callback above releases it, but must complete without deadlock.
    std::thread external_stop([&] { bus.stop(); });
    REQUIRE(wait_until([&] { return bus.get_health().state == TransportState::kStopping; }));

    {
        const std::scoped_lock lock(mutex);
        release_now = true;
    }
    cv.notify_all();

    external_stop.join();
    REQUIRE(bus.get_health().state == TransportState::kClosed);
    REQUIRE(later_calls.load() == 0);
}

TEST_CASE("managed bus health reports a disabled throwing subscriber",
          "[managed_bus][health][subscriber]") {
    FakeTransport *raw = nullptr;
    ManagedCANBus bus([&] {
        auto transport = std::make_unique<FakeTransport>();
        raw = transport.get();
        return transport;
    });
    bus.start();

    std::atomic<int> calls{0};
    bus.subscribe([&](const Message &) {
        ++calls;
        throw std::runtime_error("managed subscriber failure");
    });
    for (int i = 0; i < 4; ++i) {
        raw->inject(make_frame(0x28, static_cast<std::byte>(i)));
    }

    REQUIRE(wait_until([&] { return bus.get_health().disabled_subscriber_count == 1; }));
    const auto health = bus.get_health();
    REQUIRE(calls.load() == 3);
    REQUIRE(health.last_subscriber_error.find("managed subscriber failure") != std::string::npos);
    bus.stop();
}

TEST_CASE("managed bus destructor contains a retryable transport close failure",
          "[managed_bus][lifecycle]") {
    FakeTransport *raw = nullptr;
    {
        ManagedCANBus bus([&] {
            auto transport = std::make_unique<FakeTransport>();
            raw = transport.get();
            return transport;
        });
        bus.start();
        raw->fail_next_closes(1);
    }
}

TEST_CASE("pool shares one connection until the last lease closes", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";
    std::vector<FakeTransport *> created;

    auto factory = [&created] {
        auto transport = std::make_unique<FakeTransport>();
        created.push_back(transport.get());
        return transport;
    };

    auto first = pool.acquire(key, factory);
    auto second = pool.acquire(key, factory);
    REQUIRE(created.size() == 1);
    REQUIRE(&first.bus() == &second.bus());
    REQUIRE(pool.lease_count(key) == 2);

    first.close();
    REQUIRE(second.bus().get_health().state == TransportState::kOpen);
    REQUIRE(pool.lease_count(key) == 1);

    second.close();
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("lease close is idempotent and access after close fails", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";
    auto lease = pool.acquire(key, [] { return std::make_unique<FakeTransport>(); });

    lease.close();
    lease.close();
    REQUIRE_THROWS_AS(lease.bus(), ClosedError);
    REQUIRE_THROWS_AS(lease.shared_bus(), ClosedError);
}

TEST_CASE("lease move assignment preserves both owners when target close fails", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey first_key;
    first_key.backend = "fake";
    first_key.endpoint = "can0";
    CANBusKey second_key = first_key;
    second_key.endpoint = "can1";

    FakeTransport *first_transport = nullptr;
    auto first = pool.acquire(first_key, [&] {
        auto transport = std::make_unique<FakeTransport>();
        first_transport = transport.get();
        return transport;
    });
    auto second = pool.acquire(second_key, [] { return std::make_unique<FakeTransport>(); });

    first_transport->fail_next_closes(1);
    REQUIRE_THROWS_AS(first = std::move(second), TransportIOError);

    // The failed assignment must modify neither operand: the target remains
    // the only explicit retry owner for its failed close, and the source still
    // owns the lease that was going to replace it.
    REQUIRE_NOTHROW(first.bus());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- a failed target
    // close leaves the source unchanged, as this test verifies.
    REQUIRE_NOTHROW(second.bus());
    REQUIRE(pool.lease_count(first_key) == 1);
    REQUIRE(pool.lease_count(second_key) == 1);

    REQUIRE_NOTHROW(first.close());
    REQUIRE_NOTHROW(second.close());
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("lease move assignment closes the target before adopting the source", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey first_key;
    first_key.backend = "fake";
    first_key.endpoint = "can0";
    CANBusKey second_key = first_key;
    second_key.endpoint = "can1";

    auto first = pool.acquire(first_key, [] { return std::make_unique<FakeTransport>(); });
    auto second = pool.acquire(second_key, [] { return std::make_unique<FakeTransport>(); });
    auto *second_bus = &second.bus();

    first = std::move(second);
    REQUIRE(pool.lease_count(first_key) == 0);
    REQUIRE(pool.lease_count(second_key) == 1);
    REQUIRE(&first.bus() == second_bus);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- the documented
    // moved-from state is the contract under test.
    REQUIRE_THROWS_AS(second.bus(), ClosedError);

    // Self-move remains a no-op and keeps the adopted lease usable.
    auto *same_lease = &first;
    first = std::move(*same_lease);
    REQUIRE(&first.bus() == second_bus);
    first.close();
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("a moved-from lease can adopt a new owner and be destroyed in a callback",
          "[pool][lifecycle]") {
    SharedCANBusPool pool;
    CANBusKey first_key;
    first_key.backend = "fake";
    first_key.endpoint = "can0";
    CANBusKey second_key = first_key;
    second_key.endpoint = "can1";
    CANBusKey trigger_key = first_key;
    trigger_key.endpoint = "can2";

    auto moved_from = pool.acquire(first_key, [] { return std::make_unique<FakeTransport>(); });
    auto first_owner = std::move(moved_from);
    std::optional<CANBusLease> source;
    source.emplace(pool.acquire(second_key, [] { return std::make_unique<FakeTransport>(); }));

    // Reusing the moved-from target must adopt the source's preallocated
    // deferred-cleanup node as well as its bus ownership.
    moved_from = std::move(*source);
    REQUIRE(pool.lease_count(first_key) == 1);
    REQUIRE(pool.lease_count(second_key) == 1);

    FakeTransport *trigger_transport = nullptr;
    auto trigger = pool.acquire(trigger_key, [&] {
        auto transport = std::make_unique<FakeTransport>();
        trigger_transport = transport.get();
        return transport;
    });
    std::atomic<bool> callback_returned{false};
    std::atomic<bool> retry_rejected{false};
    trigger.bus().subscribe([&](const Message &) {
        // source now contains a valid moved-from lease with no bus/task. Its
        // callback-thread destruction must be a harmless no-op.
        source.reset();
        try {
            CANBusLease::retry_deferred_cleanup();
        } catch (const ValidationError &) {
            retry_rejected.store(true, std::memory_order_release);
        }
        callback_returned.store(true, std::memory_order_release);
    });

    trigger_transport->inject(make_frame(0x28, std::byte{0x62}));
    REQUIRE(wait_until([&] { return callback_returned.load(std::memory_order_acquire); }));
    REQUIRE_FALSE(source.has_value());
    REQUIRE(retry_rejected.load(std::memory_order_acquire));

    moved_from.close();
    first_owner.close();
    trigger.close();
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("concurrent acquire uses one factory and connection", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";
    std::atomic<int> factory_calls{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::mutex leases_mutex;
    std::vector<CANBusLease> leases;

    auto factory = [&factory_calls] {
        ++factory_calls;
        return std::make_unique<FakeTransport>();
    };

    std::vector<std::thread> callers;
    callers.reserve(16);
    for (int i = 0; i < 16; ++i) {
        callers.emplace_back([&] {
            ++ready;
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto lease = pool.acquire(key, factory);
            const std::scoped_lock lock(leases_mutex);
            leases.push_back(std::move(lease));
        });
    }
    REQUIRE(wait_until([&] { return ready.load() == 16; }));
    go.store(true, std::memory_order_release);
    for (auto &caller : callers) {
        caller.join();
    }

    REQUIRE(factory_calls.load() == 1);
    REQUIRE(pool.lease_count(key) == 16);
    ManagedCANBus *shared = &leases.front().bus();
    for (auto &lease : leases) {
        REQUIRE(&lease.bus() == shared);
    }
    for (auto &lease : leases) {
        lease.close();
    }
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("failed pool start rolls back the preconstructed physical reservation", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey key;
    // Heap-backed strings exercise the path whose former post-mutation key
    // reconstruction could throw and strand physical_active_.
    key.backend = std::string(4096, 'b');
    key.endpoint = std::string(4096, 'e');
    key.device_index = 3;
    key.channel_index = 7;
    key.nom_baud = 500000;
    int factory_calls = 0;

    REQUIRE_THROWS_AS(pool.acquire(key,
                                   [&]() -> std::unique_ptr<linkerbot::can::CANTransport> {
                                       ++factory_calls;
                                       throw TransportIOError("injected start failure");
                                   }),
                      TransportIOError);
    REQUIRE(factory_calls == 1);
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());

    // The same physical wire with a different configuration must be reusable;
    // a stale reservation from the failed open would reject this acquisition.
    auto replacement_key = key;
    replacement_key.nom_baud = 1000000;
    auto replacement =
        pool.acquire(replacement_key, [] { return std::make_unique<FakeTransport>(); });
    REQUIRE(pool.lease_count(replacement_key) == 1);
    replacement.close();
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("a failed lease close must be retried explicitly by the same lease", "[pool]") {
    // Updated for the lease-close retry contract: a failed close() no longer
    // makes the lease's state a silent no-op, and the pool entry now stays
    // reserved (closing) until *this* lease's close() is retried and
    // succeeds -- a brand-new acquire() for the same key is not allowed to
    // race a fresh bus onto a wire whose previous close may not have
    // actually released it. So, unlike the old version of this test, we
    // must retry the same lease's close() (which now succeeds, since
    // fail_next_closes(1) only arms a single failure) before the key
    // becomes reusable.
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";
    int factory_calls = 0;
    FakeTransport *current = nullptr;

    auto factory = [&] {
        ++factory_calls;
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    };

    auto first = pool.acquire(key, factory);
    (void)first.bus();
    current->fail_next_closes(1);

    REQUIRE_THROWS_AS(first.close(), TransportIOError);
    // The failed close retains the lease's own reference: bus() still works,
    // the entry is still shown as active, and the lease count was decremented
    // exactly once (not left dangling, not double-decremented on retry).
    REQUIRE_NOTHROW(first.bus());
    REQUIRE(pool.lease_count(key) == 1);
    REQUIRE(pool.active_keys() == std::vector<CANBusKey>{key});

    REQUIRE_NOTHROW(first.close()); // explicit retry succeeds.
    REQUIRE(pool.active_keys().empty());

    auto second = pool.acquire(key, factory);
    // A successful retry releases the old physical bus before reacquisition.
    REQUIRE(factory_calls == 2);
    REQUIRE(second.bus().get_health().generation == 1);
    REQUIRE(second.bus().get_health().state == TransportState::kOpen);
    second.close();
}

TEST_CASE("a new acquire rejects a key whose owning lease still needs a close retry", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";
    FakeTransport *current = nullptr;
    auto factory = [&] {
        auto transport = std::make_unique<FakeTransport>();
        current = transport.get();
        return transport;
    };
    auto first = pool.acquire(key, factory);
    current->fail_next_closes(1);
    REQUIRE_THROWS_AS(first.close(), TransportIOError);
    REQUIRE(pool.lease_count(key) == 1);
    REQUIRE_THROWS_AS(pool.acquire(key, factory), FaultedError);
    REQUIRE_NOTHROW(first.close());
    auto next = pool.acquire(key, factory);
    REQUIRE(pool.lease_count(key) == 1);
    next.close();
}

TEST_CASE("a failed lease close can still be retried after the pool wrapper is destroyed",
          "[pool]") {
    CANBusKey key;
    key.backend = "fake";
    FakeTransport *current = nullptr;
    std::optional<CANBusLease> lease;
    {
        SharedCANBusPool pool;
        lease = pool.acquire(key, [&] {
            auto transport = std::make_unique<FakeTransport>();
            current = transport.get();
            return transport;
        });
    } // The pool wrapper is destroyed here; the shared PoolState must live on
      // via the lease's shared_ptr so a later retry still has somewhere to go.

    current->fail_next_closes(1);
    REQUIRE_THROWS_AS(lease->close(), TransportIOError);
    REQUIRE_NOTHROW(lease->bus().get_health());
    REQUIRE_NOTHROW(lease->close()); // retry succeeds even with no pool wrapper alive.
}

TEST_CASE("destructor close failures remain pending for explicit deferred retry", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey first_key;
    first_key.backend = "fake";
    first_key.endpoint = "can0";
    CANBusKey second_key = first_key;
    second_key.endpoint = "can1";

    FakeTransport *first_transport = nullptr;
    FakeTransport *second_transport = nullptr;
    std::optional<CANBusLease> first;
    std::optional<CANBusLease> second;
    first.emplace(pool.acquire(first_key, [&] {
        auto transport = std::make_unique<FakeTransport>();
        first_transport = transport.get();
        return transport;
    }));
    second.emplace(pool.acquire(second_key, [&] {
        auto transport = std::make_unique<FakeTransport>();
        second_transport = transport.get();
        return transport;
    }));

    // One failure is observed by the destructor's synchronous attempt; the
    // second is observed by the cleanup worker. The intrusive tasks must then
    // remain as the retry owners for both original pool records.
    first_transport->fail_next_closes(2);
    second_transport->fail_next_closes(2);
    first.reset();
    second.reset();

    REQUIRE_THROWS_AS(CANBusLease::drain_deferred_cleanup(), FaultedError);
    REQUIRE(CANBusLease::pending_deferred_cleanup() == 2);
    REQUIRE(pool.lease_count(first_key) == 1);
    REQUIRE(pool.lease_count(second_key) == 1);
    REQUIRE(pool.active_keys().size() == 2);
    REQUIRE_THROWS_AS(pool.acquire(first_key, [] { return std::make_unique<FakeTransport>(); }),
                      FaultedError);
    REQUIRE_THROWS_AS(pool.acquire(second_key, [] { return std::make_unique<FakeTransport>(); }),
                      FaultedError);

    CANBusLease::retry_deferred_cleanup();
    REQUIRE_NOTHROW(CANBusLease::drain_deferred_cleanup());
    REQUIRE(CANBusLease::pending_deferred_cleanup() == 0);
    REQUIRE(pool.lease_count(first_key) == 0);
    REQUIRE(pool.lease_count(second_key) == 0);
    REQUIRE(pool.active_keys().empty());

    // Retrying with no failed work is explicitly idempotent.
    CANBusLease::retry_deferred_cleanup();
    REQUIRE_NOTHROW(CANBusLease::drain_deferred_cleanup());
}

TEST_CASE("pool acquire and lease close reject calls from a dispatcher's own receive thread",
          "[pool][lifecycle]") {
    // Regresses the pool deadlock scenario: acquiring or closing a lease from
    // inside a subscriber/fault callback could otherwise block on a lock or
    // a bus->stop()/recover() that itself needs to join the very receive
    // thread the callback is running on. Both entry points must fail fast
    // (ValidationError) instead of ever reaching a blocking wait.
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";

    FakeTransport *raw = nullptr;
    auto lease = pool.acquire(key, [&raw] {
        auto transport = std::make_unique<FakeTransport>();
        raw = transport.get();
        return transport;
    });

    std::atomic<bool> acquire_rejected{false};
    std::atomic<bool> close_rejected{false};
    std::atomic<bool> callback_finished{false};
    lease.bus().subscribe([&](const Message &) {
        // Same key as the lease this very callback's bus belongs to: without
        // the early rejection, this could otherwise wait on the pool's
        // condition_variable for a close that itself needs this thread to
        // finish first.
        try {
            pool.acquire(key, [] { return std::make_unique<FakeTransport>(); });
        } catch (const ValidationError &) {
            acquire_rejected.store(true, std::memory_order_release);
        }
        try {
            lease.close();
        } catch (const ValidationError &) {
            close_rejected.store(true, std::memory_order_release);
        }
        callback_finished.store(true, std::memory_order_release);
    });

    raw->inject(make_frame(0x28, std::byte{0x60}));
    REQUIRE(wait_until([&] { return callback_finished.load(std::memory_order_acquire); }));
    REQUIRE(acquire_rejected.load(std::memory_order_acquire));
    REQUIRE(close_rejected.load(std::memory_order_acquire));
    // The rejected close() must not have disturbed the lease's own state: the
    // pool entry is still active with the lease intact, and closing it for
    // real afterwards (from outside any callback) still works normally.
    REQUIRE(pool.lease_count(key) == 1);
    lease.close();
}

TEST_CASE("a lease outlives its pool and still closes safely", "[pool]") {
    // Regresses R19: CANBusLease must hold a shared, ref-counted pool state
    // rather than a raw SharedCANBusPool*, so a lease that outlives the pool
    // wrapper object can still be used and closed correctly.
    CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";

    std::optional<CANBusLease> lease;
    {
        SharedCANBusPool pool;
        lease = pool.acquire(key, [] { return std::make_unique<FakeTransport>(); });
    } // The pool wrapper is destroyed here; the shared PoolState must live on.

    REQUIRE_NOTHROW(lease->bus().get_health());
    REQUIRE_NOTHROW(lease->close());
}

TEST_CASE("last lease destruction in a receive callback is deferred safely", "[pool][lifecycle]") {
    int close_failures = 0;
    SECTION("normal close") {
        close_failures = 0;
    }
    SECTION("transient close failure is retried by the deferred owner") {
        close_failures = 1;
    }

    CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";

    std::optional<CANBusLease> lease;
    FakeTransport *transport = nullptr;
    auto transport_destroyed = std::make_shared<std::atomic<bool>>(false);
    {
        SharedCANBusPool pool;
        lease.emplace(pool.acquire(key, [&] {
            auto fake = std::make_unique<DestructionTrackingFakeTransport>(transport_destroyed);
            transport = fake.get();
            return fake;
        }));
    } // The lease is now the last owner of PoolState and ManagedCANBus.

    std::atomic<bool> callback_returned{false};
    std::atomic<bool> drain_rejected_in_callback{false};
    std::atomic<bool> retry_rejected_in_callback{false};
    lease->bus().subscribe([&](const Message &) {
        lease.reset();
        try {
            CANBusLease::drain_deferred_cleanup();
        } catch (const ValidationError &) {
            drain_rejected_in_callback.store(true, std::memory_order_release);
        }
        try {
            CANBusLease::retry_deferred_cleanup();
        } catch (const ValidationError &) {
            retry_rejected_in_callback.store(true, std::memory_order_release);
        }
        callback_returned.store(true, std::memory_order_release);
    });

    transport->fail_next_closes(close_failures);
    transport->inject(make_frame(0x28, std::byte{0x61}));
    REQUIRE(wait_until([&] { return callback_returned.load(std::memory_order_acquire); }));
    REQUIRE_FALSE(lease.has_value());
    REQUIRE(drain_rejected_in_callback.load(std::memory_order_acquire));
    REQUIRE(retry_rejected_in_callback.load(std::memory_order_acquire));

    if (close_failures != 0) {
        REQUIRE_THROWS_AS(CANBusLease::drain_deferred_cleanup(), FaultedError);
        REQUIRE(CANBusLease::pending_deferred_cleanup() == 1);
        REQUIRE_FALSE(transport_destroyed->load(std::memory_order_acquire));
        CANBusLease::retry_deferred_cleanup();
    }
    REQUIRE_NOTHROW(CANBusLease::drain_deferred_cleanup());
    REQUIRE(CANBusLease::pending_deferred_cleanup() == 0);
    REQUIRE(transport_destroyed->load(std::memory_order_acquire));
}

TEST_CASE("the pool outlives a closed lease and stays reusable", "[pool]") {
    SharedCANBusPool pool;
    CANBusKey key;
    key.backend = "fake";

    {
        auto lease = pool.acquire(key, [] { return std::make_unique<FakeTransport>(); });
        lease.close();
    }
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());

    auto lease = pool.acquire(key, [] { return std::make_unique<FakeTransport>(); });
    REQUIRE(pool.lease_count(key) == 1);
    lease.close();
}

TEST_CASE("same physical channel with a conflicting configuration is rejected", "[pool]") {
    // Regresses R19: acquiring the same physical backend/endpoint/device/
    // channel with a different fd_mode/bitrate must be explicitly rejected
    // rather than silently sharing (or racing to open) two configurations.
    SharedCANBusPool pool;
    CANBusKey key_a;
    key_a.backend = "fake";
    key_a.endpoint = "can0";
    key_a.fd_mode = false;
    key_a.nom_baud = 500000;

    auto lease = pool.acquire(key_a, [] { return std::make_unique<FakeTransport>(); });

    CANBusKey key_b = key_a;
    key_b.nom_baud = 1000000; // same physical channel, conflicting bitrate

    REQUIRE_THROWS_AS(pool.acquire(key_b, [] { return std::make_unique<FakeTransport>(); }),
                      ValidationError);
    REQUIRE(pool.lease_count(key_b) == 0);

    lease.close();
    // Once fully closed, the physical channel is free again for a new config.
    auto retry = pool.acquire(key_b, [] { return std::make_unique<FakeTransport>(); });
    REQUIRE(pool.lease_count(key_b) == 1);
    retry.close();
}

TEST_CASE("ManagedCANBus factory must be callable", "[managed_bus]") {
    ManagedCANBus::TransportFactory empty;
    REQUIRE_THROWS_AS(ManagedCANBus(empty), ValidationError);
}
