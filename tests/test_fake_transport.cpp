#include <catch2/catch_test_macros.hpp>

#include "optional_value.hpp"

#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/message.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

using linkerbot::can::FakeTransport;
using linkerbot::can::Message;

using linkerbot::test_support::require_value;

namespace {

    auto sense(uint32_t id, std::byte cmd) -> Message {
        Message msg;
        msg.id = id;
        msg.dlc = 1;
        msg.data[0] = std::to_integer<uint8_t>(cmd);
        return msg;
    }

} // namespace

TEST_CASE("echo_cmd pads a sense request with fill bytes", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, 6, 0x11);

    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x02})));

    auto reply = transport.read();
    REQUIRE(reply.has_value());
    REQUIRE(require_value(reply).id == 0x28);
    REQUIRE(require_value(reply).dlc == 7);
    REQUIRE(require_value(reply).data[0] == 0x02);
    for (size_t i = 1; i <= 6; ++i) {
        REQUIRE(require_value(reply).data[i] == 0x11);
    }
}

TEST_CASE("echo_cmd mirrors a control payload back", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x01, 6);

    Message control;
    control.id = 0x28;
    control.dlc = 7;
    control.data[0] = 0x01;
    for (uint8_t i = 1; i <= 6; ++i) {
        control.data[i] = static_cast<uint8_t>(i * 10);
    }
    REQUIRE_NOTHROW(transport.write(control));

    auto reply = transport.read();
    REQUIRE(reply.has_value());
    REQUIRE(require_value(reply).dlc == 7);
    REQUIRE(require_value(reply).data[1] == 10);
    REQUIRE(require_value(reply).data[6] == 60);
}

TEST_CASE("echo_cmd fills a control payload when the request is shorter than the reply",
          "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x01, 6, 0xEE);

    Message partial;
    partial.id = 0x28;
    partial.dlc = 3;
    partial.data[0] = 0x01;
    partial.data[1] = 0x10;
    partial.data[2] = 0x20;
    REQUIRE_NOTHROW(transport.write(partial));

    const auto reply = transport.read();
    REQUIRE(reply.has_value());
    REQUIRE(require_value(reply).dlc == 7);
    REQUIRE(require_value(reply).data[1] == 0x10);
    REQUIRE(require_value(reply).data[2] == 0x20);
    for (size_t i = 3; i <= 6; ++i) {
        REQUIRE(require_value(reply).data[i] == 0xEE);
    }
}

TEST_CASE("echo_cmd accepts the largest reply that fits one CAN FD frame", "[fake_transport]") {
    FakeTransport transport;
    REQUIRE_NOTHROW(transport.echo_cmd(0x28, 0x02, linkerbot::can::kMaxDataLen - 1, 0xA5));

    auto request = sense(0x28, std::byte{0x02});
    request.is_fd = true;
    REQUIRE_NOTHROW(transport.write(request));

    const auto reply = transport.read();
    REQUIRE(reply.has_value());
    REQUIRE(require_value(reply).dlc == linkerbot::can::kMaxDataLen);
    REQUIRE(require_value(reply).is_fd);
    REQUIRE(require_value(reply).data.front() == 0x02);
    REQUIRE(require_value(reply).data.back() == 0xA5);
}

TEST_CASE("echo_cmd rejects reply lengths that cannot fit in a CAN FD frame", "[fake_transport]") {
    FakeTransport transport;

    REQUIRE_THROWS_AS(transport.echo_cmd(0x28, 0x02, linkerbot::can::kMaxDataLen),
                      linkerbot::can::ValidationError);
    REQUIRE_THROWS_AS(transport.echo_cmd(0x28, 0x02, 128), linkerbot::can::ValidationError);

    // A failed registration must not replace a previously configured handler.
    transport.echo_cmd(0x28, 0x02, 1, 0x7B);
    REQUIRE_THROWS_AS(transport.echo_cmd(0x28, 0x02, linkerbot::can::kMaxDataLen),
                      linkerbot::can::ValidationError);
    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x02})));
    const auto reply = transport.read();
    REQUIRE(reply.has_value());
    REQUIRE(require_value(reply).dlc == 2);
    REQUIRE(require_value(reply).data[1] == 0x7B);
}

TEST_CASE("echo_cmd rejects malformed request lengths before copying payload", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x01, 6);

    auto malformed = sense(0x28, std::byte{0x01});
    malformed.is_fd = true;
    malformed.dlc = static_cast<uint8_t>(linkerbot::can::kMaxDataLen + 1);
    REQUIRE_THROWS_AS(transport.write(malformed), linkerbot::can::ValidationError);
    REQUIRE_FALSE(transport.read().has_value());
}

TEST_CASE("echo_cmd enforces classic CAN request and reply payload limits", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, 7, 0x11);

    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x02})));
    const auto maximum = transport.read();
    REQUIRE(maximum.has_value());
    REQUIRE(require_value(maximum).dlc == 8);

    transport.echo_cmd(0x28, 0x02, 8, 0x11);
    REQUIRE_THROWS_AS(transport.write(sense(0x28, std::byte{0x02})),
                      linkerbot::can::ValidationError);

    auto oversized_request = sense(0x28, std::byte{0x02});
    oversized_request.dlc = 9;
    REQUIRE_THROWS_AS(transport.write(oversized_request), linkerbot::can::ValidationError);
}

TEST_CASE("echo_cmd ignores a classic command that does not select its large FD reply",
          "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, linkerbot::can::kMaxDataLen - 1, 0xA5);

    // The configured reply requires CAN FD, but a different command does not
    // select that reply and must retain the established no-response behavior.
    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x99})));
    REQUIRE_FALSE(transport.read().has_value());

    // Matching the handler from a classic frame still rejects the oversized
    // reply rather than truncating it or silently upgrading the frame flags.
    REQUIRE_THROWS_AS(transport.write(sense(0x28, std::byte{0x02})),
                      linkerbot::can::ValidationError);
    REQUIRE_FALSE(transport.read().has_value());
}

TEST_CASE("echo_cmd preserves valid classic CAN FD and BRS frame flags", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, 6, 0x11);

    auto classic = sense(0x28, std::byte{0x02});
    classic.is_extended = true;
    REQUIRE_NOTHROW(transport.write(classic));
    const auto classic_reply = transport.read();
    REQUIRE(classic_reply.has_value());
    REQUIRE_FALSE(require_value(classic_reply).is_fd);
    REQUIRE(require_value(classic_reply).is_extended);
    REQUIRE(require_value(classic_reply).dlc == 7);

    auto fd = sense(0x28, std::byte{0x02});
    fd.is_fd = true;
    fd.is_brs = true;
    REQUIRE_NOTHROW(transport.write(fd));
    const auto fd_reply = transport.read();
    REQUIRE(fd_reply.has_value());
    REQUIRE(require_value(fd_reply).is_fd);
    REQUIRE(require_value(fd_reply).is_brs);
    REQUIRE_FALSE(require_value(fd_reply).is_extended);
    REQUIRE(require_value(fd_reply).dlc == 7);

    auto invalid_brs = sense(0x28, std::byte{0x02});
    invalid_brs.is_brs = true;
    REQUIRE_THROWS_AS(transport.write(invalid_brs), linkerbot::can::ValidationError);
}

TEST_CASE("a mismatched command produces no reply", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, 4);

    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x99}))); // wrong cmd byte
    REQUIRE_FALSE(transport.read().has_value());

    REQUIRE_NOTHROW(transport.write(sense(0x27, std::byte{0x02}))); // unregistered id
    REQUIRE_FALSE(transport.read().has_value());

    // Both writes were still recorded — `sent()` is the assertion surface even
    // when the device would not answer.
    REQUIRE(transport.sent().size() == 2);
}

TEST_CASE("sent history accumulates and clears", "[fake_transport]") {
    FakeTransport transport;

    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x01})));
    REQUIRE_NOTHROW(transport.write(sense(0x28, std::byte{0x02})));
    REQUIRE(transport.sent().size() == 2);

    transport.clear_sent();
    REQUIRE(transport.sent().empty());
}

TEST_CASE("injected frames read back in order", "[fake_transport]") {
    FakeTransport transport;
    transport.inject(sense(0x28, std::byte{0xA1}));
    transport.inject(sense(0x28, std::byte{0xA2}));

    auto first = transport.read();
    auto second = transport.read();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(require_value(first).data[0] == 0xA1);
    REQUIRE(require_value(second).data[0] == 0xA2);
    REQUIRE_FALSE(transport.read().has_value());
}

TEST_CASE("close makes writes fail and drops injections", "[fake_transport]") {
    FakeTransport transport;
    transport.close();

    REQUIRE_THROWS_AS(transport.write(sense(0x28, std::byte{0x01})), linkerbot::can::ClosedError);
    transport.inject(sense(0x28, std::byte{0x02}));
    REQUIRE_THROWS_AS(transport.read(), linkerbot::can::ClosedError);
}

TEST_CASE("Fake transport rejects direct operations after close and saved write errors", "[fake]") {
    linkerbot::can::FakeTransport fake;
    linkerbot::can::Message frame;
    frame.id = 1;
    fake.write_batch({});
    fake.fail_writes("injected write failure");
    REQUIRE_THROWS_AS(fake.write_batch({frame}), linkerbot::can::TransportIOError);
    fake.close();
    REQUIRE_THROWS_AS(fake.read(), linkerbot::can::ClosedError);
    REQUIRE_THROWS_AS(fake.write(frame), linkerbot::can::ClosedError);
    REQUIRE_THROWS_AS(fake.write_batch({frame}), linkerbot::can::ClosedError);
}

TEST_CASE("Fake batch replies wake a waiting reader", "[fake]") {
    linkerbot::can::FakeTransport fake;
    fake.echo_cmd(0x20, 1, 1, 7);
    linkerbot::can::Message frame;
    frame.id = 0x20;
    frame.dlc = 1;
    frame.data[0] = 1;
    fake.write_batch({frame});
    const auto reply = fake.read();
    REQUIRE(reply.has_value());
    REQUIRE(reply->data[1] == 7);
}

TEST_CASE("Fake read failure wakes an idle receiver", "[fake][concurrency]") {
    FakeTransport fake;
    std::promise<void> starting;
    auto reader = std::async(std::launch::async, [&] {
        starting.set_value();
        for (;;) {
            (void)fake.read();
        }
    });
    starting.get_future().wait();
    // Leave the reader idle in its bounded condition-variable wait before
    // injecting the error. Both a concurrent and an already-published error
    // must reach the caller rather than returning an empty successful read.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    fake.fail_reads("injected idle read failure");
    REQUIRE_THROWS_AS(reader.get(), linkerbot::can::TransportIOError);
}
