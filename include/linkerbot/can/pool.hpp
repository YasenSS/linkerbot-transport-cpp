#pragma once

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/managed_bus.hpp>

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace linkerbot::can {

    // Identifies one physical CAN/CAN-FD channel. Keys that differ in backend,
    // endpoint, device/channel index, FD mode or bitrate must not share a bus.
    struct CANBusKey {
        std::string backend;
        std::string endpoint;
        unsigned device_index = 0;
        unsigned channel_index = 0;
        bool fd_mode = false;
        uint32_t nom_baud = 0;
        uint32_t dat_baud = 0;

        friend auto operator<(const CANBusKey &lhs, const CANBusKey &rhs) noexcept -> bool {
            return std::tie(lhs.backend, lhs.endpoint, lhs.device_index, lhs.channel_index,
                            lhs.fd_mode, lhs.nom_baud, lhs.dat_baud) <
                   std::tie(rhs.backend, rhs.endpoint, rhs.device_index, rhs.channel_index,
                            rhs.fd_mode, rhs.nom_baud, rhs.dat_baud);
        }

        friend auto operator==(const CANBusKey &lhs, const CANBusKey &rhs) noexcept -> bool {
            return std::tie(lhs.backend, lhs.endpoint, lhs.device_index, lhs.channel_index,
                            lhs.fd_mode, lhs.nom_baud, lhs.dat_baud) ==
                   std::tie(rhs.backend, rhs.endpoint, rhs.device_index, rhs.channel_index,
                            rhs.fd_mode, rhs.nom_baud, rhs.dat_baud);
        }
    };

    static_assert(noexcept(CANBusKey(std::declval<CANBusKey &&>())),
                  "CANBusKey must remain nothrow movable for lease acquisition rollback safety");
    static_assert(noexcept(std::declval<CANBusKey &>() = std::declval<CANBusKey &&>()),
                  "CANBusKey must remain nothrow move assignable for lease ownership safety");

    namespace detail {

        // Identifies one physical channel independent of the logical
        // configuration (FD mode / bitrate) requested for it. Used to reject
        // an attempt to reacquire the same physical wire with conflicting
        // settings while a bus for it is still open or pending a close retry.
        struct PhysicalChannelKey {
            std::string backend;
            std::string endpoint;
            unsigned device_index = 0;
            unsigned channel_index = 0;

            friend auto operator<(const PhysicalChannelKey &lhs,
                                  const PhysicalChannelKey &rhs) noexcept -> bool {
                return std::tie(lhs.backend, lhs.endpoint, lhs.device_index, lhs.channel_index) <
                       std::tie(rhs.backend, rhs.endpoint, rhs.device_index, rhs.channel_index);
            }
        };

        inline auto physical_channel_key(const CANBusKey &key) -> PhysicalChannelKey {
            return PhysicalChannelKey{key.backend, key.endpoint, key.device_index,
                                      key.channel_index};
        }

        // Leases keep this state alive after the pool wrapper is destroyed.
        // The mutex protects maps only; start/stop/recover run outside it.
        // Opening/closing operations coordinate through the condition variable.
        // A failed final close retains its owning lease/count, reserves the
        // physical channel and rejects new acquisition until that lease retries.
        class PoolState {
          public:
            PoolState() = default;
            PoolState(const PoolState &) = delete;
            auto operator=(const PoolState &) -> PoolState & = delete;

            auto acquire(CANBusKey key, ManagedCANBus::TransportFactory factory,
                         ManagedCANBus::Options options) -> std::shared_ptr<ManagedCANBus> {
                if (!factory) {
                    throw ValidationError("factory must be callable");
                }

                std::shared_ptr<ManagedCANBus> bus;
                bool need_start = false;
                bool need_recover = false;
                {
                    std::unique_lock lock(mutex_);
                    for (;;) {
                        auto it = entries_.find(key);
                        if (it != entries_.end()) {
                            if (it->second.close_failed) {
                                throw FaultedError("physical close failed; retry the owning lease's close before acquiring");
                            }
                            if (it->second.opening || it->second.closing) {
                                state_cv_.wait(lock);
                                continue;
                            }
                            if (it->second.leases == 0) {
                                it->second.opening = true;
                                bus = it->second.bus;
                                need_recover = true;
                                break;
                            }
                            ++it->second.leases;
                            return it->second.bus;
                        }

                        const auto physical = physical_channel_key(key);
                        // Keep a second fully-constructed copy for Entry before
                        // publishing either map node. From this point onward,
                        // rollback and close use only stored keys and cannot
                        // allocate while pool state is transitional.
                        auto entry_physical = physical;
                        auto conflict = physical_active_.find(physical);
                        if (conflict != physical_active_.end() && !(conflict->second == key)) {
                            throw ValidationError(
                                "requested CAN bus configuration conflicts with an already "
                                "active configuration for the same physical channel");
                        }

                        bus = std::make_shared<ManagedCANBus>(std::move(factory), std::move(options));
                        auto [physical_it, physical_inserted] =
                            physical_active_.emplace(std::move(physical), key);
                        if (!physical_inserted) {
                            throw InternalError(
                                "physical CAN channel reservation exists without a pool entry");
                        }
                        try {
                            entries_.emplace(
                                key, Entry{bus, std::move(entry_physical), 0, true, false, false});
                        } catch (...) {
                            physical_active_.erase(physical_it);
                            throw;
                        }
                        need_start = true;
                        break;
                    }
                }

                std::exception_ptr op_error;
                try {
                    if (need_start) {
                        bus->start();
                    } else if (need_recover) {
                        bus->recover();
                    }
                } catch (...) {
                    op_error = std::current_exception();
                }

                NotifyOnExit notify{state_cv_};
                {
                    const std::scoped_lock lock(mutex_);
                    auto it = entries_.find(key);
                    if (op_error) {
                        if (need_start) {
                            const auto physical_it = physical_active_.find(it->second.physical);
                            if (physical_it != physical_active_.end()) {
                                physical_active_.erase(physical_it);
                            }
                            entries_.erase(it);
                        } else {
                            it->second.opening = false;
                        }
                    } else {
                        it->second.bus = bus;
                        it->second.opening = false;
                        ++it->second.leases;
                    }
                }
                if (op_error) {
                    std::rethrow_exception(op_error);
                }
                return bus;
            }

            [[nodiscard]] auto lease_count(const CANBusKey &key) const -> int {
                const std::scoped_lock lock(mutex_);
                auto it = entries_.find(key);
                return it == entries_.end() ? 0 : it->second.leases;
            }

            [[nodiscard]] auto active_keys() const -> std::vector<CANBusKey> {
                const std::scoped_lock lock(mutex_);
                std::vector<CANBusKey> keys;
                keys.reserve(entries_.size());
                for (const auto &entry : entries_) {
                    keys.push_back(entry.first);
                }
                return keys;
            }

            auto release(const CANBusKey &key, const std::shared_ptr<ManagedCANBus> &bus) -> void {
                {
                    const std::scoped_lock lock(mutex_);
                    auto it = entries_.find(key);
                    if (it == entries_.end() || it->second.bus != bus) {
                        throw ClosedError("CAN bus lease does not belong to this pool");
                    }
                    if (it->second.leases <= 0) {
                        throw ClosedError("CAN bus lease count is already zero");
                    }
                    if (it->second.leases > 1) {
                        --it->second.leases;
                        return;
                    }
                    it->second.closing = true;
                }
                finish_stop_attempt(key, bus);
            }

            // Retry only the physical stop; retain the last lease until success.
            auto retry_close(const CANBusKey &key, const std::shared_ptr<ManagedCANBus> &bus)
                -> void {
                {
                    const std::scoped_lock lock(mutex_);
                    auto it = entries_.find(key);
                    if (it == entries_.end() || it->second.bus != bus || !it->second.close_failed) {
                        throw ClosedError("CAN bus lease is not pending a close retry");
                    }
                    it->second.close_failed = false;
                    it->second.closing = true;
                }
                finish_stop_attempt(key, bus);
            }

          private:
            auto finish_stop_attempt(const CANBusKey &key, const std::shared_ptr<ManagedCANBus> &bus)
                -> void {
                NotifyOnExit notify{state_cv_};
                std::exception_ptr stop_error;
                try {
                    bus->stop();
                } catch (...) {
                    stop_error = std::current_exception();
                }

                {
                    const std::scoped_lock lock(mutex_);
                    auto it = entries_.find(key);
                    if (it != entries_.end() && it->second.bus == bus && stop_error) {
                        it->second.closing = false;
                        it->second.close_failed = true;
                    }
                    if (it != entries_.end() && it->second.bus == bus && !stop_error) {
                        // Only a successful close frees the physical channel;
                        // a failed attempt remains reserved until its owner retries.
                        it->second.closing = false;
                        const auto physical_it = physical_active_.find(it->second.physical);
                        if (physical_it != physical_active_.end()) {
                            physical_active_.erase(physical_it);
                        }
                        entries_.erase(it);
                    }
                }
                if (stop_error) {
                    std::rethrow_exception(stop_error);
                }
            }

            struct Entry {
                std::shared_ptr<ManagedCANBus> bus;
                PhysicalChannelKey physical;
                int leases = 0;
                bool opening = false;
                bool closing = false;
                bool close_failed = false;
            };

            struct NotifyOnExit {
                std::condition_variable &cv;
                ~NotifyOnExit() {
                    cv.notify_all();
                }
            };

            mutable std::mutex mutex_;
            std::condition_variable state_cv_;
            std::map<CANBusKey, Entry> entries_;
            std::map<PhysicalChannelKey, CANBusKey> physical_active_;
        };

        struct DeferredLeaseCloseTask {
            DeferredLeaseCloseTask *next = nullptr;
            std::shared_ptr<PoolState> state;
            CANBusKey key;
            std::shared_ptr<ManagedCANBus> bus;
            bool retry = false;
        };

        // Process-local, joinable cleanup worker. Tasks are intrusive and are
        // allocated when a lease is created, so releasing the final lease from
        // a receive callback only moves noexcept ownership fields and links a
        // node under this mutex. A failed backend close remains in failed_head_
        // as the sole retry owner until retry_failed() is explicitly called.
        class DeferredLeaseCloseCleanup {
          public:
            DeferredLeaseCloseCleanup() : worker_([this] { run(); }) {}

            ~DeferredLeaseCloseCleanup() {
                {
                    const std::scoped_lock lock(mutex_);
                    stopping_ = true;
                    cv_.notify_all();
                }
                worker_.join();
                destroy_list(failed_head_);
            }

            DeferredLeaseCloseCleanup(const DeferredLeaseCloseCleanup &) = delete;
            auto
            operator=(const DeferredLeaseCloseCleanup &) -> DeferredLeaseCloseCleanup & = delete;

            static auto instance() -> DeferredLeaseCloseCleanup & {
                static DeferredLeaseCloseCleanup cleanup;
                return cleanup;
            }

            auto enqueue(DeferredLeaseCloseTask *task) noexcept -> void {
                const std::scoped_lock lock(mutex_);
                task->next = nullptr;
                if (tail_) {
                    tail_->next = task;
                } else {
                    head_ = task;
                }
                tail_ = task;
                ++queued_;
                cv_.notify_all();
            }

            auto retry_failed() noexcept -> void {
                const std::scoped_lock lock(mutex_);
                if (!failed_head_) {
                    return;
                }
                if (tail_) {
                    tail_->next = failed_head_;
                } else {
                    head_ = failed_head_;
                }
                tail_ = failed_tail_;
                queued_ += failed_;
                failed_head_ = nullptr;
                failed_tail_ = nullptr;
                failed_ = 0;
                cv_.notify_all();
            }

            auto drain() -> void {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return queued_ == 0 && !active_; });
                if (failed_ != 0) {
                    throw FaultedError("deferred CAN bus close failed; call "
                                       "CANBusLease::retry_deferred_cleanup() and drain again");
                }
            }

            [[nodiscard]] auto pending() const noexcept -> std::size_t {
                const std::scoped_lock lock(mutex_);
                return queued_ + failed_ + (active_ ? 1U : 0U);
            }

          private:
            static auto destroy_list(DeferredLeaseCloseTask *task) noexcept -> void {
                while (task) {
                    auto *next = task->next;
                    delete task;
                    task = next;
                }
            }

            auto run() noexcept -> void {
                for (;;) {
                    DeferredLeaseCloseTask *task = nullptr;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [&] { return stopping_ || head_; });
                        if (!head_) {
                            return;
                        }
                        task = head_;
                        head_ = head_->next;
                        if (!head_) {
                            tail_ = nullptr;
                        }
                        task->next = nullptr;
                        --queued_;
                        active_ = true;
                    }

                    bool failed = false;
                    try {
                        if (task->retry) {
                            task->state->retry_close(task->key, task->bus);
                        } else {
                            task->state->release(task->key, task->bus);
                        }
                    } catch (...) {
                        task->retry = true;
                        failed = true;
                    }

                    if (!failed) {
                        delete task;
                        task = nullptr;
                    }
                    {
                        const std::scoped_lock lock(mutex_);
                        active_ = false;
                        if (failed) {
                            if (failed_tail_) {
                                failed_tail_->next = task;
                            } else {
                                failed_head_ = task;
                            }
                            failed_tail_ = task;
                            ++failed_;
                        }
                        cv_.notify_all();
                    }
                }
            }

            mutable std::mutex mutex_;
            std::condition_variable cv_;
            DeferredLeaseCloseTask *head_ = nullptr;
            DeferredLeaseCloseTask *tail_ = nullptr;
            DeferredLeaseCloseTask *failed_head_ = nullptr;
            DeferredLeaseCloseTask *failed_tail_ = nullptr;
            std::size_t queued_ = 0;
            std::size_t failed_ = 0;
            bool active_ = false;
            bool stopping_ = false;
            std::thread worker_;
        };

    } // namespace detail

    // Explicit ownership token for one shared ManagedCANBus reference. Holds
    // a shared_ptr to the pool's control block rather than a raw pool
    // pointer, so closing a lease is safe even if the SharedCANBusPool
    // wrapper object that created it has since been destroyed.
    //
    // close() retains bus_/state_ (rather than moving them away) until the
    // underlying stop actually succeeds, so a failed close leaves the lease
    // fully able to retry: a second close() call re-attempts the physical
    // stop (via PoolState::retry_close(), which does not re-decrement the
    // pool's lease count) instead of silently no-op'ing. close_call_mutex_
    // serializes concurrent close() calls on the same lease so at most one
    // release()/retry_close() call is ever in flight for it at a time
    // (no double decrement, no duplicate stop); it is a separate mutex from
    // mutex_ (which only guards the plain data fields) so it is never held
    // across the potentially-blocking pool call while a quick, unrelated
    // bus() access is pending.
    class CANBusLease {
      public:
        CANBusLease(const CANBusLease &) = delete;
        auto operator=(const CANBusLease &) -> CANBusLease & = delete;

        CANBusLease(CANBusLease &&other) noexcept
            : state_(std::move(other.state_)), key_(std::move(other.key_)),
              bus_(std::move(other.bus_)), close_failed_(other.close_failed_),
              deferred_task_(std::move(other.deferred_task_)) {
            other.close_failed_ = false;
        }

        // Replacing an open lease can require physically closing its bus, and
        // that operation can fail. Keep move construction noexcept for
        // containers, but let move assignment report the close failure before
        // either lease is modified. This preserves the target as the explicit
        // retry owner instead of silently orphaning a faulted pool entry.
        auto operator=(CANBusLease &&other) -> CANBusLease & {
            if (this != &other) {
                close();
                const std::scoped_lock lock(mutex_, other.mutex_);
                state_ = std::move(other.state_);
                key_ = std::move(other.key_);
                bus_ = std::move(other.bus_);
                close_failed_ = other.close_failed_;
                other.close_failed_ = false;
                if (!deferred_task_) {
                    deferred_task_ = std::move(other.deferred_task_);
                }
            }
            return *this;
        }

        ~CANBusLease() {
            if (CANDispatcher::is_any_receive_thread()) {
                close_deferred_silently();
            } else {
                close_silently();
            }
        }

        // Waits until all currently queued close operations finish. A failed
        // close is retained and reported instead of being erased; call
        // retry_deferred_cleanup() after the backend is ready, then drain
        // again. These process-wide controls must be called externally, never
        // from a CAN receive/fault callback.
        static auto drain_deferred_cleanup() -> void {
            if (CANDispatcher::is_any_receive_thread()) {
                throw ValidationError("deferred CAN bus cleanup drain requires an external caller");
            }
            detail::DeferredLeaseCloseCleanup::instance().drain();
        }

        static auto retry_deferred_cleanup() -> void {
            if (CANDispatcher::is_any_receive_thread()) {
                throw ValidationError("deferred CAN bus cleanup retry requires an external caller");
            }
            detail::DeferredLeaseCloseCleanup::instance().retry_failed();
        }

        [[nodiscard]] static auto pending_deferred_cleanup() -> std::size_t {
            return detail::DeferredLeaseCloseCleanup::instance().pending();
        }

        [[nodiscard]] auto bus() const -> ManagedCANBus & {
            const std::scoped_lock lock(mutex_);
            if (!bus_) {
                throw ClosedError("CAN bus lease is closed");
            }
            return *bus_;
        }

        // The bus object may outlive the lease; releasing the lease still
        // closes the physical connection when it is the pool's last owner.
        [[nodiscard]] auto shared_bus() const -> std::shared_ptr<ManagedCANBus> {
            const std::scoped_lock lock(mutex_);
            if (!bus_) throw ClosedError("CAN bus lease is closed");
            return bus_;
        }

        auto close() -> void {
            // Checked before ever touching close_call_mutex_ or the pool's
            // own lock/condition_variable, so a dispatcher callback thread
            // fails fast instead of risking a deadlock against the very
            // receive-thread join a close() would otherwise perform.
            if (CANDispatcher::is_any_receive_thread()) {
                throw ValidationError(
                    "CAN bus lease cannot be closed synchronously from a CAN "
                    "dispatcher's own receive or fault callback thread; defer "
                    "close() to another thread");
            }

            const std::scoped_lock close_lock(close_call_mutex_);

            std::shared_ptr<ManagedCANBus> bus;
            std::shared_ptr<detail::PoolState> state;
            CANBusKey key;
            bool retry;
            {
                const std::scoped_lock lock(mutex_);
                if (!bus_) {
                    return; // already closed by an earlier successful attempt.
                }
                bus = bus_;
                state = state_;
                key = key_;
                retry = close_failed_;
            }

            try {
                if (retry) {
                    state->retry_close(key, bus);
                } else {
                    state->release(key, bus);
                }
            } catch (...) {
                const std::scoped_lock lock(mutex_);
                close_failed_ = true;
                throw;
            }

            const std::scoped_lock lock(mutex_);
            bus_.reset();
            state_.reset();
            close_failed_ = false;
        }

      private:
        friend class SharedCANBusPool;

        CANBusLease(std::shared_ptr<detail::PoolState> state, CANBusKey key,
                    std::shared_ptr<ManagedCANBus> bus,
                    std::unique_ptr<detail::DeferredLeaseCloseTask> deferred_task) noexcept
            : state_(std::move(state)), key_(std::move(key)), bus_(std::move(bus)),
              deferred_task_(std::move(deferred_task)) {}

        auto close_silently() -> void {
            try {
                close();
            } catch (...) { // NOLINT(bugprone-empty-catch) destructor must not throw
                close_deferred_silently();
            }
        }

        // A receive callback cannot synchronously stop and join its own
        // dispatcher. Transfer the complete ownership token to an external
        // thread instead; that thread performs the same PoolState transition
        // as close(), so the lease count and physical-channel reservation are
        // not discarded merely to avoid the self-join.
        auto close_deferred_silently() noexcept -> void {
            const std::scoped_lock lock(mutex_);
            if (!bus_) {
                return;
            }
            auto *task = deferred_task_.release();
            // Every active lease owns a task allocated during construction.
            // A missing node would violate the move invariant and make safe
            // callback-time cleanup impossible.
            if (!task) {
                std::terminate();
            }
            task->state = std::move(state_);
            task->key = std::move(key_);
            task->bus = std::move(bus_);
            task->retry = close_failed_;
            close_failed_ = false;
            detail::DeferredLeaseCloseCleanup::instance().enqueue(task);
        }

        mutable std::mutex mutex_;
        std::mutex close_call_mutex_;
        std::shared_ptr<detail::PoolState> state_;
        CANBusKey key_;
        std::shared_ptr<ManagedCANBus> bus_;
        bool close_failed_ = false;
        std::unique_ptr<detail::DeferredLeaseCloseTask> deferred_task_;
    };

    // Share exactly one open ManagedCANBus for each caller-defined channel key.
    // Factories must not open a device; start()/recover() own physical I/O.
    class SharedCANBusPool {
      public:
        SharedCANBusPool() = default;
        ~SharedCANBusPool() = default;
        SharedCANBusPool(const SharedCANBusPool &) = delete;
        auto operator=(const SharedCANBusPool &) -> SharedCANBusPool & = delete;
        SharedCANBusPool(SharedCANBusPool &&) = delete;
        auto operator=(SharedCANBusPool &&) -> SharedCANBusPool & = delete;

        auto acquire(CANBusKey key, ManagedCANBus::TransportFactory factory) -> CANBusLease {
            return acquire(std::move(key), std::move(factory), ManagedCANBus::Options{});
        }

        auto acquire(CANBusKey key, ManagedCANBus::TransportFactory factory,
                     ManagedCANBus::Options options) -> CANBusLease {
            // Checked before ever touching PoolState's lock/condition_variable
            // (see PoolState's class comment): a dispatcher callback thread
            // must fail fast rather than risk deadlocking against its own
            // receive-thread join.
            if (CANDispatcher::is_any_receive_thread()) {
                throw ValidationError(
                    "SharedCANBusPool::acquire cannot be called synchronously from a "
                    "CAN dispatcher's own receive or fault callback thread; defer "
                    "to another thread");
            }
            // Initialize every potentially-throwing cleanup resource before
            // PoolState::acquire increments a lease count. Once acquire
            // succeeds, constructing the returned lease only performs
            // noexcept moves and cannot leave an ownerless pool entry.
            (void)detail::DeferredLeaseCloseCleanup::instance();
            auto deferred_task = std::make_unique<detail::DeferredLeaseCloseTask>();
            // Copy key into the state machine before it can increment a lease.
            // If a string allocation fails, the outer key and pool counts are
            // untouched. A successful acquire returns only a shared_ptr, so
            // the remaining key transfer into CANBusLease is noexcept.
            auto bus = state_->acquire(key, std::move(factory), std::move(options));
            return CANBusLease(state_, std::move(key), std::move(bus), std::move(deferred_task));
        }

        [[nodiscard]] auto lease_count(const CANBusKey &key) const -> int {
            return state_->lease_count(key);
        }

        [[nodiscard]] auto active_keys() const -> std::vector<CANBusKey> {
            return state_->active_keys();
        }

      private:
        std::shared_ptr<detail::PoolState> state_ = std::make_shared<detail::PoolState>();
    };

} // namespace linkerbot::can
