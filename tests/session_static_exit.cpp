#include <linkerbot/can/dispatcher.hpp>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/managed_bus.hpp>
#include <linkerbot/transport/session.hpp>

#include <cstdlib>
#include <memory>
#include <string_view>

namespace {
    // These owners intentionally predate the registries initialized by main().
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    std::shared_ptr<linkerbot::can::ManagedCANBus> retained_bus;
    std::unique_ptr<linkerbot::can::CANDispatcher> retained_dispatcher;
    std::shared_ptr<linkerbot::transport::Session> retained_session;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

auto main(int argc, char **argv) -> int {
    if (argc != 2) {
        return EXIT_FAILURE;
    }

    const std::string_view mode(argv[1]);
    if (mode == "shared") {
        retained_bus = std::make_shared<linkerbot::can::ManagedCANBus>(
            [] { return std::make_unique<linkerbot::can::FakeTransport>(); });
        retained_session = linkerbot::transport::Session::share(retained_bus);
    } else if (mode == "borrowed") {
        retained_dispatcher = std::make_unique<linkerbot::can::CANDispatcher>(
            std::make_unique<linkerbot::can::FakeTransport>());
        retained_session = linkerbot::transport::Session::borrow(*retained_dispatcher);
    } else {
        return EXIT_FAILURE;
    }

    return retained_session ? EXIT_SUCCESS : EXIT_FAILURE;
}
