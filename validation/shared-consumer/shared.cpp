#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <memory>

extern "C" int transport_shared_smoke() {
    auto session =
        linkerbot::transport::Session::own(std::make_unique<linkerbot::can::FakeTransport>());
    session->close();
    return 0;
}
