// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Standard definitions for SAE J2534-1 / J2534-2 Pass-Thru API.
//
// Declared here so the backend has zero compile-time dependencies on vendor
// SDKs or headers. SAE J2534 is an open public standard; any compliant Pass-Thru
// driver DLL exports these exact C functions and structures.

#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#define CALLBACK
#endif

namespace torquebus::j2534 {

// --- Return Error Codes -----------------------------------------------------

inline constexpr long STATUS_NOERROR = 0x00;
inline constexpr long ERR_NOT_SUPPORTED = 0x01;
inline constexpr long ERR_INVALID_CHANNEL_ID = 0x02;
inline constexpr long ERR_INVALID_PROTOCOL_ID = 0x03;
inline constexpr long ERR_NULLPARAMETER = 0x04;
inline constexpr long ERR_INVALID_IOCTL_VALUE = 0x05;
inline constexpr long ERR_INVALID_FLAGS = 0x06;
inline constexpr long ERR_FAILED = 0x07;
inline constexpr long ERR_DEVICE_NOT_CONNECTED = 0x08;
inline constexpr long ERR_TIMEOUT = 0x09;
inline constexpr long ERR_INVALID_MSG = 0x0A;
inline constexpr long ERR_INVALID_TIME_INTERVAL = 0x0B;
inline constexpr long ERR_EXCEEDED_LIMIT = 0x0C;
inline constexpr long ERR_INVALID_MSG_ID = 0x0D;
inline constexpr long ERR_DEVICE_IN_USE = 0x0E;
inline constexpr long ERR_INVALID_IOCTL_ID = 0x0F;
inline constexpr long ERR_BUFFER_EMPTY = 0x10;
inline constexpr long ERR_BUFFER_FULL = 0x11;
inline constexpr long ERR_BUFFER_OVERFLOW = 0x12;
inline constexpr long ERR_PIN_INVALID = 0x13;
inline constexpr long ERR_CHANNEL_IN_USE = 0x14;
inline constexpr long ERR_MSG_PROTOCOL_ID = 0x15;
inline constexpr long ERR_INVALID_FILTER_ID = 0x16;
inline constexpr long ERR_NO_FLOW_CONTROL = 0x17;
inline constexpr long ERR_NOT_UNIQUE = 0x18;
inline constexpr long ERR_UNTRAINED_ADDR = 0x19;
inline constexpr long ERR_INVALID_DEVICE_ID = 0x1A;

// --- Protocol IDs -----------------------------------------------------------

inline constexpr unsigned long PROTOCOL_J1850VPW = 0x01;
inline constexpr unsigned long PROTOCOL_J1850PWM = 0x02;
inline constexpr unsigned long PROTOCOL_ISO9141 = 0x03;
inline constexpr unsigned long PROTOCOL_ISO14230 = 0x04;
inline constexpr unsigned long PROTOCOL_CAN = 0x05;
inline constexpr unsigned long PROTOCOL_ISO15765 = 0x06;
inline constexpr unsigned long PROTOCOL_CAN_PS = 0x8005;

// --- Filter Types -----------------------------------------------------------

inline constexpr unsigned long PASS_FILTER = 0x01;
inline constexpr unsigned long BLOCK_FILTER = 0x02;
inline constexpr unsigned long FLOW_CONTROL_FILTER = 0x03;

// --- Message Flags ----------------------------------------------------------

inline constexpr unsigned long CAN_29BIT_ID = 0x00000100;
inline constexpr unsigned long ISO15765_FRAME_PAD = 0x00000040;
inline constexpr unsigned long ISO15765_ADDR_TYPE = 0x00000080;
inline constexpr unsigned long CAN_ID_BOTH = 0x00000200;
inline constexpr unsigned long TX_MSG_TYPE = 0x00000001;

// --- Ioctl IDs --------------------------------------------------------------

inline constexpr unsigned long GET_CONFIG = 0x01;
inline constexpr unsigned long SET_CONFIG = 0x02;
inline constexpr unsigned long READ_VBATT = 0x03;
inline constexpr unsigned long FIVE_BAUD_INIT = 0x04;
inline constexpr unsigned long FAST_INIT = 0x05;
inline constexpr unsigned long CLEAR_TX_BUFFER = 0x07;
inline constexpr unsigned long CLEAR_RX_BUFFER = 0x08;
inline constexpr unsigned long CLEAR_PERIODIC_MSGS = 0x09;
inline constexpr unsigned long CLEAR_MSG_FILTERS = 0x0A;
inline constexpr unsigned long CLEAR_FUNCT_MSG_LOOKUP_TABLE = 0x0B;
inline constexpr unsigned long ADD_TO_FUNCT_MSG_LOOKUP_TABLE = 0x0C;
inline constexpr unsigned long DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE = 0x0D;
inline constexpr unsigned long READ_PROG_VOLTAGE = 0x0E;

// --- Parameter IDs for SET_CONFIG / GET_CONFIG ------------------------------

inline constexpr unsigned long DATA_RATE = 0x01;
inline constexpr unsigned long LOOPBACK = 0x03;
inline constexpr unsigned long NODE_ADDRESS = 0x04;
inline constexpr unsigned long NETWORK_LINE = 0x05;
inline constexpr unsigned long BIT_SAMPLE_POINT = 0x17;
inline constexpr unsigned long SYNC_JUMP_WIDTH = 0x18;

// --- Maximum Message Data Size ----------------------------------------------

inline constexpr unsigned long kMaxPassThruDataSize = 4128;

// --- Data Structures --------------------------------------------------------

#pragma pack(push, 1)

struct PASSTHRU_MSG {
    unsigned long ProtocolID{};
    unsigned long RxStatus{};
    unsigned long TxFlags{};
    unsigned long Timestamp{};
    unsigned long DataSize{};
    unsigned long ExtraDataIndex{};
    unsigned char Data[kMaxPassThruDataSize]{};
};

struct SCONFIG {
    unsigned long Parameter{};
    unsigned long Value{};
};

struct SCONFIG_LIST {
    unsigned long NumOfParams{};
    SCONFIG* ConfigPtr{nullptr};
};

#pragma pack(pop)

// --- Function Pointer Signatures --------------------------------------------

using PtOpen = long(CALLBACK*)(void* pName, unsigned long* pDeviceID);
using PtClose = long(CALLBACK*)(unsigned long DeviceID);
using PtConnect = long(CALLBACK*)(unsigned long DeviceID,
                                  unsigned long ProtocolID,
                                  unsigned long Flags,
                                  unsigned long Baudrate,
                                  unsigned long* pChannelID);
using PtDisconnect = long(CALLBACK*)(unsigned long ChannelID);
using PtReadMsgs = long(CALLBACK*)(unsigned long ChannelID,
                                   PASSTHRU_MSG* pMsg,
                                   unsigned long* pNumMsgs,
                                   unsigned long Timeout);
using PtWriteMsgs = long(CALLBACK*)(unsigned long ChannelID,
                                    PASSTHRU_MSG* pMsg,
                                    unsigned long* pNumMsgs,
                                    unsigned long Timeout);
using PtStartMsgFilter = long(CALLBACK*)(unsigned long ChannelID,
                                         unsigned long FilterType,
                                         PASSTHRU_MSG* pMaskMsg,
                                         PASSTHRU_MSG* pPatternMsg,
                                         PASSTHRU_MSG* pFlowControlMsg,
                                         unsigned long* pFilterID);
using PtStopMsgFilter = long(CALLBACK*)(unsigned long ChannelID, unsigned long FilterID);
using PtSetProgVoltage =
    long(CALLBACK*)(unsigned long DeviceID, unsigned long PinNumber, unsigned long Voltage);
using PtReadVersion = long(CALLBACK*)(unsigned long DeviceID,
                                      char* pFirmwareVersion,
                                      char* pDllVersion,
                                      char* pApiVersion);
using PtGetLastError = long(CALLBACK*)(char* pErrorDescription);
using PtIoctl = long(CALLBACK*)(unsigned long ChannelID,
                                unsigned long IoctlID,
                                void* pInput,
                                void* pOutput);

/// Holds resolved function pointers of a loaded J2534 DLL.
struct J2534FunctionTable {
    PtOpen PassThruOpen{nullptr};
    PtClose PassThruClose{nullptr};
    PtConnect PassThruConnect{nullptr};
    PtDisconnect PassThruDisconnect{nullptr};
    PtReadMsgs PassThruReadMsgs{nullptr};
    PtWriteMsgs PassThruWriteMsgs{nullptr};
    PtStartMsgFilter PassThruStartMsgFilter{nullptr};
    PtStopMsgFilter PassThruStopMsgFilter{nullptr};
    PtSetProgVoltage PassThruSetProgVoltage{nullptr};
    PtReadVersion PassThruReadVersion{nullptr};
    PtGetLastError PassThruGetLastError{nullptr};
    PtIoctl PassThruIoctl{nullptr};

    [[nodiscard]] bool isValid() const noexcept
    {
        return PassThruOpen != nullptr && PassThruClose != nullptr && PassThruConnect != nullptr
            && PassThruDisconnect != nullptr && PassThruReadMsgs != nullptr
            && PassThruWriteMsgs != nullptr && PassThruStartMsgFilter != nullptr
            && PassThruStopMsgFilter != nullptr;
    }
};

/// Translates a J2534 error code to a readable string.
[[nodiscard]] constexpr const char* errorString(long errorCode) noexcept
{
    switch (errorCode) {
    case STATUS_NOERROR: return "No error";
    case ERR_NOT_SUPPORTED: return "Function not supported";
    case ERR_INVALID_CHANNEL_ID: return "Invalid channel ID";
    case ERR_INVALID_PROTOCOL_ID: return "Invalid protocol ID";
    case ERR_NULLPARAMETER: return "Null parameter";
    case ERR_INVALID_IOCTL_VALUE: return "Invalid IOCTL value";
    case ERR_INVALID_FLAGS: return "Invalid flags";
    case ERR_FAILED: return "Operation failed";
    case ERR_DEVICE_NOT_CONNECTED: return "Device not connected";
    case ERR_TIMEOUT: return "Operation timed out";
    case ERR_INVALID_MSG: return "Invalid message";
    case ERR_INVALID_TIME_INTERVAL: return "Invalid time interval";
    case ERR_EXCEEDED_LIMIT: return "Exceeded limit";
    case ERR_INVALID_MSG_ID: return "Invalid message ID";
    case ERR_DEVICE_IN_USE: return "Device in use";
    case ERR_INVALID_IOCTL_ID: return "Invalid IOCTL ID";
    case ERR_BUFFER_EMPTY: return "Buffer empty";
    case ERR_BUFFER_FULL: return "Buffer full";
    case ERR_BUFFER_OVERFLOW: return "Buffer overflow";
    case ERR_PIN_INVALID: return "Invalid pin";
    case ERR_CHANNEL_IN_USE: return "Channel in use";
    case ERR_MSG_PROTOCOL_ID: return "Message protocol mismatch";
    case ERR_INVALID_FILTER_ID: return "Invalid filter ID";
    case ERR_NO_FLOW_CONTROL: return "No flow control filter";
    case ERR_NOT_UNIQUE: return "Not unique";
    case ERR_UNTRAINED_ADDR: return "Untrained address";
    case ERR_INVALID_DEVICE_ID: return "Invalid device ID";
    default: return "Unknown J2534 error";
    }
}

} // namespace torquebus::j2534
