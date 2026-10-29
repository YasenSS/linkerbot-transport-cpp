#ifdef __linux__
#include <catch2/catch_test_macros.hpp>
#include <linkerbot/can/slcan_transport.hpp>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <poll.h>
#include <thread>
#include <unistd.h>

using namespace linkerbot::can;
using namespace std::chrono_literals;
namespace {
struct Pty {
    int master = -1;
    std::string path;
    Pty() {
        master=::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        try {
            if (master<0 || ::grantpt(master) || ::unlockpt(master)) throw std::runtime_error("Cannot create test PTY");
            char name[128]{};
            if (::ptsname_r(master,name,sizeof(name))) throw std::runtime_error("Cannot name test PTY");
            path=name;
        } catch (...) { if(master>=0)::close(master); throw; }
    }
    ~Pty() { if (master>=0) ::close(master); }
    auto config() const -> SlcanConfig { SlcanConfig c; c.port=path; c.serial_baud=460800; return c; }
    auto receive(size_t size) -> std::string {
        std::string result;
        const auto deadline=std::chrono::steady_clock::now()+1s;
        while (result.size()<size && std::chrono::steady_clock::now()<deadline) {
            pollfd p{master,POLLIN,0};
            if (::poll(&p,1,10)>0) { char bytes[256]; auto n=::read(master,bytes,sizeof(bytes)); if (n>0) result.append(bytes,static_cast<size_t>(n)); }
        }
        return result;
    }
};
}
TEST_CASE("SLCAN Linux native serial preserves binary bytes and releases exclusive ownership", "[slcan][native-serial]") {
    Pty pty;
    REQUIRE(enumerate_slcan_ports({pty.path})==std::vector<std::string>{pty.path});
    auto port=open_slcan_serial(pty.config());
    REQUIRE_THROWS(open_slcan_serial(pty.config()));
    REQUIRE(enumerate_slcan_ports({pty.path})==std::vector<std::string>{pty.path}); // Metadata does not open even an occupied port.
    const std::string bytes{"\0\n\r\x11\x13\xff",6};
    port->write_all(bytes,100ms); REQUIRE(pty.receive(bytes.size())==bytes);
    REQUIRE(::write(pty.master,bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size()));
    REQUIRE(port->read_some(256,100ms)==bytes);
    REQUIRE(port->read_some(256,5ms).empty());
    port->close(); REQUIRE_NOTHROW(port->close());
    REQUIRE_THROWS_AS(port->read_some(1,1ms),ClosedError);
    REQUIRE_THROWS_AS(port->write_all("C\r",1ms),ClosedError);
    REQUIRE_NOTHROW(open_slcan_serial(pty.config()));
}
TEST_CASE("SLCAN Linux native cancellation interrupts both blocked read and write", "[slcan][native-serial]") {
    Pty pty; auto port=open_slcan_serial(pty.config());
    std::promise<void> started;
    auto reader=std::async(std::launch::async,[&] {
        started.set_value();
        try { (void)port->read_some(1,5s); } catch (const ClosedError &) { return true; } return false;
    });
    started.get_future().wait(); port->cancel();
    REQUIRE(reader.wait_for(500ms)==std::future_status::ready); REQUIRE(reader.get());
    REQUIRE_NOTHROW(port->close());
    port=open_slcan_serial(pty.config());
    std::promise<void> writing;
    auto writer=std::async(std::launch::async,[&] {
        writing.set_value();
        try { port->write_all(std::string(4*1024*1024,'x'),5s); } catch (const ClosedError &) { return true; } return false;
    });
    writing.get_future().wait();
    // Wait until the PTY receives a prefix so this write is in flight before cancel.
    const auto prefix=pty.receive(1);
    port->cancel();
    REQUIRE(writer.wait_for(500ms)==std::future_status::ready); REQUIRE(writer.get()); REQUIRE_FALSE(prefix.empty());
    port->close();
}
TEST_CASE("SLCAN Linux reports missing devices disconnect and bounded partial writes", "[slcan][native-serial]") {
    auto missing=SlcanConfig{}; missing.port="/dev/linkerbot-test-missing-serial";
    REQUIRE(enumerate_slcan_ports({missing.port}).empty());
    REQUIRE_THROWS_AS(open_slcan_serial(missing),TransportIOError);
    Pty pty; auto port=open_slcan_serial(pty.config());
    REQUIRE_THROWS_AS(port->write_all(std::string(4*1024*1024,'x'),5ms),TimeoutError);
    ::close(pty.master); pty.master=-1;
    REQUIRE_THROWS_AS(port->read_some(1,100ms),TransportIOError);
    REQUIRE_NOTHROW(port->close());
}
TEST_CASE("SLCAN Linux CANable FD crosses the real serial implementation without ACKs", "[slcan][native-serial][fd]") {
    Pty pty; auto c=pty.config(); c.protocol=SlcanProtocol::kCanableFd; c.fd=true;
    SlcanTransport transport(c);
    REQUIRE(pty.receive(10)=="C\rS8\rY5\rO\r");
    Message m; m.id=0x123; m.is_fd=true; m.is_extended=true; m.dlc=64;
    for (unsigned i=0;i<64;++i) m.data[i]=static_cast<uint8_t>(i);
    const auto wire=encode_slcan_frame(m,SlcanProtocol::kCanableFd);
    transport.write(m); REQUIRE(pty.receive(wire.size())==wire);
    REQUIRE(::write(pty.master,wire.data(),wire.size())==static_cast<ssize_t>(wire.size()));
    const auto reply=transport.read(); REQUIRE(reply); REQUIRE(reply->id==m.id);
    REQUIRE(reply->is_fd); REQUIRE(reply->is_extended); REQUIRE_FALSE(reply->is_brs); REQUIRE(reply->data==m.data);
    transport.close(); REQUIRE(pty.receive(2)=="C\r");
}
#endif
