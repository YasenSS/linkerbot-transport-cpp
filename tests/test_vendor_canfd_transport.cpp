#include <catch2/catch_test_macros.hpp>

#include "frame_tx_cases.hpp"
#include "optional_value.hpp"

#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/vendor_canfd_transport.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using linkerbot::can::CanFDConfig;
using linkerbot::can::CanFDMsg;
using linkerbot::can::kFrameTypeBrs;
using linkerbot::can::kFrameTypeFd;
using linkerbot::can::LinkKind;
using linkerbot::can::Message;
using linkerbot::can::vendor_dlc_to_length;
using linkerbot::can::vendor_length_to_dlc;
using linkerbot::can::VendorCanFdApi;
using linkerbot::can::VendorCanFdTransport;
namespace life = linkerbot::can::vendor_canfd_lifecycle;

using linkerbot::test_support::require_value;

namespace {

    struct FakeLib {
        std::string name = "/fake/libcanbus.so";
        std::atomic<int> init_calls{0};
        std::atomic<int> exit_calls{0};
        std::atomic<int> scan_calls{0};
        std::atomic<int> open_calls{0};
        std::atomic<int> close_calls{0};
        std::atomic<int> canfd_init_calls{0};
        std::atomic<int> transmit_calls{0};
        std::atomic<int> receive_calls{0};

        int init_result = 0;
        int exit_result = 0;
        int scan_result = 1;
        int open_result = 0;
        int close_result = 0;
        int canfd_init_result = 0;
        int transmit_result = 1;
        int receive_error = 0; // <0 forces Receive to return this status

        bool throw_on_canfd_init = false;
        std::vector<CanFDMsg> rx_frames;
        CanFDMsg last_tx{};
        std::vector<CanFDMsg> last_batch;

        // R05 regression seam: when set, LibExit() blocks until the test
        // releases exit_gate, and flips exit_entered as soon as it starts
        // blocking so the test can deterministically observe "Exit is in
        // flight" without racing thread startup.
        bool block_exit = false;
        std::atomic<bool> exit_entered{false};
        std::promise<void> exit_gate;

        static FakeLib *active;

        // Names match vendor C entry points; see vendor_canfd_abi.hpp.
        // NOLINTBEGIN(readability-identifier-naming)
        static int LibInit() {
            ++active->init_calls;
            return active->init_result;
        }
        static int LibExit() {
            ++active->exit_calls;
            if (active->block_exit) {
                active->exit_entered.store(true);
                active->exit_gate.get_future().wait();
            }
            return active->exit_result;
        }
        static int Scan() {
            ++active->scan_calls;
            return active->scan_result;
        }
        static int Open(unsigned, unsigned) {
            ++active->open_calls;
            return active->open_result;
        }
        static int Close(unsigned, unsigned) {
            ++active->close_calls;
            return active->close_result;
        }
        static int CanFdInit(unsigned, unsigned, CanFDConfig *) {
            ++active->canfd_init_calls;
            if (active->throw_on_canfd_init) {
                throw std::runtime_error("vendor init crashed");
            }
            return active->canfd_init_result;
        }
        static int Transmit(unsigned, unsigned, CanFDMsg *msgs, unsigned count, int) {
            ++active->transmit_calls;
            active->last_batch.clear();
            if (msgs != nullptr) {
                for (unsigned i = 0; i < count; ++i) {
                    active->last_batch.push_back(msgs[i]);
                }
                if (count >= 1) {
                    active->last_tx = msgs[0];
                }
            }
            if (active->transmit_result == 1) {
                return static_cast<int>(count);
            }
            return active->transmit_result;
        }
        static int Receive(unsigned, unsigned, CanFDMsg *msgs, unsigned max_frames, int) {
            ++active->receive_calls;
            if (active->receive_error < 0) {
                return active->receive_error;
            }
            const int n = static_cast<int>(
                std::min(active->rx_frames.size(), static_cast<size_t>(max_frames)));
            for (int i = 0; i < n; ++i) {
                msgs[i] = active->rx_frames[static_cast<size_t>(i)];
            }
            if (n > 0) {
                active->rx_frames.erase(active->rx_frames.begin(), active->rx_frames.begin() + n);
            }
            return n;
        }
        // NOLINTEND(readability-identifier-naming)
    };

    FakeLib *FakeLib::active = nullptr;

    auto make_api(FakeLib &lib) -> std::shared_ptr<VendorCanFdApi> {
        FakeLib::active = &lib;
        auto api = std::make_shared<VendorCanFdApi>();
        api->lifecycle_key = lib.name;
        api->lib_init = &FakeLib::LibInit;
        api->lib_exit = &FakeLib::LibExit;
        api->scan_device = &FakeLib::Scan;
        api->open_device = &FakeLib::Open;
        api->close_device = &FakeLib::Close;
        api->canfd_init = &FakeLib::CanFdInit;
        api->transmit = &FakeLib::Transmit;
        api->receive = &FakeLib::Receive;
        return api;
    }

    struct LifecycleGuard {
        LifecycleGuard() {
            life::reset_for_test();
        }
        ~LifecycleGuard() {
            life::reset_for_test();
        }
    };

    template <typename Fn> auto throws_with(const char *needle, Fn &&fn) -> bool {
        try {
            fn();
        } catch (const std::exception &e) {
            return std::string(e.what()).find(needle) != std::string::npos;
        }
        return false;
    }

} // namespace

TEST_CASE("vendor DLC length tables match Python", "[vendor_canfd]") {
    REQUIRE(vendor_dlc_to_length(0) == 0);
    REQUIRE(vendor_dlc_to_length(8) == 8);
    REQUIRE(vendor_dlc_to_length(9) == 12);
    REQUIRE(vendor_dlc_to_length(15) == 64);
    REQUIRE(vendor_length_to_dlc(0) == 0);
    REQUIRE(vendor_length_to_dlc(8) == 8);
    REQUIRE(vendor_length_to_dlc(9) == 9);
    REQUIRE(vendor_length_to_dlc(12) == 9);
    REQUIRE(vendor_length_to_dlc(64) == 15);
    REQUIRE_THROWS(vendor_dlc_to_length(16));
    REQUIRE_THROWS(vendor_length_to_dlc(65));
}

TEST_CASE("vendor receive timeout remains bounded", "[vendor_canfd][stop]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport::Config cfg;
    cfg.receive_timeout_ms = linkerbot::can::kVendorCanFdMaxReceiveTimeoutMs + 1;
    REQUIRE_THROWS_AS(VendorCanFdTransport(cfg, api), linkerbot::can::ValidationError);
    REQUIRE(lib.scan_calls.load() == 0);
}

TEST_CASE("LibCANbus_Init runs once per shared library", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);

    {
        VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);
        REQUIRE(lib.init_calls.load() == 1);
        REQUIRE(lib.exit_calls.load() == 0);
        auto snap = life::snapshot();
        REQUIRE(snap.at(lib.name) == 1);
        transport.close();
    }

    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("multiple transports share one Init/Exit", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);

    VendorCanFdTransport::Config cfg_b;
    cfg_b.channel_index = 1;

    auto a = std::make_unique<VendorCanFdTransport>(VendorCanFdTransport::Config{}, api);
    auto b = std::make_unique<VendorCanFdTransport>(cfg_b, api);

    REQUIRE(lib.init_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 0);

    a->close();
    REQUIRE(lib.exit_calls.load() == 0);

    b->close();
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("CANFD_Init failure releases lifecycle and closes device", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.canfd_init_result = 3;
    auto api = make_api(lib);

    REQUIRE(throws_with("CANFD_Init",
                        [&] { VendorCanFdTransport(VendorCanFdTransport::Config{}, api); }));

    REQUIRE(lib.init_calls.load() == 1);
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("CANFD_Init exception closes device and releases lifecycle", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.throw_on_canfd_init = true;
    auto api = make_api(lib);

    REQUIRE(throws_with("vendor init crashed",
                        [&] { VendorCanFdTransport(VendorCanFdTransport::Config{}, api); }));

    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("LibCANbus_Init nonzero does not bump refcount", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.init_result = -1;
    auto api = make_api(lib);

    REQUIRE(throws_with("LibCANbus_Init",
                        [&] { VendorCanFdTransport(VendorCanFdTransport::Config{}, api); }));

    REQUIRE(lib.exit_calls.load() == 0);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("CloseDevice failure retains ownership for a real retry", "[vendor_canfd]") {
    // R06: a single failed CAN_CloseDevice must not fake success. Ownership
    // (the lifecycle refcount) is retained so a later close() call performs a
    // genuine second CAN_CloseDevice call instead of replaying a cached result.
    LifecycleGuard guard;
    FakeLib lib;
    lib.close_result = 1;
    auto api = make_api(lib);

    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);
    REQUIRE(throws_with("CAN_CloseDevice", [&] { transport.close(); }));
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 0);
    REQUIRE(life::snapshot().at(lib.name) == 1);

    // New I/O is rejected immediately even though the physical close is still
    // pending retry.
    Message msg;
    msg.dlc = 1;
    msg.data[0] = 1;
    REQUIRE_THROWS_AS(transport.write(msg), linkerbot::can::ClosedError);
    REQUIRE(lib.transmit_calls.load() == 0);

    // Fix the vendor side and retry: this must be a genuine second
    // CAN_CloseDevice call, not a cached "already closed" success.
    lib.close_result = 0;
    REQUIRE_NOTHROW(transport.close());
    REQUIRE(lib.close_calls.load() == 2);
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::snapshot().empty());

    // Idempotent once truly closed: no further CAN_CloseDevice calls.
    REQUIRE_NOTHROW(transport.close());
    REQUIRE(lib.close_calls.load() == 2);
}

TEST_CASE("vendor lifecycle serializes last Exit against next Init", "[vendor_canfd]") {
    // R05: the last Exit for a key and the next Init (any key) must not
    // overlap. Force LibExit() to block mid-call while a concurrent
    // transport construction (acquire()) races it; the new Init must not
    // observe the vendor library's global state mid-teardown.
    LifecycleGuard guard;
    FakeLib lib;
    lib.block_exit = true;
    auto api = make_api(lib);

    auto first = std::make_unique<VendorCanFdTransport>(VendorCanFdTransport::Config{}, api);
    REQUIRE(lib.init_calls.load() == 1);

    std::thread closer([&] { first->close(); });
    while (!lib.exit_entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::promise<std::unique_ptr<VendorCanFdTransport>> second_promise;
    auto second_future = second_promise.get_future();
    std::thread opener([&] {
        second_promise.set_value(
            std::make_unique<VendorCanFdTransport>(VendorCanFdTransport::Config{}, api));
    });

    // Exit is still blocking: the opener thread must be stuck waiting for
    // lifecycle_mutex_ before it can even attempt LibInit.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(lib.init_calls.load() == 1);

    lib.exit_gate.set_value();
    closer.join();

    REQUIRE(second_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto second = second_future.get();
    REQUIRE(lib.init_calls.load() == 2);
    REQUIRE(lib.exit_calls.load() == 1);

    lib.block_exit = false;
    second->close();
    opener.join();
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("a second close() call waits for the in-progress Exit to finish", "[vendor_canfd]") {
    // Regresses the ordering bug where close() marked the device closed and
    // notified other close() callers before vendor_canfd_lifecycle::release()
    // (and thus LibCANbus_Exit) had actually finished: a second caller could
    // observe "done" while the first was still inside Exit.
    LifecycleGuard guard;
    FakeLib lib;
    lib.block_exit = true;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    std::promise<void> first_close_started;
    std::promise<void> first_close_done;
    auto first_close_done_future = first_close_done.get_future();
    std::thread first_closer([&] {
        first_close_started.set_value();
        transport.close();
        first_close_done.set_value();
    });
    first_close_started.get_future().wait();
    while (!lib.exit_entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Exit is genuinely blocked: the first close() must not have finished.
    REQUIRE(first_close_done_future.wait_for(std::chrono::milliseconds(50)) ==
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

    // Neither call may finish while Exit is still blocked.
    REQUIRE(first_close_done_future.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout);
    REQUIRE(second_close_done_future.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout);

    lib.exit_gate.set_value();
    first_closer.join();
    second_closer.join();

    REQUIRE(first_close_done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(second_close_done_future.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    // Exactly one physical close and one Exit: the second caller observed
    // completion, it did not perform (or trigger) a redundant attempt.
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("Exit failure is propagated by close() and retried without reclosing the device",
          "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.exit_result = 7;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    REQUIRE(throws_with("LibCANbus_Exit", [&] { transport.close(); }));
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 1);
    // Refcount already hit zero and the entry is retained (not torn down) so
    // a retry re-invokes Exit instead of replaying a cached result.
    REQUIRE(life::snapshot().at(lib.name) == 0);

    // No new Init or device open may cross a failed Exit retry boundary.
    const auto opened_before = lib.open_calls.load();
    REQUIRE_THROWS_AS(VendorCanFdTransport(VendorCanFdTransport::Config{}, api),
                      linkerbot::can::FaultedError);
    REQUIRE(lib.init_calls.load() == 1);
    REQUIRE(lib.open_calls.load() == opened_before);

    // New I/O stays rejected while the retry is pending.
    Message msg;
    msg.dlc = 1;
    REQUIRE_THROWS_AS(transport.write(msg), linkerbot::can::ClosedError);

    lib.exit_result = 0;
    REQUIRE_NOTHROW(transport.close());
    // CAN_CloseDevice must not be re-issued: the device was already physically
    // closed on the first attempt, only Exit needed a retry.
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 2);
    REQUIRE(life::snapshot().empty());

    // Idempotent once truly done.
    REQUIRE_NOTHROW(transport.close());
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 2);
}

TEST_CASE("loader-produced handles are each unloaded exactly once", "[vendor_canfd][loader]") {
    // R06/leak fix: two independent load_vendor_canfd_api() calls for the same
    // library produce two different VendorCanFdApi instances sharing one
    // lifecycle_key, but vendor_canfd_lifecycle only ever retains the first
    // one in its table. Before library_owner, release() only ever unloaded
    // that retained instance's handle, leaking every other load's handle for
    // the same key. library_owner ties each instance's handle to its own
    // object lifetime instead, so both get unloaded regardless of which one
    // the lifecycle table happens to keep.
    LifecycleGuard guard;
    FakeLib lib;
    auto api_a = make_api(lib);
    auto api_b = make_api(lib); // same lifecycle_key: simulates a second load

    int unload_calls_a = 0;
    int unload_calls_b = 0;
    api_a->library_owner =
        std::shared_ptr<void>(&unload_calls_a, [](void *p) { ++*static_cast<int *>(p); });
    api_b->library_owner =
        std::shared_ptr<void>(&unload_calls_b, [](void *p) { ++*static_cast<int *>(p); });

    VendorCanFdTransport::Config cfg_b;
    cfg_b.channel_index = 1;
    auto a = std::make_unique<VendorCanFdTransport>(VendorCanFdTransport::Config{}, api_a);
    auto b = std::make_unique<VendorCanFdTransport>(cfg_b, api_b);

    a->close();
    b->close();
    a.reset();
    b.reset();
    api_a.reset();
    api_b.reset();

    REQUIRE(unload_calls_a == 1);
    REQUIRE(unload_calls_b == 1);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("write and read convert DLC and payload", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);

    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    Message out;
    out.id = 0x123;
    out.is_extended = true;
    out.is_fd = true;
    out.dlc = 12;
    for (uint8_t i = 0; i < 12; ++i) {
        out.data[i] = static_cast<uint8_t>(i + 1);
    }
    REQUIRE_NOTHROW(transport.write(out));
    REQUIRE(lib.transmit_calls.load() == 1);
    REQUIRE(lib.last_tx.ID == 0x123);
    REQUIRE(lib.last_tx.ExternFlag == 1);
    REQUIRE(lib.last_tx.DLC == 9); // length 12 -> DLC 9
    REQUIRE(lib.last_tx.FrameType == kFrameTypeFd);
    REQUIRE(lib.last_tx.Data[0] == 1);
    REQUIRE(lib.last_tx.Data[11] == 12);

    CanFDMsg rx{};
    rx.ID = 0x456;
    rx.FrameType = kFrameTypeFd;
    rx.ExternFlag = 0;
    rx.DLC = 15; // 64 bytes
    rx.Data[0] = 0xAB;
    rx.Data[63] = 0xCD;
    lib.rx_frames.push_back(rx);

    auto got = transport.read();
    REQUIRE(got.has_value());
    REQUIRE(require_value(got).id == 0x456);
    REQUIRE_FALSE(require_value(got).is_extended);
    REQUIRE(require_value(got).is_fd);
    REQUIRE_FALSE(require_value(got).is_brs);
    REQUIRE(require_value(got).dlc == 64);
    REQUIRE(require_value(got).data[0] == 0xAB);
    REQUIRE(require_value(got).data[63] == 0xCD);

    REQUIRE_FALSE(transport.read().has_value());
    transport.close();
}

TEST_CASE("write/read fail after close", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);

    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);
    transport.close();

    Message msg;
    msg.dlc = 1;
    msg.data[0] = 1;
    REQUIRE_THROWS_AS(transport.write(msg), linkerbot::can::ClosedError);
    REQUIRE_THROWS_AS(transport.read(), linkerbot::can::ClosedError);
}

TEST_CASE("no adapter and out-of-range device_index", "[vendor_canfd]") {
    LifecycleGuard guard;
    {
        FakeLib lib;
        lib.scan_result = 0;
        auto api = make_api(lib);
        REQUIRE(throws_with("No CANFD adapter",
                            [&] { VendorCanFdTransport(VendorCanFdTransport::Config{}, api); }));
        REQUIRE(lib.exit_calls.load() == 1);
    }
    {
        FakeLib lib;
        lib.scan_result = 1;
        auto api = make_api(lib);
        VendorCanFdTransport::Config cfg;
        cfg.device_index = 5;
        REQUIRE(throws_with("out of range", [&] { VendorCanFdTransport(cfg, api); }));
        REQUIRE(lib.exit_calls.load() == 1);
    }
}

TEST_CASE("vendor TX/RX follow the shared FrameType vectors", "[vendor_canfd][frame]") {
    for (std::size_t i = 0; i < linkerbot::can::test::kFrameTxCaseCount; ++i) {
        const auto &row = linkerbot::can::test::kFrameTxCases[i];
        if (row.link != LinkKind::kCanFd) {
            continue;
        }
        INFO(row.name);
        LifecycleGuard guard;
        FakeLib lib;
        auto api = make_api(lib);
        VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

        Message msg = linkerbot::can::test::make_tx_message(row);
        if (!row.expect_ok) {
            REQUIRE_THROWS_AS(transport.write(msg), linkerbot::can::ValidationError);
            REQUIRE(lib.transmit_calls.load() == 0);
            transport.close();
            continue;
        }
        REQUIRE_NOTHROW(transport.write(msg));
        REQUIRE(lib.last_tx.FrameType == row.expect_frame_type);
        transport.close();
    }

    for (std::size_t i = 0; i < linkerbot::can::test::kFrameRxCaseCount; ++i) {
        const auto &row = linkerbot::can::test::kFrameRxCases[i];
        INFO(row.name);
        LifecycleGuard guard;
        FakeLib lib;
        auto api = make_api(lib);
        VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

        CanFDMsg rx{};
        rx.ID = 0x50;
        rx.FrameType = row.frame_type;
        rx.DLC = 1;
        rx.Data[0] = 0xAA;
        lib.rx_frames.push_back(rx);

        if (!row.expect_ok) {
            REQUIRE_THROWS(transport.read());
            transport.close();
            continue;
        }
        auto got = transport.read();
        REQUIRE(got.has_value());
        REQUIRE(require_value(got).is_fd == row.expect_is_fd);
        REQUIRE(require_value(got).is_brs == row.expect_is_brs);
        transport.close();
    }
}

TEST_CASE("a malformed frame in a vendor RX batch does not drop neighbours",
          "[vendor_canfd][frame]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    auto make_rx = [](uint32_t id, uint8_t frame_type) {
        CanFDMsg rx{};
        rx.ID = id;
        rx.FrameType = frame_type;
        rx.DLC = 1;
        rx.Data[0] = 0xAA;
        return rx;
    };

    lib.rx_frames.push_back(make_rx(0x10, kFrameTypeFd));
    lib.rx_frames.push_back(make_rx(0x11, kFrameTypeBrs)); // BRS without FD
    lib.rx_frames.push_back(make_rx(0x12, kFrameTypeFd));

    auto first = transport.read();
    auto second = transport.read();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(require_value(first).id == 0x10);
    REQUIRE(require_value(second).id == 0x12);
    REQUIRE_FALSE(transport.read().has_value());
    transport.close();
}

TEST_CASE("Config.frame_type does not override Message flags", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport::Config cfg;
    cfg.frame_type = static_cast<uint8_t>(kFrameTypeFd | kFrameTypeBrs);
    VendorCanFdTransport transport(cfg, api);

    Message untagged;
    untagged.id = 1;
    untagged.dlc = 1;
    untagged.data[0] = 1;
    REQUIRE_NOTHROW(transport.write(untagged));
    REQUIRE(lib.last_tx.FrameType == kFrameTypeFd);

    Message with_brs;
    with_brs.id = 1;
    with_brs.dlc = 1;
    with_brs.data[0] = 1;
    with_brs.is_fd = true;
    with_brs.is_brs = true;
    REQUIRE_NOTHROW(transport.write(with_brs));
    REQUIRE(lib.last_tx.FrameType == static_cast<uint8_t>(kFrameTypeFd | kFrameTypeBrs));
    transport.close();
}

TEST_CASE("vendor write rejects out-of-range IDs and DLC", "[vendor_canfd]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    Message standard_too_wide;
    standard_too_wide.id = 0x800;
    standard_too_wide.dlc = 1;
    REQUIRE_THROWS_AS(transport.write(standard_too_wide), linkerbot::can::ValidationError);

    Message fd_too_long;
    fd_too_long.is_fd = true;
    fd_too_long.dlc = 65;
    REQUIRE_THROWS_AS(transport.write(fd_too_long), linkerbot::can::ValidationError);
    REQUIRE(lib.transmit_calls.load() == 0);
    transport.close();
}

TEST_CASE("vendor IO errors retain native status", "[vendor_canfd][errors]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    Message message;
    message.id = 1;
    message.dlc = 1;
    lib.transmit_result = 0;
    try {
        transport.write(message);
        FAIL("short vendor transmit should throw");
    } catch (const linkerbot::can::TransportIOError &error) {
        REQUIRE(error.code() == linkerbot::can::ErrorCode::kTransportIo);
        REQUIRE(error.native_status() == 0);
    }

    lib.receive_error = -9;
    try {
        (void)transport.read();
        FAIL("vendor receive error should throw");
    } catch (const linkerbot::can::TransportIOError &error) {
        REQUIRE(error.code() == linkerbot::can::ErrorCode::kTransportIo);
        REQUIRE(error.native_status() == -9);
    }
    transport.close();
}

TEST_CASE("write_batch empty is a no-op", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);
    REQUIRE_NOTHROW(transport.write_batch({}));
    REQUIRE(lib.transmit_calls.load() == 0);
    transport.close();
}

TEST_CASE("write_batch of one uses the single-frame path", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    Message msg;
    msg.id = 0x11;
    msg.dlc = 1;
    msg.data[0] = 0xAA;
    REQUIRE_NOTHROW(transport.write_batch({msg}));
    REQUIRE(lib.transmit_calls.load() == 1);
    REQUIRE(lib.last_batch.size() == 1);
    REQUIRE(lib.last_tx.ID == 0x11);
    transport.close();
}

TEST_CASE("write_batch issues one vendor call with count", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    std::vector<Message> frames;
    for (uint8_t i = 0; i < 5; ++i) {
        Message msg;
        msg.id = 0x100u + i;
        msg.dlc = 1;
        msg.data[0] = i;
        frames.push_back(msg);
    }
    REQUIRE_NOTHROW(transport.write_batch(frames));
    REQUIRE(lib.transmit_calls.load() == 1);
    REQUIRE(lib.last_batch.size() == 5);
    for (uint8_t i = 0; i < 5; ++i) {
        REQUIRE(lib.last_batch[i].ID == 0x100u + i);
        REQUIRE(lib.last_batch[i].Data[0] == i);
    }
    transport.close();
}

TEST_CASE("write_batch preserves payload and zeroes padding", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    Message full;
    full.id = 1;
    full.is_fd = true;
    full.dlc = 64;
    std::fill(full.data.begin(), full.data.end(), static_cast<uint8_t>(0xFF));

    Message short_frame;
    short_frame.id = 2;
    short_frame.is_fd = true;
    short_frame.dlc = 2;
    short_frame.data[0] = 0xAA;
    short_frame.data[1] = 0xBB;

    REQUIRE_NOTHROW(transport.write_batch({full, short_frame}));
    REQUIRE(lib.last_batch.size() == 2);
    REQUIRE(lib.last_batch[0].DLC == 15);
    for (int i = 0; i < 64; ++i) {
        REQUIRE(lib.last_batch[0].Data[i] == 0xFF);
    }
    REQUIRE(lib.last_batch[1].Data[0] == 0xAA);
    REQUIRE(lib.last_batch[1].Data[1] == 0xBB);
    for (int i = 2; i < 64; ++i) {
        REQUIRE(lib.last_batch[1].Data[i] == 0);
    }
    transport.close();
}

TEST_CASE("write_batch short send fails the whole batch", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.transmit_result = 2;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    std::vector<Message> frames(5);
    for (auto &msg : frames) {
        msg.id = 1;
        msg.dlc = 1;
        msg.data[0] = 1;
    }
    try {
        transport.write_batch(frames);
        FAIL("short batch send should throw");
    } catch (const linkerbot::can::TransportIOError &error) {
        REQUIRE(error.native_status() == 2);
        REQUIRE(std::string(error.what()).find("short send") != std::string::npos);
    }
    transport.close();
}

TEST_CASE("write_batch rejects more than the vendor limit", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);

    std::vector<Message> too_many(linkerbot::can::kVendorCanFdMaxTransmitBatch + 1);
    for (auto &msg : too_many) {
        msg.id = 1;
        msg.dlc = 1;
    }
    REQUIRE_THROWS_AS(transport.write_batch(too_many), linkerbot::can::ValidationError);
    REQUIRE(lib.transmit_calls.load() == 0);
    transport.close();
}

TEST_CASE("write_batch after close raises without touching the vendor", "[vendor_canfd][batch]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport(VendorCanFdTransport::Config{}, api);
    transport.close();

    Message a;
    a.id = 1;
    a.dlc = 1;
    Message b = a;
    b.id = 2;
    const auto before = lib.transmit_calls.load();
    REQUIRE_THROWS_AS(transport.write_batch({a, b}), linkerbot::can::ClosedError);
    REQUIRE(lib.transmit_calls.load() == before);
}

TEST_CASE("failed constructor rollback retains the device before library Exit",
          "[vendor_canfd][lifecycle]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    lib.canfd_init_result = 7;
    lib.close_result = 8;
    REQUIRE(throws_with("CANFD_Init failed", [&] { VendorCanFdTransport transport({}, api); }));
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 0);
    REQUIRE(life::snapshot().at(lib.name) == 1);
    REQUIRE(life::pending_cleanup_count(lib.name) == 1);
    REQUIRE_THROWS_AS(life::retry_deferred_cleanup(lib.name), linkerbot::can::TransportIOError);
    REQUIRE(lib.exit_calls.load() == 0);
    lib.close_result = 0;
    lib.canfd_init_result = 0;
    VendorCanFdTransport recovered({}, api); // retries old close/Exit before Init
    REQUIRE(lib.close_calls.load() == 3);
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(lib.init_calls.load() == 2);
    REQUIRE(life::pending_cleanup_count(lib.name) == 0);
    recovered.close();
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("destructor Exit failure retains a retry without reclosing the device",
          "[vendor_canfd][lifecycle]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    {
        VendorCanFdTransport transport({}, api);
        lib.exit_result = 9;
    }
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(lib.exit_calls.load() == 1);
    REQUIRE(life::pending_cleanup_count(lib.name) == 1);
    REQUIRE_THROWS_AS(life::retry_deferred_cleanup(lib.name), linkerbot::can::TransportIOError);
    REQUIRE(lib.close_calls.load() == 1);
    lib.exit_result = 0;
    life::retry_deferred_cleanup(lib.name);
    REQUIRE(lib.close_calls.load() == 1);
    REQUIRE(life::pending_cleanup_count(lib.name) == 0);
    REQUIRE(life::snapshot().empty());
}

TEST_CASE("native channel configuration is rejected before any vendor call",
          "[vendor_canfd][enumeration]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    api->native_channel_numbers = true;
    VendorCanFdTransport::Config config;
    config.channel_index = 1;
    CHECK_THROWS_AS(VendorCanFdTransport(config, api), linkerbot::can::ValidationError);
    CHECK_THROWS_AS(linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 1, {}}}),
                    linkerbot::can::ValidationError);
    CHECK(lib.init_calls == 0);
    CHECK(lib.scan_calls == 0);
    CHECK(lib.open_calls == 0);
    config.channel_index = 0;
    config.device_index = ~0U;
    CHECK_THROWS_AS(VendorCanFdTransport(config, api), linkerbot::can::ValidationError);
    CHECK(lib.open_calls == 0);
}

TEST_CASE("vendor enumeration keeps type errors and valid neighbours",
          "[vendor_canfd][enumeration]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.scan_result = 6;
    auto api = make_api(lib);
    api->native_channel_numbers = true;
    api->channel_type = [](unsigned dev, unsigned) {
        if (dev == 3) {
            throw std::runtime_error("type query threw");
        }
        const int types[] = {1, 0, 2, 0, -7, 1};
        return types[dev];
    };
    const auto result = linkerbot::can::enumerate_vendor_canfd_channels(api);
    REQUIRE(result.channels.size() == 2);
    CHECK(result.channels[1].device_index == 5);
    REQUIRE(result.failures.size() == 4);
    CHECK(result.failures[0].code == "unsupported_channel_type");
    CHECK(result.failures[1].native_status == 2);
    CHECK(result.failures[2].message.find("type query threw") != std::string::npos);
    CHECK(result.failures[3].native_status == -7);
    CHECK(lib.open_calls == 0);
    CHECK(life::snapshot().empty());
}

TEST_CASE("scan and Exit failures retain enumeration diagnostics and cleanup",
          "[vendor_canfd][enumeration]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    api->channel_type = [](unsigned, unsigned) { return 1; };
    lib.scan_result = -4;
    auto result = linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}});
    REQUIRE(result.failures.size() == 1);
    CHECK(result.failures[0].native_status == -4);
    CHECK(life::snapshot().empty());
    lib.scan_result = 1;
    lib.exit_result = 9;
    result = linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}});
    REQUIRE(result.failures.size() == 1);
    CHECK(result.failures[0].stage == "cleanup");
    CHECK(result.channels.empty());
    CHECK(life::pending_cleanup_count(lib.name) == 1);
    result = linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}});
    CHECK(result.failures[0].stage == "cleanup");
    CHECK(lib.scan_calls == 2);
    lib.exit_result = 0;
    result = linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}});
    CHECK(result.channels.size() == 1);
    CHECK(life::snapshot().empty());
    CHECK(life::pending_cleanup_count(lib.name) == 0);
}

TEST_CASE("vendor enumeration rejects incomplete APIs and deduplicates selected channels",
          "[vendor_canfd][enumeration]") {
    LifecycleGuard guard;
    REQUIRE_THROWS_AS(
        linkerbot::can::enumerate_vendor_canfd_channels(std::shared_ptr<VendorCanFdApi>{}),
        linkerbot::can::ValidationError);

    FakeLib lib;
    auto api = make_api(lib);
    const auto report =
        linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}, {0, 0, {}}});
    REQUIRE(report.channels.empty());
    REQUIRE(report.failures.size() == 1);
    CHECK(report.failures[0].stage == "type");
    CHECK(report.failures[0].code == "type_unavailable");
    CHECK(lib.scan_calls == 1);
    CHECK(life::snapshot().empty());
}

TEST_CASE("vendor transport propagates a negative device scan status", "[vendor_canfd][scan]") {
    LifecycleGuard guard;
    FakeLib lib;
    lib.scan_result = -7;
    auto api = make_api(lib);
    try {
        VendorCanFdTransport transport({}, api);
        FAIL("negative scan status accepted");
    } catch (const linkerbot::can::TransportIOError &error) {
        CHECK(error.native_status() == -7);
    }
    CHECK(lib.open_calls == 0);
    CHECK(lib.exit_calls == 1);
    CHECK(life::snapshot().empty());
}

TEST_CASE("enumeration serializes its busy check and scan against opening",
          "[vendor_canfd][enumeration]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    api->channel_type = [](unsigned, unsigned) { return 1; };
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<bool> first{true};
    api->scan_device = [&] {
        if (first.exchange(false)) {
            entered.set_value();
            gate.wait();
        }
        return FakeLib::Scan();
    };
    auto enumerating = std::async(std::launch::async, [&] {
        return linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}});
    });
    entered.get_future().wait();
    auto opening = std::async(std::launch::async, [&] {
        return std::make_unique<VendorCanFdTransport>(VendorCanFdTransport::Config{}, api);
    });
    CHECK(opening.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    release.set_value();
    REQUIRE(enumerating.get().channels.size() == 1);
    auto transport = opening.get();
    CHECK(lib.scan_calls == 2);
    const auto result = linkerbot::can::enumerate_vendor_canfd_channels(api, {{0, 0, {}}});
    REQUIRE(result.failures.size() == 1);
    CHECK(result.failures[0].stage == "busy");
    CHECK(lib.scan_calls == 2);
}

TEST_CASE("throwing process initialization leaves no stale lifecycle entry",
          "[vendor_canfd][lifecycle]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    api->lib_init = []() -> int { throw std::runtime_error("init exception"); };
    CHECK_THROWS_AS(VendorCanFdTransport({}, api), linkerbot::can::TransportIOError);
    CHECK(life::snapshot().empty());
    api->lib_init = &FakeLib::LibInit;
    VendorCanFdTransport recovered({}, api);
}

TEST_CASE("vendor send timeout has a finite upper bound", "[vendor_canfd][stop]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport::Config config;
    config.send_timeout_ms = linkerbot::can::kVendorCanFdMaxSendTimeoutMs + 1;
    CHECK_THROWS_AS(VendorCanFdTransport(config, api), linkerbot::can::ValidationError);
    CHECK(lib.init_calls == 0);
    CHECK(lib.open_calls == 0);
}

TEST_CASE("vendor lifecycle validates API identity and symmetric initialization",
          "[vendor_canfd][validation]") {
    using namespace linkerbot::can;
    LifecycleGuard guard;
    REQUIRE_THROWS_AS(life::acquire(nullptr), ValidationError);
    auto empty = std::make_shared<VendorCanFdApi>();
    REQUIRE_THROWS_AS(life::acquire(empty), ValidationError);
    FakeLib lib;
    auto api = make_api(lib);
    api->lib_exit = nullptr;
    REQUIRE_THROWS_AS(life::acquire(api), ValidationError);
    REQUIRE_NOTHROW(life::release(""));
    REQUIRE_NOTHROW(life::release("unowned library"));
    REQUIRE_THROWS_AS(VendorCanFdTransport({}, nullptr), ValidationError);
    api = make_api(lib);
    api->receive = nullptr;
    REQUIRE_THROWS_AS(VendorCanFdTransport({}, api), ValidationError);
    VendorCanFdTransport::Config config;
    config.send_timeout_ms = -1;
    REQUIRE_THROWS_AS(VendorCanFdTransport(config, make_api(lib)), ValidationError);
}

TEST_CASE("vendor rejects open failure wrong channel type and overreported receive counts",
          "[vendor_canfd][faults]") {
    using namespace linkerbot::can;
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    lib.open_result = -7;
    REQUIRE_THROWS_AS(VendorCanFdTransport({}, api), TransportIOError);
    REQUIRE(life::snapshot().empty());
    lib.open_result = 0;
    api->channel_type = [](unsigned, unsigned) { return 0; };
    REQUIRE_THROWS_AS(VendorCanFdTransport({}, api), TransportIOError);
    REQUIRE(life::snapshot().empty());
    api->channel_type = nullptr;
    api->receive = [](unsigned, unsigned, CanFDMsg *, unsigned count, int) {
        return int(count + 1);
    };
    VendorCanFdTransport transport({}, api);
    REQUIRE(transport.prefers_write_batch());
    REQUIRE_THROWS_AS(transport.read(), TransportIOError);
    transport.close();
}

TEST_CASE("vendor classifies API exceptions and ignores remote-only receives",
          "[vendor_canfd][faults]") {
    using namespace linkerbot::can;
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    api->scan_device = []() -> int { throw ValidationError("injected invalid scan"); };
    REQUIRE_THROWS_AS(VendorCanFdTransport({}, api), ValidationError);
    api->scan_device = []() -> int { throw 42; };
    REQUIRE_THROWS_AS(VendorCanFdTransport({}, api), TransportIOError);
    api = make_api(lib);
    CanFDMsg remote{};
    remote.RemoteFlag = 1;
    lib.rx_frames = {remote};
    VendorCanFdTransport transport({}, api);
    REQUIRE_FALSE(transport.read());
    transport.close();
    auto report = enumerate_vendor_canfd_channels(api, {{2, 0, {}}});
    REQUIRE(report.channels.empty());
    REQUIRE_FALSE(report.failures.empty());
    api->native_channel_numbers = true;
    api->device_level_type = true;
    REQUIRE_THROWS_AS(enumerate_vendor_canfd_channels(api, {{0, 1, {}}}), ValidationError);
    api->native_channel_numbers = false;
    report = enumerate_vendor_canfd_channels(api, {{0, 1, {}}});
    REQUIRE(report.channels.empty());
    REQUIRE_FALSE(report.failures.empty());
}

namespace {
    void count_legacy_unload(void *handle) {
        ++*static_cast<unsigned *>(handle);
    }
} // namespace
TEST_CASE("vendor legacy module ownership unloads once after the last release", "[vendor]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    unsigned unloads = 0;
    api->library_handle = &unloads;
    api->unload = count_legacy_unload;
    life::acquire(api);
    life::release(api->lifecycle_key);
    REQUIRE(unloads == 1);
    REQUIRE(api->library_handle == nullptr);
    life::release(api->lifecycle_key);
    REQUIRE(unloads == 1);
}

TEST_CASE("Vendor preserves raw timestamp counters and explicit classic frames", "[vendor]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport transport({}, api);
    for (const uint32_t stamp : {uint32_t{0}, uint32_t{0xffffffff}, uint32_t{1}}) {
        CanFDMsg raw{};
        raw.ID = 0x123;
        raw.TimeStamp = stamp;
        lib.rx_frames = {raw};
        auto received = require_value(transport.read());
        const auto timestamp = require_value(received.timestamp);
        CHECK(timestamp.value == stamp);
        CHECK(timestamp.counter_bits == 32);
        CHECK(timestamp.unit == linkerbot::can::TimestampUnit::kRawTicks);
        CHECK(timestamp.clock == linkerbot::can::TimestampClock::kVendorRaw);
        CHECK_FALSE(received.is_fd);
        received.format = linkerbot::can::FrameFormat::kClassic;
        transport.write(received);
        CHECK(lib.last_tx.FrameType == 0);
        CHECK(lib.last_tx.TimeStamp == 0);
    }
    Message classic;
    classic.format = linkerbot::can::FrameFormat::kClassic;
    Message invalid = classic;
    invalid.dlc = 9;
    const auto calls = lib.transmit_calls.load();
    REQUIRE_THROWS_AS(transport.write_batch({classic, invalid}), linkerbot::can::ValidationError);
    CHECK(lib.transmit_calls.load() == calls);
    transport.write_batch({classic, Message{}});
    REQUIRE(lib.last_batch.size() == 2);
    CHECK(lib.last_batch[0].FrameType == 0);
    CHECK(lib.last_batch[1].FrameType == kFrameTypeFd);
}

TEST_CASE("Vendor echo opt-in is rejected before library loading or device setup", "[vendor]") {
    LifecycleGuard guard;
    FakeLib lib;
    auto api = make_api(lib);
    VendorCanFdTransport::Config config;
    config.library_path = "/nonexistent/vendor-library";
    config.receive_own_messages = true;
    REQUIRE_THROWS_AS(VendorCanFdTransport(config), linkerbot::can::ValidationError);
    REQUIRE_THROWS_AS(VendorCanFdTransport(config, api), linkerbot::can::ValidationError);
    CHECK(lib.init_calls.load() == 0);
    CHECK(lib.open_calls.load() == 0);
}
