#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace linkerbot::can {

    // Classic CAN uses up to 8 bytes; CAN FD up to 64.
    inline constexpr size_t kMaxDataLen = 64;

    // One CAN / CAN FD frame, shaped so a single struct serves both. `dlc` is a
    // payload length in bytes (NOT the 0-15 wire DLC nibble); backends that speak
    // wire DLC convert at their boundary.
    //
    // `is_fd` / `is_brs` are the per-frame FD/BRS flags. Encoding, illegal-combo
    // rejection, and untagged defaults are in frame.hpp.
    struct Message {
        uint32_t id = 0;
        std::array<uint8_t, kMaxDataLen> data = {};
        uint8_t dlc = 0; // payload length in bytes (0–8 classic, 0–64 FD)
        bool is_extended = false;
        bool is_fd = false;
        bool is_brs = false;
    };

} // namespace linkerbot::can
