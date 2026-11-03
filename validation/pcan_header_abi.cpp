// Compile-only optional comparison against a separately obtained official
// PCANBasic.h. Never redistribute the vendor header with this repository.
// Define PCAN_REFERENCE_HEADER to the quoted absolute path of that header.
#include <cstddef>
#include <cstdint>
#include <type_traits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define PCAN_REFERENCE_CALL __stdcall
#else
// Compile layout/signature checks on Linux with exact Windows-width types.
// This does not execute or validate Windows calling conventions.
using WORD = uint16_t;
using DWORD = uint32_t;
using BYTE = uint8_t;
using UINT64 = uint64_t;
using LPSTR = char *;
#define __stdcall
#define PCAN_REFERENCE_CALL
#endif
#ifndef PCAN_REFERENCE_HEADER
#error Define PCAN_REFERENCE_HEADER to the official PCANBasic.h path
#endif
#include PCAN_REFERENCE_HEADER
#include <linkerbot/can/pcan_transport.hpp>
namespace abi = linkerbot::can::pcan_abi;
static_assert(sizeof(TPCANMsg) == sizeof(abi::ClassicMessage));
static_assert(sizeof(TPCANMsgFD) == sizeof(abi::FdMessage));
static_assert(sizeof(TPCANChannelInformation) == sizeof(abi::ChannelInformation));
static_assert(sizeof(TPCANTimestamp) == sizeof(abi::Timestamp));
static_assert(sizeof(TPCANTimestampFD) == sizeof(abi::FdTimestamp));
static_assert(offsetof(TPCANMsg, DATA) == offsetof(abi::ClassicMessage, data));
static_assert(offsetof(TPCANMsgFD, DATA) == offsetof(abi::FdMessage, data));
static_assert(offsetof(TPCANChannelInformation, device_features) ==
              offsetof(abi::ChannelInformation, features));
static_assert(offsetof(TPCANChannelInformation, device_name) ==
              offsetof(abi::ChannelInformation, device_name));
static_assert(offsetof(TPCANChannelInformation, device_id) ==
              offsetof(abi::ChannelInformation, device_id));
static_assert(offsetof(TPCANChannelInformation, channel_condition) ==
              offsetof(abi::ChannelInformation, condition));
static_assert(PCAN_CHANNEL_FEATURES == abi::channel_features);
static_assert(PCAN_CHANNEL_CONDITION == abi::channel_condition);
static_assert(PCAN_ATTACHED_CHANNELS == abi::attached_channels);
static_assert(PCAN_ATTACHED_CHANNELS_COUNT == abi::attached_count);
static_assert(std::is_same_v<decltype(&CAN_Initialize),
                             abi::Status(PCAN_REFERENCE_CALL *)(uint16_t, uint16_t, uint8_t,
                                                                uint32_t, uint16_t)>);
static_assert(std::is_same_v<decltype(&CAN_InitializeFD),
                             abi::Status(PCAN_REFERENCE_CALL *)(uint16_t, char *)>);
static_assert(
    std::is_same_v<decltype(&CAN_Uninitialize), abi::Status(PCAN_REFERENCE_CALL *)(uint16_t)>);
static_assert(std::is_same_v<decltype(&CAN_GetValue), abi::Status(PCAN_REFERENCE_CALL *)(
                                                          uint16_t, uint8_t, void *, uint32_t)>);
static_assert(std::is_same_v<decltype(&CAN_Read), abi::Status(PCAN_REFERENCE_CALL *)(
                                                      uint16_t, TPCANMsg *, TPCANTimestamp *)>);
static_assert(
    std::is_same_v<decltype(&CAN_Write), abi::Status(PCAN_REFERENCE_CALL *)(uint16_t, TPCANMsg *)>);
static_assert(
    std::is_same_v<decltype(&CAN_ReadFD),
                   abi::Status(PCAN_REFERENCE_CALL *)(uint16_t, TPCANMsgFD *, TPCANTimestampFD *)>);
static_assert(std::is_same_v<decltype(&CAN_WriteFD),
                             abi::Status(PCAN_REFERENCE_CALL *)(uint16_t, TPCANMsgFD *)>);
