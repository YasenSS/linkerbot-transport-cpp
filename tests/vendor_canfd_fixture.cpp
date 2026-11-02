// Independent native ABI fixture. Deliberately does not include SDK ABI headers:
// a production adapter using the wrong argument positions must fail this DLL.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define API extern "C" __declspec(dllexport) int __stdcall
#define INDEX unsigned device
#define CHECK_CHANNEL
#define CHANNEL 0U
#else
#define API extern "C" __attribute__((visibility("default"))) int
#define INDEX unsigned device, unsigned channel
#define CHECK_CHANNEL                                                                              \
    if (channel > 1)                                                                               \
        return -91;
#define CHANNEL channel
#endif

namespace {
    struct Configuration {
        uint32_t nominal, data;
        uint16_t nominal_prescaler;
        uint8_t nominal_tseg1, nominal_tseg2, nominal_sjw;
        uint8_t data_prescaler, data_tseg1, data_tseg2, data_sjw;
        uint8_t flags, mode, type;
    };
    struct Frame {
        uint32_t id, timestamp;
        uint8_t format, dlc, extended, remote, bus_status, error_status, tx_errors, rx_errors;
        uint8_t payload[64];
    };
    static_assert(sizeof(Configuration) == 20 && sizeof(Frame) == 80);
    // Process-wide mutable state models one loaded vendor driver instance.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    std::mutex mutex;
    std::array<int, 32> values{};
    std::array<std::array<bool, 2>, 6> opened{};
    std::array<std::array<std::deque<Frame>, 2>, 6> queues;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
    // Counters 0=scan 1=open 2=close 3=config 4=tx 5=rx 6=init 7=exit;
    // controls 8=fail close 9=fail config 10=fail open 11=IO delay;
    // observations 12=last dev 13=last count 14=last timeout 15=nominal 16=data;
    // controls 17=scan result override (0 -> 6), 18=short tx; observation 19=channel.
    void observe(unsigned device, unsigned channel) {
        values[12] = static_cast<int>(device);
        values[19] = static_cast<int>(channel);
    }
} // namespace
// These names are the dynamic fixture ABI consumed by the loader tests.
// NOLINTBEGIN(readability-identifier-naming)
API Test_Get(unsigned key) {
    std::lock_guard<std::mutex> lock(mutex);
    return key < values.size() ? values[key] : -99;
}
API Test_Set(unsigned key, int value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (key >= values.size()) {
        return -99;
    }
    values[key] = value;
    return 0;
}
API CAN_ScanDevice() {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[0];
    return (values[17] != 0) ? values[17] : 6;
}
#if !defined(_WIN32) && !defined(FIXTURE_LEGACY)
API LibCANbus_Init() {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[6];
    return 0;
}
#ifndef FIXTURE_PARTIAL
API LibCANbus_Exit() {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[7];
    return 0;
}
#endif
API CAN_GetCHType(unsigned device, unsigned channel) {
    const int types[] = {1, 0, 2, 99, -7, 1};
    return device < 6 && channel <= 1 ? types[device] : -9;
}
#else
// Match the misspelling in the examined Windows vendor DLL.
#if defined(_WIN32)
API CAN_GeDevType(unsigned device) {
#else
API CAN_GetDevType(unsigned device) {
#endif
    const int types[] = {1, 0, 2, 99, -7, 1};
    return device < 6 ? types[device] : -9;
}
#endif
API CAN_OpenDevice(INDEX) {
    CHECK_CHANNEL
    std::lock_guard<std::mutex> lock(mutex);
    ++values[1];
    observe(device, CHANNEL);
    if (values[10] != 0) {
        return values[10];
    }
    if (device >= 6 || opened[device][CHANNEL]) {
        return -2;
    }
    opened[device][CHANNEL] = true;
    return 0;
}
API CAN_CloseDevice(INDEX) {
    CHECK_CHANNEL
    std::lock_guard<std::mutex> lock(mutex);
    ++values[2];
    observe(device, CHANNEL);
    if (values[8] != 0) {
        return values[8];
    }
    if (device >= 6 || !opened[device][CHANNEL]) {
        return -3;
    }
    opened[device][CHANNEL] = false;
    queues[device][CHANNEL].clear();
    return 0;
}
API CANFD_Init(INDEX, Configuration *config) {
    CHECK_CHANNEL
    std::lock_guard<std::mutex> lock(mutex);
    ++values[3];
    observe(device, CHANNEL);
    if ((config == nullptr) || device >= 6 || !opened[device][CHANNEL]) {
        return -4;
    }
    values[15] = static_cast<int>(config->nominal);
    values[16] = static_cast<int>(config->data);
    if (values[9] != 0) {
        return values[9];
    }
    return config->nominal == 1000000 && config->data == 5000000 && config->type == 1 ? 0 : -5;
}
#ifdef FIXTURE_DEPENDENT
extern "C" int fixture_dependency();
#endif
API CANFD_Transmit(INDEX, Frame *frames, unsigned count, int timeout_ms) {
    CHECK_CHANNEL
    std::unique_lock<std::mutex> lock(mutex);
    ++values[4];
    observe(device, CHANNEL);
    values[13] = static_cast<int>(count);
    values[14] = timeout_ms;
#ifdef FIXTURE_DEPENDENT
    if (fixture_dependency() != 42)
        return -50;
#endif
    if ((frames == nullptr) || device >= 6 || !opened[device][CHANNEL] || count > 100 ||
        timeout_ms < 0 || timeout_ms > 100) {
        return -6;
    }
    const auto sent = (values[18] != 0) ? count - 1 : count;
    for (unsigned i = 0; i < sent; ++i) {
        queues[device][CHANNEL].push_back(frames[i]);
    }
    const int delay = std::min(values[11], timeout_ms);
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    return static_cast<int>(sent);
}
#ifndef FIXTURE_MISSING_RX
API CANFD_Receive(INDEX, Frame *frames, unsigned count, int timeout_ms) {
    CHECK_CHANNEL
    std::unique_lock<std::mutex> lock(mutex);
    ++values[5];
    observe(device, CHANNEL);
    values[13] = static_cast<int>(count);
    values[14] = timeout_ms;
    if ((frames == nullptr) || device >= 6 || !opened[device][CHANNEL] || count != 64 ||
        timeout_ms < 0 || timeout_ms > 10) {
        return -7;
    }
    const auto size = std::min(count, static_cast<unsigned>(queues[device][CHANNEL].size()));
    for (unsigned i = 0; i < size; ++i) {
        frames[i] = queues[device][CHANNEL].front();
        queues[device][CHANNEL].pop_front();
    }
    const int delay = std::min(values[11], timeout_ms);
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    return static_cast<int>(size);
}
#endif
// NOLINTEND(readability-identifier-naming)
