#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

int main() {
    namespace can = linkerbot::can;
    namespace transport = linkerbot::transport;
    try {
        auto adapter = std::make_unique<can::FakeTransport>();
        // Reply to command 0x02 with the command byte followed by 0x40.
        adapter->echo_cmd(0x28, 0x02, 1, 0x40);
        auto session = transport::Session::own(std::move(adapter));
        auto inbox = session->subscribe_routes({{0x28, false}});

        can::Message request;
        request.id = 0x28;
        request.dlc = 1;
        request.data[0] = 0x02;
        transport::SendOptions options;
        options.deadline = transport::Clock::now() + std::chrono::seconds{2};
        const auto receipt = session->submit({{request}}, options);
        const auto result = receipt.wait_until(options.deadline);
        const auto reply = inbox->receive_until(options.deadline);
        session->close();

        if (result.outcome != transport::SendOutcome::dispatched || result.sent_frames != 1 ||
            !reply || reply->message.id != request.id || reply->message.dlc != 2 ||
            reply->message.data[0] != 0x02 || reply->message.data[1] != 0x40) {
            std::cerr << "Unexpected send result or in-memory reply\n";
            return 1;
        }
        std::cout << "In-memory send and receive passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Quickstart failed: " << error.what() << '\n';
        return 1;
    }
}
