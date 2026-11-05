#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace linkerbot;

namespace {
    struct TxRig {
        can::FakeTransport *fake;
        std::shared_ptr<transport::Session> session;
        TxRig() {
            auto backend = std::make_unique<can::FakeTransport>();
            fake = backend.get();
            session = transport::Session::own(std::move(backend));
        }
        ~TxRig() {
            fake->release_tx_gate();
            session->close();
        }
        [[nodiscard]] transport::SendReceipt
        block_writer(const std::string &endpoint = "blocker") const {
            fake->arm_tx_gate();
            transport::SendOptions options;
            options.endpoint = endpoint;
            auto receipt = session->submit({{{}}}, options);
            REQUIRE(fake->wait_until_tx_entered(2s));
            return receipt;
        }
    };
    transport::MessageBatch batch(size_t count, uint32_t id = 0x28) {
        transport::MessageBatch result;
        result.frames.resize(count);
        for (auto &frame : result.frames) {
            frame.id = id;
        }
        return result;
    }
    transport::SendOptions endpoint(const std::string &name) {
        transport::SendOptions options;
        options.endpoint = name;
        return options;
    }
    transport::SendOptions latest(const std::string &name, const std::string &family = "angles") {
        auto options = endpoint(name);
        options.policy = transport::SendPolicy::latest;
        options.family = family;
        return options;
    }
} // namespace

TEST_CASE("Session SDK capacity atomically admits frame groups separately from latest slots",
          "[session]") {
    TxRig rig;
    transport::SessionOptions options;
    options.separate_policy_capacity = true;
    options.fifo_unit = transport::CapacityUnit::kFrames;
    options.fifo_capacity = 7;
    options.latest_capacity = 1;
    rig.session->configure(options);
    auto blocker = rig.block_writer();
    auto group = rig.session->submit(batch(7), endpoint("arm"));
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), endpoint("arm")), can::QueueFullError);
    auto target = rig.session->submit(batch(2), latest("hand"));
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), latest("other")), can::QueueFullError);
    auto replacement = rig.session->replace_latest_if_pending(batch(3), latest("hand"));
    REQUIRE(replacement.has_value());
    REQUIRE(target.wait_result_until(transport::Clock::now()).outcome ==
            transport::SendOutcome::superseded);
    REQUIRE_THROWS_AS(target.wait(), transport::CancelledError);
    auto health = rig.session->health();
    REQUIRE(health.pending_fifo == 7);
    REQUIRE(health.pending_latest == 1);
    REQUIRE(health.in_flight);
    REQUIRE(health.in_flight_frames == 1);
    REQUIRE(health.accepted == 4);
    REQUIRE(health.superseded == 1);
    REQUIRE(health.rejected_full == 2);
    REQUIRE(health.oldest_pending_age.has_value());
    rig.fake->release_tx_gate();
    blocker.wait();
    group.wait();
    replacement->wait();
    REQUIRE(rig.fake->sent().size() == 11);
    REQUIRE(rig.session->health().pending_fifo == 0);
    REQUIRE(rig.session->health().dispatched == 3);
}

TEST_CASE("Session SDK batch capacity charges one unit for a multi-frame request", "[session]") {
    TxRig rig;
    transport::SessionOptions options;
    options.separate_policy_capacity = true;
    options.fifo_capacity = 1;
    rig.session->configure(options);
    auto blocker = rig.block_writer();
    auto group = rig.session->submit(batch(7));
    REQUIRE(rig.session->health().pending_fifo == 1);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1)), can::QueueFullError);
    rig.fake->release_tx_gate();
    blocker.wait();
    group.wait();
}

TEST_CASE("Session conditional latest replacement never queues behind a claimed request",
          "[session]") {
    TxRig rig;
    rig.fake->arm_tx_gate();
    auto claimed = rig.session->submit(batch(1), latest("hand"));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    const auto before = rig.session->health().accepted;
    REQUIRE_FALSE(rig.session->replace_latest_if_pending(batch(1), latest("hand")));
    REQUIRE(rig.session->health().accepted == before);
    REQUIRE_THROWS_AS(rig.session->replace_latest_if_pending(batch(1), endpoint("hand")),
                      can::ValidationError);
    const auto revision = claimed.notification()->revision();
    REQUIRE(claimed.result().claimed_order.has_value());
    rig.fake->release_tx_gate();
    claimed.wait();
    REQUIRE(claimed.notification()->wait_until(revision, transport::Clock::now()));
    REQUIRE(rig.fake->sent().size() == 1);
}

TEST_CASE("Session receipt result waits retain failure without changing legacy throwing waits",
          "[session]") {
    TxRig rig;
    rig.fake->fail_writes("expected failure");
    auto failed = rig.session->submit(batch(1));
    auto result = failed.wait_result_until(transport::Deadline::max());
    REQUIRE(result.outcome == transport::SendOutcome::failed);
    REQUIRE(result.error != nullptr);
    REQUIRE_THROWS_AS(failed.wait(), can::TransportIOError);
    transport::SendReceipt empty;
    REQUIRE_THROWS_AS(empty.wait_result_until(transport::Clock::now()), can::ValidationError);
}

TEST_CASE(
    "Session endpoint stop removes waiting work and drains a claimed batch without truncation",
    "[session]") {
    TxRig rig;
    rig.fake->arm_tx_gate();
    auto active = rig.session->submit(batch(3, 0x30), endpoint("left"));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    auto dropped = rig.session->submit(batch(1, 0x31), endpoint("left"));
    auto retained = rig.session->submit(batch(1, 0x32), endpoint("right"));
    // A bounded timeout leaves the admission gate closed, retaining drain ownership.
    REQUIRE_THROWS_AS(rig.session->stop_endpoint("left", transport::Clock::now()),
                      can::TimeoutError);
    REQUIRE(dropped.wait_result_until(transport::Clock::now()).outcome ==
            transport::SendOutcome::cancelled);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), endpoint("left")), can::ClosedError);
    REQUIRE_THROWS_AS(rig.session->resume_endpoint("left"), can::ValidationError);
    rig.fake->release_tx_gate();
    active.wait();
    REQUIRE(rig.session->stop_endpoint("left", transport::Deadline::max()) == 0);
    retained.wait();
    REQUIRE(rig.fake->sent().size() == 4);
    REQUIRE(active.result().sent_frames == 3);
    rig.session->resume_endpoint("left");
    rig.session->submit(batch(1), endpoint("left")).wait();
    REQUIRE(rig.session->stop_endpoint("unused", transport::Deadline::max()) == 0);
    rig.session->close();
    REQUIRE_THROWS_AS(rig.session->resume_endpoint("left"), can::ClosedError);
}

TEST_CASE("Session endpoint stop releases the queue lock while awaiting a claimed send",
          "[session]") {
    TxRig rig;
    auto active = rig.block_writer("left");
    auto dropped = rig.session->submit(batch(1), endpoint("left"));
    auto stopper = std::async(std::launch::async, [&] {
        return rig.session->stop_endpoint("left", transport::Clock::now() + 2s);
    });
    const auto cancelled = dropped.wait_result_until(transport::Clock::now() + 1s);
    auto other = rig.session->submit(batch(1), endpoint("right"));
    rig.fake->release_tx_gate();
    REQUIRE(cancelled.outcome == transport::SendOutcome::cancelled);
    REQUIRE(stopper.wait_for(2s) == std::future_status::ready);
    REQUIRE(stopper.get() == 1);
    active.wait();
    other.wait();
}

TEST_CASE("Session generation and oldest in-flight age are checked during admission", "[session]") {
    TxRig rig;
    auto stale = endpoint("arm");
    stale.expected_generation = rig.session->health().generation + 1;
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), stale), can::ClosedError);
    stale.expected_generation = rig.session->health().generation;
    stale.max_pending_age = 0us;
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), stale), can::ValidationError);
    auto active = rig.block_writer();
    stale.max_pending_age = 1us;
    // Gate entry proves the existing request is in flight, and its age remains
    // part of admission even though it no longer occupies waiting FIFO capacity.
    std::this_thread::sleep_for(1ms);
    REQUIRE_THROWS_AS(rig.session->submit(batch(7), stale), can::TimeoutError);
    REQUIRE(rig.session->health().rejected_stale == 1);
    rig.fake->release_tx_gate();
    active.wait();
    rig.session->submit(batch(1), stale).wait();
}

TEST_CASE("Session optional endpoint FIFO prevents priority from overtaking an earlier request",
          "[session]") {
    TxRig rig;
    transport::SessionOptions options;
    options.endpoint_fifo = true;
    options.aging_quantum = 50ms;
    rig.session->configure(options);
    auto blocker = rig.block_writer();
    auto first = endpoint("arm");
    first.priority = -10;
    auto later = endpoint("arm");
    later.priority = 100;
    auto unrelated = endpoint("hand");
    unrelated.priority = 1;
    auto a = rig.session->submit(batch(1, 0x11), first);
    auto b = rig.session->submit(batch(1, 0x12), later);
    auto c = rig.session->submit(batch(1, 0x13), unrelated);
    rig.fake->release_tx_gate();
    blocker.wait();
    a.wait();
    b.wait();
    c.wait();
    const auto sent = rig.fake->sent();
    REQUIRE(sent.size() == 4);
    REQUIRE(sent[1].id == 0x13);
    REQUIRE(sent[2].id == 0x11);
    REQUIRE(sent[3].id == 0x12);
}

TEST_CASE("Session SDK configuration rejects invalid options before use", "[session]") {
    TxRig rig;
    const auto defaults = rig.session->options();
    REQUIRE(defaults.tx_capacity == 2000);
    REQUIRE_FALSE(defaults.separate_policy_capacity);
    REQUIRE_FALSE(defaults.endpoint_fifo);
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto options = defaults;
        switch (invalid) {
        case 0:
            options.tx_capacity = 0;
            break;
        case 1:
            options.fifo_capacity = 0;
            break;
        case 2:
            options.latest_capacity = 0;
            break;
        case 3:
            options.aging_quantum = 0ms;
            break;
        default:
            options.send_period = -1us;
            break;
        }
        REQUIRE_THROWS_AS(rig.session->configure(options), can::ValidationError);
    }
    rig.session->submit(batch(1)).wait();
    REQUIRE_THROWS_AS(rig.session->configure(defaults), can::ValidationError);
}
