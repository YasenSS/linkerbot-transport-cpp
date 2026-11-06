#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>
#include <vector>

using namespace std::chrono_literals;
using namespace linkerbot;

namespace {
    struct EdgeRig {
        can::FakeTransport *fake;
        std::shared_ptr<transport::Session> session;
        EdgeRig() {
            auto backend = std::make_unique<can::FakeTransport>();
            fake = backend.get();
            session = transport::Session::own(std::move(backend));
        }
        ~EdgeRig() {
            fake->release_tx_gate();
            session->close();
        }
    };
    transport::MessageBatch edge_batch(uint32_t id = 1) {
        can::Message message;
        message.id = id;
        return {{message}};
    }
    transport::SendOptions edge_endpoint(const char *name) {
        transport::SendOptions options;
        options.endpoint = name;
        return options;
    }
} // namespace

TEST_CASE("Session SDK fatal transmit threshold invalidates queued work without reopening",
          "[session]") {
    can::FakeTransport *fake = nullptr;
    auto session = transport::Session::create([&] {
        auto backend = std::make_unique<can::FakeTransport>();
        fake = backend.get();
        return backend;
    });
    struct Cleanup {
        can::FakeTransport *backend;
        std::shared_ptr<transport::Session> session;
        ~Cleanup() {
            backend->release_tx_gate();
            session->close();
        }
    } cleanup{fake, session};
    transport::SessionOptions config;
    config.max_consecutive_send_errors = 2;
    session->configure(config);
    auto route = session->subscribe_routes({{1, false}});
    fake->fail_writes("expected backend failure");
    fake->arm_tx_gate();
    auto first = session->submit(edge_batch());
    REQUIRE(fake->wait_until_tx_entered(2s));
    auto second = session->submit(edge_batch());
    auto unsent = session->submit(edge_batch());
    fake->release_tx_gate();
    REQUIRE(first.wait_result_until(transport::Clock::now() + 2s).outcome ==
            transport::SendOutcome::failed);
    REQUIRE(second.wait_result_until(transport::Clock::now() + 2s).outcome ==
            transport::SendOutcome::failed);
    REQUIRE(unsent.wait_result_until(transport::Clock::now() + 2s).outcome ==
            transport::SendOutcome::cancelled);
    const auto health = session->health();
    REQUIRE_FALSE(health.open);
    REQUIRE(health.failed == 2);
    REQUIRE(health.cancelled == 1);
    REQUIRE(health.consecutive_send_errors == 2);
    REQUIRE(route->health().closed);
    REQUIRE_THROWS_AS(session->submit(edge_batch()), can::ClosedError);
    REQUIRE_THROWS_AS(session->recover(health.generation), can::FaultedError);
}

TEST_CASE("Session SDK endpoint scheduling uses deadline then service history for equal ranks",
          "[session]") {
    EdgeRig rig;
    transport::SessionOptions config;
    config.endpoint_fifo = true;
    config.aging_quantum = 1h;
    rig.session->configure(config);
    rig.fake->arm_tx_gate();
    auto blocker = rig.session->submit(edge_batch(1), edge_endpoint("blocker"));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    auto a = edge_endpoint("a");
    auto b = edge_endpoint("b");
    auto first_a = rig.session->submit(edge_batch(2), a);
    auto second_a = rig.session->submit(edge_batch(3), a);
    auto first_b = rig.session->submit(edge_batch(4), b);
    rig.fake->release_tx_gate();
    blocker.wait();
    first_a.wait();
    second_a.wait();
    first_b.wait();
    const auto sent = rig.fake->sent();
    REQUIRE(sent.size() == 4);
    REQUIRE(sent[1].id == 2);
    REQUIRE(sent[2].id == 4);
    REQUIRE(sent[3].id == 3);
    rig.fake->arm_tx_gate();
    auto next_blocker = rig.session->submit(edge_batch(1), edge_endpoint("blocker"));
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    a.deadline = transport::Clock::now() + 10s;
    b.deadline = a.deadline - 1s;
    auto later_deadline = rig.session->submit(edge_batch(5), a);
    auto earlier_deadline = rig.session->submit(edge_batch(6), b);
    rig.fake->release_tx_gate();
    next_blocker.wait();
    later_deadline.wait();
    earlier_deadline.wait();
    const auto after = rig.fake->sent();
    REQUIRE(after.size() == 7);
    REQUIRE(after[5].id == 6);
    REQUIRE(after[6].id == 5);
}

TEST_CASE("Session endpoint drain rejects writer reentry before taking the state mutex",
          "[session]") {
    EdgeRig rig;
    bool rejected = false;
    rig.fake->on_request(1, [&](const can::Message &) {
        try {
            (void)rig.session->stop_endpoint("arm", transport::Clock::now());
        } catch (const can::ValidationError &) {
            rejected = true;
        }
        return std::vector<can::Message>{};
    });
    rig.session->submit(edge_batch(), edge_endpoint("arm")).wait();
    REQUIRE(rejected);
}

TEST_CASE("Session closed without routes still notifies a surviving bridge", "[session]") {
    EdgeRig rig;
    auto signal = rig.session->notification();
    const auto before = signal->revision();
    rig.session->close();
    REQUIRE(signal->wait_until(before, transport::Clock::now()));
    REQUIRE_THROWS_AS(rig.session->configure({}), can::ValidationError);
}

TEST_CASE("Session rejects invalid explicit formats before replacing a latest task", "[session]") {
    EdgeRig rig;
    rig.fake->arm_tx_gate();
    auto active = rig.session->submit(edge_batch());
    REQUIRE(rig.fake->wait_until_tx_entered(2s));
    auto options = edge_endpoint("arm");
    options.policy = transport::SendPolicy::latest;
    options.family = "target";
    auto old = rig.session->submit(edge_batch(2), options);
    const auto before = rig.session->health();
    // Deliberately invalid input for the admission rollback test.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid = static_cast<can::FrameFormat>(-1);
    for (const auto format : {can::FrameFormat::kClassic, invalid}) {
        auto batch = edge_batch(3);
        batch.frames[0].format = format;
        batch.frames[0].dlc = 9;
        REQUIRE_THROWS_AS(rig.session->submit(batch, options), can::ValidationError);
        CHECK(rig.session->health().accepted == before.accepted);
        CHECK(rig.session->health().pending_tx == before.pending_tx);
        CHECK_FALSE(old.completed());
    }
    rig.fake->release_tx_gate();
    active.wait();
    old.wait();
    const auto sent = rig.fake->sent();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].id == 2);
}
