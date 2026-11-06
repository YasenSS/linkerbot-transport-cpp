#pragma once

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/can/socketcan_frame.hpp>
#include <linkerbot/can/transport.hpp>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/net_tstamp.h>

namespace linkerbot::can {

    // Request a 10 ms socket receive timeout so the dispatcher can check its
    // stop flag between reads. This is not a hard driver or shutdown deadline.
    inline constexpr long kSocketCANReceiveTimeoutUs = 10000;
    // Request a 50 ms socket send timeout to limit ordinary TX-queue waits.
    // close() still waits for in-flight calls; the timeout does not guarantee
    // driver completion or imply that a failed write sent no frame.
    inline constexpr long kSocketCANSendTimeoutUs = 50000;

    enum class SocketCANTimestampMode : int { kDisabled, kSoftware, kHardware };
    struct SocketCANOptions {
        bool fd_mode = false;
        bool receive_own_messages = false;
        SocketCANTimestampMode timestamp_mode = SocketCANTimestampMode::kDisabled;
    };

    // Linux SocketCAN backend (classic CAN and CAN FD).
    class SocketCANTransport : public CANTransport {
      public:
        // Test-only seam: adopt an already-open, already-configured fd (for
        // example one end of a UNIX socketpair) instead of opening a real
        // CAN socket. Lets tests exercise close()/read()/write() ownership
        // and in-flight I/O coordination without a physical CAN interface.
        // Takes ownership of fd; SO_RCVTIMEO/SO_SNDTIMEO are applied same as
        // the production constructor (both are generic socket options, not
        // CAN-specific, so they also apply to a socketpair fd). Optional timestamp
        // mode is applied; receive-own opt-in requires an actual CAN socket.
        // The existing false/disabled defaults leave those options unchanged.
        struct AdoptFd {
            int fd = -1;
            bool fd_mode = false;
            std::function<void()> on_io_in_flight;
            SocketCANTimestampMode timestamp_mode = SocketCANTimestampMode::kDisabled;
            bool receive_own_messages = false;
        };

        // fd_mode=true enables CAN_RAW_FD_FRAMES (CAN FD).
        explicit SocketCANTransport(std::string_view interface_name, bool fd_mode = false)
            : SocketCANTransport(
                  SocketCANOptions{fd_mode, false, SocketCANTimestampMode::kDisabled},
                  interface_name) {}

        SocketCANTransport(const SocketCANOptions &options, std::string_view interface_name)
            : fd_mode_(options.fd_mode) {
            validate_timestamp_mode(options.timestamp_mode);
            if (interface_name.empty() || interface_name.size() >= IFNAMSIZ) {
                throw ValidationError("SocketCAN interface name is empty or too long");
            }
            const int opened_fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
            if (opened_fd < 0) {
                throw TransportIOError("Failed to create CAN socket",
                                       std::error_code(errno, std::generic_category()));
            }

            try {
                if (options.receive_own_messages) {
                    int enable = 1;
                    if (::setsockopt(opened_fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &enable,
                                     sizeof(enable)) < 0) {
                        throw TransportIOError("Failed to enable CAN receive-own messages",
                                               std::error_code(errno, std::generic_category()));
                    }
                }
                apply_timestamp_mode(opened_fd, options.timestamp_mode);
            } catch (...) {
                (void)::close(opened_fd);
                throw;
            }

            if (fd_mode_) {
                int enable = 1;
                if (::setsockopt(opened_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable,
                                 sizeof(enable)) < 0) {
                    const int saved_errno = errno;
                    ::close(opened_fd);
                    throw TransportIOError("Failed to enable CAN FD frames",
                                           std::error_code(saved_errno, std::generic_category()));
                }
            }

            struct ifreq ifr {};
            auto len = std::min(interface_name.size(), static_cast<size_t>(IFNAMSIZ - 1));
            std::memcpy(ifr.ifr_name, interface_name.data(), len);
            ifr.ifr_name[len] = '\0';
            if (::ioctl(opened_fd, SIOCGIFINDEX, &ifr) < 0) {
                const int saved_errno = errno;
                ::close(opened_fd);
                throw TransportIOError(std::string("Failed to get interface index for ") +
                                           std::string(interface_name),
                                       std::error_code(saved_errno, std::generic_category()));
            }

            struct sockaddr_can addr {};
            addr.can_family = AF_CAN;
            addr.can_ifindex = ifr.ifr_ifindex;

            if (::bind(opened_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
                const int saved_errno = errno;
                ::close(opened_fd);
                throw TransportIOError(std::string("Failed to bind CAN socket to ") +
                                           std::string(interface_name),
                                       std::error_code(saved_errno, std::generic_category()));
            }

            apply_timeouts(opened_fd);
            fd_ = opened_fd;
        }

        // See AdoptFd above: test-only seam, not for production interfaces.
        explicit SocketCANTransport(AdoptFd handle)
            : fd_mode_(handle.fd_mode), on_io_in_flight_(std::move(handle.on_io_in_flight)) {
            if (handle.fd < 0) {
                throw ValidationError("AdoptFd requires an already-open fd");
            }
            try {
                validate_timestamp_mode(handle.timestamp_mode);
                if (handle.receive_own_messages) {
                    int enable = 1;
                    if (::setsockopt(handle.fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &enable,
                                     sizeof(enable)) < 0) {
                        throw TransportIOError("Failed to enable CAN receive-own messages",
                                               std::error_code(errno, std::generic_category()));
                    }
                }
                apply_timestamp_mode(handle.fd, handle.timestamp_mode);
            } catch (...) {
                (void)::close(handle.fd);
                throw;
            }
            apply_timeouts(handle.fd);
            fd_ = handle.fd;
        }

        ~SocketCANTransport() override {
            try {
                SocketCANTransport::close();
            } catch (...) { // NOLINT(bugprone-empty-catch) destructor must not throw
            }
        }

        SocketCANTransport(const SocketCANTransport &) = delete;
        SocketCANTransport &operator=(const SocketCANTransport &) = delete;

        // Moving is only well-defined when no read()/write() is concurrently
        // in flight on `other` (same precondition the previous atomic-swap
        // implementation had); it is intended for transfer-of-ownership at
        // construction time, before a dispatcher thread is started.
        // These assignments must occur after locking `other`; member initializers would race.
        // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
        SocketCANTransport(SocketCANTransport &&other) noexcept {
            const std::scoped_lock lock(other.io_mutex_);
            fd_ = other.fd_;
            fd_mode_ = other.fd_mode_;
            closed_ = other.closed_;
            close_complete_ = other.close_complete_;
            on_io_in_flight_ = std::move(other.on_io_in_flight_);
            other.fd_ = -1;
            other.closed_ = true;
        }
        // NOLINTEND(cppcoreguidelines-prefer-member-initializer)

        SocketCANTransport &operator=(SocketCANTransport &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            int old_fd = -1;
            {
                const std::scoped_lock lock(io_mutex_, other.io_mutex_);
                old_fd = fd_;
                fd_ = other.fd_;
                fd_mode_ = other.fd_mode_;
                closed_ = other.closed_;
                close_complete_ = other.close_complete_;
                on_io_in_flight_ = std::move(other.on_io_in_flight_);
                other.fd_ = -1;
                other.closed_ = true;
            }
            if (old_fd >= 0) {
                (void)::close(old_fd);
            }
            return *this;
        }

        auto read() -> std::optional<Message> override {
            if (fd_mode_) {
                return read_fd();
            }
            const IoInFlightGuard guard(*this);
            struct can_frame frame {};
            std::optional<ReceiveTimestamp> timestamp;
            bool is_rx = true;
            auto nbytes = receive_frame(guard.fd(), &frame, sizeof(frame), timestamp, is_rx);
            if (nbytes != static_cast<ssize_t>(sizeof(frame))) {
                if (nbytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return std::nullopt;
                }
                if (nbytes < 0) {
                    const int saved_errno = errno;
                    throw TransportIOError(std::string("CAN read failed: ") +
                                               std::strerror(saved_errno),
                                           std::error_code(saved_errno, std::generic_category()));
                }
                throw TransportIOError("SocketCAN read returned an unexpected frame size", {},
                                       static_cast<int>(nbytes));
            }

            // Error/remote frames are link status, not application data
            // (R18): filter them out instead of decoding garbage/zeroed
            // payload as a normal Message. The caller's next read() picks up
            // whatever comes after.
            if (is_can_error_frame(frame.can_id) || is_can_remote_frame(frame.can_id)) {
                return std::nullopt;
            }

            auto message = decode_socketcan_frame(frame);
            message.timestamp = timestamp;
            message.is_rx = is_rx;
            return message;
        }

        auto write(const Message &message) -> void override {
            const IoInFlightGuard guard(*this);
            const auto encoded =
                encode_socketcan_frame(fd_mode_ ? LinkKind::kCanFd : LinkKind::kClassic, message);
            const auto nbytes = ::write(guard.fd(), encoded.data(), encoded.size());
            if (nbytes != static_cast<ssize_t>(encoded.size())) {
                const auto system_error = nbytes < 0
                                              ? std::error_code(errno, std::generic_category())
                                              : std::error_code{};
                throw TransportIOError("SocketCAN write did not transmit one complete frame",
                                       system_error);
            }
        }

        auto close() -> void override {
            std::unique_lock lock(io_mutex_);
            // Reject new I/O immediately and unconditionally, even while
            // another close() call is still in progress.
            closed_ = true;
            for (;;) {
                if (close_complete_) {
                    return;
                }
                if (closing_in_progress_) {
                    // A concurrent close() call must wait for the in-progress
                    // attempt to actually finish releasing the fd — closed_
                    // being true is not enough, or a second caller could
                    // report success while the fd is still open and in use.
                    io_cv_.wait(lock, [this] { return close_complete_; });
                    return;
                }

                closing_in_progress_ = true;
                // Wait for any read()/write() that already captured the fd to
                // finish. Only after that do we actually release the fd
                // number, so it can never be reused by unrelated code while a
                // syscall might still reference it (R18). The mutex is only
                // held for this bookkeeping, never across the blocking
                // recvmsg()/write() syscalls themselves, so a slow write cannot
                // starve RX (or vice versa).
                io_cv_.wait(lock, [this] { return in_flight_ == 0; });
                const int fd = fd_;
                fd_ = -1;
                lock.unlock();

                // POSIX close() releases the fd number back to the kernel
                // even when it reports an error, so — unlike the vendor CAN
                // FD backend's CAN_CloseDevice — retrying ::close() on the
                // same number afterwards would risk operating on an
                // unrelated, meanwhile-reused descriptor. There is therefore
                // exactly one real close() syscall attempt ever made per
                // transport; a failure is surfaced once (to whichever caller
                // happened to perform it) and every caller — this one and any
                // that were waiting — is done afterwards regardless.
                std::exception_ptr failure;
                if (fd >= 0 && ::close(fd) < 0) {
                    const int saved_errno = errno;
                    failure = std::make_exception_ptr(
                        TransportIOError("Failed to close SocketCAN socket",
                                         std::error_code(saved_errno, std::generic_category())));
                }

                lock.lock();
                closing_in_progress_ = false;
                close_complete_ = true;
                lock.unlock();
                io_cv_.notify_all();

                if (failure) {
                    std::rethrow_exception(failure);
                }
                return;
            }
        }

      private:
        // Bookkeeping-only RAII guard: reserves the fd for the duration of
        // one recvmsg()/write() syscall so close() cannot free (and let
        // something else reuse) the fd number while that syscall is still
        // outstanding. Never held across the blocking syscall itself.
        class IoInFlightGuard {
          public:
            explicit IoInFlightGuard(SocketCANTransport &owner) : owner_(owner) {
                {
                    const std::scoped_lock lock(owner_.io_mutex_);
                    if (owner_.closed_ || owner_.fd_ < 0) {
                        throw ClosedError("SocketCAN transport is closed");
                    }
                    ++owner_.in_flight_;
                    fd_ = owner_.fd_;
                }
                try {
                    if (owner_.on_io_in_flight_) {
                        owner_.on_io_in_flight_();
                    }
                } catch (...) {
                    const std::scoped_lock lock(owner_.io_mutex_);
                    if (--owner_.in_flight_ == 0) {
                        owner_.io_cv_.notify_all();
                    }
                    throw;
                }
            }

            ~IoInFlightGuard() {
                const std::scoped_lock lock(owner_.io_mutex_);
                if (--owner_.in_flight_ == 0) {
                    owner_.io_cv_.notify_all();
                }
            }

            IoInFlightGuard(const IoInFlightGuard &) = delete;
            auto operator=(const IoInFlightGuard &) -> IoInFlightGuard & = delete;

            [[nodiscard]] auto fd() const noexcept -> int {
                return fd_;
            }

          private:
            SocketCANTransport &owner_;
            int fd_ = -1;
        };

        static auto validate_timestamp_mode(SocketCANTimestampMode mode) -> void {
            if (mode != SocketCANTimestampMode::kDisabled &&
                mode != SocketCANTimestampMode::kSoftware &&
                mode != SocketCANTimestampMode::kHardware) {
                throw ValidationError("Unknown SocketCAN timestamp mode");
            }
        }

        static auto apply_timestamp_mode(int fd, SocketCANTimestampMode mode) -> void {
            if (mode == SocketCANTimestampMode::kDisabled) {
                return;
            }
            const int flags = mode == SocketCANTimestampMode::kHardware
                                  ? SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE
                                  : SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
            if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
                throw TransportIOError("Failed to configure SocketCAN timestamps",
                                       std::error_code(errno, std::generic_category()));
            }
        }

        static auto receive_frame(int fd, void *frame, size_t length,
                                  std::optional<ReceiveTimestamp> &timestamp,
                                  bool &is_rx) -> ssize_t {
            alignas(struct cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(timespec) * 3)>
                control{};
            struct iovec vector {
                frame, length
            };
            struct msghdr header {};
            header.msg_iov = &vector;
            header.msg_iovlen = 1;
            header.msg_control = control.data();
            header.msg_controllen = control.size();
            const auto received = ::recvmsg(fd, &header, 0);
            if (received >= 0) {
                if ((header.msg_flags & MSG_TRUNC) != 0) {
                    throw TransportIOError("SocketCAN receive frame was truncated");
                }
                timestamp = decode_socketcan_timestamp(header);
                is_rx = (header.msg_flags & MSG_CONFIRM) == 0;
            }
            return received;
        }

        static auto apply_timeouts(int fd) -> void {
            struct timeval rcv_tv {};
            rcv_tv.tv_sec = 0;
            rcv_tv.tv_usec = kSocketCANReceiveTimeoutUs;
            if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv)) < 0) {
                const int saved_errno = errno;
                ::close(fd);
                throw TransportIOError("Failed to set SO_RCVTIMEO on CAN socket",
                                       std::error_code(saved_errno, std::generic_category()));
            }

            struct timeval snd_tv {};
            snd_tv.tv_sec = 0;
            snd_tv.tv_usec = kSocketCANSendTimeoutUs;
            if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv)) < 0) {
                const int saved_errno = errno;
                ::close(fd);
                throw TransportIOError("Failed to set SO_SNDTIMEO on CAN socket",
                                       std::error_code(saved_errno, std::generic_category()));
            }
        }

        auto read_fd() -> std::optional<Message> {
            const IoInFlightGuard guard(*this);
            struct canfd_frame frame {};
            std::optional<ReceiveTimestamp> timestamp;
            bool is_rx = true;
            auto nbytes = receive_frame(guard.fd(), &frame, sizeof(frame), timestamp, is_rx);
            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return std::nullopt;
                }
                const int saved_errno = errno;
                throw TransportIOError(std::string("CAN FD read failed: ") +
                                           std::strerror(saved_errno),
                                       std::error_code(saved_errno, std::generic_category()));
            }
            if (nbytes == 0) {
                return std::nullopt;
            }

            if (nbytes == static_cast<ssize_t>(sizeof(struct can_frame))) {
                struct can_frame classic {};
                std::memcpy(&classic, &frame, sizeof(classic));
                if (is_can_error_frame(classic.can_id) || is_can_remote_frame(classic.can_id)) {
                    return std::nullopt;
                }
                auto message = decode_socketcan_frame(classic);
                message.timestamp = timestamp;
                message.is_rx = is_rx;
                return message;
            }
            if (nbytes == static_cast<ssize_t>(sizeof(struct canfd_frame))) {
                if (is_can_error_frame(frame.can_id) || is_can_remote_frame(frame.can_id)) {
                    return std::nullopt;
                }
                auto message = decode_socketcan_frame(frame);
                message.timestamp = timestamp;
                message.is_rx = is_rx;
                return message;
            }
            throw TransportIOError("SocketCAN read returned an unexpected frame size", {},
                                   static_cast<int>(nbytes));
        }

        mutable std::mutex io_mutex_;
        mutable std::condition_variable io_cv_;
        int fd_ = -1;         // guarded by io_mutex_; never touched during a blocking syscall
        int in_flight_ = 0;   // guarded by io_mutex_
        bool closed_ = false; // guarded by io_mutex_; rejects new I/O as soon as close() starts
        // closing_in_progress_/close_complete_ (both guarded by io_mutex_) form
        // the actual close() completion barrier: exactly one caller performs
        // the real ::close() syscall, and every other concurrent caller waits
        // for close_complete_ (not just closed_) before returning, so none of
        // them can report success while the fd is still open.
        bool closing_in_progress_ = false;
        bool close_complete_ = false;
        bool fd_mode_ = false;
        std::function<void()> on_io_in_flight_;
    };

} // namespace linkerbot::can
