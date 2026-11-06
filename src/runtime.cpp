#include <linkerbot/transport/runtime.hpp>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace linkerbot::transport {
    namespace {
        bool same_scheduling(const SessionOptions &a, const SessionOptions &b) {
            return std::tie(a.tx_capacity, a.separate_policy_capacity, a.fifo_capacity,
                            a.latest_capacity, a.fifo_unit, a.endpoint_fifo, a.aging_quantum,
                            a.send_period, a.max_consecutive_send_errors, a.domain_round_robin,
                            a.max_route_keys, a.max_pending_rx, a.max_batch_frames) ==
                   std::tie(b.tx_capacity, b.separate_policy_capacity, b.fifo_capacity,
                            b.latest_capacity, b.fifo_unit, b.endpoint_fifo, b.aging_quantum,
                            b.send_period, b.max_consecutive_send_errors, b.domain_round_robin,
                            b.max_route_keys, b.max_pending_rx, b.max_batch_frames);
        }

        struct RecoveryAttempt {
            std::mutex mutex;
            std::condition_variable changed;
            bool completed = false;
            uint64_t generation = 0;
            std::exception_ptr error;
        };
        struct Channel {
            can::CANBusLease lease;
            std::shared_ptr<Session> session;
            ChannelSpec spec;
            mutable std::mutex mutex;
            size_t devices = 0;
            size_t reset_blockers = 0;
            bool closing = false;
            bool closed = false;
            std::exception_ptr close_error;
            std::shared_ptr<RecoveryAttempt> recovery;

            Channel(can::CANBusLease connection, std::shared_ptr<Session> parent,
                    ChannelSpec specification)
                : lease(std::move(connection)), session(std::move(parent)),
                  spec(std::move(specification)) {}

            void acquire(ChannelResetPolicy policy) {
                std::lock_guard<std::mutex> lock(mutex);
                if (closing || closed || (recovery && !recovery->completed)) {
                    throw can::FaultedError("channel release is incomplete or already closed");
                }
                ++devices;
                if (policy == ChannelResetPolicy::kRequireDetach) {
                    ++reset_blockers;
                }
            }
            void release(ChannelResetPolicy policy) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (devices > 1) {
                        --devices;
                        if (policy == ChannelResetPolicy::kRequireDetach) {
                            --reset_blockers;
                        }
                        return;
                    }
                    closing = true;
                }
                try {
                    session->close();
                    lease.close();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    close_error = std::current_exception();
                    throw;
                }
                std::lock_guard<std::mutex> lock(mutex);
                devices = 0;
                reset_blockers = 0;
                closed = true;
                closing = false;
                close_error = {};
            }
        };

        struct LogicalLease {
            std::shared_ptr<Channel> channel;
            bool active = false;
            ChannelResetPolicy policy;
            LogicalLease(std::shared_ptr<Channel> owner, ChannelResetPolicy reset_policy)
                : channel(std::move(owner)), active(true), policy(reset_policy) {
                channel->acquire(policy);
            }
            ~LogicalLease() {
                if (active) {
                    try {
                        release();
                    } catch (...) {
                        // The physical lease destructor retains failed cleanup ownership.
                        std::lock_guard<std::mutex> lock(channel->mutex);
                        channel->close_error = std::current_exception();
                    }
                }
            }
            void release() {
                if (active) {
                    channel->release(policy);
                    active = false;
                }
            }
        };
    } // namespace

    struct TransportRuntime::Impl {
        struct Entry {
            std::weak_ptr<Channel> channel;
            std::condition_variable changed;
            bool opening = false;
            std::thread::id initializer;
            uint64_t attempt = 0;
        };
        struct WaitEdge {
            Entry *entry;
            std::thread::id initializer;
            uint64_t attempt;
        };
        can::SharedCANBusPool pool;
        mutable std::mutex mutex;
        std::map<can::CANBusKey, std::shared_ptr<Entry>> channels;
        std::map<std::thread::id, WaitEdge> waiters;

        std::shared_ptr<Channel> find(const can::CANBusKey &key) const {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = channels.find(key);
            if (it == channels.end()) {
                return {};
            }
            return it->second->channel.lock();
        }
    };
    TransportRuntime::TransportRuntime() {
        static const auto core = std::make_shared<Impl>();
        impl_ = core;
    }
    std::shared_ptr<DeviceSession> TransportRuntime::open_device(ChannelSpec channel,
                                                                 DeviceSpec device,
                                                                 Session::Factory factory) {
        if (channel.configuration_fingerprint.empty() || !factory || device.identity.empty() ||
            device.scheduling.send_period.count() < 0 ||
            (device.channel_reset != ChannelResetPolicy::kAllowCoordinatedReset &&
             device.channel_reset != ChannelResetPolicy::kRequireDetach)) {
            throw can::ValidationError("channel fingerprint and factory must be explicit");
        }
        std::shared_ptr<Channel> context;
        std::shared_ptr<Impl::Entry> entry;
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            auto &slot = impl_->channels[channel.key];
            if (!slot) {
                slot = std::make_shared<Impl::Entry>();
            }
            entry = slot;
            while (entry->opening) {
                const auto caller = std::this_thread::get_id();
                auto owner = entry->initializer;
                for (;;) {
                    if (owner == caller) {
                        throw can::ValidationError("cyclic channel initialization");
                    }
                    const auto waiting = impl_->waiters.find(owner);
                    if (waiting == impl_->waiters.end()) {
                        break;
                    }
                    const auto &edge = waiting->second;
                    if (!edge.entry->opening || edge.entry->attempt != edge.attempt ||
                        edge.entry->initializer != edge.initializer) {
                        break;
                    }
                    owner = edge.initializer;
                }
                if (!impl_->waiters
                         .emplace(caller,
                                  Impl::WaitEdge{entry.get(), entry->initializer, entry->attempt})
                         .second) {
                    throw can::ValidationError("reentrant initialization of the same channel");
                }
                struct ClearWait {
                    Impl *core;
                    std::thread::id thread;
                    ~ClearWait() {
                        core->waiters.erase(thread);
                    }
                } waiting{impl_.get(), caller};
                entry->changed.wait(lock);
            }
            context = entry->channel.lock();
            if (!context) {
                entry->opening = true;
                entry->initializer = std::this_thread::get_id();
                ++entry->attempt;
            }
        }
        if (!context) {
            try {
                auto lease = impl_->pool.acquire(channel.key, std::move(factory));
                auto session = Session::share(lease.shared_bus());
                session->configure(channel.scheduling);
                context = std::make_shared<Channel>(std::move(lease), session, channel);
                std::lock_guard<std::mutex> lock(impl_->mutex);
                entry->channel = context;
                entry->opening = false;
                entry->changed.notify_all();
            } catch (...) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                entry->opening = false;
                entry->changed.notify_all();
                throw;
            }
        }
        if (context->spec.configuration_fingerprint != channel.configuration_fingerprint ||
            !same_scheduling(context->spec.scheduling, channel.scheduling)) {
            throw can::ValidationError("shared channel configurations differ");
        }
        auto lease = std::make_shared<LogicalLease>(context, device.channel_reset);
        std::function<void()> release = [lease] { lease->release(); };
        auto handle = context->session->attach_device(std::move(device.identity),
                                                      std::move(device.scheduling));
        handle->set_channel_release(std::move(release));
        return handle;
    }
    size_t TransportRuntime::active_devices(const can::CANBusKey &key) const {
        const auto context = impl_->find(key);
        if (!context) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(context->mutex);
        return context->devices;
    }
    std::shared_ptr<DeviceSession>
    TransportRuntime::rebind_device(const can::CANBusKey &key,
                                    const std::shared_ptr<DeviceSession> &previous,
                                    DeviceSpec device, Deadline deadline) {
        const auto context = impl_->find(key);
        if (!context || !previous || previous->closed() ||
            previous->health().channel_instance != context->session->health().channel_instance) {
            throw can::ValidationError("rebind requires an active handle on this channel");
        }
        if (device.identity.empty() || device.scheduling.send_period.count() < 0 ||
            (device.channel_reset != ChannelResetPolicy::kAllowCoordinatedReset &&
             device.channel_reset != ChannelResetPolicy::kRequireDetach)) {
            throw can::ValidationError("invalid replacement device profile");
        }
        // Pin the connection before releasing the previous device's logical lease.
        auto lease = std::make_shared<LogicalLease>(context, device.channel_reset);
        std::function<void()> release = [lease] { lease->release(); };
        previous->close_until(deadline);
        auto handle = context->session->attach_device(std::move(device.identity),
                                                      std::move(device.scheduling));
        handle->set_channel_release(std::move(release));
        return handle;
    }
    SessionHealth TransportRuntime::channel_health(const can::CANBusKey &key) const {
        const auto context = impl_->find(key);
        if (!context) {
            throw can::ClosedError("channel has no active owner");
        }
        return context->session->health();
    }
    uint64_t TransportRuntime::recover_channel(const can::CANBusKey &key,
                                               uint64_t observed_generation) {
        const auto context = impl_->find(key);
        if (!context) {
            throw can::ClosedError("channel has no active owner");
        }
        std::shared_ptr<RecoveryAttempt> attempt;
        bool owner = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->closing || context->closed || context->reset_blockers != 0) {
                throw can::ValidationError("channel reset requires all devices to permit it");
            }
            attempt = context->recovery;
            if (!attempt || attempt->completed) {
                attempt = std::make_shared<RecoveryAttempt>();
                context->recovery = attempt;
                owner = true;
            }
        }
        if (owner) {
            std::exception_ptr error;
            uint64_t generation = 0;
            try {
                generation = context->session->recover(observed_generation);
            } catch (...) {
                error = std::current_exception();
            }
            std::lock_guard<std::mutex> lock(context->mutex);
            std::lock_guard<std::mutex> result_lock(attempt->mutex);
            attempt->generation = generation;
            attempt->error = std::move(error);
            attempt->completed = true;
            attempt->changed.notify_all();
        }
        std::unique_lock<std::mutex> lock(attempt->mutex);
        attempt->changed.wait(lock, [&] { return attempt->completed; });
        if (attempt->error) {
            std::rethrow_exception(attempt->error);
        }
        return attempt->generation;
    }
} // namespace linkerbot::transport
