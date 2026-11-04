#include "smoke.hpp"

#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>
#include <ros/node_handle.h>

namespace linkerbot_ros1_smoke {
    class CommSmoke final : public nodelet::Nodelet {
        void onInit() override {
            run_comm_smoke();
            getNodeHandle().setParam("/comm_smoke/nodelet_passed", true);
        }
    };
} // namespace linkerbot_ros1_smoke

PLUGINLIB_EXPORT_CLASS(linkerbot_ros1_smoke::CommSmoke, nodelet::Nodelet)
