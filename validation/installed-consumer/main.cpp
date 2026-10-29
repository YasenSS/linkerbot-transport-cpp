#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/pcan_transport.hpp>
#include <linkerbot/can/slcan_transport.hpp>
#include <linkerbot/can/vendor_canfd_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <can_transport.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <utility>

int main() {
    linkerbot::can::FakeTransport transport;
    linkerbot::can::Message message;
    message.id = 0x123;
    message.dlc = 1;
    message.data[0] = 42;
    transport.write(message);
    if (transport.sent().size() != 1) return 1;
    transport.close();
    // Verify installed Session symbols and routing without hardware.
    auto adapter = std::make_unique<linkerbot::can::FakeTransport>();
    adapter->echo_cmd(message.id, 42, 1, 7);
    auto session = linkerbot::transport::Session::own(std::move(adapter));
    auto inbox = session->subscribe_routes({{message.id, false}});
    linkerbot::transport::SendOptions options;
    options.deadline = linkerbot::transport::Clock::now() + std::chrono::seconds{2};
    session->send({{message}}, options);
    const auto reply = inbox->receive_until(options.deadline);
    session->close();
    if (!reply || reply->message.id != message.id || reply->message.dlc != 2 ||
        reply->message.data[1] != 7 || !inbox->health().closed) return 6;
    // Exercise installed SLCAN symbols without opening any COM port.
    linkerbot::can::SlcanConfig serial;
    serial.port = "COM10";
    linkerbot::can::validate_slcan_config(serial);
    const auto encoded = linkerbot::can::encode_slcan_frame(message);
    const auto decoded = linkerbot::can::decode_slcan_frame(encoded.substr(0, encoded.size() - 1));
    if (decoded.id != message.id || decoded.data[0] != 42 ||
        !linkerbot::can::enumerate_slcan_ports({}).empty()) return 4;
    serial.protocol = linkerbot::can::SlcanProtocol::kCanableFd;
    serial.fd = true;
    linkerbot::can::validate_slcan_config(serial);
    message.is_fd = true; message.dlc = 64; message.is_brs = true;
    const auto fd = linkerbot::can::encode_slcan_frame(message, serial.protocol);
    const auto roundtrip = linkerbot::can::decode_slcan_frame(fd.substr(0,fd.size()-1),serial.protocol);
    if (!roundtrip.is_fd || !roundtrip.is_brs || roundtrip.dlc != 64) return 5;
    // Explicit missing paths make this independent of machine-installed drivers.
    try {
        (void)linkerbot::can::load_pcan_api("__missing_linkerbot_driver__/PCANBasic.dll");
        return 2;
    } catch (const linkerbot::can::TransportIOError &) {}
    try {
        (void)linkerbot::can::load_vendor_canfd_api("__missing_linkerbot_driver__/HCanbus.dll");
        return 3;
    } catch (const linkerbot::can::TransportIOError &) {}
    std::cout << "Installed comm and missing-driver behavior passed\n";
}
