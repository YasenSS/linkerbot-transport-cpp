#include <catch2/catch_test_macros.hpp>
#include <linkerbot/serial/byte_stream.hpp>

#include <algorithm>
#include <future>
#include <thread>
#include <vector>

#ifdef __linux__
#include <cstdlib>
#include <fcntl.h>
#include <linkerbot/can/slcan_transport.hpp>
#include <poll.h>
#include <unistd.h>
#endif

using namespace linkerbot;
using namespace linkerbot::serial;
using namespace std::chrono_literals;
#ifdef __linux__
namespace {
    auto deadline(std::chrono::milliseconds timeout = 500ms) {
        return transport::Clock::now() + timeout;
    }
} // namespace
#endif
TEST_CASE("Serial configuration validates every public enum and byte width", "[serial]") {
    Config config;
    config.port = "fake";
    REQUIRE_NOTHROW(validate_config(config));
    for (const auto &port : {std::string{}, std::string("bad\0port", 8)}) {
        auto invalid = config;
        invalid.port = port;
        REQUIRE_THROWS_AS(validate_config(invalid), std::invalid_argument);
    }
    for (const auto baud : {uint32_t{0}, uint32_t{4000001}}) {
        auto invalid = config;
        invalid.baud_rate = baud;
        REQUIRE_THROWS_AS(validate_config(invalid), std::invalid_argument);
    }
    for (const uint8_t bits : {uint8_t{4}, uint8_t{9}}) {
        auto invalid = config;
        invalid.data_bits = bits;
        REQUIRE_THROWS_AS(validate_config(invalid), std::invalid_argument);
    }
    config.parity = static_cast<Parity>(99);
    REQUIRE_THROWS_AS(validate_config(config), std::invalid_argument);
    config.parity = Parity::kOdd;
    config.stop_bits = static_cast<StopBits>(99);
    REQUIRE_THROWS_AS(validate_config(config), std::invalid_argument);
    config.stop_bits = StopBits::kTwo;
    config.rs485 = static_cast<Rs485Mode>(99);
    REQUIRE_THROWS_AS(validate_config(config), std::invalid_argument);
    config.rs485 = Rs485Mode::kKernel;
    REQUIRE_NOTHROW(validate_config(config));
}
#ifdef __linux__
namespace {
    struct Pty {
        int master = -1;
        Config config;
        Pty() : master(::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) {
            if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
                if (master >= 0) {
                    (void)::close(master);
                }
                throw std::runtime_error("Cannot open PTY");
            }
            char name[128]{};
            if (::ptsname_r(master, name, sizeof(name)) != 0) {
                (void)::close(master);
                throw std::runtime_error("Cannot name PTY");
            }
            config.port = name;
            config.parity = Parity::kNone;
        }
        ~Pty() {
            if (master >= 0) {
                (void)::close(master);
            }
        }
        Pty(const Pty &) = delete;
        Pty &operator=(const Pty &) = delete;
    };
} // namespace
TEST_CASE("Serial Linux preserves binary bytes and excludes aliases and SLCAN",
          "[serial][native-serial]") {
    Pty pty;
    auto stream = open_serial(pty.config);
    REQUIRE_THROWS(open_serial(pty.config));
    auto alias = pty.config;
    alias.port = "/dev/pts/../pts/" + pty.config.port.substr(pty.config.port.find_last_of('/') + 1);
    REQUIRE_THROWS(open_serial(alias));
    can::SlcanConfig slcan;
    slcan.port = pty.config.port;
    REQUIRE_THROWS(can::open_slcan_serial(slcan));
    const uint8_t bytes[]{0, 10, 13, 17, 19, 0xff};
    REQUIRE(stream->write_some(bytes, sizeof(bytes), deadline(), {}) == sizeof(bytes));
    stream->drain_output_until(deadline(), {});
    uint8_t buffer[256]{};
    pollfd ready{pty.master, POLLIN, 0};
    REQUIRE(::poll(&ready, 1, 500) == 1);
    REQUIRE(::read(pty.master, buffer, sizeof(buffer)) == static_cast<ssize_t>(sizeof(bytes)));
    REQUIRE(std::equal(std::begin(bytes), std::end(bytes), buffer));
    REQUIRE(::write(pty.master, bytes, sizeof(bytes)) == static_cast<ssize_t>(sizeof(bytes)));
    REQUIRE(stream->read_some(buffer, sizeof(buffer), deadline(), {}) == sizeof(bytes));
    REQUIRE(std::equal(std::begin(bytes), std::end(bytes), buffer));
    REQUIRE(stream->read_some(buffer, sizeof(buffer), deadline(5ms), {}) == 0);
    REQUIRE(stream->read_some(nullptr, 0, deadline(), {}) == 0);
    REQUIRE(stream->write_some(nullptr, 0, deadline(), {}) == 0);
    REQUIRE_THROWS_AS(stream->read_some(nullptr, 1, deadline(), {}), std::invalid_argument);
    REQUIRE_THROWS_AS(stream->write_some(nullptr, 1, deadline(), {}), std::invalid_argument);
    stream->close();
    stream->close();
    REQUIRE_THROWS_AS(stream->read_some(buffer, 1, deadline(), {}), ClosedError);
    REQUIRE_THROWS_AS(stream->write_some(bytes, 1, deadline(), {}), ClosedError);
    REQUIRE_THROWS_AS(stream->drain_output_until(deadline(), {}), ClosedError);
    REQUIRE_NOTHROW(open_serial(pty.config));
    can::SlcanConfig slcan_owner;
    slcan_owner.port = pty.config.port;
    auto slcan_stream = can::open_slcan_serial(slcan_owner);
    REQUIRE_THROWS(open_serial(pty.config));
    slcan_stream->close();
    REQUIRE_NOTHROW(open_serial(pty.config));
}
TEST_CASE("Serial Linux rejects missing nonserial and unsupported RS485 devices",
          "[serial][native-serial]") {
    Config invalid;
    invalid.port = "COM1";
    REQUIRE_THROWS_AS(open_serial(invalid), std::invalid_argument);
    invalid.port = "/dev/linkerbot-rtu-not-a-device";
    REQUIRE_THROWS_AS(open_serial(invalid), IOError);
    invalid.port = "/dev/null";
    REQUIRE_THROWS_AS(open_serial(invalid), IOError);
    Pty pty;
    auto config = pty.config;
    config.rs485 = Rs485Mode::kKernel;
    REQUIRE_THROWS_AS(open_serial(config), IOError);
    REQUIRE_NOTHROW(open_serial(pty.config));
    for (const uint8_t bits : {uint8_t{5}, uint8_t{6}, uint8_t{7}, uint8_t{8}}) {
        config = pty.config;
        config.data_bits = bits;
        REQUIRE_NOTHROW(open_serial(config));
    }
    config = pty.config;
    config.stop_bits = StopBits::kTwo;
    REQUIRE_NOTHROW(open_serial(config));
    config.parity = Parity::kOdd;
    REQUIRE_NOTHROW(open_serial(config));
}
TEST_CASE("Serial Linux cancellation bounds read write and same-direction contention",
          "[serial][concurrency]") {
    Pty pty;
    auto stream = open_serial(pty.config);
    uint8_t data = 0;
    auto reader = std::async(std::launch::async, [&] {
        try {
            (void)stream->read_some(&data, 1, deadline(5s), {});
        } catch (const ClosedError &) {
            return true;
        }
        return false;
    });
    std::this_thread::sleep_for(10ms);
    uint8_t other = 0;
    REQUIRE(stream->read_some(&other, 1, deadline(5ms), {}) == 0);
    stream->cancel();
    REQUIRE(reader.wait_for(500ms) == std::future_status::ready);
    REQUIRE(reader.get());
    stream->close();
    stream = open_serial(pty.config);
    transport::CancellationSource cancellation;
    auto token_reader = std::async(std::launch::async, [&] {
        try {
            (void)stream->read_some(&data, 1, deadline(5s), cancellation.token());
        } catch (const transport::CancelledError &) {
            return true;
        }
        return false;
    });
    cancellation.cancel();
    REQUIRE(token_reader.wait_for(500ms) == std::future_status::ready);
    REQUIRE(token_reader.get());
    std::vector<uint8_t> large(size_t{4} * 1024 * 1024, 0x55);
    auto writer = std::async(std::launch::async, [&] {
        size_t offset = 0;
        try {
            const auto end = deadline(5s);
            while (offset < large.size()) {
                offset += stream->write_some(large.data() + offset, large.size() - offset, end, {});
            }
        } catch (const ClosedError &) {
            return true;
        }
        return false;
    });
    pollfd ready{pty.master, POLLIN, 0};
    const auto status = ::poll(&ready, 1, 500);
    stream->close();
    REQUIRE(status == 1);
    REQUIRE(writer.wait_for(500ms) == std::future_status::ready);
    REQUIRE(writer.get());
}
TEST_CASE("Serial Linux reports expired writes and disconnected devices",
          "[serial][native-serial]") {
    Pty pty;
    auto stream = open_serial(pty.config);
    uint8_t byte = 1;
    REQUIRE_THROWS_AS(stream->write_some(&byte, 1, deadline(-1ms), {}), TimeoutError);
    REQUIRE_THROWS_AS(stream->drain_output_until(deadline(-1ms), {}), TimeoutError);
    std::vector<uint8_t> large(size_t{4} * 1024 * 1024, 0x55);
    const auto end = deadline(5ms);
    size_t offset = 0;
    REQUIRE_THROWS_AS(
        [&] {
            while (offset < large.size()) {
                offset += stream->write_some(large.data() + offset, large.size() - offset, end, {});
            }
        }(),
        TimeoutError);
    (void)::close(pty.master);
    pty.master = -1;
    REQUIRE_THROWS_AS(stream->read_some(&byte, 1, deadline(), {}), IOError);
    REQUIRE_THROWS_AS(stream->write_some(&byte, 1, deadline(), {}), IOError);
}
#elif defined(_WIN32)
TEST_CASE("Serial Windows rejects non-COM paths and unavailable direction modes",
          "[serial][native-serial]") {
    Config config;
    config.port = "/dev/ttyUSB0";
    REQUIRE_THROWS_AS(open_serial(config), std::invalid_argument);
    config.port = "COM0";
    REQUIRE_THROWS_AS(open_serial(config), std::invalid_argument);
    config.port = "COM1x";
    REQUIRE_THROWS_AS(open_serial(config), std::invalid_argument);
    config.port = "COM1";
    config.rs485 = Rs485Mode::kKernel;
    REQUIRE_THROWS_AS(open_serial(config), std::invalid_argument);
    config.rs485 = Rs485Mode::kAdapterControlled;
    config.port = "COM4294967295";
    REQUIRE_THROWS_AS(open_serial(config), IOError);
}
#else
TEST_CASE("Serial native backend rejects unsupported platforms", "[serial]") {
    Config config;
    config.port = "fake";
    REQUIRE_THROWS_AS(open_serial(config), IOError);
}
#endif
