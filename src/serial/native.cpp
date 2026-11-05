#include <linkerbot/serial/byte_stream.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <asm/termbits.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace linkerbot::serial {
    namespace {
#if defined(__linux__) || defined(_WIN32)
        using transport::Clock;
        constexpr auto kPollInterval = std::chrono::milliseconds{5};

        void check(Deadline deadline, CancellationToken token, bool cancelled) {
            token.throw_if_cancelled();
            if (cancelled) {
                throw ClosedError("Serial stream closed or cancelled");
            }
            if (Clock::now() >= deadline) {
                throw TimeoutError("Serial operation timed out");
            }
        }

        auto acquire(std::timed_mutex &mutex, Deadline deadline, CancellationToken token,
                     const std::atomic<bool> &cancelled) -> std::unique_lock<std::timed_mutex> {
            std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
            for (;;) {
                check(deadline, token, cancelled.load());
                if (lock.try_lock_until(std::min(deadline, Clock::now() + kPollInterval))) {
                    return lock;
                }
            }
        }

#endif
#if defined(__linux__)
        [[noreturn]] void io_error(const char *operation) {
            throw IOError(operation, {errno, std::system_category()});
        }

        class NativeSerial final : public ByteStream {
            int fd_ = -1;
            bool exclusive_ = false;
            std::atomic<bool> cancelled_{false};
            std::timed_mutex read_mutex_, write_mutex_;
            std::mutex close_mutex_;

            void wait(short events, Deadline deadline, CancellationToken token) {
                for (;;) {
                    check(deadline, token, cancelled_);
                    const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                        std::min(deadline - Clock::now(),
                                 std::chrono::duration_cast<Clock::duration>(kPollInterval)));
                    pollfd descriptor{fd_, events, 0};
                    const int status =
                        ::poll(&descriptor, 1,
                               static_cast<int>(
                                   std::max(std::chrono::milliseconds::rep{0}, left.count())));
                    if (status < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        io_error("poll serial");
                    }
                    check(deadline, token, cancelled_);
                    if ((descriptor.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0) {
                        throw IOError("Serial device disconnected");
                    }
                    if ((descriptor.revents & events) != 0) {
                        return;
                    }
                }
            }

          public:
            explicit NativeSerial(const Config &config) {
                if (config.port.rfind("/dev/", 0) != 0) {
                    throw std::invalid_argument("Linux serial port must be an absolute /dev/ path");
                }
                fd_ = ::open(config.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
                if (fd_ < 0) {
                    io_error("open serial");
                }
                try {
                    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
                        io_error("lock serial");
                    }
                    termios2 settings{};
                    if (::ioctl(fd_, TCGETS2, &settings) != 0) {
                        io_error("get serial settings");
                    }
                    if (::ioctl(fd_, TIOCEXCL) != 0) {
                        io_error("exclusive serial");
                    }
                    exclusive_ = true;
                    settings.c_iflag = 0;
                    settings.c_oflag = 0;
                    settings.c_lflag = 0;
                    settings.c_cflag = BOTHER | CREAD | CLOCAL;
                    switch (config.data_bits) {
                    case 5:
                        settings.c_cflag |= CS5;
                        break;
                    case 6:
                        settings.c_cflag |= CS6;
                        break;
                    case 7:
                        settings.c_cflag |= CS7;
                        break;
                    default:
                        settings.c_cflag |= CS8;
                        break;
                    }
                    if (config.parity != Parity::kNone) {
                        settings.c_cflag |= PARENB;
                        if (config.parity == Parity::kOdd) {
                            settings.c_cflag |= PARODD;
                        }
                    }
                    if (config.stop_bits == StopBits::kTwo) {
                        settings.c_cflag |= CSTOPB;
                    }
                    settings.c_ispeed = config.baud_rate;
                    settings.c_ospeed = config.baud_rate;
                    settings.c_cc[VMIN] = 0;
                    settings.c_cc[VTIME] = 0;
                    if (::ioctl(fd_, TCSETS2, &settings) != 0) {
                        io_error("configure serial");
                    }
                    if (config.rs485 == Rs485Mode::kKernel) {
                        serial_rs485 rs485{};
                        rs485.flags = SER_RS485_ENABLED |
                                      (config.rts_high_during_send ? SER_RS485_RTS_ON_SEND
                                                                   : SER_RS485_RTS_AFTER_SEND);
                        if (::ioctl(fd_, TIOCSRS485, &rs485) != 0) {
                            io_error("configure kernel RS485");
                        }
                    }
                    if (::ioctl(fd_, TCFLSH, 2) != 0) {
                        io_error("flush serial");
                    }
                } catch (...) {
                    if (exclusive_) {
                        (void)::ioctl(fd_, TIOCNXCL);
                    }
                    (void)::close(fd_);
                    throw;
                }
            }
            ~NativeSerial() override {
                close();
            }
            size_t read_some(uint8_t *data, size_t capacity, Deadline deadline,
                             CancellationToken token) override {
                if (capacity != 0 && data == nullptr) {
                    throw std::invalid_argument("Null serial read buffer");
                }
                if (capacity == 0) {
                    return 0;
                }
                try {
                    auto lock = acquire(read_mutex_, deadline, token, cancelled_);
                    for (;;) {
                        wait(POLLIN, deadline, token);
                        const auto count = ::read(fd_, data, std::min(capacity, size_t{65536}));
                        if (count > 0) {
                            return static_cast<size_t>(count);
                        }
                        if (count == 0) {
                            throw IOError("Serial end of stream");
                        }
                        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                            io_error("read serial");
                        }
                    }
                } catch (const TimeoutError &) {
                    return 0;
                }
            }
            size_t write_some(const uint8_t *data, size_t size, Deadline deadline,
                              CancellationToken token) override {
                if (size != 0 && data == nullptr) {
                    throw std::invalid_argument("Null serial write buffer");
                }
                if (size == 0) {
                    return 0;
                }
                auto lock = acquire(write_mutex_, deadline, token, cancelled_);
                for (;;) {
                    wait(POLLOUT, deadline, token);
                    const auto count = ::write(fd_, data, std::min(size, size_t{65536}));
                    if (count > 0) {
                        return static_cast<size_t>(count);
                    }
                    if (count == 0) {
                        throw IOError("Serial write made no progress");
                    }
                    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        io_error("write serial");
                    }
                }
            }
            void drain_output_until(Deadline deadline, CancellationToken token) override {
                auto lock = acquire(write_mutex_, deadline, token, cancelled_);
                for (;;) {
                    check(deadline, token, cancelled_);
                    int pending = 0;
                    if (::ioctl(fd_, TIOCOUTQ, &pending) != 0) {
                        io_error("query serial output queue");
                    }
                    if (pending == 0) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            void cancel() noexcept override {
                cancelled_ = true;
            }
            void close() override {
                cancel();
                const std::lock_guard<std::mutex> state(close_mutex_);
                const std::scoped_lock io(read_mutex_, write_mutex_);
                if (fd_ >= 0) {
                    if (exclusive_) {
                        (void)::ioctl(fd_, TIOCNXCL);
                    }
                    // Linux consumes this descriptor even when close reports EINTR.
                    (void)::close(fd_);
                    fd_ = -1;
                    exclusive_ = false;
                }
            }
        };
#elif defined(_WIN32)
        [[noreturn]] void io_error(const char *operation, DWORD code = GetLastError()) {
            throw IOError(operation, {static_cast<int>(code), std::system_category()});
        }
        class NativeSerial final : public ByteStream {
            HANDLE handle_ = INVALID_HANDLE_VALUE;
            std::atomic<bool> cancelled_{false};
            std::timed_mutex read_mutex_, write_mutex_;
            std::mutex close_mutex_;

            size_t transfer(bool writing, uint8_t *data, size_t size, Deadline deadline,
                            CancellationToken token) {
                check(deadline, token, cancelled_);
                const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (event == nullptr) {
                    io_error("create serial event");
                }
                struct EventOwner {
                    HANDLE handle;
                    ~EventOwner() {
                        (void)CloseHandle(handle);
                    }
                } owner{event};
                OVERLAPPED operation{};
                operation.hEvent = event;
                DWORD count = 0;
                const auto bytes = static_cast<DWORD>(std::min(size, size_t{65536}));
                const BOOL done = writing ? WriteFile(handle_, data, bytes, &count, &operation)
                                          : ReadFile(handle_, data, bytes, &count, &operation);
                if (done != 0) {
                    return count;
                }
                if (GetLastError() != ERROR_IO_PENDING) {
                    io_error("start serial I/O");
                }
                try {
                    for (;;) {
                        check(deadline, token, cancelled_);
                        const DWORD status = WaitForSingleObject(event, 1);
                        if (status == WAIT_OBJECT_0) {
                            break;
                        }
                        if (status == WAIT_FAILED) {
                            io_error("wait serial I/O");
                        }
                    }
                } catch (...) {
                    (void)CancelIoEx(handle_, &operation);
                    // The OVERLAPPED and caller buffer must outlive OS cancellation.
                    // A defective driver can exceed the deadline while draining it.
                    (void)GetOverlappedResult(handle_, &operation, &count, TRUE);
                    throw;
                }
                if (!GetOverlappedResult(handle_, &operation, &count, FALSE)) {
                    if (cancelled_) {
                        throw ClosedError("Serial stream cancelled");
                    }
                    io_error("complete serial I/O");
                }
                return count;
            }

          public:
            explicit NativeSerial(const Config &config) {
                if (config.rs485 != Rs485Mode::kAdapterControlled) {
                    throw std::invalid_argument(
                        "Windows requires an automatic-direction RS485 adapter");
                }
                std::string name = config.port;
                if (name.rfind("\\\\.\\", 0) == 0) {
                    name.erase(0, 4);
                }
                if (name.rfind("COM", 0) != 0 || name.size() <= 3 ||
                    name.find_first_not_of("0123456789", 3) != std::string::npos ||
                    name[3] == '0') {
                    throw std::invalid_argument(
                        "Windows serial port must be COM followed by a positive number");
                }
                const std::string path = "\\\\.\\" + name;
                const std::wstring wide(path.begin(), path.end());
                handle_ = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                      nullptr);
                if (handle_ == INVALID_HANDLE_VALUE) {
                    io_error("open serial exclusively");
                }
                try {
                    DCB settings{};
                    settings.DCBlength = sizeof(settings);
                    if (!GetCommState(handle_, &settings)) {
                        io_error("get serial settings");
                    }
                    settings.BaudRate = config.baud_rate;
                    settings.ByteSize = config.data_bits;
                    settings.Parity =
                        config.parity == Parity::kNone
                            ? NOPARITY
                            : (config.parity == Parity::kEven ? EVENPARITY : ODDPARITY);
                    settings.StopBits =
                        config.stop_bits == StopBits::kOne ? ONESTOPBIT : TWOSTOPBITS;
                    settings.fBinary = TRUE;
                    settings.fParity = config.parity != Parity::kNone;
                    settings.fOutxCtsFlow = FALSE;
                    settings.fOutxDsrFlow = FALSE;
                    settings.fDtrControl = DTR_CONTROL_DISABLE;
                    settings.fDsrSensitivity = FALSE;
                    settings.fTXContinueOnXoff = TRUE;
                    settings.fOutX = FALSE;
                    settings.fInX = FALSE;
                    settings.fErrorChar = FALSE;
                    settings.fNull = FALSE;
                    settings.fRtsControl = RTS_CONTROL_DISABLE;
                    settings.fAbortOnError = FALSE;
                    if (!SetCommState(handle_, &settings)) {
                        io_error("configure serial");
                    }
                    COMMTIMEOUTS timeouts{};
                    timeouts.ReadIntervalTimeout = MAXDWORD;
                    if (!SetCommTimeouts(handle_, &timeouts) ||
                        !PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
                        io_error("configure serial timeouts");
                    }
                } catch (...) {
                    (void)CloseHandle(handle_);
                    throw;
                }
            }
            ~NativeSerial() override {
                try {
                    close();
                } catch (...) {
                    // Explicit close reports Windows cleanup errors.
                    return;
                }
            }
            size_t read_some(uint8_t *data, size_t capacity, Deadline deadline,
                             CancellationToken token) override {
                if (capacity != 0 && data == nullptr) {
                    throw std::invalid_argument("Null serial read buffer");
                }
                if (capacity == 0) {
                    return 0;
                }
                try {
                    auto lock = acquire(read_mutex_, deadline, token, cancelled_);
                    for (;;) {
                        const auto count = transfer(false, data, capacity, deadline, token);
                        if (count != 0) {
                            return count;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                } catch (const TimeoutError &) {
                    return 0;
                }
            }
            size_t write_some(const uint8_t *data, size_t size, Deadline deadline,
                              CancellationToken token) override {
                if (size != 0 && data == nullptr) {
                    throw std::invalid_argument("Null serial write buffer");
                }
                if (size == 0) {
                    return 0;
                }
                auto lock = acquire(write_mutex_, deadline, token, cancelled_);
                return transfer(true, const_cast<uint8_t *>(data), size, deadline, token);
            }
            void drain_output_until(Deadline deadline, CancellationToken token) override {
                auto lock = acquire(write_mutex_, deadline, token, cancelled_);
                for (;;) {
                    check(deadline, token, cancelled_);
                    COMSTAT status{};
                    DWORD errors = 0;
                    if (!ClearCommError(handle_, &errors, &status)) {
                        io_error("query serial queue");
                    }
                    if (errors != 0) {
                        throw IOError("Serial driver reported a communication error");
                    }
                    if (status.cbOutQue == 0) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            void cancel() noexcept override {
                cancelled_ = true;
            }
            void close() override {
                cancel();
                const std::lock_guard<std::mutex> state(close_mutex_);
                const std::scoped_lock io(read_mutex_, write_mutex_);
                if (handle_ != INVALID_HANDLE_VALUE) {
                    if (!CloseHandle(handle_)) {
                        io_error("close serial");
                    }
                    handle_ = INVALID_HANDLE_VALUE;
                }
            }
        };
#endif
    } // namespace
    std::unique_ptr<ByteStream> open_serial(const Config &config) {
        validate_config(config);
#if defined(__linux__) || defined(_WIN32)
        return std::make_unique<NativeSerial>(config);
#else
        throw IOError("Native serial requires Linux or Windows");
#endif
    }
} // namespace linkerbot::serial
