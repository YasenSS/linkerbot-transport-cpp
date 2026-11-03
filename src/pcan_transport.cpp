#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/pcan_transport.hpp>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace linkerbot::can {
    namespace {
        using namespace pcan_abi;
        constexpr std::array<uint8_t, 16> kLengths{0, 1,  2,  3,  4,  5,  6,  7,
                                                   8, 12, 16, 20, 24, 32, 48, 64};
        uint16_t classic_bitrate(uint32_t rate) {
            switch (rate) {
            case 1000000:
                return 0x14;
            case 800000:
                return 0x16;
            case 500000:
                return 0x1c;
            case 250000:
                return 0x11c;
            case 125000:
                return 0x31c;
            case 100000:
                return 0x432f;
            case 50000:
                return 0x472f;
            case 20000:
                return 0x532f;
            case 10000:
                return 0x672f;
            case 5000:
                return 0x7f7f;
            default:
                throw ValidationError("Unsupported PCAN Classic bitrate");
            }
        }
        std::string hex_value(uint32_t value) {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << value;
            return stream.str();
        }
        PcanFailure classify(Status status) {
            if ((status & 0x10) != 0u) {
                return PcanFailure::kBusOff;
            }
            if (status == 0x400 || status == 0x800) {
                return PcanFailure::kChannelBusy;
            }
            if ((status & (0x200 | 0x1000 | 0x04000000)) != 0u) {
                return PcanFailure::kAdapterUnavailable;
            }
            if ((status & (0x01 | 0x80)) != 0u) {
                return PcanFailure::kTransmitQueueFull;
            }
            if ((status & (0x04 | 0x08 | 0x40000 | 0x02 | 0x40)) != 0u) {
                return PcanFailure::kBusError;
            }
            return PcanFailure::kOther;
        }
        const char *reason_name(PcanFailure reason) {
            switch (reason) {
            case PcanFailure::kBusOff:
                return "bus_off";
            case PcanFailure::kAdapterUnavailable:
                return "adapter_unavailable_or_uninitialized";
            case PcanFailure::kTransmitQueueFull:
                return "transmit_queue_full";
            case PcanFailure::kBusError:
                return "bus_error_or_overrun";
            case PcanFailure::kChannelBusy:
                return "channel_in_use";
            default:
                return "driver_error";
            }
        }
        void check(Status status, const char *operation) {
            if (status != 0u) {
                throw PcanTransportError(operation, status);
            }
        }
        struct Claim {
            std::shared_ptr<PcanApi> api;
            PcanConfig config;
            bool initialized = false;
            bool deferred = false;
        };
        using Key = std::pair<std::string, uint16_t>;
        std::recursive_mutex &registry_mutex() {
            static std::recursive_mutex mutex;
            return mutex;
        }
        std::map<Key, std::shared_ptr<Claim>> &claims() {
            static std::map<Key, std::shared_ptr<Claim>> table;
            return table;
        }
        Key key(const Claim &claim) {
            return {claim.api->library_path, claim.config.channel};
        }
        void retry_locked(const std::shared_ptr<PcanApi> &api,
                          std::vector<PcanEnumerationFailure> *diagnostics = nullptr) {
            for (auto it = claims().begin(); it != claims().end();) {
                auto &claim = *it->second;
                if (claim.api->library_path != api->library_path || !claim.deferred) {
                    ++it;
                    continue;
                }
                try {
                    if (claim.initialized) {
                        check(claim.api->uninitialize(claim.config.channel),
                              "CAN_Uninitialize deferred cleanup");
                    }
                } catch (const CANError &error) {
                    if (diagnostics == nullptr) {
                        throw;
                    }
                    diagnostics->push_back(
                        {"cleanup", error.what(), claim.config.channel, error.native_status()});
                    ++it;
                    continue;
                }
                it = claims().erase(it);
            }
        }
        std::shared_ptr<PcanApi> load_for_config(const PcanConfig &config) {
            validate_pcan_config(config); // configuration errors precede even module loading
            return load_pcan_api(config.library_path);
        }
    } // namespace
    PcanTransportError::PcanTransportError(const std::string &operation, uint32_t status)
        : TransportIOError(operation + ": " + reason_name(classify(status)) + " (" +
                               hex_value(status) + ")",
                           {}, static_cast<int>(status)),
          reason_(classify(status)) {}

    auto PcanFdTiming::bitrate_string(uint32_t nominal, uint32_t data) const -> std::string {
        const std::array<uint32_t, 6> clocks{80000000, 60000000, 40000000,
                                             30000000, 24000000, 20000000};
        if (std::find(clocks.begin(), clocks.end(), clock_hz) == clocks.end()) {
            throw ValidationError("Unsupported PCAN FD clock");
        }
        const auto valid = [](uint32_t brp, uint32_t t1, uint32_t t2, uint32_t sjw, uint32_t max1,
                              uint32_t max2) {
            return brp >= 1 && brp <= 1024 && t1 >= 1 && t1 <= max1 && t2 >= 1 && t2 <= max2 &&
                   sjw >= 1 && sjw <= t2;
        };
        if (!valid(nominal_prescaler, nominal_tseg1, nominal_tseg2, nominal_sjw, 256, 128) ||
            !valid(data_prescaler, data_tseg1, data_tseg2, data_sjw, 32, 16)) {
            throw ValidationError("PCAN FD timing segments/prescalers/SJW are invalid");
        }
        const auto matches = [&](uint32_t rate, uint32_t brp, uint32_t t1, uint32_t t2) {
            return (rate != 0u) && static_cast<uint64_t>(rate) * brp * (1 + t1 + t2) == clock_hz;
        };
        if (!matches(nominal, nominal_prescaler, nominal_tseg1, nominal_tseg2) ||
            !matches(data, data_prescaler, data_tseg1, data_tseg2)) {
            throw ValidationError(
                "PCAN FD timing does not produce the requested nominal/data bitrates");
        }
        return "f_clock=" + std::to_string(clock_hz) +
               ",nom_brp=" + std::to_string(nominal_prescaler) +
               ",nom_tseg1=" + std::to_string(nominal_tseg1) +
               ",nom_tseg2=" + std::to_string(nominal_tseg2) +
               ",nom_sjw=" + std::to_string(nominal_sjw) +
               ",data_brp=" + std::to_string(data_prescaler) +
               ",data_tseg1=" + std::to_string(data_tseg1) +
               ",data_tseg2=" + std::to_string(data_tseg2) +
               ",data_sjw=" + std::to_string(data_sjw);
    }
    auto validate_pcan_config(const PcanConfig &config) -> void {
        if (config.channel == 0u) {
            throw ValidationError("PCAN requires an explicit nonzero channel handle; global "
                                  "cleanup handle is forbidden");
        }
        if (config.receive_timeout.count() < 0 ||
            config.receive_timeout > std::chrono::milliseconds(100)) {
            throw ValidationError("PCAN receive timeout must be 0..100 ms");
        }
        if (config.library_path && (config.library_path->empty() ||
                                    config.library_path->find('\0') != std::string::npos)) {
            throw ValidationError("PCAN library path must be nonempty and contain no NUL");
        }
        if (config.mode == PcanMode::kClassic) {
            (void)classic_bitrate(config.nominal_bitrate);
        } else if (config.mode == PcanMode::kFd) {
            (void)config.fd_timing.bitrate_string(config.nominal_bitrate, config.data_bitrate);
        } else {
            throw ValidationError("Unknown PCAN protocol mode");
        }
    }
    auto pcan_channel_name(uint16_t handle) -> std::string {
        // Formatting only: availability is ALWAYS read through ATTACHED_CHANNELS.
        if (handle >= 0x51 && handle <= 0x58) {
            return "PCAN_USBBUS" + std::to_string(handle - 0x50);
        }
        if (handle >= 0x509 && handle <= 0x510) {
            return "PCAN_USBBUS" + std::to_string(handle - 0x500);
        }
        return "PCAN_" + hex_value(handle);
    }
    auto pcan_channel_handle(const std::string &name) -> uint16_t {
        const std::string prefix = "PCAN_USBBUS";
        std::string value = name;
        bool usb = value.compare(0, prefix.size(), prefix) == 0;
        if (usb) {
            value = value.substr(prefix.size());
        } else if (value.compare(0, 5, "PCAN_") == 0) {
            value = value.substr(5);
        }
        if (value.empty() || value[0] == '-' || value[0] == '+' ||
            value.find_first_of(" \t\r\n") != std::string::npos) {
            throw ValidationError("Invalid PCAN channel name");
        }
        size_t consumed = 0;
        unsigned long number;
        try {
            number = std::stoul(value, &consumed, usb ? 10 : 0);
        } catch (const std::exception &) {
            throw ValidationError("Invalid PCAN channel name");
        }
        if (consumed != value.size() || number == 0 || number > (usb ? 16UL : 65535UL)) {
            throw ValidationError("Invalid PCAN channel handle");
        }
        if (!usb) {
            return static_cast<uint16_t>(number);
        }
        const auto base = number <= 8 ? 0x50UL : 0x500UL;
        return static_cast<uint16_t>(base + number);
    }
    auto retry_pcan_cleanup(const std::shared_ptr<PcanApi> &api) -> void {
        if (!api || api->library_path.empty() || !api->uninitialize) {
            throw ValidationError("PCAN cleanup API is incomplete");
        }
        std::lock_guard<std::recursive_mutex> lock(registry_mutex());
        retry_locked(api);
    }
    auto pending_pcan_cleanup(const std::shared_ptr<PcanApi> &api) -> size_t {
        if (!api) {
            throw ValidationError("PCAN API is null");
        }
        std::lock_guard<std::recursive_mutex> lock(registry_mutex());
        return static_cast<size_t>(
            std::count_if(claims().begin(), claims().end(), [&](const auto &item) {
                return item.first.first == api->library_path && item.second->deferred;
            }));
    }
    auto enumerate_pcan_channels(const std::shared_ptr<PcanApi> &api) -> PcanEnumeration {
        if (!api || api->library_path.empty() || !api->get_value || !api->uninitialize) {
            throw ValidationError("PCAN enumeration API is incomplete");
        }
        std::lock_guard<std::recursive_mutex> lock(registry_mutex());
        PcanEnumeration result;
        retry_locked(api, &result.failures);
        std::vector<ChannelInformation> raw;
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint32_t count = 0;
            check(api->get_value(0, attached_count, &count, sizeof(count)),
                  "CAN_GetValue(ATTACHED_CHANNELS_COUNT)");
            if (count > 4096) {
                throw TransportIOError("PCAN attached channel count exceeds safety limit");
            }
            if (count == 0u) {
                return result;
            }
            raw.assign(count, {});
            const auto status =
                api->get_value(0, attached_channels, raw.data(),
                               static_cast<uint32_t>(raw.size() * sizeof(ChannelInformation)));
            if (status == 0u) {
                break;
            }
            if (attempt == 2 || status != 0x8000) {
                check(status, "CAN_GetValue(ATTACHED_CHANNELS)");
            }
        }
        std::map<uint16_t, bool> seen;
        for (const auto &item : raw) {
            if ((item.handle == 0u) || !seen.emplace(item.handle, true).second) {
                result.failures.push_back(
                    {"enumerate", "Invalid/duplicate attached PCAN handle", item.handle, {}});
                continue;
            }
            PcanChannel channel;
            channel.handle = item.handle;
            channel.native_name = pcan_channel_name(item.handle);
            channel.hardware_name.assign(item.device_name,
                                         std::find(item.device_name, item.device_name + 33, '\0'));
            channel.device_id = item.device_id;
            channel.device_type = item.device_type;
            channel.controller = item.controller;
            channel.features = item.features;
            channel.condition = item.condition;
            channel.library_path = api->library_path;
            channel.owned_by_process = claims().count({api->library_path, item.handle}) != 0;
            uint32_t features = 0;
            const auto feature_status =
                api->get_value(item.handle, channel_features, &features, sizeof(features));
            if (feature_status == 0u) {
                channel.features = features;
                channel.supports_fd = (features & fd_capable) != 0;
            } else {
                result.failures.push_back({"capability", "PCAN channel capability is unknown",
                                           item.handle, static_cast<int>(feature_status)});
            }
            const auto condition_status = api->get_value(
                item.handle, channel_condition, &channel.condition, sizeof(channel.condition));
            if (condition_status != 0u) {
                channel.condition = 0; // Never treat a failed availability query as available.
                result.failures.push_back({"condition", "PCAN channel availability is unknown",
                                           item.handle, static_cast<int>(condition_status)});
            }
            result.channels.push_back(std::move(channel));
        }
        return result;
    }
    auto enumerate_pcan_channels(const std::optional<std::string> &path) -> PcanEnumeration {
        return enumerate_pcan_channels(load_pcan_api(path));
    }

    struct PcanTransport::State {
        PcanConfig config;
        std::shared_ptr<PcanApi> api;
        std::shared_ptr<Claim> claim;
        std::atomic<bool> closed{false};
        std::mutex io_mutex, read_mutex, wait_mutex;
        std::condition_variable wake;
    };
    // Member initialization order keeps loading ahead of the final config move;
    // delegating-constructor arguments alone do not provide that ordering.
    struct PcanTransport::Prepared {
        PcanConfig config;
        std::shared_ptr<PcanApi> api;

        explicit Prepared(PcanConfig incoming)
            : config(std::move(incoming)), api(load_for_config(config)) {}
    };
    PcanTransport::PcanTransport(PcanConfig config) : PcanTransport(Prepared{std::move(config)}) {}
    PcanTransport::PcanTransport(Prepared prepared)
        : PcanTransport(std::move(prepared.config), std::move(prepared.api)) {}
    PcanTransport::PcanTransport(PcanConfig config, std::shared_ptr<PcanApi> api)
        : state_(std::make_unique<State>()) {
        validate_pcan_config(config);
        if (!api || api->library_path.empty() || !api->initialize || !api->uninitialize ||
            !api->read || !api->write || !api->get_value) {
            throw ValidationError("PCAN API is incomplete");
        }
        if (config.mode == PcanMode::kFd &&
            (!api->initialize_fd || !api->read_fd || !api->write_fd)) {
            throw TransportIOError("PCAN-Basic library is missing CAN_InitializeFD/ReadFD/WriteFD");
        }
        state_->config = config;
        state_->api = api;
        const Key channel_key{api->library_path, config.channel};
        std::lock_guard<std::recursive_mutex> lock(registry_mutex());
        const auto enumeration = enumerate_pcan_channels(api);
        if (claims().count(channel_key) != 0u) {
            throw FaultedError("PCAN channel already owned; duplicate or conflicting "
                               "mode/bitrate/timing configuration");
        }
        const auto found = std::find_if(enumeration.channels.begin(), enumeration.channels.end(),
                                        [&](const auto &c) { return c.handle == config.channel; });
        if (found == enumeration.channels.end()) {
            throw TransportIOError("Requested PCAN channel is not attached");
        }
        if (config.mode == PcanMode::kFd && found->supports_fd != true) {
            throw ValidationError(
                "Requested PCAN channel is Classic-only or its FD capability is unknown");
        }
        if ((found->condition & available) == 0u) {
            throw FaultedError("PCAN channel is occupied, unavailable or its condition is unknown");
        }
        auto claim = std::make_shared<Claim>();
        claim->api = api;
        claim->config = config;
        claims().emplace(channel_key, claim);
        try {
            Status status;
            if (config.mode == PcanMode::kFd) {
                auto timing =
                    config.fd_timing.bitrate_string(config.nominal_bitrate, config.data_bitrate);
                status = api->initialize_fd(config.channel, timing.data());
            } else {
                status = api->initialize(config.channel, classic_bitrate(config.nominal_bitrate), 0,
                                         0, 0);
            }
            claim->initialized = status == ok || status == caution;
            if (status != 0u) {
                if (claim->initialized) {
                    const auto cleanup = api->uninitialize(config.channel);
                    if (cleanup != 0u) {
                        claim->deferred = true;
                    } else {
                        claim->initialized = false;
                        claims().erase(channel_key);
                    }
                }
                throw PcanTransportError("CAN_Initialize/InitializeFD", status);
            }
            state_->claim = claim;
        } catch (...) {
            if (!claim->initialized) {
                claims().erase(channel_key);
            } else {
                claim->deferred = true;
            }
            throw;
        }
    }
    PcanTransport::~PcanTransport() {
        try {
            close();
        } catch (...) {
            std::lock_guard<std::recursive_mutex> lock(registry_mutex());
            if (state_->claim) {
                state_->claim->deferred = true;
            }
        }
    }
    auto PcanTransport::close() -> void {
        state_->closed.store(true);
        state_->wake.notify_all();
        std::lock_guard<std::mutex> io(state_->io_mutex);
        std::lock_guard<std::recursive_mutex> registry(registry_mutex());
        if (!state_->claim) {
            return;
        }
        check(state_->api->uninitialize(state_->config.channel), "CAN_Uninitialize");
        state_->claim->initialized = false;
        claims().erase(key(*state_->claim));
        state_->claim.reset();
    }
    auto PcanTransport::write(const Message &message) -> void {
        std::lock_guard<std::mutex> lock(state_->io_mutex);
        if (state_->closed.load()) {
            throw ClosedError("PCAN interface is closed");
        }
        validate_frame_flags(message.is_fd, message.is_brs);
        validate_arbitration_id(message.id, message.is_extended);
        validate_payload_length(message.dlc, message.is_fd ? LinkKind::kCanFd : LinkKind::kClassic);
        if (state_->config.mode == PcanMode::kClassic && message.is_fd) {
            throw ValidationError("FD frame cannot be sent on PCAN Classic mode");
        }
        // PCAN flags are explicit: false/false means a Classic frame even in FD
        // mode. Hand protocol encoders explicitly tag their FD frames.
        const auto type = static_cast<uint8_t>((message.is_extended ? 2 : 0) |
                                               (message.is_fd ? 4 : 0) | (message.is_brs ? 8 : 0));
        if (state_->config.mode == PcanMode::kFd) {
            FdMessage frame{};
            frame.id = message.id;
            frame.type = type;
            frame.dlc = static_cast<uint8_t>(
                std::lower_bound(kLengths.begin(), kLengths.end(), message.dlc) - kLengths.begin());
            std::memcpy(frame.data, message.data.data(), message.dlc);
            check(state_->api->write_fd(state_->config.channel, &frame), "CAN_WriteFD");
        } else {
            ClassicMessage frame{};
            frame.id = message.id;
            frame.type = type;
            frame.length = message.dlc;
            std::memcpy(frame.data, message.data.data(), message.dlc);
            check(state_->api->write(state_->config.channel, &frame), "CAN_Write");
        }
    }
    auto PcanTransport::read() -> std::optional<Message> {
        std::lock_guard<std::mutex> reader(state_->read_mutex);
        const auto deadline = std::chrono::steady_clock::now() + state_->config.receive_timeout;
        for (unsigned inspected = 0; inspected < 256; ++inspected) {
            Status status;
            uint8_t type;
            uint8_t dlc;
            Message message;
            {
                std::lock_guard<std::mutex> lock(state_->io_mutex);
                if (state_->closed.load()) {
                    throw ClosedError("PCAN interface is closed");
                }
                if (state_->config.mode == PcanMode::kFd) {
                    FdMessage frame{};
                    FdTimestamp timestamp{};
                    status = state_->api->read_fd(state_->config.channel, &frame, &timestamp);
                    type = frame.type;
                    dlc = frame.dlc;
                    message.id = frame.id;
                    std::copy(std::begin(frame.data), std::end(frame.data), message.data.begin());
                } else {
                    ClassicMessage frame{};
                    Timestamp timestamp{};
                    status = state_->api->read(state_->config.channel, &frame, &timestamp);
                    type = frame.type;
                    dlc = frame.length;
                    message.id = frame.id;
                    std::copy(std::begin(frame.data), std::end(frame.data), message.data.begin());
                }
            }
            if (status == receive_empty) {
                std::unique_lock<std::mutex> wait(state_->wait_mutex);
                if (std::chrono::steady_clock::now() >= deadline) {
                    return std::nullopt;
                }
                state_->wake.wait_until(wait,
                                        std::min(deadline, std::chrono::steady_clock::now() +
                                                               std::chrono::milliseconds(1)),
                                        [&] { return state_->closed.load(); });
                continue;
            }
            check(status, state_->config.mode == PcanMode::kFd ? "CAN_ReadFD" : "CAN_Read");
            if ((type & 0xc0) != 0) {
                throw TransportIOError(
                    "PCAN status/error frame received; inspect driver bus state");
            }
            if ((type & (1 | 0x20)) == 0) {
                message.is_extended = (type & 2) != 0;
                message.is_fd = (type & 4) != 0;
                message.is_brs = (type & 8) != 0;
                validate_frame_flags(message.is_fd, message.is_brs);
                validate_arbitration_id(message.id, message.is_extended);
                if (state_->config.mode == PcanMode::kClassic && message.is_fd) {
                    throw ValidationError("PCAN Classic read returned an FD frame");
                }
                if (dlc >= kLengths.size()) {
                    throw ValidationError("PCAN returned invalid DLC");
                }
                message.dlc = state_->config.mode == PcanMode::kFd ? kLengths[dlc] : dlc;
                validate_payload_length(message.dlc,
                                        message.is_fd ? LinkKind::kCanFd : LinkKind::kClassic);
                return message;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
        }
        return std::nullopt; // A flood of non-data frames cannot make read unbounded.
    }
} // namespace linkerbot::can
