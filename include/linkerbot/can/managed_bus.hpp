#pragma once

#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/can/transport.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace linkerbot::can {

    struct SendReceiptState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::exception_ptr error;
    };

    // Completes when the backend has accepted the frame, or with a typed error.
    // Enqueue success is not physical send success.
    class SendReceipt {
      public:
        explicit SendReceipt(std::shared_ptr<SendReceiptState> state) : state_(std::move(state)) {
            if (!state_) {
                throw ValidationError("send receipt is empty");
            }
        }

        auto wait() const -> void {
            std::unique_lock lock(state_->mutex);
            state_->cv.wait(lock, [&] { return state_->done; });
            if (state_->error) {
                std::rethrow_exception(state_->error);
            }
        }

        auto wait_for(std::chrono::milliseconds timeout) const -> void {
            std::unique_lock lock(state_->mutex);
            if (!state_->cv.wait_for(lock, timeout, [&] { return state_->done; })) {
                throw TimeoutError("send receipt timed out");
            }
            if (state_->error) {
                std::rethrow_exception(state_->error);
            }
        }

        [[nodiscard]] auto completed() const -> bool {
            const std::scoped_lock lock(state_->mutex);
            return state_->done;
        }

      private:
        std::shared_ptr<SendReceiptState> state_;
    };

    // SDK-facing lifecycle owner. Construction stores a factory and does not
    // open a device or start threads. start()/stop()/recover() are serialized.
    class ManagedCANBus {
      public:
        using TransportFactory = std::function<std::unique_ptr<CANTransport>()>;
        using FrameCallback = CANDispatcher::FrameCallback;
        using FaultCallback = CANDispatcher::FaultCallback;

        struct Options {
            CANDispatcher::Options dispatcher;
            std::size_t send_queue_size = 2000;
            std::size_t send_batch_max = 32;
            std::chrono::milliseconds send_interval{0};
            FaultCallback on_fault;
        };

        explicit ManagedCANBus(TransportFactory factory) : ManagedCANBus(std::move(factory), Options{}) {}

        ManagedCANBus(TransportFactory factory, Options options)
            : factory_(std::move(factory)), options_(std::move(options)) {
            if (!factory_) {
                throw ValidationError("ManagedCANBus factory must be callable");
            }
            if (options_.send_queue_size == 0) {
                throw ValidationError("send_queue_size must be positive");
            }
            if (options_.send_batch_max == 0) {
                throw ValidationError("send_batch_max must be positive");
            }
            if (options_.send_interval.count() < 0) {
                throw ValidationError("send_interval must be non-negative");
            }
        }

        ~ManagedCANBus() {
            try {
                stop();
            } catch (...) { // NOLINT(bugprone-empty-catch) destructor must not throw
            }
        }

        ManagedCANBus(const ManagedCANBus &) = delete;
        auto operator=(const ManagedCANBus &) -> ManagedCANBus & = delete;
        ManagedCANBus(ManagedCANBus &&) = delete;
        auto operator=(ManagedCANBus &&) -> ManagedCANBus & = delete;

        auto start() -> void {
            reject_if_receive_thread();
            const std::scoped_lock lock(operation_mutex_);
            start_locked();
        }

        auto stop() -> void {
            reject_if_receive_thread();
            const std::scoped_lock lock(operation_mutex_);
            stop_locked();
        }

        auto recover() -> void {
            reject_if_receive_thread();
            const std::scoped_lock lock(operation_mutex_);
            stop_locked();
            start_locked();
        }

        auto subscribe(FrameCallback callback) -> SubscriptionId {
            if (!callback) {
                throw ValidationError("subscriber callback must be callable");
            }
            return subscribers_.subscribe(std::move(callback));
        }

        // Prevents a callback already copied into an in-flight on_message()
        // snapshot from ever being entered, and blocks until an already
        // running *external* invocation finishes before returning. Never
        // blocks when called from inside the callback's own execution.
        auto unsubscribe(SubscriptionId id) -> bool {
            return subscribers_.unsubscribe(id);
        }

        auto send(const Message &message) -> void {
            const auto dispatcher = require_dispatcher();
            dispatcher->send(message);
        }

        auto send_batch_until(const std::vector<Message> &frames,
                              std::chrono::steady_clock::time_point deadline,
                              std::chrono::microseconds gap,
                              std::chrono::microseconds initial_delay,
                              const std::function<bool()> &cancelled,
                              size_t &sent, bool &uncertain) -> void {
            const auto dispatcher = require_dispatcher();
            dispatcher->send_batch_until(frames, deadline, gap, initial_delay,
                                         cancelled, sent, uncertain);
        }

        auto queue_send(const Message &message) -> SendReceipt {
            auto state = std::make_shared<SendReceiptState>();
            {
                const std::scoped_lock lock(state_mutex_);
                (void)require_dispatcher_locked();
                const std::scoped_lock queue_lock(queue_mutex_);
                if (send_queue_.size() >= options_.send_queue_size) {
                    throw QueueFullError("CAN send queue is full");
                }
                send_queue_.push_back({message, state, active_token_});
            }
            ensure_send_thread();
            queue_cv_.notify_one();
            return SendReceipt(std::move(state));
        }

        [[nodiscard]] auto get_health() const -> TransportHealth {
            TransportHealth snapshot;
            std::shared_ptr<CANDispatcher> dispatcher;
            {
                const std::scoped_lock lock(state_mutex_);
                snapshot.state = state_;
                snapshot.generation = generation_;
                snapshot.last_error_code = last_error_code_;
                snapshot.last_error = last_error_;
                dispatcher = dispatcher_;
            }
            if (dispatcher) {
                auto inner = dispatcher->get_health();
                snapshot.consecutive_errors = inner.consecutive_errors;
                snapshot.last_successful_tx = inner.last_successful_tx;
                snapshot.last_successful_rx = inner.last_successful_rx;
                if (inner.last_error_code.has_value()) {
                    snapshot.last_error_code = inner.last_error_code;
                    snapshot.last_error = inner.last_error;
                }
            }
            {
                const std::scoped_lock lock(queue_mutex_);
                snapshot.pending_tx = send_queue_.size();
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
        struct QueuedSend {
            Message message;
            std::shared_ptr<SendReceiptState> receipt;
            std::shared_ptr<void> generation_token;
        };

        auto start_locked() -> void {
            std::shared_ptr<void> token;
            {
                const std::scoped_lock lock(state_mutex_);
                if (state_ == TransportState::kOpen) {
                    return;
                }
                if (dispatcher_) {
                    throw FaultedError(
                        "ManagedCANBus owns a failed dispatcher; call recover() or stop()");
                }
                state_ = TransportState::kStarting;
                token = std::make_shared<int>(0);
                active_token_ = token;
            }

            CANDispatcher::Options dispatcher_options = options_.dispatcher;
            dispatcher_options.on_fault = [this, token](const CANError &error) {
                on_dispatcher_fault(token, error);
            };

            std::unique_ptr<CANDispatcher> created;
            try {
                auto transport = factory_();
                if (!transport) {
                    throw ValidationError("ManagedCANBus factory returned a null transport");
                }
                created = std::make_unique<CANDispatcher>(std::move(transport),
                                                          std::move(dispatcher_options));
                created->subscribe([this, token](const Message &message) {
                    on_message(token, message);
                });
                {
                    const std::scoped_lock lock(state_mutex_);
                    if (state_ == TransportState::kFaulted) {
                        throw FaultedError(last_error_.empty()
                                               ? "dispatcher failed while starting"
                                               : last_error_);
                    }
                    dispatcher_ = std::shared_ptr<CANDispatcher>(created.release());
                    ++generation_;
                    last_error_code_.reset();
                    last_error_.clear();
                    state_ = TransportState::kOpen;
                }
            } catch (...) {
                auto error = std::current_exception();
                if (created) {
                    try {
                        created->stop();
                    } catch (...) {
                        const std::scoped_lock lock(state_mutex_);
                        dispatcher_ = std::shared_ptr<CANDispatcher>(created.release());
                        state_ = TransportState::kFaulted;
                        record_error_locked(std::current_exception());
                        active_token_.reset();
                        std::rethrow_exception(error);
                    }
                }
                {
                    const std::scoped_lock lock(state_mutex_);
                    if (!dispatcher_) {
                        active_token_.reset();
                    }
                    state_ = TransportState::kFaulted;
                    record_error_locked(error);
                }
                std::rethrow_exception(error);
            }
        }

        auto stop_locked() -> void {
            std::shared_ptr<CANDispatcher> dispatcher;
            {
                const std::scoped_lock lock(state_mutex_);
                if (state_ == TransportState::kClosed && !dispatcher_) {
                    return;
                }
                if (!dispatcher_) {
                    active_token_.reset();
                    state_ = TransportState::kClosed;
                    return;
                }
                state_ = TransportState::kStopping;
                active_token_.reset();
                dispatcher = dispatcher_;
            }

            send_running_.store(false, std::memory_order_release);
            queue_cv_.notify_all();
            fail_queued(std::make_exception_ptr(
                ClosedError("CAN bus stopped before queued send completed")));

            std::exception_ptr close_error;
            {
                const std::scoped_lock send_thread_lock(send_thread_mutex_);
                send_running_.store(false, std::memory_order_release);
                queue_cv_.notify_all();
                try {
                    dispatcher->stop();
                } catch (...) {
                    close_error = std::current_exception();
                }
                if (send_thread_.joinable()) {
                    send_thread_.join();
                }
            }

            {
                const std::scoped_lock lock(state_mutex_);
                if (close_error) {
                    state_ = TransportState::kFaulted;
                    record_error_locked(close_error);
                } else {
                    dispatcher_.reset();
                    state_ = TransportState::kClosed;
                }
            }
            if (close_error) {
                std::rethrow_exception(close_error);
            }
        }

        auto require_dispatcher_locked() const -> std::shared_ptr<CANDispatcher> {
            if (state_ == TransportState::kFaulted) {
                throw FaultedError(last_error_.empty()
                                       ? "ManagedCANBus is faulted"
                                       : "ManagedCANBus is faulted: " + last_error_);
            }
            if (state_ != TransportState::kOpen || !dispatcher_) {
                throw ClosedError("ManagedCANBus is closed");
            }
            return dispatcher_;
        }

        auto require_dispatcher() -> std::shared_ptr<CANDispatcher> {
            const std::scoped_lock lock(state_mutex_);
            return require_dispatcher_locked();
        }

        auto ensure_send_thread() -> void {
            const std::scoped_lock lock(send_thread_mutex_);
            if (send_thread_.joinable()) {
                return;
            }
            {
                const std::scoped_lock state_lock(state_mutex_);
                if (state_ != TransportState::kOpen || !dispatcher_) {
                    return;
                }
                send_running_.store(true, std::memory_order_release);
            }
            send_thread_ = std::thread(&ManagedCANBus::send_loop, this);
        }

        auto send_loop() -> void {
            while (send_running_.load(std::memory_order_acquire)) {
                std::vector<QueuedSend> batch;
                {
                    std::unique_lock lock(queue_mutex_);
                    queue_cv_.wait(lock, [&] {
                        return !send_queue_.empty() ||
                               !send_running_.load(std::memory_order_acquire);
                    });
                    if (!send_running_.load(std::memory_order_acquire)) {
                        break;
                    }
                    while (!send_queue_.empty() && batch.size() < options_.send_batch_max) {
                        batch.push_back(std::move(send_queue_.front()));
                        send_queue_.pop_front();
                    }
                }
                if (batch.empty()) {
                    continue;
                }

                std::shared_ptr<CANDispatcher> dispatcher;
                {
                    const std::scoped_lock lock(state_mutex_);
                    if (state_ != TransportState::kOpen || !dispatcher_) {
                        auto error = std::make_exception_ptr(
                            ClosedError("queued send dropped because the bus recovered"));
                        for (auto &item : batch) {
                            complete_receipt(*item.receipt, error);
                        }
                        continue;
                    }
                    dispatcher = dispatcher_;
                }
                dispatch_batch(*dispatcher, std::move(batch));
                if (options_.send_interval.count() > 0) {
                    std::this_thread::sleep_for(options_.send_interval);
                }
            }
        }

        auto dispatch_batch(CANDispatcher &dispatcher, std::vector<QueuedSend> batch) -> void {
            std::shared_ptr<void> token;
            {
                const std::scoped_lock lock(state_mutex_);
                token = active_token_;
            }
            std::vector<QueuedSend> live;
            live.reserve(batch.size());
            for (auto &item : batch) {
                if (item.generation_token != token) {
                    complete_receipt(*item.receipt,
                                     std::make_exception_ptr(ClosedError(
                                         "queued send dropped because the bus recovered")));
                    continue;
                }
                live.push_back(std::move(item));
            }
            if (live.empty()) {
                return;
            }

            std::vector<Message> frames;
            frames.reserve(live.size());
            for (const auto &item : live) {
                frames.push_back(item.message);
            }

            try {
                if (frames.size() > 1 && dispatcher.prefers_write_batch()) {
                    dispatcher.send_batch(frames);
                } else {
                    for (const auto &frame : frames) {
                        dispatcher.send(frame);
                    }
                }
            } catch (...) {
                auto error = std::current_exception();
                for (auto &item : live) {
                    complete_receipt(*item.receipt, error);
                }
                return;
            }
            for (auto &item : live) {
                complete_receipt(*item.receipt, nullptr);
            }
        }

        auto on_message(const std::shared_ptr<void> &token, const Message &message) -> void {
            {
                const std::scoped_lock lock(state_mutex_);
                if (token != active_token_ || state_ != TransportState::kOpen) {
                    return;
                }
            }
            auto snapshot = subscribers_.snapshot();
            for (const auto &entry : snapshot) {
                {
                    const std::scoped_lock lock(state_mutex_);
                    if (token != active_token_ || state_ != TransportState::kOpen) {
                        return;
                    }
                }
                detail::SubscriberRegistry<FrameCallback>::invoke(*entry, message);
            }
        }

        // Rejects a synchronous start/stop/recover call made from within this
        // bus's own dispatcher receive/fault callback thread. Without this
        // guard, stop()/recover() called from that thread would drop the last
        // shared_ptr to the still-running CANDispatcher on its own receive
        // thread, causing std::thread::~thread() to run on a joinable thread
        // that is currently executing -> std::terminate. Checked before
        // operation_mutex_ is acquired so it can never itself deadlock against
        // a concurrent external start()/stop()/recover() holding that lock.
        auto reject_if_receive_thread() const -> void {
            std::shared_ptr<CANDispatcher> dispatcher;
            {
                const std::scoped_lock lock(state_mutex_);
                dispatcher = dispatcher_;
            }
            if (dispatcher && dispatcher->is_receive_thread()) {
                throw ValidationError(
                    "ManagedCANBus start/stop/recover cannot be called synchronously "
                    "from its own receive or fault callback; defer the call to another thread");
            }
        }

        auto on_dispatcher_fault(const std::shared_ptr<void> &token, const CANError &error)
            -> void {
            {
                const std::scoped_lock lock(state_mutex_);
                if (token != active_token_) {
                    return;
                }
                state_ = TransportState::kFaulted;
                last_error_code_ = error.code();
                last_error_ = error.what();
            }
            send_running_.store(false, std::memory_order_release);
            queue_cv_.notify_all();
            fail_queued(std::make_exception_ptr(FaultedError(error.what())));
            if (options_.on_fault) {
                try {
                    options_.on_fault(error);
                } catch (...) {
                }
            }
        }

        auto fail_queued(const std::exception_ptr &error) -> void {
            std::deque<QueuedSend> leftover;
            {
                const std::scoped_lock lock(queue_mutex_);
                leftover.swap(send_queue_);
            }
            for (auto &item : leftover) {
                complete_receipt(*item.receipt, error);
            }
        }

        static auto complete_receipt(SendReceiptState &state, std::exception_ptr error) -> void {
            {
                const std::scoped_lock lock(state.mutex);
                if (state.done) {
                    return;
                }
                state.error = std::move(error);
                state.done = true;
            }
            state.cv.notify_all();
        }

        auto record_error_locked(const std::exception_ptr &error) -> void {
            try {
                if (error) {
                    std::rethrow_exception(error);
                }
            } catch (const CANError &classified) {
                last_error_code_ = classified.code();
                last_error_ = classified.what();
            } catch (const std::exception &classified) {
                last_error_code_ = ErrorCode::kInternal;
                last_error_ = classified.what();
            } catch (...) {
                last_error_code_ = ErrorCode::kInternal;
                last_error_ = "unknown stop failure";
            }
        }

        TransportFactory factory_;
        Options options_;

        mutable std::mutex operation_mutex_;
        mutable std::mutex state_mutex_;
        TransportState state_ = TransportState::kClosed;
        uint64_t generation_ = 0;
        std::optional<ErrorCode> last_error_code_;
        std::string last_error_;
        std::shared_ptr<CANDispatcher> dispatcher_;
        std::shared_ptr<void> active_token_;

        detail::SubscriberRegistry<FrameCallback> subscribers_;

        mutable std::mutex queue_mutex_;
        std::condition_variable queue_cv_;
        std::deque<QueuedSend> send_queue_;
        std::mutex send_thread_mutex_;
        std::thread send_thread_;
        std::atomic<bool> send_running_{false};
    };

} // namespace linkerbot::can
