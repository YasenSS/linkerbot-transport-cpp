#include <catch2/catch_test_macros.hpp>
#include <linkerbot/modbus/rtu.hpp>

#include "rtu_test_stream.hpp"

using namespace linkerbot;
using namespace linkerbot::modbus;
using test_support::with_crc;

TEST_CASE("RTU CRC and request vectors preserve wire byte order", "[modbus][codec]") {
    const Bytes vector{0x01, 0x03, 0x00, 0x00, 0x00, 0x0a};
    REQUIRE(crc16(vector.data(), vector.size()) == 0xcdc5);
    REQUIRE(crc16(nullptr, 0) == 0xffff);
    REQUIRE_THROWS_AS(crc16(nullptr, 1), std::invalid_argument);
    REQUIRE(encode_request({1, 3, 0, 10, {}}) == Bytes{1, 3, 0, 0, 0, 10, 0xc5, 0xcd});
    REQUIRE(encode_request({1, 6, 0x1234, 1, {0xabcd}}) ==
            with_crc({1, 6, 0x12, 0x34, 0xab, 0xcd}));
    REQUIRE(encode_request({7, 16, 0x2345, 2, {0x0102, 0xfedc}}) ==
            with_crc({7, 16, 0x23, 0x45, 0, 2, 4, 1, 2, 0xfe, 0xdc}));
}
TEST_CASE("RTU rejects unsupported functions broadcast and register overflow", "[modbus][codec]") {
    Request valid{1, 3, 0, 1, {}};
    for (const uint8_t unit : {uint8_t{0}, uint8_t{248}, uint8_t{255}}) {
        auto request = valid;
        request.unit_id = unit;
        REQUIRE_THROWS_AS(encode_request(request), std::invalid_argument);
    }
    for (const uint8_t function : {uint8_t{0}, uint8_t{1}, uint8_t{5}, uint8_t{23}}) {
        auto request = valid;
        request.function = function;
        REQUIRE_THROWS_AS(encode_request(request), std::invalid_argument);
    }
    for (const auto count : {uint16_t{0}, uint16_t{126}}) {
        auto request = valid;
        request.quantity = count;
        REQUIRE_THROWS_AS(encode_request(request), std::invalid_argument);
    }
    REQUIRE_THROWS_AS(encode_request({1, 3, 65535, 2, {}}), std::invalid_argument);
    REQUIRE_THROWS_AS(encode_request({1, 3, 0, 1, {1}}), std::invalid_argument);
    REQUIRE_THROWS_AS(encode_request({1, 6, 0, 2, {1, 2}}), std::invalid_argument);
    REQUIRE_THROWS_AS(encode_request({1, 16, 0, 1, {}}), std::invalid_argument);
    REQUIRE_THROWS_AS(encode_request({1, 16, 0, 124, std::vector<uint16_t>(124)}),
                      std::invalid_argument);
    REQUIRE_NOTHROW(encode_request({247, 4, 65535, 1, {}}));
    REQUIRE(encode_request({1, 16, 0, 123, std::vector<uint16_t>(123)}).size() == 255);
}
TEST_CASE("RTU validates read lengths CRC unit and function", "[modbus][codec]") {
    const Request request{1, 3, 7, 2, {}};
    REQUIRE(decode_response(request, with_crc({1, 3, 4, 0x12, 0x34, 0xab, 0xcd})) ==
            Bytes{0x12, 0x34, 0xab, 0xcd});
    for (const auto &frame : std::vector<Bytes>{{},
                                                Bytes(257),
                                                with_crc({2, 3, 4, 0, 1, 0, 2}),
                                                with_crc({1, 4, 4, 0, 1, 0, 2}),
                                                with_crc({1, 3, 2, 0, 1}),
                                                with_crc({1, 3, 3, 0, 1, 0, 2})}) {
        REQUIRE_THROWS_AS(decode_response(request, frame), ProtocolError);
    }
    auto bad = with_crc({1, 3, 4, 0, 1, 0, 2});
    bad.back() ^= 1;
    REQUIRE_THROWS_AS(decode_response(request, bad), CrcError);
    auto largest = Bytes{1, 4, 250};
    largest.resize(253);
    REQUIRE(decode_response({1, 4, 0, 125, {}}, with_crc(largest)).size() == 250);
    // Arbitrary 256-byte input is bounded and rejected rather than truncated to uint8_t.
    REQUIRE_THROWS_AS(decode_response(request, with_crc(Bytes(254))), ProtocolError);
}
TEST_CASE("RTU validates write echoes and retains device exception codes", "[modbus][codec]") {
    const Request single{3, 6, 0x1234, 1, {0xabcd}};
    REQUIRE(decode_response(single, encode_request(single)) == Bytes{0x12, 0x34, 0xab, 0xcd});
    REQUIRE_THROWS_AS(decode_response(single, with_crc({3, 6, 0x12, 0x35, 0xab, 0xcd})),
                      ProtocolError);
    REQUIRE_THROWS_AS(decode_response(single, with_crc({3, 6, 0x12, 0x34, 0xab, 0xce})),
                      ProtocolError);
    const Request multiple{3, 16, 0x1234, 2, {0xabcd, 0x5678}};
    REQUIRE(decode_response(multiple, with_crc({3, 16, 0x12, 0x34, 0, 2})) ==
            Bytes{0x12, 0x34, 0, 2});
    REQUIRE_THROWS_AS(decode_response(multiple, with_crc({3, 16, 0x12, 0x34, 0, 3})),
                      ProtocolError);
    try {
        (void)decode_response(single, with_crc({3, 0x86, 2}));
        FAIL("Exception reply accepted");
    } catch (const ExceptionResponse &error) {
        REQUIRE(error.code() == 2);
    }
    REQUIRE_THROWS_AS(decode_response(single, with_crc({3, 0x86, 0})), ProtocolError);
    REQUIRE_THROWS_AS(decode_response(single, with_crc({3, 0x86, 2, 1})), ProtocolError);
    REQUIRE_THROWS_AS(decode_response(single, with_crc({3, 0x83, 2})), ProtocolError);
}
TEST_CASE("RTU timing uses configured character width and high speed fixed intervals",
          "[modbus][codec]") {
    serial::Config config;
    config.port = "fake";
    config.baud_rate = 9600;
    REQUIRE(rtu_timing(config).inter_character == std::chrono::microseconds{1719});
    REQUIRE(rtu_timing(config).inter_frame == std::chrono::microseconds{4011});
    config.parity = serial::Parity::kNone;
    config.stop_bits = serial::StopBits::kTwo;
    REQUIRE(rtu_timing(config).inter_frame == std::chrono::microseconds{4011});
    config.stop_bits = serial::StopBits::kOne;
    REQUIRE(rtu_timing(config).inter_frame == std::chrono::microseconds{3646});
    config.baud_rate = 19200;
    REQUIRE(rtu_timing(config).inter_frame == std::chrono::microseconds{1823});
    config.baud_rate = 115200;
    REQUIRE(rtu_timing(config).inter_character == std::chrono::microseconds{750});
    REQUIRE(rtu_timing(config).inter_frame == std::chrono::microseconds{1750});
    config.data_bits = 7;
    REQUIRE_THROWS_AS(rtu_timing(config), std::invalid_argument);
}
