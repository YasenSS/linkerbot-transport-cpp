#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <future>
#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace linkerbot;

namespace {
    can::Message physical_frame(uint32_t id) {
        can::Message frame;
        frame.id = id;
        return frame;
    }
} // namespace

TEST_CASE("Physical start-only deadline completes a batch already inside the backend") {
    const bool start_only = GENERATE(false, true);
    auto ptr = std::make_unique<can::FakeTransport>();
    auto *fake = ptr.get();
    fake->arm_tx_gate();
    can::CANDispatcher dispatcher(std::move(ptr));
    size_t sent = 0;
    bool uncertain = false;
    const auto deadline = std::chrono::steady_clock::now() + 20ms;
    auto writer = std::async(std::launch::async, [&] {
        try {
            dispatcher.send_batch_until({physical_frame(1), physical_frame(2)}, deadline, 1ms, 0us,
                                        {}, sent, uncertain, start_only, 0us);
            return false;
        } catch (const can::TimeoutError &) {
            return true;
        }
    });
    const bool entered = fake->wait_until_tx_entered(1s);
    std::this_thread::sleep_until(deadline + 1ms);
    fake->release_tx_gate();
    const bool expired = writer.get();
    REQUIRE(entered);
    REQUIRE(expired == !start_only);
    REQUIRE(sent == (start_only ? 2u : 1u));
    REQUIRE_FALSE(uncertain);
}

TEST_CASE("Physical start-only deadline still bounds initial delay and lock acquisition") {
    auto ptr = std::make_unique<can::FakeTransport>();
    auto *fake = ptr.get();
    can::CANDispatcher dispatcher(std::move(ptr));
    size_t sent = 0;
    bool uncertain = false;
    SECTION("initial delay") {
        REQUIRE_THROWS_AS(dispatcher.send_batch_until({physical_frame(1)},
                                                      std::chrono::steady_clock::now() + 1ms, 0us,
                                                      20ms, {}, sent, uncertain, true, 0us),
                          can::TimeoutError);
        REQUIRE(sent == 0);
        REQUIRE(fake->sent().empty());
    }
    SECTION("another physical batch holds the lock") {
        fake->arm_tx_gate();
        auto writer = std::async(std::launch::async, [&] { dispatcher.send(physical_frame(1)); });
        const bool entered = fake->wait_until_tx_entered(1s);
        bool expired = false;
        try {
            dispatcher.send_batch_until({physical_frame(2)}, std::chrono::steady_clock::now() + 1ms,
                                        0us, 0us, {}, sent, uncertain, true, 0us);
        } catch (const can::TimeoutError &) {
            expired = true;
        }
        fake->release_tx_gate();
        writer.get();
        REQUIRE(entered);
        REQUIRE(expired);
        REQUIRE(sent == 0);
        REQUIRE(fake->sent().size() == 1);
    }
}

TEST_CASE("Physical frame period covers single-frame submissions and disables native merging") {
    auto ptr = std::make_unique<can::FakeTransport>();
    auto *fake = ptr.get();
    fake->set_prefer_batch(true);
    std::vector<std::chrono::steady_clock::time_point> writes;
    for (uint32_t id = 1; id <= 3; ++id) {
        fake->on_request(id, [&](const can::Message &) {
            writes.push_back(std::chrono::steady_clock::now());
            return std::vector<can::Message>{};
        });
    }
    can::CANDispatcher dispatcher(std::move(ptr));
    size_t sent = 0;
    bool uncertain = false;
    const auto deadline = std::chrono::steady_clock::time_point::max();
    dispatcher.send_batch_until({physical_frame(1), physical_frame(2)}, deadline, 0us, 0us, {},
                                sent, uncertain, true, 20ms);
    REQUIRE(sent == 2);
    dispatcher.send_batch_until({physical_frame(3)}, deadline, 0us, 0us, {}, sent, uncertain, true,
                                20ms);
    REQUIRE(sent == 1);
    REQUIRE(writes.size() == 3);
    // Handler timestamps follow the dispatcher's write-start anchor. Their
    // scheduling delay need not be identical, so leave a margin while still
    // detecting a missing period or a period reset between submissions.
    REQUIRE(writes[1] - writes[0] >= 15ms);
    REQUIRE(writes[2] - writes[1] >= 15ms);
    REQUIRE(std::chrono::steady_clock::now() - writes[2] >= 15ms);
    REQUIRE(fake->batch_writes().empty());
}

TEST_CASE("Physical final-frame cooldown preserves accepted delivery on cancel or expiry") {
    const bool cancel = GENERATE(false, true);
    auto ptr = std::make_unique<can::FakeTransport>();
    auto *fake = ptr.get();
    fake->set_prefer_batch(GENERATE(false, true));
    std::atomic<bool> cancelled{false};
    std::chrono::steady_clock::time_point first_write;
    fake->on_request(1, [&](const can::Message &) {
        first_write = std::chrono::steady_clock::now();
        cancelled.store(cancel);
        return std::vector<can::Message>{};
    });
    can::CANDispatcher dispatcher(std::move(ptr));
    size_t sent = 0;
    bool uncertain = false;
    dispatcher.send_batch_until(
        {physical_frame(1)}, std::chrono::steady_clock::now() + 2ms, 0us, 0us,
        [&] { return cancelled.load(); }, sent, uncertain, false, 10ms);
    REQUIRE(sent == 1);
    REQUIRE_FALSE(uncertain);
    dispatcher.send_batch_until({physical_frame(2)}, std::chrono::steady_clock::time_point::max(),
                                0us, 0us, {}, sent, uncertain, true, 0us);
    // first_write is a handler timestamp, later than the pacing anchor.
    REQUIRE(std::chrono::steady_clock::now() - first_write >= 8ms);
    REQUIRE(fake->sent().size() == 2);
}

TEST_CASE("Physical frame period cancellation leaves a known sent prefix") {
    auto ptr = std::make_unique<can::FakeTransport>();
    auto *fake = ptr.get();
    bool cancelled = false;
    fake->on_request(1, [&](const can::Message &) {
        cancelled = true;
        return std::vector<can::Message>{};
    });
    can::CANDispatcher dispatcher(std::move(ptr));
    size_t sent = 0;
    bool uncertain = false;
    REQUIRE_THROWS_AS(dispatcher.send_batch_until(
                          {physical_frame(1), physical_frame(2)},
                          std::chrono::steady_clock::time_point::max(), 0us, 0us,
                          [&] { return cancelled; }, sent, uncertain, true, 10ms),
                      transport::CancelledError);
    REQUIRE(sent == 1);
    REQUIRE_FALSE(uncertain);
}

TEST_CASE("Physical compatibility overload validates period and forwards through managed bus") {
    can::ManagedCANBus bus([] { return std::make_unique<can::FakeTransport>(); });
    bus.start();
    size_t sent = 0;
    bool uncertain = false;
    REQUIRE_THROWS_AS(bus.send_batch_until({physical_frame(1)},
                                           std::chrono::steady_clock::time_point::max(), 0us, 0us,
                                           {}, sent, uncertain, true, -1us),
                      can::ValidationError);
    bus.send_batch_until({physical_frame(1)}, std::chrono::steady_clock::time_point::max(), 0us,
                         0us, {}, sent, uncertain, true, 1us);
    REQUIRE(sent == 1);
    REQUIRE_FALSE(uncertain);
    bus.stop();
}
