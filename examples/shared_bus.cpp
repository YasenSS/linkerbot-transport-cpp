// Two devices share one Session, with a separate receive route for each ID.
// Uses FakeTransport so the example runs without hardware.
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

namespace can = linkerbot::can;
namespace transport = linkerbot::transport;

namespace {
    constexpr uint32_t kLeftHandId = 0x28;
    constexpr uint32_t kRightHandId = 0x27;
    constexpr uint8_t kSenseAngle = 0x02;

    can::Message sense_request(uint32_t id) {
        can::Message message;
        message.id = id;
        message.dlc = 1;
        message.data[0] = kSenseAngle;
        return message;
    }
} // namespace

int main() {
    try {
        auto adapter = std::make_unique<can::FakeTransport>();
        adapter->echo_cmd(kLeftHandId, kSenseAngle, 6, 0x40);
        adapter->echo_cmd(kRightHandId, kSenseAngle, 6, 0x60);
        auto session = transport::Session::own(std::move(adapter));

        // Register before sending so a fast reply cannot be missed.
        auto left = session->subscribe_routes({{kLeftHandId, false}});
        auto right = session->subscribe_routes({{kRightHandId, false}});
        transport::SendOptions options;
        options.deadline = transport::Clock::now() + std::chrono::seconds{2};
        auto receipt =
            session->submit({{sense_request(kLeftHandId), sense_request(kRightHandId)}}, options);
        receipt.wait_until(options.deadline);

        const auto left_reply = left->receive_until(options.deadline);
        const auto right_reply = right->receive_until(options.deadline);
        session->close();
        if (!left_reply || !right_reply) {
            std::cerr << "Timed out waiting for device replies\n";
            return 1;
        }
        for (const auto *reply : {&*left_reply, &*right_reply}) {
            if (reply->message.dlc != 7 || reply->message.data[0] != kSenseAngle) {
                std::cerr << "Unexpected device reply\n";
                return 1;
            }
        }
        std::cout << "left  angle[0] = " << static_cast<int>(left_reply->message.data[1]) << '\n'
                  << "right angle[0] = " << static_cast<int>(right_reply->message.data[1]) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
