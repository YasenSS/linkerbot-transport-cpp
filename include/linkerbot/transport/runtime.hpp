#pragma once

#include <linkerbot/can/pool.hpp>
#include <linkerbot/transport/session.hpp>

namespace linkerbot::transport {
    enum class ChannelResetPolicy { kAllowCoordinatedReset, kRequireDetach };
    struct ChannelSpec {
        can::CANBusKey key;
        // Canonical identity for all backend settings not represented in key.
        std::string configuration_fingerprint;
        SessionOptions scheduling;
    };
    struct DeviceSpec {
        std::string identity;
        DeviceOptions scheduling;
        ChannelResetPolicy channel_reset = ChannelResetPolicy::kAllowCoordinatedReset;
    };

    // All instances in this linked runtime share one channel registry and pool.
    // Last-device close releases the physical lease; failed closes are retryable.
    class TransportRuntime {
      public:
        TransportRuntime();
        std::shared_ptr<DeviceSession> open_device(ChannelSpec channel, DeviceSpec device,
                                                   Session::Factory factory);
        // Replace only this logical registration. SDK revalidation runs separately.
        std::shared_ptr<DeviceSession> rebind_device(const can::CANBusKey &key,
                                                     const std::shared_ptr<DeviceSession> &previous,
                                                     DeviceSpec device, Deadline deadline);
        [[nodiscard]] size_t active_devices(const can::CANBusKey &key) const;
        [[nodiscard]] SessionHealth channel_health(const can::CANBusKey &key) const;
        uint64_t recover_channel(const can::CANBusKey &key, uint64_t observed_generation);

      private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
} // namespace linkerbot::transport
