#include <catch2/catch_test_macros.hpp>
#include <linkerbot/can/vendor_canfd_transport.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define CALL __stdcall
#else
#include <dlfcn.h>
#define CALL
#endif

using namespace linkerbot::can;
namespace fs = std::filesystem;
namespace {
std::string fixture_path(const char *name = "LINKERBOT_TEST_VENDOR_LIBRARY") {
    const auto value = std::getenv(name);
    if (!value) throw std::runtime_error(std::string("CTest fixture path missing: ") + name);
    return value;
}
struct Fixture {
    std::shared_ptr<VendorCanFdApi> api = load_vendor_canfd_api(fixture_path());
    using Get = int (CALL *)(unsigned);
    using Set = int (CALL *)(unsigned, int);
    Get get;
    Set set;
    Fixture() {
#if defined(_WIN32)
        get = reinterpret_cast<Get>(GetProcAddress(static_cast<HMODULE>(api->library_owner.get()), "Test_Get"));
        set = reinterpret_cast<Set>(GetProcAddress(static_cast<HMODULE>(api->library_owner.get()), "Test_Set"));
#else
        get = reinterpret_cast<Get>(dlsym(api->library_owner.get(), "Test_Get"));
        set = reinterpret_cast<Set>(dlsym(api->library_owner.get(), "Test_Set"));
#endif
        if (!get || !set) throw std::runtime_error("Fixture control symbols missing");
        for (unsigned key = 0; key < 32; ++key) set(key, 0);
    }
    ~Fixture() {
        set(8, 0); set(9, 0); set(10, 0);
        try { vendor_canfd_lifecycle::retry_deferred_cleanup(api->lifecycle_key); } catch (...) {}
    }
};
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() /
        fs::u8path("linkerbot MiniCANFD 中文 " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { fs::create_directories(path); }
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
std::vector<VendorCanFdChannel> all_channels() {
    std::vector<VendorCanFdChannel> result;
    for (unsigned i = 0; i < 6; ++i) result.push_back({i, 0, {}});
    return result;
}
}

TEST_CASE("native loader verifies FD types without opening channels", "[vendor_loader]") {
    Fixture fixture;
    const auto result = enumerate_vendor_canfd_channels(fixture.api, all_channels());
    REQUIRE(result.channels.size() == 2);
    CHECK(result.channels[0].device_index == 0);
    CHECK(result.channels[1].device_index == 5);
    CHECK(result.channels[1].channel_index == 0);
    CHECK(result.channels[0].library_path == fixture.api->lifecycle_key);
    REQUIRE(result.failures.size() == 4);
    CHECK(result.failures[0].native_status == 0);
    CHECK(result.failures[1].native_status == 2);
    CHECK(result.failures[2].native_status == 99);
    CHECK(result.failures[3].native_status == -7);
    CHECK(fixture.get(0) == 1);
    CHECK(fixture.get(1) == 0);
    CHECK(fixture.get(3) == 0);
    CHECK(fixture.get(4) == 0);
#if defined(_WIN32)
    CHECK_FALSE(fixture.api->lib_init);
    CHECK_FALSE(fixture.api->lib_exit);
    CHECK(fixture.get(6) == 0);
    CHECK(fixture.get(7) == 0);
    CHECK(enumerate_vendor_canfd_channels(fixture.api).channels.size() == 2);
#else
    CHECK(fixture.get(6) == 1);
    CHECK(fixture.get(7) == 1);
    const auto unspecified = enumerate_vendor_canfd_channels(fixture.api);
    REQUIRE(unspecified.failures.size() == 1);
    CHECK(unspecified.failures[0].stage == "layout");
#endif
}

TEST_CASE("native ABI transfers all payload lengths and frame flags", "[vendor_loader]") {
    Fixture fixture;
    VendorCanFdTransport::Config config;
    config.device_index = 5;
    VendorCanFdTransport transport(config, fixture.api);
    CHECK(fixture.get(12) == 5);
    CHECK(fixture.get(15) == 1000000);
    CHECK(fixture.get(16) == 5000000);
    CHECK_FALSE(transport.read());
    for (const bool extended : {false, true}) for (const bool brs : {false, true}) {
        std::vector<Message> messages;
        for (unsigned length = 0; length <= 64; ++length) {
            Message message;
            message.id = extended ? 0x1234567 : 0x123;
            message.is_extended = extended;
            message.is_fd = true;
            message.is_brs = brs; // Includes O30 standard/no BRS and L30 extended FD.
            message.dlc = static_cast<uint8_t>(length);
            for (unsigned i = 0; i < length; ++i) message.data[i] = static_cast<uint8_t>(i + length);
            messages.push_back(message);
        }
        transport.write(messages.front());
        CHECK(fixture.get(13) == 1);
        REQUIRE(transport.read());
        transport.write_batch(messages);
        CHECK(fixture.get(13) == 65);
        for (const auto &sent : messages) {
            const auto received = transport.read();
            REQUIRE(received);
            CHECK(received->id == sent.id);
            CHECK(received->is_extended == extended);
            CHECK(received->is_fd);
            CHECK(received->is_brs == brs);
            CHECK(received->dlc == vendor_dlc_to_length(vendor_length_to_dlc(sent.dlc)));
            CHECK(received->data == sent.data); // Includes zero padding of noncanonical lengths.
        }
        CHECK_FALSE(transport.read());
    }
    CanFDMsg classic{};
    classic.ID = 0x321; classic.DLC = 8; classic.Data[7] = 42;
    REQUIRE(fixture.api->transmit(5, 0, &classic, 1, 10) == 1);
    const auto received = transport.read();
    REQUIRE(received);
    CHECK_FALSE(received->is_fd);
    CHECK_FALSE(received->is_brs);
    CHECK(received->data[7] == 42);
    transport.close();
}

TEST_CASE("scan snapshots and channel claims survive failures", "[vendor_loader]") {
    Fixture fixture;
    VendorCanFdTransport first({}, fixture.api);
    REQUIRE(fixture.get(0) == 1);
    fixture.set(17, 1); // Simulated plug change must not refresh an active snapshot.
    VendorCanFdTransport::Config other;
    other.device_index = 5;
    VendorCanFdTransport second(other, fixture.api);
    CHECK(fixture.get(0) == 1);
    CHECK_THROWS_AS(VendorCanFdTransport({}, fixture.api), FaultedError);
    CHECK(fixture.get(1) == 2);
    auto busy = enumerate_vendor_canfd_channels(fixture.api, all_channels());
    REQUIRE(busy.failures.size() == 1);
    CHECK(busy.failures[0].stage == "busy");
    CHECK(fixture.get(0) == 1);
    fixture.set(8, -8);
    CHECK_THROWS_AS(first.close(), TransportIOError);
    CHECK_THROWS_AS(VendorCanFdTransport({}, fixture.api), FaultedError);
    CHECK(enumerate_vendor_canfd_channels(fixture.api, all_channels()).channels.empty());
    fixture.set(8, 0);
    first.close();
    second.close();
    fixture.set(17, 0);
    CHECK(enumerate_vendor_canfd_channels(fixture.api, all_channels()).channels.size() == 2);
    CHECK(fixture.get(0) == 2);
    VendorCanFdTransport reopened({}, fixture.api);
    CHECK(fixture.get(0) == 3);
}

TEST_CASE("native init rollback retains a failed physical close", "[vendor_loader]") {
    Fixture fixture;
    fixture.set(9, -9); fixture.set(8, -8);
    CHECK_THROWS_AS(VendorCanFdTransport({}, fixture.api), TransportIOError);
    CHECK(vendor_canfd_lifecycle::pending_cleanup_count(fixture.api->lifecycle_key) == 1);
    const auto failed = enumerate_vendor_canfd_channels(fixture.api, all_channels());
    REQUIRE_FALSE(failed.failures.empty());
    CHECK(failed.failures[0].stage == "cleanup");
    CHECK(fixture.get(0) == 1);
    fixture.set(8, 0); fixture.set(9, 0);
    VendorCanFdTransport recovered({}, fixture.api);
    CHECK(vendor_canfd_lifecycle::pending_cleanup_count(fixture.api->lifecycle_key) == 0);
    CHECK(fixture.get(0) == 2);
}

TEST_CASE("native read write and close finish within configured waits", "[vendor_loader]") {
    Fixture fixture;
    fixture.set(11, 10);
    VendorCanFdTransport transport({}, fixture.api);
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto reading = std::async(std::launch::async, [&] {
        ready.wait();
        try { for (int i = 0; i < 100; ++i) (void)transport.read(); } catch (const ClosedError &) {}
    });
    auto writing = std::async(std::launch::async, [&] {
        ready.wait();
        try { Message message; message.id = 1; for (int i = 0; i < 100; ++i) transport.write(message); }
        catch (const ClosedError &) {}
    });
    start.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((fixture.get(4) == 0 || fixture.get(5) == 0) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(fixture.get(4) > 0);
    CHECK(fixture.get(5) > 0);
    const auto before = std::chrono::steady_clock::now();
    transport.close();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
    CHECK(reading.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    CHECK(writing.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    reading.get(); writing.get();
    transport.close();
    CHECK(fixture.get(2) == 1);
}

TEST_CASE("explicit Unicode module paths resolve without fallback", "[vendor_loader]") {
    TemporaryDirectory directory;
    const auto path = directory.path / fs::u8path("测试库 with spaces" + fs::path(fixture_path()).extension().string());
    fs::copy_file(fs::u8path(fixture_path()), path);
    auto api = load_vendor_canfd_api(path.u8string());
    CHECK(fs::equivalent(fs::u8path(api->lifecycle_key), path));
    const auto absent = (directory.path / "missing.dll").u8string();
#if defined(_WIN32)
    const auto previous_size = GetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", nullptr, 0);
    std::wstring previous(previous_size, L'\0');
    if (previous_size) { const auto n = GetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", previous.data(), previous_size); previous.resize(n); }
    SetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", path.c_str());
    try {
        CHECK_THROWS_AS(load_vendor_canfd_api(absent), TransportIOError);
        auto from_env = load_vendor_canfd_api(std::nullopt);
        CHECK(fs::equivalent(fs::u8path(from_env->lifecycle_key), path));
    } catch (...) { SetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", previous_size ? previous.c_str() : nullptr); throw; }
    SetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", previous_size ? previous.c_str() : nullptr);
#else
    const char *old = std::getenv("LINKERBOT_CANFD_LIB");
    const std::optional<std::string> previous = old ? std::optional<std::string>(old) : std::nullopt;
    setenv("LINKERBOT_CANFD_LIB", path.c_str(), 1);
    CHECK_THROWS_AS(load_vendor_canfd_api(absent), TransportIOError);
    CHECK(fs::equivalent(fs::u8path(load_vendor_canfd_api(std::nullopt)->lifecycle_key), path));
    if (previous) setenv("LINKERBOT_CANFD_LIB", previous->c_str(), 1); else unsetenv("LINKERBOT_CANFD_LIB");
#endif
    CHECK_THROWS_AS(load_vendor_canfd_api(std::string{}), ValidationError);
    CHECK_THROWS_AS(load_vendor_canfd_api(std::string("bad\0name", 8)), ValidationError);
}

TEST_CASE("native loader reports missing symbols and invalid binaries", "[vendor_loader]") {
    try {
        (void)load_vendor_canfd_api(fixture_path("LINKERBOT_TEST_VENDOR_MISSING"));
        FAIL("missing CANFD_Receive accepted");
    } catch (const TransportIOError &error) {
        CHECK(std::string(error.what()).find("CANFD_Receive") != std::string::npos);
#if defined(_WIN32)
        CHECK(error.native_status() == ERROR_PROC_NOT_FOUND);
        CHECK(error.system_error().value() == ERROR_PROC_NOT_FOUND);
#endif
    }
    TemporaryDirectory directory;
    const auto invalid = directory.path / "invalid.dll";
    { std::ofstream stream(invalid, std::ios::binary); stream << "invalid binary"; }
    CHECK_THROWS_AS(load_vendor_canfd_api(invalid.u8string()), TransportIOError);
#if defined(_WIN32)
    const auto wrong_machine = directory.path / "wrong-machine.dll";
    fs::copy_file(fs::u8path(fixture_path()), wrong_machine);
    {
        std::fstream stream(wrong_machine, std::ios::binary | std::ios::in | std::ios::out);
        uint32_t pe_offset = 0;
        stream.seekg(0x3c); stream.read(reinterpret_cast<char *>(&pe_offset), sizeof(pe_offset));
        REQUIRE(stream.good());
        const uint16_t arm64_machine = 0xaa64;
        stream.seekp(pe_offset + 4); stream.write(reinterpret_cast<const char *>(&arm64_machine), sizeof(arm64_machine));
    }
    try { (void)load_vendor_canfd_api(wrong_machine.u8string()); FAIL("wrong machine accepted"); }
    catch (const TransportIOError &error) { CHECK(error.native_status() == ERROR_BAD_EXE_FORMAT); }
#endif
}

#if defined(_WIN32)
TEST_CASE("Windows rejects nonzero native channel before calling vendor", "[vendor_loader]") {
    Fixture fixture;
    VendorCanFdTransport::Config config;
    config.channel_index = 1;
    CHECK_THROWS_AS(VendorCanFdTransport(config, fixture.api), ValidationError);
    CHECK_THROWS_AS(VendorCanFdTransport(config), ValidationError);
    CHECK_THROWS_AS(enumerate_vendor_canfd_channels(fixture.api, {{0, 1, {}}}), ValidationError);
    CHECK(fixture.get(0) == 0);
    CHECK(fixture.get(1) == 0);
}
TEST_CASE("Windows resolves colocated dependencies and reports their absence", "[vendor_loader]") {
    TemporaryDirectory directory;
    const auto original = fs::u8path(fixture_path("LINKERBOT_TEST_VENDOR_DEPENDENT"));
    const auto library = directory.path / original.filename();
    fs::copy_file(original, library);
    try { (void)load_vendor_canfd_api(library.u8string()); FAIL("missing dependency accepted"); }
    catch (const TransportIOError &error) { CHECK(error.native_status() == ERROR_MOD_NOT_FOUND); }
    const auto dependency = fs::u8path(fixture_path("LINKERBOT_TEST_VENDOR_DEPENDENCY"));
    fs::copy_file(dependency, directory.path / dependency.filename());
    const auto api = load_vendor_canfd_api(library.u8string());
    CHECK(fs::equivalent(fs::u8path(api->lifecycle_key), library));
}
#else
TEST_CASE("Linux rejects incomplete lifecycle export pairs", "[vendor_loader]") {
    CHECK_THROWS_AS(load_vendor_canfd_api(fixture_path("LINKERBOT_TEST_VENDOR_PARTIAL")), TransportIOError);
    const auto legacy = load_vendor_canfd_api(fixture_path("LINKERBOT_TEST_VENDOR_LEGACY"));
    CHECK_FALSE(legacy->lib_init);
    CHECK_FALSE(legacy->lib_exit);
    CHECK(legacy->device_level_type);
    const auto result = enumerate_vendor_canfd_channels(legacy, {{0, 0, {}}, {0, 1, {}}});
    CHECK(result.channels.size() == 1);
    REQUIRE(result.failures.size() == 1);
    CHECK(result.failures[0].stage == "layout");
}
#endif

TEST_CASE("independent native loads remain owned until the last live user", "[vendor_loader]") {
    auto first_api = load_vendor_canfd_api(fixture_path());
    auto second_api = load_vendor_canfd_api(fixture_path());
    const std::weak_ptr<void> first_owner = first_api->library_owner;
    const std::weak_ptr<void> second_owner = second_api->library_owner;
    auto first = std::make_unique<VendorCanFdTransport>(VendorCanFdTransport::Config{}, first_api);
    VendorCanFdTransport::Config config;
    config.device_index = 5;
    auto second = std::make_unique<VendorCanFdTransport>(config, second_api);
    first_api.reset(); second_api.reset();
    first->close(); first.reset();
    CHECK_FALSE(first_owner.expired()); // shared lifecycle still owns its Init API
    Message message; message.id = 0x123; message.dlc = 1; message.data[0] = 42;
    second->write(message);
    const auto received = second->read();
    REQUIRE(received);
    CHECK(received->data[0] == 42);
    second->close();
    CHECK(first_owner.expired());
    CHECK_FALSE(second_owner.expired()); // this Transport's independent loader reference
    second.reset();
    CHECK(second_owner.expired());
}

#if !defined(_WIN32)
TEST_CASE("native Linux adapter preserves both device and channel indices", "[vendor_loader]") {
    Fixture fixture;
    VendorCanFdTransport first({}, fixture.api);
    VendorCanFdTransport::Config config;
    config.channel_index = 1;
    VendorCanFdTransport second(config, fixture.api);
    CHECK(fixture.get(12) == 0);
    CHECK(fixture.get(19) == 1);
    Message message; message.id = 1; message.dlc = 1; message.data[0] = 17;
    second.write(message);
    CHECK_FALSE(first.read());
    const auto received = second.read();
    REQUIRE(received);
    CHECK(received->data[0] == 17);
    second.close();
    CHECK(fixture.get(19) == 1);
    CHECK(fixture.get(0) == 1);
}
#endif

TEST_CASE("native loader factory entry points honor the explicit environment library",
          "[vendor_loader]") {
    Fixture fixture;
    const auto configured = fixture_path();
    const char *value = std::getenv(kVendorCanFdLibEnv);
    const auto previous = value ? std::optional<std::string>{value} : std::nullopt;
    struct RestoreEnvironment {
        std::optional<std::string> previous;
        ~RestoreEnvironment() {
#if defined(_WIN32)
            _putenv_s(kVendorCanFdLibEnv, previous ? previous->c_str() : "");
#else
            if (previous)
                setenv(kVendorCanFdLibEnv, previous->c_str(), 1);
            else
                unsetenv(kVendorCanFdLibEnv);
#endif
        }
    } restore{previous};
#if defined(_WIN32)
    REQUIRE(_putenv_s(kVendorCanFdLibEnv, configured.c_str()) == 0);
#else
    REQUIRE(setenv(kVendorCanFdLibEnv, configured.c_str(), 1) == 0);
#endif
    {
        auto transport = make_vendor_canfd_transport();
        REQUIRE(transport->prefers_write_batch());
        transport->close();
    }
    VendorCanFdTransport::Config config;
    config.library_path = configured;
    {
        auto transport = make_vendor_canfd_transport(config);
        REQUIRE(transport->device_index() == 0);
        transport->close();
    }
    const auto channels =
        enumerate_vendor_canfd_channels(std::optional<std::string>{configured}, {{0, 0, {}}});
    REQUIRE(channels.channels.size() == 1);
}
