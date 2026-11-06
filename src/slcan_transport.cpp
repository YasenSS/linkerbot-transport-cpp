#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/slcan_transport.hpp>
#include <mutex>

namespace linkerbot::can {
    auto retry_native_slcan_cleanup() -> void;
    auto pending_native_slcan_cleanup() -> size_t;
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr size_t kMaxFrames = 256;
        constexpr std::array<uint8_t, 16> kLengths{0, 1,  2,  3,  4,  5,  6,  7,
                                                   8, 12, 16, 20, 24, 32, 48, 64};
        constexpr uint32_t kCanableRates[]{10000,  20000,  50000,  100000,  125000,
                                           250000, 500000, 750000, 1000000, 83300};
        constexpr uint32_t kRates[] = {10000,  20000,  50000,  100000, 125000,
                                       250000, 500000, 800000, 1000000};
        char hex(unsigned value) {
            return "0123456789ABCDEF"[value & 15];
        }
        unsigned unhex(char c) {
            if (c >= '0' && c <= '9') {
                return static_cast<unsigned>(c - '0');
            }
            if (c >= 'a' && c <= 'f') {
                return static_cast<unsigned>(c - 'a' + 10);
            }
            if (c >= 'A' && c <= 'F') {
                return static_cast<unsigned>(c - 'A' + 10);
            }
            throw ValidationError("Invalid SLCAN hex digit");
        }
        struct Cleanup {
            std::mutex mutex;
            std::vector<std::shared_ptr<SlcanSerialPort>> ports;
        };
        auto cleanup() -> Cleanup & {
            // Serial ports retained after a failed close must survive static destruction.
            static auto *value = new Cleanup; // NOLINT(cppcoreguidelines-owning-memory)
            return *value;
        }
        void retain(std::shared_ptr<SlcanSerialPort> port) {
            auto &store = cleanup();
            std::lock_guard<std::mutex> lock(store.mutex);
            store.ports.push_back(std::move(port));
        }
        auto remaining(Clock::time_point deadline) -> std::chrono::milliseconds {
            return std::max(
                std::chrono::milliseconds{1},
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
        }
    } // namespace

    auto slcan_port_name(std::string_view name) -> std::string {
        if (name.empty() || name.find_first_of("\r\n\t") != std::string_view::npos ||
            name.find('\0') != std::string_view::npos) {
            throw ValidationError("Invalid SLCAN serial port");
        }
        if (name.substr(0, 5) == "/dev/") {
            const auto path = std::filesystem::path(std::string{name});
            for (const auto &part : path) {
                if (part == "..") {
                    throw ValidationError("SLCAN device path cannot contain ..");
                }
            }
            const auto normalized = path.lexically_normal().generic_string();
            if (normalized.size() <= 5 || normalized.back() == '/') {
                throw ValidationError("SLCAN needs a device path under /dev");
            }
            return normalized;
        }
        if (name.substr(0, 4) == R"(\\.\)") {
            name.remove_prefix(4);
        }
        if (name.size() < 4 || name.size() > 8 ||
            (name.substr(0, 3) != "COM" && name.substr(0, 3) != "com") || name[3] == '0') {
            throw ValidationError("SLCAN port must be COM1..COM65535 or an absolute /dev/... path");
        }
        uint32_t number = 0;
        for (size_t i = 3; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                throw ValidationError("Invalid SLCAN COM port");
            }
            number = number * 10 + static_cast<unsigned>(name[i] - '0');
        }
        if ((number == 0u) || number > 65535) {
            throw ValidationError("SLCAN COM port out of range");
        }
        return "COM" + std::to_string(number);
    }
    auto validate_slcan_config(const SlcanConfig &config) -> void {
        if (config.receive_own_messages) {
            throw ValidationError("SLCAN firmware profiles do not support receive-own messages");
        }
        (void)slcan_port_name(config.port);
        const bool canable = config.protocol == SlcanProtocol::kCanableFd;
        if ((!canable && config.protocol != SlcanProtocol::kLawicelClassic) ||
            (!canable && (config.fd || config.brs)) || (config.brs && !config.fd)) {
            throw ValidationError("FD/BRS requires the explicit CANable FD profile and fd=true");
        }
        const auto *begin = canable ? std::begin(kCanableRates) : std::begin(kRates);
        const auto *end = canable ? std::end(kCanableRates) : std::end(kRates);
        if (std::find(begin, end, config.nominal_bitrate) == end) {
            throw ValidationError("Unsupported SLCAN CAN bitrate for selected firmware profile");
        }
        if (config.fd && config.data_bitrate != 2'000'000 && config.data_bitrate != 5'000'000) {
            throw ValidationError("CANable FD data bitrate must be 2M or 5M");
        }
        if (config.serial_baud < 1200 || config.serial_baud > 3000000) {
            throw ValidationError(
                "SLCAN serial baud must be 1200..3000000 (independent of CAN bitrate)");
        }
        if (config.receive_timeout.count() < 1 || config.receive_timeout.count() > 100 ||
            config.command_timeout.count() < 1 || config.command_timeout.count() > 5000 ||
            config.startup_delay.count() < 0 || config.startup_delay.count() > 5000) {
            throw ValidationError("Invalid SLCAN I/O timeout or startup delay");
        }
    }
    auto encode_slcan_frame(const Message &message) -> std::string {
        return encode_slcan_frame(message, SlcanProtocol::kLawicelClassic);
    }
    auto encode_slcan_frame(const Message &message, SlcanProtocol protocol) -> std::string {
        if (protocol != SlcanProtocol::kLawicelClassic && protocol != SlcanProtocol::kCanableFd) {
            throw ValidationError("Unknown SLCAN firmware profile");
        }
        const auto flags = resolve_tx_flags_with_default(
            protocol == SlcanProtocol::kCanableFd ? LinkKind::kCanFd : LinkKind::kClassic, message,
            false);
        char kind = 't';
        if (flags.is_fd) {
            kind = flags.is_brs ? 'b' : 'd';
        }
        if (message.is_extended) {
            kind = static_cast<char>(kind - 'a' + 'A');
        }
        std::string result(1, kind);
        const auto digits = message.is_extended ? 8 : 3;
        for (int i = digits - 1; i >= 0; --i) {
            result += hex(message.id >> (i * 4));
        }
        const auto dlc = flags.is_fd
                             ? static_cast<unsigned>(
                                   std::lower_bound(kLengths.begin(), kLengths.end(), message.dlc) -
                                   kLengths.begin())
                             : message.dlc;
        result += hex(dlc);
        const auto bytes = flags.is_fd ? kLengths[dlc] : message.dlc;
        for (size_t i = 0; i < bytes; ++i) {
            const auto value = i < message.dlc ? message.data[i] : uint8_t{0};
            result += hex(value >> 4);
            result += hex(value);
        }
        return result + '\r';
    }
    auto decode_slcan_frame(std::string_view line) -> Message {
        return decode_slcan_frame(line, SlcanProtocol::kLawicelClassic);
    }
    auto decode_slcan_frame(std::string_view line, SlcanProtocol protocol) -> Message {
        if (protocol != SlcanProtocol::kLawicelClassic && protocol != SlcanProtocol::kCanableFd) {
            throw ValidationError("Unknown SLCAN firmware profile");
        }
        if (line.empty()) {
            throw ValidationError("Empty SLCAN frame");
        }
        const auto kind = line[0];
        Message message;
        message.is_fd = kind == 'd' || kind == 'D' || kind == 'b' || kind == 'B';
        message.is_brs = kind == 'b' || kind == 'B';
        if ((kind != 't' && kind != 'T' && !message.is_fd) ||
            (message.is_fd && protocol != SlcanProtocol::kCanableFd)) {
            throw ValidationError("Unsupported SLCAN frame type for selected profile");
        }
        message.is_extended = kind == 'T' || kind == 'D' || kind == 'B';
        const size_t digits = message.is_extended ? 8 : 3;
        if (line.size() < digits + 2) {
            throw ValidationError("Incomplete SLCAN frame");
        }
        for (size_t i = 1; i <= digits; ++i) {
            message.id = (message.id << 4) | unhex(line[i]);
        }
        const auto dlc = unhex(line[digits + 1]);
        if (!message.is_fd && dlc > 8) {
            throw ValidationError("Classic SLCAN payload exceeds 8 bytes");
        }
        message.dlc = message.is_fd ? kLengths[dlc] : static_cast<uint8_t>(dlc);
        validate_arbitration_id(message.id, message.is_extended);
        if (line.size() != digits + 2 + static_cast<size_t>(message.dlc * 2)) {
            throw ValidationError("Invalid SLCAN payload length");
        }
        for (size_t i = 0; i < message.dlc; ++i) {
            message.data[i] = static_cast<uint8_t>((unhex(line[digits + 2 + i * 2]) << 4) |
                                                   unhex(line[digits + 3 + i * 2]));
        }
        return message;
    }
    auto retry_slcan_cleanup() -> void {
        retry_native_slcan_cleanup();
        auto &store = cleanup();
        std::lock_guard<std::mutex> lock(store.mutex);
        std::exception_ptr error;
        for (auto it = store.ports.begin(); it != store.ports.end();) {
            try {
                (*it)->cancel();
                (*it)->close();
                it = store.ports.erase(it);
            } catch (...) {
                if (!error) {
                    error = std::current_exception();
                }
                ++it;
            }
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }
    auto pending_slcan_cleanup() -> size_t {
        auto &store = cleanup();
        std::lock_guard<std::mutex> lock(store.mutex);
        return store.ports.size() + pending_native_slcan_cleanup();
    }

    struct SlcanTransport::State {
        SlcanConfig config;
        std::shared_ptr<SlcanSerialPort> serial;
        std::mutex reader, writer, lifecycle, fault_mutex, parser_gate;
        std::condition_variable parser_ready;
        bool ack_writer = false;
        std::atomic<bool> closing{false};
        bool released = false;
        std::string line;
        std::deque<Message> frames;
        uint64_t acknowledgements = 0;
        std::exception_ptr fault;

        void check_fault() {
            std::lock_guard<std::mutex> lock(fault_mutex);
            if (fault) {
                std::rethrow_exception(fault);
            }
        }
        void remember_fault() {
            std::lock_guard<std::mutex> lock(fault_mutex);
            if (!fault) {
                fault = std::current_exception();
            }
        }
        void ingest(std::string_view bytes) {
            if (bytes.size() > 256) {
                throw TransportIOError("SLCAN serial exceeded requested read bound");
            }
            for (char c : bytes) {
                if (c == '\a') {
                    throw TransportIOError("SLCAN adapter rejected command (BEL)");
                }
                if (c == '\r') {
                    if (line.empty() || line == "z" || line == "Z") {
                        ++acknowledgements;
                    } else {
                        if (frames.size() >= kMaxFrames) {
                            throw QueueFullError("SLCAN receive queue overflow");
                        }
                        try {
                            auto message = decode_slcan_frame(line, config.protocol);
                            if (message.is_fd && !config.fd) {
                                throw ValidationError("FD received on Classic SLCAN connection");
                            }
                            frames.push_back(message);
                        } catch (const ValidationError &error) {
                            throw TransportIOError(error.what());
                        }
                    }
                    line.clear();
                } else {
                    if (static_cast<unsigned char>(c) < 0x20 ||
                        static_cast<unsigned char>(c) > 0x7e ||
                        line.size() >= (config.protocol == SlcanProtocol::kCanableFd && config.fd
                                            ? 138U
                                            : 26U)) {
                        throw TransportIOError("Invalid or overlong SLCAN ASCII line");
                    }
                    line += c;
                }
            }
        }
        void pump(std::chrono::milliseconds timeout) {
            check_fault();
            try {
                ingest(serial->read_some(256, timeout));
            } catch (...) {
                remember_fault();
                throw;
            }
        }
        void command(std::string_view command) {
            const auto deadline = Clock::now() + config.command_timeout;
            const auto previous = acknowledgements;
            serial->write_all(command, remaining(deadline));
            // CANable 2 deliberately has no serial ACK/NACK. Completion means
            // only that every byte reached the serial driver, never CAN bus ACK.
            if (config.protocol == SlcanProtocol::kCanableFd) {
                return;
            }
            while (acknowledgements == previous) {
                if (closing) {
                    throw ClosedError("SLCAN is closing");
                }
                if (Clock::now() >= deadline) {
                    throw TimeoutError("SLCAN adapter acknowledgement timeout");
                }
                pump(std::min(config.receive_timeout, remaining(deadline)));
            }
        }
    };

    // Open from owned configuration before moving it into the transport; two
    // delegating-constructor arguments would have unspecified evaluation order.
    struct SlcanTransport::Prepared {
        SlcanConfig config;
        std::shared_ptr<SlcanSerialPort> serial;

        explicit Prepared(SlcanConfig incoming)
            : config(std::move(incoming)), serial(open_slcan_serial(config)) {}
    };
    SlcanTransport::SlcanTransport(SlcanConfig config)
        : SlcanTransport(Prepared{std::move(config)}) {}
    SlcanTransport::SlcanTransport(Prepared prepared)
        : SlcanTransport(std::move(prepared.config), std::move(prepared.serial)) {}
    SlcanTransport::SlcanTransport(SlcanConfig config, std::shared_ptr<SlcanSerialPort> serial) {
        validate_slcan_config(config);
        if (!serial) {
            throw ValidationError("SLCAN serial port is null");
        }
        state_ = std::make_unique<State>();
        auto &s = *state_;
        s.config = std::move(config);
        s.serial = std::move(serial);
        try {
            s.command("C\r");
            const bool canable = s.config.protocol == SlcanProtocol::kCanableFd;
            const auto *begin = canable ? std::begin(kCanableRates) : std::begin(kRates);
            const auto *end = canable ? std::end(kCanableRates) : std::end(kRates);
            const auto index = std::find(begin, end, s.config.nominal_bitrate) - begin;
            s.command(std::string{"S"} + static_cast<char>('0' + index) + '\r');
            if (s.config.fd) {
                s.command(s.config.data_bitrate == 2'000'000 ? "Y2\r" : "Y5\r");
            }
            s.command("O\r");
        } catch (...) {
            s.serial->cancel();
            // O may have reached the adapter even when its ACK was lost. Attempt
            // bus close as well as local release, without replacing the root error.
            try {
                s.serial->write_all("C\r", s.config.command_timeout);
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // Keep the initialization error authoritative, then still try
                // local release below; a failed bus-close command cannot skip it.
            }
            try {
                s.serial->close();
            } catch (...) {
                retain(s.serial);
            }
            throw;
        }
    }
    SlcanTransport::~SlcanTransport() {
        try {
            close();
        } catch (...) {
            if (state_ && !state_->released) {
                retain(state_->serial);
            }
        }
    }
    auto SlcanTransport::read() -> std::optional<Message> {
        auto &s = *state_;
        std::unique_lock<std::mutex> gate(s.parser_gate);
        s.parser_ready.wait(gate, [&] { return !s.ack_writer || s.closing; });
        std::lock_guard<std::mutex> lock(s.reader);
        gate.unlock();
        if (s.closing) {
            throw ClosedError("SLCAN is closed");
        }
        s.check_fault();
        if (s.frames.empty()) {
            s.pump(s.config.receive_timeout);
        }
        if (s.frames.empty()) {
            return {};
        }
        auto frame = s.frames.front();
        s.frames.pop_front();
        return frame;
    }
    auto SlcanTransport::write(const Message &message) -> void {
        auto &s = *state_;
        if ((message.is_fd || message.format == FrameFormat::kFd) && !s.config.fd) {
            throw ValidationError("FD frame requires fd=true");
        }
        const auto encoded = encode_slcan_frame(message, s.config.protocol);
        std::lock_guard<std::mutex> writer(s.writer);
        const auto send = [&] {
            if (s.closing) {
                throw ClosedError("SLCAN is closed");
            }
            s.check_fault();
            try {
                s.command(encoded);
            } catch (...) {
                s.remember_fault();
                throw;
            }
        };
        // CANable has no serial ACK and may send while its reader is waiting.
        if (s.config.protocol == SlcanProtocol::kCanableFd) {
            send();
            return;
        }
        // Lawicel shares the parser for ACKs. Reserve it before waiting for an
        // in-flight read, so a continuous receive loop cannot starve the sender.
        {
            std::lock_guard<std::mutex> gate(s.parser_gate);
            s.ack_writer = true;
        }
        const auto unblock = [](State *state) {
            {
                std::lock_guard<std::mutex> gate(state->parser_gate);
                state->ack_writer = false;
            }
            state->parser_ready.notify_all();
        };
        std::unique_ptr<State, decltype(unblock)> reservation(&s, unblock);
        std::lock_guard<std::mutex> reader(s.reader);
        send();
    }
    auto SlcanTransport::close() -> void {
        auto &s = *state_;
        std::lock_guard<std::mutex> lifecycle(s.lifecycle);
        if (s.released) {
            return;
        }
        s.closing = true;
        s.parser_ready.notify_all();
        s.serial->cancel();
        std::scoped_lock io(s.writer, s.reader);
        std::exception_ptr error;
        // Best effort bus close. Always release the local serial even if this
        // command cannot be sent because USB was unplugged or the link faulted.
        try {
            s.serial->write_all("C\r", s.config.command_timeout);
        } catch (...) {
            error = std::current_exception();
        }
        try {
            s.serial->close();
            s.released = true;
            s.frames.clear();
            s.line.clear();
        } catch (...) {
            throw;
        } // Keep ownership so explicit close can retry.
        if (error) {
            std::rethrow_exception(error);
        }
    }
} // namespace linkerbot::can
