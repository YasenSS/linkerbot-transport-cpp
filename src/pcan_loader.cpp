#include <cstdlib>
#include <filesystem>
#include <linkerbot/can/pcan_transport.hpp>
#include <system_error>
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define PCAN_CALL __stdcall
#else
#include <dlfcn.h>
#define PCAN_CALL
#endif

namespace linkerbot::can {
    namespace {
#if defined(_WIN32)
        [[noreturn]] void win_error(const std::string &what, DWORD code) {
            throw TransportIOError(what + " (Windows error " + std::to_string(code) + ")",
                                   {static_cast<int>(code), std::system_category()},
                                   static_cast<int>(code));
        }
        std::wstring wide(const std::string &s) {
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                                 static_cast<int>(s.size()), nullptr, 0);
            if (!size)
                win_error("PCAN library path must be UTF-8", GetLastError());
            std::wstring result(size, L'\0');
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), result.data(), size))
                win_error("PCAN path conversion", GetLastError());
            return result;
        }
        std::string narrow(const std::wstring &s) {
            const int size =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            if (!size)
                win_error("PCAN module path encoding", GetLastError());
            std::string result(size, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), result.data(), size, nullptr,
                                     nullptr))
                win_error("PCAN module path encoding", GetLastError());
            return result;
        }
#endif
        struct Module {
            std::shared_ptr<void> owner;
            std::string path;
        };
        Module open_module(std::optional<std::string> requested) {
#if defined(_WIN32)
            if (!requested) {
                const DWORD size = GetEnvironmentVariableW(L"LINKERBOT_PCAN_LIB", nullptr, 0);
                if (size) {
                    std::wstring value(size, L'\0');
                    const DWORD n =
                        GetEnvironmentVariableW(L"LINKERBOT_PCAN_LIB", value.data(), size);
                    if (!n || n >= size)
                        win_error("Read LINKERBOT_PCAN_LIB", GetLastError());
                    value.resize(n);
                    requested = narrow(value);
                }
            }
#else
            if (!requested) {
                if (auto *const value = std::getenv("LINKERBOT_PCAN_LIB");
                    (value != nullptr) && (*value != 0u)) {
                    requested = value;
                }
            }
#endif
            if (requested && (requested->empty() || requested->find('\0') != std::string::npos)) {
                throw ValidationError("PCAN library path must be nonempty and contain no NUL");
            }
#if defined(_WIN32)
            std::wstring path =
                requested ? std::filesystem::absolute(std::filesystem::path(wide(*requested)))
                                .lexically_normal()
                                .wstring()
                          : L"PCANBasic.dll";
            DLL_DIRECTORY_COOKIE cookie = nullptr;
            if (requested) {
                cookie = AddDllDirectory(std::filesystem::path(path).parent_path().c_str());
                if (!cookie)
                    win_error("PCAN AddDllDirectory", GetLastError());
            }
            const auto handle =
                LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                   (requested ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR : 0));
            if (!handle) {
                const DWORD code = GetLastError();
                if (cookie)
                    RemoveDllDirectory(cookie);
                win_error("LoadLibraryExW " + narrow(path), code);
            }
            auto owner = std::shared_ptr<void>(handle, [cookie](void *module) {
                FreeLibrary(static_cast<HMODULE>(module));
                if (cookie)
                    RemoveDllDirectory(cookie);
            });
            for (DWORD size = 512; size <= 32768; size *= 2) {
                std::wstring buffer(size, L'\0');
                const DWORD n = GetModuleFileNameW(handle, buffer.data(), size);
                if (!n)
                    win_error("PCAN GetModuleFileNameW", GetLastError());
                if (n < size) {
                    buffer.resize(n);
                    return {std::move(owner), narrow(buffer)};
                }
            }
            win_error("PCAN module path too long", ERROR_INSUFFICIENT_BUFFER);
#else
            const auto path =
                requested ? std::filesystem::absolute(std::filesystem::u8path(*requested)).string()
                          : "libpcanbasic.so";
            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr) {
                const char *error = dlerror();
                throw TransportIOError("PCAN dlopen " + path + ": " +
                                       ((error != nullptr) ? error : "unknown loader error"));
            }
            auto owner = std::shared_ptr<void>(handle, [](void *module) { dlclose(module); });
            Dl_info info{};
            if ((dladdr(dlsym(handle, "CAN_GetValue"), &info) == 0) ||
                (info.dli_fname == nullptr)) {
                throw TransportIOError(path + " missing CAN_GetValue or unresolved module path");
            }
            return {std::move(owner), std::filesystem::canonical(info.dli_fname).string()};
#endif
        }
        template <typename Fn>
        Fn bind(const Module &module, const char *name, bool required = true) {
#if defined(_WIN32)
            auto symbol = GetProcAddress(static_cast<HMODULE>(module.owner.get()), name);
            const DWORD code = symbol ? 0 : GetLastError();
            if (!symbol && required)
                win_error(module.path + " missing symbol " + name, code);
#else
            auto *symbol = dlsym(module.owner.get(), name);
            if (!symbol && required) {
                throw TransportIOError(module.path + " missing symbol " + name);
            }
#endif
            return reinterpret_cast<Fn>(symbol);
        }
    } // namespace
    auto load_pcan_api(const std::optional<std::string> &path) -> std::shared_ptr<PcanApi> {
        const auto module = open_module(path);
        auto api = std::make_shared<PcanApi>();
        using namespace pcan_abi;
        api->get_value =
            bind<Status(PCAN_CALL *)(uint16_t, uint8_t, void *, uint32_t)>(module, "CAN_GetValue");
        api->initialize =
            bind<Status(PCAN_CALL *)(uint16_t, uint16_t, uint8_t, uint32_t, uint16_t)>(
                module, "CAN_Initialize");
        api->uninitialize = bind<Status(PCAN_CALL *)(uint16_t)>(module, "CAN_Uninitialize");
        api->read =
            bind<Status(PCAN_CALL *)(uint16_t, ClassicMessage *, Timestamp *)>(module, "CAN_Read");
        api->write = bind<Status(PCAN_CALL *)(uint16_t, ClassicMessage *)>(module, "CAN_Write");
        api->initialize_fd =
            bind<Status(PCAN_CALL *)(uint16_t, char *)>(module, "CAN_InitializeFD", false);
        api->read_fd = bind<Status(PCAN_CALL *)(uint16_t, FdMessage *, FdTimestamp *)>(
            module, "CAN_ReadFD", false);
        api->write_fd =
            bind<Status(PCAN_CALL *)(uint16_t, FdMessage *)>(module, "CAN_WriteFD", false);
        api->set_value = bind<Status(PCAN_CALL *)(uint16_t, uint8_t, void *, uint32_t)>(
            module, "CAN_SetValue", false);
        api->library_path = module.path;
        api->library_owner = module.owner;
        return api;
    }
} // namespace linkerbot::can
