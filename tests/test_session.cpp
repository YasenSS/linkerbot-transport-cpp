#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <future>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <mutex>
#include <thread>
#include <vector>
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

struct RecoverableSessionRig {
    std::atomic<int> factory_calls{0};
    std::atomic<bool> fail_next_open{false};
    std::function<void(int)> before_open;
    std::mutex sent_mutex;
    std::vector<uint8_t> accepted;
    std::shared_ptr<transport::Session> session;

    RecoverableSessionRig() {
        session = transport::Session::create([this] {
            const int call = ++factory_calls;
            if (before_open)
                before_open(call);
            if (fail_next_open.exchange(false))
                throw can::TransportIOError("injected reopen failure");
            auto fake = std::make_unique<can::FakeTransport>();
            fake->on_request(0x28, [this](const can::Message &message) {
                std::lock_guard<std::mutex> lock(sent_mutex);
                accepted.push_back(message.data[0]);
                return std::vector<can::Message>{message};
            });
            return fake;
        });
    }
    ~RecoverableSessionRig() {
        session->close();
    }
    std::vector<uint8_t> sent() {
        std::lock_guard<std::mutex> lock(sent_mutex);
        return accepted;
    }
};
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
TEST_CASE("Session recover invalidates old routes and cancels batches without replay",
          "[session][recovery]") {
    RecoverableSessionRig rig;
    const auto old_generation = rig.session->health().generation;
    auto old_route = rig.session->subscribe_routes({{0x28, false}});
    auto waiting_route = rig.session->subscribe_routes({{0x29, false}});
    auto receiver = std::async(std::launch::async, [&] {
        try {
            (void)waiting_route->receive_until(transport::Clock::now() + 5s);
            return false;
        } catch (const can::ClosedError &) {
            return true;
        }
    });

    // The echoed first frame proves this batch owns the writer. Its long gap
    // leaves both an unfinished frame and a queued batch for recover to cancel.
    auto active = rig.session->submit({{frame(0x28, 1), frame(0x28, 2)}, 10s});
    REQUIRE(old_route->receive_until(transport::Clock::now() + 2s));
    auto queued = rig.session->submit({{frame(0x28, 3)}});
    const auto generation = rig.session->recover(old_generation);

    REQUIRE(generation == old_generation + 1);
    REQUIRE(rig.factory_calls == 2);
    REQUIRE(rig.session->health().open);
    REQUIRE(rig.session->health().recoveries == 1);
    REQUIRE(old_route->health().closed);
    REQUIRE_THROWS_AS(old_route->try_receive(), can::ClosedError);
    REQUIRE(receiver.wait_for(2s) == std::future_status::ready);
    REQUIRE(receiver.get());
    REQUIRE_THROWS_AS(active.wait_until(transport::Clock::now() + 2s), transport::CancelledError);
    REQUIRE(active.result().outcome == transport::SendOutcome::cancelled);
    REQUIRE(active.result().generation == old_generation);
    REQUIRE(active.result().sent_frames == 1);
    REQUIRE_FALSE(active.result().current_frame_uncertain);
    REQUIRE_THROWS_AS(queued.wait_until(transport::Clock::now() + 2s), can::ClosedError);
    REQUIRE(queued.result().outcome == transport::SendOutcome::cancelled);
    REQUIRE(queued.result().sent_frames == 0);

    auto route = rig.session->subscribe_routes({{0x28, false}});
    auto fresh = rig.session->submit({{frame(0x28, 4)}});
    REQUIRE(fresh.wait_until(transport::Clock::now() + 2s).generation == generation);
    const auto reply = route->receive_until(transport::Clock::now() + 2s);
    REQUIRE(reply);
    REQUIRE(reply->generation == generation);
    REQUIRE(reply->message.data[0] == 4);
    REQUIRE(rig.sent() == std::vector<uint8_t>{1, 4});
}

TEST_CASE("Session recover coalesces concurrent callers observing the same generation",
          "[session][recovery]") {
    RecoverableSessionRig rig;
    const auto observed = rig.session->health().generation;
    std::promise<void> reopening;
    auto entered = reopening.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    rig.before_open = [&](int call) {
        if (call == 2) {
            reopening.set_value();
            if (released.wait_for(5s) != std::future_status::ready)
                throw can::TimeoutError("test reopen gate expired");
        }
    };
    std::promise<void> start;
    auto started = start.get_future().share();
    std::vector<std::future<uint64_t>> callers;
    for (int i = 0; i < 8; ++i) {
        callers.push_back(std::async(std::launch::async, [&, started] {
            started.wait();
            return rig.session->recover(observed);
        }));
    }
    start.set_value();
    const auto status = entered.wait_for(2s);
    // Release before assertions so a failed assertion cannot strand workers.
    release.set_value();
    REQUIRE(status == std::future_status::ready);
    for (auto &caller : callers) {
        REQUIRE(caller.wait_for(2s) == std::future_status::ready);
        REQUIRE(caller.get() == observed + 1);
    }
    REQUIRE(rig.factory_calls == 2);
    REQUIRE(rig.session->health().recoveries == 1);
    REQUIRE(rig.session->recover(observed) == observed + 1);
    REQUIRE(rig.factory_calls == 2);
    // Also drain a real send through the rebuilt connection.
    auto route = rig.session->subscribe_routes({{0x28, false}});
    auto sent = rig.session->submit({{frame(0x28, 5)}});
    sent.wait_until(transport::Clock::now() + 2s);
    REQUIRE(route->receive_until(transport::Clock::now() + 2s));
    rig.session->close();
}

TEST_CASE("Session recover retries a failed factory without reviving old routes",
          "[session][recovery]") {
    RecoverableSessionRig rig;
    const auto observed = rig.session->health().generation;
    auto old_route = rig.session->subscribe_routes({{0x28, false}});
    rig.fail_next_open = true;
    REQUIRE_THROWS_AS(rig.session->recover(observed), can::TransportIOError);
    REQUIRE(rig.factory_calls == 2);
    REQUIRE_FALSE(rig.session->health().open);
    REQUIRE(rig.session->health().recoveries == 0);
    REQUIRE(old_route->health().closed);
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}}), can::ClosedError);
    REQUIRE_THROWS_AS(rig.session->subscribe_routes({{0x28, false}}), can::ClosedError);

    const auto generation = rig.session->recover(observed);
    REQUIRE(generation == observed + 1);
    REQUIRE(rig.factory_calls == 3);
    REQUIRE(rig.session->health().open);
    REQUIRE(rig.session->health().recoveries == 1);
    REQUIRE_THROWS_AS(old_route->try_receive(), can::ClosedError);
    auto route = rig.session->subscribe_routes({{0x28, false}});
    auto receipt = rig.session->submit({{frame(0x28, 6)}});
    receipt.wait_until(transport::Clock::now() + 2s);
    const auto reply = route->receive_until(transport::Clock::now() + 2s);
    REQUIRE(reply);
    REQUIRE(reply->generation == generation);
    REQUIRE(reply->message.data[0] == 6);
    REQUIRE(rig.sent() == std::vector<uint8_t>{6});
}

TEST_CASE("Session recover rejects closed and non-recreatable sessions", "[session][recovery]") {
    SECTION("explicitly closed factory session cannot reopen") {
        RecoverableSessionRig rig;
        const auto observed = rig.session->health().generation;
        rig.session->close();
        REQUIRE_THROWS_AS(rig.session->recover(observed), can::ClosedError);
        REQUIRE(rig.factory_calls == 1);
    }
    SECTION("owned one-shot transport remains usable after rejected recovery") {
        SessionRig rig;
        REQUIRE_THROWS_AS(rig.session->recover(rig.session->health().generation),
                          can::ValidationError);
        REQUIRE(rig.session->health().open);
        auto receipt = rig.session->submit({{frame(0x28)}});
        REQUIRE(receipt.wait_until(transport::Clock::now() + 2s).outcome ==
                transport::SendOutcome::dispatched);
    }
    SECTION("borrowed dispatcher remains usable after rejected recovery") {
        can::CANDispatcher dispatcher(std::make_unique<can::FakeTransport>());
        auto session = transport::Session::borrow(dispatcher);
        REQUIRE_THROWS_AS(session->recover(session->health().generation), can::ValidationError);
        REQUIRE(session->health().open);
        session->close();
        REQUIRE_NOTHROW(dispatcher.send(frame(0x28)));
    }
}

TEST_CASE("Session recover rejects lifecycle calls from receive and writer callbacks",
          "[session][recovery]") {
    SECTION("receive callback cannot recover a borrowed dispatcher") {
        auto fake = std::make_unique<can::FakeTransport>();
        auto *raw = fake.get();
        can::CANDispatcher dispatcher(std::move(fake));
        auto session = transport::Session::borrow(dispatcher);
        const auto observed = session->health().generation;
        std::promise<bool> rejected;
        auto result = rejected.get_future();
        const auto subscription = dispatcher.subscribe([&](const can::Message &) {
            try {
                session->recover(observed);
                rejected.set_value(false);
            } catch (const can::ValidationError &) {
                rejected.set_value(true);
            }
        });
        raw->inject(frame(0x28));
        const auto status = result.wait_for(2s);
        dispatcher.unsubscribe(subscription);
        REQUIRE(status == std::future_status::ready);
        REQUIRE(result.get());
        REQUIRE(session->health().open);
        session->close();
        REQUIRE_NOTHROW(dispatcher.send(frame(0x28)));
    }
    SECTION("writer callback cannot recover its own session") {
        SessionRig rig;
        const auto observed = rig.session->health().generation;
        std::atomic<bool> rejected{false};
        rig.fake->on_request(0x28, [&](const can::Message &) {
            try {
                rig.session->recover(observed);
            } catch (const can::ValidationError &) {
                rejected = true;
            }
            return std::vector<can::Message>{};
        });
        auto receipt = rig.session->submit({{frame(0x28)}});
        REQUIRE(receipt.wait_until(transport::Clock::now() + 2s).outcome ==
                transport::SendOutcome::dispatched);
        REQUIRE(rejected);
        REQUIRE(rig.session->health().open);
    }
}

TEST_CASE("Session validates route changes and send options without losing existing routes",
          "[session][validation]") {
    SessionRig rig;
    SessionRig other;
    auto route = rig.session->subscribe_routes({{0x28, false}});
    REQUIRE_THROWS_AS(rig.session->update_routes(nullptr, {{0x29, false}}), can::ValidationError);
    REQUIRE_THROWS_AS(other.session->update_routes(route, {{0x29, false}}), can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->update_routes(route, {}), can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->subscribe_routes({{0x29, false}, {0x29, false}}),
                      can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->subscribe_routes({{0x800, false}}), can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->subscribe_routes({{0x29, false}}, 0), can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->submit({{}}), can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}, -1us}), can::ValidationError);
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}, 0us, -1us}), can::ValidationError);
    transport::SendOptions latest;
    latest.policy = transport::SendPolicy::latest;
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}}, latest), can::ValidationError);
    latest.endpoint = "hand";
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}}, latest), can::ValidationError);
    transport::SendOptions expired;
    expired.deadline = transport::Clock::now();
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}}, expired), can::TimeoutError);
    rig.session->update_routes(route, {{0x29, false}});
    auto reused = rig.session->subscribe_routes({{0x28, false}});
    rig.fake->inject(frame(0x29, 7));
    REQUIRE(route->receive_until(transport::Clock::now() + 2s)->message.data[0] == 7);
    route->close();
    REQUIRE_THROWS_AS(rig.session->update_routes(route, {{0x29, false}}), can::ClosedError);
}

TEST_CASE("Session endpoint cancellation preserves unrelated work and cancels queued tokens",
          "[session][cancellation]") {
    SessionRig rig;
    rig.fake->arm_tx_gate();
    transport::SendOptions a;
    a.endpoint = "a";
    transport::SendOptions b;
    b.endpoint = "b";
    transport::CancellationSource token;
    transport::SendOptions cancelled;
    cancelled.cancellation = token.token();
    auto active = rig.session->submit({{frame(0x28, 1)}}, a);
    const bool entered = rig.fake->wait_until_tx_entered(2s);
    auto same = rig.session->submit({{frame(0x28, 2)}}, a);
    auto unrelated = rig.session->submit({{frame(0x28, 3)}}, b);
    auto queued_token = rig.session->submit({{frame(0x28, 4)}}, cancelled);
    rig.session->cancel_endpoint("a");
    rig.session->cancel_endpoint("absent");
    token.cancel();
    rig.fake->release_tx_gate();
    REQUIRE(entered);
    REQUIRE(active.wait_until(transport::Clock::now() + 2s).outcome ==
            transport::SendOutcome::dispatched);
    REQUIRE_THROWS_AS(same.wait_until(transport::Clock::now() + 2s), transport::CancelledError);
    unrelated.wait_until(transport::Clock::now() + 2s);
    REQUIRE_THROWS_AS(queued_token.wait_until(transport::Clock::now() + 2s),
                      transport::CancelledError);
    REQUIRE(rig.fake->sent().size() == 2);
    REQUIRE(rig.fake->sent()[1].data[0] == 3);
    REQUIRE_THROWS_AS(rig.session->submit({{frame(0x28)}}, cancelled), transport::CancelledError);
}

TEST_CASE("Session backend failures and pacing deadlines retain physical acceptance details",
          "[session][faults]") {
    SECTION("backend failure is uncertain until a fresh successful submission") {
        SessionRig rig;
        rig.fake->fail_writes("injected failure");
        auto failed = rig.session->submit({{frame(0x28)}});
        REQUIRE_THROWS_AS(failed.wait_until(transport::Clock::now() + 2s), can::TransportIOError);
        REQUIRE(failed.result().outcome == transport::SendOutcome::failed);
        REQUIRE(failed.result().current_frame_uncertain);
        REQUIRE(failed.result().sent_frames == 0);
        rig.session->close();
    }
    SECTION("deadline during initial pacing expires before backend acceptance") {
        SessionRig rig;
        transport::SendOptions options;
        options.deadline = transport::Clock::now() + 50ms;
        auto expired = rig.session->submit({{frame(0x28)}, 0us, 10s}, options);
        REQUIRE_THROWS_AS(expired.wait_until(transport::Clock::now() + 2s), can::TimeoutError);
        REQUIRE(expired.result().outcome == transport::SendOutcome::expired);
        REQUIRE_FALSE(expired.result().current_frame_uncertain);
        REQUIRE(rig.fake->sent().empty());
    }
}

TEST_CASE("Session shared and borrowed registries reject reuse after explicit close",
          "[session][lifecycle]") {
    REQUIRE_THROWS_AS(transport::Session::share(nullptr), can::ValidationError);
    REQUIRE_THROWS_AS(
        transport::Session::create([] { return std::make_unique<can::FakeTransport>(); }, 0),
        can::ValidationError);
    auto bus =
        std::make_shared<can::ManagedCANBus>([] { return std::make_unique<can::FakeTransport>(); });
    auto shared = transport::Session::share(bus);
    REQUIRE(transport::Session::share(bus) == shared);
    REQUIRE(shared->transport_health().connected());
    shared->close();
    REQUIRE_FALSE(shared->transport_health().connected());
    REQUIRE(bus->get_health().connected());
    REQUIRE_THROWS_AS(transport::Session::share(bus), can::ClosedError);
    can::CANDispatcher dispatcher(std::make_unique<can::FakeTransport>());
    auto borrowed = transport::Session::borrow(dispatcher);
    REQUIRE(transport::Session::borrow(dispatcher) == borrowed);
    REQUIRE(borrowed->transport_health().connected());
    borrowed->close();
    REQUIRE_THROWS_AS(transport::Session::borrow(dispatcher), can::ClosedError);
}
