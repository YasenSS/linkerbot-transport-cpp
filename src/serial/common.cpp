#include <linkerbot/serial/byte_stream.hpp>

namespace linkerbot::serial {
    void validate_config(const Config &config) {
        if (config.port.empty() || config.port.find('\0') != std::string::npos ||
            config.baud_rate == 0 || config.baud_rate > 4000000 || config.data_bits < 5 ||
            config.data_bits > 8) {
            throw std::invalid_argument("Invalid serial port, baud rate or data bits");
        }
        if (config.parity != Parity::kNone && config.parity != Parity::kEven &&
            config.parity != Parity::kOdd) {
            throw std::invalid_argument("Invalid serial parity");
        }
        if (config.stop_bits != StopBits::kOne && config.stop_bits != StopBits::kTwo) {
            throw std::invalid_argument("Invalid serial stop bits");
        }
        if (config.rs485 != Rs485Mode::kAdapterControlled && config.rs485 != Rs485Mode::kKernel) {
            throw std::invalid_argument("Invalid RS485 mode");
        }
    }
} // namespace linkerbot::serial
