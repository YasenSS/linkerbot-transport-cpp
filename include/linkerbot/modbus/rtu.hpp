#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <linkerbot/serial/byte_stream.hpp>

namespace linkerbot::modbus {
    using transport::CancellationToken;
    using transport::Deadline;
    using Bytes = std::vector<uint8_t>;

    class ProtocolError : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
    class CrcError : public ProtocolError {
      public:
        using ProtocolError::ProtocolError;
    };
    class FaultedError : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
    class QueueFullError : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
    class ExceptionResponse : public ProtocolError {
      public:
        explicit ExceptionResponse(uint8_t code);
        [[nodiscard]] uint8_t code() const noexcept;

      private:
        uint8_t code_;
    };

    struct Request {
        uint8_t unit_id = 1;
        uint8_t function = 3;
        uint16_t address = 0;
        uint16_t quantity = 1;
        std::vector<uint16_t> values;
    };

    // Supported functions: 03, 04, 06 and 10. Broadcasts are rejected.
    // Addresses are zero-based PDU addresses, not 4xxxx register references.
    void validate_request(const Request &request);
    uint16_t crc16(const uint8_t *data, size_t size);
    Bytes encode_request(const Request &request);
    // Reads return register bytes without the byte count. Writes return the
    // four-byte address/value (06) or address/quantity (10) echo, in wire order.
    // Throws ExceptionResponse for a valid device exception. All frames require CRC.
    Bytes decode_response(const Request &request, const Bytes &frame);

    struct Timing {
        std::chrono::microseconds inter_character;
        std::chrono::microseconds inter_frame;
    };
    Timing rtu_timing(const serial::Config &config);

    struct RtuConfig {
        serial::Config serial;
        size_t queue_capacity = 16;      // Waiting transactions, excluding the active one.
        bool discard_local_echo = false; // Only for adapters known to echo every request.
    };
    struct TransactionOptions {
        Deadline deadline = Deadline::max();
        CancellationToken cancellation;
    };
    enum class Outcome { kResponse, kException, kFailed, kCancelled, kExpired };
    struct TransactionResult {
        Outcome outcome = Outcome::kFailed;
        uint64_t generation = 0;
        Bytes data;
        size_t bytes_sent = 0; // Known completed write_some calls only.
        bool request_dispatched = false;
        bool delivery_uncertain = false;
        std::exception_ptr error;
        Deadline accepted_at{};
        Deadline completed_at{};
        void rethrow_if_error() const;
    };
    struct RtuHealth {
        bool open = false;
        bool faulted = false;
        bool recovering = false;
        uint64_t generation = 1;
        uint64_t transactions = 0;
        uint64_t exceptions = 0;
        uint64_t crc_errors = 0;
        uint64_t timeouts = 0;
        uint64_t recoveries = 0;
        size_t pending = 0;
        size_t high_water = 0;
    };

    // Share this object between all clients on one serial line. FIFO admission
    // and request/reply I/O are serialized across unit IDs. No automatic retries.
    // An uncertain transaction faults the whole line until explicit recover().
    class RtuSession {
      public:
        using Factory = std::function<std::unique_ptr<serial::ByteStream>(const serial::Config &)>;
        static std::shared_ptr<RtuSession> create(RtuConfig config,
                                                  Factory factory = serial::open_serial);
        ~RtuSession();
        RtuSession(const RtuSession &) = delete;
        RtuSession &operator=(const RtuSession &) = delete;
        // Invalid/admission errors throw. Once admitted, errors live in result.
        [[nodiscard]] TransactionResult transact(const Request &request,
                                                 TransactionOptions options);
        [[nodiscard]] RtuHealth health() const;
        // Invalidates the previous line and recreates its stream. Does not prove
        // device identity or purge delayed device replies: the next transaction
        // still enforces an idle line. No targets are replayed. Call externally.
        uint64_t recover(uint64_t observed_generation);
        void close();

      private:
        struct Impl;
        explicit RtuSession(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    class Client {
      public:
        Client(std::shared_ptr<RtuSession> session, uint8_t unit_id);
        std::vector<uint16_t> read_holding_registers(uint16_t address, uint16_t count,
                                                     TransactionOptions options);
        std::vector<uint16_t> read_input_registers(uint16_t address, uint16_t count,
                                                   TransactionOptions options);
        [[nodiscard]] TransactionResult write_single_register(uint16_t address, uint16_t value,
                                                              TransactionOptions options);
        [[nodiscard]] TransactionResult write_multiple_registers(uint16_t address,
                                                                 std::vector<uint16_t> values,
                                                                 TransactionOptions options);

      private:
        std::vector<uint16_t> read(uint8_t function, uint16_t address, uint16_t count,
                                   TransactionOptions options);
        std::shared_ptr<RtuSession> session_;
        uint8_t unit_id_;
    };
} // namespace linkerbot::modbus
