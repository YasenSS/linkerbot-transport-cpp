#ifdef __linux__
#include <catch2/catch_test_macros.hpp>
#include <dlfcn.h>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <linkerbot/can/pcan_transport.hpp>
#include <linkerbot/can/slcan_transport.hpp>
#include <linkerbot/can/socketcan_transport.hpp>
#include <linkerbot/can/vendor_canfd_transport.hpp>
#include <linkerbot/serial/byte_stream.hpp>
#include <linkerbot/transport/session.hpp>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <asm/termbits.h>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace std::chrono_literals;
namespace {
    enum class Fault {
        kNone,
        kOpen,
        kEvent,
        kLock,
        kGet,
        kExclusive,
        kSet,
        kFlush,
        kRs485,
        kSanitize,
        kPollError,
        kPollInterrupt,
        kPollInvalid,
        kPollHangup,
        kReadError,
        kReadZero,
        kReadRetry,
        kWriteError,
        kWriteZero,
        kWriteRetry,
        kOutputError,
        kOutputPending,
        kClose,
        kModule,
        kSearch,
        kModulePath,
        kStat,
        kSendTimeout,
        kPollEmpty,
        kReadLateRetry,
        kWriteBlocked,
        kPollTimeout,
        kCloseWake,
        kReadCancel,
        kWriteCancel,
        kCanSocket,
        kCanEchoOption,
        kCanFdOption,
        kCanTimestampOption,
        kCanBindAll
    };
    // Faults are thread-local and armed only around the operation under test.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    thread_local linkerbot::can::SlcanSerialPort *cancel_on_io = nullptr;
    thread_local unsigned close_attempt = 0;
    thread_local unsigned search_attempt = 0;
    thread_local Fault fault = Fault::kNone;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
    bool take(Fault expected) {
        if (fault != expected) {
            return false;
        }
        fault = Fault::kNone;
        errno = EIO;
        return true;
    }
    struct Arm {
        explicit Arm(Fault value) {
            fault = value;
        }
        ~Arm() {
            fault = Fault::kNone;
        }
    };
    struct Pty {
        int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        std::string path;
        Pty() {
            if (master < 0 || (::grantpt(master) != 0) || (::unlockpt(master) != 0)) {
                throw std::runtime_error("PTY setup failed");
            }
            char name[128]{};
            if (::ptsname_r(master, name, sizeof(name)) != 0) {
                throw std::runtime_error("PTY name failed");
            }
            path = name;
        }
        ~Pty() {
            (void)::close(master);
        }
        Pty(const Pty &) = delete;
        Pty &operator=(const Pty &) = delete;
    };
} // namespace
// GNU ld --wrap requires these exact reserved symbol names. Keep the exception
// scoped to the linker ABI declarations and definitions, not the test code.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" {
void *__real_dlopen(const char *, int);
int __real_dladdr(const void *, Dl_info *);
int __real_stat(const char *, struct stat *);
int __real_setsockopt(int, int, int, const void *, socklen_t);
void *__wrap_dlopen(const char *path, int flags) {
    if (fault == Fault::kModule) {
        return nullptr;
    }
    if (fault == Fault::kSearch) {
        if (++search_attempt < 3) {
            return nullptr;
        }
        return __real_dlopen(std::getenv("LINKERBOT_TEST_VENDOR_LIBRARY"), flags);
    }
    return __real_dlopen(path, flags);
}
int __wrap_dladdr(const void *address, Dl_info *info) {
    return take(Fault::kModulePath) ? 0 : __real_dladdr(address, info);
}
int __wrap_stat(const char *path, struct stat *info) {
    return take(Fault::kStat) ? -1 : __real_stat(path, info);
}
#ifdef _STAT_VER
// glibc before 2.33 inlines stat() as __xstat(_STAT_VER, ...). Wrapping
// stat alone leaves the production call untouched on Focal/GCC 9.
int __real___xstat(int, const char *, struct stat *);
int __wrap___xstat(int version, const char *path, struct stat *info) {
    return take(Fault::kStat) ? -1 : __real___xstat(version, path, info);
}
#endif
int __real_socket(int, int, int);
int __real_bind(int, const struct sockaddr *, socklen_t);
int __wrap_socket(int domain, int type, int protocol) {
    if (domain == PF_CAN && take(Fault::kCanSocket)) {
        return -1;
    }
    return __real_socket(domain, type, protocol);
}
int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length) {
    if (address->sa_family == AF_CAN && take(Fault::kCanBindAll)) {
        // Exercise constructor success without configuring a physical interface.
        auto target = *reinterpret_cast<const sockaddr_can *>(address);
        target.can_ifindex = 0;
        return __real_bind(fd, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
    }
    return __real_bind(fd, address, length);
}
int __wrap_setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    if ((level == SOL_CAN_RAW && option == CAN_RAW_RECV_OWN_MSGS && take(Fault::kCanEchoOption)) ||
        (level == SOL_CAN_RAW && option == CAN_RAW_FD_FRAMES && take(Fault::kCanFdOption)) ||
        (level == SOL_SOCKET && option == SO_TIMESTAMPING && take(Fault::kCanTimestampOption))) {
        return -1;
    }
    if (option == SO_SNDTIMEO && take(Fault::kSendTimeout)) {
        return -1;
    }
    return __real_setsockopt(fd, level, option, value, size);
}

int __real_open(const char *, int, ...);
int __real_eventfd(unsigned int, int);
int __real_flock(int, int);
int __real_ioctl(int, unsigned long, ...);
int __real_poll(pollfd *, nfds_t, int);
ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t);
int __real_close(int);
int __wrap_open(const char *path, int flags, ...) {
    if (take(Fault::kOpen)) {
        return -1;
    }
    // All production serial opens omit O_CREAT. Preserve the mode for any
    // test/runtime caller that does create a file.
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        const auto mode = va_arg(args, mode_t);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}
int __wrap_eventfd(unsigned int value, int flags) {
    return take(Fault::kEvent) ? -1 : __real_eventfd(value, flags);
}
int __wrap_flock(int fd, int op) {
    return take(Fault::kLock) ? -1 : __real_flock(fd, op);
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    // ioctl arguments are request-specific: no vararg access for TIOCEXCL,
    // an int for TCFLSH, and the exact pointer type for the remaining requests.
    if (request == TIOCEXCL) {
        return take(Fault::kExclusive) ? -1 : __real_ioctl(fd, request);
    }
    if (request == TIOCNXCL) {
        return __real_ioctl(fd, request);
    }
    va_list args;
    va_start(args, request);
    int result = -1;
    if (request == SIOCGIFINDEX) {
        auto *value = va_arg(args, struct ifreq *);
        result = __real_ioctl(fd, request, value);
    } else if (request == TCFLSH) {
        const int value = va_arg(args, int);
        result = take(Fault::kFlush) ? -1 : __real_ioctl(fd, request, value);
    } else if (request == TCGETS2 || request == TCSETS2) {
        auto *value = va_arg(args, termios2 *);
        result = take(request == TCGETS2 ? Fault::kGet : Fault::kSet)
                     ? -1
                     : __real_ioctl(fd, request, value);
    } else if (request == TIOCSRS485) {
        auto *value = va_arg(args, serial_rs485 *);
        if (take(Fault::kSanitize)) {
            value->flags = 0;
        } else if (take(Fault::kRs485)) {
            va_end(args);
            return -1;
        }
        // A PTY has no RS485 driver. This models the driver's successful
        // return, including the sanitized flags written back by the kernel.
        result = 0;
    } else if (request == TIOCOUTQ) {
        auto *value = va_arg(args, int *);
        if (take(Fault::kOutputError)) {
            result = -1;
        } else if (take(Fault::kOutputPending)) {
            *value = 1;
            result = 0;
        } else {
            result = __real_ioctl(fd, request, value);
        }
    }
    va_end(args);
    return result;
}
int __wrap_poll(pollfd *fds, nfds_t n, int timeout) {
    if (take(Fault::kPollTimeout)) {
        return 0;
    }
    if (take(Fault::kPollEmpty)) {
        ::usleep(static_cast<useconds_t>(timeout + 1) * 1000);
        fds[0].revents = 0;
        return 1;
    }
    if (take(Fault::kPollError)) {
        return -1;
    }
    if (take(Fault::kPollInterrupt)) {
        errno = EINTR;
        return -1;
    }
    if (take(Fault::kPollInvalid)) {
        fds[0].revents = POLLNVAL;
        return 1;
    }
    if (take(Fault::kPollHangup)) {
        fds[0].revents = POLLHUP;
        return 1;
    }
    return __real_poll(fds, n, timeout);
}
ssize_t __wrap_read(int fd, void *data, size_t size) {
    if (take(Fault::kReadCancel)) {
        cancel_on_io->cancel();
        errno = EAGAIN;
        return -1;
    }
    if (take(Fault::kReadLateRetry)) {
        ::usleep(3000);
        errno = EAGAIN;
        return -1;
    }
    if (take(Fault::kReadError)) {
        return -1;
    }
    if (take(Fault::kReadZero)) {
        return 0;
    }
    if (take(Fault::kReadRetry)) {
        errno = EAGAIN;
        return -1;
    }
    return __real_read(fd, data, size);
}
ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (take(Fault::kWriteCancel)) {
        cancel_on_io->cancel();
        return 1;
    }
    if (take(Fault::kWriteBlocked)) {
        errno = EAGAIN;
        fault = Fault::kPollTimeout;
        return -1;
    }
    if (take(Fault::kWriteError)) {
        return -1;
    }
    if (take(Fault::kWriteZero)) {
        return 0;
    }
    if (take(Fault::kWriteRetry)) {
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, data, size);
}
int __wrap_close(int fd) {
    const int result = __real_close(fd);
    if (fault == Fault::kCloseWake && ++close_attempt == 2) {
        (void)take(Fault::kCloseWake);
        return -1;
    }
    return take(Fault::kClose) ? -1 : result;
}
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
TEST_CASE("Linux serial setup failures release exclusive ownership", "[serial][syscalls]") {
    Pty pty;
    linkerbot::serial::Config config;
    config.port = pty.path;
    for (auto value :
         {Fault::kOpen, Fault::kLock, Fault::kGet, Fault::kExclusive, Fault::kSet, Fault::kFlush}) {
        {
            Arm arm(value);
            REQUIRE_THROWS_AS(linkerbot::serial::open_serial(config), linkerbot::serial::IOError);
        }
        REQUIRE_NOTHROW(linkerbot::serial::open_serial(config));
    }
    config.rs485 = linkerbot::serial::Rs485Mode::kKernel;
    for (bool high : {false, true}) {
        config.rts_high_during_send = high;
        for (auto value : {Fault::kRs485, Fault::kSanitize}) {
            Arm arm(value);
            REQUIRE_THROWS_AS(linkerbot::serial::open_serial(config), linkerbot::serial::IOError);
        }
        REQUIRE_NOTHROW(linkerbot::serial::open_serial(config));
    }
}
TEST_CASE("Linux serial retries transient errors and reports permanent errors",
          "[serial][syscalls]") {
    Pty pty;
    linkerbot::serial::Config config;
    config.port = pty.path;
    auto stream = linkerbot::serial::open_serial(config);
    const auto end = [] { return linkerbot::transport::Clock::now() + 500ms; };
    uint8_t byte = 0x55;
    REQUIRE(::write(pty.master, &byte, 1) == 1);
    for (auto value :
         {Fault::kPollError, Fault::kPollInvalid, Fault::kReadError, Fault::kReadZero}) {
        Arm arm(value);
        REQUIRE_THROWS_AS(stream->read_some(&byte, 1, end(), {}), linkerbot::serial::IOError);
    }
    {
        Arm arm(Fault::kPollInterrupt);
        REQUIRE(stream->read_some(&byte, 1, end(), {}) == 1);
    }
    REQUIRE(::write(pty.master, &byte, 1) == 1);
    {
        Arm arm(Fault::kReadRetry);
        REQUIRE(stream->read_some(&byte, 1, end(), {}) == 1);
    }
    for (auto value : {Fault::kWriteError, Fault::kWriteZero}) {
        Arm arm(value);
        REQUIRE_THROWS_AS(stream->write_some(&byte, 1, end(), {}), linkerbot::serial::IOError);
    }
    {
        Arm arm(Fault::kWriteRetry);
        REQUIRE(stream->write_some(&byte, 1, end(), {}) == 1);
    }
    {
        Arm arm(Fault::kOutputError);
        REQUIRE_THROWS_AS(stream->drain_output_until(end(), {}), linkerbot::serial::IOError);
    }
    {
        Arm arm(Fault::kOutputPending);
        REQUIRE_NOTHROW(stream->drain_output_until(end(), {}));
    }
}
TEST_CASE("Linux SLCAN setup failures release descriptors and ownership", "[slcan][syscalls]") {
    Pty pty;
    linkerbot::can::SlcanConfig config;
    config.port = pty.path;
    for (auto value : {Fault::kEvent, Fault::kOpen, Fault::kLock, Fault::kGet, Fault::kExclusive,
                       Fault::kSet, Fault::kFlush}) {
        {
            Arm arm(value);
            REQUIRE_THROWS_AS(linkerbot::can::open_slcan_serial(config),
                              linkerbot::can::TransportIOError);
        }
        REQUIRE_NOTHROW(linkerbot::can::open_slcan_serial(config));
    }
}
TEST_CASE("Linux SLCAN I/O failures preserve retry and close semantics", "[slcan][syscalls]") {
    Pty pty;
    linkerbot::can::SlcanConfig config;
    config.port = pty.path;
    auto stream = linkerbot::can::open_slcan_serial(config);
    REQUIRE(::write(pty.master, "q", 1) == 1);
    {
        Arm arm(Fault::kPollInterrupt);
        REQUIRE(stream->read_some(1, 500ms) == "q");
    }
    REQUIRE(::write(pty.master, "q", 1) == 1);
    for (auto value : {Fault::kPollError, Fault::kPollInvalid, Fault::kPollHangup, Fault::kReadZero,
                       Fault::kReadError}) {
        Arm arm(value);
        REQUIRE_THROWS_AS(stream->read_some(1, 500ms), linkerbot::can::TransportIOError);
    }
    {
        Arm arm(Fault::kReadLateRetry);
        REQUIRE(stream->read_some(1, 1ms).empty());
    }
    {
        Arm arm(Fault::kPollEmpty);
        REQUIRE(stream->read_some(1, 1ms).empty());
    }
    {
        Arm arm(Fault::kReadRetry);
        REQUIRE(stream->read_some(1, 500ms) == "q");
    }
    REQUIRE_THROWS_AS(stream->write_all("q", 0ms), linkerbot::can::TimeoutError);
    for (auto value : {Fault::kWriteZero, Fault::kWriteError}) {
        Arm arm(value);
        REQUIRE_THROWS_AS(stream->write_all("q", 500ms), linkerbot::can::TransportIOError);
    }
    {
        Arm arm(Fault::kWriteRetry);
        REQUIRE_NOTHROW(stream->write_all("q", 500ms));
    }
    {
        Arm arm(Fault::kWriteBlocked);
        REQUIRE_THROWS_AS(stream->write_all("q", 500ms), linkerbot::can::TimeoutError);
    }
    {
        Arm arm(Fault::kClose);
        REQUIRE_THROWS_AS(stream->close(), linkerbot::can::TransportIOError);
    }
    REQUIRE_NOTHROW(stream->close());
    REQUIRE_NOTHROW(linkerbot::can::open_slcan_serial(config));
    stream = linkerbot::can::open_slcan_serial(config);
    {
        Arm arm(Fault::kCloseWake);
        close_attempt = 0;
        REQUIRE_THROWS_AS(stream->close(), linkerbot::can::TransportIOError);
    }
    REQUIRE_NOTHROW(linkerbot::can::open_slcan_serial(config));
    stream = linkerbot::can::open_slcan_serial(config);
    {
        Arm arm(Fault::kClose);
        REQUIRE_NOTHROW(stream.reset());
    }
}
TEST_CASE("Linux SLCAN observes cancellation between successive native calls",
          "[slcan][syscalls]") {
    Pty pty;
    linkerbot::can::SlcanConfig config;
    config.port = pty.path;
    auto stream = linkerbot::can::open_slcan_serial(config);
    cancel_on_io = stream.get();
    REQUIRE(::write(pty.master, "q", 1) == 1);
    {
        Arm arm(Fault::kReadCancel);
        REQUIRE_THROWS_AS(stream->read_some(1, 500ms), linkerbot::can::ClosedError);
    }
    stream->close();
    stream = linkerbot::can::open_slcan_serial(config);
    cancel_on_io = stream.get();
    {
        Arm arm(Fault::kWriteCancel);
        REQUIRE_THROWS_AS(stream->write_all("qq", 500ms), linkerbot::can::ClosedError);
    }
    cancel_on_io = nullptr;
}
TEST_CASE("Linux backend initialization reports thread and socket option failures",
          "[session][socketcan][syscalls]") {
    using namespace linkerbot;
    int sockets[2]{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    {
        Arm arm(Fault::kSendTimeout);
        REQUIRE_THROWS_AS(
            can::SocketCANTransport(can::SocketCANTransport::AdoptFd{sockets[0], false, {}}),
            can::TransportIOError);
    }
    (void)::close(sockets[1]);
    {
        Arm arm(Fault::kStat);
        REQUIRE_THROWS_AS(can::enumerate_slcan_ports({"/dev/null"}), can::TransportIOError);
    }
}
TEST_CASE("Linux vendor loader reports unresolved modules and default search failures",
          "[vendor_loader][syscalls]") {
    using namespace linkerbot::can;
    const char *fixture = std::getenv("LINKERBOT_TEST_VENDOR_LIBRARY");
    const char *pcan = std::getenv("LINKERBOT_TEST_PCAN_LIBRARY");
    REQUIRE(fixture != nullptr);
    REQUIRE(pcan != nullptr);
    {
        Arm arm(Fault::kModulePath);
        REQUIRE_THROWS_AS(load_vendor_canfd_api(std::string(fixture)), TransportIOError);
    }
    {
        Arm arm(Fault::kModulePath);
        REQUIRE_THROWS_AS(load_pcan_api(std::string(pcan)), TransportIOError);
    }
    // The fixture process has no user library overrides. Keep every search
    // attempt deterministic even on machines with a real vendor driver.
    REQUIRE(::unsetenv("LINKERBOT_CANFD_LIB") == 0);
    {
        Arm arm(Fault::kModule);
        REQUIRE_THROWS_AS(load_vendor_canfd_api(std::nullopt), TransportIOError);
    }
    {
        Arm arm(Fault::kSearch);
        search_attempt = 0;
        auto api = load_vendor_canfd_api(std::nullopt);
        REQUIRE(api->scan_device != nullptr);
        REQUIRE(search_attempt == 3);
    }
}

TEST_CASE("SocketCAN option failures unwind and defaults retain disabled echo",
          "[socketcan][syscalls]") {
    using namespace linkerbot::can;
    SocketCANOptions options;
    options.fd_mode = true;
    options.receive_own_messages = true;
    options.timestamp_mode = SocketCANTimestampMode::kHardware;
    for (const auto failure : {Fault::kCanSocket, Fault::kCanEchoOption, Fault::kCanFdOption,
                               Fault::kCanTimestampOption}) {
        Arm arm(failure);
        REQUIRE_THROWS_AS(SocketCANTransport(options, "lo"), TransportIOError);
    }
    REQUIRE_THROWS_AS(SocketCANTransport(options, ""), ValidationError);
    REQUIRE_THROWS_AS(SocketCANTransport(options, "no-can-device"), TransportIOError);
    REQUIRE_THROWS_AS(SocketCANTransport(options, "lo"), TransportIOError);
    for (const auto mode : {SocketCANTimestampMode::kSoftware, SocketCANTimestampMode::kHardware}) {
        options.timestamp_mode = mode;
        Arm arm(Fault::kCanBindAll);
        REQUIRE_NOTHROW(SocketCANTransport(options, "lo"));
    }
    {
        Arm arm(Fault::kCanBindAll);
        REQUIRE_NOTHROW(SocketCANTransport("lo"));
    }
    int descriptors[2]{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) == 0);
    REQUIRE_THROWS_AS(SocketCANTransport(SocketCANTransport::AdoptFd{
                          descriptors[0], false, {}, SocketCANTimestampMode::kDisabled, true}),
                      TransportIOError);
    CHECK(::fcntl(descriptors[0], F_GETFD) == -1);
    (void)::close(descriptors[1]);
    REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) == 0);
    {
        Arm arm(Fault::kCanTimestampOption);
        REQUIRE_THROWS_AS(SocketCANTransport(SocketCANTransport::AdoptFd{
                              descriptors[0], false, {}, SocketCANTimestampMode::kHardware}),
                          TransportIOError);
    }
    CHECK(::fcntl(descriptors[0], F_GETFD) == -1);
    (void)::close(descriptors[1]);
}

#endif
