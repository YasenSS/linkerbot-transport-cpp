#include <catch2/catch_test_macros.hpp>

#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/message.hpp>

#include <cstdint>
#include <vector>

using linkerbot::can::FakeTransport;
using linkerbot::can::Message;

namespace {

    auto sense(uint32_t id, uint8_t cmd) -> Message {
        Message msg;
        msg.id = id;
        msg.dlc = 1;
        msg.data[0] = cmd;
        return msg;
    }

} // namespace

TEST_CASE("echo_cmd pads a sense request with fill bytes", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, 6, 0x11);

    REQUIRE_NOTHROW(transport.write(sense(0x28, 0x02)));

    auto reply = transport.read();
    REQUIRE(reply.has_value());
    REQUIRE(reply->id == 0x28);
    REQUIRE(reply->dlc == 7);
    REQUIRE(reply->data[0] == 0x02);
    for (size_t i = 1; i <= 6; ++i) {
        REQUIRE(reply->data[i] == 0x11);
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
    REQUIRE(reply->dlc == 7);
    REQUIRE(reply->data[1] == 10);
    REQUIRE(reply->data[6] == 60);
}

TEST_CASE("a mismatched command produces no reply", "[fake_transport]") {
    FakeTransport transport;
    transport.echo_cmd(0x28, 0x02, 4);

    REQUIRE_NOTHROW(transport.write(sense(0x28, 0x99))); // wrong cmd byte
    REQUIRE_FALSE(transport.read().has_value());

    REQUIRE_NOTHROW(transport.write(sense(0x27, 0x02))); // unregistered id
    REQUIRE_FALSE(transport.read().has_value());

    // Both writes were still recorded — `sent()` is the assertion surface even
    // when the device would not answer.
    REQUIRE(transport.sent().size() == 2);
}

TEST_CASE("sent history accumulates and clears", "[fake_transport]") {
    FakeTransport transport;

    REQUIRE_NOTHROW(transport.write(sense(0x28, 0x01)));
    REQUIRE_NOTHROW(transport.write(sense(0x28, 0x02)));
    REQUIRE(transport.sent().size() == 2);

    transport.clear_sent();
    REQUIRE(transport.sent().empty());
}

TEST_CASE("injected frames read back in order", "[fake_transport]") {
    FakeTransport transport;
    transport.inject(sense(0x28, 0xA1));
    transport.inject(sense(0x28, 0xA2));

    auto first = transport.read();
    auto second = transport.read();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->data[0] == 0xA1);
    REQUIRE(second->data[0] == 0xA2);
    REQUIRE_FALSE(transport.read().has_value());
}

TEST_CASE("close makes writes fail and drops injections", "[fake_transport]") {
    FakeTransport transport;
    transport.close();

    REQUIRE_THROWS_AS(transport.write(sense(0x28, 0x01)), linkerbot::can::ClosedError);
    transport.inject(sense(0x28, 0x02));
    REQUIRE_THROWS_AS(transport.read(), linkerbot::can::ClosedError);
}
