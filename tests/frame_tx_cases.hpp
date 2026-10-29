#pragma once

#include <linkerbot/can/frame.hpp>

#include <cstddef>
#include <cstdint>

// Shared TX mapping vectors for SocketCAN and vendor backends. `expect_frame_type`
// is the vendor FrameType byte; SocketCAN uses the same resolved is_fd / is_brs.

namespace linkerbot::can::test {

    struct FrameTxCase {
        const char *name;
        LinkKind link;
        bool is_fd;
        bool is_brs;
        bool expect_ok;
        bool expect_is_fd;
        bool expect_is_brs;
        uint8_t expect_frame_type;
    };

    inline constexpr FrameTxCase kFrameTxCases[] = {
        {"fd-untagged", LinkKind::kCanFd, false, false, true, true, false, kFrameTypeFd},
        {"fd-explicit", LinkKind::kCanFd, true, false, true, true, false, kFrameTypeFd},
        {"fd-brs", LinkKind::kCanFd, true, true, true, true, true,
         static_cast<uint8_t>(kFrameTypeFd | kFrameTypeBrs)},
        {"fd-brs-without-fd", LinkKind::kCanFd, false, true, false, false, false, 0},
        {"classic-untagged", LinkKind::kClassic, false, false, true, false, false, 0},
        {"classic-fd", LinkKind::kClassic, true, false, false, false, false, 0},
        {"classic-brs", LinkKind::kClassic, true, true, false, false, false, 0},
        {"classic-brs-without-fd", LinkKind::kClassic, false, true, false, false, false, 0},
    };

    inline constexpr std::size_t kFrameTxCaseCount =
        sizeof(kFrameTxCases) / sizeof(kFrameTxCases[0]);

    struct FrameRxCase {
        const char *name;
        uint8_t frame_type;
        bool expect_ok;
        bool expect_is_fd;
        bool expect_is_brs;
    };

    inline constexpr FrameRxCase kFrameRxCases[] = {
        {"classic", 0x00, true, false, false},
        {"fd", kFrameTypeFd, true, true, false},
        {"fd-brs", static_cast<uint8_t>(kFrameTypeFd | kFrameTypeBrs), true, true, true},
        {"brs-without-fd", kFrameTypeBrs, false, false, false},
    };

    inline constexpr std::size_t kFrameRxCaseCount =
        sizeof(kFrameRxCases) / sizeof(kFrameRxCases[0]);

    inline auto make_tx_message(const FrameTxCase &row) -> Message {
        Message msg;
        msg.id = 0x123;
        msg.dlc = 1;
        msg.data[0] = 0x01;
        msg.is_fd = row.is_fd;
        msg.is_brs = row.is_brs;
        return msg;
    }

} // namespace linkerbot::can::test
