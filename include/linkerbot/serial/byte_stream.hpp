#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <linkerbot/transport/cancellation.hpp>

namespace linkerbot::serial {
    using transport::CancellationToken;
    using transport::Deadline;

    enum class Parity { kNone, kEven, kOdd };
    enum class StopBits { kOne, kTwo };
    enum class Rs485Mode { kAdapterControlled, kKernel };

    struct Config {
        std::string port;
        uint32_t baud_rate = 115200;
        uint8_t data_bits = 8;
        Parity parity = Parity::kEven;
        StopBits stop_bits = StopBits::kOne;
        Rs485Mode rs485 = Rs485Mode::kAdapterControlled;
        bool rts_high_during_send = true;
    };

    class IOError : public std::system_error {
      public:
        explicit IOError(const std::string &message, std::error_code error = {})
            : std::system_error(error, message) {}
    };
    class ClosedError : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
    class TimeoutError : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };

    void validate_config(const Config &config);

    // One reader and one writer may run concurrently. close()/cancel() interrupt
    // both. cancel() is permanent; reopen by constructing a new stream. A caller
    // must retain ownership until all operations have returned.
    class ByteStream {
      public:
        virtual ~ByteStream() = default;
        // Returns zero only when the deadline expires (or capacity is zero).
        // EOF, cancellation and I/O failures throw. A successful read can be short.
        virtual size_t read_some(uint8_t *data, size_t capacity, Deadline deadline,
                                 CancellationToken cancellation = {}) = 0;
        virtual size_t write_some(const uint8_t *data, size_t size, Deadline deadline,
                                  CancellationToken cancellation = {}) = 0;
        // Wait for the driver output queue to empty. This is not a peer ACK.
        virtual void drain_output_until(Deadline deadline, CancellationToken cancellation = {}) = 0;
        virtual void cancel() noexcept = 0;
        virtual void close() = 0;
    };

    // Validates without discovery; opens exactly the requested port exclusively.
    // Linux and Windows are supported. kKernel RS485 is Linux-only and requires
    // driver support. Automatic direction adapters use kAdapterControlled.
    std::unique_ptr<ByteStream> open_serial(const Config &config);
} // namespace linkerbot::serial
