#include <catch2/catch_test_macros.hpp>
#include <future>

#include "rtu_test_stream.hpp"

using namespace linkerbot;
using namespace linkerbot::modbus;
using namespace linkerbot::test_support;
using namespace std::chrono_literals;
namespace {
    const Request kRead{1, 3, 0x100, 2, {}};
    void await_writes(const std::shared_ptr<RtuWire> &wire) {
        std::unique_lock<std::mutex> lock(wire->mutex);
        REQUIRE(wire->changed.wait_for(lock, 1s, [&] { return wire->writes > 0; }));
    }
} // namespace
TEST_CASE("RTU Client reads and writes through fragmented byte streams", "[modbus][session]") {
    auto wire = std::make_shared<RtuWire>();
    wire->read_chunk = 1;
    wire->write_chunk = 2;
    wire->reply = normal_reply;
    auto session = make_session(wire);
    Client client(session, 1);
    REQUIRE(client.read_holding_registers(0x100, 2, options()) ==
            std::vector<uint16_t>{0x1230, 0x1231});
    REQUIRE(client.read_input_registers(0x100, 1, options()) == std::vector<uint16_t>{0x1230});
    const auto single = client.write_single_register(0x100, 0xabcd, options());
    REQUIRE(single.outcome == Outcome::kResponse);
    REQUIRE(single.request_dispatched);
    REQUIRE_FALSE(single.delivery_uncertain);
    REQUIRE(single.bytes_sent == 8);
    REQUIRE(single.completed_at >= single.accepted_at);
    REQUIRE_NOTHROW(single.rethrow_if_error());
    REQUIRE(client.write_multiple_registers(0x100, {1, 0xabcd}, options()).outcome ==
            Outcome::kResponse);
    REQUIRE(session->health().transactions == 4);
    REQUIRE_FALSE(session->health().faulted);
    REQUIRE_THROWS_AS(Client(nullptr, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(Client(session, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(client.write_multiple_registers(0, {}, options()), std::invalid_argument);
    REQUIRE_THROWS_AS(client.write_multiple_registers(0, std::vector<uint16_t>(124), options()),
                      std::invalid_argument);
    REQUIRE_NOTHROW(session->close());
    REQUIRE_NOTHROW(session->close());
    REQUIRE_THROWS_AS(session->transact(kRead, options()), serial::ClosedError);
    REQUIRE_THROWS_AS(session->recover(1), serial::ClosedError);
}
TEST_CASE("RTU factory and admission validation do not write bytes", "[modbus][session]") {
    auto config = rtu_config();
    config.queue_capacity = 0;
    REQUIRE_THROWS_AS(make_session(std::make_shared<RtuWire>(), config), std::invalid_argument);
    REQUIRE_THROWS_AS(RtuSession::create(rtu_config(), {}), std::invalid_argument);
    REQUIRE_THROWS_AS(
        RtuSession::create(
            rtu_config(),
            [](const serial::Config &) -> std::unique_ptr<serial::ByteStream> { return nullptr; }),
        std::invalid_argument);
    auto wire = std::make_shared<RtuWire>();
    auto session = make_session(wire);
    REQUIRE_THROWS_AS(session->transact(kRead, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(session->transact(kRead, options(-1ms)), serial::TimeoutError);
    transport::CancellationSource cancellation;
    cancellation.cancel();
    REQUIRE_THROWS_AS(
        session->transact(kRead, {transport::Clock::now() + 1s, cancellation.token()}),
        transport::CancelledError);
    REQUIRE(wire->writes == 0);
}
TEST_CASE("RTU device exceptions preserve the line for the next transaction", "[modbus][session]") {
    auto wire = std::make_shared<RtuWire>();
    wire->reply = [](const Bytes &request) {
        return with_crc({request[0], static_cast<uint8_t>(request[1] | 0x80U), 2});
    };
    auto session = make_session(wire);
    auto result = session->transact(kRead, options());
    REQUIRE(result.outcome == Outcome::kException);
    REQUIRE_FALSE(result.delivery_uncertain);
    REQUIRE_THROWS_AS(result.rethrow_if_error(), ExceptionResponse);
    REQUIRE(session->health().exceptions == 1);
    REQUIRE_FALSE(session->health().faulted);
    wire->reply = normal_reply;
    REQUIRE(session->transact(kRead, options()).outcome == Outcome::kResponse);
}
TEST_CASE("RTU failure after transmission faults the line without retrying", "[modbus][session]") {
    for (int scenario = 0; scenario < 8; ++scenario) {
        auto wire = std::make_shared<RtuWire>();
        if (scenario == 0) {
            wire->reply = [](const Bytes &) { return Bytes{}; };
        }
        if (scenario == 1) {
            wire->fail_write = true;
        }
        if (scenario == 2) {
            wire->zero_write = true;
        }
        if (scenario == 3) {
            wire->oversized_write = true;
        }
        if (scenario == 4) {
            wire->reply = [](const Bytes &) {
                auto b = with_crc({1, 3, 4, 0, 1, 0, 2});
                b.back() ^= 1;
                return b;
            };
        }
        if (scenario == 5) {
            wire->reply = [](const Bytes &) { return with_crc({2, 3, 4, 0, 1, 0, 2}); };
        }
        if (scenario == 6) {
            wire->reply = [](const Bytes &) { return Bytes{1, 3, 255}; };
        }
        if (scenario == 7) {
            wire->reply = normal_reply;
            wire->oversized_read = true;
        }
        auto session = make_session(wire);
        const auto result = session->transact(kRead, options(20ms));
        REQUIRE(result.error);
        REQUIRE(result.delivery_uncertain);
        REQUIRE(session->health().faulted);
        REQUIRE(wire->writes == 1);
        REQUIRE_THROWS_AS(session->transact(kRead, options()), FaultedError);
        REQUIRE_THROWS(result.rethrow_if_error());
        if (scenario == 0) {
            REQUIRE(result.outcome == Outcome::kExpired);
            REQUIRE(session->health().timeouts == 1);
        }
        if (scenario == 4) {
            REQUIRE(session->health().crc_errors == 1);
        }
    }
}
TEST_CASE("RTU rejects stale input and does not consume it as a fresh reply", "[modbus][session]") {
    auto wire = std::make_shared<RtuWire>();
    wire->input.push_back(1);
    auto session = make_session(wire);
    const auto result = session->transact(kRead, options());
    REQUIRE(result.outcome == Outcome::kFailed);
    REQUIRE_FALSE(result.delivery_uncertain);
    REQUIRE(wire->writes == 0);
    REQUIRE(session->health().faulted);
}
TEST_CASE("RTU admission budget includes quiet interval and does not fault an unsent request",
          "[modbus][session]") {
    auto wire = std::make_shared<RtuWire>();
    auto session = make_session(wire);
    const auto result = session->transact(kRead, options(1ms));
    REQUIRE(result.outcome == Outcome::kExpired);
    REQUIRE_FALSE(result.delivery_uncertain);
    REQUIRE_FALSE(session->health().faulted);
    REQUIRE(wire->writes == 0);
}
TEST_CASE("RTU local echo requires an explicit matching adapter profile", "[modbus][session]") {
    auto config = rtu_config();
    config.discard_local_echo = true;
    auto wire = std::make_shared<RtuWire>();
    wire->reply = [](const Bytes &request) {
        auto response = request;
        const auto reply = normal_reply(request);
        response.insert(response.end(), reply.begin(), reply.end());
        return response;
    };
    auto session = make_session(wire, config);
    Client client(session, 1);
    REQUIRE(client.write_single_register(0, 0x1234, options()).outcome == Outcome::kResponse);
    wire->reply = normal_reply;
    const auto result = session->transact(kRead, options(20ms));
    REQUIRE(result.error);
    REQUIRE(session->health().faulted);
}
TEST_CASE("RTU shared units serialize and bound waiting transactions",
          "[modbus][session][concurrency]") {
    auto config = rtu_config();
    config.queue_capacity = 1;
    auto wire = std::make_shared<RtuWire>();
    wire->block_drain = true;
    wire->reply = normal_reply;
    auto session = make_session(wire, config);
    auto first =
        std::async(std::launch::async, [&] { return session->transact(kRead, options(2s)); });
    await_writes(wire);
    auto second = std::async(std::launch::async,
                             [&] { return session->transact({2, 3, 0, 1, {}}, options(1s)); });
    const auto wait = transport::Clock::now() + 1s;
    while (session->health().pending != 1 && transport::Clock::now() < wait) {
        std::this_thread::yield();
    }
    const auto pending = session->health().pending;
    bool full = false;
    try {
        (void)session->transact(kRead, options());
    } catch (const QueueFullError &) {
        full = true;
    }
    {
        const std::lock_guard<std::mutex> lock(wire->mutex);
        wire->block_drain = false;
        wire->changed.notify_all();
    }
    REQUIRE(first.get().outcome == Outcome::kResponse);
    REQUIRE(second.get().outcome == Outcome::kResponse);
    REQUIRE(pending == 1);
    REQUIRE(full);
    REQUIRE(wire->sent.size() == 16);
    REQUIRE(wire->sent[0] == 1);
    REQUIRE(wire->sent[8] == 2);
}
TEST_CASE("RTU queued cancellation and timeout never send the queued request",
          "[modbus][session][concurrency]") {
    auto wire = std::make_shared<RtuWire>();
    wire->block_drain = true;
    wire->reply = normal_reply;
    auto session = make_session(wire);
    auto first =
        std::async(std::launch::async, [&] { return session->transact(kRead, options(2s)); });
    await_writes(wire);
    REQUIRE_THROWS_AS(session->transact(kRead, options(5ms)), serial::TimeoutError);
    transport::CancellationSource cancellation;
    auto queued = std::async(std::launch::async, [&] {
        try {
            (void)session->transact(kRead, {transport::Clock::now() + 1s, cancellation.token()});
        } catch (const transport::CancelledError &) {
            return true;
        }
        return false;
    });
    cancellation.cancel();
    REQUIRE(queued.get());
    {
        const std::lock_guard<std::mutex> lock(wire->mutex);
        wire->block_drain = false;
        wire->changed.notify_all();
    }
    REQUIRE(first.get().outcome == Outcome::kResponse);
    REQUIRE(wire->writes == 1);
}
TEST_CASE("RTU active cancellation records uncertain delivery and requires recovery",
          "[modbus][session][concurrency]") {
    auto wire = std::make_shared<RtuWire>();
    wire->block_drain = true;
    auto session = make_session(wire);
    transport::CancellationSource cancellation;
    auto active = std::async(std::launch::async, [&] {
        return session->transact(kRead, {transport::Clock::now() + 1s, cancellation.token()});
    });
    await_writes(wire);
    cancellation.cancel();
    const auto result = active.get();
    REQUIRE(result.outcome == Outcome::kCancelled);
    REQUIRE(result.delivery_uncertain);
    REQUIRE(session->health().faulted);
    REQUIRE(wire->writes == 1);
}
TEST_CASE("RTU close wakes active and waiting operations", "[modbus][session][concurrency]") {
    auto wire = std::make_shared<RtuWire>();
    wire->block_drain = true;
    auto session = make_session(wire);
    auto active =
        std::async(std::launch::async, [&] { return session->transact(kRead, options(2s)); });
    await_writes(wire);
    auto queued = std::async(std::launch::async, [&] {
        try {
            (void)session->transact(kRead, options(2s));
        } catch (const serial::ClosedError &) {
            return true;
        }
        return false;
    });
    session->close();
    REQUIRE(active.get().error);
    REQUIRE(queued.get());
    REQUIRE_FALSE(session->health().open);
}
TEST_CASE("RTU recovery advances generation without replay and leaves failures faulted",
          "[modbus][session][concurrency]") {
    auto old = std::make_shared<RtuWire>();
    auto next = std::make_shared<RtuWire>();
    next->reply = normal_reply;
    std::atomic<unsigned> calls{0};
    auto session = RtuSession::create(
        rtu_config(), [&](const serial::Config &) -> std::unique_ptr<serial::ByteStream> {
            const auto call = ++calls;
            if (call == 3) {
                throw serial::IOError("reopen failed");
            }
            if (call == 4) {
                return nullptr;
            }
            return std::make_unique<RtuStream>(call == 1 ? old : next);
        });
    REQUIRE(session->transact(kRead, options(10ms)).error);
    REQUIRE(session->recover(1) == 2);
    REQUIRE(old->closed);
    REQUIRE(next->writes == 0);
    REQUIRE(session->recover(1) == 2);
    REQUIRE(calls == 2);
    REQUIRE(session->transact(kRead, options()).outcome == Outcome::kResponse);
    REQUIRE_THROWS_AS(session->recover(2), serial::IOError);
    REQUIRE(session->health().faulted);
    REQUIRE_FALSE(session->health().recovering);
    REQUIRE_THROWS_AS(session->recover(2), std::invalid_argument);
    auto replacement = std::make_shared<RtuWire>();
    replacement->reply = normal_reply;
    // The next stream must be newly opened rather than reuse a cancelled stream.
    next = replacement;
    REQUIRE(session->recover(2) == 3);
    REQUIRE(session->health().recoveries == 2);
    REQUIRE(session->transact(kRead, options()).generation == 3);
}
TEST_CASE("RTU lifecycle calls from backend callbacks reject reentry", "[modbus][session]") {
    auto wire = std::make_shared<RtuWire>();
    auto session = make_session(wire);
    wire->reply = [&](const Bytes &request) {
        REQUIRE_THROWS_AS(session->close(), std::logic_error);
        REQUIRE_THROWS_AS(session->recover(1), std::logic_error);
        return normal_reply(request);
    };
    REQUIRE(session->transact(kRead, options()).outcome == Outcome::kResponse);
}

TEST_CASE("RTU recovery cancels active work and invalidates queued units together",
          "[modbus][session][concurrency]") {
    auto old = std::make_shared<RtuWire>();
    old->block_drain = true;
    auto next = std::make_shared<RtuWire>();
    next->reply = normal_reply;
    unsigned opens = 0;
    auto session = RtuSession::create(rtu_config(), [&](const serial::Config &) {
        return std::make_unique<RtuStream>(++opens == 1 ? old : next);
    });
    auto active =
        std::async(std::launch::async, [&] { return session->transact(kRead, options(2s)); });
    await_writes(old);
    auto queued = std::async(std::launch::async, [&] {
        try {
            (void)session->transact({2, 3, 0, 1, {}}, options(2s));
        } catch (const FaultedError &) {
            return true;
        }
        return false;
    });
    const auto end = transport::Clock::now() + 1s;
    while (session->health().pending == 0 && transport::Clock::now() < end) {
        std::this_thread::yield();
    }
    const auto generation = session->recover(1);
    REQUIRE(active.get().delivery_uncertain);
    REQUIRE(queued.get());
    REQUIRE(generation == 2);
    REQUIRE(old->writes == 1);
    REQUIRE(next->writes == 0);
    REQUIRE(session->transact(kRead, options()).outcome == Outcome::kResponse);
}
TEST_CASE("RTU concurrent recovery callers reuse a completed generation",
          "[modbus][session][concurrency]") {
    std::atomic<unsigned> opens{0};
    auto session = RtuSession::create(rtu_config(), [&](const serial::Config &) {
        ++opens;
        auto wire = std::make_shared<RtuWire>();
        wire->reply = normal_reply;
        return std::make_unique<RtuStream>(wire);
    });
    auto first = std::async(std::launch::async, [&] { return session->recover(1); });
    auto second = std::async(std::launch::async, [&] { return session->recover(1); });
    REQUIRE(first.get() == 2);
    REQUIRE(second.get() == 2);
    REQUIRE(opens == 2);
}
TEST_CASE("RTU truncated and mismatched frames never become successful transactions",
          "[modbus][session]") {
    for (const auto &response :
         std::vector<Bytes>{Bytes{1, 3, 4}, with_crc({1, 4, 4, 0, 1, 0, 2}),
                            with_crc({1, 3, 2, 0, 1}), with_crc({1, 0x83, 0})}) {
        auto wire = std::make_shared<RtuWire>();
        wire->reply = [response](const Bytes &) { return response; };
        auto session = make_session(wire);
        const auto result = session->transact(kRead, options(20ms));
        REQUIRE(result.error);
        REQUIRE(result.delivery_uncertain);
        REQUIRE(session->health().faulted);
        REQUIRE(wire->writes == 1);
    }
}
