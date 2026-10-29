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

        friend auto operator<(const CANBusKey &lhs, const CANBusKey &rhs) -> bool {
            return std::tie(lhs.backend, lhs.endpoint, lhs.device_index, lhs.channel_index,
                            lhs.fd_mode, lhs.nom_baud, lhs.dat_baud) <
                   std::tie(rhs.backend, rhs.endpoint, rhs.device_index, rhs.channel_index,
                            rhs.fd_mode, rhs.nom_baud, rhs.dat_baud);
        }

        friend auto operator==(const CANBusKey &lhs, const CANBusKey &rhs) -> bool {
            return std::tie(lhs.backend, lhs.endpoint, lhs.device_index, lhs.channel_index,
                            lhs.fd_mode, lhs.nom_baud, lhs.dat_baud) ==
                   std::tie(rhs.backend, rhs.endpoint, rhs.device_index, rhs.channel_index,
                            rhs.fd_mode, rhs.nom_baud, rhs.dat_baud);
        }
    };

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

            friend auto operator<(const PhysicalChannelKey &lhs, const PhysicalChannelKey &rhs)
                -> bool {
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
                         ManagedCANBus::Options options)
                -> std::pair<CANBusKey, std::shared_ptr<ManagedCANBus>> {
                if (!factory) {
                    throw ValidationError("factory must be callable");
                }

                std::shared_ptr<ManagedCANBus> bus;
                bool need_start = false;
                bool need_recover = false;
                CANBusKey actual_key = key;
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
                            return {it->first, it->second.bus};
                        }

                        const auto physical = physical_channel_key(key);
                        auto conflict = physical_active_.find(physical);
                        if (conflict != physical_active_.end() && !(conflict->second == key)) {
                            throw ValidationError(
                                "requested CAN bus configuration conflicts with an already "
                                "active configuration for the same physical channel");
                        }

                        bus = std::make_shared<ManagedCANBus>(std::move(factory), std::move(options));
                        physical_active_.emplace(physical, key);
                        try { entries_.emplace(key, Entry{bus, 0, true, false, false}); }
                        catch (...) { physical_active_.erase(physical); throw; }
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

                {
                    const std::scoped_lock lock(mutex_);
                    auto it = entries_.find(key);
                    if (op_error) {
                        if (need_start) {
                            entries_.erase(it);
                            physical_active_.erase(physical_channel_key(key));
                        } else {
                            it->second.opening = false;
                        }
                    } else {
                        it->second.bus = bus;
                        it->second.opening = false;
                        ++it->second.leases;
                        actual_key = it->first;
                    }
                }
                state_cv_.notify_all();
                if (op_error) {
                    std::rethrow_exception(op_error);
                }
                return {actual_key, bus};
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
                        entries_.erase(it);
                        physical_active_.erase(physical_channel_key(key));
                    }
                }
                state_cv_.notify_all();
                if (stop_error) {
                    std::rethrow_exception(stop_error);
                }
            }

            struct Entry {
                std::shared_ptr<ManagedCANBus> bus;
                int leases = 0;
                bool opening = false;
                bool closing = false;
                bool close_failed = false;
            };

            mutable std::mutex mutex_;
            std::condition_variable state_cv_;
            std::map<CANBusKey, Entry> entries_;
            std::map<PhysicalChannelKey, CANBusKey> physical_active_;
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
              bus_(std::move(other.bus_)), close_failed_(other.close_failed_) {
            other.close_failed_ = false;
        }

        auto operator=(CANBusLease &&other) noexcept -> CANBusLease & {
            if (this != &other) {
                close_silently();
                state_ = std::move(other.state_);
                key_ = std::move(other.key_);
                bus_ = std::move(other.bus_);
                close_failed_ = other.close_failed_;
                other.close_failed_ = false;
            }
            return *this;
        }

        ~CANBusLease() {
            close_silently();
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
                    std::shared_ptr<ManagedCANBus> bus)
            : state_(std::move(state)), key_(std::move(key)), bus_(std::move(bus)) {}

        auto close_silently() -> void {
            try {
                close();
            } catch (...) { // NOLINT(bugprone-empty-catch) destructor must not throw
            }
        }

        mutable std::mutex mutex_;
        std::mutex close_call_mutex_;
        std::shared_ptr<detail::PoolState> state_;
        CANBusKey key_;
        std::shared_ptr<ManagedCANBus> bus_;
        bool close_failed_ = false;
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
            auto [actual_key, bus] =
                state_->acquire(std::move(key), std::move(factory), std::move(options));
            return CANBusLease(state_, std::move(actual_key), std::move(bus));
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
