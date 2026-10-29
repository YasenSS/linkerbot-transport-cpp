#include <catch2/catch_test_macros.hpp>
#include <linkerbot/can/pcan_transport.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define CALL __stdcall
#else
#include <dlfcn.h>
#define CALL
#endif
using namespace linkerbot::can;
namespace fs=std::filesystem;
namespace {
std::string path(const char *key="LINKERBOT_TEST_PCAN_LIBRARY") {
    const auto value=std::getenv(key);if(!value)throw std::runtime_error(std::string("Missing fixture path ")+key);return value;
}
template <typename F> F symbol(const std::shared_ptr<PcanApi> &api,const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<F>(GetProcAddress(static_cast<HMODULE>(api->library_owner.get()),name));
#else
    return reinterpret_cast<F>(dlsym(api->library_owner.get(),name));
#endif
}
struct Fixture {
    std::shared_ptr<PcanApi> api=load_pcan_api(path());
    using Get=uint32_t(CALL *)(unsigned);
    using Set=uint32_t(CALL *)(unsigned,uint32_t);
    Get get=symbol<Get>(api,"Test_Get");
    Set set=symbol<Set>(api,"Test_Set");
    Fixture() {
        if(!get||!set)throw std::runtime_error("fixture controls missing");
        for(unsigned i=0;i<32;++i)set(i,0);
        using Clear=uint32_t(CALL *)();symbol<Clear>(api,"Test_ClearReplies")();
    }
    ~Fixture(){set(10,0);set(11,0);set(12,0);set(13,0);set(19,0);try{retry_pcan_cleanup(api);}catch(...){} }
    PcanConfig config(PcanMode mode=PcanMode::kClassic) const {
        PcanConfig value;value.mode=mode;value.channel=mode==PcanMode::kFd?0x52:0x51;value.library_path=api->library_path;return value;
    }
};
}
TEST_CASE("PCAN discovers attached channels and preserves capabilities and conditions","[pcan]") {
    Fixture fixture;const auto found=enumerate_pcan_channels(fixture.api);
    REQUIRE(found.channels.size()==4);REQUIRE(found.failures.size()==1);
    CHECK(found.channels[0].native_name=="PCAN_USBBUS1");
    CHECK(found.channels[0].supports_fd==false);CHECK(found.channels[1].supports_fd==true);
    CHECK_FALSE(found.channels[2].supports_fd.has_value());CHECK(found.channels[3].condition==2);
    CHECK(found.channels[1].device_id==101);CHECK(found.channels[1].controller==1);
    CHECK(found.channels[1].hardware_name=="Independent PCAN");
    CHECK(found.failures[0].native_status==0x4000);CHECK(fixture.get(0)==0);CHECK(fixture.get(1)==0);
}
TEST_CASE("PCAN Classic initialization frames and bounded empty receive","[pcan]") {
    Fixture fixture;PcanTransport transport(fixture.config(),fixture.api);
    CHECK(fixture.get(0)==1);CHECK(fixture.get(1)==0);CHECK(fixture.get(9)==0x14);
    CHECK_FALSE(transport.read());
    for(bool extended:{false,true})for(unsigned length=0;length<=8;++length){
        Message message;message.id=extended?0x1abcdef:0x321;message.is_extended=extended;message.dlc=uint8_t(length);
        for(unsigned i=0;i<length;++i)message.data[i]=uint8_t(i+3);
        transport.write(message);const auto received=transport.read();REQUIRE(received);
        CHECK(received->id==message.id);CHECK(received->is_extended==extended);CHECK_FALSE(received->is_fd);CHECK(received->dlc==length);CHECK(received->data==message.data);
    }
    CHECK(fixture.get(6)==0);CHECK(fixture.get(4)==0);
    Message bad;bad.is_fd=true;CHECK_THROWS_AS(transport.write(bad),ValidationError);
    bad.is_fd=false;bad.dlc=9;CHECK_THROWS_AS(transport.write(bad),ValidationError);
    transport.close();transport.close();CHECK(fixture.get(2)==1);
    CHECK_THROWS_AS(transport.read(),ClosedError);CHECK_THROWS_AS(transport.write(Message{}),ClosedError);
}
TEST_CASE("PCAN FD uses FD API and explicit Classic FD BRS flags for every length","[pcan]") {
    Fixture fixture;const auto config=fixture.config(PcanMode::kFd);PcanTransport transport(config,fixture.api);
    CHECK(fixture.get(0)==0);CHECK(fixture.get(1)==1);
    using Copy=uint32_t(CALL *)(char *,uint32_t);char timing[512]{};
    REQUIRE(symbol<Copy>(fixture.api,"Test_CopyTiming")(timing,sizeof(timing))==0);
    CHECK(std::string(timing)==config.fd_timing.bitrate_string(1000000,5000000));
    const unsigned lengths[]{0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    for(bool extended:{false,true})for(bool brs:{false,true})for(unsigned n=0;n<=64;++n){
        Message message;message.id=extended?0x1234567:0x123;message.is_extended=extended;message.is_fd=true;message.is_brs=brs;message.dlc=uint8_t(n);
        for(unsigned i=0;i<n;++i)message.data[i]=uint8_t(i+n);
        transport.write(message);const auto received=transport.read();REQUIRE(received);
        CHECK(received->is_fd);CHECK(received->is_brs==brs);CHECK(received->is_extended==extended);CHECK(received->id==message.id);
        CHECK(received->dlc==*std::lower_bound(std::begin(lengths),std::end(lengths),n));CHECK(received->data==message.data);
    }
    Message classic;classic.id=4;classic.dlc=8;transport.write(classic);const auto received=transport.read();REQUIRE(received);CHECK_FALSE(received->is_fd);CHECK_FALSE(received->is_brs);
    CHECK(fixture.get(3)==0);CHECK(fixture.get(5)==0);CHECK(fixture.get(6)==261);
    Message invalid;invalid.is_brs=true;CHECK_THROWS_AS(transport.write(invalid),ValidationError);
    invalid.is_brs=false;invalid.dlc=9;CHECK_THROWS_AS(transport.write(invalid),ValidationError);
    invalid.dlc=0;invalid.is_fd=true;invalid.id=0x800;CHECK_THROWS_AS(transport.write(invalid),ValidationError);
}
TEST_CASE("PCAN rejects invalid configuration and capability before initialization","[pcan]") {
    Fixture fixture;auto config=fixture.config(PcanMode::kFd);
    config.channel=0x51;CHECK_THROWS_AS(PcanTransport(config,fixture.api),ValidationError);
    config.channel=0x53;CHECK_THROWS_AS(PcanTransport(config,fixture.api),ValidationError);
    config.channel=0x54;CHECK_THROWS_AS(PcanTransport(config,fixture.api),FaultedError);
    config.channel=0x59;CHECK_THROWS_AS(PcanTransport(config,fixture.api),TransportIOError);
    CHECK(fixture.get(0)==0);CHECK(fixture.get(1)==0);CHECK(fixture.get(2)==0);
    config.channel=0;CHECK_THROWS_AS(PcanTransport(config,fixture.api),ValidationError);
    config.channel=0x52;config.data_bitrate=4000000;CHECK_THROWS_AS(PcanTransport(config,fixture.api),ValidationError);
    config.data_bitrate=5000000;config.fd_timing.nominal_sjw=17;CHECK_THROWS_AS(PcanTransport(config,fixture.api),ValidationError);
    config.fd_timing.nominal_sjw=16;config.receive_timeout=std::chrono::milliseconds(101);CHECK_THROWS_AS(PcanTransport(config,fixture.api),ValidationError);
    auto partial=load_pcan_api(path("LINKERBOT_TEST_PCAN_CLASSIC"));config=fixture.config(PcanMode::kFd);CHECK_THROWS_AS(PcanTransport(config,partial),TransportIOError);
}
TEST_CASE("PCAN status errors preserve native bits and empty does not hide bus off","[pcan]") {
    Fixture fixture;PcanTransport transport(fixture.config(),fixture.api);
    for(const auto status:{0x80U,0x01U,0x10U,0x1400U,0x200U,0x04U,0x400U}){
        fixture.set(13,status);
        try{transport.write(Message{});FAIL("expected native error");}
        catch(const PcanTransportError &error){
            CHECK(error.native_status()==int(status));
            CHECK(error.reason()==(status==0x80||status==1?PcanFailure::kTransmitQueueFull:status==0x10?PcanFailure::kBusOff:status==0x04?PcanFailure::kBusError:status==0x400?PcanFailure::kChannelBusy:PcanFailure::kAdapterUnavailable));
        }
    }
    fixture.set(13,0);fixture.set(12,0x30);
    try{(void)transport.read();FAIL("bus off masked by empty");}catch(const PcanTransportError &error){CHECK(error.reason()==PcanFailure::kBusOff);CHECK(error.native_status()==0x30);}
}
TEST_CASE("PCAN claims detect conflicts and only release owned successful initialization","[pcan]") {
    Fixture fixture;auto config=fixture.config();
    PcanTransport first(config,fixture.api);
    CHECK_THROWS_AS(PcanTransport(config,fixture.api),FaultedError);
    auto conflict=config;conflict.mode=PcanMode::kFd;
    CHECK_THROWS_AS(PcanTransport(conflict,fixture.api),FaultedError);
    CHECK(fixture.get(0)==1);CHECK(fixture.get(1)==0);CHECK(fixture.get(2)==0);
    fixture.set(11,0x200);CHECK_THROWS_AS(first.close(),PcanTransportError);
    CHECK_THROWS_AS(PcanTransport(config,fixture.api),FaultedError);
    CHECK_THROWS_AS(first.write(Message{}),ClosedError);
    fixture.set(11,0);first.close();
    PcanTransport reopened(config,fixture.api);reopened.close();
    config=fixture.config(PcanMode::kFd);PcanTransport fd(config,fixture.api);fd.close();
    config.mode=PcanMode::kClassic;PcanTransport changed(config,fixture.api);changed.close();
    fixture.set(10,0x400);const auto before=fixture.get(2);CHECK_THROWS_AS(PcanTransport(config,fixture.api),PcanTransportError);CHECK(fixture.get(2)==before);
}
TEST_CASE("PCAN deferred close failure retains module and does not block another channel","[pcan]") {
    Fixture fixture;fixture.set(19,0x51);
    {PcanTransport first(fixture.config(),fixture.api);fixture.set(11,0x200);}
    CHECK(pending_pcan_cleanup(fixture.api)==1);
    const auto report=enumerate_pcan_channels(fixture.api);CHECK(report.channels.size()==4);CHECK_FALSE(report.failures.empty());
    PcanTransport other(fixture.config(PcanMode::kFd),fixture.api);Message msg;msg.id=1;msg.is_fd=true;other.write(msg);REQUIRE(other.read());other.close();
    CHECK(pending_pcan_cleanup(fixture.api)==1);fixture.set(11,0);retry_pcan_cleanup(fixture.api);CHECK(pending_pcan_cleanup(fixture.api)==0);
    PcanTransport recovered(fixture.config(),fixture.api);
}
TEST_CASE("PCAN caution rollback owns only the channel it initialized","[pcan]") {
    Fixture fixture;fixture.set(10,0x2000000);fixture.set(11,0x200);
    CHECK_THROWS_AS(PcanTransport(fixture.config(),fixture.api),PcanTransportError);
    CHECK(fixture.get(2)==1);CHECK(pending_pcan_cleanup(fixture.api)==1);
    fixture.set(10,0);fixture.set(11,0);PcanTransport recovered(fixture.config(),fixture.api);CHECK(pending_pcan_cleanup(fixture.api)==0);
}
TEST_CASE("PCAN close interrupts empty polling and concurrent callers are idempotent","[pcan]") {
    Fixture fixture;auto config=fixture.config();config.receive_timeout=std::chrono::milliseconds(100);PcanTransport transport(config,fixture.api);
    auto reading=std::async(std::launch::async,[&]{try{(void)transport.read();}catch(const ClosedError&){} });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!fixture.get(3)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    CHECK(fixture.get(3)>0);
    const auto before=std::chrono::steady_clock::now();
    auto a=std::async(std::launch::async,[&]{transport.close();});auto b=std::async(std::launch::async,[&]{transport.close();});a.get();b.get();reading.get();
    CHECK(std::chrono::steady_clock::now()-before<std::chrono::seconds(1));CHECK(fixture.get(2)==1);
}
TEST_CASE("PCAN channel names and exact FD timing validate without I O","[pcan]") {
    for(const auto handle:{uint16_t(0x51),uint16_t(0x509),uint16_t(0x41),uint16_t(0x801)})CHECK(pcan_channel_handle(pcan_channel_name(handle))==handle);
    for(const auto bad:{"PCAN_USBBUS0","PCAN_USBBUS17","PCAN_USBBUS-1","0","-1","0x10000"," 81"})CHECK_THROWS_AS(pcan_channel_handle(bad),ValidationError);
    PcanFdTiming timing;CHECK(timing.bitrate_string(1000000,5000000).find("data_tseg1=11")!=std::string::npos);
    timing.clock_hz=0;CHECK_THROWS_AS(timing.bitrate_string(1000000,5000000),ValidationError);
}
TEST_CASE("PCAN explicit Unicode module path and missing symbol do not fall back","[pcan]") {
    const auto directory=fs::temp_directory_path()/fs::u8path("PCAN 测试 with spaces "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    struct Cleanup{fs::path p;~Cleanup(){std::error_code error;fs::remove_all(p,error);}} cleanup{directory};
    const auto filename=directory/fs::u8path("PCAN 测试"+fs::path(path()).extension().string());fs::copy_file(fs::u8path(path()),filename);
    auto api=load_pcan_api(filename.u8string());CHECK(fs::equivalent(fs::u8path(api->library_path),filename));
    CHECK_THROWS_AS(load_pcan_api((directory/"missing.dll").u8string()),TransportIOError);
    CHECK_THROWS_AS(load_pcan_api(path("LINKERBOT_TEST_PCAN_MISSING")),TransportIOError);
    CHECK_THROWS_AS(load_pcan_api(std::string{}),ValidationError);
}
