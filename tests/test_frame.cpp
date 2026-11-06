#include <catch2/catch_test_macros.hpp>

#include "frame_tx_cases.hpp"

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/message.hpp>
#if defined(__linux__)
#include <linkerbot/can/socketcan_frame.hpp>
#endif

#include <cstddef>
#include <stdexcept>

using linkerbot::can::decode_frame_type;
using linkerbot::can::encode_frame_type;
using linkerbot::can::kExtendedIdMask;
using linkerbot::can::kFrameTypeBrs;
using linkerbot::can::kFrameTypeFd;
using linkerbot::can::kStandardIdMask;
using linkerbot::can::LinkKind;
using linkerbot::can::Message;
using linkerbot::can::resolve_tx_flags;
using linkerbot::can::ValidationError;
#if defined(__linux__)
using linkerbot::can::SocketCANTxFrame;
#endif
using linkerbot::can::validate_arbitration_id;
using linkerbot::can::wire_frame_type;
using linkerbot::can::test::kFrameRxCaseCount;
using linkerbot::can::test::kFrameRxCases;
using linkerbot::can::test::kFrameTxCaseCount;
using linkerbot::can::test::kFrameTxCases;
using linkerbot::can::test::make_tx_message;

TEST_CASE("encode/decode FrameType bits", "[frame]") {
    REQUIRE(encode_frame_type(false, false) == 0);
    REQUIRE(encode_frame_type(true, false) == kFrameTypeFd);
    REQUIRE(encode_frame_type(true, true) == static_cast<uint8_t>(kFrameTypeFd | kFrameTypeBrs));
    REQUIRE_THROWS_AS(encode_frame_type(false, true), ValidationError);

    auto classic = decode_frame_type(0);
    REQUIRE_FALSE(classic.is_fd);
    REQUIRE_FALSE(classic.is_brs);

    auto fd = decode_frame_type(kFrameTypeFd);
    REQUIRE(fd.is_fd);
    REQUIRE_FALSE(fd.is_brs);

    auto brs = decode_frame_type(static_cast<uint8_t>(kFrameTypeFd | kFrameTypeBrs));
    REQUIRE(brs.is_fd);
    REQUIRE(brs.is_brs);

    REQUIRE_THROWS_AS(decode_frame_type(kFrameTypeBrs), ValidationError);
}

TEST_CASE("shared TX vectors map SocketCAN flags and vendor FrameType", "[frame]") {
    for (std::size_t i = 0; i < kFrameTxCaseCount; ++i) {
        const auto &row = kFrameTxCases[i];
        INFO(row.name);
        Message msg = make_tx_message(row);
        if (!row.expect_ok) {
            REQUIRE_THROWS_AS(resolve_tx_flags(row.link, msg), ValidationError);
            continue;
        }
        auto flags = resolve_tx_flags(row.link, msg);
        REQUIRE(flags.is_fd == row.expect_is_fd);
        REQUIRE(flags.is_brs == row.expect_is_brs);
        REQUIRE(wire_frame_type(row.link, msg) == row.expect_frame_type);
        REQUIRE(encode_frame_type(flags.is_fd, flags.is_brs) == row.expect_frame_type);
    }
}

TEST_CASE("shared RX vectors restore is_fd and is_brs", "[frame]") {
    for (std::size_t i = 0; i < kFrameRxCaseCount; ++i) {
        const auto &row = kFrameRxCases[i];
        INFO(row.name);
        if (!row.expect_ok) {
            REQUIRE_THROWS_AS(decode_frame_type(row.frame_type), ValidationError);
            continue;
        }
        auto flags = decode_frame_type(row.frame_type);
        REQUIRE(flags.is_fd == row.expect_is_fd);
        REQUIRE(flags.is_brs == row.expect_is_brs);
    }
}

TEST_CASE("arbitration ID range is checked", "[frame]") {
    REQUIRE_NOTHROW(validate_arbitration_id(kStandardIdMask, false));
    REQUIRE_THROWS_AS(validate_arbitration_id(kStandardIdMask + 1, false), ValidationError);
    REQUIRE_NOTHROW(validate_arbitration_id(kExtendedIdMask, true));
    REQUIRE_THROWS_AS(validate_arbitration_id(kExtendedIdMask + 1, true), ValidationError);

    Message classic_too_wide;
    classic_too_wide.id = 0x800;
    classic_too_wide.dlc = 1;
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kCanFd, classic_too_wide), ValidationError);

    Message extended_too_wide;
    extended_too_wide.id = 0x20000000u;
    extended_too_wide.is_extended = true;
    extended_too_wide.dlc = 1;
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kCanFd, extended_too_wide), ValidationError);
}

TEST_CASE("classic link rejects FD payload length", "[frame]") {
    Message msg;
    msg.dlc = 9;
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kClassic, msg), ValidationError);
    REQUIRE_NOTHROW(resolve_tx_flags(LinkKind::kCanFd, msg));
}

#if defined(__linux__)
TEST_CASE("SocketCAN encoding executes the shared TX vectors", "[frame][socketcan]") {
    for (std::size_t i = 0; i < kFrameTxCaseCount; ++i) {
        const auto &row = kFrameTxCases[i];
        INFO(row.name);
        const Message message = make_tx_message(row);
        if (!row.expect_ok) {
            REQUIRE_THROWS_AS(linkerbot::can::encode_socketcan_frame(row.link, message),
                              ValidationError);
            continue;
        }

        const SocketCANTxFrame encoded = linkerbot::can::encode_socketcan_frame(row.link, message);
        REQUIRE(encoded.is_fd == row.expect_is_fd);
        if (encoded.is_fd) {
            REQUIRE(encoded.size() == sizeof(struct canfd_frame));
            REQUIRE(((encoded.fd_frame.flags & CANFD_BRS) != 0) == row.expect_is_brs);
            REQUIRE(encoded.fd_frame.len == message.dlc);
        } else {
            REQUIRE(encoded.size() == sizeof(struct can_frame));
            REQUIRE(encoded.classic_frame.can_dlc == message.dlc);
        }
    }
}

TEST_CASE("SocketCAN decoding restores classic FD and BRS flags", "[frame][socketcan]") {
    struct can_frame classic {};
    classic.can_id = 0x123;
    classic.can_dlc = 1;
    classic.data[0] = 0x11;
    const auto classic_message = linkerbot::can::decode_socketcan_frame(classic);
    REQUIRE_FALSE(classic_message.is_fd);
    REQUIRE_FALSE(classic_message.is_brs);

    struct canfd_frame fd {};
    fd.can_id = CAN_EFF_FLAG | 0x12345;
    fd.len = 12;
    fd.flags = CANFD_BRS;
    fd.data[0] = 0x22;
    const auto fd_message = linkerbot::can::decode_socketcan_frame(fd);
    REQUIRE(fd_message.is_fd);
    REQUIRE(fd_message.is_brs);
    REQUIRE(fd_message.is_extended);
    REQUIRE(fd_message.id == 0x12345);
    REQUIRE(fd_message.dlc == 12);
}
#endif

TEST_CASE("Explicit frame formats preserve backend defaults and reject conflicts", "[frame]") {
    using linkerbot::can::FrameFormat;
    Message message;
    linkerbot::can::FrameFlags (*legacy_resolver)(LinkKind, const Message &) = &resolve_tx_flags;
    REQUIRE(legacy_resolver(LinkKind::kCanFd, message).is_fd);
    REQUIRE(resolve_tx_flags(LinkKind::kCanFd, message).is_fd);
    REQUIRE_FALSE(
        linkerbot::can::resolve_tx_flags_with_default(LinkKind::kCanFd, message, false).is_fd);
    message.format = FrameFormat::kClassic;
    REQUIRE_FALSE(resolve_tx_flags(LinkKind::kCanFd, message).is_fd);
    REQUIRE_FALSE(resolve_tx_flags(LinkKind::kClassic, message).is_fd);
    message.dlc = 9;
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kCanFd, message), ValidationError);
    message.dlc = 8;
    message.is_fd = true;
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kCanFd, message), ValidationError);
    message.is_brs = true;
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kCanFd, message), ValidationError);
    message.format = FrameFormat::kFd;
    REQUIRE(linkerbot::can::resolve_tx_flags_with_default(LinkKind::kCanFd, message, false).is_brs);
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kClassic, message), ValidationError);
    message.is_fd = false;
    message.is_brs = false;
    REQUIRE(linkerbot::can::resolve_tx_flags_with_default(LinkKind::kCanFd, message, false).is_fd);
    // Deliberately invalid input for the rejection test.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    message.format = static_cast<FrameFormat>(-1);
    REQUIRE_THROWS_AS(resolve_tx_flags(LinkKind::kCanFd, message), ValidationError);
}
