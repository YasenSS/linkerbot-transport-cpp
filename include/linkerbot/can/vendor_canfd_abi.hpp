#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace linkerbot::can {

    // Vendor libcanbus / HCanbus C ABI layouts (natural alignment, matching
    // Python ctypes.Structure without _pack_).
    //
    // Field names deliberately keep the vendor's CamelCase so they can be diffed
    // against the vendor header; this is the one place in the repo that does.
    // NOLINTBEGIN(readability-identifier-naming, cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

    struct CanFDConfig {
        uint32_t NomBaud = 0;
        uint32_t DatBaud = 0;
        uint16_t NomPre = 0;
        uint8_t NomTseg1 = 0;
        uint8_t NomTseg2 = 0;
        uint8_t NomSJW = 0;
        uint8_t DatPre = 0;
        uint8_t DatTseg1 = 0;
        uint8_t DatTseg2 = 0;
        uint8_t DatSJW = 0;
        uint8_t Config = 0;
        uint8_t Model = 0;
        uint8_t Cantype = 0;
    };

    struct CanFDMsg {
        uint32_t ID = 0;
        uint32_t TimeStamp = 0;
        uint8_t FrameType = 0;
        uint8_t DLC = 0; // wire DLC 0–15 (not byte length)
        uint8_t ExternFlag = 0;
        uint8_t RemoteFlag = 0;
        uint8_t BusSatus = 0;
        uint8_t ErrSatus = 0;
        uint8_t TECounter = 0;
        uint8_t RECounter = 0;
        uint8_t Data[64] = {};
    };

    // V1.3 layouts, corroborated by the x64 HCanbus sample recorded in
    // docs/zh/minicanfd.md. Do not change packing globally.
    static_assert(sizeof(CanFDConfig) == 20 && alignof(CanFDConfig) == 4);
    static_assert(sizeof(int) == 4 && sizeof(unsigned) == 4);
    static_assert(std::is_standard_layout_v<CanFDConfig> && std::is_trivially_copyable_v<CanFDConfig>);
    static_assert(std::is_standard_layout_v<CanFDMsg> && std::is_trivially_copyable_v<CanFDMsg>);
    static_assert(offsetof(CanFDConfig, NomBaud) == 0 && offsetof(CanFDConfig, DatBaud) == 4);
    static_assert(offsetof(CanFDConfig, NomPre) == 8);
    static_assert(offsetof(CanFDConfig, NomTseg1) == 10 && offsetof(CanFDConfig, NomTseg2) == 11);
    static_assert(offsetof(CanFDConfig, NomSJW) == 12 && offsetof(CanFDConfig, DatPre) == 13);
    static_assert(offsetof(CanFDConfig, DatTseg1) == 14 && offsetof(CanFDConfig, DatTseg2) == 15);
    static_assert(offsetof(CanFDConfig, DatSJW) == 16);
    static_assert(offsetof(CanFDConfig, Config) == 17);
    static_assert(offsetof(CanFDConfig, Model) == 18);
    static_assert(offsetof(CanFDConfig, Cantype) == 19);
    static_assert(sizeof(CanFDMsg) == 80 && alignof(CanFDMsg) == 4);
    static_assert(offsetof(CanFDMsg, ID) == 0 && offsetof(CanFDMsg, TimeStamp) == 4);
    static_assert(offsetof(CanFDMsg, FrameType) == 8);
    static_assert(offsetof(CanFDMsg, DLC) == 9);
    static_assert(offsetof(CanFDMsg, ExternFlag) == 10);
    static_assert(offsetof(CanFDMsg, RemoteFlag) == 11 && offsetof(CanFDMsg, BusSatus) == 12);
    static_assert(offsetof(CanFDMsg, ErrSatus) == 13 && offsetof(CanFDMsg, TECounter) == 14);
    static_assert(offsetof(CanFDMsg, RECounter) == 15);
    static_assert(offsetof(CanFDMsg, Data) == 16);

    using FnLibCANbusInit = int (*)();
    using FnLibCANbusExit = int (*)();
    using FnCANScanDevice = int (*)();
    using FnCANOpenDevice = int (*)(unsigned device, unsigned channel);
    using FnCANCloseDevice = int (*)(unsigned device, unsigned channel);
    using FnCANFDInit = int (*)(unsigned device, unsigned channel, CanFDConfig *config);
    using FnCANFDTransmit = int (*)(unsigned device, unsigned channel, CanFDMsg *msgs,
                                    unsigned count, int timeout_ms);
    using FnCANFDReceive = int (*)(unsigned device, unsigned channel, CanFDMsg *msgs,
                                   unsigned max_frames, int timeout_ms);

    // Resolved vendor entry points. Production code fills this via dlopen;
    // unit tests inject a mock table.
    struct VendorCanFdApi {
        // Platform-neutral adapter calls, NOT pointers into the native DLL.
        // The loader wraps separately typed Windows/Linux entry points.
        // Existing injectable Linux-shaped function tables remain usable.
        std::function<int()> lib_init; // optional, always absent on Windows
        std::function<int()> lib_exit;
        std::function<int()> scan_device;
        std::function<int(unsigned, unsigned)> open_device;
        std::function<int(unsigned, unsigned)> close_device;
        std::function<int(unsigned, unsigned, CanFDConfig *)> canfd_init;
        std::function<int(unsigned, unsigned, CanFDMsg *, unsigned, int)> transmit;
        std::function<int(unsigned, unsigned, CanFDMsg *, unsigned, int)> receive;
        std::function<int(unsigned, unsigned)> channel_type;
        bool native_channel_numbers = false; // Windows devNum, channel must be 0
        bool device_level_type = false; // legacy Linux type query only verifies channel 0

        // Stable key for process-level Init/Exit refcount (realpath or mock id).
        std::string lifecycle_key;

        // Opaque library handle; closed by unload when the last ref drops.
        // Kept for tests that inject a raw handle/unload pair directly; real
        // loader-produced instances use library_owner instead (see below) so
        // vendor_canfd_lifecycle::release() must not call unload() when
        // library_owner is set, to avoid unloading a handle it does not own.
        void *library_handle = nullptr;
        void (*unload)(void *library_handle) = nullptr;

        // RAII ownership of a real loaded module, one owner per successful
        // load_vendor_canfd_api() call. This is deliberately independent of
        // the process-wide LibCANbus_Init/Exit refcount in
        // vendor_canfd_lifecycle: several VendorCanFdApi instances can share
        // one lifecycle_key (and thus one Init/Exit pair) while each still
        // dlopen'd (and must dlclose) its own handle. Tying the deleter to
        // this specific object's lifetime, rather than to whichever single
        // instance the lifecycle table happens to retain, is what makes every
        // load's handle actually get released.
        std::shared_ptr<void> library_owner;
    };

    // NOLINTEND(readability-identifier-naming, cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

} // namespace linkerbot::can
