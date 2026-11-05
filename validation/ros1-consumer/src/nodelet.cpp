#include "smoke.hpp"

#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>
#include <ros/node_handle.h>

namespace linkerbot_ros1_smoke {
    class TransportSmoke final : public nodelet::Nodelet {
        void onInit() override {
            run_transport_smoke();
            getNodeHandle().setParam("/transport_smoke/nodelet_passed", true);
        }
    };
} // namespace linkerbot_ros1_smoke

PLUGINLIB_EXPORT_CLASS(linkerbot_ros1_smoke::TransportSmoke, nodelet::Nodelet)
