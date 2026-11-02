#include <linkerbot/can/vendor_canfd_transport.hpp>

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace linkerbot::can {
    namespace {
#if defined(_WIN32)
        [[noreturn]] void windows_error(const std::string &operation, DWORD status) {
            throw TransportIOError(
                operation + " (Windows error " + std::to_string(status) + ")",
                std::error_code(static_cast<int>(status), std::system_category()),
                static_cast<int>(status));
        }
        std::wstring widen(const std::string &text) {
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                  static_cast<int>(text.size()), nullptr, 0);
            if (!count)
                windows_error("Invalid UTF-8 vendor library path", GetLastError());
            std::wstring result(static_cast<size_t>(count), L'\0');
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), result.data(), count))
                windows_error("Convert vendor library path", GetLastError());
            return result;
        }
        std::string utf8(const std::wstring &text) {
            const int count =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!count)
                windows_error("Encode vendor module path", GetLastError());
            std::string result(static_cast<size_t>(count), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), result.data(), count, nullptr,
                                     nullptr))
                windows_error("Encode vendor module path", GetLastError());
            return result;
        }
        std::optional<std::string> environment_path() {
            const DWORD size = GetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", nullptr, 0);
            if (!size)
                return std::nullopt;
            std::wstring buffer(size, L'\0');
            const DWORD length =
                GetEnvironmentVariableW(L"LINKERBOT_CANFD_LIB", buffer.data(), size);
            if (!length || length >= size)
                windows_error("Read LINKERBOT_CANFD_LIB", GetLastError());
            buffer.resize(length);
            return utf8(buffer);
        }
        void *symbol(void *handle, const char *name) {
            return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), name));
        }
        std::string module_path(void *handle) {
            for (DWORD capacity = 512; capacity <= 32768; capacity *= 2) {
                std::wstring buffer(capacity, L'\0');
                const DWORD size =
                    GetModuleFileNameW(static_cast<HMODULE>(handle), buffer.data(), capacity);
                if (!size)
                    windows_error("GetModuleFileNameW", GetLastError());
                if (size < capacity) {
                    buffer.resize(size);
                    return utf8(buffer);
                }
            }
            windows_error("Vendor module path exceeds 32768 characters", ERROR_INSUFFICIENT_BUFFER);
        }
        std::shared_ptr<void> open_library(const std::string &candidate, bool explicit_path) {
            auto path = widen(candidate);
            DLL_DIRECTORY_COOKIE directory = nullptr;
            DWORD flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
            if (explicit_path) {
                path = std::filesystem::absolute(std::filesystem::path(path))
                           .lexically_normal()
                           .wstring();
                const auto parent = std::filesystem::path(path).parent_path().wstring();
                directory = AddDllDirectory(parent.c_str());
                if (!directory)
                    windows_error("AddDllDirectory for " + candidate, GetLastError());
                flags |= LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
            }
            HMODULE handle = LoadLibraryExW(path.c_str(), nullptr, flags);
            if (!handle) {
                const DWORD status = GetLastError();
                if (directory)
                    RemoveDllDirectory(directory);
                windows_error("LoadLibraryExW " + utf8(path), status);
            }
            return std::shared_ptr<void>(handle, [directory](void *module) {
                FreeLibrary(static_cast<HMODULE>(module));
                if (directory)
                    RemoveDllDirectory(directory);
            });
        }
#else
        std::optional<std::string> environment_path() {
            if (const char *value = std::getenv(kVendorCanFdLibEnv); value && value[0])
                return value;
            return std::nullopt;
        }
        void *symbol(void *handle, const char *name) {
            return dlsym(handle, name);
        }
        std::shared_ptr<void> open_library(const std::string &candidate, bool) {
            void *handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                const char *error = dlerror();
                throw TransportIOError("dlopen " + candidate + ": " +
                                       (error ? error : "unknown loader error"));
            }
            return std::shared_ptr<void>(handle, [](void *module) { dlclose(module); });
        }
        std::string module_path(void *scan_symbol) {
            Dl_info info{};
            if (dladdr(scan_symbol, &info) == 0 || !info.dli_fname)
                throw TransportIOError(
                    "Cannot resolve vendor module path; supply an absolute library_path");
            return std::filesystem::canonical(info.dli_fname).string();
        }
#endif

        template <typename Fn> Fn optional_symbol(void *handle, const char *name) {
            return reinterpret_cast<Fn>(symbol(handle, name));
        }
        template <typename Fn>
        Fn required_symbol(void *handle, const char *name, const std::string &path) {
            auto result = optional_symbol<Fn>(handle, name);
            if (!result) {
#if defined(_WIN32)
                const DWORD status = GetLastError();
                windows_error(path + " missing symbol " + name, status);
#else
                throw TransportIOError(path + " missing symbol " + name);
#endif
            }
            return result;
        }

        std::shared_ptr<VendorCanFdApi> bind_library(std::shared_ptr<void> owner) {
            auto api = std::make_shared<VendorCanFdApi>();
            void *handle = owner.get();
#if defined(_WIN32)
            api->lifecycle_key = module_path(handle);
#else
            api->lifecycle_key = module_path(symbol(handle, "CAN_ScanDevice"));
#endif
            const auto &path = api->lifecycle_key;
#if defined(_WIN32)
            // HCanbus Windows ABI: devNum is a native channel number, not device*2+ch.
            using Open = int(__stdcall *)(unsigned);
            using Init = int(__stdcall *)(unsigned, CanFDConfig *);
            using IO = int(__stdcall *)(unsigned, CanFDMsg *, unsigned, int);
            using Type = int(__stdcall *)(unsigned);
            using Scan = int(__stdcall *)();
            api->scan_device = required_symbol<Scan>(handle, "CAN_ScanDevice", path);
            const auto open = required_symbol<Open>(handle, "CAN_OpenDevice", path);
            const auto close = required_symbol<Open>(handle, "CAN_CloseDevice", path);
            const auto init = required_symbol<Init>(handle, "CANFD_Init", path);
            const auto tx = required_symbol<IO>(handle, "CANFD_Transmit", path);
            const auto rx = required_symbol<IO>(handle, "CANFD_Receive", path);
            auto type = optional_symbol<Type>(handle, "CAN_GetDevType");
            if (!type)
                type = required_symbol<Type>(handle, "CAN_GeDevType", path);
            const auto check = [](unsigned channel) {
                if (channel != 0)
                    throw ValidationError("Windows MiniCANFD channel_index must be 0");
            };
            api->open_device = [open, check](unsigned dev, unsigned ch) {
                check(ch);
                return open(dev);
            };
            api->close_device = [close, check](unsigned dev, unsigned ch) {
                check(ch);
                return close(dev);
            };
            api->canfd_init = [init, check](unsigned dev, unsigned ch, CanFDConfig *cfg) {
                check(ch);
                return init(dev, cfg);
            };
            api->transmit = [tx, check](unsigned dev, unsigned ch, CanFDMsg *msgs, unsigned n,
                                        int ms) {
                check(ch);
                return tx(dev, msgs, n, ms);
            };
            api->receive = [rx, check](unsigned dev, unsigned ch, CanFDMsg *msgs, unsigned n,
                                       int ms) {
                check(ch);
                return rx(dev, msgs, n, ms);
            };
            api->channel_type = [type, check](unsigned dev, unsigned ch) {
                check(ch);
                return type(dev);
            };
            api->native_channel_numbers = true;
            // No Windows LibCANbus_Init/Exit binding: this DLL owns its lifecycle.
#else
            api->scan_device = required_symbol<FnCANScanDevice>(handle, "CAN_ScanDevice", path);
            api->lib_init = optional_symbol<FnLibCANbusInit>(handle, "LibCANbus_Init");
            api->lib_exit = optional_symbol<FnLibCANbusExit>(handle, "LibCANbus_Exit");
            if (static_cast<bool>(api->lib_init) != static_cast<bool>(api->lib_exit))
                throw TransportIOError(path + " exports only one of LibCANbus_Init/Exit");
            api->open_device = required_symbol<FnCANOpenDevice>(handle, "CAN_OpenDevice", path);
            api->close_device = required_symbol<FnCANCloseDevice>(handle, "CAN_CloseDevice", path);
            api->canfd_init = required_symbol<FnCANFDInit>(handle, "CANFD_Init", path);
            api->transmit = required_symbol<FnCANFDTransmit>(handle, "CANFD_Transmit", path);
            api->receive = required_symbol<FnCANFDReceive>(handle, "CANFD_Receive", path);
            if (api->lib_init) {
                api->channel_type =
                    optional_symbol<int (*)(unsigned, unsigned)>(handle, "CAN_GetCHType");
            } else {
                const auto type = optional_symbol<int (*)(unsigned)>(handle, "CAN_GetDevType");
                if (type) {
                    api->channel_type = [type](unsigned dev, unsigned ch) {
                        if (ch != 0)
                            throw ValidationError(
                                "Device-level type query only verifies channel 0");
                        return type(dev);
                    };
                    api->device_level_type = true;
                }
            }
#endif
            api->library_owner = std::move(owner);
            return api;
        }
    } // namespace

    auto load_vendor_canfd_api(const std::optional<std::string> &library_path)
        -> std::shared_ptr<VendorCanFdApi> {
        auto explicit_path = library_path ? library_path : environment_path();
        if (explicit_path) {
            if (explicit_path->empty() || explicit_path->find('\0') != std::string::npos)
                throw ValidationError("Vendor library path must be non-empty and contain no NUL");
                // Windows converts strict UTF-8 before resolving its absolute wide
                // path, so invalid encoding retains the native conversion error.
#if defined(_WIN32)
            return bind_library(open_library(*explicit_path, true));
#else
            // Explicit names are paths, not search hints.
            const auto path =
                std::filesystem::absolute(std::filesystem::u8path(*explicit_path)).u8string();
            return bind_library(open_library(path, true));
#endif
        }
#if defined(_WIN32)
        return bind_library(open_library("HCanbus.dll", false));
#else
        std::string errors;
        for (const auto &candidate :
             {std::string("libcanbus.so"), std::string("/usr/local/lib/libcanbus.so"),
              (std::filesystem::current_path() / "libcanbus.so").string()}) {
            std::shared_ptr<void> owner;
            try {
                owner = open_library(candidate, false);
            } catch (const TransportIOError &error) {
                errors += std::string(error.what()) + '\n';
                continue;
            }
            // Once a module loads, an ABI error is fatal, not an excuse to swap it.
            return bind_library(std::move(owner));
        }
        throw TransportIOError("Unable to load CANFD vendor library. Pass Config.library_path or "
                               "set LINKERBOT_CANFD_LIB.\n" +
                               errors);
#endif
    }
} // namespace linkerbot::can
