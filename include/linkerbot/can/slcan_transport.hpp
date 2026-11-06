#pragma once
#include <chrono>
#include <linkerbot/can/transport.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Public feature-test macros must remain usable in #if expressions.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define LINKERBOT_TRANSPORT_SLCAN_FD 1
// Legacy feature-test spelling remains available to existing consumers.
#define LINKERBOT_COMM_SLCAN_FD LINKERBOT_TRANSPORT_SLCAN_FD

namespace linkerbot::can {
    // Firmware profiles are explicit: Lawicel requires command ACKs; CANable 2
    // uses d/D/b/B and Y2/Y5 and does not acknowledge serial commands.
    enum class SlcanProtocol { kLawicelClassic, kCanableFd };
    struct SlcanConfig {
        std::string port; // Explicit COM1..COM65535 or Linux /dev/... path.
        uint32_t serial_baud = 115200;
        uint32_t nominal_bitrate = 1'000'000;
        // BRS requires FD; each Message selects its format/flags.
        bool fd = false, brs = false;
        std::chrono::milliseconds receive_timeout{10};  // 1..100 ms
        std::chrono::milliseconds command_timeout{500}; // 1..5000 ms
        SlcanProtocol protocol = SlcanProtocol::kLawicelClassic;
        uint32_t data_bitrate = 5'000'000;          // CANable FD: 2M or 5M, independent of UART.
        std::chrono::milliseconds startup_delay{0}; // 0..5000 ms; USB reset settling, if needed.
        bool receive_own_messages = false; // true is unsupported by these firmware profiles.
    };
    auto validate_slcan_config(const SlcanConfig &config) -> void; // No I/O.
    [[nodiscard]] auto slcan_port_name(std::string_view name) -> std::string;
    // Metadata only: no open/initialization. Empty allowlist returns empty.
    [[nodiscard]] auto
    enumerate_slcan_ports(const std::vector<std::string> &allowlist) -> std::vector<std::string>;
    [[nodiscard]] auto encode_slcan_frame(const Message &message) -> std::string;
    [[nodiscard]] auto decode_slcan_frame(std::string_view line) -> Message;
    [[nodiscard]] auto encode_slcan_frame(const Message &message,
                                          SlcanProtocol protocol) -> std::string;
    [[nodiscard]] auto decode_slcan_frame(std::string_view line, SlcanProtocol protocol) -> Message;

    // Byte-stream seam for deterministic offline tests. read_some returns at most
    // max_bytes, empty on timeout; write_all must fail on partial write/timeouts.
    // cancel interrupts current calls; repeated close is safe. Retain ownership
    // of resources that can still be released (Linux never retries a consumed fd).
    // Implementations must bound operations by the given budget.
    class SlcanSerialPort {
      public:
        virtual ~SlcanSerialPort() = default;
        virtual auto read_some(size_t max_bytes,
                               std::chrono::milliseconds timeout) -> std::string = 0;
        virtual auto write_all(std::string_view bytes,
                               std::chrono::milliseconds timeout) -> void = 0;
        virtual auto cancel() noexcept -> void = 0;
        virtual auto close() -> void = 0;
    };
    [[nodiscard]] auto
    open_slcan_serial(const SlcanConfig &config) -> std::shared_ptr<SlcanSerialPort>;
    // Failed constructor/destructor cleanup remains owned here, and is retryable.
    auto retry_slcan_cleanup() -> void;
    [[nodiscard]] auto pending_slcan_cleanup() -> size_t;

    class SlcanTransport final : public CANTransport {
      public:
        explicit SlcanTransport(SlcanConfig config);
        SlcanTransport(SlcanConfig config, std::shared_ptr<SlcanSerialPort> serial);
        ~SlcanTransport() override;
        auto read() -> std::optional<Message> override;
        auto write(const Message &message) -> void override;
        auto close() -> void override;

      private:
        struct Prepared;
        explicit SlcanTransport(Prepared prepared);
        struct State;
        std::unique_ptr<State> state_;
    };
} // namespace linkerbot::can
