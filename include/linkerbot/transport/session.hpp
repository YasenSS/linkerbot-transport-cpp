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

    struct RouteKey {
        uint32_t id = 0;
        bool extended = false;
        friend bool operator<(const RouteKey &a, const RouteKey &b) noexcept {
            return a.id < b.id || (a.id == b.id && a.extended < b.extended);
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
    };

    struct RouteHealth {
        bool closed = false;
        uint64_t generation = 0;
        uint64_t overflow = 0;
        size_t pending = 0;
        size_t high_water = 0;
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
        RouteHealth health() const;
        void close() noexcept;

      private:
        friend class Session;
        struct State;
        explicit RouteInbox(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };

    enum class SendOutcome { pending, dispatched, superseded, cancelled, expired, failed };
    enum class SendPolicy { fifo, latest };

    struct MessageBatch {
        std::vector<can::Message> frames;
        std::chrono::microseconds gap{0};
        // Some classic protocols require a delay before the first frame too.
        std::chrono::microseconds initial_delay{0};
    };

    struct SendOptions {
        Deadline deadline = Deadline::max();
        CancellationToken cancellation;
        std::string endpoint;
        std::string family;
        SendPolicy policy = SendPolicy::fifo;
        int priority = 0;
    };

    struct SendResult {
        SendOutcome outcome = SendOutcome::pending;
        uint64_t generation = 0;
        Deadline accepted_at{};
        std::optional<Deadline> claimed_at;
        std::optional<Deadline> completed_at;
        size_t total_frames = 0;
        size_t sent_frames = 0;
        // A backend exception may occur after its current frame reached the bus.
        bool current_frame_uncertain = false;
        std::exception_ptr error;
    };

    class SendReceipt {
      public:
        SendReceipt() = default;
        SendResult result() const;
        SendResult wait_until(Deadline deadline) const;
        void wait() const;
        bool completed() const;

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
        void update_routes(const std::shared_ptr<RouteInbox> &inbox,
                           const std::vector<RouteKey> &keys);
        SendReceipt submit(MessageBatch batch, SendOptions options = {});
        void send(MessageBatch batch, SendOptions options = {});
        void cancel_endpoint(const std::string &endpoint);
        SessionHealth health() const;
        // Legacy transport diagnostics projected through the Session boundary.
        can::TransportHealth transport_health() const;
        // Concurrent callers presenting the same observed generation cause only
        // one physical reopen; every old route/receipt is invalidated.
        uint64_t recover(uint64_t observed_generation);
        void close();

      private:
        struct Impl;
        class Cleanup;
        explicit Session(std::unique_ptr<Impl> impl);
        void start();
        std::unique_ptr<Impl> impl_;
    };
} // namespace linkerbot::transport
