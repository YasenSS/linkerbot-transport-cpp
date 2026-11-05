#include <linkerbot/modbus/rtu.hpp>
#include <linkerbot/serial/byte_stream.hpp>

#include <stdexcept>

int main() {
    linkerbot::serial::Config config;
    config.port = "not-opened";
    linkerbot::serial::validate_config(config);
    const linkerbot::modbus::Request request{1, 6, 0x1234, 1, {0xabcd}};
    const auto frame = linkerbot::modbus::encode_request(request);
    if (linkerbot::modbus::decode_response(request, frame) !=
        linkerbot::modbus::Bytes{0x12, 0x34, 0xab, 0xcd}) {
        throw std::runtime_error("Installed RTU codec mismatch");
    }
    return 0;
}
