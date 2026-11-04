#include "smoke.hpp"

#include <ros/ros.h>

#include <exception>

int main(int argc, char **argv) {
    ros::init(argc, argv, "comm_smoke_node");
    try {
        run_comm_smoke();
        ros::NodeHandle().setParam("/comm_smoke/node_passed", true);
        return 0;
    } catch (const std::exception &error) {
        ROS_ERROR_STREAM(error.what());
        return 1;
    }
}
