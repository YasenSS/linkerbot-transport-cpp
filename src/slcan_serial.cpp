#include <algorithm>
#include <atomic>
#include <filesystem>
#include <linkerbot/can/slcan_transport.hpp>
#include <mutex>
#include <system_error>
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
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace linkerbot::can {
    namespace {
#ifdef _WIN32
        [[noreturn]] void win_error(const char *operation, DWORD code = GetLastError()) {
            throw TransportIOError(
                std::string(operation) + " (Windows error " + std::to_string(code) + ")",
                {static_cast<int>(code), std::system_category()}, static_cast<int>(code));
        }
        struct NativeCleanup {
            std::mutex mutex;
            std::vector<HANDLE> handles;
        };
        auto native_cleanup() -> NativeCleanup & {
            static auto *store = new NativeCleanup;
            return *store;
        }
        void retain_handle(HANDLE handle) {
            auto &store = native_cleanup();
            std::lock_guard<std::mutex> lock(store.mutex);
            store.handles.push_back(handle);
        }
        void dispose(HANDLE handle) noexcept {
            if (handle && handle != INVALID_HANDLE_VALUE && !CloseHandle(handle))
                retain_handle(handle);
        }
        class NativeSerial final : public SlcanSerialPort {
            HANDLE handle_ = INVALID_HANDLE_VALUE;
            std::mutex read_mutex_, write_mutex_, handle_mutex_;
            std::atomic<bool> cancelled_{false};

          public:
            explicit NativeSerial(const SlcanConfig &config) {
                const auto name = slcan_port_name(config.port);
                if (name.substr(0, 3) != "COM")
                    throw ValidationError("Windows SLCAN requires a COM port");
                const auto port = "\\\\.\\" + name;
                const std::wstring wide(port.begin(), port.end()); // Canonical COM name is ASCII.
                handle_ = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                      nullptr);
                if (handle_ == INVALID_HANDLE_VALUE)
                    win_error("Open SLCAN serial (exclusive)");
                try {
                    DCB dcb{};
                    dcb.DCBlength = sizeof(dcb);
                    if (!GetCommState(handle_, &dcb))
                        win_error("GetCommState");
                    dcb.BaudRate = config.serial_baud;
                    dcb.ByteSize = 8;
                    dcb.Parity = NOPARITY;
                    dcb.StopBits = ONESTOPBIT;
                    dcb.fBinary = TRUE;
                    dcb.fParity = FALSE;
                    dcb.fOutxCtsFlow = FALSE;
                    dcb.fOutxDsrFlow = FALSE;
                    dcb.fDtrControl = DTR_CONTROL_DISABLE;
                    dcb.fDsrSensitivity = FALSE;
                    dcb.fTXContinueOnXoff = TRUE;
                    dcb.fOutX = FALSE;
                    dcb.fInX = FALSE;
                    dcb.fErrorChar = FALSE;
                    dcb.fNull = FALSE;
                    dcb.fRtsControl = RTS_CONTROL_DISABLE;
                    dcb.fAbortOnError = FALSE;
                    if (!SetCommState(handle_, &dcb))
                        win_error("SetCommState 8N1");
                    COMMTIMEOUTS timeouts{};
                    // Return currently available bytes immediately; OVERLAPPED wait
                    // plus a bounded idle wait below handles an empty input buffer.
                    timeouts.ReadIntervalTimeout = MAXDWORD;
                    if (!SetCommTimeouts(handle_, &timeouts))
                        win_error("SetCommTimeouts");
                    if (!PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR))
                        win_error("PurgeComm");
                } catch (...) {
                    dispose(handle_);
                    handle_ = INVALID_HANDLE_VALUE;
                    throw;
                }
            }
            ~NativeSerial() override {
                try {
                    close();
                } catch (...) {
                    dispose(handle_);
                    handle_ = INVALID_HANDLE_VALUE;
                }
            }
            auto transfer(bool writing, void *data, DWORD size,
                          std::chrono::milliseconds timeout) -> DWORD {
                HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!event)
                    win_error("CreateEvent SLCAN");
                struct EventOwner {
                    HANDLE value;
                    ~EventOwner() {
                        dispose(value);
                    }
                } owner{event};
                OVERLAPPED overlapped{};
                overlapped.hEvent = event;
                DWORD transferred = 0;
                {
                    std::lock_guard<std::mutex> lock(handle_mutex_);
                    if (handle_ == INVALID_HANDLE_VALUE)
                        throw ClosedError("SLCAN serial closed");
                    if (!writing && cancelled_)
                        throw ClosedError("SLCAN read cancelled");
                    const auto done =
                        writing ? WriteFile(handle_, data, size, &transferred, &overlapped)
                                : ReadFile(handle_, data, size, &transferred, &overlapped);
                    if (done)
                        return transferred;
                    if (GetLastError() != ERROR_IO_PENDING)
                        win_error(writing ? "WriteFile SLCAN" : "ReadFile SLCAN");
                }
                const auto wait = WaitForSingleObject(event, static_cast<DWORD>(timeout.count()));
                if (wait != WAIT_OBJECT_0) {
                    const auto wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
                    CancelIoEx(handle_, &overlapped);
                    // The OS owns the OVERLAPPED/buffer until cancellation completes.
                    // Drain before returning; a wedged driver can exceed the budget.
                    GetOverlappedResult(handle_, &overlapped, &transferred, TRUE);
                    if (wait != WAIT_TIMEOUT)
                        win_error("Wait SLCAN I/O", wait_error);
                    if (writing)
                        throw TimeoutError("SLCAN serial write timed out; current bytes uncertain");
                    return 0;
                }
                if (!GetOverlappedResult(handle_, &overlapped, &transferred, FALSE)) {
                    const auto error = GetLastError();
                    if (error == ERROR_OPERATION_ABORTED && cancelled_)
                        throw ClosedError("SLCAN I/O cancelled");
                    win_error("Complete SLCAN I/O", error);
                }
                return transferred;
            }
            auto read_some(size_t max_bytes,
                           std::chrono::milliseconds timeout) -> std::string override {
                std::lock_guard<std::mutex> lock(read_mutex_);
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                std::string bytes(std::min(size_t{256}, max_bytes), '\0');
                if (bytes.empty())
                    return {};
                const auto count =
                    transfer(false, bytes.data(), static_cast<DWORD>(bytes.size()), timeout);
                bytes.resize(count);
                if (!count) {
                    // No busy spin when the serial timeout mode returns zero bytes.
                    while (!cancelled_ && std::chrono::steady_clock::now() < deadline)
                        Sleep(1);
                    if (cancelled_)
                        throw ClosedError("SLCAN read cancelled");
                }
                return bytes;
            }
            auto write_all(std::string_view bytes,
                           std::chrono::milliseconds timeout) -> void override {
                std::lock_guard<std::mutex> lock(write_mutex_);
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                size_t offset = 0;
                while (offset < bytes.size()) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
                    if (remaining.count() <= 0)
                        throw TimeoutError("SLCAN serial partial write timed out");
                    const auto count =
                        transfer(true, const_cast<char *>(bytes.data() + offset),
                                 static_cast<DWORD>(bytes.size() - offset), remaining);
                    if (!count)
                        throw TransportIOError("SLCAN serial write made no progress");
                    offset += count;
                }
            }
            auto cancel() noexcept -> void override {
                std::lock_guard<std::mutex> lock(handle_mutex_);
                cancelled_ = true;
                if (handle_ != INVALID_HANDLE_VALUE)
                    CancelIoEx(handle_, nullptr);
            }
            auto close() -> void override {
                cancel();
                std::scoped_lock io(read_mutex_, write_mutex_);
                std::lock_guard<std::mutex> lock(handle_mutex_);
                if (handle_ == INVALID_HANDLE_VALUE)
                    return;
                if (!CloseHandle(handle_))
                    win_error("CloseHandle SLCAN serial");
                handle_ = INVALID_HANDLE_VALUE;
            }
        };
#elif defined(__linux__)
        [[noreturn]] void posix_error(const char *operation, int code = errno) {
            throw TransportIOError(std::string(operation) + ": " +
                                       std::system_category().message(code),
                                   {code, std::system_category()}, code);
        }
        class NativeSerial final : public SlcanSerialPort {
            int fd_ = -1, wake_ = -1;
            std::mutex read_mutex_, write_mutex_, state_mutex_;
            std::atomic<bool> cancelled_{false};
            bool exclusive_ = false;

            auto wait(short events, std::chrono::steady_clock::time_point deadline,
                      bool after_cancel = false) -> bool {
                for (;;) {
                    if (cancelled_ && !after_cancel) {
                        throw ClosedError("SLCAN serial I/O cancelled");
                    }
                    const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
                    const auto timeout =
                        static_cast<int>(std::clamp<int64_t>(left.count(), 0, INT_MAX));
                    pollfd descriptors[2]{{fd_, events, 0}, {after_cancel ? -1 : wake_, POLLIN, 0}};
                    const int result = ::poll(descriptors, 2, timeout);
                    if (result < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        posix_error("poll SLCAN serial");
                    }
                    if (cancelled_ && !after_cancel) {
                        throw ClosedError("SLCAN serial I/O cancelled");
                    }
                    if (result == 0) {
                        return false;
                    }
                    if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
                        throw TransportIOError("SLCAN serial disconnected or invalid");
                    }
                    if ((descriptors[0].revents & events) != 0) {
                        return true;
                    }
                    if ((descriptors[0].revents & POLLHUP) != 0) {
                        throw TransportIOError("SLCAN serial disconnected");
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        return false;
                    }
                }
            }

          public:
            explicit NativeSerial(const SlcanConfig &config) {
                const auto path = slcan_port_name(config.port);
                if (path.substr(0, 5) != "/dev/") {
                    throw ValidationError("Linux SLCAN requires an absolute /dev/... path");
                }
                try {
                    wake_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    if (wake_ < 0) {
                        posix_error("eventfd SLCAN cancellation");
                    }
                    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
                    if (fd_ < 0) {
                        posix_error("Open SLCAN serial");
                    }
                    // flock also rejects same-process opens and aliases. TIOCEXCL
                    // additionally protects against serial clients that do not flock.
                    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
                        posix_error("Lock SLCAN serial (exclusive)");
                    }
                    termios2 settings{};
                    if (::ioctl(fd_, TCGETS2, &settings) != 0) {
                        posix_error("TCGETS2 SLCAN serial");
                    }
                    if (::ioctl(fd_, TIOCEXCL) != 0) {
                        posix_error("TIOCEXCL SLCAN serial");
                    }
                    exclusive_ = true;
                    settings.c_iflag = 0;
                    settings.c_oflag = 0;
                    settings.c_lflag = 0;
                    settings.c_cflag = BOTHER | CS8 | CREAD | CLOCAL;
                    settings.c_ispeed = config.serial_baud;
                    settings.c_ospeed = config.serial_baud;
                    settings.c_cc[VMIN] = 0;
                    settings.c_cc[VTIME] = 0;
                    if (::ioctl(fd_, TCSETS2, &settings) != 0) {
                        posix_error("TCSETS2 SLCAN raw 8N1 baudrate");
                    }
                    if (::ioctl(fd_, TCFLSH, 2) != 0) {
                        posix_error("Flush SLCAN serial"); // TCIOFLUSH
                    }
                } catch (...) {
                    if (fd_ >= 0) {
                        if (exclusive_) {
                            (void)::ioctl(fd_, TIOCNXCL);
                        }
                        (void)::close(fd_);
                        fd_ = -1;
                    }
                    if (wake_ >= 0) {
                        (void)::close(wake_);
                        wake_ = -1;
                    }
                    throw;
                }
            }
            ~NativeSerial() override {
                try {
                    close();
                } catch (...) {
                    // Linux native-close errors refer to consumed descriptors;
                    // this destructor must not propagate a close diagnostic.
                    return;
                }
            }
            auto read_some(size_t max_bytes,
                           std::chrono::milliseconds timeout) -> std::string override {
                std::lock_guard<std::mutex> lock(read_mutex_);
                if (fd_ < 0 || cancelled_) {
                    throw ClosedError("SLCAN serial closed or cancelled");
                }
                if (timeout.count() < 0) {
                    throw ValidationError("Negative serial timeout");
                }
                std::string bytes(std::min(size_t{256}, max_bytes), '\0');
                if (bytes.empty()) {
                    return {};
                }
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (wait(POLLIN, deadline)) {
                    const auto count = ::read(fd_, bytes.data(), bytes.size());
                    if (count > 0) {
                        bytes.resize(static_cast<size_t>(count));
                        return bytes;
                    }
                    if (count == 0) {
                        throw TransportIOError("SLCAN serial end of stream");
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        posix_error("Read SLCAN serial");
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        break;
                    }
                }
                return {};
            }
            auto write_all(std::string_view bytes,
                           std::chrono::milliseconds timeout) -> void override {
                std::lock_guard<std::mutex> lock(write_mutex_);
                if (fd_ < 0) {
                    throw ClosedError("SLCAN serial closed");
                }
                const bool after_cancel =
                    cancelled_; // A new best-effort C during close is allowed.
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                size_t offset = 0;
                while (offset < bytes.size()) {
                    if (cancelled_ && !after_cancel) {
                        throw ClosedError("SLCAN serial write cancelled");
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        throw TimeoutError("SLCAN serial write timed out; bytes may be partial");
                    }
                    const auto count = ::write(fd_, bytes.data() + offset, bytes.size() - offset);
                    if (count > 0) {
                        offset += static_cast<size_t>(count);
                        continue;
                    }
                    if (count == 0) {
                        throw TransportIOError("SLCAN serial write made no progress");
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        posix_error("Write SLCAN serial");
                    }
                    if (!wait(POLLOUT, deadline, after_cancel)) {
                        throw TimeoutError("SLCAN serial write timed out; bytes may be partial");
                    }
                }
            }
            auto cancel() noexcept -> void override {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cancelled_ = true;
                if (wake_ >= 0) {
                    const uint64_t one = 1;
                    const auto result = ::write(wake_, &one, sizeof(one));
                    (void)result;
                }
            }
            auto close() -> void override {
                cancel();
                std::scoped_lock io(read_mutex_, write_mutex_);
                std::lock_guard<std::mutex> lock(state_mutex_);
                int error = 0;
                if (fd_ >= 0) {
                    if (exclusive_) {
                        (void)::ioctl(fd_, TIOCNXCL);
                    }
                    // On Linux close releases the descriptor even on EINTR/EIO.
                    // Never retry a numeric fd that another thread may have reused.
                    const int fd = fd_;
                    fd_ = -1;
                    exclusive_ = false;
                    if (::close(fd) != 0) {
                        error = errno;
                    }
                }
                if (wake_ >= 0) {
                    const int fd = wake_;
                    wake_ = -1;
                    if ((::close(fd) != 0) && (error == 0)) {
                        error = errno;
                    }
                }
                if (error != 0) {
                    posix_error("Close SLCAN serial", error);
                }
            }
        };

#endif
    } // namespace
    auto open_slcan_serial(const SlcanConfig &config) -> std::shared_ptr<SlcanSerialPort> {
        validate_slcan_config(config);
#if defined(_WIN32) || defined(__linux__)
        auto port = std::make_shared<NativeSerial>(config);
        if (config.startup_delay.count() != 0) {
            std::this_thread::sleep_for(config.startup_delay);
        }
        return port;
#else
        throw TransportIOError("Native SLCAN serial backend requires Windows or Linux");
#endif
    }
    auto
    enumerate_slcan_ports(const std::vector<std::string> &allowlist) -> std::vector<std::string> {
        std::vector<std::string> normalized;
        for (const auto &name : allowlist) {
            const auto port = slcan_port_name(name);
            if (std::find(normalized.begin(), normalized.end(), port) != normalized.end()) {
                throw ValidationError("Duplicate SLCAN allowed port");
            }
            normalized.push_back(port);
        }
        if (normalized.empty()) {
            return {};
        }
#ifdef _WIN32
        std::vector<std::string> ports;
        for (const auto &port : normalized) {
            if (port.substr(0, 3) != "COM")
                throw ValidationError("Windows SLCAN requires COM ports");
            const std::wstring wide(port.begin(), port.end());
            wchar_t target[32768];
            if (QueryDosDeviceW(wide.c_str(), target, 32768))
                ports.push_back(port);
            else if (GetLastError() != ERROR_FILE_NOT_FOUND)
                win_error("QueryDosDevice SLCAN");
        }
        return ports;
#elif defined(__linux__)
        std::vector<std::string> ports;
        for (const auto &port : normalized) {
            if (port.substr(0, 5) != "/dev/") {
                throw ValidationError("Linux SLCAN requires /dev/... ports");
            }
            struct stat info {};
            if (::stat(port.c_str(), &info) == 0) {
                if (S_ISCHR(info.st_mode)) {
                    ports.push_back(port);
                } else {
                    throw ValidationError("SLCAN port must be a character device");
                }
            } else if (errno != ENOENT && errno != ENOTDIR) {
                posix_error("stat SLCAN port");
            }
        }
        return ports;
#else
        throw TransportIOError("SLCAN serial enumeration requires Windows or Linux");
#endif
    }
    // Internal cleanup hooks also cover handles retained after partial native setup.
    auto retry_native_slcan_cleanup() -> void {
#ifdef _WIN32
        auto &store = native_cleanup();
        std::lock_guard<std::mutex> lock(store.mutex);
        for (auto it = store.handles.begin(); it != store.handles.end();) {
            if (!CloseHandle(*it))
                win_error("Retry CloseHandle SLCAN");
            it = store.handles.erase(it);
        }
#endif
    }
    auto pending_native_slcan_cleanup() -> size_t {
#ifdef _WIN32
        auto &store = native_cleanup();
        std::lock_guard<std::mutex> lock(store.mutex);
        return store.handles.size();
#else
        return 0;
#endif
    }
} // namespace linkerbot::can
