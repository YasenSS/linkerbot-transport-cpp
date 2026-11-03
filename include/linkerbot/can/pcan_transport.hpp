#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <linkerbot/can/transport.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace linkerbot::can {
    // SDK-owned declarations of the Classic/FD subset of PCAN-Basic. No vendor SDK
    // header or import library is required. ABI reference: official PCANBasic.h,
    // last change 2025-10-24; source URL/hash recorded in docs/zh/pcan.md.
    namespace pcan_abi {
        using Status = uint32_t;
        using Handle = uint16_t;
        // Preserve the 0.3 ABI helper names used by the optional validation consumer.
        // NOLINTBEGIN(readability-identifier-naming)
        inline constexpr Status ok = 0, receive_empty = 0x20, caution = 0x02000000;
        inline constexpr uint8_t attached_count = 0x2a, attached_channels = 0x2b;
        inline constexpr uint8_t channel_features = 0x16, channel_condition = 0x0d;
        inline constexpr uint32_t fd_capable = 1, available = 1;
        // NOLINTEND(readability-identifier-naming)
        struct ClassicMessage {
            uint32_t id;
            uint8_t type, length, data[8];
        };
        struct FdMessage {
            uint32_t id;
            uint8_t type, dlc, data[64];
        };
        struct Timestamp {
            uint32_t millis;
            uint16_t overflow, micros;
        };
        using FdTimestamp = uint64_t;
        struct ChannelInformation {
            Handle handle;
            uint8_t device_type, controller;
            uint32_t features;
            char device_name[33];
            uint32_t device_id, condition;
        };
        static_assert(sizeof(ClassicMessage) == 16 && alignof(ClassicMessage) == 4);
        static_assert(sizeof(FdMessage) == 72 && alignof(FdMessage) == 4);
        static_assert(offsetof(ClassicMessage, data) == 6 && offsetof(FdMessage, data) == 6);
        static_assert(offsetof(FdMessage, dlc) == 5 && offsetof(FdMessage, type) == 4);
        static_assert(sizeof(Timestamp) == 8 && offsetof(Timestamp, micros) == 6);
        static_assert(sizeof(FdTimestamp) == 8);
        static_assert(sizeof(ChannelInformation) == 52 && alignof(ChannelInformation) == 4);
        static_assert(offsetof(ChannelInformation, features) == 4);
        static_assert(offsetof(ChannelInformation, device_name) == 8);
        static_assert(offsetof(ChannelInformation, device_id) == 44);
        static_assert(offsetof(ChannelInformation, condition) == 48);
    } // namespace pcan_abi

    struct PcanApi {
        std::string library_path; // actual module path; UTF-8 on Windows
        std::shared_ptr<void> library_owner;
        std::function<pcan_abi::Status(uint16_t, uint8_t, void *, uint32_t)> get_value;
        std::function<pcan_abi::Status(uint16_t, uint16_t, uint8_t, uint32_t, uint16_t)> initialize;
        std::function<pcan_abi::Status(uint16_t)> uninitialize;
        std::function<pcan_abi::Status(uint16_t, pcan_abi::ClassicMessage *, pcan_abi::Timestamp *)>
            read;
        std::function<pcan_abi::Status(uint16_t, pcan_abi::ClassicMessage *)> write;
        std::function<pcan_abi::Status(uint16_t, char *)> initialize_fd;
        std::function<pcan_abi::Status(uint16_t, pcan_abi::FdMessage *, pcan_abi::FdTimestamp *)>
            read_fd;
        std::function<pcan_abi::Status(uint16_t, pcan_abi::FdMessage *)> write_fd;
    };

    enum class PcanMode { kClassic, kFd };
    struct PcanFdTiming {
        uint32_t clock_hz = 80'000'000;
        uint32_t nominal_prescaler = 1, nominal_tseg1 = 63, nominal_tseg2 = 16, nominal_sjw = 16;
        uint32_t data_prescaler = 1, data_tseg1 = 11, data_tseg2 = 4, data_sjw = 4;
        // Pure validation: exact requested rates, supported ranges and SJW <= TSEG2.
        [[nodiscard]] auto bitrate_string(uint32_t nominal_bitrate,
                                          uint32_t data_bitrate) const -> std::string;
    };
    struct PcanConfig {
        uint16_t channel = 0; // explicit enumerated handle; 0 (all channels) is forbidden
        PcanMode mode = PcanMode::kClassic;
        uint32_t nominal_bitrate = 1'000'000, data_bitrate = 5'000'000;
        PcanFdTiming fd_timing;
        std::optional<std::string> library_path;       // UTF-8; LINKERBOT_PCAN_LIB if absent
        std::chrono::milliseconds receive_timeout{10}; // 0..100 ms, close interrupts polling
    };
    struct PcanChannel {
        uint16_t handle = 0;
        std::string native_name, hardware_name;
        uint8_t device_type = 0, controller = 0;
        uint32_t device_id = 0, features = 0, condition = 0;
        std::optional<bool> supports_fd;
        bool owned_by_process = false;
        std::string library_path;
    };
    struct PcanEnumerationFailure {
        std::string stage, message;
        std::optional<uint16_t> channel;
        std::optional<int> native_status;
    };
    struct PcanEnumeration {
        std::vector<PcanChannel> channels;
        std::vector<PcanEnumerationFailure> failures;
    };
    enum class PcanFailure {
        kBusOff,
        kAdapterUnavailable,
        kTransmitQueueFull,
        kBusError,
        kChannelBusy,
        kOther
    };
    class PcanTransportError : public TransportIOError {
      public:
        PcanTransportError(const std::string &operation, uint32_t status);
        [[nodiscard]] auto reason() const noexcept -> PcanFailure {
            return reason_;
        }

      private:
        PcanFailure reason_;
    };
    [[nodiscard]] auto
    load_pcan_api(const std::optional<std::string> &path = {}) -> std::shared_ptr<PcanApi>;
    [[nodiscard]] auto
    enumerate_pcan_channels(const std::shared_ptr<PcanApi> &api) -> PcanEnumeration;
    [[nodiscard]] auto
    enumerate_pcan_channels(const std::optional<std::string> &path = {}) -> PcanEnumeration;
    [[nodiscard]] auto pcan_channel_name(uint16_t handle) -> std::string;
    [[nodiscard]] auto pcan_channel_handle(const std::string &name) -> uint16_t;
    auto validate_pcan_config(const PcanConfig &config) -> void; // no I/O
    // Retry resources transferred by failed constructor/destructor cleanup. Explicit
    // close failures remain owned by their live Transport and must be retried there.
    auto retry_pcan_cleanup(const std::shared_ptr<PcanApi> &api) -> void;
    [[nodiscard]] auto pending_pcan_cleanup(const std::shared_ptr<PcanApi> &api) -> size_t;

    class PcanTransport final : public CANTransport {
      public:
        explicit PcanTransport(PcanConfig config);
        PcanTransport(PcanConfig config, std::shared_ptr<PcanApi> api);
        ~PcanTransport() override;
        auto read() -> std::optional<Message> override;
        auto write(const Message &message) -> void override;
        auto close() -> void override;

      private:
        struct State;
        std::unique_ptr<State> state_;
    };
} // namespace linkerbot::can
