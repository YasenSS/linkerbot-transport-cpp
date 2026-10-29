#pragma once

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/message.hpp>

#include <cstdint>

namespace linkerbot::can {

    // Vendor FrameType bits (libcanbus / HCanbus), shared with SocketCAN mapping.
    // bit 2 = CAN FD, bit 3 = bit-rate switching (BRS).
    inline constexpr uint8_t kFrameTypeFd = 0x04;
    inline constexpr uint8_t kFrameTypeBrs = 0x08;

    inline constexpr uint32_t kStandardIdMask = 0x7FFu;
    inline constexpr uint32_t kExtendedIdMask = 0x1FFFFFFFu;

    // Physical link capability. Classic SocketCAN cannot emit FD/BRS frames.
    // Vendor CAN FD adapters and SocketCAN with fd_mode=true are CanFd links.
    enum class LinkKind {
        kClassic,
        kCanFd,
    };

    struct FrameFlags {
        bool is_fd = false;
        bool is_brs = false;
    };

    // P0.1 public FD/BRS rules:
    //
    // - Message::is_fd / is_brs are the per-frame source of truth.
    // - BRS is opt-in: only is_brs=true enables it. Backends never upgrade.
    // - is_brs && !is_fd is illegal on every backend (rejected, not normalized).
    // - bool cannot express "unspecified". Untagged (false, false) is resolved by
    //   link kind: CanFd => FD without BRS (FrameType 0x04); Classic => classic CAN.
    // - Config::frame_type is not applied to TX frames and is not a CANFD_Init
    //   field. Session-wide BRS belongs on Message (or a later SDK stamp).
    // - SocketCAN and vendor encode the same resolved flags: is_fd => FD frame /
    //   FrameType bit 0x04; is_brs => CANFD_BRS / FrameType bit 0x08.

    inline auto validate_frame_flags(bool is_fd, bool is_brs) -> void {
        if (is_brs && !is_fd) {
            throw ValidationError("BRS requires CAN FD");
        }
    }

    inline auto validate_arbitration_id(uint32_t id, bool is_extended) -> void {
        const uint32_t max_id = is_extended ? kExtendedIdMask : kStandardIdMask;
        if (id > max_id) {
            throw ValidationError(is_extended ? "extended arbitration ID exceeds 29 bits"
                                              : "standard arbitration ID exceeds 11 bits");
        }
    }

    inline auto validate_payload_length(uint8_t dlc, LinkKind link) -> void {
        if (dlc > kMaxDataLen) {
            throw ValidationError("payload length exceeds 64 bytes");
        }
        if (link == LinkKind::kClassic && dlc > 8) {
            throw ValidationError("classic CAN payload exceeds 8 bytes");
        }
    }

    [[nodiscard]] inline auto encode_frame_type(bool is_fd, bool is_brs) -> uint8_t {
        validate_frame_flags(is_fd, is_brs);
        uint8_t frame_type = 0;
        if (is_fd) {
            frame_type |= kFrameTypeFd;
        }
        if (is_brs) {
            frame_type |= kFrameTypeBrs;
        }
        return frame_type;
    }

    [[nodiscard]] inline auto decode_frame_type(uint8_t frame_type) -> FrameFlags {
        FrameFlags flags;
        flags.is_fd = (frame_type & kFrameTypeFd) != 0;
        flags.is_brs = (frame_type & kFrameTypeBrs) != 0;
        validate_frame_flags(flags.is_fd, flags.is_brs);
        return flags;
    }

    [[nodiscard]] inline auto resolve_tx_flags(LinkKind link, const Message &message)
        -> FrameFlags {
        validate_frame_flags(message.is_fd, message.is_brs);
        validate_arbitration_id(message.id, message.is_extended);
        validate_payload_length(message.dlc, link);

        if (link == LinkKind::kClassic) {
            if (message.is_fd || message.is_brs) {
                throw ValidationError("CAN FD frame cannot be sent on a classic link");
            }
            return FrameFlags{};
        }

        // CanFd link: untagged (false, false) means FD without BRS.
        if (message.is_fd || message.is_brs) {
            return FrameFlags{true, message.is_brs};
        }
        return FrameFlags{true, false};
    }

    [[nodiscard]] inline auto wire_frame_type(LinkKind link, const Message &message) -> uint8_t {
        const auto flags = resolve_tx_flags(link, message);
        return encode_frame_type(flags.is_fd, flags.is_brs);
    }

} // namespace linkerbot::can
