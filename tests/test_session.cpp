#include <catch2/catch_test_macros.hpp>
#include <linkerbot/transport/session.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <future>
#include <thread>
using namespace std::chrono_literals;
using namespace linkerbot;
namespace {
struct SessionRig {
    can::FakeTransport *fake;
    std::shared_ptr<transport::Session> session;
    SessionRig() {
        auto ptr = std::make_unique<can::FakeTransport>(); fake = ptr.get();
        session = transport::Session::own(std::move(ptr));
    }
    ~SessionRig() { session->close(); }
};
can::Message frame(uint32_t id, uint8_t value = 0) {
    can::Message m; m.id = id; m.dlc = 1; m.data[0] = value; return m;
}
}
TEST_CASE("Session exact routes include IDE and registration is atomic", "[session]") {
    SessionRig rig;
    auto first = rig.session->subscribe_routes({{0x28, false}}, 2);
    REQUIRE_THROWS(rig.session->subscribe_routes({{0x29, false}, {0x28, false}}, 2));
    auto second = rig.session->subscribe_routes({{0x29, false}}, 2);
    auto extended = rig.session->subscribe_routes({{0x28, true}}, 2);
    auto ext = frame(0x28, 22); ext.is_extended = true;
    rig.fake->inject(ext);
    auto received = extended->receive_until(transport::Clock::now() + 500ms);
    REQUIRE(received.has_value()); REQUIRE(received->message.data[0] == 22);
    REQUIRE_FALSE(first->try_receive());
    first->close();
    REQUIRE_NOTHROW(rig.session->subscribe_routes({{0x28, false}}, 2));
}
TEST_CASE("Session bounded inbox rejects overflow and invalid RX", "[session]") {
    SessionRig rig;
    auto route = rig.session->subscribe_routes({{0x28, false}}, 1);
    rig.fake->inject(frame(0x28, 1));
    rig.fake->inject(frame(0x28, 2));
    auto invalid = frame(0x28); invalid.dlc = 65; rig.fake->inject(invalid);
    auto barrier = rig.session->subscribe_routes({{0x29, false}}, 1);
    rig.fake->inject(frame(0x29));
    REQUIRE(barrier->receive_until(transport::Clock::now() + 500ms));
    REQUIRE(route->health().overflow == 1);
    REQUIRE(rig.session->health().invalid_frames == 1);
    REQUIRE(route->try_receive()->message.data[0] == 1);
}
TEST_CASE("Session queued deadline and latest never replace a claimed batch", "[session]") {
    SessionRig rig;
    rig.fake->arm_tx_gate();
    transport::SendOptions latest; latest.endpoint = "hand"; latest.family = "angle";
    latest.policy = transport::SendPolicy::latest;
    auto claimed = rig.session->submit({{frame(0x28, 1)}, 0us, 0us}, latest);
    REQUIRE(rig.fake->wait_until_tx_entered(500ms));
    auto superseded = rig.session->submit({{frame(0x28, 2)}, 0us, 0us}, latest);
    auto kept = rig.session->submit({{frame(0x28, 3)}, 0us, 0us}, latest);
    REQUIRE(superseded.result().outcome == transport::SendOutcome::superseded);
    transport::SendOptions expired; expired.deadline = transport::Clock::now() + 10ms;
    auto timed = rig.session->submit({{frame(0x28, 4)}, 0us, 0us}, expired);
    REQUIRE_THROWS_AS(timed.wait_until(expired.deadline), can::TimeoutError);
    rig.fake->release_tx_gate();
    claimed.wait(); kept.wait();
    REQUIRE_THROWS_AS(timed.wait_until(transport::Clock::now() + 500ms), can::TimeoutError);
    const auto sent = rig.fake->sent();
    REQUIRE(sent.size() == 2); REQUIRE(sent[0].data[0] == 1); REQUIRE(sent[1].data[0] == 3);
}
TEST_CASE("Session cancellation interrupts pacing with known accepted count", "[session]") {
    SessionRig rig;
    transport::CancellationSource cancel;
    transport::SendOptions options; options.cancellation = cancel.token();
    auto route = rig.session->subscribe_routes({{0x28, false}});
    rig.fake->echo_cmd(0x28, 1, 0);
    auto receipt = rig.session->submit({{frame(0x28, 1), frame(0x28, 2)}, 1s, 0us}, options);
    REQUIRE(route->receive_until(transport::Clock::now() + 500ms));
    cancel.cancel();
    REQUIRE_THROWS_AS(receipt.wait_until(transport::Clock::now() + 500ms), transport::CancelledError);
    REQUIRE(receipt.result().sent_frames == 1);
    REQUIRE_FALSE(receipt.result().current_frame_uncertain);
    REQUIRE(rig.fake->sent().size() == 1);
}
TEST_CASE("Session cancellation after full backend acceptance keeps the dispatched result", "[session]") {
    SessionRig rig;
    transport::CancellationSource cancel;
    transport::SendOptions options; options.cancellation = cancel.token();
    rig.fake->arm_tx_gate();
    auto receipt = rig.session->submit({{frame(0x28, 1)}, 0us, 0us}, options);
    REQUIRE(rig.fake->wait_until_tx_entered(1s));
    // The only frame is already inside the backend write. Cancelling here
    // cannot unsend it, so the batch has no unresolved frame left to report.
    cancel.cancel();
    rig.fake->release_tx_gate();
    const auto result = receipt.wait_until(transport::Clock::now() + 2s);
    REQUIRE(result.outcome == transport::SendOutcome::dispatched);
    REQUIRE(result.sent_frames == 1);
    REQUIRE_FALSE(result.current_frame_uncertain);
    REQUIRE(rig.fake->sent().size() == 1);
}
TEST_CASE("Session close during the claimed write keeps the accepted frames dispatched", "[session]") {
    SessionRig rig;
    rig.fake->arm_tx_gate();
    auto receipt = rig.session->submit({{frame(0x28, 1)}, 0us, 0us});
    REQUIRE(rig.fake->wait_until_tx_entered(1s));
    auto closing = std::async(std::launch::async, [&] { rig.session->close(); });
    // close() cancels the claimed job while its frame sits in the backend.
    while (rig.session->health().open) std::this_thread::yield();
    rig.fake->release_tx_gate();
    REQUIRE(closing.wait_for(2s) == std::future_status::ready);
    closing.get();
    const auto result = receipt.result();
    REQUIRE(result.outcome == transport::SendOutcome::dispatched);
    REQUIRE(result.sent_frames == 1);
    REQUIRE_FALSE(result.current_frame_uncertain);
    REQUIRE(rig.fake->sent().size() == 1);
}
TEST_CASE("Session close invalidates routes and wakes receiver", "[session]") {
    SessionRig rig;
    auto inbox = rig.session->subscribe_routes({{0x28, false}});
    auto waiter = std::async(std::launch::async, [&] {
        try { (void)inbox->receive_until(transport::Clock::now() + 10s); return false; }
        catch (const can::ClosedError &) { return true; }
    });
    rig.session->close();
    REQUIRE(waiter.wait_for(500ms) == std::future_status::ready);
    REQUIRE(waiter.get());
    REQUIRE(inbox->health().closed);
}

#include <linkerbot/can/pool.hpp>
TEST_CASE("Session explicitly releases its pool lease and preserves failed close retry", "[session][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key; key.backend = "fake";
    can::FakeTransport *raw = nullptr;
    auto lease = pool.acquire(key, [&] {
        auto fake = std::make_unique<can::FakeTransport>(); raw = fake.get(); return fake;
    });
    auto session = transport::Session::lease(std::move(lease));
    raw->fail_next_closes(1);
    REQUIRE_THROWS_AS(session->close(), can::TransportIOError);
    REQUIRE(pool.lease_count(key) == 1);
    REQUIRE_NOTHROW(session->close());
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());
    REQUIRE_FALSE(session->health().open);
}

TEST_CASE("Session adopts pool ownership when a nonowning session already exists", "[session][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key; key.backend = "fake";
    auto factory = [] { return std::make_unique<can::FakeTransport>(); };
    auto lease = pool.acquire(key, factory);
    auto session = transport::Session::share(lease.shared_bus());
    auto adopted = transport::Session::lease(std::move(lease));
    REQUIRE(adopted == session);
    REQUIRE(pool.lease_count(key) == 1);
    auto second = transport::Session::lease(pool.acquire(key, factory));
    REQUIRE(second == session);
    REQUIRE(pool.lease_count(key) == 2);
    REQUIRE(session->health().open);
    session->send({{frame(0x28)}, 0us, 0us});
    session->close();
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("Final borrowed Session release from RX defers join safely", "[session][lifecycle]") {
    auto fake = std::make_unique<can::FakeTransport>();
    auto *raw = fake.get();
    can::CANDispatcher dispatcher(std::move(fake));
    auto session = transport::Session::borrow(dispatcher);
    std::promise<void> released;
    auto done = released.get_future();
    auto id = dispatcher.subscribe([&](const can::Message &) {
        session.reset();
        released.set_value();
    });
    raw->inject(frame(0x28));
    const auto status = done.wait_for(1s);
    dispatcher.unsubscribe(id);
    transport::Session::drain_deferred_cleanup();
    REQUIRE(status == std::future_status::ready);
    REQUIRE(dispatcher.get_health().subscriber_count == 0);
    REQUIRE_NOTHROW(dispatcher.send(frame(0x29)));
}

TEST_CASE("Final owned Session release from backend callback defers writer join", "[session][lifecycle]") {
    auto fake = std::make_unique<can::FakeTransport>();
    auto *raw = fake.get();
    std::shared_ptr<transport::Session> session;
    std::promise<void> released;
    auto done = released.get_future();
    raw->on_request(0x28, [&](const can::Message &) {
        session.reset();
        released.set_value();
        return std::vector<can::Message>{};
    });
    session = transport::Session::own(std::move(fake));
    raw->arm_tx_gate();
    auto receipt = session->submit({{frame(0x28)}});
    const auto entered = raw->wait_until_tx_entered(1s);
    raw->release_tx_gate();
    const auto status = done.wait_for(1s);
    transport::Session::drain_deferred_cleanup();
    REQUIRE(entered);
    REQUIRE(status == std::future_status::ready);
    REQUIRE(receipt.completed());
}
