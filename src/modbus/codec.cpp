#include <linkerbot/modbus/rtu.hpp>

#include <string>

namespace linkerbot::modbus {
    namespace {
        void append_word(Bytes &bytes, uint16_t value) {
            bytes.push_back(static_cast<uint8_t>(value >> 8));
            bytes.push_back(static_cast<uint8_t>(value & 0xffU));
        }
    } // namespace
    ExceptionResponse::ExceptionResponse(uint8_t code)
        : ProtocolError("Modbus exception " + std::to_string(code)), code_(code) {}
    uint8_t ExceptionResponse::code() const noexcept {
        return code_;
    }

    void validate_request(const Request &request) {
        if (request.unit_id == 0 || request.unit_id > 247) {
            throw std::invalid_argument(
                "Modbus unit ID must be in 1..247; broadcasts are disabled");
        }
        const bool reading = request.function == 3 || request.function == 4;
        if (!reading && request.function != 6 && request.function != 16) {
            throw std::invalid_argument("Unsupported Modbus function");
        }
        uint16_t limit = 123;
        if (reading) {
            limit = 125;
        } else if (request.function == 6) {
            limit = 1;
        }
        if (request.quantity == 0 || request.quantity > limit ||
            uint32_t{request.address} + request.quantity > 65536U ||
            (reading && !request.values.empty()) ||
            (!reading && request.values.size() != request.quantity)) {
            throw std::invalid_argument("Invalid Modbus register count, range or values");
        }
    }
    uint16_t crc16(const uint8_t *data, size_t size) {
        if (data == nullptr && size != 0) {
            throw std::invalid_argument("Null CRC buffer");
        }
        uint16_t crc = 0xffff;
        for (size_t index = 0; index < size; ++index) {
            crc ^= data[index];
            for (int bit = 0; bit < 8; ++bit) {
                crc = static_cast<uint16_t>((crc >> 1) ^ ((crc & 1U) != 0 ? 0xa001U : 0U));
            }
        }
        return crc;
    }
    Bytes encode_request(const Request &request) {
        validate_request(request);
        Bytes frame{request.unit_id, request.function};
        append_word(frame, request.address);
        if (request.function == 6) {
            append_word(frame, request.values.front());
        } else {
            append_word(frame, request.quantity);
            if (request.function == 16) {
                frame.push_back(static_cast<uint8_t>(request.values.size() * 2));
                for (const auto value : request.values) {
                    append_word(frame, value);
                }
            }
        }
        const uint16_t crc = crc16(frame.data(), frame.size());
        frame.push_back(static_cast<uint8_t>(crc & 0xffU));
        frame.push_back(static_cast<uint8_t>(crc >> 8));
        return frame;
    }
    Bytes decode_response(const Request &request, const Bytes &frame) {
        validate_request(request);
        if (frame.size() < 5 || frame.size() > 256) {
            throw ProtocolError("Invalid RTU response length");
        }
        if (crc16(frame.data(), frame.size()) != 0) {
            throw CrcError("Invalid RTU response CRC");
        }
        if (frame[0] != request.unit_id) {
            throw ProtocolError("Unexpected Modbus unit ID");
        }
        if (frame[1] == static_cast<uint8_t>(request.function | 0x80U)) {
            if (frame.size() != 5 || frame[2] == 0) {
                throw ProtocolError("Invalid Modbus exception response");
            }
            throw ExceptionResponse(frame[2]);
        }
        if (frame[1] != request.function) {
            throw ProtocolError("Unexpected Modbus function");
        }
        if (request.function == 3 || request.function == 4) {
            if (frame.size() != size_t{request.quantity} * 2 + 5 ||
                frame[2] != request.quantity * 2) {
                throw ProtocolError("Unexpected Modbus register byte count");
            }
            return {frame.begin() + 3, frame.end() - 2};
        }
        const auto encoded = encode_request(request);
        if (frame.size() != 8 || frame[2] != encoded[2] || frame[3] != encoded[3] ||
            frame[4] != encoded[4] || frame[5] != encoded[5]) {
            throw ProtocolError("Unexpected Modbus write echo");
        }
        return {frame.begin() + 2, frame.end() - 2};
    }
    Timing rtu_timing(const serial::Config &config) {
        serial::validate_config(config);
        if (config.data_bits != 8) {
            throw std::invalid_argument("Modbus RTU requires eight data bits");
        }
        if (config.baud_rate > 19200) {
            return {std::chrono::microseconds{750}, std::chrono::microseconds{1750}};
        }
        const uint64_t bits = 1U + config.data_bits +
                              (config.parity == serial::Parity::kNone ? 0U : 1U) +
                              (config.stop_bits == serial::StopBits::kOne ? 1U : 2U);
        const auto duration = [&](uint64_t halves) {
            const uint64_t numerator = bits * halves * 1000000U;
            const uint64_t denominator = uint64_t{config.baud_rate} * 2;
            return std::chrono::microseconds{(numerator + denominator - 1) / denominator};
        };
        return {duration(3), duration(7)};
    }
    void TransactionResult::rethrow_if_error() const {
        if (error) {
            std::rethrow_exception(error);
        }
    }
} // namespace linkerbot::modbus
