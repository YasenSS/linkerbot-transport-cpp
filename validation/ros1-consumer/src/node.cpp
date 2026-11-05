#include "smoke.hpp"

#include <ros/ros.h>

#include <exception>

int main(int argc, char **argv) {
    ros::init(argc, argv, "transport_smoke_node");
    try {
        run_transport_smoke();
        ros::NodeHandle().setParam("/transport_smoke/node_passed", true);
        return 0;
    } catch (const std::exception &error) {
        ROS_ERROR_STREAM(error.what());
        return 1;
    }
}
