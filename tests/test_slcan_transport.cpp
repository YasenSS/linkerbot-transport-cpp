#include "optional_value.hpp"

#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <deque>
#include <future>
#include <linkerbot/can/slcan_transport.hpp>
#include <mutex>

using namespace linkerbot::can;
using namespace std::chrono_literals;
using linkerbot::test_support::require_value;

namespace {
    struct Serial final : SlcanSerialPort {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::string> chunks;
        std::vector<std::string> sent;
        std::string acknowledgement = "\r";
        bool ack = true, bell = false, fail_write = false, fail_close = false;
        bool closed = false, cancelled = false, block = false, entered = false;
        bool cancel_returns_empty = false, ignore_read_limit = false;
        size_t closes = 0;
        auto read_some(size_t limit, std::chrono::milliseconds timeout) -> std::string override {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            changed.notify_all();
            if (block) {
                changed.wait_for(lock, timeout, [&] { return cancelled || !chunks.empty(); });
            }
            if (cancelled && !cancel_returns_empty) {
                throw ClosedError("cancelled");
            }
            if (cancelled) {
                return {};
            }
            if (chunks.empty()) {
                return {};
            }
            auto result = ignore_read_limit ? chunks.front() : chunks.front().substr(0, limit);
            chunks.front().erase(0, result.size());
            if (chunks.front().empty()) {
                chunks.pop_front();
            }
            return result;
        }
        auto write_all(std::string_view bytes, std::chrono::milliseconds) -> void override {
            std::lock_guard<std::mutex> lock(mutex);
            if (fail_write) {
                throw TransportIOError("unplugged");
            }
            sent.emplace_back(bytes);
            if (bell) {
                chunks.emplace_back("\a");
            } else if (ack) {
                chunks.emplace_back(acknowledgement);
            }
        }
        auto cancel() noexcept -> void override {
            std::lock_guard<std::mutex> lock(mutex);
            cancelled = true;
            changed.notify_all();
        }
        auto close() -> void override {
            ++closes;
            if (fail_close) {
                throw TransportIOError("close failed");
            }
            closed = true;
        }
    };
    auto config() -> SlcanConfig {
        SlcanConfig c;
        c.port = "COM10";
        c.command_timeout = 10ms;
        return c;
    }
    auto frame() -> Message {
        Message m;
        m.id = 0x123;
        m.dlc = 2;
        m.data[0] = 0xab;
        m.data[1] = 0xcd;
        return m;
    }
} // namespace
TEST_CASE("SLCAN validates ports UART and CAN profiles before I/O", "[slcan]") {
    REQUIRE(slcan_port_name("COM10") == "COM10");
    REQUIRE(slcan_port_name("\\\\.\\COM123") == "COM123");
    REQUIRE(slcan_port_name("com65535") == "COM65535");
    for (const auto *name : {"COM0", "COM01", "COM65536", "COM-1", "COM1x", "/tmp/ttyS0",
                             "/dev/../tmp/ttyS0", "COM1\\x"}) {
        REQUIRE_THROWS_AS(slcan_port_name(name), ValidationError);
    }
    REQUIRE(slcan_port_name("/dev/ttyACM0") == "/dev/ttyACM0");
    REQUIRE(slcan_port_name("/dev/serial/by-id/adapter") == "/dev/serial/by-id/adapter");
    auto c = config();
    REQUIRE_NOTHROW(validate_slcan_config(c));
    c.serial_baud = 0;
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    c = config();
    c.nominal_bitrate = 123456;
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    c = config();
    c.fd = true;
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    c = config();
    c.brs = true;
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    c = config();
    // A scoped enum has fixed underlying type int; 99 is representable. Verify
    // that the public validator rejects an unknown protocol value.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    c.protocol = static_cast<SlcanProtocol>(99);
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    c = config();
    c.receive_timeout = 0ms;
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    c = config();
    c.command_timeout = 5001ms;
    REQUIRE_THROWS_AS(validate_slcan_config(c), ValidationError);
    REQUIRE(enumerate_slcan_ports({}).empty());
    REQUIRE_THROWS_AS(enumerate_slcan_ports({"COM10", "com10"}), ValidationError);
    REQUIRE_THROWS_AS(SlcanTransport(config(), {}), ValidationError);
}
#ifdef _WIN32
TEST_CASE("SLCAN native missing COM port preserves system diagnostics", "[slcan]") {
    // Metadata only. Never open a port that happens to exist on this runner.
    if (!enumerate_slcan_ports({"COM65535"}).empty())
        SKIP("Reserved test name exists on runner");
    auto c = config();
    c.port = "COM65535";
    try {
        (void)open_slcan_serial(c);
        FAIL("Missing serial port opened");
    } catch (const TransportIOError &error) {
        REQUIRE(error.native_status());
        REQUIRE(error.system_error().value() != 0);
    }
}
#endif
TEST_CASE("SLCAN Classic codecs preserve IDs and exact payloads", "[slcan]") {
    auto m = frame();
    REQUIRE(encode_slcan_frame(m) == "t1232ABCD\r");
    auto decoded = decode_slcan_frame("t1232abcd");
    REQUIRE(decoded.id == m.id);
    REQUIRE(decoded.dlc == 2);
    REQUIRE(decoded.data == m.data);
    m.is_extended = true;
    m.id = 0x1fffffff;
    m.dlc = 8;
    m.data.fill(0xff);
    REQUIRE(encode_slcan_frame(m) == "T1FFFFFFF8FFFFFFFFFFFFFFFF\r");
    decoded = decode_slcan_frame("T1FFFFFFF8FFFFFFFFFFFFFFFF");
    REQUIRE(decoded.id == m.id);
    REQUIRE(decoded.is_extended);
    REQUIRE(decoded.dlc == 8);
    REQUIRE_FALSE(decoded.is_fd);
    REQUIRE_FALSE(decoded.is_brs);
    REQUIRE(encode_slcan_frame(Message{}) == "t0000\r");
    for (const auto *line : {"", "r1230", "d1230", "t8000", "T200000000", "t1239", "t1232AB",
                             "t1231ABCD", "t12g0", "t1230FFFF"}) {
        REQUIRE_THROWS_AS(decode_slcan_frame(line), ValidationError);
    }
    REQUIRE_THROWS_AS(decode_slcan_frame("t12"), ValidationError);
    m = frame();
    m.is_fd = true;
    REQUIRE_THROWS_AS(encode_slcan_frame(m), ValidationError);
    m = frame();
    m.is_brs = true;
    REQUIRE_THROWS_AS(encode_slcan_frame(m), ValidationError);
    m = frame();
    m.dlc = 9;
    REQUIRE_THROWS_AS(encode_slcan_frame(m), ValidationError);
}
TEST_CASE("SLCAN maps each CAN bitrate independently of UART and accepts TX ACKs", "[slcan]") {
    const uint32_t rates[]{10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000};
    for (size_t i = 0; i < 9; ++i) {
        auto c = config();
        c.nominal_bitrate = rates[i];
        c.serial_baud = 230400;
        auto serial = std::make_shared<Serial>();
        SlcanTransport transport(c, serial);
        REQUIRE(serial->sent[1] == std::string{"S"} + static_cast<char>('0' + i) + '\r');
        for (const auto *ack : {"z\r", "Z\r", "\r"}) {
            serial->acknowledgement = ack;
            REQUIRE_NOTHROW(transport.write(frame()));
        }
    }
}
TEST_CASE("SLCAN startup ACKs and split coalesced frames", "[slcan]") {
    auto serial = std::make_shared<Serial>();
    SlcanTransport transport(config(), serial);
    REQUIRE(serial->sent == std::vector<std::string>{"C\r", "S8\r", "O\r"});
    serial->chunks = {"t12", "32ABCD\rt3210\r"};
    REQUIRE_FALSE(transport.read());
    auto m = transport.read();
    REQUIRE(m);
    REQUIRE(require_value(m).id == 0x123);
    REQUIRE(require_value(m).data[0] == 0xab);
    m = transport.read();
    REQUIRE(m);
    REQUIRE(require_value(m).id == 0x321);
    REQUIRE_FALSE(transport.read());
    serial->chunks = {"t4560\r"};
    transport.write(frame()); // Queues a real frame while awaiting the adapter ACK.
    REQUIRE(serial->sent.back() == "t1232ABCD\r");
    m = transport.read();
    REQUIRE(m);
    REQUIRE(require_value(m).id == 0x456);
    transport.close();
    REQUIRE(serial->closed);
    REQUIRE(serial->sent.back() == "C\r");
    REQUIRE_NOTHROW(transport.close());
    REQUIRE_THROWS_AS(transport.read(), ClosedError);
    REQUIRE_THROWS_AS(transport.write(frame()), ClosedError);
}
TEST_CASE("SLCAN reports malformed ASCII and bounds partial lines and queues", "[slcan]") {
    for (const auto &chunk : {std::string("\a"), std::string("t1232zzzz\r"), std::string("\n"),
                              std::string(27, 't'), std::string("t8000\r")}) {
        auto serial = std::make_shared<Serial>();
        SlcanTransport transport(config(), serial);
        serial->chunks = {chunk};
        REQUIRE_THROWS_AS(transport.read(), TransportIOError);
        REQUIRE_THROWS(transport.read()); // Terminal fault, no silent resync.
    }
    auto serial = std::make_shared<Serial>();
    SlcanTransport transport(config(), serial);
    serial->chunks = {"t123"};
    REQUIRE_FALSE(transport.read());
    REQUIRE_FALSE(transport.read());
    serial->chunks = {"0\r"};
    REQUIRE(require_value(transport.read()).id == 0x123);

    auto oversized = std::make_shared<Serial>();
    SlcanTransport oversized_transport(config(), oversized);
    oversized->ignore_read_limit = true;
    oversized->chunks = {std::string(257, 'x')};
    REQUIRE_THROWS_AS(oversized_transport.read(), TransportIOError);
}
TEST_CASE("SLCAN bounds queued frames while a command awaits ACK", "[slcan]") {
    auto serial = std::make_shared<Serial>();
    auto c = config();
    c.command_timeout = 1000ms;
    SlcanTransport transport(c, serial);
    serial->ack = false;
    for (int i = 0; i < 257; ++i) {
        serial->chunks.emplace_back("t1230\r");
    }
    REQUIRE_THROWS_AS(transport.write(frame()), QueueFullError);
    REQUIRE_THROWS_AS(transport.read(), QueueFullError);
}
TEST_CASE("SLCAN startup and TX failures never claim successful acceptance", "[slcan]") {
    SECTION("BEL") {
        auto serial = std::make_shared<Serial>();
        serial->bell = true;
        REQUIRE_THROWS_AS(SlcanTransport(config(), serial), TransportIOError);
        REQUIRE(serial->closed);
    }
    SECTION("timeout") {
        auto serial = std::make_shared<Serial>();
        serial->ack = false;
        REQUIRE_THROWS_AS(SlcanTransport(config(), serial), TimeoutError);
        REQUIRE(serial->closed);
        REQUIRE(serial->sent.back() == "C\r"); // O could have succeeded before a lost ACK.
    }
    SECTION("TX") {
        auto serial = std::make_shared<Serial>();
        SlcanTransport transport(config(), serial);
        serial->ack = false;
        REQUIRE_THROWS_AS(transport.write(frame()), TimeoutError);
        REQUIRE_THROWS(transport.write(frame())); // Never reuse an uncorrelated late ACK.
    }
    SECTION("disconnect") {
        auto serial = std::make_shared<Serial>();
        SlcanTransport transport(config(), serial);
        serial->fail_write = true;
        REQUIRE_THROWS_AS(transport.write(frame()), TransportIOError);
        REQUIRE_THROWS_AS(transport.close(), TransportIOError);
        REQUIRE(serial->closed); // Failed C never prevents local release.
        REQUIRE_NOTHROW(transport.close());
    }
    SECTION("write failure while unwinding startup") {
        auto serial = std::make_shared<Serial>();
        serial->fail_write = true;
        REQUIRE_THROWS_AS(SlcanTransport(config(), serial), TransportIOError);
        REQUIRE(serial->closed);
        REQUIRE(serial->closes == 1);
    }
}
TEST_CASE("SLCAN close stops an acknowledgement wait that returns no bytes", "[slcan]") {
    auto serial = std::make_shared<Serial>();
    SlcanTransport transport(config(), serial);
    {
        std::lock_guard<std::mutex> lock(serial->mutex);
        serial->ack = false;
        serial->block = true;
        serial->entered = false;
        serial->cancel_returns_empty = true;
    }
    auto writer = std::async(std::launch::async, [&] { transport.write(frame()); });
    {
        std::unique_lock<std::mutex> lock(serial->mutex);
        REQUIRE(serial->changed.wait_for(lock, 100ms, [&] { return serial->entered; }));
    }
    REQUIRE_NOTHROW(transport.close());
    REQUIRE_THROWS_AS(writer.get(), ClosedError);
}
TEST_CASE("SLCAN close interrupts a pending read", "[slcan]") {
    auto serial = std::make_shared<Serial>();
    auto c = config();
    c.receive_timeout = 100ms;
    SlcanTransport transport(c, serial);
    {
        std::lock_guard<std::mutex> lock(serial->mutex);
        serial->block = true;
        serial->entered = false;
    }
    auto reader = std::async(std::launch::async, [&] {
        try {
            transport.read();
        } catch (const ClosedError &) {
            return; // Expected when close interrupts the pending read.
        }
    });
    {
        std::unique_lock<std::mutex> lock(serial->mutex);
        REQUIRE(serial->changed.wait_for(lock, 100ms, [&] { return serial->entered; }));
    }
    REQUIRE_NOTHROW(transport.close());
    REQUIRE(reader.wait_for(100ms) == std::future_status::ready);
    reader.get();
}
TEST_CASE("SLCAN failed local close retains ownership for explicit retry", "[slcan]") {
    auto serial = std::make_shared<Serial>();
    SlcanTransport transport(config(), serial);
    serial->fail_close = true;
    REQUIRE_THROWS_AS(transport.close(), TransportIOError);
    REQUIRE_FALSE(serial->closed);
    serial->fail_close = false;
    REQUIRE_NOTHROW(transport.close());
    REQUIRE(serial->closed);
    REQUIRE(serial->closes == 2);
}
TEST_CASE("SLCAN constructor and destructor failures retain retryable resources", "[slcan]") {
    REQUIRE(pending_slcan_cleanup() == 0);
    auto serial = std::make_shared<Serial>();
    serial->fail_close = true;
    SECTION("constructor") {
        serial->bell = true;
        REQUIRE_THROWS(SlcanTransport(config(), serial));
    }
    SECTION("destructor") {
        SlcanTransport transport(config(), serial);
    }
    REQUIRE(pending_slcan_cleanup() == 1);
    REQUIRE_THROWS(retry_slcan_cleanup());
    REQUIRE(pending_slcan_cleanup() == 1);
    serial->fail_close = false;
    REQUIRE_NOTHROW(retry_slcan_cleanup());
    REQUIRE(pending_slcan_cleanup() == 0);
    REQUIRE(serial->closed);
}

TEST_CASE("SLCAN CANable FD codecs preserve flags DLC padding and all payload lengths",
          "[slcan][fd]") {
    for (const bool extended : {false, true}) {
        for (const bool brs : {false, true}) {
            for (unsigned length = 0; length <= 64; ++length) {
                auto m = frame();
                m.id = extended ? 0x1fffffff : 0x7ff;
                m.is_extended = extended;
                m.is_fd = true;
                m.is_brs = brs;
                m.dlc = static_cast<uint8_t>(length);
                for (size_t i = 0; i < length; ++i) {
                    m.data[i] = static_cast<uint8_t>(i + 1);
                }
                const auto wire = encode_slcan_frame(m, SlcanProtocol::kCanableFd);
                REQUIRE(wire[0] == (extended ? (brs ? 'B' : 'D') : (brs ? 'b' : 'd')));
                const auto decoded = decode_slcan_frame(
                    std::string_view{wire}.substr(0, wire.size() - 1), SlcanProtocol::kCanableFd);
                REQUIRE(decoded.id == m.id);
                REQUIRE(decoded.is_extended == extended);
                REQUIRE(decoded.is_fd);
                REQUIRE(decoded.is_brs == brs);
                REQUIRE(decoded.dlc >= length);
                for (size_t i = 0; i < decoded.dlc; ++i) {
                    REQUIRE(decoded.data[i] == (i < length ? i + 1 : 0));
                }
            }
        }
    }
    REQUIRE(encode_slcan_frame(frame(), SlcanProtocol::kCanableFd) == "t1232ABCD\r");
    REQUIRE_THROWS(decode_slcan_frame("d8000", SlcanProtocol::kCanableFd));
    REQUIRE_THROWS(decode_slcan_frame("D200000000", SlcanProtocol::kCanableFd));
    REQUIRE_THROWS(decode_slcan_frame("d123G", SlcanProtocol::kCanableFd));
    REQUIRE_THROWS(decode_slcan_frame("d1239AB", SlcanProtocol::kCanableFd));
    REQUIRE_THROWS(decode_slcan_frame("d1230FFFF", SlcanProtocol::kCanableFd));
}
TEST_CASE("SLCAN CANable FD is explicitly selected and never waits for nonexistent ACKs",
          "[slcan][fd]") {
    auto c = config();
    c.protocol = SlcanProtocol::kCanableFd;
    c.fd = true;
    for (const auto data : {2'000'000U, 5'000'000U}) {
        c.data_bitrate = data;
        auto serial = std::make_shared<Serial>();
        serial->ack = false;
        SlcanTransport transport(c, serial);
        REQUIRE(serial->sent == std::vector<std::string>{
                                    "C\r", "S8\r", data == 2'000'000 ? "Y2\r" : "Y5\r", "O\r"});
        auto m = frame();
        m.is_fd = true;
        m.is_brs = true;
        REQUIRE_NOTHROW(transport.write(m));
        REQUIRE(serial->sent.back() == "b1232ABCD\r");
        REQUIRE_NOTHROW(transport.write(frame()));
        REQUIRE(serial->sent.back() == "t1232ABCD\r");
        m.dlc = 64;
        m.is_extended = true;
        const auto wire = encode_slcan_frame(m, SlcanProtocol::kCanableFd);
        serial->chunks = {wire.substr(0, 25), wire.substr(25)};
        REQUIRE_FALSE(transport.read());
        const auto decoded = transport.read();
        REQUIRE(decoded);
        REQUIRE(require_value(decoded).dlc == 64);
        REQUIRE(require_value(decoded).is_extended);
        REQUIRE(require_value(decoded).is_brs);
        REQUIRE_NOTHROW(transport.close());
    }
    c.nominal_bitrate = 750000;
    REQUIRE_NOTHROW(validate_slcan_config(c));
    c.nominal_bitrate = 800000;
    REQUIRE_THROWS(validate_slcan_config(c));
    c.nominal_bitrate = 1000000;
    c.data_bitrate = 4000000;
    REQUIRE_THROWS(validate_slcan_config(c));
    c.data_bitrate = 5000000;
    c.startup_delay = -1ms;
    REQUIRE_THROWS(validate_slcan_config(c));
    c.startup_delay = 5001ms;
    REQUIRE_THROWS(validate_slcan_config(c));
}
TEST_CASE("SLCAN CANable Classic keeps FD disabled without requiring serial ACKs", "[slcan][fd]") {
    auto c = config();
    c.protocol = SlcanProtocol::kCanableFd;
    auto serial = std::make_shared<Serial>();
    serial->ack = false;
    SlcanTransport transport(c, serial);
    REQUIRE(serial->sent == std::vector<std::string>{"C\r", "S8\r", "O\r"});
    auto m = frame();
    m.is_fd = true;
    REQUIRE_THROWS_AS(transport.write(m), ValidationError);
    REQUIRE_NOTHROW(transport.write(frame()));
    serial->chunks = {"d1230\r"};
    REQUIRE_THROWS_AS(transport.read(), TransportIOError);
}

TEST_CASE("SLCAN rejects malformed device paths and unknown profiles at codec entry points",
          "[slcan][validation]") {
    for (const auto &port :
         {std::string{}, std::string{"COM\n10"}, std::string{"/dev/"}, std::string{"/dev/tty/"}}) {
        REQUIRE_THROWS_AS(slcan_port_name(port), ValidationError);
    }
    // Unknown but representable scoped-enum values must be rejected at the API.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto unknown = static_cast<SlcanProtocol>(99);
    REQUIRE_THROWS_AS(encode_slcan_frame(frame(), unknown), ValidationError);
    REQUIRE_THROWS_AS(decode_slcan_frame("t1230", unknown), ValidationError);
}

TEST_CASE("SLCAN explicit formats preserve classic defaults and reject unsupported echo",
          "[slcan]") {
    Message message;
    message.id = 0x123;
    CHECK(encode_slcan_frame(message, SlcanProtocol::kCanableFd) == "t1230\r");
    message.format = FrameFormat::kClassic;
    CHECK(encode_slcan_frame(message, SlcanProtocol::kCanableFd) == "t1230\r");
    message.format = FrameFormat::kFd;
    CHECK(encode_slcan_frame(message, SlcanProtocol::kCanableFd) == "d1230\r");
    REQUIRE_THROWS_AS(encode_slcan_frame(message), ValidationError);
    message.format = static_cast<FrameFormat>(-1);
    REQUIRE_THROWS_AS(encode_slcan_frame(message, SlcanProtocol::kCanableFd), ValidationError);
    SlcanConfig config;
    config.port = "COM1";
    config.receive_own_messages = true;
    REQUIRE_THROWS_AS(validate_slcan_config(config), ValidationError);
}
