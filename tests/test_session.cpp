#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
using namespace std::chrono_literals;
using namespace linkerbot;
namespace {
#if defined(__linux__)
    void pin_to_first_available_cpu() {
        cpu_set_t available;
        CPU_ZERO(&available);
        if (pthread_getaffinity_np(pthread_self(), sizeof(available), &available) != 0) {
            return;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &available)) {
                continue;
            }
            cpu_set_t selected;
            CPU_ZERO(&selected);
            CPU_SET(cpu, &selected);
            (void)pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
            return;
        }
    }
#else
    void pin_to_first_available_cpu() {}
#endif

    struct SessionRig {
        can::FakeTransport *fake;
        std::shared_ptr<transport::Session> session;
        SessionRig() {
            auto ptr = std::make_unique<can::FakeTransport>();
            fake = ptr.get();
            session = transport::Session::own(std::move(ptr));
        }
        ~SessionRig() {
            session->close();
        }
    };

    class ReentrantCloseTransport final : public can::CANTransport {
      public:
        std::function<void()> on_close;

        std::optional<can::Message> read() override {
            std::this_thread::sleep_for(1ms);
            return std::nullopt;
        }
        void write(const can::Message &) override {}
        void close() override {
            if (!closed_.exchange(true) && on_close) {
                on_close();
            }
        }

      private:
        std::atomic<bool> closed_{false};
    };

    void run_with_watchdog(const std::function<void()> &action) {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cv.wait_for(lock, 3s, [&] { return done; })) {
                std::abort();
            }
        });
        std::exception_ptr failure;
        try {
            action();
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_all();
        watchdog.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    can::Message frame(uint32_t id, uint8_t value = 0) {
        can::Message m;
        m.id = id;
        m.dlc = 1;
        m.data[0] = value;
        return m;
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
                if (before_open) {
                    before_open(call);
                }
                if (fail_next_open.exchange(false)) {
                    throw can::TransportIOError("injected reopen failure");
                }
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
} // namespace
TEST_CASE("Session exact routes include IDE and registration is atomic", "[session]") {
    SessionRig rig;
    auto first = rig.session->subscribe_routes({{0x28, false}}, 2);
    REQUIRE_THROWS(rig.session->subscribe_routes({{0x29, false}, {0x28, false}}, 2));
    auto second = rig.session->subscribe_routes({{0x29, false}}, 2);
    auto extended = rig.session->subscribe_routes({{0x28, true}}, 2);
    auto ext = frame(0x28, 22);
    ext.is_extended = true;
    rig.fake->inject(ext);
    auto received = extended->receive_until(transport::Clock::now() + 500ms);
    REQUIRE(received.has_value());
    REQUIRE(received->message.data[0] == 22);
    REQUIRE_FALSE(first->try_receive());
    first->close();
    REQUIRE_NOTHROW(rig.session->subscribe_routes({{0x28, false}}, 2));
}
TEST_CASE("Session bounded inbox rejects overflow and invalid RX", "[session]") {
    SessionRig rig;
    auto route = rig.session->subscribe_routes({{0x28, false}}, 1);
    rig.fake->inject(frame(0x28, 1));
    rig.fake->inject(frame(0x28, 2));
    auto invalid = frame(0x28);
    invalid.dlc = 65;
    rig.fake->inject(invalid);
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
    transport::SendOptions latest;
    latest.endpoint = "hand";
    latest.family = "angle";
    latest.policy = transport::SendPolicy::latest;
    auto claimed = rig.session->submit({{frame(0x28, 1)}, 0us, 0us}, latest);
    REQUIRE(rig.fake->wait_until_tx_entered(500ms));
    auto other_latest = latest;
    other_latest.endpoint = "arm";
    auto unrelated = rig.session->submit({{frame(0x28, 5)}, 0us, 0us}, other_latest);
    auto superseded = rig.session->submit({{frame(0x28, 2)}, 0us, 0us}, latest);
    auto kept = rig.session->submit({{frame(0x28, 3)}, 0us, 0us}, latest);
    REQUIRE(superseded.result().outcome == transport::SendOutcome::superseded);
    transport::SendOptions expired;
    expired.deadline = transport::Clock::now() + 10ms;
    auto timed = rig.session->submit({{frame(0x28, 4)}, 0us, 0us}, expired);
    REQUIRE_THROWS_AS(timed.wait_until(expired.deadline), can::TimeoutError);
    rig.fake->release_tx_gate();
    claimed.wait();
    unrelated.wait();
    kept.wait();
    REQUIRE_THROWS_AS(timed.wait_until(transport::Clock::now() + 500ms), can::TimeoutError);
    const auto sent = rig.fake->sent();
    REQUIRE(sent.size() == 3);
    REQUIRE(sent[0].data[0] == 1);
    REQUIRE(sent[1].data[0] == 5);
    REQUIRE(sent[2].data[0] == 3);
}

TEST_CASE("Session enforces send capacity while priority still orders queued work", "[session]") {
    can::FakeTransport *raw = nullptr;
    auto session = transport::Session::create(
        [&] {
            auto fake = std::make_unique<can::FakeTransport>();
            raw = fake.get();
            return fake;
        },
        2);
    raw->arm_tx_gate();

    auto active = session->submit({{frame(0x28, 1)}});
    REQUIRE(raw->wait_until_tx_entered(500ms));
    transport::SendOptions low;
    low.priority = 0;
    auto low_priority = session->submit({{frame(0x28, 2)}}, low);
    transport::SendOptions high;
    high.priority = 10;
    auto high_priority = session->submit({{frame(0x28, 3)}}, high);
    REQUIRE_THROWS_AS(session->submit({{frame(0x28, 4)}}), can::QueueFullError);

    raw->release_tx_gate();
    active.wait();
    high_priority.wait();
    low_priority.wait();
    const auto sent = raw->sent();
    REQUIRE(sent.size() == 3);
    REQUIRE(sent[0].data[0] == 1);
    REQUIRE(sent[1].data[0] == 3);
    REQUIRE(sent[2].data[0] == 2);
    session->close();
}
TEST_CASE("Session cancellation interrupts pacing with known accepted count", "[session]") {
    SessionRig rig;
    transport::CancellationSource cancel;
    transport::SendOptions options;
    options.cancellation = cancel.token();
    auto route = rig.session->subscribe_routes({{0x28, false}});
    rig.fake->echo_cmd(0x28, 1, 0);
    auto receipt = rig.session->submit({{frame(0x28, 1), frame(0x28, 2)}, 1s, 0us}, options);
    REQUIRE(route->receive_until(transport::Clock::now() + 500ms));
    cancel.cancel();
    REQUIRE_THROWS_AS(receipt.wait_until(transport::Clock::now() + 500ms),
                      transport::CancelledError);
    REQUIRE(receipt.result().sent_frames == 1);
    REQUIRE_FALSE(receipt.result().current_frame_uncertain);
    REQUIRE(rig.fake->sent().size() == 1);
}
TEST_CASE("Session cancellation after full backend acceptance keeps the dispatched result",
          "[session]") {
    SessionRig rig;
    transport::CancellationSource cancel;
    transport::SendOptions options;
    options.cancellation = cancel.token();
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
TEST_CASE("Session close during the claimed write keeps the accepted frames dispatched",
          "[session]") {
    SessionRig rig;
    rig.fake->arm_tx_gate();
    auto receipt = rig.session->submit({{frame(0x28, 1)}, 0us, 0us});
    REQUIRE(rig.fake->wait_until_tx_entered(1s));
    auto closing = std::async(std::launch::async, [&] { rig.session->close(); });
    // close() cancels the claimed job while its frame sits in the backend.
    while (rig.session->health().open) {
        std::this_thread::yield();
    }
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
        try {
            (void)inbox->receive_until(transport::Clock::now() + 10s);
            return false;
        } catch (const can::ClosedError &) {
            return true;
        }
    });
    rig.session->close();
    REQUIRE(waiter.wait_for(500ms) == std::future_status::ready);
    REQUIRE(waiter.get());
    REQUIRE(inbox->health().closed);
}

#include <linkerbot/can/pool.hpp>
TEST_CASE("Session explicitly releases its pool lease and preserves failed close retry",
          "[session][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "fake";
    can::FakeTransport *raw = nullptr;
    auto lease = pool.acquire(key, [&] {
        auto fake = std::make_unique<can::FakeTransport>();
        raw = fake.get();
        return fake;
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

TEST_CASE("Session cleanup rejects registry reentry from transport close",
          "[session][lifecycle][registry][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "reentrant-close";
    ReentrantCloseTransport *raw = nullptr;
    auto lease = pool.acquire(key, [&] {
        auto transport = std::make_unique<ReentrantCloseTransport>();
        raw = transport.get();
        return transport;
    });
    auto bus = lease.shared_bus();
    auto session = transport::Session::lease(std::move(lease));
    std::atomic<int> rejections{0};
    raw->on_close = [&] {
        try {
            (void)transport::Session::share(bus);
        } catch (const can::ValidationError &) {
            ++rejections;
        }
    };

    SECTION("explicit close") {
        run_with_watchdog([&] { session->close(); });
    }
    SECTION("last reference destruction") {
        run_with_watchdog([&] { session.reset(); });
    }

    REQUIRE(rejections == 1);
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("Session adopts pool ownership when a nonowning session already exists",
          "[session][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "fake";
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

TEST_CASE("Session lease registry rejects adding ownership to an explicitly closed session",
          "[session][lifecycle][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "fake";
    auto factory = [] { return std::make_unique<can::FakeTransport>(); };
    auto first = pool.acquire(key, factory);
    auto second = pool.acquire(key, factory);
    auto session = transport::Session::lease(std::move(first));

    session->close();
    REQUIRE_THROWS_AS(transport::Session::lease(std::move(second)), can::ClosedError);
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

TEST_CASE("Final owned Session release from backend callback defers writer join",
          "[session][lifecycle]") {
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
    raw->fail_next_closes(1);
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

TEST_CASE("Session destructor contains a retryable owned transport close failure",
          "[session][lifecycle]") {
    auto fake = std::make_unique<can::FakeTransport>();
    auto *raw = fake.get();
    auto session = transport::Session::own(std::move(fake));
    raw->fail_next_closes(1);
    REQUIRE_NOTHROW(session.reset());
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
            if (released.wait_for(5s) != std::future_status::ready) {
                throw can::TimeoutError("test reopen gate expired");
            }
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
    (void)sent.wait_until(transport::Clock::now() + 2s);
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
    (void)receipt.wait_until(transport::Clock::now() + 2s);
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
        std::atomic<bool> close_rejected{false};
        std::atomic<bool> drain_rejected{false};
        rig.fake->on_request(0x28, [&](const can::Message &) {
            try {
                rig.session->recover(observed);
            } catch (const can::ValidationError &) {
                rejected = true;
            }
            try {
                rig.session->close();
            } catch (const can::ValidationError &) {
                close_rejected = true;
            }
            try {
                transport::Session::drain_deferred_cleanup();
            } catch (const can::ValidationError &) {
                drain_rejected = true;
            }
            return std::vector<can::Message>{};
        });
        auto receipt = rig.session->submit({{frame(0x28)}});
        REQUIRE(receipt.wait_until(transport::Clock::now() + 2s).outcome ==
                transport::SendOutcome::dispatched);
        REQUIRE(rejected);
        REQUIRE(close_rejected);
        REQUIRE(drain_rejected);
        REQUIRE(rig.session->health().open);
    }
}

TEST_CASE("Session validates route changes and send options without losing existing routes",
          "[session][validation]") {
    transport::SendReceipt empty_receipt;
    REQUIRE_THROWS_AS(empty_receipt.result(), can::ValidationError);
    REQUIRE_THROWS_AS(empty_receipt.wait_until(transport::Clock::now()), can::ValidationError);

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
    (void)unrelated.wait_until(transport::Clock::now() + 2s);
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

TEST_CASE("Session shared registry permits independent factory reentry",
          "[session][lifecycle][registry]") {
    auto inner_bus =
        std::make_shared<can::ManagedCANBus>([] { return std::make_unique<can::FakeTransport>(); });
    std::shared_ptr<transport::Session> inner;
    auto outer_bus = std::make_shared<can::ManagedCANBus>([&] {
        inner = transport::Session::share(inner_bus);
        return std::make_unique<can::FakeTransport>();
    });

    auto outer = transport::Session::share(outer_bus);
    REQUIRE(inner);
    REQUIRE(inner != outer);
    REQUIRE(transport::Session::share(inner_bus) == inner);
    REQUIRE(inner->health().open);
    REQUIRE(outer->health().open);

    outer->close();
    inner->close();
}

TEST_CASE("Session shared registry rejects recursive initialization of one bus",
          "[session][lifecycle][registry]") {
    std::shared_ptr<can::ManagedCANBus> bus;
    std::atomic<int> rejections{0};
    bus = std::make_shared<can::ManagedCANBus>([&] {
        try {
            (void)transport::Session::share(bus);
        } catch (const can::ValidationError &) {
            ++rejections;
        }
        return std::make_unique<can::FakeTransport>();
    });

    auto session = transport::Session::share(bus);
    REQUIRE(rejections == 1);
    REQUIRE(session->health().open);
    REQUIRE(transport::Session::share(bus) == session);
    session->close();
}

TEST_CASE("Session shared registry coalesces concurrent first initialization",
          "[session][lifecycle][registry]") {
    std::atomic<int> factory_calls{0};
    std::promise<void> factory_entered;
    auto entered = factory_entered.get_future();
    std::promise<void> release_factory;
    auto release = release_factory.get_future().share();
    auto bus = std::make_shared<can::ManagedCANBus>([&] {
        if (++factory_calls == 1) {
            factory_entered.set_value();
        }
        if (release.wait_for(5s) != std::future_status::ready) {
            throw can::TimeoutError("test factory gate expired");
        }
        return std::make_unique<can::FakeTransport>();
    });

    std::vector<std::future<std::shared_ptr<transport::Session>>> callers;
    for (int i = 0; i < 8; ++i) {
        callers.push_back(
            std::async(std::launch::async, [bus] { return transport::Session::share(bus); }));
    }
    const auto status = entered.wait_for(2s);
    // Always release before assertions so failed checks cannot strand callers.
    release_factory.set_value();
    REQUIRE(status == std::future_status::ready);

    std::shared_ptr<transport::Session> shared;
    for (auto &caller : callers) {
        REQUIRE(caller.wait_for(2s) == std::future_status::ready);
        auto current = caller.get();
        if (!shared) {
            shared = current;
        }
        REQUIRE(current == shared);
    }
    REQUIRE(factory_calls == 1);
    REQUIRE(shared->health().open);
    shared->close();
}

TEST_CASE("Session share and lease concurrently adopt one pooled session",
          "[session][lifecycle][registry][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";
    std::atomic<int> factory_calls{0};
    auto lease = pool.acquire(key, [&] {
        ++factory_calls;
        return std::make_unique<can::FakeTransport>();
    });
    auto bus = lease.shared_bus();

    std::promise<void> start;
    auto started = start.get_future().share();
    auto sharing = std::async(std::launch::async, [bus, started] {
        started.wait();
        return transport::Session::share(bus);
    });
    auto leasing = std::async(std::launch::async, [lease = std::move(lease), started]() mutable {
        started.wait();
        return transport::Session::lease(std::move(lease));
    });
    start.set_value();

    REQUIRE(sharing.wait_for(2s) == std::future_status::ready);
    REQUIRE(leasing.wait_for(2s) == std::future_status::ready);
    auto shared = sharing.get();
    auto adopted = leasing.get();
    REQUIRE(shared == adopted);
    REQUIRE(factory_calls == 1);
    REQUIRE(pool.lease_count(key) == 1);
    REQUIRE(shared->health().open);

    shared->close();
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());
}

TEST_CASE("Session registry waits for deferred leased cleanup before replacement",
          "[session][lifecycle][registry][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";
    std::atomic<int> factory_calls{0};
    can::FakeTransport *first_transport = nullptr;
    auto lease = pool.acquire(key, [&] {
        auto fake = std::make_unique<can::FakeTransport>();
        if (++factory_calls == 1) {
            first_transport = fake.get();
        }
        return fake;
    });
    auto bus = lease.shared_bus();
    auto session = transport::Session::lease(std::move(lease));

    first_transport->arm_tx_gate();
    auto receipt = session->submit({{frame(0x28, 0x31)}});
    const bool writer_entered = first_transport->wait_until_tx_entered(2s);

    std::promise<void> released;
    auto release_observed = released.get_future();
    const auto subscription = bus->subscribe([&](const can::Message &) {
        session.reset();
        released.set_value();
    });
    first_transport->inject(frame(0x29, 0x41));
    const auto release_status = release_observed.wait_for(2s);

    auto replacement =
        std::async(std::launch::async, [bus] { return transport::Session::share(bus); });
    const auto blocked_until_cleanup = replacement.wait_for(50ms);
    // Always release the writer before assertions so a failed check cannot
    // strand deferred cleanup or the replacement future.
    first_transport->release_tx_gate();

    REQUIRE(writer_entered);
    REQUIRE(release_status == std::future_status::ready);
    REQUIRE(blocked_until_cleanup == std::future_status::timeout);
    REQUIRE(replacement.wait_for(2s) == std::future_status::ready);
    auto current = replacement.get();
    transport::Session::drain_deferred_cleanup();

    REQUIRE(factory_calls == 2);
    REQUIRE(current->health().open);
    REQUIRE(bus->get_health().connected());
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(receipt.completed());

    bus->unsubscribe(subscription);
    current->close();
    bus->stop();
}

TEST_CASE("Session registry waits for deferred shared cleanup before replacement",
          "[session][lifecycle][registry]") {
    std::atomic<int> factory_calls{0};
    can::FakeTransport *raw = nullptr;
    auto bus = std::make_shared<can::ManagedCANBus>([&] {
        auto fake = std::make_unique<can::FakeTransport>();
        if (++factory_calls == 1) {
            raw = fake.get();
        }
        return fake;
    });
    auto session = transport::Session::share(bus);

    raw->arm_tx_gate();
    auto receipt = session->submit({{frame(0x28, 0x32)}});
    const bool writer_entered = raw->wait_until_tx_entered(2s);
    std::promise<void> released;
    auto release_observed = released.get_future();
    const auto subscription = bus->subscribe([&](const can::Message &) {
        session.reset();
        released.set_value();
    });
    raw->inject(frame(0x29, 0x42));
    const auto release_status = release_observed.wait_for(2s);

    auto replacement =
        std::async(std::launch::async, [bus] { return transport::Session::share(bus); });
    const auto blocked_until_cleanup = replacement.wait_for(50ms);
    raw->release_tx_gate();

    REQUIRE(writer_entered);
    REQUIRE(release_status == std::future_status::ready);
    REQUIRE(blocked_until_cleanup == std::future_status::timeout);
    REQUIRE(replacement.wait_for(2s) == std::future_status::ready);
    auto current = replacement.get();
    transport::Session::drain_deferred_cleanup();
    REQUIRE(factory_calls == 1);
    REQUIRE(receipt.completed());
    REQUIRE(current->health().open);

    bus->unsubscribe(subscription);
    current->close();
    bus->stop();
}

TEST_CASE("Session registry waiters recheck a replacement active generation",
          "[session][lifecycle][registry]") {
    std::atomic<int> factory_calls{0};
    can::FakeTransport *raw = nullptr;
    auto bus = std::make_shared<can::ManagedCANBus>([&] {
        auto fake = std::make_unique<can::FakeTransport>();
        if (++factory_calls == 1) {
            raw = fake.get();
        }
        return fake;
    });
    auto session = transport::Session::share(bus);

    raw->arm_tx_gate();
    auto receipt = session->submit({{frame(0x28, 0x34)}});
    const bool writer_entered = raw->wait_until_tx_entered(2s);
    if (!writer_entered) {
        raw->release_tx_gate();
        FAIL("Session writer did not reach the cleanup test gate");
    }

    std::promise<void> released;
    auto release_observed = released.get_future();
    const auto subscription = bus->subscribe([&](const can::Message &) {
        session.reset();
        released.set_value();
    });
    raw->inject(frame(0x29, 0x44));
    const auto release_status = release_observed.wait_for(2s);
    if (release_status != std::future_status::ready) {
        raw->release_tx_gate();
        bus->unsubscribe(subscription);
        FAIL("Session callback did not enqueue deferred cleanup");
    }

    // Queue several callers behind the active cleanup tombstone. Once the
    // tombstone is released, one caller can publish the replacement before a
    // later waiter reacquires registry_mutex. Every waiter must consume the
    // old lifetime transition and recheck the new weak Session generation.
    constexpr size_t caller_count = 16;
    std::promise<void> start;
    auto start_gate = start.get_future().share();
    std::mutex callers_started_mutex;
    std::condition_variable callers_started_changed;
    size_t callers_started = 0;
    std::vector<std::thread> caller_threads;
    std::vector<std::future<std::shared_ptr<transport::Session>>> callers;
    try {
        caller_threads.reserve(caller_count);
        callers.reserve(caller_count);
        for (size_t i = 0; i < caller_count; ++i) {
            std::packaged_task<std::shared_ptr<transport::Session>()> task([&, bus, start_gate] {
                pin_to_first_available_cpu();
                start_gate.wait();
                {
                    std::lock_guard<std::mutex> lock(callers_started_mutex);
                    ++callers_started;
                }
                callers_started_changed.notify_one();
                return transport::Session::share(bus);
            });
            auto result = task.get_future();
            caller_threads.emplace_back(std::move(task));
            callers.push_back(std::move(result));
        }
    } catch (...) {
        const auto launch_failure = std::current_exception();
        start.set_value();
        raw->release_tx_gate();
        const auto cleanup_deadline = std::chrono::steady_clock::now() + 5s;
        for (auto &caller : callers) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= cleanup_deadline ||
                caller.wait_for(cleanup_deadline - now) != std::future_status::ready) {
                std::abort();
            }
        }
        for (auto &thread : caller_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        for (auto &caller : callers) {
            try {
                (void)caller.get();
            } catch (...) {
            }
        }
        std::rethrow_exception(launch_failure);
    }
    start.set_value();
    std::unique_lock<std::mutex> start_lock(callers_started_mutex);
    const bool all_started = callers_started_changed.wait_for(
        start_lock, 5s, [&] { return callers_started == caller_count; });
    start_lock.unlock();
    std::this_thread::sleep_for(50ms);
    bool all_blocked = true;
    for (auto &caller : callers) {
        all_blocked = all_blocked && caller.wait_for(0ms) == std::future_status::timeout;
    }

    raw->release_tx_gate();
    const auto completion_deadline = std::chrono::steady_clock::now() + 5s;
    for (auto &caller : callers) {
        const auto now = std::chrono::steady_clock::now();
        // A timed Catch failure would destroy joinable worker threads. Abort
        // at the hard bound so CTest reports the deadlock without relying on
        // stack unwinding.
        if (now >= completion_deadline ||
            caller.wait_for(completion_deadline - now) != std::future_status::ready) {
            std::abort();
        }
    }
    for (auto &thread : caller_threads) {
        thread.join();
    }

    std::exception_ptr failure;
    std::shared_ptr<transport::Session> current;
    std::vector<std::shared_ptr<transport::Session>> results;
    results.reserve(callers.size());
    for (auto &caller : callers) {
        try {
            auto result = caller.get();
            if (!current) {
                current = result;
            }
            results.push_back(std::move(result));
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }

    transport::Session::drain_deferred_cleanup();
    const bool same_session =
        current && std::all_of(results.begin(), results.end(),
                               [&](const auto &result) { return result == current; });
    const bool receipt_completed = receipt.completed();
    const bool current_open = current && current->health().open;
    const int observed_factory_calls = factory_calls;
    bus->unsubscribe(subscription);
    for (auto &result : results) {
        if (result) {
            result->close();
        }
    }
    bus->stop();

    if (failure) {
        std::rethrow_exception(failure);
    }
    REQUIRE(all_started);
    REQUIRE(all_blocked);
    REQUIRE(observed_factory_calls == 1);
    REQUIRE(same_session);
    REQUIRE(receipt_completed);
    REQUIRE(current_open);
}

TEST_CASE("Session registry waits for deferred borrowed cleanup before replacement",
          "[session][lifecycle][registry]") {
    auto fake = std::make_unique<can::FakeTransport>();
    auto *raw = fake.get();
    can::CANDispatcher dispatcher(std::move(fake));
    auto session = transport::Session::borrow(dispatcher);

    raw->arm_tx_gate();
    auto receipt = session->submit({{frame(0x28, 0x33)}});
    const bool writer_entered = raw->wait_until_tx_entered(2s);
    std::promise<void> released;
    auto release_observed = released.get_future();
    const auto subscription = dispatcher.subscribe([&](const can::Message &) {
        session.reset();
        released.set_value();
    });
    raw->inject(frame(0x29, 0x43));
    const auto release_status = release_observed.wait_for(2s);

    auto replacement =
        std::async(std::launch::async, [&] { return transport::Session::borrow(dispatcher); });
    const auto blocked_until_cleanup = replacement.wait_for(50ms);
    raw->release_tx_gate();

    REQUIRE(writer_entered);
    REQUIRE(release_status == std::future_status::ready);
    REQUIRE(blocked_until_cleanup == std::future_status::timeout);
    REQUIRE(replacement.wait_for(2s) == std::future_status::ready);
    auto current = replacement.get();
    transport::Session::drain_deferred_cleanup();
    REQUIRE(receipt.completed());
    REQUIRE(current->health().open);

    dispatcher.unsubscribe(subscription);
    current->close();
    dispatcher.stop();
}

TEST_CASE("Session shared registry rejects cross-backend initialization cycles",
          "[session][lifecycle][registry]") {
    std::shared_ptr<can::ManagedCANBus> bus_a;
    std::shared_ptr<can::ManagedCANBus> bus_b;
    std::promise<void> a_entered;
    std::promise<void> b_entered;
    auto a_ready = a_entered.get_future().share();
    auto b_ready = b_entered.get_future().share();
    std::atomic<int> rejections{0};
    std::mutex watchdog_mutex;
    std::condition_variable watchdog_cv;
    bool watchdog_done = false;
    // If the regression returns, std::future destruction would otherwise wait
    // forever for the two deadlocked async calls. Abort this test process at a
    // hard bound so CI reports a failure instead of hanging indefinitely.
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(watchdog_mutex);
        if (!watchdog_cv.wait_for(lock, 5s, [&] { return watchdog_done; })) {
            std::abort();
        }
    });

    bus_a = std::make_shared<can::ManagedCANBus>([&] {
        a_entered.set_value();
        if (b_ready.wait_for(5s) != std::future_status::ready) {
            throw can::TimeoutError("test bus B factory barrier expired");
        }
        try {
            (void)transport::Session::share(bus_b);
        } catch (const can::ValidationError &) {
            ++rejections;
        }
        return std::make_unique<can::FakeTransport>();
    });
    bus_b = std::make_shared<can::ManagedCANBus>([&] {
        b_entered.set_value();
        if (a_ready.wait_for(5s) != std::future_status::ready) {
            throw can::TimeoutError("test bus A factory barrier expired");
        }
        try {
            (void)transport::Session::share(bus_a);
        } catch (const can::ValidationError &) {
            ++rejections;
        }
        return std::make_unique<can::FakeTransport>();
    });

    auto outer_a = std::async(std::launch::async, [&] { return transport::Session::share(bus_a); });
    auto outer_b = std::async(std::launch::async, [&] { return transport::Session::share(bus_b); });
    const auto status_a = outer_a.wait_for(2s);
    const auto status_b = outer_b.wait_for(2s);
    if (status_a != std::future_status::ready || status_b != std::future_status::ready) {
        FAIL("cyclic Session initialization did not terminate within two seconds");
    }

    std::shared_ptr<transport::Session> session_a;
    std::shared_ptr<transport::Session> session_b;
    std::exception_ptr failure;
    try {
        session_a = outer_a.get();
        session_b = outer_b.get();
    } catch (...) {
        failure = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(watchdog_mutex);
        watchdog_done = true;
    }
    watchdog_cv.notify_all();
    watchdog.join();
    if (failure) {
        std::rethrow_exception(failure);
    }

    REQUIRE(rejections == 1);
    REQUIRE(session_a->health().open);
    REQUIRE(session_b->health().open);
    session_a->close();
    session_b->close();
}

TEST_CASE("Session registry ignores waits for a published initialization attempt",
          "[session][lifecycle][registry]") {
    struct PublisherResult {
        std::shared_ptr<transport::Session> session_a;
        std::shared_ptr<transport::Session> session_b;
        bool rejected = false;
        std::string rejection;
    };

    std::shared_ptr<can::ManagedCANBus> bus_a;
    std::shared_ptr<can::ManagedCANBus> bus_b;
    std::atomic<int> a_factory_calls{0};
    std::atomic<int> b_factory_calls{0};
    std::shared_ptr<transport::Session> nested_b;
    std::promise<void> b_factory_entered;
    auto b_factory_ready = b_factory_entered.get_future();
    std::promise<void> release_b_factory;
    auto b_factory_release = release_b_factory.get_future().share();
    std::promise<void> a_factory_entered;
    auto a_factory_ready = a_factory_entered.get_future();
    std::promise<void> allow_a_to_wait_for_b;
    auto a_wait_allowed = allow_a_to_wait_for_b.get_future().share();

    bus_b = std::make_shared<can::ManagedCANBus>([&] {
        ++b_factory_calls;
        b_factory_entered.set_value();
        if (b_factory_release.wait_for(5s) != std::future_status::ready) {
            throw can::TimeoutError("test B publication gate expired");
        }
        return std::make_unique<can::FakeTransport>();
    });
    bus_a = std::make_shared<can::ManagedCANBus>([&] {
        ++a_factory_calls;
        a_factory_entered.set_value();
        if (a_wait_allowed.wait_for(5s) != std::future_status::ready) {
            throw can::TimeoutError("test A dependency gate expired");
        }
        nested_b = transport::Session::share(bus_b);
        return std::make_unique<can::FakeTransport>();
    });

    std::packaged_task<PublisherResult()> publisher_task([&] {
        pin_to_first_available_cpu();
        PublisherResult result;
        result.session_b = transport::Session::share(bus_b);
        try {
            // B is already published, so A's temporary wait on B is no longer
            // a live dependency even if that waiter has not run again yet.
            result.session_a = transport::Session::share(bus_a);
        } catch (const can::ValidationError &error) {
            result.rejected = true;
            result.rejection = error.what();
        }
        return result;
    });
    auto publisher = publisher_task.get_future();
    std::thread publisher_thread(std::move(publisher_task));
    if (b_factory_ready.wait_for(2s) != std::future_status::ready) {
        std::abort();
    }

    std::packaged_task<std::shared_ptr<transport::Session>()> waiter_task([&] {
        pin_to_first_available_cpu();
        return transport::Session::share(bus_a);
    });
    auto waiter = waiter_task.get_future();
    std::thread waiter_thread;
    try {
        waiter_thread = std::thread(std::move(waiter_task));
    } catch (...) {
        const auto launch_failure = std::current_exception();
        allow_a_to_wait_for_b.set_value();
        release_b_factory.set_value();
        if (publisher.wait_for(5s) != std::future_status::ready) {
            std::abort();
        }
        publisher_thread.join();
        try {
            (void)publisher.get();
        } catch (...) {
        }
        std::rethrow_exception(launch_failure);
    }
    if (a_factory_ready.wait_for(2s) != std::future_status::ready) {
        std::abort();
    }

    allow_a_to_wait_for_b.set_value();
    // The public API does not expose wait-edge installation. Keep both
    // workers on one Linux CPU and allow the A initializer to block on B
    // before publishing B and immediately requesting A on the publisher.
    std::this_thread::sleep_for(20ms);
    release_b_factory.set_value();

    const auto completion_deadline = std::chrono::steady_clock::now() + 5s;
    const auto ready_by_deadline = [&](auto &future) {
        const auto now = std::chrono::steady_clock::now();
        return now < completion_deadline &&
               future.wait_for(completion_deadline - now) == std::future_status::ready;
    };
    if (!ready_by_deadline(publisher) || !ready_by_deadline(waiter)) {
        std::abort();
    }
    publisher_thread.join();
    waiter_thread.join();

    PublisherResult published;
    std::shared_ptr<transport::Session> outer_a;
    std::exception_ptr failure;
    try {
        published = publisher.get();
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        outer_a = waiter.get();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }

    const bool same_a = published.session_a && published.session_a == outer_a;
    const bool same_b = published.session_b && published.session_b == nested_b;
    const bool a_open = outer_a && outer_a->health().open;
    const bool b_open = nested_b && nested_b->health().open;
    if (published.session_a) {
        published.session_a->close();
    }
    if (outer_a) {
        outer_a->close();
    }
    if (published.session_b) {
        published.session_b->close();
    }
    nested_b.reset();
    bus_a->stop();
    bus_b->stop();

    if (failure) {
        std::rethrow_exception(failure);
    }
    INFO("unexpected validation error: " << published.rejection);
    REQUIRE_FALSE(published.rejected);
    REQUIRE(a_factory_calls == 1);
    REQUIRE(b_factory_calls == 1);
    REQUIRE(same_a);
    REQUIRE(same_b);
    REQUIRE(a_open);
    REQUIRE(b_open);
}

TEST_CASE("Session registry rebinds a waiter to a replacement initialization attempt",
          "[session][lifecycle][registry]") {
    std::shared_ptr<can::ManagedCANBus> bus_a;
    std::shared_ptr<can::ManagedCANBus> bus_b;
    std::atomic<int> b_factory_calls{0};
    std::atomic<int> rejections{0};
    std::promise<void> first_b_entered;
    auto first_b_ready = first_b_entered.get_future();
    std::promise<void> release_first_b;
    auto first_b_release = release_first_b.get_future().share();
    std::promise<void> a_factory_entered;
    auto a_factory_ready = a_factory_entered.get_future();

    bus_b = std::make_shared<can::ManagedCANBus>([&] {
        const int call = ++b_factory_calls;
        if (call == 1) {
            first_b_entered.set_value();
            if (first_b_release.wait_for(5s) != std::future_status::ready) {
                throw can::TimeoutError("test first B initialization gate expired");
            }
            throw can::TransportIOError("injected first B initialization failure");
        }
        try {
            (void)transport::Session::share(bus_a);
        } catch (const can::ValidationError &) {
            ++rejections;
        }
        return std::make_unique<can::FakeTransport>();
    });
    bus_a = std::make_shared<can::ManagedCANBus>([&] {
        a_factory_entered.set_value();
        try {
            (void)transport::Session::share(bus_b);
        } catch (const can::ValidationError &) {
            ++rejections;
        }
        return std::make_unique<can::FakeTransport>();
    });

    auto first_b = std::async(std::launch::async, [&] {
        pin_to_first_available_cpu();
        return transport::Session::share(bus_b);
    });
    const auto first_entered = first_b_ready.wait_for(2s);
    if (first_entered != std::future_status::ready) {
        release_first_b.set_value();
        if (first_b.wait_for(5s) != std::future_status::ready) {
            std::abort();
        }
        FAIL("first B initialization did not reach the test gate");
    }

    // This caller waits on the first B attempt before A's initializer does,
    // making it the replacement-attempt contender after the injected failure.
    std::future<std::shared_ptr<transport::Session>> replacement_b;
    std::future<std::shared_ptr<transport::Session>> outer_a;
    try {
        replacement_b = std::async(std::launch::async, [&] {
            pin_to_first_available_cpu();
            return transport::Session::share(bus_b);
        });
        std::this_thread::sleep_for(20ms);
        outer_a = std::async(std::launch::async, [&] {
            pin_to_first_available_cpu();
            return transport::Session::share(bus_a);
        });
    } catch (...) {
        const auto launch_failure = std::current_exception();
        release_first_b.set_value();
        const auto cleanup_deadline = std::chrono::steady_clock::now() + 5s;
        for (auto *future : {&first_b, &replacement_b}) {
            if (!future->valid()) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= cleanup_deadline ||
                future->wait_for(cleanup_deadline - now) != std::future_status::ready) {
                std::abort();
            }
        }
        for (auto *future : {&first_b, &replacement_b}) {
            if (!future->valid()) {
                continue;
            }
            try {
                (void)future->get();
            } catch (...) {
            }
        }
        std::rethrow_exception(launch_failure);
    }
    const auto a_entered = a_factory_ready.wait_for(2s);
    std::this_thread::sleep_for(20ms);
    release_first_b.set_value();

    const auto completion_deadline = std::chrono::steady_clock::now() + 5s;
    const auto ready_by_deadline = [&](auto &future) {
        const auto now = std::chrono::steady_clock::now();
        return now < completion_deadline &&
               future.wait_for(completion_deadline - now) == std::future_status::ready;
    };
    if (!ready_by_deadline(first_b) || !ready_by_deadline(outer_a) ||
        !ready_by_deadline(replacement_b)) {
        std::abort();
    }

    bool first_failed = false;
    std::exception_ptr failure;
    try {
        (void)first_b.get();
    } catch (const can::TransportIOError &) {
        first_failed = true;
    } catch (...) {
        failure = std::current_exception();
    }
    std::shared_ptr<transport::Session> session_a;
    std::shared_ptr<transport::Session> session_b;
    try {
        session_a = outer_a.get();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    try {
        session_b = replacement_b.get();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }

    const bool session_a_open = session_a && session_a->health().open;
    const bool session_b_open = session_b && session_b->health().open;
    if (session_a) {
        session_a->close();
    }
    if (session_b) {
        session_b->close();
    }

    if (failure) {
        std::rethrow_exception(failure);
    }

    REQUIRE(a_entered == std::future_status::ready);
    REQUIRE(first_failed);
    REQUIRE(b_factory_calls == 2);
    REQUIRE(rejections == 1);
    REQUIRE(session_a_open);
    REQUIRE(session_b_open);
}

TEST_CASE("Session shared registry rolls back failed initialization for retry",
          "[session][lifecycle][registry]") {
    std::atomic<int> factory_calls{0};
    auto bus = std::make_shared<can::ManagedCANBus>([&] {
        if (++factory_calls == 1) {
            throw can::TransportIOError("injected first initialization failure");
        }
        return std::make_unique<can::FakeTransport>();
    });

    REQUIRE_THROWS_AS(transport::Session::share(bus), can::TransportIOError);
    auto session = transport::Session::share(bus);
    REQUIRE(factory_calls == 2);
    REQUIRE(session->health().open);
    REQUIRE(transport::Session::share(bus) == session);
    session->close();
}

TEST_CASE("Session leased registry rolls back a closed backend for retry",
          "[session][lifecycle][registry][pool]") {
    can::SharedCANBusPool pool;
    can::CANBusKey key;
    key.backend = "fake";
    key.endpoint = "can0";
    std::atomic<int> factory_calls{0};
    auto lease = pool.acquire(key, [&] {
        ++factory_calls;
        return std::make_unique<can::FakeTransport>();
    });
    auto bus = lease.shared_bus();
    bus->stop();

    REQUIRE_THROWS_AS(transport::Session::lease(std::move(lease)), can::ClosedError);
    REQUIRE(pool.lease_count(key) == 0);
    REQUIRE(pool.active_keys().empty());

    // The failed lease initialization must abandon its registry reservation;
    // the same ManagedCANBus can then be started and shared normally.
    auto session = transport::Session::share(bus);
    REQUIRE(factory_calls == 2);
    REQUIRE(session->health().open);
    session->close();
    bus->stop();
}

TEST_CASE("Session borrowed registry abandons each closed dispatcher initialization",
          "[session][lifecycle][registry]") {
    can::CANDispatcher dispatcher(std::make_unique<can::FakeTransport>());
    dispatcher.stop();

    REQUIRE_THROWS_AS(transport::Session::borrow(dispatcher), can::ClosedError);
    // A second attempt proves that the first exception cleared `initializing`
    // instead of leaving this dispatcher key permanently reserved.
    REQUIRE_THROWS_AS(transport::Session::borrow(dispatcher), can::ClosedError);
}
