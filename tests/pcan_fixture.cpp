// Independent native PCAN-Basic ABI fixture; no SDK or vendor headers.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#define API extern "C" __declspec(dllexport) uint32_t __stdcall
#else
#define API extern "C" __attribute__((visibility("default"))) uint32_t
#endif
namespace {
    struct Classic {
        uint32_t id;
        uint8_t type, length, data[8];
    };
    struct FD {
        uint32_t id;
        uint8_t type, dlc, data[64];
    };
    struct Stamp {
        uint32_t millis;
        uint16_t overflow, micros;
    };
    struct Info {
        uint16_t handle;
        uint8_t device, controller;
        uint32_t features;
        char name[33];
        uint32_t id, condition;
    };
    static_assert(sizeof(Classic) == 16 && sizeof(FD) == 72 && sizeof(Stamp) == 8 &&
                  sizeof(Info) == 52);
    // Process-wide mutable state models one loaded PCAN driver instance.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    std::mutex mutex;
    std::array<uint32_t, 32> values{};
    std::map<uint16_t, int> modes;
    std::map<uint16_t, std::deque<FD>> receive;
    using Route = std::tuple<uint16_t, uint32_t, unsigned, bool>;
    std::map<Route, std::vector<FD>> replies;
    std::string last_timing;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
#ifndef PCAN_FIXTURE_CLASSIC_ONLY
    constexpr unsigned kLengths[]{0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
#endif
    bool channel(uint16_t ch) {
        return ch >= 0x51 && ch <= 0x54;
    }
    uint32_t fault(unsigned slot, uint16_t ch) {
        return (values[19] == 0u) || values[19] == ch ? values[slot] : 0;
    }
    void respond(uint16_t ch, const FD &frame) {
        if (values[14] == 0u) {
            receive[ch].push_back(frame);
            return;
        }
        for (const unsigned command : {(frame.dlc != 0u) ? unsigned(frame.data[0]) : 0U, 256U}) {
            const auto found = replies.find({ch, frame.id, command, (frame.type & 4) != 0});
            if (found != replies.end()) {
                for (const auto &reply : found->second) {
                    receive[ch].push_back(reply);
                }
            }
        }
    }
} // namespace
// These names are the dynamic fixture ABI consumed by the loader tests.
// NOLINTBEGIN(readability-identifier-naming)
API Test_Get(unsigned index) {
    std::lock_guard<std::mutex> lock(mutex);
    return index < values.size() ? values[index] : 0xffffffff;
}
API Test_Set(unsigned index, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index >= values.size()) {
        return 1;
    }
    values[index] = value;
    return 0;
}
API Test_ClearReplies() {
    std::lock_guard<std::mutex> lock(mutex);
    replies.clear();
    return 0;
}
API Test_AddReply(uint16_t ch, uint32_t request, unsigned command, unsigned fd, const void *reply) {
    std::lock_guard<std::mutex> lock(mutex);
    if ((reply == nullptr) || command > 256) {
        return 1;
    }
    FD frame{};
    if (fd != 0u) {
        frame = *static_cast<const FD *>(reply);
    } else {
        const auto &classic = *static_cast<const Classic *>(reply);
        frame.id = classic.id;
        frame.type = classic.type;
        frame.dlc = classic.length;
        std::copy(classic.data, classic.data + 8, frame.data);
    }
    replies[{ch, request, command, fd != 0}].push_back(frame);
    return 0;
}
API Test_CopyTiming(char *buffer, uint32_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    if ((buffer == nullptr) || size <= last_timing.size()) {
        return 1;
    }
    std::memcpy(buffer, last_timing.c_str(), last_timing.size() + 1);
    return 0;
}
// This exported test function must retain the PCAN-Basic ABI parameter order.
API CAN_GetValue(uint16_t ch, uint8_t parameter, void *buffer,
                 uint32_t size) { // NOLINT(bugprone-easily-swappable-parameters)
    std::unique_lock<std::mutex> lock(mutex);
    ++values[7];
    const auto delay = values[16];
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    lock.lock();
    if (buffer == nullptr) {
        return 0x8000;
    }
    if (parameter == 0x2a) {
        if ((ch != 0u) || size != 4) {
            return 0x8000;
        }
        *static_cast<uint32_t *>(buffer) = 4;
        return 0;
    }
    if (parameter == 0x2b) {
        if ((ch != 0u) || size != 4 * sizeof(Info)) {
            return 0x8000;
        }
        auto *info = static_cast<Info *>(buffer);
        for (unsigned i = 0; i < 4; ++i) {
            info[i] = {};
            info[i].handle = uint16_t(0x51 + i);
            info[i].device = 5;
            info[i].controller = uint8_t(i);
            info[i].features = (i != 0u) ? 1 : 0;
            std::memcpy(info[i].name, "Independent PCAN", 17);
            info[i].id = 100 + i;
            info[i].condition = i == 3 ? 2 : 1;
        }
        return 0;
    }
    if (!channel(ch) || size != 4) {
        return 0x1400;
    }
    if (parameter == 0x16) {
        if (ch == 0x53) {
            return 0x4000;
        }
        *static_cast<uint32_t *>(buffer) = ch == 0x51 ? 0 : 1;
        return 0;
    }
    if (parameter == 0x0d) {
        *static_cast<uint32_t *>(buffer) = ((modes[ch] != 0) || ch == 0x54) ? 2 : 1;
        return 0;
    }
    return 0x4000;
}
API CAN_Initialize(uint16_t ch, uint16_t baud, uint8_t old1, uint32_t old2, uint16_t old3) {
    std::unique_lock<std::mutex> lock(mutex);
    ++values[0];
    values[8] = ch;
    values[9] = baud;
    if ((ch == 0u) || !channel(ch) || (old1 != 0u) || (old2 != 0u) || (old3 != 0u) ||
        baud != 0x14) {
        return 0x8000;
    }
    const auto delay = values[17];
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    lock.lock();
    if (modes[ch] != 0) {
        return 0x4000000;
    }
    if ((fault(10, ch) != 0u) && fault(10, ch) != 0x2000000) {
        return fault(10, ch);
    }
    modes[ch] = 1;
    return fault(10, ch);
}
#ifndef PCAN_FIXTURE_CLASSIC_ONLY
API CAN_InitializeFD(uint16_t ch, char *timing) {
    std::unique_lock<std::mutex> lock(mutex);
    ++values[1];
    values[8] = ch;
    if (ch != 0x52 || (timing == nullptr)) {
        return 0x8000;
    }
    last_timing = timing;
    if (last_timing.find("nom_brp=1") == std::string::npos ||
        last_timing.find("data_tseg1=11") == std::string::npos) {
        return 0x8000;
    }
    const auto delay = values[17];
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    lock.lock();
    if (modes[ch] != 0) {
        return 0x4000000;
    }
    if ((fault(10, ch) != 0u) && fault(10, ch) != 0x2000000) {
        return fault(10, ch);
    }
    modes[ch] = 2;
    return fault(10, ch);
}
#endif
API CAN_Uninitialize(uint16_t ch) {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[2];
    values[8] = ch;
    if (ch == 0u) {
        return 0x8000; // Global cleanup would be a test failure.
    }
    if (fault(11, ch) != 0u) {
        return fault(11, ch);
    }
    if (modes[ch] == 0) {
        return 0x4000000;
    }
    modes[ch] = 0;
    receive[ch].clear();
    return 0;
}
API CAN_Write(uint16_t ch, Classic *frame) {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[5];
    values[8] = ch;
    if (modes[ch] != 1 || (frame == nullptr)) {
        return 0x4000000;
    }
    values[20] = frame->data[0];
    values[21] = frame->length;
    if (fault(13, ch) != 0u) {
        return fault(13, ch);
    }
    if (frame->length > 8 || ((frame->type & 0xfc) != 0)) {
        return 0x20000;
    }
    FD next{};
    next.id = frame->id;
    next.type = frame->type;
    next.dlc = frame->length;
    std::copy(frame->data, frame->data + 8, next.data);
    respond(ch, next);
    return 0;
}
#ifndef PCAN_FIXTURE_MISSING_READ
API CAN_Read(uint16_t ch, Classic *frame, Stamp *stamp) {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[3];
    values[8] = ch;
    if (modes[ch] != 1 || (frame == nullptr) || (stamp == nullptr)) {
        return 0x4000000;
    }
    if (fault(12, ch) != 0u) {
        return fault(12, ch);
    }
    if (receive[ch].empty()) {
        return 0x20;
    }
    const auto next = receive[ch].front();
    receive[ch].pop_front();
    values[22] = next.data[0];
    values[23] = next.dlc;
    *frame = {};
    frame->id = next.id;
    frame->type = next.type;
    frame->length = next.dlc;
    std::copy(next.data, next.data + 8, frame->data);
    *stamp = {1, 2, 3};
    return 0;
}
#endif
#ifndef PCAN_FIXTURE_CLASSIC_ONLY
API CAN_WriteFD(uint16_t ch, FD *frame) {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[6];
    values[8] = ch;
    if (modes[ch] != 2 || (frame == nullptr)) {
        return 0x4000000;
    }
    if (fault(13, ch) != 0u) {
        return fault(13, ch);
    }
    if (frame->dlc > 15 || (((frame->type & 4) == 0) && frame->dlc > 8)) {
        return 0x20000;
    }
    // Padding must be deterministic for every FD DLC.
    for (unsigned i = kLengths[frame->dlc]; i < 64; ++i) {
        if (frame->data[i] != 0u) {
            return 0x20000;
        }
    }
    respond(ch, *frame);
    return 0;
}
API CAN_ReadFD(uint16_t ch, FD *frame, uint64_t *stamp) {
    std::lock_guard<std::mutex> lock(mutex);
    ++values[4];
    values[8] = ch;
    if (modes[ch] != 2 || (frame == nullptr) || (stamp == nullptr)) {
        return 0x4000000;
    }
    if (fault(12, ch) != 0u) {
        return fault(12, ch);
    }
    if (receive[ch].empty()) {
        return 0x20;
    }
    *frame = receive[ch].front();
    receive[ch].pop_front();
    *stamp = 0x123456789abcdef0ULL;
    return 0;
}
#endif
// NOLINTEND(readability-identifier-naming)
