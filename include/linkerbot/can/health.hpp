#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <linkerbot/can/errors.hpp>
#include <optional>
#include <string>
namespace linkerbot::can {
    enum class TransportState {
        kClosed,
        kStarting,
        kOpen,
        kStopping,
        kFaulted,
    };

    struct TransportHealth {
        TransportState state = TransportState::kClosed;
        uint64_t generation = 0;
        uint32_t consecutive_errors = 0;
        std::optional<ErrorCode> last_error_code;
        std::string last_error;
        std::optional<std::chrono::steady_clock::time_point> last_successful_tx;
        std::optional<std::chrono::steady_clock::time_point> last_successful_rx;
        size_t subscriber_count = 0;
        size_t disabled_subscriber_count = 0;
        std::string last_subscriber_error;
        size_t pending_tx = 0;

        [[nodiscard]] auto connected() const noexcept -> bool {
            return state == TransportState::kOpen;
        }
    };

} // namespace linkerbot::can
