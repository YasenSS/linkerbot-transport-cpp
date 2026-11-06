#include <catch2/catch_test_macros.hpp>

#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/message.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using linkerbot::can::CANDispatcher;
using linkerbot::can::ClosedError;
using linkerbot::can::ErrorCode;
using linkerbot::can::FakeTransport;
using linkerbot::can::FaultedError;
using linkerbot::can::Message;
using linkerbot::can::TransportIOError;
using linkerbot::can::TransportState;
using linkerbot::can::ValidationError;

namespace {

    // Blocks until `count` frames have arrived, or the timeout expires. Polling on
    // a condition variable keeps the test fast without a fixed sleep.
    class FrameCollector {
      public:
        auto callback() -> CANDispatcher::FrameCallback {
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

} // namespace

TEST_CASE("dispatcher requires a transport", "[dispatcher]") {
    REQUIRE_THROWS_AS(CANDispatcher(nullptr), ValidationError);
}

TEST_CASE("subscriber snapshots reuse membership until a successful change",
          "[dispatcher][snapshot]") {
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;
    Registry registry;
    const auto empty = registry.snapshot_view();
    REQUIRE(empty->empty());
    REQUIRE(registry.snapshot_view() == empty);

    const auto first_id = registry.subscribe([] {});
    const auto first = registry.snapshot_view();
    REQUIRE(first != empty);
    REQUIRE(first->size() == 1);
    REQUIRE(registry.snapshot_view() == first);
    REQUIRE(empty->empty());

    registry.subscribe([] {});
    const auto second = registry.snapshot_view();
    REQUIRE(second != first);
    REQUIRE(second->size() == 2);
    REQUIRE(first->size() == 1);

    auto legacy = registry.snapshot();
    legacy.clear();
    REQUIRE(registry.snapshot_view() == second);
    REQUIRE(second->size() == 2);
    REQUIRE_FALSE(registry.unsubscribe(0));
    REQUIRE(registry.snapshot_view() == second);

    REQUIRE(registry.unsubscribe(first_id));
    const auto remaining = registry.snapshot_view();
    REQUIRE(remaining != second);
    REQUIRE(remaining->size() == 1);
    REQUIRE(remaining->front() == second->back());
    registry.clear();
    REQUIRE(registry.snapshot_view()->empty());
    REQUIRE(registry.snapshot_view() != remaining);
}

TEST_CASE("failed subscription guards preserve cached membership", "[dispatcher][snapshot]") {
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;
    Registry registry;
    registry.subscribe([] {});
    const auto before = registry.snapshot_view();
    REQUIRE_THROWS_AS(registry.subscribe_guarded([] {}, [] { throw ValidationError("guard"); }),
                      ValidationError);
    REQUIRE(registry.snapshot_view() == before);
    REQUIRE(registry.size() == 1);
}

TEST_CASE("cached snapshots cannot invoke cancelled entries", "[dispatcher][snapshot]") {
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;
    Registry registry;
    size_t calls = 0;
    const auto first_id = registry.subscribe([&] { ++calls; });
    registry.subscribe([&] { ++calls; });
    const auto old = registry.snapshot_view();

    REQUIRE(registry.unsubscribe(first_id));
    REQUIRE_FALSE(Registry::invoke(*old->front()));
    REQUIRE(Registry::invoke(*old->back()));
    REQUIRE(calls == 1);
    registry.clear();
    REQUIRE_FALSE(Registry::invoke(*old->back()));
    REQUIRE(calls == 1);
}

TEST_CASE("cached snapshots support self unsubscribe and subscription during dispatch",
          "[dispatcher][snapshot]") {
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;
    Registry registry;
    linkerbot::can::SubscriptionId self_id = 0;
    size_t calls = 0;
    bool unsubscribed = false;
    self_id = registry.subscribe([&] {
        unsubscribed = registry.unsubscribe(self_id);
        registry.subscribe([&] { ++calls; });
    });
    const auto old = registry.snapshot_view();
    REQUIRE(Registry::invoke(*old->front()));
    REQUIRE(unsubscribed);
    REQUIRE_FALSE(Registry::invoke(*old->front()));
    REQUIRE(calls == 0);
    const auto current = registry.snapshot_view();
    REQUIRE(current->size() == 1);
    REQUIRE(current->front() != old->front());
    REQUIRE(Registry::invoke(*current->front()));
    REQUIRE(calls == 1);
}

TEST_CASE("cached snapshots remain immutable during concurrent membership changes",
          "[dispatcher][snapshot]") {
    using Registry = linkerbot::can::detail::SubscriberRegistry<std::function<void()>>;
    Registry registry;
    registry.subscribe([] {});
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> first_change{false};
    std::atomic<bool> valid{true};
    std::atomic<unsigned> held_views{0};
    std::atomic<unsigned> reads{0};

    const auto reader = [&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        bool announced = false;
        try {
            const auto held = registry.snapshot_view();
            held_views.fetch_add(1, std::memory_order_release);
            announced = true;
            while (!first_change.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (held->size() != 1 || !held->front()) {
                valid.store(false, std::memory_order_relaxed);
            }
            for (unsigned iteration = 0; iteration < 1000; ++iteration) {
                const auto snapshot = registry.snapshot_view();
                const auto size = snapshot->size();
                if (size < 1 || size > 2) {
                    valid.store(false, std::memory_order_relaxed);
                }
                for (const auto &entry : *snapshot) {
                    if (!entry) {
                        valid.store(false, std::memory_order_relaxed);
                    }
                }
                std::this_thread::yield();
                if (snapshot->size() != size) {
                    valid.store(false, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
                if (stop.load(std::memory_order_acquire)) {
                    break;
                }
            }
        } catch (...) {
            valid.store(false, std::memory_order_relaxed);
            // Let the writer finish even if acquiring an initial view failed.
            if (!announced) {
                held_views.fetch_add(1, std::memory_order_release);
            }
        }
    };
    std::thread first_reader(reader);
    std::thread second_reader(reader);
    start.store(true, std::memory_order_release);
    while (held_views.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    try {
        for (unsigned iteration = 0; iteration < 1000; ++iteration) {
            const auto id = registry.subscribe([] {});
            if (!registry.unsubscribe(id)) {
                valid.store(false, std::memory_order_relaxed);
            }
            first_change.store(true, std::memory_order_release);
        }
    } catch (...) {
        valid.store(false, std::memory_order_relaxed);
    }
    first_change.store(true, std::memory_order_release);
    stop.store(true, std::memory_order_release);
    first_reader.join();
    second_reader.join();
    CHECK(valid.load());
    CHECK(reads.load() >= 2);
    CHECK(registry.size() == 1);
}

TEST_CASE("dispatcher delivers injected frames to a subscriber", "[dispatcher]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    FrameCollector collector;
    dispatcher.subscribe(collector.callback());

    raw->inject(make_frame(0x28, std::byte{0xAA}));

    REQUIRE(collector.wait_for(1, kTimeout));
    auto got = collector.frames();
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].id == 0x28);
    REQUIRE(got[0].data[0] == 0xAA);
}

TEST_CASE("every subscriber sees every frame", "[dispatcher]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    FrameCollector first;
    FrameCollector second;
    dispatcher.subscribe(first.callback());
    dispatcher.subscribe(second.callback());

    raw->inject(make_frame(0x27, std::byte{0x01}));

    REQUIRE(first.wait_for(1, kTimeout));
    REQUIRE(second.wait_for(1, kTimeout));
    REQUIRE(first.frames().size() == 1);
    REQUIRE(second.frames().size() == 1);
}

TEST_CASE("unsubscribe stops delivery and reports unknown ids", "[dispatcher]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    FrameCollector stays;
    FrameCollector leaves;
    dispatcher.subscribe(stays.callback());
    auto id = dispatcher.subscribe(leaves.callback());

    REQUIRE(dispatcher.unsubscribe(id));
    REQUIRE_FALSE(dispatcher.unsubscribe(id)); // already gone

    raw->inject(make_frame(0x28, std::byte{0x02}));

    REQUIRE(stays.wait_for(1, kTimeout));
    REQUIRE(leaves.frames().empty());
}

TEST_CASE("unsubscribe cancels a callback already copied into an in-flight snapshot",
          "[dispatcher][unsubscribe]") {
    // Regresses R01: recv_loop() copies the whole subscriber list before
    // releasing the lock, so unsubscribe() must independently guarantee a
    // copied-but-not-yet-entered callback is never invoked, not merely that
    // it is removed from the list new snapshots see.
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    dispatcher.subscribe([&](const Message &) {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    });

    std::atomic<int> later_calls{0};
    auto later_id = dispatcher.subscribe([&](const Message &) { ++later_calls; });

    raw->inject(make_frame(0x28, std::byte{0x40}));
    {
        // The first subscriber is now blocking recv_loop mid-dispatch, so the
        // snapshot it holds is guaranteed to already contain later_id.
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }

    REQUIRE(dispatcher.unsubscribe(later_id));

    {
        const std::scoped_lock lock(mutex);
        released = true;
    }
    cv.notify_all();

    REQUIRE(wait_until([&] { return dispatcher.get_health().state == TransportState::kOpen; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(later_calls.load() == 0);
}

TEST_CASE("unsubscribe waits for an in-flight external callback to finish",
          "[dispatcher][unsubscribe]") {
    // Regresses R01: a caller unsubscribing (e.g. from a manager destructor)
    // must be able to rely on the callback having fully returned by the time
    // unsubscribe() returns, so it is then safe to free state the callback
    // closes over.
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    std::atomic<bool> callback_finished{false};

    auto id = dispatcher.subscribe([&](const Message &) {
        {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        callback_finished.store(true, std::memory_order_release);
    });

    raw->inject(make_frame(0x28, std::byte{0x41}));
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }

    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::scoped_lock lock(mutex);
        released = true;
        cv.notify_all();
    });

    // unsubscribe() must block here until the in-flight callback returns, so
    // callback_finished is guaranteed true immediately once it returns.
    REQUIRE(dispatcher.unsubscribe(id));
    REQUIRE(callback_finished.load(std::memory_order_acquire));

    releaser.join();
}

TEST_CASE("a subscriber can unsubscribe itself without deadlocking", "[dispatcher][unsubscribe]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    linkerbot::can::SubscriptionId self_id = 0;
    std::atomic<bool> self_unsubscribe_ok{false};
    self_id = dispatcher.subscribe([&](const Message &) {
        self_unsubscribe_ok.store(dispatcher.unsubscribe(self_id), std::memory_order_release);
    });

    raw->inject(make_frame(0x28, std::byte{0x42}));
    REQUIRE(wait_until([&] { return self_unsubscribe_ok.load(std::memory_order_acquire); }));
    REQUIRE_FALSE(dispatcher.unsubscribe(self_id));
}

TEST_CASE("a throwing subscriber does not starve the others", "[dispatcher]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    std::atomic<int> throw_count{0};
    dispatcher.subscribe([&throw_count](const Message &) {
        ++throw_count;
        throw std::runtime_error("subscriber blew up");
    });

    FrameCollector healthy;
    dispatcher.subscribe(healthy.callback());

    raw->inject(make_frame(0x28, std::byte{0x03}));
    raw->inject(make_frame(0x28, std::byte{0x04}));

    REQUIRE(healthy.wait_for(2, kTimeout));
    REQUIRE(throw_count.load() == 2);
}

TEST_CASE("send reaches the transport and a handler can reply", "[dispatcher]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();

    // Sense command 0x01 -> reply with the same command plus two payload bytes.
    raw->echo_cmd(0x28, 0x01, 2, 0x7F);

    CANDispatcher dispatcher(std::move(transport));
    FrameCollector collector;
    dispatcher.subscribe(collector.callback());

    REQUIRE_NOTHROW(dispatcher.send(make_frame(0x28, std::byte{0x01})));

    REQUIRE(collector.wait_for(1, kTimeout));
    auto reply = collector.frames().at(0);
    REQUIRE(reply.id == 0x28);
    REQUIRE(reply.dlc == 3);
    REQUIRE(reply.data[0] == 0x01);
    REQUIRE(reply.data[1] == 0x7F);

    auto sent = raw->sent();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].data[0] == 0x01);
}

TEST_CASE("stop is idempotent and send fails on a closed transport", "[dispatcher]") {
    auto transport = std::make_unique<FakeTransport>();
    CANDispatcher dispatcher(std::move(transport));

    dispatcher.stop();
    dispatcher.stop(); // must not join twice

    REQUIRE_THROWS_AS(dispatcher.send(make_frame(0x28, std::byte{0x05})), ClosedError);
}

TEST_CASE("a read that throws does not kill the receive thread", "[dispatcher]") {
    // A transport whose first read throws, then behaves normally. The dispatcher
    // must survive the throw and keep delivering.
    class FlakyTransport : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            if (!threw_) {
                threw_ = true;
                throw std::runtime_error("link glitch");
            }
            if (delivered_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return std::nullopt;
            }
            delivered_ = true;
            return make_frame(0x28, std::byte{0x06});
        }

        auto write(const Message &) -> void override {}
        auto close() -> void override {}

      private:
        bool threw_ = false;
        bool delivered_ = false;
    };

    CANDispatcher dispatcher(std::make_unique<FlakyTransport>());
    FrameCollector collector;
    dispatcher.subscribe(collector.callback());

    REQUIRE(collector.wait_for(1, kTimeout));
    REQUIRE(collector.frames().at(0).data[0] == 0x06);
}

TEST_CASE("dispatcher validates its fault policy", "[dispatcher][health]") {
    CANDispatcher::Options zero_threshold;
    zero_threshold.max_consecutive_errors = 0;
    REQUIRE_THROWS_AS(CANDispatcher(std::make_unique<FakeTransport>(), std::move(zero_threshold)),
                      ValidationError);

    CANDispatcher::Options invalid_backoff;
    invalid_backoff.error_backoff_base = std::chrono::milliseconds(10);
    invalid_backoff.error_backoff_max = std::chrono::milliseconds(5);
    REQUIRE_THROWS_AS(CANDispatcher(std::make_unique<FakeTransport>(), std::move(invalid_backoff)),
                      ValidationError);

    CANDispatcher::Options zero_backoff;
    zero_backoff.error_backoff_base = std::chrono::milliseconds(0);
    REQUIRE_THROWS_AS(CANDispatcher(std::make_unique<FakeTransport>(), std::move(zero_backoff)),
                      ValidationError);
}

TEST_CASE("transient receive error backs off then recovers health", "[dispatcher][health]") {
    class TransientTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            const int attempt = ++attempts;
            if (attempt == 1) {
                first_error_at = std::chrono::steady_clock::now();
                throw TransportIOError("temporary receive failure");
            }
            if (attempt == 2) {
                recovered_at = std::chrono::steady_clock::now();
                return make_frame(0x28, std::byte{0x31});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }

        auto write(const Message &) -> void override {}
        auto close() -> void override {}

        std::atomic<int> attempts{0};
        std::chrono::steady_clock::time_point first_error_at{};
        std::chrono::steady_clock::time_point recovered_at{};
    };

    auto transport = std::make_unique<TransientTransport>();
    auto *raw = transport.get();
    CANDispatcher::Options options;
    options.error_backoff_base = std::chrono::milliseconds(5);
    options.error_backoff_max = std::chrono::milliseconds(5);
    CANDispatcher dispatcher(std::move(transport), options);
    FrameCollector collector;
    dispatcher.subscribe(collector.callback());

    REQUIRE(collector.wait_for(1, kTimeout));
    REQUIRE(raw->recovered_at - raw->first_error_at >= std::chrono::milliseconds(4));
    const auto health = dispatcher.get_health();
    REQUIRE(health.state == TransportState::kOpen);
    REQUIRE(health.consecutive_errors == 0);
    REQUIRE(health.last_successful_rx.has_value());
}

TEST_CASE("consecutive receive errors fault once and reject sends", "[dispatcher][health]") {
    class BrokenTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            ++read_calls;
            throw TransportIOError("receive path is down", {}, -23);
        }

        auto write(const Message &) -> void override {
            ++write_calls;
        }

        auto close() -> void override {}

        std::atomic<int> read_calls{0};
        std::atomic<int> write_calls{0};
    };

    auto transport = std::make_unique<BrokenTransport>();
    auto *raw = transport.get();
    std::atomic<int> fault_calls{0};
    std::atomic<int> reported_status{0};
    CANDispatcher::Options options;
    options.max_consecutive_errors = 3;
    options.error_backoff_base = std::chrono::milliseconds(1);
    options.error_backoff_max = std::chrono::milliseconds(2);
    options.on_fault = [&](const linkerbot::can::CANError &error) {
        reported_status.store(error.native_status().value_or(0));
        ++fault_calls;
        throw std::runtime_error("fault observer failure must be isolated");
    };
    CANDispatcher dispatcher(std::move(transport), options);

    REQUIRE(wait_until([&] { return dispatcher.get_health().state == TransportState::kFaulted; }));
    // Fault state can become visible before the observer has published its result.
    REQUIRE(wait_until([&] { return fault_calls.load() == 1; }));
    const auto health = dispatcher.get_health();
    REQUIRE(health.consecutive_errors == 3);
    REQUIRE(health.last_error_code == ErrorCode::kTransportIo);
    REQUIRE(health.last_error == "receive path is down");
    REQUIRE(fault_calls.load() == 1);
    REQUIRE(reported_status.load() == -23);
    REQUIRE(raw->read_calls.load() == 3);
    REQUIRE_THROWS_AS(dispatcher.send(make_frame(0x28, std::byte{0x32})), FaultedError);
    REQUIRE(raw->write_calls.load() == 0);
}

TEST_CASE("malformed frames do not fault the dispatcher", "[dispatcher][health]") {
    class MalformedTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            ++read_calls;
            throw ValidationError("malformed frame: BRS set without FD");
        }

        auto write(const Message &) -> void override {}
        auto close() -> void override {}

        std::atomic<int> read_calls{0};
    };

    auto transport = std::make_unique<MalformedTransport>();
    auto *raw = transport.get();
    std::atomic<int> fault_calls{0};
    CANDispatcher::Options options;
    options.max_consecutive_errors = 3;
    options.error_backoff_base = std::chrono::milliseconds(1);
    options.error_backoff_max = std::chrono::milliseconds(1);
    options.on_fault = [&](const linkerbot::can::CANError &) { ++fault_calls; };
    CANDispatcher dispatcher(std::move(transport), options);

    REQUIRE(wait_until([&] { return raw->read_calls.load() >= 5; }));
    const auto health = dispatcher.get_health();
    REQUIRE(health.state == TransportState::kOpen);
    REQUIRE(health.consecutive_errors == 0);
    REQUIRE(health.last_error_code == ErrorCode::kValidation);
    REQUIRE(fault_calls.load() == 0);
    REQUIRE_NOTHROW(dispatcher.send(make_frame(0x28, std::byte{0x32})));
}

TEST_CASE("health tracks generation subscribers and successful IO", "[dispatcher][health]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    raw->echo_cmd(0x28, 0x33, 1, 0x44);
    CANDispatcher dispatcher(std::move(transport));
    FrameCollector collector;
    dispatcher.subscribe(collector.callback());

    auto initial = dispatcher.get_health();
    REQUIRE(initial.state == TransportState::kOpen);
    REQUIRE(initial.generation == 1);
    REQUIRE(initial.subscriber_count == 1);
    REQUIRE_FALSE(initial.last_successful_tx.has_value());
    REQUIRE_FALSE(initial.last_successful_rx.has_value());

    dispatcher.send(make_frame(0x28, std::byte{0x33}));
    REQUIRE(collector.wait_for(1, kTimeout));
    const auto after_io = dispatcher.get_health();
    REQUIRE(after_io.last_successful_tx.has_value());
    REQUIRE(after_io.last_successful_rx.has_value());
}

TEST_CASE("send preserves typed backend error details", "[dispatcher][errors]") {
    class SendFailureTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }

        auto write(const Message &) -> void override {
            throw TransportIOError("USB transmit failed", std::make_error_code(std::errc::io_error),
                                   -17);
        }

        auto close() -> void override {}
    };

    CANDispatcher dispatcher(std::make_unique<SendFailureTransport>());
    try {
        dispatcher.send(make_frame(0x28, std::byte{0x34}));
        FAIL("send should throw");
    } catch (const TransportIOError &error) {
        REQUIRE(error.code() == ErrorCode::kTransportIo);
        REQUIRE(error.system_error() == std::make_error_code(std::errc::io_error));
        REQUIRE(error.native_status() == -17);
        REQUIRE(std::string(error.what()) == "USB transmit failed");
    }
    const auto health = dispatcher.get_health();
    REQUIRE(health.last_error_code == ErrorCode::kTransportIo);
    REQUIRE(health.last_error == "USB transmit failed");
}

TEST_CASE("stop from callback prevents later copied callbacks", "[dispatcher][stop]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));
    std::atomic<int> later_calls{0};

    dispatcher.subscribe([&](const Message &) { dispatcher.stop(); });
    dispatcher.subscribe([&](const Message &) { ++later_calls; });
    raw->inject(make_frame(0x28, std::byte{0x35}));

    REQUIRE(wait_until([&] { return dispatcher.get_health().state == TransportState::kClosed; }));
    REQUIRE(later_calls.load() == 0);
    REQUIRE_NOTHROW(dispatcher.stop());
}

TEST_CASE("concurrent stop is idempotent", "[dispatcher][stop]") {
    class CloseCountingTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }
        auto write(const Message &) -> void override {}
        auto close() -> void override {
            ++close_calls;
        }
        std::atomic<int> close_calls{0};
    };

    auto transport = std::make_unique<CloseCountingTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&] { dispatcher.stop(); });
    }
    for (auto &caller : callers) {
        caller.join();
    }
    REQUIRE(dispatcher.get_health().state == TransportState::kClosed);
    REQUIRE(raw->close_calls.load() == 1);
    REQUIRE_THROWS_AS(dispatcher.send(make_frame(0x28, std::byte{0x36})), ClosedError);
}

TEST_CASE("stop preserves close failure and a later stop can complete",
          "[dispatcher][stop][errors]") {
    class RetryCloseTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }
        auto write(const Message &) -> void override {}
        auto close() -> void override {
            if (++close_calls == 1) {
                throw TransportIOError("close failed",
                                       std::make_error_code(std::errc::device_or_resource_busy));
            }
        }
        std::atomic<int> close_calls{0};
    };

    auto transport = std::make_unique<RetryCloseTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));
    try {
        dispatcher.stop();
        FAIL("first stop should preserve close failure");
    } catch (const TransportIOError &error) {
        REQUIRE(error.system_error() == std::make_error_code(std::errc::device_or_resource_busy));
    }
    REQUIRE(dispatcher.get_health().state == TransportState::kFaulted);
    REQUIRE_NOTHROW(dispatcher.stop());
    REQUIRE(raw->close_calls.load() == 2);
    REQUIRE(dispatcher.get_health().state == TransportState::kClosed);
}

TEST_CASE("stop rejects new sends while draining an in-flight send", "[dispatcher][stop]") {
    class BlockingSendTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }

        auto write(const Message &) -> void override {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }

        auto close() -> void override {}

        auto wait_until_entered() -> void {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }

        auto release() -> void {
            const std::scoped_lock lock(mutex);
            released = true;
            cv.notify_all();
        }

        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool released = false;
    };

    auto transport = std::make_unique<BlockingSendTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));
    std::thread sender([&] { dispatcher.send(make_frame(0x28, std::byte{0x37})); });
    raw->wait_until_entered();

    std::thread stopper([&] { dispatcher.stop(); });
    REQUIRE(wait_until([&] { return dispatcher.get_health().state == TransportState::kStopping; }));
    const auto before = std::chrono::steady_clock::now();
    REQUIRE_THROWS_AS(dispatcher.send(make_frame(0x28, std::byte{0x38})), ClosedError);
    REQUIRE(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(50));

    raw->release();
    sender.join();
    stopper.join();
    REQUIRE(dispatcher.get_health().state == TransportState::kClosed);
}

TEST_CASE("stop's clear and a concurrent unsubscribe rendezvous on the same in-flight callback",
          "[dispatcher][unsubscribe][stop]") {
    // Regresses the clear()/unsubscribe() barrier gap: previously clear()
    // dropped its slot without waiting for an in-flight callback, so a
    // concurrent unsubscribe() for the same id -- one that arrives after
    // clear() already removed the entry from the live list -- would find
    // nothing and return immediately even though the callback was still
    // running, letting a caller free state the callback closes over while it
    // was still executing. Both stop() and unsubscribe() must observe the
    // callback fully finished before either returns.
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    std::atomic<bool> callback_finished{false};

    auto id = dispatcher.subscribe([&](const Message &) {
        {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        callback_finished.store(true, std::memory_order_release);
    });

    raw->inject(make_frame(0x28, std::byte{0x43}));
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }

    std::atomic<bool> unsubscribe_saw_finished{false};
    std::thread stopper([&] { dispatcher.stop(); });
    std::thread unsubscriber([&] {
        // May race stop()'s clear() for the very same entry: either
        // (true or false) is an acceptable return value, but it must never
        // return before the callback has actually finished.
        dispatcher.unsubscribe(id);
        unsubscribe_saw_finished.store(callback_finished.load(std::memory_order_acquire),
                                       std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        const std::scoped_lock lock(mutex);
        released = true;
    }
    cv.notify_all();

    stopper.join();
    unsubscriber.join();
    REQUIRE(callback_finished.load(std::memory_order_acquire));
    REQUIRE(unsubscribe_saw_finished.load(std::memory_order_acquire));
}

TEST_CASE("a subscriber disabled after repeated failures is reported via health",
          "[dispatcher][health][subscriber]") {
    // A callback that keeps throwing must not be retried forever while
    // silently reporting a fully healthy bus: after enough consecutive
    // failures it is disabled, stops being invoked, and both facts are
    // surfaced through get_health().
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));

    std::atomic<int> call_count{0};
    dispatcher.subscribe([&](const Message &) {
        ++call_count;
        throw std::runtime_error("subscriber blew up repeatedly");
    });

    for (int i = 0; i < 5; ++i) {
        raw->inject(make_frame(0x28, static_cast<std::byte>(i)));
    }

    REQUIRE(wait_until([&] { return call_count.load() >= 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(call_count.load() == 3); // disabled after exactly 3 consecutive failures.

    raw->inject(make_frame(0x28, std::byte{0x99}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(call_count.load() == 3); // disabled subscriber is never invoked again.

    const auto health = dispatcher.get_health();
    REQUIRE(health.disabled_subscriber_count == 1);
    REQUIRE(health.last_subscriber_error.find("subscriber blew up repeatedly") !=
            std::string::npos);
}

TEST_CASE("batch send validates pacing and uses a native batch when available",
          "[dispatcher][batch]") {
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    raw->set_prefer_batch(true);
    CANDispatcher dispatcher(std::move(transport));

    REQUIRE_NOTHROW(dispatcher.send_batch({}));

    size_t sent = 99;
    bool uncertain = true;
    REQUIRE_THROWS_AS(
        dispatcher.send_batch_until(
            {make_frame(0x28, std::byte{1})}, std::chrono::steady_clock::now() + kTimeout,
            std::chrono::microseconds(-1), std::chrono::microseconds(0), {}, sent, uncertain),
        ValidationError);
    REQUIRE(sent == 0);
    REQUIRE_FALSE(uncertain);

    const std::vector frames{make_frame(0x28, std::byte{2}), make_frame(0x29, std::byte{3})};
    REQUIRE_NOTHROW(dispatcher.send_batch_until(frames, std::chrono::steady_clock::now() + kTimeout,
                                                std::chrono::microseconds(0),
                                                std::chrono::microseconds(0), {}, sent, uncertain));
    REQUIRE(sent == frames.size());
    REQUIRE_FALSE(uncertain);
    const auto batches = raw->batch_writes();
    REQUIRE(batches.size() == 1);
    REQUIRE(batches[0].size() == 2);
    REQUIRE(batches[0][0].id == 0x28);
    REQUIRE(batches[0][1].id == 0x29);
}

TEST_CASE("batch send cancellation is observed while waiting for the physical send boundary",
          "[dispatcher][batch][concurrency]") {
    class BlockingSendTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }

        auto write(const Message &) -> void override {
            std::unique_lock lock(mutex_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [&] { return released_; });
        }

        auto close() -> void override {}

        auto wait_until_entered() -> void {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return entered_; });
        }

        auto release() -> void {
            const std::scoped_lock lock(mutex_);
            released_ = true;
            cv_.notify_all();
        }

      private:
        std::mutex mutex_;
        std::condition_variable cv_;
        bool entered_ = false;
        bool released_ = false;
    };

    auto transport = std::make_unique<BlockingSendTransport>();
    auto *raw = transport.get();
    CANDispatcher dispatcher(std::move(transport));
    std::thread sender([&] { dispatcher.send(make_frame(0x28, std::byte{1})); });
    raw->wait_until_entered();

    size_t sent = 0;
    bool uncertain = false;
    int cancellation_checks = 0;
    REQUIRE_THROWS_AS(dispatcher.send_batch_until(
                          {make_frame(0x29, std::byte{2})},
                          std::chrono::steady_clock::now() + kTimeout, std::chrono::microseconds(0),
                          std::chrono::microseconds(0), [&] { return ++cancellation_checks > 1; },
                          sent, uncertain),
                      linkerbot::transport::CancelledError);
    REQUIRE(cancellation_checks > 1);
    REQUIRE(sent == 0);
    REQUIRE_FALSE(uncertain);

    raw->release();
    sender.join();
}

TEST_CASE("receive error backoff saturates and a later frame still recovers the dispatcher",
          "[dispatcher][health]") {
    class RecoveringTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            const auto call = ++read_calls;
            if (call <= 4) {
                throw TransportIOError("transient receive failure");
            }
            if (call == 5) {
                return make_frame(0x28, std::byte{0x71});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }

        auto write(const Message &) -> void override {}
        auto close() -> void override {}

        std::atomic<int> read_calls{0};
    };

    auto transport = std::make_unique<RecoveringTransport>();
    auto *raw = transport.get();
    CANDispatcher::Options options;
    options.max_consecutive_errors = 6;
    options.error_backoff_base = std::chrono::milliseconds(1);
    options.error_backoff_max = std::chrono::milliseconds(2);
    CANDispatcher dispatcher(std::move(transport), options);
    FrameCollector collector;
    dispatcher.subscribe(collector.callback());

    REQUIRE(collector.wait_for(1, kTimeout));
    REQUIRE(raw->read_calls.load() >= 5);
    REQUIRE(dispatcher.get_health().state == TransportState::kOpen);
    REQUIRE(dispatcher.get_health().consecutive_errors == 0);
}

TEST_CASE("external stop owns finalization when a callback has already requested stop",
          "[dispatcher][stop][concurrency]") {
    class GatedFaultCloseTransport final : public linkerbot::can::CANTransport {
      public:
        auto read() -> std::optional<Message> override {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return read_released_; });
            throw TransportIOError("injected receive failure");
        }

        auto write(const Message &) -> void override {}

        auto close() -> void override {
            std::unique_lock lock(mutex_);
            close_entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [&] { return close_released_; });
        }

        auto release_read() -> void {
            const std::scoped_lock lock(mutex_);
            read_released_ = true;
            cv_.notify_all();
        }

        auto wait_until_close_entered() -> void {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return close_entered_; });
        }

        auto release_close() -> void {
            const std::scoped_lock lock(mutex_);
            close_released_ = true;
            cv_.notify_all();
        }

      private:
        std::mutex mutex_;
        std::condition_variable cv_;
        bool read_released_ = false;
        bool close_entered_ = false;
        bool close_released_ = false;
    };

    auto transport = std::make_unique<GatedFaultCloseTransport>();
    auto *raw = transport.get();

    std::mutex mutex;
    std::condition_variable cv;
    bool self_stop_returned = false;
    bool release_callback = false;
    std::atomic<bool> callback_released{false};
    CANDispatcher *dispatcher_ptr = nullptr;
    CANDispatcher::Options options;
    options.max_consecutive_errors = 1;
    options.on_fault = [&](const linkerbot::can::CANError &) {
        dispatcher_ptr->stop();
        std::unique_lock lock(mutex);
        self_stop_returned = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_callback; });
        callback_released.store(true, std::memory_order_release);
    };
    CANDispatcher dispatcher(std::move(transport), options);
    dispatcher_ptr = &dispatcher;

    raw->release_read();
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return self_stop_returned; });
    }
    std::thread external_stop([&] { dispatcher.stop(); });
    raw->wait_until_close_entered();

    {
        const std::scoped_lock lock(mutex);
        release_callback = true;
    }
    cv.notify_all();
    REQUIRE(wait_until([&] { return callback_released.load(std::memory_order_acquire); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    raw->release_close();
    external_stop.join();
    REQUIRE(dispatcher.get_health().state == TransportState::kClosed);
}
