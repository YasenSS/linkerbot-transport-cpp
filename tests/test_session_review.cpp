#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
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
} // namespace

TEST_CASE("Session pending receipts expose only the known accepted frame prefix", "[session]") {
    const bool fail = GENERATE(false, true);
    auto backend = std::make_unique<SecondWriteGate>(fail);
    auto *gate = backend.get();
    auto session = transport::Session::own(std::move(backend));
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
