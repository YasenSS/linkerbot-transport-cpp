#include "optional_value.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>
#include <stdexcept>

using namespace std::chrono_literals;
using namespace linkerbot;
using linkerbot::test_support::require_value;

namespace {
    struct ReceiveRig {
        can::FakeTransport *fake;
        std::shared_ptr<transport::Session> session;
        ReceiveRig() {
            auto backend = std::make_unique<can::FakeTransport>();
            fake = backend.get();
            session = transport::Session::own(std::move(backend));
        }
        ~ReceiveRig() {
            session->close();
        }
    };

    can::Message received_frame(uint32_t id, bool extended = false) {
        can::Message message;
        message.id = id;
        message.is_extended = extended;
        message.dlc = 1;
        message.data[0] = 0xc0;
        return message;
    }

    template <typename Predicate>
    bool await_state(const std::shared_ptr<transport::ChangeSignal> &signal, Predicate ready) {
        const auto deadline = transport::Clock::now() + 2s;
        for (;;) {
            const auto observed = signal->revision();
            if (ready()) {
                return true;
            }
            if (!signal->wait_until(observed, deadline)) {
                return ready();
            }
        }
    }
} // namespace

TEST_CASE("Discovery observation cannot consume an owner's route or receive budget", "[session]") {
    ReceiveRig rig;
    auto config = rig.session->options();
    config.max_route_keys = 4;
    config.max_pending_rx = 1;
    rig.session->configure(config);
    auto device = rig.session->attach_device("hand");
    auto discovery = rig.session->attach_device("discovery");
    auto owner = device->subscribe_routes({{0x28, false}}, 2);
    auto tap = discovery->observe_rules({{0x20, 0x7f0, false}}, 1);
    REQUIRE(rig.session->health().route_count == 2);
    rig.fake->inject(received_frame(0x28));
    const auto normal = require_value(owner->receive_until(transport::Clock::now() + 2s));
    REQUIRE(await_state(tap->notification(), [&] { return tap->health().pending == 1; }));
    REQUIRE(rig.session->health().pending_rx == 0);
    rig.fake->inject(received_frame(0x28));
    const auto second = require_value(owner->receive_until(transport::Clock::now() + 2s));
    REQUIRE(await_state(tap->notification(), [&] { return tap->health().overflow == 1; }));
    const auto observed = require_value(tap->try_receive());
    REQUIRE(observed.compatibility_sequence == normal.compatibility_sequence);
    REQUIRE(observed.received_order == normal.received_order);
    REQUIRE(second.received_order > normal.received_order);
    tap->close();
    auto replacement = discovery->observe_rules({{0x20, 0x7f0, false}}, 1);
    rig.fake->inject(received_frame(0x21));
    REQUIRE(require_value(replacement->receive_until(transport::Clock::now() + 2s)).message.id ==
            0x21);
    discovery->close_until(transport::Deadline::max());
    REQUIRE_FALSE(owner->health().closed);
    REQUIRE_THROWS_AS(discovery->observe_rules({{0, 0, false}}, 1), can::ClosedError);
    auto terminal = device->observe_rules({{0, 0, false}}, 1);
    rig.session->close();
    REQUIRE_THROWS_AS(terminal->try_receive(), can::ClosedError);
}

TEST_CASE("Masked device routes reject legacy extractors and physical closure", "[session]") {
    ReceiveRig rig;
    rig.session->set_route_key_extractor([](const can::Message &message) {
        return transport::RouteKey{message.id, message.is_extended};
    });
    auto device = rig.session->attach_device("masked");
    REQUIRE_THROWS_AS(device->subscribe_rules({{0, 0x7ff, false}}), can::ValidationError);
    rig.session->close();
    REQUIRE_THROWS_AS(device->subscribe_rules({{0, 0x7ff, false}}), can::ClosedError);
}

TEST_CASE("Device masked routes compose with exact routes and reject wire overlap", "[session]") {
    ReceiveRig rig;
    auto arm = rig.session->attach_device("arm");
    auto hand = rig.session->attach_device("hand");
    auto masked = arm->subscribe_rules({{0x3d00, 0xff00, true}}, 2);
    auto exact = hand->subscribe_routes({{0x28, false}}, 2);
    REQUIRE(rig.session->health().route_count == 2);
    REQUIRE_THROWS_AS(hand->subscribe_routes({{0x123d01, true}}), can::ValidationError);
    REQUIRE_THROWS_AS(hand->subscribe_rules({{0x3d00, 0xff00, true}}), can::ValidationError);
    REQUIRE_THROWS_AS(hand->subscribe_rules({{0x28, 0x7ff, false}}), can::ValidationError);
    REQUIRE_THROWS_AS(hand->subscribe_rules({}), can::ValidationError);
    REQUIRE_THROWS_AS(hand->subscribe_rules({{0, 0x800, false}}), can::ValidationError);
    REQUIRE_THROWS_AS(hand->subscribe_rules({{1, 0, false}}), can::ValidationError);
    REQUIRE_THROWS_AS(hand->subscribe_rules({{0, 0, false}, {1, 0x7ff, false}}),
                      can::ValidationError);
    rig.fake->inject(received_frame(0x123d01, true));
    const auto routed = require_value(masked->receive_until(transport::Clock::now() + 2s));
    REQUIRE(routed.message.id == 0x123d01);
    REQUIRE(routed.route_key == transport::RouteKey{0x3d00, true});
    REQUIRE(routed.channel_instance == arm->health().channel_instance);
    rig.fake->inject(received_frame(0x28));
    const auto other = require_value(exact->receive_until(transport::Clock::now() + 2s));
    REQUIRE(other.compatibility_sequence > routed.compatibility_sequence);
    arm->close_until(transport::Deadline::max());
    REQUIRE(rig.session->health().route_count == 1);
    auto replacement = hand->subscribe_rules({{0x3d00, 0xff00, true}});
    REQUIRE(rig.session->health().route_count == 2);
    replacement->close();
    REQUIRE(rig.session->health().route_count == 1);
}

TEST_CASE("Channel receive budget spans device inboxes and overflow leaves sequence gaps",
          "[session]") {
    ReceiveRig rig;
    auto options = rig.session->options();
    options.max_route_keys = 2;
    options.max_pending_rx = 1;
    rig.session->configure(options);
    auto first = rig.session->attach_device("first");
    auto second = rig.session->attach_device("second");
    auto a = first->subscribe_routes({{1, false}}, 4);
    auto b = second->subscribe_rules({{2, 0x7ff, false}}, 4);
    REQUIRE_THROWS_AS(first->subscribe_routes({{3, false}}), can::QueueFullError);
    REQUIRE_THROWS_AS(second->subscribe_rules({{3, 0x7ff, false}}), can::QueueFullError);
    REQUIRE(rig.session->health().route_registration_rejections == 2);
    rig.fake->inject(received_frame(1));
    REQUIRE(await_state(a->notification(), [&] { return a->health().pending == 1; }));
    rig.fake->inject(received_frame(2));
    REQUIRE(await_state(b->notification(), [&] { return b->health().overflow == 1; }));
    const auto initial = require_value(a->try_receive());
    REQUIRE(rig.session->health().pending_rx == 0);
    REQUIRE(rig.session->health().max_pending_rx == 1);
    rig.fake->inject(received_frame(2));
    const auto following = require_value(b->receive_until(transport::Clock::now() + 2s));
    REQUIRE(following.compatibility_sequence == initial.compatibility_sequence + 2);
    rig.fake->inject(received_frame(2));
    REQUIRE(await_state(b->notification(), [&] { return b->health().pending == 1; }));
    b->close();
    REQUIRE(rig.session->health().pending_rx == 0);
    REQUIRE(rig.session->health().dropped_rx_on_close == 1);
    REQUIRE(rig.session->health().routed_rx_frames == 3);
}

TEST_CASE("Session change signals coalesce without losing a pre-wait notification", "[session]") {
    transport::ChangeSignal signal;
    const auto initial = signal.revision();
    REQUIRE_FALSE(signal.wait_until(initial, transport::Clock::now()));
    signal.notify();
    signal.notify();
    REQUIRE(signal.revision() == initial + 2);
    REQUIRE(signal.wait_until(initial, transport::Clock::now()));
    const auto observed = signal.revision();
    auto waiter = std::async(std::launch::async, [&] {
        return signal.wait_until(observed, transport::Deadline::max());
    });
    signal.notify();
    REQUIRE(waiter.wait_for(2s) == std::future_status::ready);
    REQUIRE(waiter.get());
}

TEST_CASE("Session route extractor retains original frame and records the logical key",
          "[session]") {
    ReceiveRig rig;
    rig.session->set_route_key_extractor([](const can::Message &message) {
        return transport::RouteKey{message.id & 0xff00u, message.is_extended};
    });
    auto inbox = rig.session->subscribe_routes({{0x3d00, true}}, 1);
    REQUIRE(inbox->notification() == rig.session->notification());
    REQUIRE(inbox->health().capacity == 1);
    const auto original = received_frame(0x123d01, true);
    rig.fake->inject(original);
    auto first = require_value(inbox->receive_until(transport::Clock::now() + 2s));
    REQUIRE(first.message.id == original.id);
    REQUIRE(first.message.data == original.data);
    REQUIRE(first.route_key == transport::RouteKey{0x3d00, true});
    REQUIRE(first.received_order != 0);
    REQUIRE(inbox->health().delivered == 1);
    REQUIRE_THROWS_AS(rig.session->set_route_key_extractor({}), can::ValidationError);

    rig.fake->inject(original);
    REQUIRE(await_state(inbox->notification(), [&] { return inbox->health().pending == 1; }));
    rig.fake->inject(original);
    REQUIRE(await_state(inbox->notification(), [&] { return inbox->health().overflow == 1; }));
    REQUIRE(inbox->health().delivered == 2);
    inbox->close();
    REQUIRE(inbox->health().closed);
    REQUIRE(inbox->health().dropped_on_close == 1);
    inbox->close();
    REQUIRE(inbox->health().dropped_on_close == 1);
}

TEST_CASE("Session extractor failures drop a frame without terminating the route", "[session]") {
    ReceiveRig rig;
    rig.session->set_route_key_extractor([](const can::Message &message) {
        if (message.id == 1) {
            throw std::runtime_error("extractor failure");
        }
        if (message.id == 2) {
            throw 1;
        }
        return transport::RouteKey{message.id == 3 ? 0x800u : message.id, false};
    });
    auto inbox = rig.session->subscribe_routes({{4, false}});
    for (uint32_t id = 1; id <= 3; ++id) {
        rig.fake->inject(received_frame(id));
    }
    REQUIRE(await_state(rig.session->notification(),
                        [&] { return rig.session->health().route_key_errors == 3; }));
    REQUIRE(rig.session->health().open);
    rig.fake->inject(received_frame(4));
    REQUIRE(require_value(inbox->receive_until(transport::Clock::now() + 2s)).message.id == 4);
    rig.session->close();
    REQUIRE_THROWS_AS(rig.session->set_route_key_extractor({}), can::ClosedError);
}

TEST_CASE("Session receipt claim and receive events share one order domain", "[session]") {
    ReceiveRig rig;
    auto inbox = rig.session->subscribe_routes({{0x28, false}});
    rig.fake->inject(received_frame(0x28));
    const auto stale = require_value(inbox->receive_until(transport::Clock::now() + 2s));
    rig.fake->on_request(
        0x28, [](const can::Message &request) { return std::vector<can::Message>{request}; });
    const auto receipt = rig.session->submit({{received_frame(0x28)}});
    REQUIRE(receipt.notification() == rig.session->notification());
    receipt.wait();
    const auto fresh = require_value(inbox->receive_until(transport::Clock::now() + 2s));
    REQUIRE(stale.received_order < receipt.result().claimed_order);
    REQUIRE(receipt.result().claimed_order < fresh.received_order);
    transport::SendReceipt empty;
    REQUIRE_THROWS_AS(empty.notification(), can::ValidationError);
}

TEST_CASE("Session notification wakes route consumers on close and recovery", "[session]") {
    can::FakeTransport *backend = nullptr;
    auto session = transport::Session::create([&] {
        auto fresh = std::make_unique<can::FakeTransport>();
        backend = fresh.get();
        return fresh;
    });
    // An empty extractor explicitly selects the original exact-key behavior.
    session->set_route_key_extractor({});
    auto inbox = session->subscribe_routes({{0x28, false}});
    auto signal = inbox->notification();
    backend->inject(received_frame(0x28));
    REQUIRE(await_state(signal, [&] { return inbox->health().pending == 1; }));
    const auto before_recover = signal->revision();
    const auto generation = session->health().generation;
    REQUIRE(session->recover(generation) != generation);
    REQUIRE(signal->wait_until(before_recover, transport::Clock::now()));
    REQUIRE(inbox->health().closed);
    REQUIRE(inbox->health().dropped_on_close == 1);
    REQUIRE_THROWS_AS(inbox->try_receive(), can::ClosedError);
    auto replacement = session->subscribe_routes({{0x28, false}});
    const auto before_close = signal->revision();
    session->close();
    REQUIRE(signal->wait_until(before_close, transport::Clock::now()));
    REQUIRE(replacement->health().closed);
    REQUIRE_THROWS_AS(replacement->try_receive(), can::ClosedError);
}
