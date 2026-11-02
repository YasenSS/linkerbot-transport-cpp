#include <linkerbot/transport/session.hpp>
#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <linkerbot/can/pool.hpp>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace linkerbot::transport {
    namespace {
        thread_local const void *current_writer = nullptr;
        thread_local unsigned session_cleanup_depth = 0;
        class SessionCleanupScope {
          public:
            SessionCleanupScope() {
                ++session_cleanup_depth;
            }
            ~SessionCleanupScope() {
                --session_cleanup_depth;
            }
            SessionCleanupScope(const SessionCleanupScope &) = delete;
            SessionCleanupScope &operator=(const SessionCleanupScope &) = delete;
        };
        struct RegistryEntry;
        void release_registry_lifetime(RegistryEntry *entry, uint64_t generation) noexcept;
        struct RegistryLifetime {
            RegistryLifetime() = default;
            ~RegistryLifetime();
            RegistryLifetime(const RegistryLifetime &) = delete;
            RegistryLifetime &operator=(const RegistryLifetime &) = delete;
            RegistryEntry *entry = nullptr;
            uint64_t generation = 0;
        };
    } // namespace
struct RouteInbox::State {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<RoutedMessage> queue;
    RouteHealth health;
    size_t capacity = 512;
    const void *owner = nullptr;
};
RouteInbox::RouteInbox(std::shared_ptr<State> state) : state_(std::move(state)) {}
RouteInbox::~RouteInbox() { close(); }
void RouteInbox::close() noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->health.closed = true;
    state_->queue.clear();
    state_->cv.notify_all();
}
RouteHealth RouteInbox::health() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto h = state_->health;
    h.pending = state_->queue.size();
    return h;
}
std::optional<RoutedMessage> RouteInbox::try_receive() {
    return receive_until(Clock::now());
}
std::optional<RoutedMessage> RouteInbox::receive_until(Deadline deadline) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->cv.wait_until(lock, deadline, [&] { return state_->health.closed || !state_->queue.empty(); });
    if (state_->health.closed) throw can::ClosedError("route generation is closed");
    if (state_->queue.empty()) return std::nullopt;
    auto message = std::move(state_->queue.front());
    state_->queue.pop_front();
    return message;
}

struct SendReceipt::State {
    mutable std::mutex mutex;
    std::condition_variable cv;
    SendResult result;
};
SendReceipt::SendReceipt(std::shared_ptr<State> state) : state_(std::move(state)) {}
SendResult SendReceipt::result() const {
    if (!state_) throw can::ValidationError("empty send receipt");
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->result;
}
bool SendReceipt::completed() const { return result().outcome != SendOutcome::pending; }
SendResult SendReceipt::wait_until(Deadline deadline) const {
    if (!state_) throw can::ValidationError("empty send receipt");
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!state_->cv.wait_until(lock, deadline, [&] { return state_->result.outcome != SendOutcome::pending; }))
        throw can::TimeoutError("send receipt wait expired; a claimed backend write may still complete");
    const auto result = state_->result;
    if (result.error) std::rethrow_exception(result.error);
    return result;
}
void SendReceipt::wait() const { (void)wait_until(Deadline::max()); }

struct Session::Impl {
    // Declared first so it is destroyed last, after writer, subscriptions,
    // leases and owned buses have crossed their cleanup barriers.
    RegistryLifetime registry_lifetime;
    Impl *cleanup_next = nullptr;
    void close();
    struct Job {
        MessageBatch batch;
        SendOptions options;
        CancellationSource abort;
        std::shared_ptr<SendReceipt::State> receipt;
    };
    std::shared_ptr<can::ManagedCANBus> bus;
    std::unique_ptr<can::CANDispatcher> owned_dispatcher;
    can::CANDispatcher *borrowed = nullptr;
    bool owns_bus = false;
    std::vector<std::shared_ptr<can::CANBusLease>> leases;
    can::SubscriptionId subscription = 0;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable cv;
    std::map<RouteKey, std::weak_ptr<RouteInbox::State>> routes;
    std::deque<std::shared_ptr<Job>> jobs;
    std::shared_ptr<Job> claimed;
    std::thread writer;
    bool stopping = false;
    bool recovering = false;
    size_t tx_capacity = 2000;
    uint64_t sequence = 0;
    SessionHealth health;

    static void finish(const std::shared_ptr<Job> &job, SendOutcome outcome,
                       std::exception_ptr error = {}, size_t sent = 0, bool uncertain = false) {
        std::lock_guard<std::mutex> lock(job->receipt->mutex);
        auto &r = job->receipt->result;
        if (r.outcome != SendOutcome::pending) return;
        r.outcome = outcome;
        r.error = std::move(error);
        r.sent_frames = sent;
        r.current_frame_uncertain = uncertain;
        r.completed_at = Clock::now();
        job->receipt->cv.notify_all();
    }
    void invalidate_locked(std::exception_ptr reason) {
        for (auto &[key, weak] : routes) {
            (void)key;
            if (auto route = weak.lock()) {
                std::lock_guard<std::mutex> lock(route->mutex);
                route->health.closed = true;
                route->queue.clear();
                route->cv.notify_all();
            }
        }
        routes.clear();
        for (auto &job : jobs) {
            job->abort.cancel();
            finish(job, SendOutcome::cancelled, reason);
        }
        jobs.clear();
        if (claimed) claimed->abort.cancel();
        cv.notify_all();
    }
    void refresh() {
        const auto h = bus ? bus->get_health() : borrowed->get_health();
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || recovering) return;
        if (h.generation != health.generation || (!h.connected() && health.open)) {
            invalidate_locked(std::make_exception_ptr(can::ClosedError("physical generation ended")));
            health.generation = h.generation;
        }
        health.open = h.connected();
        if (!h.last_error.empty()) health.last_error = h.last_error;
    }
    void receive(const can::Message &message) noexcept {
        try {
            refresh();
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || !health.open) return;
            try {
                can::validate_arbitration_id(message.id, message.is_extended);
                can::validate_frame_flags(message.is_fd, message.is_brs);
                can::validate_payload_length(message.dlc, message.is_fd ? can::LinkKind::kCanFd : can::LinkKind::kClassic);
            } catch (const std::exception &error) {
                ++health.invalid_frames;
                health.last_error = error.what();
                return;
            }
            ++health.rx_frames;
            health.rx_bytes += message.dlc;
            const auto it = routes.find({message.id, message.is_extended});
            auto route = it == routes.end() ? nullptr : it->second.lock();
            if (!route) { ++health.route_misses; return; }
            std::lock_guard<std::mutex> route_lock(route->mutex);
            if (route->health.closed || route->health.generation != health.generation) {
                ++health.route_misses;
                return;
            }
            if (route->queue.size() >= route->capacity) {
                ++route->health.overflow;
                ++health.overflows;
                route->cv.notify_all();
                return;
            }
            route->queue.push_back({message, health.generation, ++sequence, Clock::now()});
            route->health.high_water = std::max(route->health.high_water, route->queue.size());
            route->cv.notify_one();
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(mutex);
            health.last_error = error.what();
            health.open = false;
            invalidate_locked(std::current_exception());
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            health.last_error = "non-standard route failure";
            health.open = false;
            invalidate_locked(std::current_exception());
        }
    }
    void run() noexcept {
        current_writer = this;
        struct ClearWriter { ~ClearWriter() { current_writer = nullptr; } } clear_writer;
        try {
            for (;;) {
                refresh();
                std::shared_ptr<Job> job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait_for(lock, std::chrono::milliseconds(2), [&] { return stopping || !jobs.empty(); });
                    if (stopping && jobs.empty()) break;
                    if (jobs.empty()) continue;
                    // Priority includes bounded aging; equal ranks retain FIFO.
                    const auto now = Clock::now();
                    auto rank = [now](const std::shared_ptr<Job> &j) {
                        return static_cast<long long>(j->options.priority) +
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - j->receipt->result.accepted_at).count() / 10;
                    };
                    auto best = jobs.begin();
                    for (auto it = jobs.begin(); it != jobs.end(); ++it)
                        if (rank(*it) > rank(*best)) best = it;
                    job = *best;
                    jobs.erase(best);
                    if (job->options.cancellation.cancelled() || job->abort.token().cancelled()) {
                        finish(job, SendOutcome::cancelled, std::make_exception_ptr(CancelledError("queued send cancelled")));
                        continue;
                    }
                    if (Clock::now() >= job->options.deadline) {
                        finish(job, SendOutcome::expired, std::make_exception_ptr(can::TimeoutError("queued send expired before backend I/O")));
                        continue;
                    }
                    if (!health.open) {
                        finish(job, SendOutcome::failed, std::make_exception_ptr(can::ClosedError("session is not open")));
                        continue;
                    }
                    claimed = job;
                    std::lock_guard<std::mutex> receipt_lock(job->receipt->mutex);
                    job->receipt->result.claimed_at = Clock::now();
                }
                size_t sent = 0;
                bool uncertain = false;
                try {
                    auto cancelled = [&] { return job->abort.token().cancelled() || job->options.cancellation.cancelled(); };
                    // This primitive holds the same physical boundary used by
                    // legacy synchronous and queued sends across all pacing.
                    if (bus) bus->send_batch_until(job->batch.frames, job->options.deadline,
                        job->batch.gap, job->batch.initial_delay, cancelled, sent, uncertain);
                    else borrowed->send_batch_until(job->batch.frames, job->options.deadline,
                        job->batch.gap, job->batch.initial_delay, cancelled, sent, uncertain);
                    // A normal return means the backend accepted every frame.
                    // A cancel seen only now lost the race, and reporting it
                    // would deny a transmission known to have reached the bus.
                    // Cancellation that arrives while frames remain surfaces
                    // from send_batch_until with the accepted count instead.
                    finish(job, SendOutcome::dispatched, {}, sent, uncertain);
                } catch (const CancelledError &) {
                    finish(job, SendOutcome::cancelled, std::current_exception(), sent, uncertain);
                } catch (const can::TimeoutError &) {
                    finish(job, SendOutcome::expired, std::current_exception(), sent, uncertain);
                } catch (...) {
                    finish(job, SendOutcome::failed, std::current_exception(), sent, uncertain);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    health.tx_frames += sent;
                    for (size_t i = 0; i < sent; ++i) health.tx_bytes += job->batch.frames[i].dlc;
                    auto r = SendReceipt(job->receipt).result();
                    if (r.error) {
                        try { std::rethrow_exception(r.error); }
                        catch (const std::exception &error) { health.last_error = error.what(); }
                        catch (...) { health.last_error = "non-standard send failure"; }
                    }
                    claimed.reset();
                    cv.notify_all();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            health.open = false;
            health.last_error = "send worker failed";
            if (claimed) finish(claimed, SendOutcome::failed, std::current_exception(), 0, true);
            invalidate_locked(std::current_exception());
        }
    }
};

namespace {
std::mutex registry_mutex;
struct RegistryEntry {
    std::weak_ptr<Session> session;
    std::condition_variable changed;
    bool initializing = false;
    std::thread::id initializer;
    bool active = false;
    uint64_t generation = 0;
};
using SessionRegistry = std::map<const void *, RegistryEntry>;
SessionRegistry bus_sessions;
SessionRegistry borrowed_sessions;
// A thread can initialize several independent backends recursively, but can
// wait for at most one other initializer at a time. Guard this wait-for graph
// with registry_mutex and reject an edge that would close a cycle.
std::map<std::thread::id, std::thread::id> registry_waits;

bool wait_would_cycle(std::thread::id waiter, std::thread::id initializer) {
    auto current = initializer;
    for (size_t steps = 0; steps <= registry_waits.size(); ++steps) {
        if (current == waiter)
            return true;
        const auto next = registry_waits.find(current);
        if (next == registry_waits.end())
            return false;
        current = next->second;
    }
    // The existing graph should always be acyclic. Refuse to extend it if a
    // stale or corrupted cycle is ever observed rather than waiting forever.
    return true;
}

class RegistryWait {
  public:
    RegistryWait(std::thread::id waiter, std::thread::id initializer) : waiter_(waiter) {
        const auto inserted = registry_waits.emplace(waiter, initializer).second;
        if (!inserted)
            throw can::ValidationError("Session initializer already waits for another backend");
    }
    ~RegistryWait() {
        registry_waits.erase(waiter_);
    }
    RegistryWait(const RegistryWait &) = delete;
    RegistryWait &operator=(const RegistryWait &) = delete;

  private:
    std::thread::id waiter_;
};

struct RegistryReservation {
    RegistryEntry *entry = nullptr;
    std::shared_ptr<Session> previous;
};

RegistryReservation reserve_session(SessionRegistry &registry, const void *key) {
    // Backend close/destruction is application-controlled code. Re-entering a
    // Session registry from it can wait for the very tombstone whose cleanup
    // is executing on this thread, or form a cross-key cleanup cycle.
    if (session_cleanup_depth != 0)
        throw can::ValidationError("Session registry cannot be entered during Session cleanup");
    std::unique_lock<std::mutex> lock(registry_mutex);
    auto &entry = registry[key];
    for (;;) {
        if (auto previous = entry.session.lock())
            return {&entry, std::move(previous)};
        if (entry.active) {
            // Final release from a Session callback is cleaned on an external
            // worker. Waiting here from that same callback/writer would make
            // cleanup wait for the caller that is waiting for cleanup.
            if (current_writer || can::CANDispatcher::is_any_receive_thread())
                throw can::ValidationError(
                    "Session backend cleanup is still active on this callback thread");
            // Every active-lifetime transition notifies this per-entry CV
            // while holding registry_mutex.  Consume the event rather than
            // waiting for a particular sampled value: another caller may
            // already have published the next active generation before this
            // waiter reacquires registry_mutex.
            entry.changed.wait(lock);
            continue;
        }
        if (!entry.initializing) {
            entry.initializing = true;
            entry.initializer = std::this_thread::get_id();
            return {&entry, {}};
        }
        if (entry.initializer == std::this_thread::get_id())
            throw can::ValidationError("recursive Session initialization for the same backend");
        const auto waiter = std::this_thread::get_id();
        const auto initializer = entry.initializer;
        if (wait_would_cycle(waiter, initializer))
            throw can::ValidationError("cyclic Session backend initialization");
        RegistryWait waiting(waiter, initializer);
        // A completed attempt is the event this wait consumes.  If a new
        // attempt overtakes us before mutex reacquisition, leave this scope to
        // remove the old wait-for edge, then bind to the new initializer on
        // the next loop iteration.  Spurious wakes are safe for the same
        // reason.
        entry.changed.wait(lock);
    }
}

void publish_session(RegistryReservation &reservation, const std::shared_ptr<Session> &session,
                     RegistryLifetime &lifetime) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    ++reservation.entry->generation;
    if (reservation.entry->generation == 0)
        ++reservation.entry->generation;
    lifetime.entry = reservation.entry;
    lifetime.generation = reservation.entry->generation;
    reservation.entry->session = session;
    reservation.entry->active = true;
    reservation.entry->initializing = false;
    reservation.entry->initializer = {};
    reservation.entry->changed.notify_all();
}

void abandon_session(RegistryReservation &reservation) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    reservation.entry->initializing = false;
    reservation.entry->initializer = {};
    reservation.entry->changed.notify_all();
}

void release_registry_lifetime(RegistryEntry *entry, uint64_t generation) noexcept {
    if (!entry)
        return;
    const std::lock_guard<std::mutex> lock(registry_mutex);
    if (!entry->active || entry->generation != generation)
        return;
    entry->session.reset();
    entry->active = false;
    entry->changed.notify_all();
}

RegistryLifetime::~RegistryLifetime() {
    release_registry_lifetime(entry, generation);
}
}
// No detached worker and no allocation on final callback release. The queue
// owns Impl until close has crossed the subscription and writer barriers.
class Session::Cleanup {
public:
    Cleanup() : worker_([this] { run(); }) {}
    ~Cleanup() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; cv_.notify_all(); }
        worker_.join();
    }
    static Cleanup &instance() { static Cleanup cleanup; return cleanup; }
    void enqueue(Impl *impl) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tail_) tail_->cleanup_next = impl;
        else head_ = impl;
        tail_ = impl;
        cv_.notify_all();
    }
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !head_ && !active_; });
    }
private:
    void run() {
        for (;;) {
            Impl *impl;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || head_; });
                if (!head_) return;
                impl = head_;
                head_ = head_->cleanup_next;
                if (!head_) tail_ = nullptr;
                active_ = true;
            }
            {
                SessionCleanupScope cleanup_scope;
                try {
                    impl->close();
                } catch (...) { /* backend retains its retry ownership */
                }
                delete impl;
            }
            { std::lock_guard<std::mutex> lock(mutex_); active_ = false; cv_.notify_all(); }
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    Impl *head_ = nullptr;
    Impl *tail_ = nullptr;
    bool active_ = false;
    bool stopping_ = false;
    std::thread worker_;
};
void Session::drain_deferred_cleanup() {
    if (current_writer || can::CANDispatcher::is_any_receive_thread())
        throw can::ValidationError("session cleanup drain requires an external caller");
    Cleanup::instance().drain();
}
Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    // Construct the cleanup executor while failures can still unwind safely,
    // before this Session starts its writer or can be released by a callback.
    (void)Cleanup::instance();
}
void Session::start() {
    auto &p = *impl_;
    p.refresh();
    if (!p.health.open) throw can::ClosedError("session backend is not open");
    p.subscription = p.bus ? p.bus->subscribe([&p](const can::Message &m) { p.receive(m); })
                           : p.borrowed->subscribe([&p](const can::Message &m) { p.receive(m); });
    try { p.writer = std::thread([&p] { p.run(); }); }
    catch (...) {
        if (p.bus) p.bus->unsubscribe(p.subscription);
        else p.borrowed->unsubscribe(p.subscription);
        throw;
    }
}
std::shared_ptr<Session> Session::create(Factory factory, size_t capacity) {
    if (!capacity) throw can::ValidationError("send capacity must be positive");
    auto p = std::make_unique<Impl>();
    p->bus = std::make_shared<can::ManagedCANBus>(std::move(factory));
    p->owns_bus = true;
    p->tx_capacity = capacity;
    p->bus->start();
    auto result = std::shared_ptr<Session>(new Session(std::move(p)));
    result->start();
    return result;
}
std::shared_ptr<Session> Session::own(std::unique_ptr<can::CANTransport> transport) {
    // Keep the transport object alive until the session is destroyed, matching
    // the legacy owned-dispatcher constructors. A one-shot transport has no
    // factory with which to recreate its physical connection.
    auto p = std::make_unique<Impl>();
    p->owned_dispatcher = std::make_unique<can::CANDispatcher>(std::move(transport));
    p->borrowed = p->owned_dispatcher.get();
    auto result = std::shared_ptr<Session>(new Session(std::move(p)));
    result->start();
    return result;
}
std::shared_ptr<Session> Session::share(std::shared_ptr<can::ManagedCANBus> bus) {
    if (!bus) throw can::ValidationError("session bus must not be null");
    auto reservation = reserve_session(bus_sessions, bus.get());
    if (reservation.previous) {
        std::lock_guard<std::mutex> operation(reservation.previous->impl_->lifecycle);
        std::lock_guard<std::mutex> state(reservation.previous->impl_->mutex);
        if (reservation.previous->impl_->stopping)
            throw can::ClosedError("physical session is already closed");
        return reservation.previous;
    }
    try {
        auto p = std::make_unique<Impl>();
        p->bus = std::move(bus);
        // Factories and driver startup are application code. Never execute
        // them while holding the process-wide registry mutex.
        p->bus->start();
        auto result = std::shared_ptr<Session>(new Session(std::move(p)));
        result->start();
        publish_session(reservation, result, result->impl_->registry_lifetime);
        return result;
    } catch (...) {
        abandon_session(reservation);
        throw;
    }
}
std::shared_ptr<Session> Session::lease(can::CANBusLease lease) {
    auto holder = std::make_shared<can::CANBusLease>(std::move(lease));
    auto bus = holder->shared_bus();
    auto reservation = reserve_session(bus_sessions, bus.get());
    if (reservation.previous) {
        std::lock_guard<std::mutex> operation(reservation.previous->impl_->lifecycle);
        std::lock_guard<std::mutex> state(reservation.previous->impl_->mutex);
        if (reservation.previous->impl_->stopping)
            throw can::ClosedError("physical session is already closed");
        // A prior share(bus) may have no pool ownership at all. Retain every
        // supplied lease until explicit close or final Session destruction.
        reservation.previous->impl_->leases.push_back(std::move(holder));
        return reservation.previous;
    }
    try {
        auto p = std::make_unique<Impl>();
        p->bus = std::move(bus);
        p->leases.push_back(std::move(holder));
        auto result = std::shared_ptr<Session>(new Session(std::move(p)));
        result->start();
        publish_session(reservation, result, result->impl_->registry_lifetime);
        return result;
    } catch (...) {
        abandon_session(reservation);
        throw;
    }
}
std::shared_ptr<Session> Session::borrow(can::CANDispatcher &dispatcher) {
    auto reservation = reserve_session(borrowed_sessions, &dispatcher);
    if (reservation.previous) {
        std::lock_guard<std::mutex> state(reservation.previous->impl_->mutex);
        if (reservation.previous->impl_->stopping)
            throw can::ClosedError("borrowed session is already closed");
        return reservation.previous;
    }
    try {
        auto p = std::make_unique<Impl>();
        p->borrowed = &dispatcher;
        auto result = std::shared_ptr<Session>(new Session(std::move(p)));
        result->start();
        publish_session(reservation, result, result->impl_->registry_lifetime);
        return result;
    } catch (...) {
        abandon_session(reservation);
        throw;
    }
}
Session::~Session() {
    SessionCleanupScope cleanup_scope;
    if (current_writer || can::CANDispatcher::is_any_receive_thread()) {
        Cleanup::instance().enqueue(impl_.release());
        return;
    }
    try {
        impl_->close();
    } catch (...) {
    }
    // Keep the marker active through member destruction too: a lease or owned
    // backend destructor may retry a virtual close after close() failed.
    impl_.reset();
}
std::shared_ptr<RouteInbox> Session::subscribe_routes(const std::vector<RouteKey> &keys, size_t capacity) {
    if (!capacity) throw can::ValidationError("route capacity must be positive");
    auto state = std::make_shared<RouteInbox::State>();
    state->capacity = capacity;
    state->owner = impl_.get();
    auto inbox = std::shared_ptr<RouteInbox>(new RouteInbox(state));
    update_routes(inbox, keys);
    return inbox;
}
void Session::update_routes(const std::shared_ptr<RouteInbox> &inbox, const std::vector<RouteKey> &keys) {
    if (!inbox || inbox->state_->owner != impl_.get() || keys.empty())
        throw can::ValidationError("route group must belong to this session and contain keys");
    std::set<RouteKey> unique;
    for (const auto &key : keys) {
        can::validate_arbitration_id(key.id, key.extended);
        if (!unique.insert(key).second) throw can::ValidationError("duplicate key in route group");
    }
    auto &p = *impl_;
    p.refresh();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.stopping || !p.health.open) throw can::ClosedError("session is closed");
    std::lock_guard<std::mutex> inbox_lock(inbox->state_->mutex);
    if (inbox->state_->health.closed) throw can::ClosedError("route is closed");
    for (const auto &key : unique) {
        auto it = p.routes.find(key);
        if (it == p.routes.end()) continue;
        if (auto current = it->second.lock(); current && current != inbox->state_) {
            std::lock_guard<std::mutex> existing_lock(current->mutex);
            if (!current->health.closed) throw can::ValidationError("CAN route already registered");
        }
    }
    // Allocate all entries before removing old keys; rollback new nodes on OOM.
    std::vector<RouteKey> inserted;
    inserted.reserve(unique.size()); // rollback bookkeeping must not allocate after insertion
    try {
        for (const auto &key : unique) {
            auto [it, added] = p.routes.emplace(key, inbox->state_);
            if (added) inserted.push_back(key);
        }
    } catch (...) {
        for (const auto &key : inserted) p.routes.erase(key);
        throw;
    }
    for (const auto &key : unique) p.routes[key] = inbox->state_;
    for (auto it = p.routes.begin(); it != p.routes.end();) {
        if (it->second.lock() == inbox->state_ && !unique.count(it->first)) it = p.routes.erase(it);
        else ++it;
    }
    inbox->state_->health.generation = p.health.generation;
}
SendReceipt Session::submit(MessageBatch batch, SendOptions options) {
    if (batch.frames.empty()) throw can::ValidationError("empty send batch");
    if (batch.gap.count() < 0 || batch.initial_delay.count() < 0)
        throw can::ValidationError("batch pacing must not be negative");
    for (const auto &m : batch.frames) {
        can::validate_arbitration_id(m.id, m.is_extended);
        can::validate_frame_flags(m.is_fd, m.is_brs);
        // TX retains the legacy untagged-on-FD-link compatibility rule.
        can::validate_payload_length(m.dlc, can::LinkKind::kCanFd);
    }
    if (options.policy == SendPolicy::latest && (options.endpoint.empty() || options.family.empty()))
        throw can::ValidationError("latest requires explicit endpoint and family");
    auto job = std::make_shared<Impl::Job>();
    job->batch = std::move(batch);
    job->options = std::move(options);
    job->receipt = std::make_shared<SendReceipt::State>();
    auto &p = *impl_;
    p.refresh();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.stopping || !p.health.open) throw can::ClosedError("session is closed");
    job->options.cancellation.throw_if_cancelled();
    if (Clock::now() >= job->options.deadline) throw can::TimeoutError("send deadline expired");
    if (job->options.policy == SendPolicy::latest) {
        for (auto it = p.jobs.begin(); it != p.jobs.end();) {
            auto &old = *it;
            if (old->options.policy == SendPolicy::latest && old->options.endpoint == job->options.endpoint &&
                old->options.family == job->options.family) {
                Impl::finish(old, SendOutcome::superseded,
                    std::make_exception_ptr(CancelledError("pending latest send superseded")));
                it = p.jobs.erase(it);
            } else ++it;
        }
    }
    if (p.jobs.size() >= p.tx_capacity) throw can::QueueFullError("session send queue is full");
    auto &result = job->receipt->result;
    result.generation = p.health.generation;
    result.accepted_at = Clock::now();
    result.total_frames = job->batch.frames.size();
    p.jobs.push_back(job);
    p.health.queue_high_water = std::max(p.health.queue_high_water, p.jobs.size());
    p.cv.notify_one();
    return SendReceipt(job->receipt);
}
void Session::send(MessageBatch batch, SendOptions options) {
    const auto deadline = options.deadline;
    (void)submit(std::move(batch), std::move(options)).wait_until(deadline);
}
void Session::cancel_endpoint(const std::string &endpoint) {
    auto &p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    for (auto it = p.jobs.begin(); it != p.jobs.end();) {
        if ((*it)->options.endpoint == endpoint) {
            (*it)->abort.cancel();
            Impl::finish(*it, SendOutcome::cancelled, std::make_exception_ptr(CancelledError("endpoint cancelled")));
            it = p.jobs.erase(it);
        } else ++it;
    }
    if (p.claimed && p.claimed->options.endpoint == endpoint) p.claimed->abort.cancel();
    p.cv.notify_all();
}
SessionHealth Session::health() const {
    impl_->refresh();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto h = impl_->health;
    h.pending_tx = impl_->jobs.size() + (impl_->claimed ? 1 : 0);
    return h;
}
can::TransportHealth Session::transport_health() const {
    auto h = impl_->bus ? impl_->bus->get_health() : impl_->borrowed->get_health();
    const auto session = health();
    if (!session.open && h.connected()) h.state = can::TransportState::kClosed;
    h.pending_tx += session.pending_tx;
    return h;
}
void Session::close() {
    SessionCleanupScope cleanup_scope;
    impl_->close();
}
void Session::Impl::close() {
    auto &p = *this;
    if (current_writer || can::CANDispatcher::is_any_receive_thread())
        throw can::ValidationError("session lifecycle requires an external caller");
    std::lock_guard<std::mutex> lifecycle(p.lifecycle);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.stopping = true;
        p.health.open = false;
        p.invalidate_locked(std::make_exception_ptr(can::ClosedError("session closed")));
    }
    if (p.subscription) {
        if (p.bus) p.bus->unsubscribe(p.subscription);
        else p.borrowed->unsubscribe(p.subscription);
        p.subscription = 0;
    }
    if (p.writer.joinable()) p.writer.join();
    if (p.owns_bus) p.bus->stop(); // explicit failure remains retryable
    if (p.owned_dispatcher) p.owned_dispatcher->stop();
    std::exception_ptr release_error;
    for (auto &lease : p.leases) {
        try { lease->close(); }
        catch (...) { if (!release_error) release_error = std::current_exception(); }
    }
    if (release_error) std::rethrow_exception(release_error); // failed lease remains retryable
    p.leases.clear();
}
uint64_t Session::recover(uint64_t observed_generation) {
    auto &p = *impl_;
    if (current_writer || can::CANDispatcher::is_any_receive_thread())
        throw can::ValidationError("session lifecycle requires an external caller");
    if (!p.bus) throw can::ValidationError("physical recovery requires a transport factory or managed bus");
    std::lock_guard<std::mutex> lifecycle(p.lifecycle);
    {
        std::unique_lock<std::mutex> lock(p.mutex);
        if (p.stopping) throw can::ClosedError("closed session cannot recover");
        if (p.health.generation != observed_generation && p.health.open) return p.health.generation;
        p.recovering = true;
        p.health.open = false;
        p.invalidate_locked(std::make_exception_ptr(can::ClosedError("physical recover invalidated generation")));
        p.cv.wait(lock, [&] { return !p.claimed; });
    }
    try {
        p.bus->recover();
        const auto health = p.bus->get_health();
        std::lock_guard<std::mutex> lock(p.mutex);
        p.health.generation = health.generation;
        p.health.open = health.connected();
        p.recovering = false;
        ++p.health.recoveries;
        return p.health.generation;
    } catch (...) {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.recovering = false;
        p.health.open = false;
        throw;
    }
}
} // namespace linkerbot::transport
