#pragma once
#include <linkerbot/can/health.hpp>

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/can/transport.hpp>
#include <linkerbot/transport/cancellation.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace linkerbot::can {

    using SubscriptionId = uint64_t;

    namespace detail {

        // Thread-safe registry of cancelable callbacks shared by CANDispatcher
        // and ManagedCANBus. A single dispatch thread invokes callbacks against
        // a snapshot taken without holding the registry lock (see snapshot()).
        //
        // Each entry carries `active`/`running`, both guarded by the entry's
        // own mutex. invoke() atomically checks `active` and sets `running`
        // under that mutex before ever calling the callback; cancel() (used by
        // both unsubscribe() and clear()) atomically clears `active` under the
        // same mutex. Because both operations serialize through one mutex,
        // there is no window where invoke() can observe `active` as true after
        // cancel() has already cleared it: whichever acquires the entry's
        // mutex first determines the outcome for that in-flight or as-yet
        // unentered invocation. cancel() then waits (releasing the mutex while
        // waiting) for `running` to become false, unless it is being called
        // from inside the callback's own execution (self-unsubscribe/self-stop),
        // in which case it must not block on its own completion.
        //
        // A callback already copied into an in-flight snapshot but not yet
        // entered is therefore guaranteed to never run once cancel() returns;
        // an already-entered callback is guaranteed to have fully returned
        // before cancel() (and thus unsubscribe()/clear()) returns to an
        // external (different-thread) caller.
        //
        // unsubscribe() and clear() both remove entries from the live `slots_`
        // list (consulted by snapshot()) but retain a reachable reference in
        // `pending_` until cancellation's wait-for-completion barrier has
        // actually finished. This lets a second, concurrent unsubscribe() call
        // for the same id -- one that arrives after the entry has already been
        // removed from `slots_` by clear() or by a first unsubscribe() call --
        // still find the entry and honor the same barrier, instead of finding
        // nothing and returning immediately while the callback may still be
        // running. Only the call that actually removed the entry from
        // `slots_` (the "claimant") returns true; racing callers return false,
        // but only after the barrier they all wait on has been satisfied.
        //
        // A callback that keeps throwing is automatically disabled after
        // kMaxConsecutiveFailures consecutive exceptions (via the same
        // `active` flag) so one perpetually failing subscriber cannot be
        // invoked forever; disabled_count()/last_error() expose this for
        // health reporting so a caller cannot mistake "callback errors are
        // being silently swallowed forever" for a fully healthy bus.
        template <typename Callback>
        class SubscriberRegistry {
          public:
            struct Entry {
                explicit Entry(Callback cb) : callback(std::move(cb)) {}
                Callback callback;
                mutable std::mutex mutex;
                std::condition_variable cv;
                bool active = true;
                size_t running = 0;
                bool failed = false;
                uint32_t consecutive_failures = 0;
                std::string last_error;
            };

            SubscriberRegistry() = default;
            SubscriberRegistry(const SubscriberRegistry &) = delete;
            auto operator=(const SubscriberRegistry &) -> SubscriberRegistry & = delete;

            template <typename Guard>
            auto subscribe_guarded(Callback callback, Guard &&guard) -> SubscriptionId {
                const std::unique_lock lock(mutex_);
                guard();
                const auto id = next_id_++;
                slots_.push_back({id, std::make_shared<Entry>(std::move(callback))});
                return id;
            }

            auto subscribe(Callback callback) -> SubscriptionId {
                return subscribe_guarded(std::move(callback), [] {});
            }

            // See the class comment: honors the cancellation barrier even for
            // a racing second call once the first call has already removed
            // the live slot (via unsubscribe() or a concurrent clear()).
            auto unsubscribe(SubscriptionId id) -> bool {
                std::shared_ptr<Entry> entry;
                bool claimed = false;
                {
                    const std::unique_lock lock(mutex_);
                    auto it = std::find_if(slots_.begin(), slots_.end(),
                                           [id](const Slot &slot) { return slot.id == id; });
                    if (it != slots_.end()) {
                        entry = it->entry;
                        slots_.erase(it);
                        pending_.emplace(id, entry);
                        claimed = true;
                    } else {
                        auto pending_it = pending_.find(id);
                        if (pending_it == pending_.end()) {
                            return false; // never existed, or already fully torn down.
                        }
                        entry = pending_it->second;
                    }
                }
                cancel(*entry);
                if (current_entry_ != entry.get()) {
                    const std::unique_lock lock(mutex_);
                    pending_.erase(id);
                }
                return claimed;
            }

            // Cancels every live entry, each honoring the same barrier as
            // unsubscribe() (including self-cancellation from within a
            // callback that is itself triggering the clear, e.g. a self-stop).
            // A concurrent unsubscribe() for one of these ids will still find
            // it via `pending_` and wait on the same barrier rather than
            // returning immediately.
            auto clear() -> void {
                std::vector<std::pair<SubscriptionId, std::shared_ptr<Entry>>> to_cancel;
                {
                    const std::unique_lock lock(mutex_);
                    to_cancel.reserve(slots_.size() + pending_.size());
                    for (auto &[id, entry] : pending_) to_cancel.emplace_back(id, entry);
                    for (auto &slot : slots_) {
                        to_cancel.emplace_back(slot.id, slot.entry);
                        pending_.emplace(slot.id, slot.entry);
                    }
                    slots_.clear();
                }
                for (auto &[id, entry] : to_cancel) {
                    cancel(*entry);
                    if (current_entry_ != entry.get()) {
                        const std::unique_lock lock(mutex_);
                        pending_.erase(id);
                    }
                }
            }

            [[nodiscard]] auto snapshot() const -> std::vector<std::shared_ptr<Entry>> {
                const std::shared_lock lock(mutex_);
                std::vector<std::shared_ptr<Entry>> out;
                out.reserve(slots_.size());
                for (const auto &slot : slots_) {
                    out.push_back(slot.entry);
                }
                return out;
            }

            [[nodiscard]] auto size() const -> std::size_t {
                const std::shared_lock lock(mutex_);
                return slots_.size();
            }

            [[nodiscard]] auto disabled_count() const -> std::size_t {
                const std::shared_lock lock(mutex_);
                std::size_t count = 0;
                for (const auto &slot : slots_) {
                    const std::scoped_lock entry_lock(slot.entry->mutex);
                    if (slot.entry->failed) {
                        ++count;
                    }
                }
                return count;
            }

            [[nodiscard]] auto last_error() const -> std::string {
                const std::shared_lock lock(mutex_);
                std::string last;
                for (const auto &slot : slots_) {
                    const std::scoped_lock entry_lock(slot.entry->mutex);
                    if (!slot.entry->last_error.empty()) {
                        last = slot.entry->last_error;
                    }
                }
                return last;
            }

            // Invokes entry.callback(args...) unless it has already been (or
            // is concurrently being) cancelled or disabled. A callback that
            // throws kMaxConsecutiveFailures times in a row is disabled (see
            // the class comment) instead of being retried forever silently.
            template <typename... Args>
            static auto invoke(Entry &entry, Args &&...args) -> bool {
                {
                    std::unique_lock lock(entry.mutex);
                    if (!entry.active) {
                        return false;
                    }
                    ++entry.running;
                }
                auto *previous = current_entry_;
                current_entry_ = &entry;
                std::string failure_message;
                try {
                    entry.callback(std::forward<Args>(args)...);
                } catch (const std::exception &ex) {
                    failure_message = ex.what();
                } catch (...) {
                    failure_message = "callback threw a non-standard exception";
                }
                current_entry_ = previous;
                {
                    std::unique_lock lock(entry.mutex);
                    --entry.running;
                    if (failure_message.empty()) {
                        entry.consecutive_failures = 0;
                    } else {
                        entry.last_error = std::move(failure_message);
                        if (++entry.consecutive_failures >= kMaxConsecutiveFailures) {
                            entry.failed = true;
                            entry.active = false;
                        }
                    }
                }
                entry.cv.notify_all();
                return true;
            }

          private:
            static constexpr uint32_t kMaxConsecutiveFailures = 3;

            struct Slot {
                SubscriptionId id;
                std::shared_ptr<Entry> entry;
            };

            // Deactivates entry immediately (under entry.mutex) so no
            // in-flight-but-not-yet-entered invocation can slip through, then
            // waits for an already-running invocation to finish -- unless this
            // call is itself happening from inside that very invocation, in
            // which case waiting would deadlock, so it returns immediately.
            static auto cancel(Entry &entry) -> void {
                std::unique_lock lock(entry.mutex);
                entry.active = false;
                if (current_entry_ == &entry) {
                    return;
                }
                entry.cv.wait(lock, [&] { return !entry.running; });
            }

            static inline thread_local Entry *current_entry_ = nullptr;

            mutable std::shared_mutex mutex_;
            std::vector<Slot> slots_;
            std::unordered_map<SubscriptionId, std::shared_ptr<Entry>> pending_;
            SubscriptionId next_id_ = 1;
        };

    } // namespace detail

    // Shared-bus fan-out: owns one CANTransport, polls it on a dedicated thread,
    // and broadcasts every received frame to all subscribers. Transient receive
    // errors use a short bounded backoff. A configured number of consecutive
    // errors faults the dispatcher and is surfaced exactly once.
    class CANDispatcher {
      public:
        using FrameCallback = std::function<void(const Message &)>;
        using FaultCallback = std::function<void(const CANError &)>;

        struct Options {
            uint32_t max_consecutive_errors = 10;
            std::chrono::milliseconds error_backoff_base{5};
            std::chrono::milliseconds error_backoff_max{50};
            FaultCallback on_fault;
        };

        explicit CANDispatcher(std::unique_ptr<CANTransport> transport)
            : CANDispatcher(std::move(transport), Options{}) {}

        CANDispatcher(std::unique_ptr<CANTransport> transport, Options options)
            : transport_(std::move(transport)), options_(std::move(options)) {
            if (!transport_) {
                throw ValidationError("CANDispatcher requires a non-null transport");
            }
            if (options_.max_consecutive_errors == 0) {
                throw ValidationError("max_consecutive_errors must be positive");
            }
            if (options_.error_backoff_base.count() <= 0 ||
                options_.error_backoff_base > options_.error_backoff_max) {
                throw ValidationError(
                    "dispatcher error backoff must be positive and bounded");
            }

            {
                const std::scoped_lock lock(health_mutex_);
                health_.state = TransportState::kStarting;
                health_.generation = 1;
                health_.state = TransportState::kOpen;
            }
            recv_thread_ = std::thread(&CANDispatcher::recv_loop, this);
        }

        ~CANDispatcher() {
            try {
                stop();
            } catch (...) { // NOLINT(bugprone-empty-catch) destructor must not throw
            }
        }

        CANDispatcher(const CANDispatcher &) = delete;
        CANDispatcher &operator=(const CANDispatcher &) = delete;
        CANDispatcher(CANDispatcher &&) = delete;
        CANDispatcher &operator=(CANDispatcher &&) = delete;

        auto subscribe(FrameCallback callback) -> SubscriptionId {
            if (!callback) {
                throw ValidationError("subscriber callback must be callable");
            }
            return subscribers_.subscribe_guarded(std::move(callback), [this] { require_open(); });
        }

        // Prevents a callback already copied into an in-flight recv_loop()
        // snapshot from ever being entered, and blocks until an already
        // running *external* invocation finishes before returning, so the
        // caller can safely destroy state the callback closes over. Never
        // blocks when called from inside the callback's own execution.
        auto unsubscribe(SubscriptionId id) -> bool {
            return subscribers_.unsubscribe(id);
        }

        // True when called from this dispatcher's own receive thread (e.g.
        // from within a subscriber or fault callback). Lets callers reject
        // synchronous lifecycle calls that would otherwise destroy this
        // dispatcher from its own still-running thread. Uses a thread_local
        // marker set/cleared around recv_loop() rather than comparing against
        // recv_thread_.get_id(), because recv_thread_ itself is concurrently
        // read/joined/moved by stop() from other threads and a std::thread
        // object's members are not safe to read concurrently with those
        // operations.
        [[nodiscard]] auto is_receive_thread() const noexcept -> bool {
            return current_recv_dispatcher_ == this;
        }

        // True when called from *any* CANDispatcher's own receive thread,
        // regardless of which dispatcher instance. Intended for shared
        // infrastructure (e.g. SharedCANBusPool) that cannot check identity
        // against one specific dispatcher, so it can reject a lifecycle call
        // before ever touching a lock or blocking on a join that the calling
        // receive thread itself would need to complete.
        [[nodiscard]] static auto is_any_receive_thread() noexcept -> bool {
            return current_recv_dispatcher_ != nullptr;
        }

        auto send(const Message &frame) -> void {
            // Fast rejection keeps callers from queueing behind an in-flight
            // write after stop/fault has already begun.
            require_open();
            const std::unique_lock send_lock(send_mutex_);
            require_open();
            try {
                transport_->write(frame);
            } catch (const CANError &error) {
                record_send_error(error);
                throw;
            } catch (const std::exception &error) {
                TransportIOError classified(std::string("transport write failed: ") +
                                            error.what());
                record_send_error(classified);
                throw classified;
            } catch (...) {
                InternalError classified("transport write failed with a non-standard exception");
                record_send_error(classified);
                throw classified;
            }

            const std::scoped_lock lock(health_mutex_);
            health_.last_successful_tx = std::chrono::steady_clock::now();
        }

        auto send_batch(const std::vector<Message> &frames) -> void {
            if (frames.empty()) {
                return;
            }
            require_open();
            const std::unique_lock send_lock(send_mutex_);
            require_open();
            try {
                transport_->write_batch(frames);
            } catch (const CANError &error) {
                record_send_error(error);
                throw;
            } catch (const std::exception &error) {
                TransportIOError classified(std::string("transport write_batch failed: ") +
                                            error.what());
                record_send_error(classified);
                throw classified;
            } catch (...) {
                InternalError classified(
                    "transport write_batch failed with a non-standard exception");
                record_send_error(classified);
                throw classified;
            }

            const std::scoped_lock lock(health_mutex_);
            health_.last_successful_tx = std::chrono::steady_clock::now();
        }

        // A single physical execution boundary shared with send/send_batch.
        // Deadline includes waiting for that boundary and every pacing step.
        // Backend exceptions preserve accepted count plus uncertainty of the
        // current frame/native batch; callers must never automatically replay.
        auto send_batch_until(const std::vector<Message> &frames,
                              std::chrono::steady_clock::time_point deadline,
                              std::chrono::microseconds gap,
                              std::chrono::microseconds initial_delay,
                              const std::function<bool()> &cancelled,
                              size_t &sent, bool &uncertain) -> void {
            sent = 0;
            uncertain = false;
            if (gap.count() < 0 || initial_delay.count() < 0)
                throw ValidationError("negative transmit pacing");
            const auto check = [&] {
                require_open();
                if (cancelled && cancelled())
                    throw transport::CancelledError("physical batch cancelled");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw TimeoutError("physical batch deadline expired");
            };
            std::unique_lock<std::mutex> lock(send_mutex_, std::defer_lock);
            while (!lock.try_lock()) {
                check();
                std::this_thread::sleep_until(std::min(deadline,
                    std::chrono::steady_clock::now() + std::chrono::microseconds(250)));
            }
            const auto pace = [&](std::chrono::microseconds delay) {
                const auto until = std::chrono::steady_clock::now() + delay;
                while (std::chrono::steady_clock::now() < until) {
                    check();
                    std::this_thread::sleep_until(std::min({until, deadline,
                        std::chrono::steady_clock::now() + std::chrono::microseconds(250)}));
                }
                check();
            };
            try {
                check();
                pace(initial_delay);
                if (gap.count() == 0 && transport_->prefers_write_batch() && !frames.empty()) {
                    uncertain = true;
                    transport_->write_batch(frames);
                    sent = frames.size();
                    uncertain = false;
                } else {
                    for (const auto &frame : frames) {
                        if (sent) pace(gap);
                        check();
                        uncertain = true;
                        transport_->write(frame);
                        uncertain = false;
                        ++sent;
                    }
                }
                const std::scoped_lock health_lock(health_mutex_);
                health_.last_successful_tx = std::chrono::steady_clock::now();
            } catch (const CANError &error) {
                record_send_error(error);
                throw;
            } catch (const transport::CancelledError &) {
                throw;
            } catch (const std::exception &error) {
                TransportIOError classified(std::string("batch write failed: ") + error.what());
                record_send_error(classified);
                throw classified;
            } catch (...) {
                InternalError classified("batch write failed with a non-standard exception");
                record_send_error(classified);
                throw classified;
            }
        }

        [[nodiscard]] auto prefers_write_batch() const -> bool {
            return transport_ && transport_->prefers_write_batch();
        }

        // Requests stop immediately. New sends are rejected before this method
        // waits for an in-flight send or joins the receive thread. Calling stop()
        // from a receive callback never joins the current thread; the loop marks
        // itself closed when the callback returns.
        auto stop() -> void {
            running_.store(false, std::memory_order_release);
            {
                const std::scoped_lock lock(health_mutex_);
                if (health_.state != TransportState::kClosed) {
                    health_.state = TransportState::kStopping;
                }
            }
            subscribers_.clear();

            if (is_receive_thread()) {
                self_stop_requested_.store(true, std::memory_order_release);
                return;
            }

            const std::scoped_lock stop_lock(stop_mutex_);
            {
                // Drain a send that crossed the stop boundary before running_
                // changed. Senders arriving later observe the closed state.
                const std::scoped_lock send_lock(send_mutex_);
            }
            std::exception_ptr close_error;
            if (!transport_closed_) {
                try {
                    transport_->close();
                    transport_closed_ = true;
                } catch (const CANError &error) {
                    record_close_error(error);
                    close_error = std::current_exception();
                } catch (const std::exception &error) {
                    TransportIOError classified(std::string("transport close failed: ") +
                                                error.what());
                    record_close_error(classified);
                    close_error = std::make_exception_ptr(classified);
                } catch (...) {
                    InternalError classified(
                        "transport close failed with a non-standard exception");
                    record_close_error(classified);
                    close_error = std::make_exception_ptr(classified);
                }
            }
            if (recv_thread_.joinable()) {
                recv_thread_.join();
            }
            if (close_error) {
                std::rethrow_exception(close_error);
            }
            mark_closed();
        }

        [[nodiscard]] auto get_health() const -> TransportHealth {
            TransportHealth snapshot;
            {
                const std::scoped_lock lock(health_mutex_);
                snapshot = health_;
            }
            snapshot.subscriber_count = subscribers_.size();
            snapshot.disabled_subscriber_count = subscribers_.disabled_count();
            auto subscriber_error = subscribers_.last_error();
            if (!subscriber_error.empty()) {
                snapshot.last_subscriber_error = std::move(subscriber_error);
            }
            return snapshot;
        }

      private:
        auto require_open() const -> void {
            const std::scoped_lock lock(health_mutex_);
            if (health_.state == TransportState::kFaulted) {
                throw FaultedError(health_.last_error.empty()
                                       ? "CAN dispatcher is faulted"
                                       : "CAN dispatcher is faulted: " + health_.last_error);
            }
            if (!running_.load(std::memory_order_acquire) ||
                health_.state != TransportState::kOpen) {
                throw ClosedError("CAN dispatcher is closed");
            }
        }

        auto recv_loop() -> void {
            current_recv_dispatcher_ = this;
            struct ClearMarkerOnExit {
                ~ClearMarkerOnExit() { current_recv_dispatcher_ = nullptr; }
            } clear_marker_on_exit;
            (void)clear_marker_on_exit;

            uint32_t consecutive_errors = 0;
            while (running_.load(std::memory_order_acquire)) {
                std::optional<Message> frame;
                try {
                    frame = transport_->read();
                } catch (const CANError &error) {
                    if (!running_.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (!counts_toward_fault(error)) {
                        record_send_error(error);
                        std::this_thread::sleep_for(options_.error_backoff_base);
                        continue;
                    }
                    ++consecutive_errors;
                    if (handle_receive_error(error, consecutive_errors)) {
                        break;
                    }
                    std::this_thread::sleep_for(error_backoff(consecutive_errors));
                    continue;
                } catch (const std::exception &error) {
                    if (!running_.load(std::memory_order_acquire)) {
                        break;
                    }
                    TransportIOError classified(std::string("transport read failed: ") +
                                                error.what());
                    ++consecutive_errors;
                    if (handle_receive_error(classified, consecutive_errors)) {
                        break;
                    }
                    std::this_thread::sleep_for(error_backoff(consecutive_errors));
                    continue;
                } catch (...) {
                    if (!running_.load(std::memory_order_acquire)) {
                        break;
                    }
                    InternalError classified(
                        "transport read failed with a non-standard exception");
                    ++consecutive_errors;
                    if (handle_receive_error(classified, consecutive_errors)) {
                        break;
                    }
                    std::this_thread::sleep_for(error_backoff(consecutive_errors));
                    continue;
                }

                consecutive_errors = 0;
                {
                    const std::scoped_lock lock(health_mutex_);
                    health_.consecutive_errors = 0;
                }
                if (!frame) {
                    continue;
                }
                {
                    const std::scoped_lock lock(health_mutex_);
                    health_.last_successful_rx = std::chrono::steady_clock::now();
                }

                auto snapshot = subscribers_.snapshot();
                for (const auto &entry : snapshot) {
                    if (!running_.load(std::memory_order_acquire)) {
                        break;
                    }
                    detail::SubscriberRegistry<FrameCallback>::invoke(*entry, *frame);
                }
            }

            if (self_stop_requested_.load(std::memory_order_acquire)) {
                finalize_self_stop();
            }
        }

        [[nodiscard]] static auto counts_toward_fault(const CANError &error) -> bool {
            switch (error.code()) {
            case ErrorCode::kValidation:
            case ErrorCode::kTimeout:
                return false;
            default:
                return true;
            }
        }

        [[nodiscard]] auto handle_receive_error(const CANError &error, uint32_t count) -> bool {
            const bool fatal = count >= options_.max_consecutive_errors;
            {
                const std::scoped_lock lock(health_mutex_);
                health_.consecutive_errors = count;
                health_.last_error_code = error.code();
                health_.last_error = error.what();
                if (fatal) {
                    health_.state = TransportState::kFaulted;
                }
            }
            if (!fatal) {
                return false;
            }

            running_.store(false, std::memory_order_release);
            if (!fault_reported_.exchange(true, std::memory_order_acq_rel) &&
                options_.on_fault) {
                try {
                    options_.on_fault(error);
                } catch (...) {
                    // Fault reporting must not terminate the receive thread.
                }
            }
            return true;
        }

        auto record_send_error(const CANError &error) -> void {
            const std::scoped_lock lock(health_mutex_);
            health_.last_error_code = error.code();
            health_.last_error = error.what();
        }

        auto record_close_error(const CANError &error) -> void {
            const std::scoped_lock lock(health_mutex_);
            health_.state = TransportState::kFaulted;
            health_.last_error_code = error.code();
            health_.last_error = error.what();
        }

        auto finalize_self_stop() noexcept -> void {
            // If an external stop already owns stop_mutex_, it is waiting to
            // join this thread and will perform close after this method returns.
            std::unique_lock stop_lock(stop_mutex_, std::try_to_lock);
            if (!stop_lock.owns_lock()) {
                return;
            }
            const std::scoped_lock send_lock(send_mutex_);
            if (!transport_closed_) {
                try {
                    transport_->close();
                    transport_closed_ = true;
                } catch (const CANError &error) {
                    record_close_error(error);
                    return;
                } catch (const std::exception &error) {
                    TransportIOError classified(std::string("transport close failed: ") +
                                                error.what());
                    record_close_error(classified);
                    return;
                } catch (...) {
                    InternalError classified(
                        "transport close failed with a non-standard exception");
                    record_close_error(classified);
                    return;
                }
            }
            mark_closed();
        }

        [[nodiscard]] auto error_backoff(uint32_t count) const
            -> std::chrono::milliseconds {
            const auto base = options_.error_backoff_base.count();
            const auto maximum = options_.error_backoff_max.count();
            if (static_cast<uint64_t>(count) >
                static_cast<uint64_t>(maximum / base)) {
                return options_.error_backoff_max;
            }
            return std::chrono::milliseconds(base * count);
        }

        auto mark_closed() -> void {
            const std::scoped_lock lock(health_mutex_);
            health_.state = TransportState::kClosed;
        }

        static inline thread_local const CANDispatcher *current_recv_dispatcher_ = nullptr;

        std::unique_ptr<CANTransport> transport_;
        Options options_;
        std::atomic<bool> running_{true};
        std::atomic<bool> fault_reported_{false};
        std::atomic<bool> self_stop_requested_{false};
        std::thread recv_thread_;
        mutable std::mutex stop_mutex_;
        mutable std::mutex send_mutex_;
        bool transport_closed_ = false;

        detail::SubscriberRegistry<FrameCallback> subscribers_;

        mutable std::mutex health_mutex_;
        TransportHealth health_;
    };

} // namespace linkerbot::can
