#pragma once

#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>

inline void run_comm_smoke() {
    namespace can = linkerbot::can;
    namespace transport = linkerbot::transport;
    auto adapter = std::make_unique<can::FakeTransport>();
    adapter->echo_cmd(0x28, 0x02, 1, 0x40);
    auto session = transport::Session::own(std::move(adapter));
    auto inbox = session->subscribe_routes({{0x28, false}});
    can::Message request;
    request.id = 0x28;
    request.dlc = 1;
    request.data[0] = 0x02;
    transport::SendOptions options;
    options.deadline = transport::Clock::now() + std::chrono::seconds{2};
    const auto result = session->submit({{request}}, options).wait_until(options.deadline);
    const auto reply = inbox->receive_until(options.deadline);
    session->close();
    if (result.outcome != transport::SendOutcome::dispatched || result.sent_frames != 1 || !reply ||
        reply->message.id != request.id || reply->message.dlc != 2 ||
        reply->message.data[0] != 0x02 || reply->message.data[1] != 0x40) {
        throw std::runtime_error("ROS consumer in-memory transport failed");
    }
}
