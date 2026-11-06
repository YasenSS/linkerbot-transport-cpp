#pragma once

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/message.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/socket.h>

#include <linux/can.h>

namespace linkerbot::can {

    // Decode only kernel-provided metadata. Never substitute a host read time.
    [[nodiscard]] inline auto
    decode_socketcan_timestamp(struct msghdr header) -> std::optional<ReceiveTimestamp> {
        if ((header.msg_flags & MSG_CTRUNC) != 0) {
            return std::nullopt;
        }
        for (auto *control = CMSG_FIRSTHDR(&header); control != nullptr;
             control = CMSG_NXTHDR(&header, control)) {
            const auto offset =
                static_cast<size_t>(reinterpret_cast<const unsigned char *>(control) -
                                    static_cast<const unsigned char *>(header.msg_control));
            if (control->cmsg_len > header.msg_controllen - offset) {
                return std::nullopt;
            }
            if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SO_TIMESTAMPING ||
                control->cmsg_len < CMSG_LEN(sizeof(timespec) * 3)) {
                continue;
            }
            std::array<timespec, 3> stamps{};
            std::memcpy(stamps.data(), CMSG_DATA(control), sizeof(stamps));
            const bool hardware = stamps[2].tv_sec != 0 || stamps[2].tv_nsec != 0;
            const auto &stamp = stamps[hardware ? 2 : 0];
            if (stamp.tv_sec < 0 || stamp.tv_nsec < 0 || stamp.tv_nsec >= 1'000'000'000 ||
                (stamp.tv_sec == 0 && stamp.tv_nsec == 0)) {
                return std::nullopt;
            }
            const auto seconds = static_cast<uint64_t>(stamp.tv_sec);
            const auto nanos = static_cast<uint64_t>(stamp.tv_nsec);
            if (seconds > (std::numeric_limits<uint64_t>::max() - nanos) / 1'000'000'000) {
                return std::nullopt;
            }
            return ReceiveTimestamp{
                seconds * 1'000'000'000 + nanos, TimestampUnit::kNanoseconds,
                hardware ? TimestampClock::kDevice : TimestampClock::kSystemRealtime,
                hardware ? TimestampSource::kHardware : TimestampSource::kSoftware, 64};
        }
        return std::nullopt;
    }

    struct SocketCANTxFrame {
        bool is_fd = false;
        struct can_frame classic_frame {};
        struct canfd_frame fd_frame {};

        [[nodiscard]] auto data() const noexcept -> const void * {
            return is_fd ? static_cast<const void *>(&fd_frame)
                         : static_cast<const void *>(&classic_frame);
        }

        [[nodiscard]] auto size() const noexcept -> size_t {
            return is_fd ? sizeof(fd_frame) : sizeof(classic_frame);
        }
    };

    [[nodiscard]] inline auto encode_socketcan_frame(LinkKind link,
                                                     const Message &message) -> SocketCANTxFrame {
        const auto flags = resolve_tx_flags(link, message);
        SocketCANTxFrame encoded;
        encoded.is_fd = flags.is_fd;

        if (flags.is_fd) {
            encoded.fd_frame.can_id = message.id;
            if (message.is_extended) {
                encoded.fd_frame.can_id |= CAN_EFF_FLAG;
            }
            encoded.fd_frame.len = message.dlc;
            if (flags.is_brs) {
                encoded.fd_frame.flags |= CANFD_BRS;
            }
            std::memcpy(encoded.fd_frame.data, message.data.data(), message.dlc);
            return encoded;
        }

        encoded.classic_frame.can_id = message.id;
        if (message.is_extended) {
            encoded.classic_frame.can_id |= CAN_EFF_FLAG;
        }
        encoded.classic_frame.can_dlc = message.dlc;
        std::memcpy(encoded.classic_frame.data, message.data.data(), message.dlc);
        return encoded;
    }

    // Link-state frames the kernel can hand back on a raw CAN socket that are
    // not application data (R18): CAN_ERR_FLAG marks an error message frame
    // (bus-off, ACK error, ...) and CAN_RTR_FLAG marks a remote transmission
    // request (its can_dlc is the *requested* length, not received payload).
    // Callers must check these before decode_socketcan_frame() so link status
    // is never silently presented as a normal Message.
    [[nodiscard]] inline auto is_can_error_frame(canid_t can_id) noexcept -> bool {
        return (can_id & CAN_ERR_FLAG) != 0;
    }

    [[nodiscard]] inline auto is_can_remote_frame(canid_t can_id) noexcept -> bool {
        return (can_id & CAN_RTR_FLAG) != 0;
    }

    [[nodiscard]] inline auto decode_socketcan_frame(const struct can_frame &frame) -> Message {
        Message message;
        if ((frame.can_id & CAN_EFF_FLAG) != 0) {
            message.id = frame.can_id & CAN_EFF_MASK;
            message.is_extended = true;
        } else {
            message.id = frame.can_id & CAN_SFF_MASK;
        }
        message.dlc = std::min(frame.can_dlc, static_cast<__u8>(8));
        std::memcpy(message.data.data(), frame.data, message.dlc);
        return message;
    }

    [[nodiscard]] inline auto decode_socketcan_frame(const struct canfd_frame &frame) -> Message {
        Message message;
        if ((frame.can_id & CAN_EFF_FLAG) != 0) {
            message.id = frame.can_id & CAN_EFF_MASK;
            message.is_extended = true;
        } else {
            message.id = frame.can_id & CAN_SFF_MASK;
        }
        message.is_fd = true;
        message.is_brs = (frame.flags & CANFD_BRS) != 0;
        validate_frame_flags(message.is_fd, message.is_brs);
        if (frame.len > kMaxDataLen) {
            throw ValidationError("SocketCAN FD payload length exceeds 64 bytes");
        }
        message.dlc = frame.len;
        std::memcpy(message.data.data(), frame.data, message.dlc);
        return message;
    }

} // namespace linkerbot::can
