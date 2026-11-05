#include <chrono>
#include <iostream>
#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
    using namespace std::chrono_literals;
    using namespace linkerbot;

    void check(bool value, const char *message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    }

    struct ReleaseGate {
        can::FakeTransport *backend;
        ~ReleaseGate() {
            backend->release_tx_gate();
        }
    };

    void run_consumer() {
        auto backend = std::make_unique<can::FakeTransport>();
        auto *fake = backend.get();
        auto session = transport::Session::own(std::move(backend));
        const ReleaseGate gate{fake};
        transport::SessionOptions settings;
        settings.separate_policy_capacity = true;
        settings.fifo_unit = transport::CapacityUnit::kFrames;
        settings.fifo_capacity = 2;
        settings.latest_capacity = 1;
        settings.endpoint_fifo = true;
        settings.aging_quantum = 50ms;
        session->configure(settings);
        check(session->options().fifo_unit == transport::CapacityUnit::kFrames,
              "installed Session options differ");
        session->set_route_key_extractor([](const can::Message &message) {
            return transport::RouteKey{message.id, message.is_extended};
        });
        auto inbox = session->subscribe_routes({{0x28, false}}, 2);
        auto signal = session->notification();
        check(inbox->notification() == signal, "inbox signal is not shared");
        const auto observed = signal->revision();
        fake->arm_tx_gate();
        transport::SendOptions sending;
        sending.endpoint = "blocker";
        sending.expected_generation = session->health().generation;
        sending.deadline_policy = transport::DeadlinePolicy::kStartOnly;
        const transport::MessageBatch single{{can::Message{}}};
        auto blocker = session->submit(single, sending);
        check(fake->wait_until_tx_entered(2s), "installed writer did not enter backend");
        check(blocker.notification() == signal, "receipt signal is not shared");
        check(signal->wait_until(observed, transport::Clock::now() + 2s),
              "claim notification was lost");

        transport::SendOptions arm;
        arm.endpoint = "arm";
        auto group = session->submit({{can::Message{}, can::Message{}}}, arm);
        bool rejected = false;
        try {
            (void)session->submit(single, arm);
        } catch (const can::QueueFullError &) {
            rejected = true;
        }
        check(rejected && session->health().pending_fifo == 2,
              "installed FIFO did not charge frame capacity");

        transport::SendOptions latest;
        latest.endpoint = "hand";
        latest.family = "angles";
        latest.policy = transport::SendPolicy::latest;
        auto old = session->submit(single, latest);
        auto replacement = session->replace_latest_if_pending(single, latest);
        check(replacement.has_value(), "installed conditional replacement missed pending slot");
        check(old.wait_result_until(transport::Clock::now() + 2s).outcome ==
                  transport::SendOutcome::superseded,
              "installed receipt did not return superseded value");
        check(session->stop_endpoint("hand", transport::Clock::now() + 2s) == 1,
              "installed endpoint stop did not remove pending slot");
        check(replacement->wait_result_until(transport::Clock::now() + 2s).outcome ==
                  transport::SendOutcome::cancelled,
              "installed endpoint stop did not complete receipt");
        session->resume_endpoint("hand");
        fake->release_tx_gate();
        (void)blocker.wait_until(transport::Clock::now() + 2s);
        (void)group.wait_until(transport::Clock::now() + 2s);
        check(fake->sent().size() == 3, "installed Session dispatched cancelled work");
        session->close();
        check(inbox->health().closed, "installed close left inbox open");
    }
} // namespace

int main() {
    try {
        run_consumer();
        std::cout << "Installed Session compatibility APIs passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Installed Session compatibility consumer failed: " << error.what() << '\n';
        return 1;
    }
}
