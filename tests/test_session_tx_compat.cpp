#include "optional_value.hpp"

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
    transport::MessageBatch batch(size_t count) {
        transport::MessageBatch result;
        result.frames.resize(count);
        for (auto &frame : result.frames) {
            frame.id = 0x28;
        }
        return result;
    }
    transport::MessageBatch with_id(transport::MessageBatch result, uint32_t id) {
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
    transport::SendOptions latest(const std::string &name) {
        auto options = endpoint(name);
        options.policy = transport::SendPolicy::latest;
        options.family = "angles";
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
    test_support::require_value(replacement).wait();
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
    auto active = rig.session->submit(with_id(batch(3), 0x30), endpoint("left"));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    auto dropped = rig.session->submit(with_id(batch(1), 0x31), endpoint("left"));
    auto retained = rig.session->submit(with_id(batch(1), 0x32), endpoint("right"));
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

TEST_CASE("Session endpoint age excludes unrelated claimed and queued work", "[session]") {
    TxRig rig;
    auto active = rig.block_writer("hand");
    auto waiting = rig.session->submit(batch(1), endpoint("hand"));
    auto arm = endpoint("arm");
    arm.max_pending_age = 1us;
    std::this_thread::sleep_for(1ms);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), arm), can::TimeoutError);
    arm.pending_age_scope = transport::PendingAgeScope::kEndpoint;
    auto accepted = rig.session->submit(batch(7), arm);
    std::this_thread::sleep_for(1ms);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), arm), can::TimeoutError);
    REQUIRE(rig.session->health().rejected_stale == 2);
    rig.fake->release_tx_gate();
    active.wait();
    waiting.wait();
    accepted.wait();
}

TEST_CASE("Session endpoint age includes its own claimed work", "[session]") {
    TxRig rig;
    auto active = rig.block_writer("arm");
    auto arm = endpoint("arm");
    arm.max_pending_age = 1us;
    arm.pending_age_scope = transport::PendingAgeScope::kEndpoint;
    std::this_thread::sleep_for(1ms);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), arm), can::TimeoutError);
    auto hand = arm;
    hand.endpoint = "hand";
    auto accepted = rig.session->submit(batch(1), hand);
    rig.fake->release_tx_gate();
    active.wait();
    accepted.wait();
}

TEST_CASE("Session admission domains share request limits and isolate frame budgets", "[session]") {
    TxRig rig;
    auto blocker = rig.block_writer();
    auto hands = std::make_shared<transport::AdmissionDomain>(
        transport::AdmissionLimits{1, 1, transport::CapacityUnit::kBatches});
    auto arms = std::make_shared<transport::AdmissionDomain>(
        transport::AdmissionLimits{7, 1, transport::CapacityUnit::kFrames});
    auto left = endpoint("left");
    left.admission_domain = hands;
    auto right = endpoint("right");
    right.admission_domain = hands;
    auto arm = endpoint("arm");
    arm.admission_domain = arms;
    auto hand_request = rig.session->submit(batch(3), left);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), right), can::QueueFullError);
    auto targets = rig.session->submit(batch(7), arm);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), arm), can::QueueFullError);
    auto replaceable = latest("left");
    replaceable.admission_domain = hands;
    auto old = rig.session->submit(batch(1), replaceable);
    auto replacement = rig.session->replace_latest_if_pending(batch(2), replaceable);
    REQUIRE(replacement.has_value());
    REQUIRE(old.wait_result_until(transport::Clock::now()).outcome ==
            transport::SendOutcome::superseded);
    replaceable.endpoint = "right";
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), replaceable), can::QueueFullError);
    rig.session->cancel_endpoint("left");
    auto released = rig.session->submit(batch(1), right);
    rig.fake->release_tx_gate();
    blocker.wait();
    targets.wait();
    released.wait();
    REQUIRE(hand_request.completed());
    REQUIRE(rig.session->health().rejected_full == 3);
}

TEST_CASE("Session global capacity still limits independent admission domains", "[session]") {
    TxRig rig;
    transport::SessionOptions settings;
    settings.tx_capacity = 1;
    rig.session->configure(settings);
    auto blocker = rig.block_writer();
    auto first = endpoint("first");
    first.admission_domain =
        std::make_shared<transport::AdmissionDomain>(transport::AdmissionLimits{});
    auto second = endpoint("second");
    second.admission_domain =
        std::make_shared<transport::AdmissionDomain>(transport::AdmissionLimits{});
    auto queued = rig.session->submit(batch(1), first);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), second), can::QueueFullError);
    rig.fake->release_tx_gate();
    blocker.wait();
    queued.wait();
}

TEST_CASE("Admission domains validate immutable limits", "[session]") {
    REQUIRE_THROWS_AS(transport::AdmissionDomain(transport::AdmissionLimits{0, 1}),
                      can::ValidationError);
    REQUIRE_THROWS_AS(transport::AdmissionDomain(transport::AdmissionLimits{1, 0}),
                      can::ValidationError);
    REQUIRE_THROWS_AS(transport::AdmissionDomain(transport::AdmissionLimits{
                          1, 1, static_cast<transport::CapacityUnit>(99)}),
                      can::ValidationError);
    const transport::AdmissionDomain domain({7, 2, transport::CapacityUnit::kFrames});
    REQUIRE(domain.limits().fifo_capacity == 7);
    REQUIRE(domain.limits().latest_capacity == 2);
    REQUIRE(domain.limits().fifo_unit == transport::CapacityUnit::kFrames);
    TxRig rig;
    auto invalid = endpoint("arm");
    invalid.pending_age_scope = static_cast<transport::PendingAgeScope>(99);
    REQUIRE_THROWS_AS(rig.session->submit(batch(1), invalid), can::ValidationError);
}

TEST_CASE("Device sessions isolate admission routes and retryable drain", "[session]") {
    TxRig rig;
    REQUIRE_THROWS_AS(rig.session->attach_device(""), can::ValidationError);
    auto left = rig.session->attach_device("left");
    auto right = rig.session->attach_device("right");
    REQUIRE_THROWS_AS(rig.session->submit(batch(1)), can::ValidationError);
    REQUIRE(left->generation() == rig.session->health().generation);
    REQUIRE_FALSE(left->closed());
    REQUIRE_THROWS_AS(rig.session->attach_device("left"), can::ValidationError);
    auto left_inbox = left->subscribe_routes({{0x28, false}});
    auto right_inbox = right->subscribe_routes({{0x29, false}});
    rig.fake->arm_tx_gate();
    auto active = left->submit(batch(3));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    auto pending = left->submit(batch(1));
    auto retained = right->submit(batch(1));
    REQUIRE_THROWS_AS(left->close_until(transport::Clock::now()), can::TimeoutError);
    REQUIRE_FALSE(left->closed());
    REQUIRE_THROWS_AS(left->submit(batch(1)), can::ClosedError);
    REQUIRE_THROWS_AS(left->subscribe_routes({{0x30, false}}), can::ClosedError);
    REQUIRE_THROWS_AS(rig.session->attach_device("left"), can::ValidationError);
    REQUIRE(pending.completed());
    rig.fake->release_tx_gate();
    active.wait();
    retained.wait();
    left->close_until(transport::Deadline::max());
    left->close_until(transport::Clock::now());
    REQUIRE(left->closed());
    REQUIRE(left_inbox->health().closed);
    REQUIRE_FALSE(right_inbox->health().closed);
    REQUIRE(rig.session->health().open);
    auto replacement = rig.session->attach_device("left");
    replacement->subscribe_routes({{0x28, false}});
    replacement->submit(batch(1)).wait();
    right->submit(batch(1)).wait();
}

TEST_CASE("Device sessions retain native latest and generation checks", "[session]") {
    can::FakeTransport *backend = nullptr;
    auto session = transport::Session::create([&] {
        auto fresh = std::make_unique<can::FakeTransport>();
        backend = fresh.get();
        return fresh;
    });
    auto device = session->attach_device("arm");
    auto options = latest("ignored");
    REQUIRE_FALSE(device->replace_latest_if_pending(batch(1), options));
    REQUIRE_THROWS_AS(device->replace_latest_if_pending(batch(1), {}), can::ValidationError);
    backend->arm_tx_gate();
    auto active = device->submit(batch(1));
    REQUIRE(backend->wait_until_tx_entered(2s));
    auto old = device->submit(batch(1), options);
    auto replacement = device->replace_latest_if_pending(batch(2), options);
    REQUIRE(replacement.has_value());
    REQUIRE(old.wait_result_until(transport::Clock::now()).outcome ==
            transport::SendOutcome::superseded);
    backend->release_tx_gate();
    active.wait();
    test_support::require_value(replacement).wait();
    const auto generation = session->health().generation;
    REQUIRE(session->recover(generation) != generation);
    REQUIRE_THROWS_AS(device->submit(batch(1)), can::ClosedError);
    REQUIRE_THROWS_AS(device->subscribe_routes({{0x28, false}}), can::ClosedError);
    device->close_until(transport::Deadline::max());
    session->close();
    REQUIRE_THROWS_AS(session->attach_device("closed"), can::ClosedError);
}

TEST_CASE("Device attachment cannot reinterpret an active legacy Session", "[session]") {
    TxRig rig;
    rig.session->submit(batch(1)).wait();
    REQUIRE_THROWS_AS(rig.session->attach_device("arm"), can::ValidationError);
    rig.session->submit(batch(1)).wait();
}

TEST_CASE("Device close rejects writer callbacks before changing admission", "[session]") {
    TxRig rig;
    auto device = rig.session->attach_device("arm");
    bool rejected = false;
    rig.fake->on_request(0x28, [&](const can::Message &) {
        try {
            device->close_until(transport::Clock::now());
        } catch (const can::ValidationError &) {
            rejected = true;
        }
        return std::vector<can::Message>{};
    });
    device->submit(batch(1)).wait();
    REQUIRE(rejected);
    REQUIRE_FALSE(device->closed());
    device->submit(batch(1)).wait();
}

TEST_CASE("Concurrent device close observes its own waiting deadline", "[session]") {
    TxRig rig;
    auto device = rig.session->attach_device("arm");
    rig.fake->arm_tx_gate();
    auto active = device->submit(batch(1));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    auto queued = device->submit(batch(1));
    auto closer =
        std::async(std::launch::async, [&] { device->close_until(transport::Deadline::max()); });
    struct ReleaseGate {
        can::FakeTransport *backend;
        ~ReleaseGate() {
            backend->release_tx_gate();
        }
    } release{rig.fake};
    REQUIRE(queued.wait_result_until(transport::Clock::now() + 2s).outcome ==
            transport::SendOutcome::cancelled);
    REQUIRE_THROWS_AS(device->close_until(transport::Clock::now()), can::TimeoutError);
    rig.fake->release_tx_gate();
    closer.get();
    active.wait();
    REQUIRE(device->closed());
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
    auto a = rig.session->submit(with_id(batch(1), 0x11), first);
    auto b = rig.session->submit(with_id(batch(1), 0x12), later);
    auto c = rig.session->submit(with_id(batch(1), 0x13), unrelated);
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
