#pragma once

#include <linkerbot/can/errors.hpp>
#include <linkerbot/can/message.hpp>

#include <optional>
#include <vector>

namespace linkerbot::can {

    // Frame send/receive on one physical link. Implementations own the link's
    // lifetime: acquire it in the constructor, release it in the destructor.
    //
    // `read()` must not block indefinitely — CANDispatcher polls it from a
    // dedicated thread and relies on a bounded return to observe its stop flag.
    // Return std::nullopt on timeout (that is normal, not an error); throw only
    // for a genuinely broken link.
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

        // Returns the next received frame, or std::nullopt if none arrived within
        // the implementation's receive timeout.
        virtual auto read() -> std::optional<Message> = 0;

        // Returns after the backend accepts the frame. Failures throw a stable
        // CANError subtype and retain a system error or vendor status when one
        // exists.
        virtual auto write(const Message &message) -> void = 0;

        // Transmit several frames. The default implementation writes sequentially.
        // A failed batch must be treated as failure for every frame: backends that
        // coalesce into one syscall cannot report which frames reached the bus.
        virtual auto write_batch(const std::vector<Message> &messages) -> void {
            for (const auto &message : messages) {
                write(message);
            }
        }

        [[nodiscard]] virtual auto prefers_write_batch() const -> bool {
            return false;
        }

        // Interrupt pending I/O and release the physical link. Must be
        // idempotent and safe after a partial initialization or prior failure.
        virtual auto close() -> void = 0;

      protected:
        CANTransport() = default;
    };

} // namespace linkerbot::can
