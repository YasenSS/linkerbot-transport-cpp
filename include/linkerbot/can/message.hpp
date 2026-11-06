#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace linkerbot::can {

    // Classic CAN uses up to 8 bytes; CAN FD up to 64.
    inline constexpr size_t kMaxDataLen = 64;

    // Unspecified preserves each backend's existing untagged-frame behavior.
    enum class FrameFormat : int { kUnspecified, kClassic, kFd };
    enum class TimestampClock { kDevice, kSystemRealtime, kVendorRaw, kDriver };
    enum class TimestampUnit { kNanoseconds, kMicroseconds, kRawTicks };
    enum class TimestampSource { kHardware, kSoftware, kDriver };
    struct ReceiveTimestamp {
        uint64_t value = 0;
        TimestampUnit unit = TimestampUnit::kRawTicks;
        TimestampClock clock = TimestampClock::kVendorRaw;
        TimestampSource source = TimestampSource::kDriver;
        uint8_t counter_bits = 64;
    };

    // One CAN / CAN FD frame, shaped so a single struct serves both. `dlc` is a
    // payload length in bytes (NOT the 0-15 wire DLC nibble); backends that speak
    // wire DLC convert at their boundary.
    //
    // `format` selects an explicit TX format; kUnspecified retains the backend
    // interpretation of is_fd/is_brs. Validation and defaults are in frame.hpp.
    struct Message {
        uint32_t id = 0;
        std::array<uint8_t, kMaxDataLen> data = {};
        uint8_t dlc = 0; // payload length in bytes (0–8 classic, 0–64 FD)
        bool is_extended = false;
        bool is_fd = false;
        bool is_brs = false;
        FrameFormat format = FrameFormat::kUnspecified;
        // RX metadata only. TX ignores it; interpret the recorded clock and unit.
        std::optional<ReceiveTimestamp> timestamp = std::nullopt;
        std::optional<bool> is_rx = std::nullopt;
    };

} // namespace linkerbot::can
