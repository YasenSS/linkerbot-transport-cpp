#include <catch2/catch_test_macros.hpp>

#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <linkerbot/can/pool.hpp>
#include <linkerbot/transport/session.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using namespace linkerbot;

namespace {
    enum class Failure { kNone, kIo, kStandard, kUnknown, kTimeout, kValidation };
    void fail(Failure mode) {
        switch (mode) {
        case Failure::kIo:
            throw can::TransportIOError("injected I/O failure");
        case Failure::kStandard:
            throw std::runtime_error("injected standard failure");
        case Failure::kUnknown:
            throw 42;
        case Failure::kTimeout:
            throw can::TimeoutError("injected timeout");
        case Failure::kValidation:
            throw can::ValidationError("injected invalid frame");
        case Failure::kNone:
            break;
        }
    }
    struct FaultState {
        std::atomic<Failure> read{Failure::kNone};
        std::atomic<Failure> write{Failure::kNone};
        std::atomic<Failure> close{Failure::kNone};
        std::atomic<unsigned> writes{0};
        bool batch = false;
    };
    class FaultTransport : public can::CANTransport {
      public:
        explicit FaultTransport(std::shared_ptr<FaultState> state) : state_(std::move(state)) {}
        std::optional<can::Message> read() override {
            fail(state_->read.load());
            std::this_thread::sleep_for(1ms);
            return std::nullopt;
        }
        void write(const can::Message &) override {
            fail(state_->write.load());
            ++state_->writes;
        }
        [[nodiscard]] bool prefers_write_batch() const override {
            return state_->batch;
        }
        void close() override {
            fail(state_->close.load());
        }

      private:
        std::shared_ptr<FaultState> state_;
    };
    template <typename Predicate> bool await(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }
    can::Message message() {
        can::Message m;
        m.id = 0x28;
        m.dlc = 1;
        return m;
    }
} // namespace

TEST_CASE("dispatcher classifies standard and nonstandard backend failures",
          "[dispatcher][faults]") {
    for (auto mode : {Failure::kIo, Failure::kStandard, Failure::kUnknown}) {
        auto state = std::make_shared<FaultState>();
        auto backend = std::make_unique<FaultTransport>(state);
        can::CANDispatcher dispatcher(std::move(backend));
        state->write = mode;
        REQUIRE_THROWS_AS(dispatcher.send(message()), can::CANError);
        REQUIRE_THROWS_AS(dispatcher.send_batch({message(), message()}), can::CANError);
        REQUIRE(dispatcher.get_health().last_error_code.has_value());
        REQUIRE_FALSE(dispatcher.get_health().last_error.empty());
        for (bool batch : {false, true}) {
            state->batch = batch;
            size_t sent = 100;
            bool uncertain = false;
            REQUIRE_THROWS_AS(dispatcher.send_batch_until({message(), message()},
                                                          std::chrono::steady_clock::now() + 2s,
                                                          0us, 0us, {}, sent, uncertain),
                              can::CANError);
            REQUIRE(sent == 0);
            REQUIRE(uncertain);
        }
        state->write = Failure::kNone;
        dispatcher.send_batch({message(), message()});
        REQUIRE(state->writes == 2);
        REQUIRE_THROWS_AS(dispatcher.subscribe({}), can::ValidationError);
        state->close = mode;
        REQUIRE_THROWS_AS(dispatcher.stop(), can::CANError);
        REQUIRE(dispatcher.get_health().state == can::TransportState::kFaulted);
        state->close = Failure::kNone;
        REQUIRE_NOTHROW(dispatcher.stop());
    }
}

TEST_CASE("dispatcher receive exceptions preserve fault diagnostics and isolate fault callbacks",
          "[dispatcher][faults]") {
    for (auto mode : {Failure::kStandard, Failure::kUnknown}) {
        auto state = std::make_shared<FaultState>();
        state->read = mode;
        can::CANDispatcher::Options options;
        options.max_consecutive_errors = 2;
        options.error_backoff_base = 1ms;
        options.error_backoff_max = 1ms;
        std::atomic<unsigned> faults{0};
        options.on_fault = [&](const can::CANError &) {
            ++faults;
            throw 42;
        };
        can::CANDispatcher dispatcher(std::make_unique<FaultTransport>(state), options);
        REQUIRE(await([&] { return faults.load() == 1; }));
        REQUIRE(dispatcher.get_health().state == can::TransportState::kFaulted);
        REQUIRE((dispatcher.get_health().last_error_code == can::ErrorCode::kTransportIo ||
                 dispatcher.get_health().last_error_code == can::ErrorCode::kInternal));
        REQUIRE_THROWS_AS(dispatcher.send(message()), can::FaultedError);
    }
}

TEST_CASE("dispatcher excludes validation and timeout errors from fatal receive count",
          "[dispatcher][faults]") {
    for (auto mode : {Failure::kValidation, Failure::kTimeout}) {
        auto state = std::make_shared<FaultState>();
        state->read = mode;
        can::CANDispatcher dispatcher(std::make_unique<FaultTransport>(state));
        REQUIRE(await([&] { return !dispatcher.get_health().last_error.empty(); }));
        REQUIRE(dispatcher.get_health().connected());
        REQUIRE(dispatcher.get_health().consecutive_errors == 0);
        state->read = Failure::kNone;
        dispatcher.stop();
    }
}

TEST_CASE("dispatcher self stop preserves close failures for external retry",
          "[dispatcher][faults]") {
    for (auto mode : {Failure::kIo, Failure::kStandard, Failure::kUnknown}) {
        auto state = std::make_shared<FaultState>();
        // Fake replies put stop on the receive thread; the close operation is
        // deliberately supplied by a transport with an independent fault state.
        class EchoFaultTransport : public can::CANTransport {
          public:
            can::FakeTransport fake;
            std::shared_ptr<FaultState> state;
            explicit EchoFaultTransport(std::shared_ptr<FaultState> s) : state(std::move(s)) {}
            std::optional<can::Message> read() override {
                return fake.read();
            }
            void write(const can::Message &m) override {
                fake.write(m);
            }
            void close() override {
                fail(state->close.load());
                fake.close();
            }
        };
        auto backend = std::make_unique<EchoFaultTransport>(state);
        auto *raw = backend.get();
        can::CANDispatcher dispatcher(std::move(backend));
        dispatcher.subscribe([&](const can::Message &) { dispatcher.stop(); });
        state->close = mode;
        raw->fake.inject(message());
        REQUIRE(
            await([&] { return dispatcher.get_health().state == can::TransportState::kFaulted; }));
        state->close = Failure::kNone;
        REQUIRE_NOTHROW(dispatcher.stop());
    }
}

TEST_CASE("dispatcher disables nonstandard throwing callbacks and destructor contains close errors",
          "[dispatcher][faults]") {
    auto backend = std::make_unique<can::FakeTransport>();
    auto *raw = backend.get();
    can::CANDispatcher dispatcher(std::move(backend));
    dispatcher.subscribe([](const can::Message &) { throw 42; });
    for (int i = 0; i < 3; ++i) {
        raw->inject(message());
    }
    REQUIRE(await([&] { return dispatcher.get_health().disabled_subscriber_count == 1; }));
    REQUIRE_FALSE(dispatcher.get_health().last_subscriber_error.empty());
    auto state = std::make_shared<FaultState>();
    {
        can::CANDispatcher closing(std::make_unique<FaultTransport>(state));
        state->close = Failure::kStandard;
    }
}

TEST_CASE("managed bus validates configuration and retains factory error categories",
          "[managed_bus][faults]") {
    auto factory = [] { return std::make_unique<can::FakeTransport>(); };
    REQUIRE_THROWS_AS(can::ManagedCANBus({}), can::ValidationError);
    for (int invalid = 0; invalid < 3; ++invalid) {
        can::ManagedCANBus::Options options;
        if (invalid == 0) {
            options.send_queue_size = 0;
        }
        if (invalid == 1) {
            options.send_batch_max = 0;
        }
        if (invalid == 2) {
            options.send_interval = -1ms;
        }
        REQUIRE_THROWS_AS(can::ManagedCANBus(factory, options), can::ValidationError);
    }
    REQUIRE_THROWS_AS(can::SendReceipt(nullptr), can::ValidationError);
    for (auto mode : {Failure::kStandard, Failure::kUnknown}) {
        can::ManagedCANBus bus([=]() -> std::unique_ptr<can::CANTransport> {
            fail(mode);
            return {};
        });
        REQUIRE_THROWS(bus.start());
        REQUIRE(bus.get_health().last_error_code == can::ErrorCode::kInternal);
        REQUIRE_FALSE(bus.get_health().last_error.empty());
        REQUIRE_THROWS_AS(bus.subscribe({}), can::ValidationError);
    }
    can::ManagedCANBus null_bus([]() -> std::unique_ptr<can::CANTransport> { return {}; });
    REQUIRE_THROWS_AS(null_bus.start(), can::ValidationError);
}

TEST_CASE("managed receipt failures remain observable through timed and untimed waits",
          "[managed_bus][faults]") {
    auto state = std::make_shared<FaultState>();
    state->write = Failure::kStandard;
    can::ManagedCANBus::Options options;
    options.send_interval = 1ms;
    can::ManagedCANBus bus([&] { return std::make_unique<FaultTransport>(state); }, options);
    bus.start();
    auto receipt = bus.queue_send(message());
    REQUIRE_THROWS_AS(receipt.wait_for(2s), can::TransportIOError);
    REQUIRE(receipt.completed());
    REQUIRE_THROWS_AS(receipt.wait(), can::TransportIOError);
    state->write = Failure::kNone;
    auto success = bus.queue_send(message());
    REQUIRE_NOTHROW(success.wait_for(2s));
    REQUIRE(state->writes == 1);
    bus.stop();
}

TEST_CASE("pool lease move assignment closes the old owner and retains the new key",
          "[pool][faults]") {
    can::SharedCANBusPool pool;
    can::CANBusKey first;
    first.endpoint = "one";
    can::CANBusKey second;
    second.endpoint = "two";
    auto factory = [] { return std::make_unique<can::FakeTransport>(); };
    auto a = pool.acquire(first, factory);
    auto b = pool.acquire(second, factory);
    a = std::move(b);
    REQUIRE(pool.lease_count(first) == 0);
    REQUIRE(pool.lease_count(second) == 1);
    // NOLINTNEXTLINE(bugprone-use-after-move) -- the moved-from state is the contract under test.
    REQUIRE_THROWS_AS(b.bus(), can::ClosedError);
    a.close();
    REQUIRE(pool.active_keys().empty());
    REQUIRE_THROWS_AS(pool.acquire(first, {}), can::ValidationError);
}

TEST_CASE("pool coordinates concurrent opens and retries after an initial factory failure",
          "[pool][faults]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    std::atomic<int> opens{0};
    std::promise<void> opening;
    auto entered = opening.get_future();
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto factory = [&]() -> std::unique_ptr<can::CANTransport> {
        if (++opens == 1) {
            opening.set_value();
            if (gate.wait_for(2s) != std::future_status::ready) {
                throw can::TimeoutError("test open gate expired");
            }
            throw can::TransportIOError("injected first open failure");
        }
        return std::make_unique<can::FakeTransport>();
    };
    auto first = std::async(std::launch::async, [&] {
        try {
            auto lease = pool.acquire(key, factory);
            return false;
        } catch (const can::TransportIOError &) {
            return true;
        }
    });
    const auto status = entered.wait_for(2s);
    auto second = std::async(std::launch::async, [&] { return pool.acquire(key, factory); });
    const auto blocked = second.wait_for(20ms);
    release.set_value();
    REQUIRE(status == std::future_status::ready);
    REQUIRE(blocked == std::future_status::timeout);
    REQUIRE(first.get());
    auto lease = second.get();
    REQUIRE(opens == 2);
    REQUIRE(pool.lease_count(key) == 1);
    lease.close();
    REQUIRE(pool.active_keys().empty());
}
