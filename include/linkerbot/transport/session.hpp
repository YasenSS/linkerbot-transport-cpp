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
#include <vector>

namespace linkerbot::can {
    class CANDispatcher;
    class CANTransport;
    class ManagedCANBus;
    class CANBusLease;
} // namespace linkerbot::can

namespace linkerbot::transport {

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

    struct RoutedMessage {
        can::Message message;
        uint64_t generation = 0;
        uint64_t sequence = 0;
        Deadline received_at{};
        RouteKey route_key{};
        uint64_t received_order = 0; // Same Session event domain as SendResult::claimed_order.
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
    enum class DeadlinePolicy { kExecution, kStartOnly };

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
        ~Session();
        // Final release inside RX/backend callbacks defers destruction to an
        // external cleanup worker. A borrowed dispatcher must outlive this drain.
        // Call from an external thread before destroying borrowed dispatchers.
        static void drain_deferred_cleanup();
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        std::shared_ptr<RouteInbox> subscribe_routes(const std::vector<RouteKey> &keys,
                                                     size_t capacity = 512);
        // Configure before any routes or TX/RX events. The extractor must be fast,
        // non-blocking and must not reenter Session; it runs under the state lock.
        // Empty restores exact ID/format routing. Failures drop only that frame.
        void set_route_key_extractor(std::function<RouteKey(const can::Message &)> extractor);
        [[nodiscard]] std::shared_ptr<ChangeSignal> notification() const;
        void update_routes(const std::shared_ptr<RouteInbox> &inbox,
                           const std::vector<RouteKey> &keys);
        // Validate and enqueue without waiting for transmission. Capacity counts
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
        struct Impl;
        class Cleanup;
        explicit Session(std::unique_ptr<Impl> impl);
        void start();
        std::optional<SendReceipt> submit_impl(MessageBatch batch, SendOptions options,
                                               bool replace_only);
        std::unique_ptr<Impl> impl_;
    };
} // namespace linkerbot::transport
