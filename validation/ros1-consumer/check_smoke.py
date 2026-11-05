#!/usr/bin/env python3
"""Assert actual node and dynamically loaded nodelet completed the transport probe."""
import time
import unittest

import rospy
import rostest
from nodelet.srv import NodeletUnload


class Smoke(unittest.TestCase):
    def test_node_and_nodelet(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if all(rospy.get_param('/transport_smoke/' + name, False)
                   for name in ('node_passed', 'nodelet_passed')):
                service = '/transport_smoke_manager/unload_nodelet'
                rospy.wait_for_service(service, timeout=5)
                result = rospy.ServiceProxy(service, NodeletUnload)('/transport_smoke_nodelet')
                self.assertTrue(result.success, 'Nodelet did not unload successfully')
                return
            time.sleep(0.1)
        self.fail('Node and nodelet did not both complete the in-memory SDK probe')


if __name__ == '__main__':
    rospy.init_node('transport_ros1_smoke_assertions')
    rostest.rosrun('linkerbot_transport_ros1_smoke', 'transport_ros1_smoke', Smoke)
