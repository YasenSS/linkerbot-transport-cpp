#pragma once

#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/can/transport.hpp>
#include <linkerbot/can/vendor_canfd_abi.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace linkerbot::can {

    inline constexpr int kVendorCanFdDefaultSendTimeoutMs = 10;
    inline constexpr int kVendorCanFdDefaultReceiveTimeoutMs = 10;
    inline constexpr int kVendorCanFdMaxReceiveTimeoutMs = 10;
    inline constexpr int kVendorCanFdMaxSendTimeoutMs = 100;
    inline constexpr unsigned kVendorCanFdDefaultMaxReceiveFrames = 64;
    inline constexpr uint32_t kVendorCanFdDefaultNomBaud = 1'000'000;
    inline constexpr uint32_t kVendorCanFdDefaultDatBaud = 5'000'000;
    inline constexpr uint8_t kVendorCanFdDefaultConfigFlags = 0x01 | 0x02 | 0x04;
    inline constexpr uint8_t kVendorCanFdDefaultModel = 0;
    inline constexpr uint8_t kVendorCanFdDefaultCanType = 1;
    // Documented legacy default only. TX FrameType uses Message format/flags;
    // unspecified format with false/false flags => FD without BRS.
    // This constant is not passed to CANFD_Init.
    inline constexpr uint8_t kVendorCanFdDefaultFrameType = kFrameTypeFd;
    inline constexpr unsigned kVendorCanFdMaxTransmitBatch = 100;
    inline constexpr const char *kVendorCanFdLibEnv = "LINKERBOT_CANFD_LIB";

    // CAN FD wire DLC (0–15) <-> payload byte length (0–64), matching Python.
    [[nodiscard]] auto vendor_dlc_to_length(uint8_t dlc) -> uint8_t;
    [[nodiscard]] auto vendor_length_to_dlc(uint8_t length) -> uint8_t;

    // Per-library resource coordination (also exposed for injected API tests).
    // Linux shares Init/Exit when exported; Windows has no Init/Exit calls.
    namespace vendor_canfd_lifecycle {
        auto acquire(const std::shared_ptr<VendorCanFdApi> &api) -> void;
        auto release(const std::string &key) -> void;
        [[nodiscard]] auto snapshot() -> std::unordered_map<std::string, int>;
        // Constructor/destructor failures retain their pending physical close
        // and library reference here. Explicit retry is observable; a later
        // acquire of the same library also retries before allowing a new Init.
        auto retry_deferred_cleanup(const std::string &key) -> void;
        [[nodiscard]] auto pending_cleanup_count(const std::string &key) -> size_t;
        auto reset_for_test() -> void;
    } // namespace vendor_canfd_lifecycle

    struct VendorCanFdChannel {
        unsigned device_index = 0;
        unsigned channel_index = 0;
        std::string library_path; // resolved native module path, UTF-8 on Windows
    };
    struct VendorCanFdEnumerationFailure {
        std::string stage;
        std::string code;
        std::string message;
        std::optional<unsigned> device_index;
        std::optional<unsigned> channel_index;
        std::optional<int> native_status;
    };
    struct VendorCanFdEnumeration {
        std::vector<VendorCanFdChannel> channels;
        std::vector<VendorCanFdEnumerationFailure> failures;
    };
    // No channel open/initialization or CAN I/O. Linux requires confirmed
    // device/channel pairs; Windows enumerates native devNum values by default.
    [[nodiscard]] auto enumerate_vendor_canfd_channels(
        const std::shared_ptr<VendorCanFdApi> &api,
        const std::vector<VendorCanFdChannel> &selected = {}) -> VendorCanFdEnumeration;
    [[nodiscard]] auto enumerate_vendor_canfd_channels(
        const std::optional<std::string> &library_path = std::nullopt,
        const std::vector<VendorCanFdChannel> &selected = {}) -> VendorCanFdEnumeration;

    // CANTransport backed by the vendor libcanbus.so / HCanbus.dll (dynamically
    // loaded). Inject into any consumer that takes a CANTransport.
    class VendorCanFdTransport : public CANTransport {
      public:
        struct Config {
            unsigned device_index = 0;
            unsigned channel_index = 0;
            // UTF-8 on Windows. An explicit path (or LINKERBOT_CANFD_LIB)
            // failure never falls back to another library.
            std::optional<std::string> library_path;
            uint32_t nom_baud = kVendorCanFdDefaultNomBaud;
            uint32_t dat_baud = kVendorCanFdDefaultDatBaud;
            uint8_t config_flags = kVendorCanFdDefaultConfigFlags;
            uint8_t model = kVendorCanFdDefaultModel;
            uint8_t cantype = kVendorCanFdDefaultCanType;
            // Not applied to TX frames and not a CANFD_Init field. Kept so existing
            // Config aggregates still compile. Set Message::is_brs to enable BRS.
            uint8_t frame_type = kVendorCanFdDefaultFrameType;
            // Driver must honor these bounds; native setup/teardown has no
            // cancellation API. See docs/zh/minicanfd.md for this limitation.
            int send_timeout_ms = kVendorCanFdDefaultSendTimeoutMs;
            int receive_timeout_ms = kVendorCanFdDefaultReceiveTimeoutMs;
            bool receive_own_messages = false; // true requires an unexposed vendor echo API.
        };

        // Load libcanbus / HCanbus from disk (or LINKERBOT_CANFD_LIB).
        VendorCanFdTransport();
        explicit VendorCanFdTransport(const Config &config);

        // Inject a resolved API table (unit tests / custom loaders).
        VendorCanFdTransport(Config config, std::shared_ptr<VendorCanFdApi> api);

        ~VendorCanFdTransport() override;

        VendorCanFdTransport(const VendorCanFdTransport &) = delete;
        auto operator=(const VendorCanFdTransport &) -> VendorCanFdTransport & = delete;
        VendorCanFdTransport(VendorCanFdTransport &&) = delete;
        auto operator=(VendorCanFdTransport &&) -> VendorCanFdTransport & = delete;

        auto read() -> std::optional<Message> override;
        auto write(const Message &message) -> void override;
        auto write_batch(const std::vector<Message> &messages) -> void override;

        [[nodiscard]] auto prefers_write_batch() const -> bool override {
            return true;
        }

        // Idempotent once the entire close (device + lifecycle release, when
        // this is the last reference) has actually completed. If
        // CAN_CloseDevice or the vendor Exit call fails this throws
        // TransportIOError and *retains* whatever has not yet succeeded, so a
        // later close() call performs a genuine retry of just the remaining
        // step: it never re-issues CAN_CloseDevice once that has truly
        // succeeded, and never fakes success for the Exit step while it is
        // still failing. A concurrent close() call from another thread waits
        // for the in-progress attempt rather than returning early.
        auto close() -> void override;

        [[nodiscard]] auto device_index() const -> unsigned {
            return config_.device_index;
        }
        [[nodiscard]] auto channel_index() const -> unsigned {
            return config_.channel_index;
        }
        [[nodiscard]] auto closed() const -> bool {
            return closed_.load();
        }

      private:
        auto open_device() -> void;
        auto ensure_open_unlocked() const -> void;
        static auto encode_tx_frame(const Message &message, CanFDMsg &out) -> void;
        auto fill_tx_frame(const Message &message) -> void;
        [[nodiscard]] auto frame_to_message(const CanFDMsg &frame) const -> Message;

        Config config_;
        std::shared_ptr<VendorCanFdApi> api_;
        std::string lifecycle_key_;

        mutable std::mutex tx_mutex_;
        mutable std::mutex rx_mutex_;
        std::atomic<bool> closed_{true};

        // Guards the whole close attempt (device close *and*, when this is
        // the last reference, the vendor lifecycle Exit). Concurrent close()
        // callers serialize into at most one in-flight attempt at a time;
        // lifecycle_done_ (not device_closed_ alone) is what a waiter is
        // released on, so a second caller cannot observe "done" while the
        // first is still inside Exit/release. device_closed_ is tracked
        // separately so a retry after an Exit failure does not re-issue
        // CAN_CloseDevice on a device that is already physically closed.
        mutable std::mutex close_mutex_;
        mutable std::condition_variable close_cv_;
        bool closing_in_progress_ = false;
        bool device_closed_ = true;
        bool lifecycle_done_ = false;
        bool channel_reserved_ = false;

        CanFDMsg tx_frame_{};
        std::vector<CanFDMsg> tx_batch_;
        std::vector<CanFDMsg> rx_buffer_;
        std::deque<Message> rx_inbox_;
    };

    [[nodiscard]] auto make_vendor_canfd_transport() -> std::unique_ptr<VendorCanFdTransport>;
    [[nodiscard]] auto make_vendor_canfd_transport(const VendorCanFdTransport::Config &config)
        -> std::unique_ptr<VendorCanFdTransport>;

    [[nodiscard]] auto load_vendor_canfd_api(const std::optional<std::string> &library_path)
        -> std::shared_ptr<VendorCanFdApi>;

} // namespace linkerbot::can
