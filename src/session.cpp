#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <linkerbot/can/pool.hpp>
#include <linkerbot/transport/session.hpp>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace linkerbot::transport {
    namespace {
        auto current_writer() -> const void *& {
            static thread_local const void *writer = nullptr;
            return writer;
        }
        auto session_cleanup_depth() -> unsigned & {
            static thread_local unsigned depth = 0;
            return depth;
        }
        class SessionCleanupScope {
          public:
            SessionCleanupScope() {
                ++session_cleanup_depth();
            }
            ~SessionCleanupScope() {
                --session_cleanup_depth();
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
    struct ChangeSignal::State {
        mutable std::mutex mutex;
        mutable std::condition_variable cv;
        uint64_t revision = 0;
    };
    ChangeSignal::ChangeSignal() : state_(std::make_unique<State>()) {}
    ChangeSignal::~ChangeSignal() = default;
    uint64_t ChangeSignal::revision() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->revision;
    }
    bool ChangeSignal::wait_until(uint64_t observed, Deadline deadline) const {
        std::unique_lock<std::mutex> lock(state_->mutex);
        const auto changed = [&] { return state_->revision != observed; };
        if (deadline == Deadline::max()) {
            state_->cv.wait(lock, changed);
            return true;
        }
        return state_->cv.wait_until(lock, deadline, changed);
    }
    void ChangeSignal::notify() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->revision;
        state_->cv.notify_all();
    }
    struct RouteInbox::State {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::deque<RoutedMessage> queue;
        RouteHealth health;
        size_t capacity = 512;
        const void *owner = nullptr;
        std::shared_ptr<ChangeSignal> signal;
    };
    RouteInbox::RouteInbox(std::shared_ptr<State> state) : state_(std::move(state)) {}
    RouteInbox::~RouteInbox() {
        close();
    }
    void RouteInbox::close() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->health.closed = true;
        state_->health.dropped_on_close += state_->queue.size();
        state_->queue.clear();
        state_->cv.notify_all();
        state_->signal->notify();
    }
    RouteHealth RouteInbox::health() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto h = state_->health;
        h.pending = state_->queue.size();
        return h;
    }
    std::shared_ptr<ChangeSignal> RouteInbox::notification() const {
        return state_->signal;
    }
    std::optional<RoutedMessage> RouteInbox::try_receive() {
        return receive_until(Clock::now());
    }
    std::optional<RoutedMessage> RouteInbox::receive_until(Deadline deadline) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait_until(lock, deadline,
                              [&] { return state_->health.closed || !state_->queue.empty(); });
        if (state_->health.closed) {
            throw can::ClosedError("route generation is closed");
        }
        if (state_->queue.empty()) {
            return std::nullopt;
        }
        auto message = std::move(state_->queue.front());
        state_->queue.pop_front();
        return message;
    }

    struct SendReceipt::State {
        mutable std::mutex mutex;
        std::condition_variable cv;
        SendResult result;
        std::atomic<size_t> progress{0};
        std::shared_ptr<ChangeSignal> signal;
    };
    SendReceipt::SendReceipt(std::shared_ptr<State> state) : state_(std::move(state)) {}
    SendResult SendReceipt::result() const {
        if (!state_) {
            throw can::ValidationError("empty send receipt");
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto result = state_->result;
        if (result.outcome == SendOutcome::pending) {
            result.sent_frames = state_->progress.load(std::memory_order_relaxed);
        }
        return result;
    }
    std::shared_ptr<ChangeSignal> SendReceipt::notification() const {
        if (!state_) {
            throw can::ValidationError("empty send receipt");
        }
        return state_->signal;
    }
    bool SendReceipt::completed() const {
        return result().outcome != SendOutcome::pending;
    }
    SendResult SendReceipt::wait_until(Deadline deadline) const {
        const auto result = wait_result_until(deadline);
        if (result.error) {
            std::rethrow_exception(result.error);
        }
        return result;
    }
    SendResult SendReceipt::wait_result_until(Deadline deadline) const {
        if (!state_) {
            throw can::ValidationError("empty send receipt");
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        const auto finished = [&] { return state_->result.outcome != SendOutcome::pending; };
        // Older libstdc++ implementations convert steady-clock deadlines to
        // system time. Converting max() can overflow and prevent the writer
        // from acquiring this mutex to publish completion.
        if (deadline == Deadline::max()) {
            state_->cv.wait(lock, finished);
        } else if (!state_->cv.wait_until(lock, deadline, finished)) {
            throw can::TimeoutError(
                "send receipt wait expired; a claimed backend write may still complete");
        }
        return state_->result;
    }
    void SendReceipt::wait() const {
        (void)wait_until(Deadline::max());
    }

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
        bool terminal_failure = false;
        bool routing_started = false;
        size_t tx_capacity = 2000;
        SessionOptions options;
        std::set<std::string> stopped_endpoints;
        uint64_t event_order = 0;
        uint64_t lifecycle_revision = 0;
        uint64_t service_order = 0;
        std::map<std::string, uint64_t> last_served;
        uint64_t sequence = 0;
        std::shared_ptr<ChangeSignal> signal = std::make_shared<ChangeSignal>();
        std::function<RouteKey(const can::Message &)> route_key_of;
        SessionHealth health;

        void finish(const std::shared_ptr<Job> &job, SendOutcome outcome,
                    std::exception_ptr error = {}, size_t sent = 0, bool uncertain = false) {
            std::lock_guard<std::mutex> lock(job->receipt->mutex);
            auto &r = job->receipt->result;
            if (r.outcome != SendOutcome::pending) {
                return;
            }
            r.outcome = outcome;
            r.error = std::move(error);
            r.sent_frames = sent;
            r.current_frame_uncertain = uncertain;
            r.completed_at = Clock::now();
            switch (outcome) {
            case SendOutcome::dispatched:
                ++health.dispatched;
                break;
            case SendOutcome::superseded:
                ++health.superseded;
                break;
            case SendOutcome::cancelled:
                ++health.cancelled;
                break;
            case SendOutcome::expired:
                ++health.expired;
                break;
            case SendOutcome::failed:
                ++health.failed;
                break;
            case SendOutcome::pending:
                break;
            }
            job->receipt->cv.notify_all();
            signal->notify();
        }
        size_t fifo_cost(const Job &job) const {
            return options.fifo_unit == CapacityUnit::kFrames ? job.batch.frames.size() : 1;
        }
        std::pair<size_t, size_t> pending_units() const {
            size_t fifo = 0;
            size_t latest = 0;
            for (const auto &job : jobs) {
                if (job->options.policy == SendPolicy::latest) {
                    ++latest;
                } else {
                    fifo += fifo_cost(*job);
                }
            }
            return {fifo, latest};
        }
        Deadline oldest_accepted() const {
            auto oldest = claimed ? claimed->receipt->result.accepted_at : Deadline::max();
            for (const auto &job : jobs) {
                oldest = std::min(oldest, job->receipt->result.accepted_at);
            }
            return oldest;
        }
        void invalidate_locked(std::exception_ptr reason) {
            ++lifecycle_revision;
            for (auto &[key, weak] : routes) {
                (void)key;
                if (auto route = weak.lock()) {
                    std::lock_guard<std::mutex> lock(route->mutex);
                    route->health.closed = true;
                    route->health.dropped_on_close += route->queue.size();
                    route->queue.clear();
                    route->cv.notify_all();
                    route->signal->notify();
                }
            }
            routes.clear();
            for (auto &job : jobs) {
                job->abort.cancel();
                finish(job, SendOutcome::cancelled, reason);
            }
            jobs.clear();
            if (claimed) {
                claimed->abort.cancel();
            }
            cv.notify_all();
            signal->notify();
        }
        void refresh() {
            uint64_t observed_revision = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || recovering || terminal_failure) {
                    return;
                }
                observed_revision = lifecycle_revision;
            }
            // Backend diagnostics are copied outside the Session lock. A recover
            // or fault may invalidate that snapshot before it can be applied,
            // including a failed reopen which retains the same generation.
            const auto h = bus ? bus->get_health() : borrowed->get_health();
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || recovering || terminal_failure ||
                observed_revision != lifecycle_revision) {
                return;
            }
            if (h.generation != health.generation || (!h.connected() && health.open)) {
                invalidate_locked(
                    std::make_exception_ptr(can::ClosedError("physical generation ended")));
                health.generation = h.generation;
            }
            health.open = h.connected();
            if (!h.last_error.empty()) {
                health.last_error = h.last_error;
            }
        }
        void receive(const can::Message &message) noexcept {
            try {
                refresh();
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || !health.open) {
                    return;
                }
                try {
                    can::validate_arbitration_id(message.id, message.is_extended);
                    can::validate_frame_flags(message.is_fd, message.is_brs);
                    can::validate_payload_length(message.dlc, message.is_fd
                                                                  ? can::LinkKind::kCanFd
                                                                  : can::LinkKind::kClassic);
                } catch (const std::exception &error) {
                    ++health.invalid_frames;
                    health.last_error = error.what();
                    return;
                }
                ++health.rx_frames;
                health.rx_bytes += message.dlc;
                RouteKey key{message.id, message.is_extended};
                try {
                    if (route_key_of) {
                        key = route_key_of(message);
                    }
                    can::validate_arbitration_id(key.id, key.extended);
                } catch (...) {
                    ++health.route_key_errors;
                    signal->notify();
                    return;
                }
                const auto it = routes.find(key);
                auto route = it == routes.end() ? nullptr : it->second.lock();
                if (!route) {
                    ++health.route_misses;
                    return;
                }
                std::lock_guard<std::mutex> route_lock(route->mutex);
                if (route->health.closed || route->health.generation != health.generation) {
                    ++health.route_misses;
                    return;
                }
                if (route->queue.size() >= route->capacity) {
                    ++route->health.overflow;
                    ++health.overflows;
                    route->cv.notify_all();
                    route->signal->notify();
                    return;
                }
                route->queue.push_back(
                    {message, health.generation, ++sequence, Clock::now(), key, ++event_order});
                ++route->health.delivered;
                route->health.high_water = std::max(route->health.high_water, route->queue.size());
                route->cv.notify_one();
                route->signal->notify();
            } catch (const std::exception &error) {
                std::lock_guard<std::mutex> lock(mutex);
                health.last_error = error.what();
                health.open = false;
                terminal_failure = true;
                invalidate_locked(std::current_exception());
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                health.last_error = "non-standard route failure";
                health.open = false;
                terminal_failure = true;
                invalidate_locked(std::current_exception());
            }
        }
        void run() noexcept {
            current_writer() = this;
            struct ClearWriter {
                ~ClearWriter() {
                    current_writer() = nullptr;
                }
            } clear_writer;
            try {
                for (;;) {
                    refresh();
                    std::shared_ptr<Job> job;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait_for(lock, std::chrono::milliseconds(2),
                                    [&] { return stopping || !jobs.empty(); });
                        if (stopping && jobs.empty()) {
                            break;
                        }
                        if (jobs.empty()) {
                            continue;
                        }
                        // Priority includes bounded aging; equal ranks retain FIFO.
                        const auto now = Clock::now();
                        auto rank = [this, now](const std::shared_ptr<Job> &j) {
                            return static_cast<long long>(j->options.priority) +
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now - j->receipt->result.accepted_at)
                                           .count() /
                                       options.aging_quantum.count();
                        };
                        auto best = jobs.begin();
                        for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                            if (options.endpoint_fifo &&
                                (*it)->options.policy == SendPolicy::fifo &&
                                std::any_of(jobs.begin(), it, [&](const auto &prior) {
                                    return prior->options.policy == SendPolicy::fifo &&
                                           prior->options.endpoint == (*it)->options.endpoint;
                                })) {
                                continue;
                            }
                            const auto served = [&](const auto &candidate) {
                                return last_served.at(candidate->options.endpoint);
                            };
                            const bool earlier =
                                (*it)->options.deadline < (*best)->options.deadline;
                            const bool fairer =
                                (*it)->options.deadline == (*best)->options.deadline &&
                                served(*it) < served(*best);
                            if (rank(*it) > rank(*best) ||
                                (options.endpoint_fifo && rank(*it) == rank(*best) &&
                                 (earlier || fairer))) {
                                best = it;
                            }
                        }
                        job = *best;
                        jobs.erase(best);
                        if (job->options.cancellation.cancelled() ||
                            job->abort.token().cancelled()) {
                            finish(
                                job, SendOutcome::cancelled,
                                std::make_exception_ptr(CancelledError("queued send cancelled")));
                            continue;
                        }
                        if (Clock::now() >= job->options.deadline) {
                            finish(job, SendOutcome::expired,
                                   std::make_exception_ptr(can::TimeoutError(
                                       "queued send expired before backend I/O")));
                            continue;
                        }
                        if (!health.open) {
                            finish(
                                job, SendOutcome::failed,
                                std::make_exception_ptr(can::ClosedError("session is not open")));
                            continue;
                        }
                        claimed = job;
                        last_served.at(job->options.endpoint) = ++service_order;
                        std::lock_guard<std::mutex> receipt_lock(job->receipt->mutex);
                        job->receipt->result.claimed_at = Clock::now();
                        job->receipt->result.claimed_order = ++event_order;
                        signal->notify();
                    }
                    size_t sent = 0;
                    bool uncertain = false;
                    auto outcome = SendOutcome::dispatched;
                    std::exception_ptr error;
                    try {
                        auto cancelled = [&] {
                            return job->abort.token().cancelled() ||
                                   job->options.cancellation.cancelled();
                        };
                        // This primitive holds the same physical boundary used by
                        // legacy synchronous and queued sends across all pacing.
                        if (bus) {
                            bus->send_batch_until(
                                job->batch.frames, job->options.deadline, job->batch.gap,
                                job->batch.initial_delay, cancelled, sent, uncertain,
                                job->options.deadline_policy == DeadlinePolicy::kStartOnly,
                                options.send_period, &job->receipt->progress);
                        } else {
                            borrowed->send_batch_until(
                                job->batch.frames, job->options.deadline, job->batch.gap,
                                job->batch.initial_delay, cancelled, sent, uncertain,
                                job->options.deadline_policy == DeadlinePolicy::kStartOnly,
                                options.send_period, &job->receipt->progress);
                        }
                        // A normal return means the backend accepted every frame.
                        // A cancel seen only now lost the race, and reporting it
                        // would deny a transmission known to have reached the bus.
                        // Cancellation that arrives while frames remain surfaces
                        // from send_batch_until with the accepted count instead.
                    } catch (const CancelledError &) {
                        outcome = SendOutcome::cancelled;
                        error = std::current_exception();
                    } catch (const can::TimeoutError &) {
                        outcome = SendOutcome::expired;
                        error = std::current_exception();
                    } catch (...) {
                        outcome = SendOutcome::failed;
                        error = std::current_exception();
                    }
                    // Extract diagnostics after leaving the backend catch, and
                    // before publishing the same exception to receipt consumers.
                    // No writer may inspect its payload after finish() wakes a
                    // consumer which can immediately rethrow and release it.
                    std::string diagnostic;
                    if (error) {
                        try {
                            try {
                                std::rethrow_exception(error);
                            } catch (const std::exception &failure) {
                                diagnostic = failure.what();
                            } catch (...) {
                                diagnostic = "non-standard send failure";
                            }
                        } catch (...) {
                            // Diagnostic allocation is best-effort. Do not replace
                            // the actual send outcome or partial-acceptance counts.
                            diagnostic.clear();
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        health.tx_frames += sent;
                        for (size_t i = 0; i < sent; ++i) {
                            health.tx_bytes += job->batch.frames[i].dlc;
                        }
                        if (error) {
                            health.last_error = std::move(diagnostic);
                        }
                        if (outcome == SendOutcome::dispatched) {
                            health.consecutive_send_errors = 0;
                        } else if (outcome == SendOutcome::failed) {
                            ++health.consecutive_send_errors;
                        }
                        finish(job, outcome, std::move(error), sent, uncertain);
                        claimed.reset();
                        if (options.max_consecutive_send_errors != 0 &&
                            health.consecutive_send_errors >= options.max_consecutive_send_errors) {
                            health.open = false;
                            terminal_failure = true;
                            invalidate_locked(std::make_exception_ptr(
                                can::FaultedError("consecutive Session send failures")));
                        }
                        cv.notify_all();
                    }
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                health.open = false;
                health.last_error = "send worker failed";
                terminal_failure = true;
                if (claimed) {
                    finish(claimed, SendOutcome::failed, std::current_exception(), 0, true);
                }
                invalidate_locked(std::current_exception());
            }
        }
    };

    namespace {
        struct RegistryEntry {
            std::weak_ptr<Session> session;
            std::condition_variable changed;
            bool initializing = false;
            std::thread::id initializer;
            uint64_t initialization_attempt = 0;
            bool active = false;
            uint64_t generation = 0;
        };
        // Registry entries are intentionally retained for the process lifetime.  Wait
        // edges and Session lifetimes hold node pointers, which stay stable across
        // insertions into std::map but would be invalidated by erasing a node.
        using SessionRegistry = std::map<const void *, RegistryEntry>;
        // A thread can initialize several independent backends recursively, but can
        // wait for at most one other initializer at a time. Guard this wait-for graph
        // with registry_mutex and reject an edge that would close a cycle.
        struct RegistryWaitEdge {
            RegistryEntry *entry;
            std::thread::id initializer;
            uint64_t initialization_attempt;
        };
        struct RegistryState {
            std::mutex mutex;
            SessionRegistry bus_sessions;
            SessionRegistry borrowed_sessions;
            std::map<std::thread::id, RegistryWaitEdge> waits;
        };
        auto registry_state() -> RegistryState & {
            // Static Session owners can be destroyed after function-local statics.
            // Keep the registry reachable until process teardown so every stored
            // RegistryEntry pointer and its mutex remain valid during their release.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            static auto *const state = new RegistryState;
            return *state;
        }
        auto registry_mutex() -> std::mutex & {
            return registry_state().mutex;
        }
        auto bus_sessions() -> SessionRegistry & {
            return registry_state().bus_sessions;
        }
        auto borrowed_sessions() -> SessionRegistry & {
            return registry_state().borrowed_sessions;
        }
        auto registry_waits() -> std::map<std::thread::id, RegistryWaitEdge> & {
            return registry_state().waits;
        }

        bool wait_would_cycle(std::thread::id waiter, const RegistryEntry &initializing_entry) {
            auto current = initializing_entry.initializer;
            for (size_t steps = 0; steps <= registry_waits().size(); ++steps) {
                if (current == waiter) {
                    return true;
                }
                const auto next = registry_waits().find(current);
                if (next == registry_waits().end()) {
                    return false;
                }
                const auto &edge = next->second;
                // Publication or abandonment satisfies this dependency before the
                // notified waiter necessarily reacquires registry_mutex.  A new
                // attempt, even by the same thread, is a different dependency.
                if (!edge.entry->initializing || edge.entry->initializer != edge.initializer ||
                    edge.entry->initialization_attempt != edge.initialization_attempt) {
                    return false;
                }
                current = edge.initializer;
            }
            // The existing graph should always be acyclic. Refuse to extend it if a
            // stale or corrupted cycle is ever observed rather than waiting forever.
            return true;
        }

        class RegistryWait {
          public:
            RegistryWait(std::thread::id waiter, RegistryEntry *entry, std::thread::id initializer,
                         uint64_t initialization_attempt)
                : waiter_(waiter) {
                const auto inserted = registry_waits()
                                          .emplace(waiter, RegistryWaitEdge{entry, initializer,
                                                                            initialization_attempt})
                                          .second;
                if (!inserted) {
                    throw can::ValidationError(
                        "Session initializer already waits for another backend");
                }
            }
            ~RegistryWait() {
                registry_waits().erase(waiter_);
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
            if (session_cleanup_depth() != 0) {
                throw can::ValidationError(
                    "Session registry cannot be entered during Session cleanup");
            }
            std::unique_lock<std::mutex> lock(registry_mutex());
            auto &entry = registry[key];
            for (;;) {
                if (auto previous = entry.session.lock()) {
                    return {&entry, std::move(previous)};
                }
                if (entry.active) {
                    // Final release from a Session callback is cleaned on an external
                    // worker. Waiting here from that same callback/writer would make
                    // cleanup wait for the caller that is waiting for cleanup.
                    if ((current_writer() != nullptr) ||
                        can::CANDispatcher::is_any_receive_thread()) {
                        throw can::ValidationError(
                            "Session backend cleanup is still active on this callback thread");
                    }
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
                    ++entry.initialization_attempt;
                    if (entry.initialization_attempt == 0) {
                        ++entry.initialization_attempt;
                    }
                    return {&entry, {}};
                }
                if (entry.initializer == std::this_thread::get_id()) {
                    throw can::ValidationError(
                        "recursive Session initialization for the same backend");
                }
                const auto waiter = std::this_thread::get_id();
                const auto initializer = entry.initializer;
                const auto initialization_attempt = entry.initialization_attempt;
                if (wait_would_cycle(waiter, entry)) {
                    throw can::ValidationError("cyclic Session backend initialization");
                }
                RegistryWait waiting(waiter, &entry, initializer, initialization_attempt);
                // A completed attempt is the event this wait consumes.  If a new
                // attempt overtakes us before mutex reacquisition, leave this scope to
                // remove the old wait-for edge, then bind to the new initializer on
                // the next loop iteration.  Spurious wakes are safe for the same
                // reason.
                entry.changed.wait(lock);
            }
        }

        void publish_session(RegistryReservation &reservation,
                             const std::shared_ptr<Session> &session, RegistryLifetime &lifetime) {
            std::lock_guard<std::mutex> lock(registry_mutex());
            ++reservation.entry->generation;
            if (reservation.entry->generation == 0) {
                ++reservation.entry->generation;
            }
            lifetime.entry = reservation.entry;
            lifetime.generation = reservation.entry->generation;
            reservation.entry->session = session;
            reservation.entry->active = true;
            reservation.entry->initializing = false;
            reservation.entry->initializer = {};
            reservation.entry->changed.notify_all();
        }

        void abandon_session(RegistryReservation &reservation) {
            std::lock_guard<std::mutex> lock(registry_mutex());
            reservation.entry->initializing = false;
            reservation.entry->initializer = {};
            reservation.entry->changed.notify_all();
        }

        void release_registry_lifetime(RegistryEntry *entry, uint64_t generation) noexcept {
            if (entry == nullptr) {
                return;
            }
            const std::lock_guard<std::mutex> lock(registry_mutex());
            if (!entry->active || entry->generation != generation) {
                return;
            }
            entry->session.reset();
            entry->active = false;
            entry->changed.notify_all();
        }

        RegistryLifetime::~RegistryLifetime() {
            release_registry_lifetime(entry, generation);
        }
    } // namespace
    // No detached worker and no allocation on final callback release. The queue
    // owns Impl until close has crossed the subscription and writer barriers.
    class Session::Cleanup {
      public:
        Cleanup() : worker_([this] { run(); }) {}
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
                cv_.notify_all();
            }
            worker_.join();
        }
        static Cleanup &instance() {
            static Cleanup cleanup;
            return cleanup;
        }
        void enqueue(Impl *impl) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tail_ != nullptr) {
                tail_->cleanup_next = impl;
            } else {
                head_ = impl;
            }
            tail_ = impl;
            cv_.notify_all();
        }
        void drain() {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return (head_ == nullptr) && !active_; });
        }

      private:
        void run() {
            for (;;) {
                std::unique_ptr<Impl> impl;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&] { return stopping_ || (head_ != nullptr); });
                    if (head_ == nullptr) {
                        return;
                    }
                    impl.reset(head_);
                    head_ = head_->cleanup_next;
                    impl->cleanup_next = nullptr;
                    if (head_ == nullptr) {
                        tail_ = nullptr;
                    }
                    active_ = true;
                }
                {
                    SessionCleanupScope cleanup_scope;
                    try {
                        impl->close();
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                        // Destroy retained ownership after leaving the handler,
                        // with the cleanup marker still active. This preserves
                        // the backend destructor's exception context.
                    }
                    impl.reset();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    active_ = false;
                    cv_.notify_all();
                }
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
        if ((current_writer() != nullptr) || can::CANDispatcher::is_any_receive_thread()) {
            throw can::ValidationError("session cleanup drain requires an external caller");
        }
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
        if (!p.health.open) {
            throw can::ClosedError("session backend is not open");
        }
        p.subscription = p.bus
                             ? p.bus->subscribe([&p](const can::Message &m) { p.receive(m); })
                             : p.borrowed->subscribe([&p](const can::Message &m) { p.receive(m); });
        try {
            p.writer = std::thread([&p] { p.run(); });
        } catch (...) {
            if (p.bus) {
                p.bus->unsubscribe(p.subscription);
            } else {
                p.borrowed->unsubscribe(p.subscription);
            }
            throw;
        }
    }
    std::shared_ptr<Session> Session::create(Factory factory, size_t capacity) {
        if (capacity == 0u) {
            throw can::ValidationError("send capacity must be positive");
        }
        auto p = std::make_unique<Impl>();
        p->bus = std::make_shared<can::ManagedCANBus>(std::move(factory));
        p->owns_bus = true;
        p->tx_capacity = capacity;
        p->options.tx_capacity = capacity;
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
        if (!bus) {
            throw can::ValidationError("session bus must not be null");
        }
        auto reservation = reserve_session(bus_sessions(), bus.get());
        if (reservation.previous) {
            std::lock_guard<std::mutex> operation(reservation.previous->impl_->lifecycle);
            std::lock_guard<std::mutex> state(reservation.previous->impl_->mutex);
            if (reservation.previous->impl_->stopping) {
                throw can::ClosedError("physical session is already closed");
            }
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
        auto reservation = reserve_session(bus_sessions(), bus.get());
        if (reservation.previous) {
            std::lock_guard<std::mutex> operation(reservation.previous->impl_->lifecycle);
            std::lock_guard<std::mutex> state(reservation.previous->impl_->mutex);
            if (reservation.previous->impl_->stopping) {
                throw can::ClosedError("physical session is already closed");
            }
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
        auto reservation = reserve_session(borrowed_sessions(), &dispatcher);
        if (reservation.previous) {
            std::lock_guard<std::mutex> state(reservation.previous->impl_->mutex);
            if (reservation.previous->impl_->stopping) {
                throw can::ClosedError("borrowed session is already closed");
            }
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
        if ((current_writer() != nullptr) || can::CANDispatcher::is_any_receive_thread()) {
            Cleanup::instance().enqueue(impl_.release());
            return;
        }
        try {
            impl_->close();
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // The reset below runs after this handler exits, preserving the
            // original exception context of backend destruction.
        }
        // Keep the marker active through member destruction too: a lease or owned
        // backend destructor may retry a virtual close after close() failed.
        impl_.reset();
    }
    std::shared_ptr<RouteInbox> Session::subscribe_routes(const std::vector<RouteKey> &keys,
                                                          size_t capacity) {
        if (capacity == 0u) {
            throw can::ValidationError("route capacity must be positive");
        }
        auto state = std::make_shared<RouteInbox::State>();
        state->capacity = capacity;
        state->owner = impl_.get();
        state->signal = impl_->signal;
        state->health.capacity = capacity;
        auto inbox = std::shared_ptr<RouteInbox>(new RouteInbox(state));
        update_routes(inbox, keys);
        return inbox;
    }
    void Session::set_route_key_extractor(std::function<RouteKey(const can::Message &)> extractor) {
        auto &p = *impl_;
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.stopping || !p.health.open) {
            throw can::ClosedError("session is closed");
        }
        if (p.routing_started || p.health.accepted != 0u) {
            throw can::ValidationError("route extractor must be configured before Session use");
        }
        p.route_key_of = std::move(extractor);
    }
    std::shared_ptr<ChangeSignal> Session::notification() const {
        return impl_->signal;
    }
    void Session::update_routes(const std::shared_ptr<RouteInbox> &inbox,
                                const std::vector<RouteKey> &keys) {
        if (!inbox || inbox->state_->owner != impl_.get() || keys.empty()) {
            throw can::ValidationError("route group must belong to this session and contain keys");
        }
        std::set<RouteKey> unique;
        for (const auto &key : keys) {
            can::validate_arbitration_id(key.id, key.extended);
            if (!unique.insert(key).second) {
                throw can::ValidationError("duplicate key in route group");
            }
        }
        auto &p = *impl_;
        p.refresh();
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.stopping || !p.health.open) {
            throw can::ClosedError("session is closed");
        }
        std::lock_guard<std::mutex> inbox_lock(inbox->state_->mutex);
        if (inbox->state_->health.closed) {
            throw can::ClosedError("route is closed");
        }
        for (const auto &key : unique) {
            auto it = p.routes.find(key);
            if (it == p.routes.end()) {
                continue;
            }
            if (auto current = it->second.lock(); current && current != inbox->state_) {
                std::lock_guard<std::mutex> existing_lock(current->mutex);
                if (!current->health.closed) {
                    throw can::ValidationError("CAN route already registered");
                }
            }
        }
        // Allocate all entries before removing old keys; rollback new nodes on OOM.
        std::vector<RouteKey> inserted;
        inserted.reserve(unique.size()); // rollback bookkeeping must not allocate after insertion
        try {
            for (const auto &key : unique) {
                auto [it, added] = p.routes.emplace(key, inbox->state_);
                if (added) {
                    inserted.push_back(key);
                }
            }
        } catch (...) {
            for (const auto &key : inserted) {
                p.routes.erase(key);
            }
            throw;
        }
        for (const auto &key : unique) {
            p.routes[key] = inbox->state_;
        }
        for (auto it = p.routes.begin(); it != p.routes.end();) {
            if (it->second.lock() == inbox->state_ && (unique.count(it->first) == 0u)) {
                it = p.routes.erase(it);
            } else {
                ++it;
            }
        }
        inbox->state_->health.generation = p.health.generation;
        p.routing_started = true;
    }
    SendReceipt Session::submit(MessageBatch batch, SendOptions options) {
        return *submit_impl(std::move(batch), std::move(options), false);
    }
    std::optional<SendReceipt> Session::replace_latest_if_pending(MessageBatch batch,
                                                                  SendOptions options) {
        if (options.policy != SendPolicy::latest) {
            throw can::ValidationError("conditional replacement requires latest policy");
        }
        return submit_impl(std::move(batch), std::move(options), true);
    }
    std::optional<SendReceipt> Session::submit_impl(MessageBatch batch, SendOptions options,
                                                    bool replace_only) {
        if (batch.frames.empty()) {
            throw can::ValidationError("empty send batch");
        }
        if (batch.gap.count() < 0 || batch.initial_delay.count() < 0) {
            throw can::ValidationError("batch pacing must not be negative");
        }
        for (const auto &m : batch.frames) {
            can::validate_arbitration_id(m.id, m.is_extended);
            can::validate_frame_flags(m.is_fd, m.is_brs);
            // TX retains the legacy untagged-on-FD-link compatibility rule.
            can::validate_payload_length(m.dlc, can::LinkKind::kCanFd);
        }
        if (options.max_pending_age && options.max_pending_age->count() <= 0) {
            throw can::ValidationError("pending age must be positive");
        }
        if (options.policy == SendPolicy::latest &&
            (options.endpoint.empty() || options.family.empty())) {
            throw can::ValidationError("latest requires explicit endpoint and family");
        }
        auto job = std::make_shared<Impl::Job>();
        job->batch = std::move(batch);
        job->options = std::move(options);
        job->receipt = std::make_shared<SendReceipt::State>();
        auto &p = *impl_;
        p.refresh();
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.stopping || !p.health.open) {
            throw can::ClosedError("session is closed");
        }
        job->receipt->signal = p.signal;
        if (p.stopped_endpoints.count(job->options.endpoint) != 0u) {
            throw can::ClosedError("endpoint admission is stopped");
        }
        if (job->options.expected_generation &&
            *job->options.expected_generation != p.health.generation) {
            throw can::ClosedError("submission generation is stale");
        }
        job->options.cancellation.throw_if_cancelled();
        if (Clock::now() >= job->options.deadline) {
            throw can::TimeoutError("send deadline expired");
        }
        const auto now = Clock::now();
        if (job->options.max_pending_age && p.oldest_accepted() != Deadline::max() &&
            std::chrono::duration_cast<std::chrono::microseconds>(now - p.oldest_accepted()) >=
                *job->options.max_pending_age) {
            ++p.health.rejected_stale;
            throw can::TimeoutError("pending work exceeded age budget");
        }
        auto previous = p.jobs.end();
        if (job->options.policy == SendPolicy::latest) {
            previous = std::find_if(p.jobs.begin(), p.jobs.end(), [&](const auto &old) {
                return old->options.policy == SendPolicy::latest &&
                       old->options.endpoint == job->options.endpoint &&
                       old->options.family == job->options.family;
            });
        }
        if (replace_only && previous == p.jobs.end()) {
            return std::nullopt;
        }
        const auto units = p.pending_units();
        bool full = previous == p.jobs.end() && p.jobs.size() >= p.tx_capacity;
        if (p.options.separate_policy_capacity) {
            if (job->options.policy == SendPolicy::latest) {
                full = previous == p.jobs.end() && units.second >= p.options.latest_capacity;
            } else {
                full = p.fifo_cost(*job) > p.options.fifo_capacity - units.first;
            }
        }
        if (full) {
            ++p.health.rejected_full;
            throw can::QueueFullError("session send queue is full");
        }
        auto &result = job->receipt->result;
        result.generation = p.health.generation;
        result.accepted_at = Clock::now();
        result.total_frames = job->batch.frames.size();
        // Allocate before changing an old receipt: failed admission must not
        // supersede accepted work. deque insertion may invalidate iterators.
        auto old = previous == p.jobs.end() ? nullptr : *previous;
        p.last_served.try_emplace(job->options.endpoint, 0);
        auto replacement_error =
            old ? std::make_exception_ptr(CancelledError("pending latest send superseded"))
                : std::exception_ptr{};
        p.jobs.push_back(job);
        if (old) {
            p.finish(old, SendOutcome::superseded, std::move(replacement_error));
            p.jobs.erase(std::find(p.jobs.begin(), p.jobs.end(), old));
        }
        ++p.health.accepted;
        p.health.queue_high_water = std::max(p.health.queue_high_water, p.jobs.size());
        p.cv.notify_one();
        return SendReceipt(job->receipt);
    }
    void Session::send(MessageBatch batch, SendOptions options) {
        const auto deadline = options.deadline;
        auto receipt = submit(std::move(batch), std::move(options));
        (void)receipt.wait_until(deadline);
    }
    void Session::cancel_endpoint(const std::string &endpoint) {
        auto &p = *impl_;
        std::lock_guard<std::mutex> lock(p.mutex);
        for (auto it = p.jobs.begin(); it != p.jobs.end();) {
            if ((*it)->options.endpoint == endpoint) {
                (*it)->abort.cancel();
                p.finish(*it, SendOutcome::cancelled,
                         std::make_exception_ptr(CancelledError("endpoint cancelled")));
                it = p.jobs.erase(it);
            } else {
                ++it;
            }
        }
        if (p.claimed && p.claimed->options.endpoint == endpoint) {
            p.claimed->abort.cancel();
        }
        p.cv.notify_all();
    }
    void Session::configure(SessionOptions options) {
        if (options.tx_capacity == 0 || options.fifo_capacity == 0 ||
            options.latest_capacity == 0 || options.aging_quantum.count() <= 0 ||
            options.send_period.count() < 0) {
            throw can::ValidationError("invalid Session scheduling options");
        }
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopping || impl_->health.accepted != 0) {
            throw can::ValidationError("configure requires a session with no accepted sends");
        }
        impl_->options = options;
        impl_->tx_capacity = options.tx_capacity;
    }
    SessionOptions Session::options() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->options;
    }
    size_t Session::stop_endpoint(const std::string &endpoint, Deadline deadline) {
        if (current_writer() != nullptr || can::CANDispatcher::is_any_receive_thread()) {
            throw can::ValidationError("endpoint drain requires an external caller");
        }
        auto &p = *impl_;
        std::unique_lock<std::mutex> lock(p.mutex);
        p.stopped_endpoints.insert(endpoint);
        size_t count = 0;
        for (auto it = p.jobs.begin(); it != p.jobs.end();) {
            if ((*it)->options.endpoint == endpoint) {
                p.finish(*it, SendOutcome::cancelled,
                         std::make_exception_ptr(can::ClosedError("endpoint stopped")));
                it = p.jobs.erase(it);
                ++count;
            } else {
                ++it;
            }
        }
        p.cv.notify_all();
        const auto drained = [&] { return !p.claimed || p.claimed->options.endpoint != endpoint; };
        if (deadline == Deadline::max()) {
            p.cv.wait(lock, drained);
        } else if (!p.cv.wait_until(lock, deadline, drained)) {
            throw can::TimeoutError("endpoint claimed send did not drain");
        }
        return count;
    }
    void Session::resume_endpoint(const std::string &endpoint) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopping || !impl_->health.open) {
            throw can::ClosedError("session closed");
        }
        if (impl_->claimed && impl_->claimed->options.endpoint == endpoint) {
            throw can::ValidationError("endpoint still has a claimed send");
        }
        impl_->stopped_endpoints.erase(endpoint);
    }
    SessionHealth Session::health() const {
        impl_->refresh();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto h = impl_->health;
        h.pending_tx = impl_->jobs.size() + (impl_->claimed ? 1 : 0);
        const auto units = impl_->pending_units();
        h.pending_fifo = units.first;
        h.pending_latest = units.second;
        h.in_flight = bool(impl_->claimed);
        h.in_flight_frames = impl_->claimed ? impl_->claimed->batch.frames.size() : 0;
        const auto oldest = impl_->oldest_accepted();
        if (oldest != Deadline::max()) {
            h.oldest_pending_age =
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - oldest);
        }
        return h;
    }
    can::TransportHealth Session::transport_health() const {
        auto h = impl_->bus ? impl_->bus->get_health() : impl_->borrowed->get_health();
        const auto session = health();
        if (!session.open && h.connected()) {
            h.state = can::TransportState::kClosed;
        }
        h.pending_tx += session.pending_tx;
        return h;
    }
    void Session::close() {
        SessionCleanupScope cleanup_scope;
        impl_->close();
    }
    void Session::Impl::close() {
        auto &p = *this;
        if ((current_writer() != nullptr) || can::CANDispatcher::is_any_receive_thread()) {
            throw can::ValidationError("session lifecycle requires an external caller");
        }
        std::lock_guard<std::mutex> lifecycle(p.lifecycle);
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            p.stopping = true;
            p.health.open = false;
            p.invalidate_locked(std::make_exception_ptr(can::ClosedError("session closed")));
        }
        if (p.subscription != 0u) {
            if (p.bus) {
                p.bus->unsubscribe(p.subscription);
            } else {
                p.borrowed->unsubscribe(p.subscription);
            }
            p.subscription = 0;
        }
        if (p.writer.joinable()) {
            p.writer.join();
        }
        if (p.owns_bus) {
            p.bus->stop(); // explicit failure remains retryable
        }
        if (p.owned_dispatcher) {
            p.owned_dispatcher->stop();
        }
        std::exception_ptr release_error;
        for (auto &lease : p.leases) {
            try {
                lease->close();
            } catch (...) {
                if (!release_error) {
                    release_error = std::current_exception();
                }
            }
        }
        if (release_error) {
            std::rethrow_exception(release_error); // failed lease remains retryable
        }
        p.leases.clear();
    }
    uint64_t Session::recover(uint64_t observed_generation) {
        auto &p = *impl_;
        if ((current_writer() != nullptr) || can::CANDispatcher::is_any_receive_thread()) {
            throw can::ValidationError("session lifecycle requires an external caller");
        }
        if (!p.bus) {
            throw can::ValidationError(
                "physical recovery requires a transport factory or managed bus");
        }
        std::lock_guard<std::mutex> lifecycle(p.lifecycle);
        {
            std::unique_lock<std::mutex> lock(p.mutex);
            if (p.stopping) {
                throw can::ClosedError("closed session cannot recover");
            }
            if (p.terminal_failure) {
                throw can::FaultedError("Session internal failure; close and recreate the session");
            }
            if (p.health.generation != observed_generation && p.health.open) {
                return p.health.generation;
            }
            p.recovering = true;
            p.health.open = false;
            p.invalidate_locked(std::make_exception_ptr(
                can::ClosedError("physical recover invalidated generation")));
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
