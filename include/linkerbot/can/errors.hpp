#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace linkerbot::can {

    enum class ErrorCode {
        kValidation,
        kClosed,
        kTimeout,
        kTransportIo,
        kFaulted,
        kQueueFull,
        kInternal,
    };

    [[nodiscard]] inline auto error_code_name(ErrorCode code) noexcept -> const char * {
        switch (code) {
        case ErrorCode::kValidation:
            return "validation";
        case ErrorCode::kClosed:
            return "closed";
        case ErrorCode::kTimeout:
            return "timeout";
        case ErrorCode::kTransportIo:
            return "transport_io";
        case ErrorCode::kFaulted:
            return "faulted";
        case ErrorCode::kQueueFull:
            return "queue_full";
        case ErrorCode::kInternal:
            return "internal";
        }
        return "internal";
    }

    class CANError : public std::runtime_error {
      public:
        CANError(ErrorCode code, std::string message, std::error_code system_error = {},
                 std::optional<int> native_status = std::nullopt)
            : std::runtime_error(std::move(message)), code_(code), system_error_(system_error),
              native_status_(native_status) {}

        [[nodiscard]] auto code() const noexcept -> ErrorCode {
            return code_;
        }

        [[nodiscard]] auto system_error() const noexcept -> const std::error_code & {
            return system_error_;
        }

        [[nodiscard]] auto native_status() const noexcept -> std::optional<int> {
            return native_status_;
        }

      private:
        ErrorCode code_;
        std::error_code system_error_;
        std::optional<int> native_status_;
    };

    class ValidationError : public CANError {
      public:
        explicit ValidationError(std::string message)
            : CANError(ErrorCode::kValidation, std::move(message)) {}
    };

    class ClosedError : public CANError {
      public:
        explicit ClosedError(std::string message)
            : CANError(ErrorCode::kClosed, std::move(message)) {}
    };

    class TimeoutError : public CANError {
      public:
        explicit TimeoutError(std::string message, std::error_code system_error = {})
            : CANError(ErrorCode::kTimeout, std::move(message), system_error) {}
    };

    class TransportIOError : public CANError {
      public:
        explicit TransportIOError(std::string message, std::error_code system_error = {},
                                  std::optional<int> native_status = std::nullopt)
            : CANError(ErrorCode::kTransportIo, std::move(message), system_error, native_status) {}
    };

    class FaultedError : public CANError {
      public:
        explicit FaultedError(std::string message)
            : CANError(ErrorCode::kFaulted, std::move(message)) {}
    };

    class QueueFullError : public CANError {
      public:
        explicit QueueFullError(std::string message)
            : CANError(ErrorCode::kQueueFull, std::move(message)) {}
    };

    class InternalError : public CANError {
      public:
        explicit InternalError(std::string message)
            : CANError(ErrorCode::kInternal, std::move(message)) {}
    };

} // namespace linkerbot::can
