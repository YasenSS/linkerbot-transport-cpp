#pragma once

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/message.hpp>

#include <optional>
#include <vector>

namespace linkerbot::can {

    // Frame send/receive on one physical link. Implementations own the link's
    // lifetime: acquire it in the constructor, release it in the destructor.
    //
    // Configure finite receive waits: CANDispatcher uses a dedicated reader
    // thread and checks its stop flag between calls. Driver calls without a
    // cancellation API may still delay shutdown beyond the configured timeout.
    // Return std::nullopt when no application frame is returned, including a
    // normal timeout or a filtered frame. Empty results need not consume the
    // full receive timeout. Invalid input, closed resources and I/O failures
    // may throw; an exception alone does not prove physical disconnection.
    //
    // Both methods are called from different threads concurrently (read from the
    // dispatcher thread, write from callers), so implementations must be
    // thread-safe with respect to that pairing.
    class CANTransport {
      public:
        virtual ~CANTransport() = default;

        CANTransport(const CANTransport &) = delete;
        CANTransport &operator=(const CANTransport &) = delete;
        CANTransport(CANTransport &&) = delete;
        CANTransport &operator=(CANTransport &&) = delete;

        // Returns an application frame, or std::nullopt if this call returns none.
        // Backends may return early after filtering a non-application frame.
        virtual auto read() -> std::optional<Message> = 0;

        // Returns after the backend accepts the frame. Failures throw a stable
        // CANError subtype and retain a system error or vendor status when one
        // exists.
        virtual auto write(const Message &message) -> void = 0;

        // Transmit several frames. The default implementation writes sequentially.
        // An exception does not roll back frames already accepted by the backend.
        // Coalesced native batches may partially complete without reporting a
        // per-frame result. Reconcile device state before retrying the batch.
        virtual auto write_batch(const std::vector<Message> &messages) -> void {
            for (const auto &message : messages) {
                write(message);
            }
        }

        [[nodiscard]] virtual auto prefers_write_batch() const -> bool {
            return false;
        }

        // Request cancellation where supported, coordinate in-flight I/O and
        // release owned resources. Repeated calls are safe after success;
        // resources retained after a failure remain available for cleanup retry.
        virtual auto close() -> void = 0;

      protected:
        CANTransport() = default;
    };

} // namespace linkerbot::can
