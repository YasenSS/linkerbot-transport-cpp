#ifdef __linux__
#include <atomic>
#include <catch2/catch_test_macros.hpp>

#include "optional_value.hpp"
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <linkerbot/can/slcan_transport.hpp>
#include <poll.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

using namespace linkerbot::can;
using namespace std::chrono_literals;
using linkerbot::test_support::require_value;
namespace {
    void interrupt_poll(int) {}

    struct Pty {
        int master = -1;
        std::string path;
        Pty() : master(::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) {
            try {
                if (master < 0 || (::grantpt(master) != 0) || (::unlockpt(master) != 0)) {
                    throw std::runtime_error("Cannot create test PTY");
                }
                char name[128]{};
                if (::ptsname_r(master, name, sizeof(name)) != 0) {
                    throw std::runtime_error("Cannot name test PTY");
                }
                path = name;
            } catch (...) {
                if (master >= 0) {
                    ::close(master);
                }
                throw;
            }
        }
        ~Pty() {
            if (master >= 0) {
                ::close(master);
            }
        }
        [[nodiscard]] auto config() const -> SlcanConfig {
            SlcanConfig c;
            c.port = path;
            c.serial_baud = 460800;
            return c;
        }
        auto receive(size_t size) -> std::string {
            std::string result;
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (result.size() < size && std::chrono::steady_clock::now() < deadline) {
                pollfd p{master, POLLIN, 0};
                if (::poll(&p, 1, 10) > 0) {
                    char bytes[256];
                    auto n = ::read(master, bytes, sizeof(bytes));
                    if (n > 0) {
                        result.append(bytes, static_cast<size_t>(n));
                    }
                }
            }
            return result;
        }
    };
} // namespace
TEST_CASE("SLCAN Linux native validation covers serial and enumeration boundaries",
          "[slcan][native-serial]") {
    SlcanConfig invalid;
    invalid.port = "COM10";
    REQUIRE_THROWS_AS(open_slcan_serial(invalid), ValidationError);

    invalid.port = "/dev/null";
    REQUIRE_THROWS_AS(open_slcan_serial(invalid), TransportIOError);
    REQUIRE_THROWS_AS(enumerate_slcan_ports({"COM10"}), ValidationError);
    REQUIRE_THROWS_AS(enumerate_slcan_ports({"/dev/fd"}), ValidationError);

    Pty pty;
    auto config = pty.config();
    config.startup_delay = 1ms;
    auto port = open_slcan_serial(config);
    REQUIRE_THROWS_AS(port->read_some(1, -1ms), ValidationError);
    REQUIRE(port->read_some(0, 1ms).empty());
    REQUIRE_NOTHROW(port->close());
}
TEST_CASE("SLCAN Linux native poll retries an interrupt without losing input",
          "[slcan][native-serial]") {
    struct sigaction action {};
    action.sa_handler = interrupt_poll;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    struct sigaction previous {};
    REQUIRE(::sigaction(SIGUSR1, &action, &previous) == 0);
    struct RestoreSignal {
        struct sigaction previous;
        ~RestoreSignal() {
            (void)::sigaction(SIGUSR1, &previous, nullptr);
        }
    } restore{previous};

    Pty pty;
    auto port = open_slcan_serial(pty.config());
    std::promise<pthread_t> reader_thread;
    auto reader = std::async(std::launch::async, [&] {
        reader_thread.set_value(pthread_self());
        return port->read_some(1, 1s);
    });
    const auto thread = reader_thread.get_future().get();
    std::this_thread::sleep_for(20ms);
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(pthread_kill(thread, SIGUSR1) == 0);
        std::this_thread::sleep_for(5ms);
    }
    const char byte = 'q';
    REQUIRE(::write(pty.master, &byte, 1) == 1);
    REQUIRE(reader.get() == "q");
    REQUIRE_NOTHROW(port->close());
}
TEST_CASE("SLCAN Linux native serial preserves binary bytes and releases exclusive ownership",
          "[slcan][native-serial]") {
    Pty pty;
    REQUIRE(enumerate_slcan_ports({pty.path}) == std::vector<std::string>{pty.path});
    auto port = open_slcan_serial(pty.config());
    REQUIRE_THROWS(open_slcan_serial(pty.config()));
    REQUIRE(enumerate_slcan_ports({pty.path}) ==
            std::vector<std::string>{pty.path}); // Metadata does not open even an occupied port.
    const std::string bytes{"\0\n\r\x11\x13\xff", 6};
    port->write_all(bytes, 100ms);
    REQUIRE(pty.receive(bytes.size()) == bytes);
    REQUIRE(::write(pty.master, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    REQUIRE(port->read_some(256, 100ms) == bytes);
    REQUIRE(port->read_some(256, 5ms).empty());
    port->close();
    REQUIRE_NOTHROW(port->close());
    REQUIRE_THROWS_AS(port->read_some(1, 1ms), ClosedError);
    REQUIRE_THROWS_AS(port->write_all("C\r", 1ms), ClosedError);
    REQUIRE_NOTHROW(open_slcan_serial(pty.config()));
}
TEST_CASE("SLCAN Linux native cancellation interrupts both blocked read and write",
          "[slcan][native-serial]") {
    Pty pty;
    auto port = open_slcan_serial(pty.config());
    std::promise<void> started;
    auto reader = std::async(std::launch::async, [&] {
        started.set_value();
        try {
            (void)port->read_some(1, 5s);
        } catch (const ClosedError &) {
            return true;
        }
        return false;
    });
    started.get_future().wait();
    port->cancel();
    REQUIRE(reader.wait_for(500ms) == std::future_status::ready);
    REQUIRE(reader.get());
    REQUIRE_NOTHROW(port->close());
    port = open_slcan_serial(pty.config());
    std::promise<void> writing;
    auto writer = std::async(std::launch::async, [&] {
        writing.set_value();
        try {
            port->write_all(std::string(static_cast<std::size_t>(4) * 1024 * 1024, 'x'), 5s);
        } catch (const ClosedError &) {
            return true;
        }
        return false;
    });
    writing.get_future().wait();
    // Wait until the PTY receives a prefix so this write is in flight before cancel.
    const auto prefix = pty.receive(1);
    port->cancel();
    REQUIRE(writer.wait_for(500ms) == std::future_status::ready);
    REQUIRE(writer.get());
    REQUIRE_FALSE(prefix.empty());
    port->close();
}
TEST_CASE("SLCAN Linux reports missing devices disconnect and bounded partial writes",
          "[slcan][native-serial]") {
    auto missing = SlcanConfig{};
    missing.port = "/dev/linkerbot-test-missing-serial";
    REQUIRE(enumerate_slcan_ports({missing.port}).empty());
    REQUIRE_THROWS_AS(open_slcan_serial(missing), TransportIOError);
    Pty pty;
    auto port = open_slcan_serial(pty.config());
    REQUIRE_THROWS_AS(
        port->write_all(std::string(static_cast<std::size_t>(4) * 1024 * 1024, 'x'), 5ms),
        TimeoutError);
    ::close(pty.master);
    pty.master = -1;
    REQUIRE_THROWS_AS(port->write_all("x", 100ms), TransportIOError);
    REQUIRE_THROWS_AS(port->read_some(1, 100ms), TransportIOError);
    REQUIRE_NOTHROW(port->close());
}
TEST_CASE("SLCAN Linux CANable FD crosses the real serial implementation without ACKs",
          "[slcan][native-serial][fd]") {
    Pty pty;
    auto c = pty.config();
    c.protocol = SlcanProtocol::kCanableFd;
    c.fd = true;
    SlcanTransport transport(c);
    REQUIRE(pty.receive(10) == "C\rS8\rY5\rO\r");
    Message m;
    m.id = 0x123;
    m.is_fd = true;
    m.is_extended = true;
    m.dlc = 64;
    for (unsigned i = 0; i < 64; ++i) {
        m.data[i] = static_cast<uint8_t>(i);
    }
    const auto wire = encode_slcan_frame(m, SlcanProtocol::kCanableFd);
    transport.write(m);
    REQUIRE(pty.receive(wire.size()) == wire);
    REQUIRE(::write(pty.master, wire.data(), wire.size()) == static_cast<ssize_t>(wire.size()));
    const auto reply = transport.read();
    REQUIRE(reply);
    REQUIRE(require_value(reply).id == m.id);
    REQUIRE(require_value(reply).is_fd);
    REQUIRE(require_value(reply).is_extended);
    REQUIRE_FALSE(require_value(reply).is_brs);
    REQUIRE(require_value(reply).data == m.data);
    transport.close();
    REQUIRE(pty.receive(2) == "C\r");
}
#endif
