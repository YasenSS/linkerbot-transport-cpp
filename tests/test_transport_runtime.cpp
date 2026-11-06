#include <catch2/catch_test_macros.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/runtime.hpp>

#include <atomic>
#include <future>

using namespace std::chrono_literals;
using namespace linkerbot;

namespace {
    transport::ChannelSpec channel(const std::string &name) {
        transport::ChannelSpec spec;
        spec.key.backend = "fake";
        spec.key.endpoint = name;
        spec.configuration_fingerprint = "fake/default";
        return spec;
    }
    struct CountedFake : can::FakeTransport {
        std::shared_ptr<std::atomic<int>> closes;
        explicit CountedFake(std::shared_ptr<std::atomic<int>> counter)
            : closes(std::move(counter)) {}
        void close() override {
            ++*closes;
            can::FakeTransport::close();
        }
    };
} // namespace

TEST_CASE("Runtime instances share a connection until the final logical lease", "[runtime]") {
    transport::TransportRuntime first;
    transport::TransportRuntime second;
    const auto spec = channel("runtime/shared");
    auto closes = std::make_shared<std::atomic<int>>(0);
    int opens = 0;
    const auto factory = [&] {
        ++opens;
        return std::make_unique<CountedFake>(closes);
    };
    REQUIRE(first.active_devices(spec.key) == 0);
    REQUIRE_THROWS_AS(first.channel_health(spec.key), can::ClosedError);
    REQUIRE_THROWS_AS(first.recover_channel(spec.key, 0), can::ClosedError);
    auto left = first.open_device(spec, {"left", {}}, factory);
    auto right = second.open_device(spec, {"right", {}}, factory);
    REQUIRE(opens == 1);
    REQUIRE(first.active_devices(spec.key) == 2);
    const auto instance = left->health().channel_instance;
    left->close_until(transport::Deadline::max());
    REQUIRE(closes->load() == 0);
    right->submit({{{}}}).wait();
    REQUIRE(second.active_devices(spec.key) == 1);
    right->close_until(transport::Deadline::max());
    REQUIRE(closes->load() == 1);
    REQUIRE(first.active_devices(spec.key) == 0);
    auto replacement = first.open_device(spec, {"left", {}}, factory);
    REQUIRE(opens == 2);
    REQUIRE(replacement->health().channel_instance != instance);
    REQUIRE_THROWS_AS(left->submit({{{}}}), can::ClosedError);
}

TEST_CASE("Device rebind keeps the physical connection and invalidates the old handle",
          "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/rebind");
    int opens = 0;
    auto device = runtime.open_device(spec, {"hand", {}}, [&] {
        ++opens;
        return std::make_unique<can::FakeTransport>();
    });
    const auto initial = device->health();
    REQUIRE_THROWS_AS(runtime.rebind_device(spec.key, {}, {"hand", {}}, transport::Deadline::max()),
                      can::ValidationError);
    REQUIRE_THROWS_AS(runtime.rebind_device(spec.key, device, {"", {}}, transport::Deadline::max()),
                      can::ValidationError);
    auto rebound =
        runtime.rebind_device(spec.key, device, {"hand", {}}, transport::Deadline::max());
    REQUIRE(opens == 1);
    REQUIRE(runtime.active_devices(spec.key) == 1);
    REQUIRE(rebound->health().generation == initial.generation);
    REQUIRE(rebound->health().channel_instance == initial.channel_instance);
    REQUIRE(device->closed());
    REQUIRE_THROWS_AS(device->begin_operation(), can::ClosedError);
    REQUIRE_THROWS_AS(device->subscribe_rules({{0, 0x7ff, false}}), can::ClosedError);
    REQUIRE(rebound->submit({{{}}}).wait_result_until(transport::Deadline::max()).outcome ==
            transport::SendOutcome::dispatched);
    const auto before = rebound->generation();
    runtime.recover_channel(spec.key, before);
    REQUIRE_THROWS_AS(rebound->begin_operation(), can::ClosedError);
    REQUIRE_THROWS_AS(rebound->observe_rules({{0, 0, false}}, 1), can::ClosedError);
    REQUIRE_THROWS_AS(rebound->subscribe_rules({{0, 0x7ff, false}}), can::ClosedError);
    auto restored =
        runtime.rebind_device(spec.key, rebound, {"hand", {}}, transport::Deadline::max());
    REQUIRE(opens == 2);
    REQUIRE(restored->generation() != before);
}

TEST_CASE("Runtime rejects conflicting profiles without opening a second backend", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/config");
    int opens = 0;
    const auto factory = [&] {
        ++opens;
        return std::make_unique<can::FakeTransport>();
    };
    auto device = runtime.open_device(spec, {"one", {}}, factory);
    auto conflict = spec;
    conflict.configuration_fingerprint = "other driver options";
    REQUIRE_THROWS_AS(runtime.open_device(conflict, {"two", {}}, factory), can::ValidationError);
    conflict = spec;
    ++conflict.scheduling.fifo_capacity;
    REQUIRE_THROWS_AS(runtime.open_device(conflict, {"two", {}}, factory), can::ValidationError);
    conflict = spec;
    conflict.key.fd_mode = true;
    REQUIRE_THROWS_AS(runtime.open_device(conflict, {"two", {}}, factory), can::ValidationError);
    REQUIRE_THROWS_AS(runtime.open_device(spec, {"one", {}}, factory), can::ValidationError);
    REQUIRE(opens == 1);
    REQUIRE(runtime.active_devices(spec.key) == 1);
}

TEST_CASE("Runtime keeps failed final shutdown reserved for retry", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/retry-close");
    can::FakeTransport *backend = nullptr;
    int opens = 0;
    const auto factory = [&] {
        ++opens;
        auto fresh = std::make_unique<can::FakeTransport>();
        backend = fresh.get();
        return fresh;
    };
    auto device = runtime.open_device(spec, {"one", {}}, factory);
    backend->fail_next_closes(1);
    REQUIRE_THROWS_AS(device->close_until(transport::Deadline::max()), can::CANError);
    REQUIRE_FALSE(device->closed());
    REQUIRE(runtime.active_devices(spec.key) == 1);
    REQUIRE_THROWS_AS(runtime.open_device(spec, {"two", {}}, factory), can::FaultedError);
    REQUIRE(opens == 1);
    device->close_until(transport::Deadline::max());
    REQUIRE(device->closed());
    auto replacement = runtime.open_device(spec, {"one", {}}, factory);
    REQUIRE(opens == 2);
}

TEST_CASE("Runtime initialization failure and same-key reentry are retryable", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/init-failure");
    auto factory = [] { return std::make_unique<can::FakeTransport>(); };
    auto invalid = spec;
    invalid.configuration_fingerprint.clear();
    REQUIRE_THROWS_AS(runtime.open_device(invalid, {"one", {}}, factory), can::ValidationError);
    REQUIRE_THROWS_AS(runtime.open_device(spec, {"", {}}, factory), can::ValidationError);
    REQUIRE_THROWS_AS(runtime.open_device(spec, {"one", {}}, {}), can::ValidationError);
    REQUIRE_THROWS_AS(runtime.open_device(spec, {"one", {}},
                                          [&]() -> std::unique_ptr<can::CANTransport> {
                                              (void)runtime.open_device(spec, {"nested", {}},
                                                                        factory);
                                              return factory();
                                          }),
                      can::ValidationError);
    auto device = runtime.open_device(spec, {"one", {}}, factory);
    REQUIRE(runtime.active_devices(spec.key) == 1);
}

TEST_CASE("Runtime concurrent opens publish one configured Session", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/concurrent");
    std::promise<void> entered;
    std::promise<void> release;
    auto ready = entered.get_future();
    auto resume = release.get_future().share();
    std::atomic<int> opens{0};
    const auto factory = [&] {
        ++opens;
        entered.set_value();
        resume.wait();
        return std::make_unique<can::FakeTransport>();
    };
    auto one = std::async(std::launch::async,
                          [&] { return runtime.open_device(spec, {"one", {}}, factory); });
    const auto started = ready.wait_for(2s);
    auto two = std::async(std::launch::async,
                          [&] { return runtime.open_device(spec, {"two", {}}, factory); });
    const auto waiting = two.wait_for(10ms);
    release.set_value();
    REQUIRE(started == std::future_status::ready);
    REQUIRE(waiting == std::future_status::timeout);
    auto first = one.get();
    auto second = two.get();
    REQUIRE(opens.load() == 1);
    REQUIRE(runtime.active_devices(spec.key) == 2);
    first->submit({{{}}}).wait();
    second->submit({{{}}}).wait();
}

TEST_CASE("Runtime reset policy protects reconstruction-only devices", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/recovery-policy");
    int opens = 0;
    const auto factory = [&] {
        ++opens;
        return std::make_unique<can::FakeTransport>();
    };
    transport::DeviceSpec arm_spec{"arm", {}};
    arm_spec.channel_reset = transport::ChannelResetPolicy::kRequireDetach;
    auto arm = runtime.open_device(spec, arm_spec, factory);
    auto hand = runtime.open_device(spec, {"hand", {}}, factory);
    const auto generation = runtime.channel_health(spec.key).generation;
    REQUIRE_THROWS_AS(runtime.recover_channel(spec.key, generation), can::ValidationError);
    REQUIRE(opens == 1);
    arm->close_until(transport::Deadline::max());
    const auto recovered = runtime.recover_channel(spec.key, generation);
    REQUIRE(recovered != generation);
    REQUIRE(opens == 2);
    REQUIRE_FALSE(hand->health().accepting);
    REQUIRE_THROWS_AS(hand->submit({{{}}}), can::ClosedError);
    REQUIRE(runtime.recover_channel(spec.key, generation) == recovered);
    REQUIRE(opens == 2);
}

TEST_CASE("Runtime permits a new attempt after failed recovery", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/recovery-retry");
    int opens = 0;
    const auto factory = [&] {
        if (++opens == 2) {
            throw can::TransportIOError("reopen failed");
        }
        return std::make_unique<can::FakeTransport>();
    };
    auto device = runtime.open_device(spec, {"hand", {}}, factory);
    const auto generation = runtime.channel_health(spec.key).generation;
    REQUIRE_THROWS_AS(runtime.recover_channel(spec.key, generation), can::CANError);
    REQUIRE_FALSE(runtime.channel_health(spec.key).open);
    REQUIRE(runtime.recover_channel(spec.key, generation) != generation);
    REQUIRE(opens == 3);
    REQUIRE_FALSE(device->health().accepting);
}

TEST_CASE("Runtime destruction retries failed physical cleanup", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto spec = channel("runtime/destructor-retry");
    auto closes = std::make_shared<std::atomic<int>>(0);
    auto device = runtime.open_device(spec, {"hand", {}}, [&] {
        auto backend = std::make_unique<CountedFake>(closes);
        backend->fail_next_closes(2);
        return backend;
    });
    device.reset();
    can::CANBusLease::drain_deferred_cleanup();
    REQUIRE(closes->load() >= 3);
    REQUIRE(runtime.active_devices(spec.key) == 0);
}

TEST_CASE("Runtime rejects a cycle between channel factories", "[runtime]") {
    transport::TransportRuntime runtime;
    const auto left = channel("runtime/cycle-left");
    const auto right = channel("runtime/cycle-right");
    std::promise<void> left_entered;
    std::promise<void> right_entered;
    auto left_ready = left_entered.get_future().share();
    auto right_ready = right_entered.get_future().share();
    const auto basic = [] { return std::make_unique<can::FakeTransport>(); };
    auto first = std::async(std::launch::async, [&] {
        try {
            auto device = runtime.open_device(left, {"outer", {}}, [&] {
                left_entered.set_value();
                right_ready.wait();
                auto nested = runtime.open_device(right, {"nested", {}}, basic);
                return basic();
            });
            return true;
        } catch (const can::ValidationError &) {
            return false;
        }
    });
    auto second = std::async(std::launch::async, [&] {
        try {
            auto device = runtime.open_device(right, {"outer", {}}, [&] {
                right_entered.set_value();
                left_ready.wait();
                auto nested = runtime.open_device(left, {"nested", {}}, basic);
                return basic();
            });
            return true;
        } catch (const can::ValidationError &) {
            return false;
        }
    });
    REQUIRE(first.wait_for(2s) == std::future_status::ready);
    REQUIRE(second.wait_for(2s) == std::future_status::ready);
    const bool first_opened = first.get();
    const bool second_opened = second.get();
    REQUIRE(first_opened != second_opened);
    REQUIRE(runtime.active_devices(left.key) == 0);
    REQUIRE(runtime.active_devices(right.key) == 0);
}
