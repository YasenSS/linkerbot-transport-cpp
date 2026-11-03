#include <catch2/catch_test_macros.hpp>

#include "optional_value.hpp"

// SocketCANTransport is Linux-only (linux/can.h, PF_CAN, ...); this whole
// file is a no-op on other platforms.
#if defined(__linux__)

#include <linkerbot/can/socketcan_frame.hpp>
#include <linkerbot/can/socketcan_transport.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

#include <linux/can.h>
#include <sys/socket.h>
#include <unistd.h>

using linkerbot::can::ClosedError;
using linkerbot::can::Message;
using linkerbot::can::SocketCANTransport;
using linkerbot::can::TransportIOError;
using linkerbot::test_support::require_value;

namespace {

    // One end (fds[0]) is handed to SocketCANTransport via AdoptFd; the other
    // (fds[1]) is the "peer" the test drives directly with raw read()/write() to
    // simulate link traffic (or its absence) without a real CAN interface.
    auto make_socketpair() -> std::array<int, 2> {
        std::array<int, 2> fds{-1, -1};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
        return fds;
    }

} // namespace

TEST_CASE("AdoptFd round-trips a normal frame end-to-end", "[socketcan][lifecycle]") {
    auto fds = make_socketpair();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, {}});

    Message msg;
    msg.id = 0x123;
    msg.dlc = 3;
    msg.data[0] = 1;
    msg.data[1] = 2;
    msg.data[2] = 3;
    REQUIRE_NOTHROW(transport.write(msg));

    struct can_frame frame {};
    REQUIRE(::read(fds[1], &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame)));
    // Echo it straight back so transport.read() has something to decode.
    REQUIRE(::write(fds[1], &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame)));

    auto got = transport.read();
    REQUIRE(got.has_value());
    REQUIRE(require_value(got).id == 0x123);
    REQUIRE(require_value(got).dlc == 3);
    REQUIRE(require_value(got).data[0] == 1);
    REQUIRE(require_value(got).data[2] == 3);

    ::close(fds[1]);
}

TEST_CASE("close() waits for an in-flight blocking read before releasing the fd",
          "[socketcan][lifecycle]") {
    auto fds = make_socketpair();

    // test_on_io_in_flight fires after in_flight_ is already incremented but
    // before the real ::read() syscall runs, and we hold it there until the
    // test releases it. That makes "a read is genuinely in flight" — and
    // "close() must not be able to finish while that is true" — a
    // deterministic fact instead of something inferred from elapsed wall-
    // clock time racing SO_RCVTIMEO.
    std::promise<void> in_flight;
    std::promise<void> release_read;
    auto release_read_future = release_read.get_future();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, [&] {
                                                                 in_flight.set_value();
                                                                 release_read_future.wait();
                                                             }});

    std::promise<bool> read_threw;
    auto read_threw_future = read_threw.get_future();
    std::thread reader([&] {
        try {
            auto got = transport.read();
            (void)got;
            read_threw.set_value(false);
        } catch (const ClosedError &) {
            read_threw.set_value(true);
        }
    });
    in_flight.get_future().wait();

    std::promise<void> close_started;
    std::promise<void> close_done;
    auto close_done_future = close_done.get_future();
    std::thread closer([&] {
        close_started.set_value();
        transport.close();
        close_done.set_value();
    });
    close_started.get_future().wait();
    // Generous bounded negative assertion: close() must not report done
    // while the read is still held in flight by the seam, but this is not a
    // minimum-elapsed-time check, so it cannot be spuriously slow-CI-flaky.
    CHECK(close_done_future.wait_for(std::chrono::milliseconds(200)) ==
          std::future_status::timeout);

    release_read.set_value();
    closer.join();
    reader.join();
    REQUIRE(read_threw_future.get() == false);

    // Idempotent double close.
    REQUIRE_NOTHROW(transport.close());

    ::close(fds[1]);
}

TEST_CASE("a second close() call waits for the first to fully complete", "[socketcan][lifecycle]") {
    auto fds = make_socketpair();

    std::promise<void> in_flight;
    std::promise<void> release_read;
    auto release_read_future = release_read.get_future();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, [&] {
                                                                 in_flight.set_value();
                                                                 release_read_future.wait();
                                                             }});

    std::thread reader([&] {
        try {
            (void)transport.read();
        } catch (const ClosedError &) {
            // Closing the transport may cancel this deliberately blocked read.
            return;
        }
    });
    in_flight.get_future().wait();

    std::promise<void> first_close_started;
    std::promise<void> first_close_done;
    auto first_close_done_future = first_close_done.get_future();
    std::thread first_closer([&] {
        first_close_started.set_value();
        transport.close();
        first_close_done.set_value();
    });
    first_close_started.get_future().wait();
    CHECK(first_close_done_future.wait_for(std::chrono::milliseconds(200)) ==
          std::future_status::timeout);

    std::promise<void> second_close_started;
    std::promise<void> second_close_done;
    auto second_close_done_future = second_close_done.get_future();
    std::thread second_closer([&] {
        second_close_started.set_value();
        transport.close();
        second_close_done.set_value();
    });
    second_close_started.get_future().wait();

    // The regression this guards: the old close() reported completion to a
    // second caller once closed_ was set, even while the first caller was
    // still waiting on in-flight I/O and had not actually released the fd
    // yet. Neither call may finish while the read is still held.
    CHECK(first_close_done_future.wait_for(std::chrono::milliseconds(200)) ==
          std::future_status::timeout);
    CHECK(second_close_done_future.wait_for(std::chrono::milliseconds(200)) ==
          std::future_status::timeout);

    release_read.set_value();
    reader.join();
    first_closer.join();
    second_closer.join();
    CHECK(first_close_done_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    CHECK(second_close_done_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

    ::close(fds[1]);
}

TEST_CASE("read()/write() throw ClosedError immediately after close()", "[socketcan][lifecycle]") {
    auto fds = make_socketpair();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, {}});
    transport.close();

    Message msg;
    msg.dlc = 1;
    REQUIRE_THROWS_AS(transport.write(msg), ClosedError);
    REQUIRE_THROWS_AS(transport.read(), ClosedError);

    ::close(fds[1]);
}

TEST_CASE("read() filters raw CAN_ERR_FLAG frames instead of decoding them",
          "[socketcan][lifecycle]") {
    auto fds = make_socketpair();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, {}});

    struct can_frame frame {};
    frame.can_id = 0x123 | CAN_ERR_FLAG;
    frame.can_dlc = 8;
    REQUIRE(::write(fds[1], &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame)));

    REQUIRE_FALSE(transport.read().has_value());

    ::close(fds[1]);
}

TEST_CASE("read() filters raw CAN_RTR_FLAG frames instead of decoding them",
          "[socketcan][lifecycle]") {
    auto fds = make_socketpair();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, {}});

    struct can_frame frame {};
    frame.can_id = 0x123 | CAN_RTR_FLAG;
    frame.can_dlc = 8; // requested length for an RTR frame, not real payload
    REQUIRE(::write(fds[1], &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame)));

    REQUIRE_FALSE(transport.read().has_value());

    ::close(fds[1]);
}

TEST_CASE("write() fails within the bounded send timeout instead of hanging",
          "[socketcan][lifecycle]") {
    auto fds = make_socketpair();
    // Shrink the send buffer so a peer that never drains fills it quickly;
    // the peer stays open (no EPIPE) but never reads, so eventually write()
    // must block until SO_SNDTIMEO (50ms) elapses and then throw instead of
    // hanging forever (R18).
    int small_sndbuf = 1024;
    REQUIRE(::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small_sndbuf, sizeof(small_sndbuf)) == 0);
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], false, {}});

    Message msg;
    msg.id = 1;
    msg.dlc = 8;

    bool threw = false;
    for (int i = 0; i < 100000 && !threw; ++i) {
        try {
            transport.write(msg);
        } catch (const TransportIOError &) {
            threw = true;
        }
    }
    REQUIRE(threw);

    ::close(fds[1]);
}

TEST_CASE("SocketCAN FD accepts Classic and FD datagrams and rejects malformed lengths",
          "[socketcan][frames]") {
    auto fds = make_socketpair();
    SocketCANTransport transport(SocketCANTransport::AdoptFd{fds[0], true, {}});
    REQUIRE_FALSE(transport.read());
    canfd_frame fd{};
    fd.can_id = CAN_EFF_FLAG | 0x1abcdef;
    fd.len = 12;
    fd.flags = CANFD_BRS;
    fd.data[11] = 77;
    REQUIRE(::write(fds[1], &fd, sizeof(fd)) == sizeof(fd));
    const auto decoded = transport.read();
    REQUIRE(decoded);
    REQUIRE(require_value(decoded).is_fd);
    REQUIRE(require_value(decoded).is_brs);
    REQUIRE(require_value(decoded).is_extended);
    REQUIRE(require_value(decoded).id == 0x1abcdef);
    REQUIRE(require_value(decoded).data[11] == 77);
    Message reply = require_value(decoded);
    transport.write(reply);
    canfd_frame encoded{};
    REQUIRE(::read(fds[1], &encoded, sizeof(encoded)) == sizeof(encoded));
    REQUIRE(encoded.can_id == fd.can_id);
    REQUIRE(encoded.flags == CANFD_BRS);
    REQUIRE(encoded.data[11] == 77);
    can_frame classic{};
    classic.can_id = CAN_EFF_FLAG | 0x123456;
    classic.can_dlc = 8;
    REQUIRE(::write(fds[1], &classic, sizeof(classic)) == sizeof(classic));
    const auto old = transport.read();
    REQUIRE(old);
    REQUIRE_FALSE(require_value(old).is_fd);
    REQUIRE(require_value(old).is_extended);
    REQUIRE(require_value(old).id == 0x123456);
    for (auto flag : {CAN_ERR_FLAG, CAN_RTR_FLAG}) {
        classic.can_id = flag;
        REQUIRE(::write(fds[1], &classic, sizeof(classic)) == sizeof(classic));
        REQUIRE_FALSE(transport.read());
        fd.can_id = flag;
        REQUIRE(::write(fds[1], &fd, sizeof(fd)) == sizeof(fd));
        REQUIRE_FALSE(transport.read());
    }
    const char byte = 0;
    REQUIRE(::write(fds[1], &byte, 1) == 1);
    REQUIRE_THROWS_AS(transport.read(), TransportIOError);
    ::close(fds[1]);
    REQUIRE_FALSE(transport.read());
    fd.len = 65;
    REQUIRE_THROWS_AS(linkerbot::can::decode_socketcan_frame(fd), linkerbot::can::ValidationError);
    reply.is_fd = false;
    reply.is_brs = false;
    reply.dlc = 8;
    const auto classic_encoded =
        linkerbot::can::encode_socketcan_frame(linkerbot::can::LinkKind::kClassic, reply);
    REQUIRE((classic_encoded.classic_frame.can_id & CAN_EFF_FLAG) != 0);
}

TEST_CASE("SocketCAN adopted handles reject invalid descriptors and release failed setup",
          "[socketcan][faults]") {
    REQUIRE_THROWS_AS(SocketCANTransport(SocketCANTransport::AdoptFd{-1, false, {}}),
                      linkerbot::can::ValidationError);
    std::array<int, 2> pipe_fds{};
    REQUIRE(::pipe(pipe_fds.data()) == 0);
    REQUIRE_THROWS_AS(SocketCANTransport(SocketCANTransport::AdoptFd{pipe_fds[0], false, {}}),
                      TransportIOError);
    ::close(pipe_fds[1]);
    for (bool fd_mode : {false, true}) {
        auto fds = make_socketpair();
        SocketCANTransport transport(
            SocketCANTransport::AdoptFd{fds[0], fd_mode, [fd = fds[0]] { ::close(fd); }});
        REQUIRE_THROWS_AS(transport.read(), TransportIOError);
        REQUIRE_THROWS_AS(transport.close(), TransportIOError);
        REQUIRE_NOTHROW(transport.close());
        ::close(fds[1]);
    }
}

TEST_CASE("SocketCAN unwinds an interrupted reservation and contains destructor close errors",
          "[socketcan][faults]") {
    auto fds = make_socketpair();
    {
        SocketCANTransport transport(SocketCANTransport::AdoptFd{
            fds[0], false, [] { throw std::runtime_error("interrupted I/O"); }});
        REQUIRE_THROWS_AS(transport.read(), std::runtime_error);
        REQUIRE_NOTHROW(transport.close());
    }
    ::close(fds[1]);
    auto short_fds = make_socketpair();
    {
        SocketCANTransport transport(SocketCANTransport::AdoptFd{short_fds[0], false, {}});
        char byte = 0;
        REQUIRE(::write(short_fds[1], &byte, 1) == 1);
        REQUIRE_THROWS_AS(transport.read(), TransportIOError);
        // No descriptor is opened between invalidation and destruction: this
        // tests the one-attempt close error contract without fd-number reuse.
        ::close(short_fds[0]);
    }
    ::close(short_fds[1]);
}

#endif // defined(__linux__)
