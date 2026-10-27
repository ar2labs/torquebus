// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "plugins/driver-j2534/J2534CanBackend.h"

#include "core/ThreadGuard.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace torquebus {

namespace {

#ifdef _WIN32
std::wstring utf8ToWide(const std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}
#endif

std::uint64_t currentTimestampNs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

J2534CanBackend::J2534CanBackend() = default;

J2534CanBackend::~J2534CanBackend()
{
    close();
}

void J2534CanBackend::setFunctionTableForTesting(const j2534::J2534FunctionTable& table,
                                                std::string mockDeviceName)
{
    m_api = table;
    m_mockDeviceName = std::move(mockDeviceName);
    m_usingMock = true;
}

bool J2534CanBackend::isAvailable() const noexcept
{
    if (m_usingMock) {
        return m_api.isValid();
    }
    const auto devices = j2534::enumerateJ2534Devices();
    return !devices.empty();
}

CanDeviceInfoList J2534CanBackend::enumerate()
{
    CanDeviceInfoList result;

    if (m_usingMock) {
        CanDeviceInfo info;
        info.handle = "j2534:0";
        info.backend = "j2534";
        info.name = m_mockDeviceName.empty() ? "Mock J2534 Pass-Thru" : m_mockDeviceName;
        info.channelIndex = 0;
        info.capabilities = capabilities();
        result.push_back(std::move(info));
        return result;
    }

    m_cachedDevices = j2534::enumerateJ2534Devices();

    for (std::size_t i = 0; i < m_cachedDevices.size(); ++i) {
        const auto& dev = m_cachedDevices[i];
        CanDeviceInfo info;
        info.handle = "j2534:" + std::to_string(i);
        info.backend = "j2534";
        info.name = dev.name.empty() ? ("J2534 Device " + std::to_string(i)) : dev.name;
        info.channelIndex = static_cast<std::uint32_t>(i);
        info.capabilities = capabilities();
        result.push_back(std::move(info));
    }

    return result;
}

bool J2534CanBackend::msgToCanFrame(const j2534::PASSTHRU_MSG& msg,
                                    CanFrame& outFrame,
                                    std::uint64_t fallbackTimestampNs) noexcept
{
    if (msg.DataSize < 4U) {
        return false;
    }

    const bool isExt = (msg.RxStatus & j2534::CAN_29BIT_ID) != 0;
    outFrame.format = isExt ? CanFrameFormat::Extended : CanFrameFormat::Standard;

    const std::uint32_t rawId = (static_cast<std::uint32_t>(msg.Data[0]) << 24)
        | (static_cast<std::uint32_t>(msg.Data[1]) << 16)
        | (static_cast<std::uint32_t>(msg.Data[2]) << 8)
        | static_cast<std::uint32_t>(msg.Data[3]);

    outFrame.identifier = isExt ? (rawId & 0x1FFFFFFF) : (rawId & 0x7FF);

    const unsigned long payloadLen = msg.DataSize - 4U;
    outFrame.length = static_cast<std::uint8_t>(
        std::min<unsigned long>(payloadLen, static_cast<unsigned long>(kMaxClassicCanPayload)));
    outFrame.dlc = outFrame.length;

    if (outFrame.length > 0) {
        std::memcpy(outFrame.data.data(), &msg.Data[4], outFrame.length);
    }

    outFrame.direction = CanDirection::Rx;
    outFrame.fd = false;
    outFrame.brs = false;
    outFrame.esi = false;
    outFrame.rtr = false;
    outFrame.error = false;

    if (msg.Timestamp > 0) {
        outFrame.timestampNs = static_cast<std::uint64_t>(msg.Timestamp) * 1000ULL;
    } else {
        outFrame.timestampNs = fallbackTimestampNs > 0 ? fallbackTimestampNs : currentTimestampNs();
    }

    return true;
}

bool J2534CanBackend::canFrameToMsg(const CanFrame& frame, j2534::PASSTHRU_MSG& outMsg) noexcept
{
    outMsg = j2534::PASSTHRU_MSG{};
    outMsg.ProtocolID = j2534::PROTOCOL_CAN;
    outMsg.TxFlags = frame.isExtended() ? j2534::CAN_29BIT_ID : 0;

    const std::size_t payloadSize = frame.payloadSize();
    outMsg.DataSize = static_cast<unsigned long>(4U + payloadSize);

    outMsg.Data[0] = static_cast<unsigned char>((frame.identifier >> 24) & 0xFF);
    outMsg.Data[1] = static_cast<unsigned char>((frame.identifier >> 16) & 0xFF);
    outMsg.Data[2] = static_cast<unsigned char>((frame.identifier >> 8) & 0xFF);
    outMsg.Data[3] = static_cast<unsigned char>(frame.identifier & 0xFF);

    if (payloadSize > 0) {
        std::memcpy(&outMsg.Data[4], frame.data.data(), payloadSize);
    }

    return true;
}

Result J2534CanBackend::open(const CanChannelConfig& config)
{
    if (m_open) {
        return Result::error(ErrorCode::InvalidState, "J2534: channel is already open");
    }

    if (!m_usingMock) {
#ifdef _WIN32
        if (m_cachedDevices.empty()) {
            m_cachedDevices = j2534::enumerateJ2534Devices();
        }

        std::size_t deviceIndex = 0;
        if (config.deviceHandle.rfind("j2534:", 0) == 0) {
            try {
                deviceIndex = std::stoul(config.deviceHandle.substr(6));
            } catch (...) {
                deviceIndex = 0;
            }
        }

        if (deviceIndex >= m_cachedDevices.size()) {
            return Result::error(ErrorCode::DeviceNotFound,
                                 "J2534: device index " + std::to_string(deviceIndex)
                                     + " not found in Windows registry");
        }

        const auto& targetDev = m_cachedDevices[deviceIndex];
        const std::wstring widePath = utf8ToWide(targetDev.libraryPath);

        HMODULE mod = LoadLibraryW(widePath.c_str());
        if (!mod) {
            return Result::error(ErrorCode::BackendUnavailable,
                                 "J2534: failed to load driver library " + targetDev.libraryPath);
        }

        m_module = mod;
        m_api.PassThruOpen = reinterpret_cast<j2534::PtOpen>(GetProcAddress(mod, "PassThruOpen"));
        m_api.PassThruClose = reinterpret_cast<j2534::PtClose>(GetProcAddress(mod, "PassThruClose"));
        m_api.PassThruConnect =
            reinterpret_cast<j2534::PtConnect>(GetProcAddress(mod, "PassThruConnect"));
        m_api.PassThruDisconnect =
            reinterpret_cast<j2534::PtDisconnect>(GetProcAddress(mod, "PassThruDisconnect"));
        m_api.PassThruReadMsgs =
            reinterpret_cast<j2534::PtReadMsgs>(GetProcAddress(mod, "PassThruReadMsgs"));
        m_api.PassThruWriteMsgs =
            reinterpret_cast<j2534::PtWriteMsgs>(GetProcAddress(mod, "PassThruWriteMsgs"));
        m_api.PassThruStartMsgFilter = reinterpret_cast<j2534::PtStartMsgFilter>(
            GetProcAddress(mod, "PassThruStartMsgFilter"));
        m_api.PassThruStopMsgFilter =
            reinterpret_cast<j2534::PtStopMsgFilter>(GetProcAddress(mod, "PassThruStopMsgFilter"));
        m_api.PassThruSetProgVoltage = reinterpret_cast<j2534::PtSetProgVoltage>(
            GetProcAddress(mod, "PassThruSetProgVoltage"));
        m_api.PassThruReadVersion =
            reinterpret_cast<j2534::PtReadVersion>(GetProcAddress(mod, "PassThruReadVersion"));
        m_api.PassThruGetLastError =
            reinterpret_cast<j2534::PtGetLastError>(GetProcAddress(mod, "PassThruGetLastError"));
        m_api.PassThruIoctl = reinterpret_cast<j2534::PtIoctl>(GetProcAddress(mod, "PassThruIoctl"));

        if (!m_api.isValid()) {
            FreeLibrary(mod);
            m_module = nullptr;
            return Result::error(ErrorCode::BackendUnavailable,
                                 "J2534: DLL " + targetDev.libraryPath
                                     + " does not export required PassThru entry points");
        }
#else
        return Result::error(ErrorCode::UnsupportedFeature,
                             "J2534 Pass-Thru is supported on Windows");
#endif
    }

    long status = m_api.PassThruOpen(nullptr, &m_deviceId);
    if (status != j2534::STATUS_NOERROR) {
        close();
        return Result::error(ErrorCode::Unknown,
                             "J2534: PassThruOpen failed ("
                                 + std::string(j2534::errorString(status)) + ")");
    }

    const unsigned long baudrate = config.timing.bitrate > 0 ? config.timing.bitrate : 500'000U;
    status = m_api.PassThruConnect(m_deviceId, j2534::PROTOCOL_CAN, 0, baudrate, &m_channelId);
    if (status != j2534::STATUS_NOERROR) {
        close();
        return Result::error(ErrorCode::BitTimingRejected,
                             "J2534: PassThruConnect failed for " + std::to_string(baudrate)
                                 + " bps (" + std::string(j2534::errorString(status)) + ")");
    }

    // Set a pass filter for all CAN traffic (both 11-bit and 29-bit)
    j2534::PASSTHRU_MSG maskMsg{};
    maskMsg.ProtocolID = j2534::PROTOCOL_CAN;
    maskMsg.DataSize = 4;

    j2534::PASSTHRU_MSG patternMsg{};
    patternMsg.ProtocolID = j2534::PROTOCOL_CAN;
    patternMsg.DataSize = 4;

    status = m_api.PassThruStartMsgFilter(m_channelId,
                                          j2534::PASS_FILTER,
                                          &maskMsg,
                                          &patternMsg,
                                          nullptr,
                                          &m_filterId);
    if (status != j2534::STATUS_NOERROR) {
        close();
        return Result::error(ErrorCode::Unknown,
                             "J2534: PassThruStartMsgFilter failed ("
                                 + std::string(j2534::errorString(status)) + ")");
    }

    if (m_api.PassThruIoctl) {
        m_api.PassThruIoctl(m_channelId, j2534::CLEAR_RX_BUFFER, nullptr, nullptr);
        m_api.PassThruIoctl(m_channelId, j2534::CLEAR_TX_BUFFER, nullptr, nullptr);
    }

    m_open = true;
    m_txFrames = 0;
    m_rxFrames = 0;
    m_errorFrames = 0;

    return Result::ok();
}

Result J2534CanBackend::start()
{
    if (!m_open) {
        return Result::error(ErrorCode::InvalidState, "J2534: cannot start a closed channel");
    }
    if (m_running) {
        return Result::ok();
    }

    m_running = true;
    m_rxThread = std::thread{guardThread(
        "the J2534 receive thread",
        [this](std::string_view /*reason*/) {
            m_running.store(false, std::memory_order_release);
        },
        [this] { rxWorkerLoop(); })};

    return Result::ok();
}

void J2534CanBackend::stop()
{
    if (!m_running) {
        return;
    }

    m_running = false;
    if (m_rxThread.joinable()) {
        m_rxThread.join();
    }
}

void J2534CanBackend::close()
{
    stop();

    if (m_open) {
        if (m_api.PassThruStopMsgFilter && m_channelId != 0 && m_filterId != 0) {
            m_api.PassThruStopMsgFilter(m_channelId, m_filterId);
            m_filterId = 0;
        }

        if (m_api.PassThruDisconnect && m_channelId != 0) {
            m_api.PassThruDisconnect(m_channelId);
            m_channelId = 0;
        }

        if (m_api.PassThruClose && m_deviceId != 0) {
            m_api.PassThruClose(m_deviceId);
            m_deviceId = 0;
        }

        m_open = false;
    }

#ifdef _WIN32
    if (m_module && !m_usingMock) {
        FreeLibrary(reinterpret_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
#endif
}

Result J2534CanBackend::transmit(const CanFrame& frame)
{
    if (!m_open) {
        return Result::error(ErrorCode::ChannelNotOpen, "J2534: channel is not open");
    }

    j2534::PASSTHRU_MSG msg{};
    if (!canFrameToMsg(frame, msg)) {
        return Result::error(ErrorCode::InvalidArgument, "J2534: failed to encode CAN frame");
    }

    unsigned long numMsgs = 1;
    constexpr unsigned long kTxTimeoutMs = 100;
    const long status = m_api.PassThruWriteMsgs(m_channelId, &msg, &numMsgs, kTxTimeoutMs);

    if (status != j2534::STATUS_NOERROR || numMsgs == 0) {
        return Result::error(ErrorCode::TransmitFailed,
                             "J2534: PassThruWriteMsgs failed ("
                                 + std::string(j2534::errorString(status)) + ")");
    }

    ++m_txFrames;

    // Dispatches echo so local bus trace sees what was actually transmitted
    if (m_frameHandler) {
        CanFrame echo = frame;
        echo.direction = CanDirection::Tx;
        echo.timestampNs = currentTimestampNs();
        const CanFrame batch[1] = {echo};
        m_frameHandler(std::span<const CanFrame>{batch, 1});
    }

    return Result::ok();
}

void J2534CanBackend::rxWorkerLoop()
{
    constexpr unsigned long kBatchCapacity = 32;
    j2534::PASSTHRU_MSG msgBuffer[kBatchCapacity];
    CanFrame frameBatch[kBatchCapacity];

    while (m_running) {
        unsigned long numMsgs = kBatchCapacity;
        constexpr unsigned long kRxTimeoutMs = 20;

        const long status = m_api.PassThruReadMsgs(m_channelId, msgBuffer, &numMsgs, kRxTimeoutMs);

        if (!m_running) {
            break;
        }

        if (status == j2534::STATUS_NOERROR && numMsgs > 0) {
            std::size_t validCount = 0;
            const std::uint64_t fallbackTs = currentTimestampNs();

            for (unsigned long i = 0; i < numMsgs; ++i) {
                if (msgToCanFrame(msgBuffer[i], frameBatch[validCount], fallbackTs)) {
                    ++validCount;
                    ++m_rxFrames;
                }
            }

            if (validCount > 0 && m_frameHandler) {
                m_frameHandler(std::span<const CanFrame>{frameBatch, validCount});
            }
        } else if (status != j2534::STATUS_NOERROR && status != j2534::ERR_BUFFER_EMPTY
                   && status != j2534::ERR_TIMEOUT) {
            ++m_errorFrames;
        }
    }
}

CanBusStatus J2534CanBackend::status() const
{
    CanBusStatus s;
    s.state = (m_open && m_running) ? CanBusState::ErrorActive : CanBusState::Offline;
    s.receiveErrorCounter = 0;
    s.transmitErrorCounter = 0;
    s.hardwareOverruns = m_errorFrames.load();
    s.softwareOverruns = 0;
    return s;
}

CanCapabilities J2534CanBackend::capabilities() const
{
    CanCapabilities caps;
    caps.canClassic = true;
    caps.canFd = false;
    caps.canFdBrs = false;
    caps.listenOnly = true;
    caps.hardwareTimestamp = true;
    caps.errorFrames = false;
    caps.hardwareFilters = true;
    caps.virtualDevice = false;
    caps.maxChannels = 1;
    return caps;
}

} // namespace torquebus
