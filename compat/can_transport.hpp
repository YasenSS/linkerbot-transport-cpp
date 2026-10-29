#pragma once

// Compatibility shim: linkerbot-hand-cpp includes this header by its flat name
// (`#include "can_transport.hpp"`) from ~91 files. Add this directory to the
// include path and those files keep compiling untouched.
//
// New code should include <linkerbot/can/message.hpp> / <linkerbot/can/transport.hpp>.

#include <linkerbot/can/message.hpp>
#include <linkerbot/can/transport.hpp>
