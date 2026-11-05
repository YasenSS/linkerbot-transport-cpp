#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using namespace std::chrono_literals;
using namespace linkerbot;

TEST_CASE("Session accepts an initial extractor after unsolicited unmatched startup traffic",
          "[session]") {
    auto backend = std::make_unique<can::FakeTransport>();
    auto *fake = backend.get();
    auto session = transport::Session::own(std::move(backend));
    can::Message frame;
    frame.id = 0x123d01;
    frame.is_extended = true;
    fake->inject(frame);
    const auto deadline = transport::Clock::now() + 2s;
    while (session->health().rx_frames == 0 && transport::Clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(session->health().rx_frames == 1);
    REQUIRE_NOTHROW(session->set_route_key_extractor([](const can::Message &message) {
        return transport::RouteKey{message.id & 0xff00u, message.is_extended};
    }));
    auto inbox = session->subscribe_routes({{0x3d00, true}});
    fake->inject(frame);
    auto received = inbox->receive_until(transport::Clock::now() + 2s);
    REQUIRE(received.has_value());
    REQUIRE(received->message.id == frame.id);
    REQUIRE(received->route_key == transport::RouteKey{0x3d00, true});
    session->close();
}

TEST_CASE("Session recovery cannot reopen route extractor configuration", "[session]") {
    auto session =
        transport::Session::create([] { return std::make_unique<can::FakeTransport>(); });
    session->set_route_key_extractor({});
    auto old_route = session->subscribe_routes({{0x28, false}});
    const auto generation = session->health().generation;
    REQUIRE(session->recover(generation) != generation);
    REQUIRE(old_route->health().closed);
    REQUIRE_THROWS_AS(session->set_route_key_extractor({}), can::ValidationError);
    auto replacement = session->subscribe_routes({{0x28, false}});
    REQUIRE_FALSE(replacement->health().closed);
    session->close();
}

TEST_CASE("Session large positive pending-age budget does not overflow the clock duration",
          "[session]") {
    auto backend = std::make_unique<can::FakeTransport>();
    auto *fake = backend.get();
    auto session = transport::Session::own(std::move(backend));
    fake->arm_tx_gate();
    auto active = session->submit({{{}}});
    const bool entered = fake->wait_until_tx_entered(2s);
    transport::SendOptions options;
    options.max_pending_age = std::chrono::microseconds::max();
    std::optional<transport::SendReceipt> admitted;
    std::exception_ptr error;
    try {
        admitted = session->submit({{{}}}, options);
    } catch (...) {
        error = std::current_exception();
    }
    fake->release_tx_gate();
    active.wait();
    if (admitted) {
        admitted->wait();
    }
    session->close();
    REQUIRE(entered);
    REQUIRE(error == nullptr);
    REQUIRE(admitted.has_value());
}

TEST_CASE("Session legacy aggregate initializers remain warning-free for strict consumers",
          "[session]") {
    const transport::SendOptions options{transport::Deadline::max(),    {}, "hand", "angle",
                                         transport::SendPolicy::latest, 0};
    const transport::RoutedMessage received{{}, 1, 2, {}};
    const transport::SendResult result{
        transport::SendOutcome::pending, 1, {}, {}, {}, 1, 0, false, {}};
    const transport::RouteHealth route{false, 1, 0, 0, 0};
    const transport::SessionHealth health{true, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""};
    REQUIRE_FALSE(options.expected_generation.has_value());
    REQUIRE_FALSE(options.max_pending_age.has_value());
    REQUIRE(received.route_key == transport::RouteKey{});
    REQUIRE_FALSE(result.claimed_order.has_value());
    REQUIRE(route.capacity == 0);
    REQUIRE_FALSE(health.oldest_pending_age.has_value());
}

namespace {
    class SecondWriteGate : public can::FakeTransport {
      public:
        explicit SecondWriteGate(bool fail) : fail_(fail) {}
        void write(const can::Message &message) override {
            if (++writes_ == 2) {
                std::unique_lock<std::mutex> lock(mutex_);
                entered_ = true;
                cv_.notify_all();
                cv_.wait(lock, [&] { return released_; });
                if (fail_) {
                    throw can::TransportIOError("second write failed");
                }
            }
            can::FakeTransport::write(message);
        }
        bool wait_for_second() {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, 2s, [&] { return entered_; });
        }
        void release() {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
            cv_.notify_all();
        }

      private:
        bool fail_;
        size_t writes_ = 0;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool entered_ = false;
        bool released_ = false;
    };
    struct ReleaseSecondWrite {
        SecondWriteGate *gate;
        ~ReleaseSecondWrite() {
            gate->release();
        }
    };
} // namespace

TEST_CASE("Session pending receipts expose only the known accepted frame prefix", "[session]") {
    const bool fail = GENERATE(false, true);
    auto backend = std::make_unique<SecondWriteGate>(fail);
    auto *gate = backend.get();
    auto session = transport::Session::own(std::move(backend));
    const ReleaseSecondWrite release_on_exit{gate};
    transport::MessageBatch batch;
    batch.frames.resize(2);
    auto receipt = session->submit(std::move(batch));
    const bool entered = gate->wait_for_second();
    const auto pending = receipt.result();
    // Release before assertions so a failed expectation never strands the writer.
    gate->release();
    const auto completed = receipt.wait_result_until(transport::Clock::now() + 2s);
    session->close();
    REQUIRE(entered);
    REQUIRE(pending.outcome == transport::SendOutcome::pending);
    REQUIRE(pending.sent_frames == 1);
    REQUIRE(completed.sent_frames == (fail ? 1u : 2u));
    REQUIRE(completed.current_frame_uncertain == fail);
    REQUIRE(completed.outcome ==
            (fail ? transport::SendOutcome::failed : transport::SendOutcome::dispatched));
}

namespace {
    class GenerationTransport : public can::FakeTransport {
      public:
        GenerationTransport(uint64_t generation, std::atomic<bool> *mismatch)
            : generation_(generation), mismatch_(mismatch) {}
        void write(const can::Message &message) override {
            can::FakeTransport::write(message);
            if (message.id != generation_) {
                mismatch_->store(true);
            }
        }

      private:
        uint64_t generation_;
        std::atomic<bool> *mismatch_;
    };
} // namespace

TEST_CASE("Session generation admission stays aligned with physical recovery under health reads",
          "[session]") {
    std::atomic<uint64_t> physical_generation{0};
    std::atomic<bool> mismatch{false};
    std::atomic<bool> fail_next{false};
    auto session = transport::Session::create([&] {
        if (fail_next.exchange(false)) {
            throw can::TransportIOError("expected reopen failure");
        }
        return std::make_unique<GenerationTransport>(++physical_generation, &mismatch);
    });
    std::atomic<bool> stop{false};
    std::atomic<size_t> health_reads{0};
    std::atomic<size_t> attempted_sends{0};
    std::atomic<bool> unexpected{false};
    std::promise<void> start;
    auto started = start.get_future().share();
    auto observer = std::async(std::launch::async, [&] {
        started.wait();
        while (!stop.load()) {
            try {
                (void)session->health();
                ++health_reads;
            } catch (...) {
                unexpected.store(true);
                return;
            }
            std::this_thread::yield();
        }
    });
    auto sender = std::async(std::launch::async, [&] {
        started.wait();
        while (!stop.load()) {
            try {
                const auto observed = session->health().generation;
                transport::SendOptions options;
                options.expected_generation = observed;
                can::Message message;
                message.id = static_cast<uint32_t>(observed);
                ++attempted_sends;
                auto receipt = session->submit({{message}}, options);
                (void)receipt.wait_result_until(transport::Clock::now() + 2s);
            } catch (const can::ClosedError &) {
                // Recovery gates submission or invalidates the observed generation.
            } catch (...) {
                unexpected.store(true);
                return;
            }
            std::this_thread::yield();
        }
    });
    start.set_value();
    bool recovery_consistent = true;
    try {
        for (size_t round = 0; round < 32; ++round) {
            auto observed = session->health().generation;
            if (round % 4 == 0) {
                fail_next.store(true);
                bool failed = false;
                try {
                    (void)session->recover(observed);
                } catch (const can::TransportIOError &) {
                    failed = true;
                }
                recovery_consistent = recovery_consistent && failed && !session->health().open;
            }
            const auto recovered = session->recover(observed);
            recovery_consistent = recovery_consistent && recovered == physical_generation.load();
            // Give the two observers work between successful reopen boundaries.
            std::this_thread::yield();
        }
    } catch (...) {
        unexpected.store(true);
    }
    stop.store(true);
    observer.get();
    sender.get();
    const auto final_generation = session->health().generation;
    session->close();
    REQUIRE_FALSE(unexpected.load());
    REQUIRE(recovery_consistent);
    REQUIRE(health_reads.load() != 0);
    REQUIRE(attempted_sends.load() != 0);
    REQUIRE_FALSE(mismatch.load());
    REQUIRE(final_generation == physical_generation.load());
}

namespace {
    class TimeoutWriteTransport : public can::FakeTransport {
      public:
        void write(const can::Message &) override {
            throw can::TimeoutError("backend write timed out");
        }
    };
    struct ReleaseFakeWrite {
        can::FakeTransport *fake;
        ~ReleaseFakeWrite() {
            fake->release_tx_gate();
        }
    };
} // namespace

TEST_CASE("Session backend write timeouts retain expiry and count toward the fault threshold",
          "[session]") {
    auto session = transport::Session::own(std::make_unique<TimeoutWriteTransport>());
    transport::SessionOptions options;
    options.max_consecutive_send_errors = 2;
    session->configure(options);
    const auto first = session->submit({{{}}}).wait_result_until(transport::Clock::now() + 2s);
    REQUIRE(first.outcome == transport::SendOutcome::expired);
    REQUIRE(first.current_frame_uncertain);
    REQUIRE(session->health().open);
    REQUIRE(session->health().consecutive_send_errors == 1);
    const auto second = session->submit({{{}}}).wait_result_until(transport::Clock::now() + 2s);
    const auto health = session->health();
    session->close();
    REQUIRE(second.outcome == transport::SendOutcome::expired);
    REQUIRE(second.current_frame_uncertain);
    REQUIRE_FALSE(health.open);
    REQUIRE(health.consecutive_send_errors == 2);
    REQUIRE(health.expired == 2);
}

TEST_CASE("Session queued expiry does not count as a failed backend write", "[session]") {
    auto backend = std::make_unique<can::FakeTransport>();
    auto *fake = backend.get();
    auto session = transport::Session::own(std::move(backend));
    const ReleaseFakeWrite release_on_exit{fake};
    transport::SessionOptions config;
    config.max_consecutive_send_errors = 1;
    session->configure(config);
    fake->arm_tx_gate();
    auto blocker = session->submit({{{}}});
    const bool entered = fake->wait_until_tx_entered(2s);
    transport::SendOptions options;
    options.deadline = transport::Clock::now() + 500ms;
    auto queued = session->submit({{{}}}, options);
    std::this_thread::sleep_until(options.deadline + 1ms);
    fake->release_tx_gate();
    blocker.wait();
    const auto result = queued.wait_result_until(transport::Clock::now() + 2s);
    const auto health = session->health();
    const auto sent = fake->sent().size();
    session->close();
    REQUIRE(entered);
    REQUIRE(result.outcome == transport::SendOutcome::expired);
    REQUIRE_FALSE(result.current_frame_uncertain);
    REQUIRE(result.sent_frames == 0);
    REQUIRE(health.open);
    REQUIRE(health.consecutive_send_errors == 0);
    REQUIRE(health.expired == 1);
    REQUIRE(sent == 1);
}

namespace {
    class PrefixTimeoutTransport : public can::FakeTransport {
      public:
        void write(const can::Message &message) override {
            if (message.id == 1) {
                can::FakeTransport::write(message);
            } else {
                throw can::TimeoutError("backend write timed out");
            }
        }
    };
} // namespace

TEST_CASE("Session successful prefixes reset earlier backend failure streaks", "[session]") {
    auto session = transport::Session::own(std::make_unique<PrefixTimeoutTransport>());
    transport::SessionOptions options;
    options.max_consecutive_send_errors = 2;
    session->configure(options);
    const auto first = session->submit({{{}}}).wait_result_until(transport::Clock::now() + 2s);
    REQUIRE(first.outcome == transport::SendOutcome::expired);
    REQUIRE(session->health().consecutive_send_errors == 1);
    can::Message accepted;
    accepted.id = 1;
    const auto partial =
        session->submit({{accepted, {}}}).wait_result_until(transport::Clock::now() + 2s);
    const auto after_partial = session->health();
    REQUIRE(partial.outcome == transport::SendOutcome::expired);
    REQUIRE(partial.sent_frames == 1);
    REQUIRE(partial.current_frame_uncertain);
    REQUIRE(after_partial.consecutive_send_errors == 1);
    REQUIRE(after_partial.open);
    const auto final = session->submit({{{}}}).wait_result_until(transport::Clock::now() + 2s);
    const auto after_final = session->health();
    session->close();
    REQUIRE(final.outcome == transport::SendOutcome::expired);
    REQUIRE(final.sent_frames == 0);
    REQUIRE(final.current_frame_uncertain);
    REQUIRE(after_final.consecutive_send_errors == 2);
    REQUIRE_FALSE(after_final.open);
}
