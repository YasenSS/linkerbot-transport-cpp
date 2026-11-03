#include <algorithm>
#include <linkerbot/can/vendor_canfd_transport.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace linkerbot::can {
    namespace {

        constexpr int kStatusOk = 0;
        constexpr int kScanNoDevice = 0;
        constexpr uint8_t kExtendedFlag = 1;
        constexpr uint8_t kStandardFlag = 0;

        constexpr std::array<uint8_t, 16> kDlcToLength = {0, 1,  2,  3,  4,  5,  6,  7,
                                                          8, 12, 16, 20, 24, 32, 48, 64};

        struct LifecycleEntry {
            int refcount = 0;
            std::shared_ptr<VendorCanFdApi> api;
            // Set once refcount has hit zero and a release() has started
            // tearing this entry down. The entry stays in the table (instead
            // of being erased immediately) until Exit actually succeeds, so
            // a failed Exit can be retried by calling release() again for
            // the same key without decrementing refcount a second time.
            bool exiting = false;
            bool unrecorded_cleanup_failure = false;
            std::optional<int> scan_count;
            std::set<std::pair<unsigned, unsigned>> channels;
        };

        // Setup/scan/teardown only. Never hold across frame I/O or its locks.
        // Recursive because cleanup and acquisition share the public lifecycle
        // helpers. Order: coordination -> cleanup -> lifecycle.
        std::recursive_mutex &coordination_mutex() {
            static std::recursive_mutex mutex;
            return mutex;
        }

        std::mutex &lifecycle_mutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::unordered_map<std::string, LifecycleEntry> &lifecycle_table() {
            static std::unordered_map<std::string, LifecycleEntry> table;
            return table;
        }

        struct DeferredCleanup {
            std::shared_ptr<VendorCanFdApi> api;
            unsigned device = 0, channel = 0;
            bool device_closed = false;
        };
        std::mutex &cleanup_mutex() {
            static std::mutex mutex;
            return mutex;
        }
        std::deque<DeferredCleanup> &deferred_cleanups() {
            static std::deque<DeferredCleanup> pending;
            return pending;
        }
        void retain_cleanup(const std::shared_ptr<VendorCanFdApi> &api, unsigned device,
                            unsigned channel, bool device_closed) noexcept {
            const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
            try {
                std::lock_guard<std::mutex> lock(cleanup_mutex());
                deferred_cleanups().push_back({api, device, channel, device_closed});
            } catch (...) {
                // Allocation failure cannot permit Init/unload over an unknown
                // live resource. The existing lifecycle entry retains the API.
                std::lock_guard<std::mutex> lock(lifecycle_mutex());
                const auto it = lifecycle_table().find(api->lifecycle_key);
                if (it != lifecycle_table().end()) {
                    it->second.unrecorded_cleanup_failure = true;
                }
            }
        }

        [[noreturn]] auto
        throw_transport_error(std::string_view what,
                              std::optional<int> native_status = std::nullopt) -> void {
            throw TransportIOError(std::string(what), {}, native_status);
        }

        template <typename Fn>
        auto invoke_vendor(std::string_view operation, Fn &&fn) -> decltype(fn()) {
            try {
                return fn();
            } catch (const CANError &) {
                throw;
            } catch (const std::exception &error) {
                throw TransportIOError(std::string(operation) + " threw: " + error.what());
            } catch (...) {
                throw TransportIOError(std::string(operation) + " threw a non-standard exception");
            }
        }

    } // namespace

    auto vendor_dlc_to_length(uint8_t dlc) -> uint8_t {
        if (dlc >= kDlcToLength.size()) {
            throw ValidationError("vendor DLC out of range (0-15)");
        }
        return kDlcToLength[dlc];
    }

    auto vendor_length_to_dlc(uint8_t length) -> uint8_t {
        if (length > kMaxDataLen) {
            throw ValidationError("CAN FD payload length exceeds 64 bytes");
        }
        for (std::size_t dlc = 0; dlc < kDlcToLength.size(); ++dlc) {
            if (length <= kDlcToLength[dlc]) {
                return static_cast<uint8_t>(dlc);
            }
        }
        throw ValidationError("CAN FD payload length exceeds 64 bytes");
    }

    namespace vendor_canfd_lifecycle {

        auto acquire(const std::shared_ptr<VendorCanFdApi> &api) -> void {
            const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
            if (!api) {
                throw ValidationError("VendorCanFdApi is null");
            }
            if (api->lifecycle_key.empty()) {
                throw ValidationError("VendorCanFdApi.lifecycle_key is empty");
            }
            if (static_cast<bool>(api->lib_init) != static_cast<bool>(api->lib_exit)) {
                throw ValidationError("Vendor lifecycle requires both Init and Exit, or neither");
            }
            retry_deferred_cleanup(api->lifecycle_key);
            const std::scoped_lock lock(lifecycle_mutex());
            auto &entry = lifecycle_table()[api->lifecycle_key];
            if (entry.unrecorded_cleanup_failure) {
                throw FaultedError(
                    "vendor cleanup ownership could not be recorded; library is quarantined");
            }
            if (entry.exiting) {
                throw FaultedError(
                    "vendor Exit is incomplete; retry closing its owner before reopening");
            }
            if (entry.refcount == 0) {
                if (api->lib_init != nullptr) {
                    int status;
                    try {
                        status = invoke_vendor("LibCANbus_Init", [&] { return api->lib_init(); });
                    } catch (...) {
                        lifecycle_table().erase(api->lifecycle_key);
                        throw;
                    }
                    if (status != kStatusOk) {
                        lifecycle_table().erase(api->lifecycle_key);
                        throw_transport_error(
                            "LibCANbus_Init failed with status " + std::to_string(status), status);
                    }
                }
                entry.api = api;
            }
            ++entry.refcount;
        }

        auto release(const std::string &key) -> void {
            const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
            if (key.empty()) {
                return;
            }
            // The last Exit for this key and the next Init (any key) must not
            // overlap (R05): a concurrent acquire() would see the table
            // entry gone and start a fresh LibCANbus_Init while the vendor
            // library global state is still being torn down by Exit. Holding
            // lifecycle_mutex() for the whole decrement/Exit/unload sequence
            // — mirroring how acquire() holds it across Init — makes that
            // overlap impossible instead of just unlikely. It also means a
            // blocking Exit here naturally blocks a concurrent release() (or
            // acquire()) call for any key, not just this one.
            const std::scoped_lock lock(lifecycle_mutex());
            auto it = lifecycle_table().find(key);
            if (it == lifecycle_table().end()) {
                // Nothing left to release: either never acquired, or a
                // previous call already finished tearing this key down.
                return;
            }
            auto &entry = it->second;
            if (!entry.exiting) {
                --entry.refcount;
                if (entry.refcount > 0) {
                    return;
                }
                // Last reference gone. The entry is kept (not erased) from
                // here on so a failed Exit below can be retried by a later
                // release(key) call without a second decrement (R06, applied
                // to the Exit half of close()).
                entry.exiting = true;
            }

            if (entry.api && entry.api->lib_exit != nullptr) {
                // Explicit nonzero status or a thrown exception must reach
                // the caller (VendorCanFdTransport::close()) so it can retry;
                // silently swallowing it here would let close() report
                // success while the vendor library never actually exited.
                const int status =
                    invoke_vendor("LibCANbus_Exit", [&] { return entry.api->lib_exit(); });
                if (status != kStatusOk) {
                    throw_transport_error(
                        "LibCANbus_Exit failed with status " + std::to_string(status), status);
                }
            }

            // Exit succeeded (or there was nothing to Exit): finish tearing
            // the entry down. Only reached on success, so a still-owned
            // handle from a failed attempt is never unloaded out from under
            // a pending retry.
            auto to_unload = std::move(entry.api);
            lifecycle_table().erase(it);
            if (to_unload && !to_unload->library_owner && to_unload->unload != nullptr &&
                to_unload->library_handle != nullptr) {
                to_unload->unload(to_unload->library_handle);
                to_unload->library_handle = nullptr;
            }
        }

        auto retry_deferred_cleanup(const std::string &key) -> void {
            const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
            // Lock order is cleanup -> lifecycle; no lifecycle path calls back
            // into this registry while retaining lifecycle_mutex.
            const std::lock_guard<std::mutex> lock(cleanup_mutex());
            auto &pending = deferred_cleanups();
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->api->lifecycle_key != key) {
                    ++it;
                    continue;
                }
                if (!it->device_closed) {
                    const int status = invoke_vendor("CAN_CloseDevice deferred cleanup", [&] {
                        return it->api->close_device(it->device, it->channel);
                    });
                    if (status != kStatusOk) {
                        throw_transport_error(
                            "CAN_CloseDevice deferred cleanup failed with status " +
                                std::to_string(status),
                            status);
                    }
                    it->device_closed = true;
                }
                {
                    const std::lock_guard<std::mutex> lifecycle(lifecycle_mutex());
                    const auto entry = lifecycle_table().find(key);
                    if (entry != lifecycle_table().end()) {
                        entry->second.channels.erase({it->device, it->channel});
                    }
                }
                release(key); // failed Exit retains this record for a real retry
                it = pending.erase(it);
            }
        }
        auto pending_cleanup_count(const std::string &key) -> size_t {
            const std::lock_guard<std::mutex> lock(cleanup_mutex());
            return static_cast<size_t>(
                std::count_if(deferred_cleanups().begin(), deferred_cleanups().end(),
                              [&](const auto &entry) { return entry.api->lifecycle_key == key; }));
        }

        auto snapshot() -> std::unordered_map<std::string, int> {
            const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
            const std::scoped_lock lock(lifecycle_mutex());
            std::unordered_map<std::string, int> out;
            for (const auto &[key, entry] : lifecycle_table()) {
                out.emplace(key, entry.refcount);
            }
            return out;
        }

        auto reset_for_test() -> void {
            const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
            const std::lock_guard<std::mutex> cleanup_lock(cleanup_mutex());
            const std::lock_guard<std::mutex> lock(lifecycle_mutex());
            deferred_cleanups().clear();
            lifecycle_table().clear();
        }

    } // namespace vendor_canfd_lifecycle

    auto enumerate_vendor_canfd_channels(const std::shared_ptr<VendorCanFdApi> &api,
                                         const std::vector<VendorCanFdChannel> &selected)
        -> VendorCanFdEnumeration {
        if (!api || !api->scan_device) {
            throw ValidationError("Vendor enumeration API is incomplete");
        }
        if (api->native_channel_numbers) {
            for (const auto &channel : selected) {
                if (channel.channel_index != 0) {
                    throw ValidationError("Windows MiniCANFD channel_index must be 0");
                }
            }
        }
        VendorCanFdEnumeration result;
        auto fail = [&](std::string stage, std::string code, std::string message,
                        std::optional<unsigned> dev = {}, std::optional<unsigned> ch = {},
                        std::optional<int> status = {}) {
            result.failures.push_back(
                {std::move(stage), std::move(code), std::move(message), dev, ch, status});
        };
        const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
        try {
            vendor_canfd_lifecycle::retry_deferred_cleanup(api->lifecycle_key);
        } catch (const CANError &error) {
            fail("cleanup", "transport_io", error.what(), {}, {}, error.native_status());
            return result;
        }
        // Includes failed Exit with refcount 0 and failed physical closes.
        if (vendor_canfd_lifecycle::snapshot().count(api->lifecycle_key) != 0u) {
            fail("busy", "backend_busy",
                 "Close all live or pending-cleanup MiniCANFD connections before rescanning");
            return result;
        }
        bool acquired = false;
        try {
            vendor_canfd_lifecycle::acquire(api);
            acquired = true;
            const int count = invoke_vendor("CAN_ScanDevice", [&] { return api->scan_device(); });
            if (count < 0) {
                throw_transport_error("CAN_ScanDevice failed with status " + std::to_string(count),
                                      count);
            }
            auto candidates = selected;
            if (api->native_channel_numbers && candidates.empty()) {
                for (int dev = 0; dev < count; ++dev) {
                    candidates.push_back({static_cast<unsigned>(dev), 0, {}});
                }
            }
            if (candidates.empty() && count > 0) {
                fail("layout", "channel_layout_required",
                     "Linux device count does not identify channel layout; provide confirmed "
                     "device/channel pairs");
            }
            std::set<std::pair<unsigned, unsigned>> inspected;
            for (auto channel : candidates) {
                if (!inspected.insert({channel.device_index, channel.channel_index}).second) {
                    continue;
                }
                if (channel.device_index >= static_cast<unsigned>(count)) {
                    fail("layout", "channel_not_found",
                         "Configured MiniCANFD device was not enumerated", channel.device_index,
                         channel.channel_index);
                    continue;
                }
                if (api->device_level_type && channel.channel_index != 0) {
                    fail("layout", "channel_layout_required",
                         "Device-level type query only verifies channel 0", channel.device_index,
                         channel.channel_index);
                    continue;
                }
                if (!api->channel_type) {
                    fail("type", "type_unavailable", "Vendor library cannot verify channel type",
                         channel.device_index, channel.channel_index);
                    continue;
                }
                try {
                    const int type = invoke_vendor("CAN channel type", [&] {
                        return api->channel_type(channel.device_index, channel.channel_index);
                    });
                    if (type != 1) {
                        fail("type",
                             type == 0 || type == 2 ? "unsupported_channel_type"
                                                    : "type_query_failed",
                             "MiniCANFD channel type is " + std::to_string(type) +
                                 " (FD requires 1)",
                             channel.device_index, channel.channel_index, type);
                        continue;
                    }
                    channel.library_path = api->lifecycle_key;
                    result.channels.push_back(std::move(channel));
                } catch (const CANError &error) {
                    fail("type", "type_query_failed", error.what(), channel.device_index,
                         channel.channel_index, error.native_status());
                }
            }
        } catch (const CANError &error) {
            fail("scan", "transport_io", error.what(), {}, {}, error.native_status());
        }
        if (acquired) {
            try {
                vendor_canfd_lifecycle::release(api->lifecycle_key);
            } catch (const CANError &error) {
                retain_cleanup(api, 0, 0, true);
                fail("cleanup", "transport_io", error.what(), {}, {}, error.native_status());
                // Candidates cannot be consumed until the failed teardown is resolved.
                result.channels.clear();
            }
        }
        return result;
    }

    auto enumerate_vendor_canfd_channels(const std::optional<std::string> &path,
                                         const std::vector<VendorCanFdChannel> &selected)
        -> VendorCanFdEnumeration {
        auto api = load_vendor_canfd_api(path);
        return enumerate_vendor_canfd_channels(api, selected);
    }

    namespace {
        auto load_for_config(const VendorCanFdTransport::Config &config)
            -> std::shared_ptr<VendorCanFdApi> {
#if defined(_WIN32)
            if (config.channel_index != 0)
                throw ValidationError("Windows MiniCANFD channel_index must be 0");
#endif
            return load_vendor_canfd_api(config.library_path);
        }
    } // namespace

    VendorCanFdTransport::VendorCanFdTransport() : VendorCanFdTransport(Config{}) {}

    VendorCanFdTransport::VendorCanFdTransport(const Config &config)
        : VendorCanFdTransport(config, load_for_config(config)) {}

    VendorCanFdTransport::VendorCanFdTransport(Config config, std::shared_ptr<VendorCanFdApi> api)
        : config_(std::move(config)), api_(std::move(api)), tx_batch_(kVendorCanFdMaxTransmitBatch),
          rx_buffer_(kVendorCanFdDefaultMaxReceiveFrames) {
        if (config_.send_timeout_ms < 0 || config_.receive_timeout_ms < 0) {
            throw ValidationError("vendor CAN FD timeouts must be non-negative");
        }
        if (config_.receive_timeout_ms > kVendorCanFdMaxReceiveTimeoutMs) {
            throw ValidationError("vendor CAN FD receive timeout must not exceed 10ms");
        }
        if (config_.send_timeout_ms > kVendorCanFdMaxSendTimeoutMs) {
            throw ValidationError("vendor CAN FD send timeout must not exceed 100ms");
        }
        if (!api_) {
            throw ValidationError("VendorCanFdApi is null");
        }
        if (api_->native_channel_numbers && config_.channel_index != 0) {
            throw ValidationError("Windows MiniCANFD channel_index must be 0");
        }
        if (api_->scan_device == nullptr || api_->open_device == nullptr ||
            api_->close_device == nullptr || api_->canfd_init == nullptr ||
            api_->transmit == nullptr || api_->receive == nullptr) {
            throw ValidationError("VendorCanFdApi is missing required entry points");
        }

        const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
        lifecycle_key_ = api_->lifecycle_key;
        vendor_canfd_lifecycle::acquire(api_);
        try {
            open_device();
        } catch (...) {
            const auto original = std::current_exception();
            try {
                close();
            } catch (...) {
                retain_cleanup(api_, config_.device_index, config_.channel_index, device_closed_);
                // Preserve the original initialization error; pending cleanup
                // remains visible and retryable through lifecycle diagnostics.
            }
            std::rethrow_exception(original);
        }
    }

    VendorCanFdTransport::~VendorCanFdTransport() {
        try {
            VendorCanFdTransport::close();
        } catch (...) { // destructor transfers unfinished resource ownership
            retain_cleanup(api_, config_.device_index, config_.channel_index, device_closed_);
        }
    }

    auto VendorCanFdTransport::open_device() -> void {
        // Caller holds coordination throughout acquire/scan/reserve/open/init.
        auto &entry = lifecycle_table().at(lifecycle_key_);
        if (!entry.scan_count) {
            const int count = invoke_vendor("CAN_ScanDevice", [&] { return api_->scan_device(); });
            if (count < 0) {
                throw_transport_error("CAN_ScanDevice failed with status " + std::to_string(count),
                                      count);
            }
            entry.scan_count = count;
        }
        const int count = *entry.scan_count;
        if (count < kScanNoDevice) {
            throw_transport_error("CAN_ScanDevice failed with status " + std::to_string(count),
                                  count);
        }
        if (count == kScanNoDevice) {
            throw TransportIOError("No CANFD adapter found");
        }
        if (config_.device_index >= static_cast<unsigned>(count)) {
            throw ValidationError("CANFD device_index " + std::to_string(config_.device_index) +
                                  " out of range; found " + std::to_string(count) + " adapter(s)");
        }

        if (!entry.channels.insert({config_.device_index, config_.channel_index}).second) {
            throw FaultedError("MiniCANFD channel is already owned by this process");
        }
        channel_reserved_ = true;
        if (api_->channel_type) {
            const int type = invoke_vendor("CAN channel type", [&] {
                return api_->channel_type(config_.device_index, config_.channel_index);
            });
            if (type != 1) {
                throw_transport_error("MiniCANFD channel is not confirmed CAN FD (type " +
                                          std::to_string(type) + ")",
                                      type);
            }
        }

        const int open_status = invoke_vendor("CAN_OpenDevice", [&] {
            return api_->open_device(config_.device_index, config_.channel_index);
        });
        if (open_status != kStatusOk) {
            throw_transport_error(
                "CAN_OpenDevice failed with status " + std::to_string(open_status), open_status);
        }

        device_closed_ = false;
        CanFDConfig cfg{};
        cfg.NomBaud = config_.nom_baud;
        cfg.DatBaud = config_.dat_baud;
        cfg.Config = config_.config_flags;
        cfg.Model = config_.model;
        cfg.Cantype = config_.cantype;
        const int init_status = invoke_vendor("CANFD_Init", [&] {
            return api_->canfd_init(config_.device_index, config_.channel_index, &cfg);
        });
        if (init_status != kStatusOk) {
            throw_transport_error("CANFD_Init failed with status " + std::to_string(init_status),
                                  init_status);
        }

        {
            const std::scoped_lock lock(close_mutex_);
            device_closed_ = false;
            lifecycle_done_ = false;
        }
        closed_.store(false);
    }

    auto VendorCanFdTransport::ensure_open_unlocked() const -> void {
        if (closed_.load()) {
            throw ClosedError("CANFD interface is closed");
        }
    }

    auto VendorCanFdTransport::encode_tx_frame(const Message &message, CanFDMsg &out) -> void {
        const auto flags = resolve_tx_flags(LinkKind::kCanFd, message);
        out = CanFDMsg{};
        out.ID = message.id;
        out.FrameType = encode_frame_type(flags.is_fd, flags.is_brs);
        out.DLC = vendor_length_to_dlc(message.dlc);
        out.ExternFlag = message.is_extended ? kExtendedFlag : kStandardFlag;
        out.RemoteFlag = 0;
        std::memcpy(static_cast<void *>(out.Data), message.data.data(), message.dlc);
    }

    auto VendorCanFdTransport::fill_tx_frame(const Message &message) -> void {
        encode_tx_frame(message, tx_frame_);
    }

    auto VendorCanFdTransport::frame_to_message(const CanFDMsg &frame) const -> Message {
        Message msg;
        msg.id = frame.ID;
        msg.is_extended = frame.ExternFlag != 0;
        const auto flags = decode_frame_type(frame.FrameType);
        msg.is_fd = flags.is_fd;
        msg.is_brs = flags.is_brs;
        validate_arbitration_id(msg.id, msg.is_extended);
        msg.dlc = vendor_dlc_to_length(frame.DLC);
        validate_payload_length(msg.dlc, msg.is_fd ? LinkKind::kCanFd : LinkKind::kClassic);
        std::memcpy(msg.data.data(), static_cast<const void *>(frame.Data), msg.dlc);
        return msg;
    }

    auto VendorCanFdTransport::read() -> std::optional<Message> {
        if (closed_.load()) {
            throw ClosedError("CANFD interface is closed");
        }

        const std::scoped_lock lock(rx_mutex_);
        ensure_open_unlocked();

        if (rx_inbox_.empty()) {
            const int received = invoke_vendor("CANFD_Receive", [&] {
                return api_->receive(config_.device_index, config_.channel_index, rx_buffer_.data(),
                                     static_cast<unsigned>(rx_buffer_.size()),
                                     config_.receive_timeout_ms);
            });
            if (received < kStatusOk) {
                throw_transport_error(
                    "CANFD_Receive failed with status " + std::to_string(received), received);
            }
            if (received == kStatusOk) {
                return std::nullopt;
            }
            if (static_cast<unsigned>(received) > rx_buffer_.size()) {
                throw TransportIOError("CANFD_Receive returned more frames than requested");
            }
            std::exception_ptr decode_error;
            for (int i = 0; i < received; ++i) {
                try {
                    const auto &frame = rx_buffer_[static_cast<size_t>(i)];
                    if (frame.RemoteFlag != 0u) {
                        continue; // RTR contains no application payload.
                    }
                    rx_inbox_.push_back(frame_to_message(frame));
                } catch (const ValidationError &) {
                    // The vendor Receive() has already consumed this batch from
                    // the adapter. Keep decoding later slots so a malformed
                    // frame cannot permanently drop its neighbours.
                    if (!decode_error) {
                        decode_error = std::current_exception();
                    }
                }
            }
            if (rx_inbox_.empty() && decode_error) {
                std::rethrow_exception(decode_error);
            }
        }

        if (rx_inbox_.empty()) {
            return std::nullopt;
        }
        Message msg = rx_inbox_.front();
        rx_inbox_.pop_front();
        return msg;
    }

    auto VendorCanFdTransport::write(const Message &message) -> void {
        const std::scoped_lock lock(tx_mutex_);
        ensure_open_unlocked();
        fill_tx_frame(message);
        const int sent = invoke_vendor("CANFD_Transmit", [&] {
            return api_->transmit(config_.device_index, config_.channel_index, &tx_frame_, 1,
                                  config_.send_timeout_ms);
        });
        if (sent != 1) {
            throw_transport_error(
                "CANFD_Transmit failed: expected 1 frame, got " + std::to_string(sent), sent);
        }
    }

    auto VendorCanFdTransport::write_batch(const std::vector<Message> &messages) -> void {
        if (messages.empty()) {
            return;
        }
        if (messages.size() > kVendorCanFdMaxTransmitBatch) {
            throw ValidationError(
                "send_batch supports at most " + std::to_string(kVendorCanFdMaxTransmitBatch) +
                " frames per call (vendor limit), got " + std::to_string(messages.size()));
        }
        if (messages.size() == 1) {
            write(messages.front());
            return;
        }

        const std::scoped_lock lock(tx_mutex_);
        ensure_open_unlocked();
        for (std::size_t i = 0; i < messages.size(); ++i) {
            encode_tx_frame(messages[i], tx_batch_[i]);
        }
        const auto count = static_cast<unsigned>(messages.size());
        const int sent = invoke_vendor("CANFD_Transmit", [&] {
            return api_->transmit(config_.device_index, config_.channel_index, tx_batch_.data(),
                                  count, config_.send_timeout_ms);
        });
        if (sent != static_cast<int>(count)) {
            throw_transport_error("CANFD_Transmit batch short send: expected " +
                                      std::to_string(count) + ", got " + std::to_string(sent),
                                  sent);
        }
    }

    auto VendorCanFdTransport::close() -> void {
        // Reject new I/O immediately and unconditionally, even if the
        // close attempt below fails or is still retrying. This is separate
        // from "fully closed" (R06): a caller must be able to keep retrying
        // without any read()/write() sneaking in on a half-closed device in
        // between.
        closed_.store(true);

        std::unique_lock lock(close_mutex_);
        for (;;) {
            if (lifecycle_done_) {
                return;
            }
            if (closing_in_progress_) {
                // A second close() call must observe the *entire* attempt
                // (device close and, when applicable, the vendor Exit call)
                // finishing, not just the device-close half of it — waiting
                // on lifecycle_done_ here (rather than device_closed_) is
                // what keeps a concurrent close() from reporting success
                // while Exit is still in flight.
                close_cv_.wait(lock, [this] { return lifecycle_done_ || !closing_in_progress_; });
                continue;
            }

            closing_in_progress_ = true;
            lock.unlock();

            // Quiesce any read()/write() that is already past ensure_open_unlocked()
            // before touching the vendor handle.
            { const std::scoped_lock tx_lock(tx_mutex_); }
            { const std::scoped_lock rx_lock(rx_mutex_); }

            std::exception_ptr failure;
            try {
                const std::lock_guard<std::recursive_mutex> coordination(coordination_mutex());
                if (!device_closed_) {
                    const int close_status = invoke_vendor("CAN_CloseDevice", [&] {
                        return api_->close_device(config_.device_index, config_.channel_index);
                    });
                    if (close_status != kStatusOk) {
                        throw_transport_error("CAN_CloseDevice failed with status " +
                                                  std::to_string(close_status),
                                              close_status);
                    }
                    // Once this succeeds the device is physically closed for
                    // good: a later retry (e.g. after an Exit failure below)
                    // must not call CAN_CloseDevice a second time.
                    device_closed_ = true;
                }

                if (channel_reserved_) {
                    lifecycle_table()
                        .at(lifecycle_key_)
                        .channels.erase({config_.device_index, config_.channel_index});
                    channel_reserved_ = false;
                }

                if (!lifecycle_key_.empty()) {
                    // May throw (explicit nonzero LibCANbus_Exit status or an
                    // exception) when this is the last reference; that
                    // failure must propagate to the caller, not be swallowed,
                    // and must not unload state release() has not actually
                    // torn down. release() itself is safe to call again on
                    // retry: it does not double-decrement the refcount.
                    vendor_canfd_lifecycle::release(lifecycle_key_);
                    lifecycle_key_.clear();
                }
            } catch (...) {
                failure = std::current_exception();
            }

            lock.lock();
            closing_in_progress_ = false;
            if (!failure) {
                lifecycle_done_ = true;
            }
            lock.unlock();
            close_cv_.notify_all();

            if (failure) {
                std::rethrow_exception(failure);
            }
            return;
        }
    }

    auto make_vendor_canfd_transport() -> std::unique_ptr<VendorCanFdTransport> {
        return std::make_unique<VendorCanFdTransport>();
    }

    auto make_vendor_canfd_transport(const VendorCanFdTransport::Config &config)
        -> std::unique_ptr<VendorCanFdTransport> {
        return std::make_unique<VendorCanFdTransport>(config);
    }

} // namespace linkerbot::can
