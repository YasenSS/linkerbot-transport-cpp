#pragma once

#include <linkerbot/can/health.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/transport/cancellation.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace linkerbot::can {
    class CANDispatcher;
    class CANTransport;
    class ManagedCANBus;
    class CANBusLease;
} // namespace linkerbot::can

namespace linkerbot::transport {
    class DeviceSession;
    class DeviceOperation;
    class TransportRuntime;

    // Coalesced state-change notification for a language/runtime bridge. Read the
    // revision before inspecting state, then wait on that revision. No application
    // callback executes on the writer or receiver, and no payload queue is added.
    class ChangeSignal {
      public:
        ChangeSignal();
        ~ChangeSignal();
        ChangeSignal(const ChangeSignal &) = delete;
        ChangeSignal &operator=(const ChangeSignal &) = delete;
        [[nodiscard]] uint64_t revision() const;
        [[nodiscard]] bool wait_until(uint64_t observed, Deadline deadline) const;
        void notify() noexcept;

      private:
        struct State;
        std::unique_ptr<State> state_;
    };

    struct RouteKey {
        uint32_t id = 0;
        bool extended = false;
        friend bool operator<(const RouteKey &a, const RouteKey &b) noexcept {
            return a.id < b.id || (a.id == b.id && !a.extended && b.extended);
        }
        friend bool operator==(const RouteKey &a, const RouteKey &b) noexcept {
            return a.id == b.id && a.extended == b.extended;
        }
    };

    struct RouteRule {
        uint32_t value = 0;
        uint32_t mask = 0x7ff;
        bool extended = false;
        friend bool operator<(const RouteRule &a, const RouteRule &b) noexcept {
            return std::tie(a.extended, a.mask, a.value) < std::tie(b.extended, b.mask, b.value);
        }
    };

    struct RoutedMessage {
        can::Message message;
        uint64_t generation = 0;
        uint64_t sequence = 0;
        Deadline received_at{};
        RouteKey route_key{};
        uint64_t received_order = 0; // Same Session event domain as SendResult::claimed_order.
        uint64_t compatibility_sequence = 0;
        uint64_t channel_instance = 0;
    };

    struct RouteHealth {
        bool closed = false;
        uint64_t generation = 0;
        uint64_t overflow = 0;
        size_t pending = 0;
        size_t high_water = 0;
        size_t capacity = 0;
        uint64_t delivered = 0;
        uint64_t dropped_on_close = 0;
    };

    // A group owns all keys atomically. close() wakes receivers and releases keys;
    // destruction is safe after the Session itself has gone away.
    class RouteInbox {
      public:
        ~RouteInbox();
        RouteInbox(const RouteInbox &) = delete;
        RouteInbox &operator=(const RouteInbox &) = delete;
        std::optional<RoutedMessage> receive_until(Deadline deadline);
        std::optional<RoutedMessage> try_receive();
        [[nodiscard]] RouteHealth health() const;
        [[nodiscard]] std::shared_ptr<ChangeSignal> notification() const;
        void close() noexcept;

      private:
        friend class Session;
        struct State;
        explicit RouteInbox(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };

    // Preserve the 0.3 public enumerator spellings and numeric values.
    // NOLINTBEGIN(readability-identifier-naming)
    enum class SendOutcome { pending, dispatched, superseded, cancelled, expired, failed };
    enum class SendPolicy { fifo, latest };
    // NOLINTEND(readability-identifier-naming)

    struct MessageBatch {
        std::vector<can::Message> frames;
        std::chrono::microseconds gap{0};
        // Some classic protocols require a delay before the first frame too.
        std::chrono::microseconds initial_delay{0};
    };

    enum class CapacityUnit { kBatches, kFrames };
    enum class DeadlinePolicy { kExecution, kStartOnly, kDispatchStart };
    // Keep existing numeric values. Resolve kDefault at the submission boundary:
    // whole Session for legacy callers, device endpoint for DeviceSession callers.
    enum class PendingAgeScope { kSession, kEndpoint, kAdmissionDomain, kDefault };

    struct AdmissionLimits {
        size_t fifo_capacity = 2000;
        size_t latest_capacity = 64;
        CapacityUnit fifo_unit = CapacityUnit::kBatches;
    };

    // Shared accounting identity, not another queue or writer. Limits are immutable.
    class AdmissionDomain {
      public:
        explicit AdmissionDomain(AdmissionLimits limits);
        AdmissionDomain(const AdmissionDomain &) = delete;
        AdmissionDomain &operator=(const AdmissionDomain &) = delete;
        [[nodiscard]] AdmissionLimits limits() const noexcept {
            return limits_;
        }

      private:
        AdmissionLimits limits_;
    };

    struct DeviceOptions {
        std::shared_ptr<const AdmissionDomain> admission_domain = nullptr;
        std::chrono::microseconds send_period{0};
        uint32_t max_consecutive_send_errors = 0;
    };

    // Opt-in SDK scheduling. Defaults retain the existing Session behavior.
    struct SessionOptions {
        size_t tx_capacity = 2000;
        bool separate_policy_capacity = false;
        size_t fifo_capacity = 2000;
        size_t latest_capacity = 64;
        CapacityUnit fifo_unit = CapacityUnit::kBatches;
        bool endpoint_fifo = false;
        std::chrono::milliseconds aging_quantum{10};
        std::chrono::microseconds send_period{0};
        uint32_t max_consecutive_send_errors = 0; // zero disables terminal TX faulting
        bool domain_round_robin = false;
        size_t max_route_keys = 0; // zero preserves the legacy unlimited registry
        size_t max_pending_rx = 0;
        size_t max_batch_frames = 0; // zero preserves legacy unbounded batch size
    };

    struct SendOptions {
        Deadline deadline = Deadline::max();
        CancellationToken cancellation;
        std::string endpoint;
        std::string family;
        SendPolicy policy = SendPolicy::fifo;
        int priority = 0;
        std::optional<uint64_t> expected_generation = std::nullopt;
        std::optional<std::chrono::microseconds> max_pending_age = std::nullopt;
        DeadlinePolicy deadline_policy = DeadlinePolicy::kExecution;
        PendingAgeScope pending_age_scope = PendingAgeScope::kDefault;
        std::shared_ptr<const AdmissionDomain> admission_domain = nullptr;
    };

    struct SendResult {
        SendOutcome outcome = SendOutcome::pending;
        uint64_t generation = 0;
        Deadline accepted_at{};
        std::optional<Deadline> claimed_at;
        std::optional<Deadline> completed_at;
        size_t total_frames = 0;
        size_t sent_frames = 0;
        // A backend exception may follow partial delivery of the current frame
        // or native batch; no per-frame acceptance count is known for that call.
        bool current_frame_uncertain = false;
        std::exception_ptr error;
        std::optional<uint64_t> claimed_order = std::nullopt;
        size_t dispatch_completed_frames = 0;
        uint64_t channel_instance = 0;
    };

    class SendReceipt {
      public:
        // Empty receipts have no state: result(), completed(), wait_until() and
        // wait() throw can::ValidationError until assigned a valid receipt.
        SendReceipt() = default;
        // Snapshot without waiting for task completion; takes the state mutex.
        // Saved task errors are returned, not rethrown.
        [[nodiscard]] SendResult result() const;
        // Wait on the steady-clock deadline. A wait timeout throws can::TimeoutError
        // without cancelling the task; a claimed driver write may still complete.
        // On task completion, rethrow its saved error or return its final result.
        // The final backend call may succeed after the send deadline; a wait
        // timeout can therefore precede a later dispatched result.
        [[nodiscard]] SendResult wait_until(Deadline deadline) const;
        // Wait for any terminal outcome without rethrowing its saved task error.
        // Waiting timeout still throws; useful for Future adapters (superseded is a value).
        [[nodiscard]] SendResult wait_result_until(Deadline deadline) const;
        // Wait without a deadline, with the same saved-error behavior.
        void wait() const;
        // True for any terminal outcome, including failure or cancellation.
        [[nodiscard]] bool completed() const;
        [[nodiscard]] std::shared_ptr<ChangeSignal> notification() const;

      private:
        friend class Session;
        struct State;
        explicit SendReceipt(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };

    struct SessionHealth {
        bool open = false;
        uint64_t generation = 0;
        uint64_t rx_frames = 0;
        uint64_t rx_bytes = 0;
        uint64_t route_misses = 0;
        uint64_t invalid_frames = 0;
        uint64_t overflows = 0;
        uint64_t tx_frames = 0;
        uint64_t tx_bytes = 0;
        uint64_t recoveries = 0;
        size_t pending_tx = 0;
        size_t queue_high_water = 0;
        std::string last_error;
        size_t pending_fifo = 0; // capacity units, excludes the claimed batch
        size_t pending_latest = 0;
        bool in_flight = false;
        size_t in_flight_frames = 0;
        std::optional<std::chrono::microseconds> oldest_pending_age = std::nullopt;
        uint64_t accepted = 0;
        uint64_t dispatched = 0;
        uint64_t superseded = 0;
        uint64_t failed = 0;
        uint64_t cancelled = 0;
        uint64_t expired = 0;
        uint64_t rejected_full = 0;
        uint64_t rejected_stale = 0;
        uint32_t consecutive_send_errors = 0;
        uint64_t route_key_errors = 0;
        size_t route_count = 0;
        size_t pending_rx = 0;
        size_t max_pending_rx = 0;
        uint64_t routed_rx_frames = 0;
        uint64_t stale_rx_frames = 0;
        uint64_t route_registration_rejections = 0;
        uint64_t dropped_rx_on_close = 0;
        uint64_t channel_instance = 0;
    };

    // One Session per physical ManagedCANBus/borrowed Dispatcher. All Session
    // entry points submit to one writer. Backend writes additionally share the
    // dispatcher's batch boundary with the legacy send APIs.
    class Session : public std::enable_shared_from_this<Session> {
      public:
        using Factory = std::function<std::unique_ptr<can::CANTransport>()>;
        static std::shared_ptr<Session> create(Factory factory, size_t tx_capacity = 2000);
        static std::shared_ptr<Session> own(std::unique_ptr<can::CANTransport> transport);
        static std::shared_ptr<Session> share(std::shared_ptr<can::ManagedCANBus> bus);
        static std::shared_ptr<Session> lease(can::CANBusLease lease);
        // Caller must keep Dispatcher alive until all runtimes/Session references
        // are destroyed. Physical recover is unavailable for this borrowed adapter.
        static std::shared_ptr<Session> borrow(can::CANDispatcher &dispatcher);
        // Logical device ownership; all devices still submit to this Session's writer.
        std::shared_ptr<DeviceSession> attach_device(std::string identity);
        std::shared_ptr<DeviceSession> attach_device(std::string identity, DeviceOptions options);
        ~Session();
        // Final release inside RX/backend callbacks defers destruction to an
        // external cleanup worker. A borrowed dispatcher must outlive this drain.
        // Call from an external thread before destroying borrowed dispatchers.
        static void drain_deferred_cleanup();
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        std::shared_ptr<RouteInbox> subscribe_routes(const std::vector<RouteKey> &keys,
                                                     size_t capacity = 512);
        // Configure before the first route registration or accepted send. Unmatched RX is allowed.
        // Must be fast, non-blocking and non-reentrant; runs under the state lock.
        // Empty restores exact ID/extended-flag routing. Failures drop only that frame.
        void set_route_key_extractor(std::function<RouteKey(const can::Message &)> extractor);
        [[nodiscard]] std::shared_ptr<ChangeSignal> notification() const;
        void update_routes(const std::shared_ptr<RouteInbox> &inbox,
                           const std::vector<RouteKey> &keys);
        // Validate and enqueue without waiting for transmission. Default capacity counts
        // waiting batches, not the executing batch. Invalid input, closed state,
        // prior cancellation, an expired deadline or a full queue throws here;
        // later transmission errors are retained in the returned receipt.
        SendReceipt submit(MessageBatch batch, SendOptions options = {});
        // Submit and wait until options.deadline; a wait timeout does not cancel.
        void send(MessageBatch batch, SendOptions options = {});
        void cancel_endpoint(const std::string &endpoint);
        // Configure before the first accepted send. No second writer is created.
        void configure(SessionOptions options);
        [[nodiscard]] SessionOptions options() const;
        [[nodiscard]] std::optional<SendReceipt> replace_latest_if_pending(MessageBatch batch,
                                                                           SendOptions options);
        // Permanently gate admission until resume_endpoint(), cancel only unclaimed
        // jobs, and wait for this endpoint's claimed batch without truncating it.
        size_t stop_endpoint(const std::string &endpoint, Deadline deadline);
        void resume_endpoint(const std::string &endpoint);
        SessionHealth health() const;
        // Legacy transport diagnostics projected through the Session boundary.
        can::TransportHealth transport_health() const;
        // Call from an external thread. Concurrent callers with the same observed
        // generation reuse one successful recovery; failed attempts may be retried.
        // Recovery closes old inboxes and aborts unfinished sends without replay;
        // completed receipts remain readable. Re-register routes after success.
        // Requires a recoverable managed bus; explicit close prevents recovery.
        uint64_t recover(uint64_t observed_generation);
        // Call from an external thread. Close inboxes, abort unfinished sends and
        // join the writer; in-flight driver calls may delay completion. Cleanup
        // errors propagate and retained resources can be retried with close().
        void close();

      private:
        friend class DeviceSession;
        struct Impl;
        class Cleanup;
        explicit Session(std::unique_ptr<Impl> impl);
        void start();
        std::optional<SendReceipt> submit_impl(MessageBatch batch, SendOptions options,
                                               bool replace_only,
                                               std::shared_ptr<void> owner = nullptr);
        std::shared_ptr<RouteInbox> subscribe_routes_impl(const std::vector<RouteKey> &keys,
                                                          size_t capacity, const void *device);
        std::shared_ptr<RouteInbox> subscribe_rules_impl(const std::vector<RouteRule> &rules,
                                                         size_t capacity, const void *device,
                                                         bool observation = false);
        std::unique_ptr<Impl> impl_;
    };

    // A device owns its admission gate and inboxes, not the physical connection.
    // Explicit close is retryable after a drain timeout. Destruction attempts close.
    struct DeviceHealth {
        bool accepting = false;
        bool closed = false;
        uint64_t generation = 0;
        size_t owned_operations = 0;
        bool faulted = false;
        uint32_t consecutive_send_errors = 0;
        uint64_t channel_instance = 0;
        size_t pending_fifo = 0;
        size_t pending_latest = 0;
        size_t in_flight = 0;
        std::optional<Deadline> oldest_pending = std::nullopt;
    };

    class DeviceSession : public std::enable_shared_from_this<DeviceSession> {
      public:
        ~DeviceSession();
        DeviceSession(const DeviceSession &) = delete;
        DeviceSession &operator=(const DeviceSession &) = delete;
        SendReceipt submit(MessageBatch batch, SendOptions options = {});
        std::optional<SendReceipt> replace_latest_if_pending(MessageBatch batch,
                                                             SendOptions options);
        std::shared_ptr<RouteInbox> subscribe_routes(const std::vector<RouteKey> &keys,
                                                     size_t capacity = 256);
        std::shared_ptr<RouteInbox> subscribe_rules(const std::vector<RouteRule> &rules,
                                                    size_t capacity = 256);
        // Bounded read-only discovery observation; never takes route ownership.
        std::shared_ptr<RouteInbox> observe_rules(const std::vector<RouteRule> &rules,
                                                  size_t capacity);
        void close_until(Deadline deadline);
        // Register before close begins. Already-owned work may finish while closing.
        DeviceOperation begin_operation();
        [[nodiscard]] DeviceHealth health() const;
        [[nodiscard]] std::shared_ptr<ChangeSignal> notification() const;
        [[nodiscard]] bool closed() const;
        [[nodiscard]] uint64_t generation() const;

      private:
        friend class Session;
        friend class DeviceOperation;
        friend class TransportRuntime;
        struct State;
        explicit DeviceSession(std::shared_ptr<State> state);
        std::optional<SendReceipt> submit_impl(MessageBatch batch, SendOptions options,
                                               bool replace_only, bool owned = false);
        std::shared_ptr<State> state_;
        void set_channel_release(std::function<void()> release);
    };

    // Thread-bound SDK work lease. No worker, payload queue or physical ownership.
    class DeviceOperation {
      public:
        DeviceOperation();
        ~DeviceOperation();
        DeviceOperation(DeviceOperation &&) noexcept;
        DeviceOperation &operator=(DeviceOperation &&) noexcept;
        DeviceOperation(const DeviceOperation &) = delete;
        DeviceOperation &operator=(const DeviceOperation &) = delete;
        SendReceipt submit(MessageBatch batch, SendOptions options = {});

      private:
        friend class DeviceSession;
        struct State;
        explicit DeviceOperation(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
} // namespace linkerbot::transport
