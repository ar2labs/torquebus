// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The only translation unit in TorqueBus that includes canlib.h.

#include "plugins/driver-kvaser/KvaserCanBackend.h"
#include "core/ThreadGuard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef TORQUEBUS_HAVE_KVASER
#include <canlib.h>
#endif

namespace torquebus {

#ifdef TORQUEBUS_HAVE_KVASER

namespace {

/// How long canReadWait blocks before returning empty-handed. Long enough that
/// an idle bus costs nothing, short enough that stop() is responsive.
constexpr unsigned long kReadTimeoutMs = 50;

/// Frames accumulated before handing a batch to the engine. The receive loop
/// also flushes whatever it has when the driver runs dry, so a quiet bus does
/// not sit on a half-full batch (rule: batching must not add latency at low
/// rates, only amortise cost at high ones).
constexpr std::size_t kBatchSize = 256;

/// Timer resolution requested from the driver, in microseconds.
///
/// CANlib hands back a 32-bit tick count, so resolution trades against how long
/// it takes to wrap: 1 us wraps after ~71 minutes, 10 us after ~12 hours. We
/// take the resolution and handle the wrap explicitly below - a measurement
/// that silently jumps backwards after an hour would be far worse than either.
constexpr unsigned int kTimerScaleMicroseconds = 1;

/// Turns a CANlib status into a message worth showing a user.
[[nodiscard]] std::string describe(canStatus status, std::string_view call)
{
    std::array<char, 128> text{};
    if (canGetErrorText(status, text.data(), static_cast<unsigned int>(text.size())) == canOK) {
        return std::format("{}: {} ({})", call, text.data(), static_cast<int>(status));
    }
    return std::format("{}: CANlib error {}", call, static_cast<int>(status));
}

[[nodiscard]] ErrorCode toErrorCode(canStatus status)
{
    switch (status) {
    case canERR_NOTFOUND:
        return ErrorCode::DeviceNotFound;
    case canERR_NOMSG:
        return ErrorCode::Timeout;
    case canERR_PARAM:
        return ErrorCode::InvalidArgument;
    case canERR_NOCHANNELS:
    case canERR_NOCARD:
        return ErrorCode::DeviceNotFound;
    case canERR_TIMEOUT:
        return ErrorCode::Timeout;
    default:
        return ErrorCode::Unknown;
    }
}

[[nodiscard]] Result toResult(canStatus status, std::string_view call)
{
    if (status == canOK) {
        return Result::ok();
    }
    return Result::error(toErrorCode(status), describe(status, call));
}

/// Maps a bitrate in bit/s onto the CANlib constant for it.
///
/// CANlib takes either one of these negative constants - in which case it picks
/// sample point and segments itself - or a real frequency together with the
/// full segment timing. Guessing segment timing on the user's behalf is how you
/// get a channel that opens cleanly and then produces error frames on a real
/// bus, so anything that is not a standard rate is refused with an explanation
/// until the Hardware Manager can ask for the timing properly (v0.3+).
[[nodiscard]] bool toCanlibBitrate(std::uint32_t bitrate, long& out)
{
    switch (bitrate) {
    case 1'000'000:
        out = canBITRATE_1M;
        return true;
    case 500'000:
        out = canBITRATE_500K;
        return true;
    case 250'000:
        out = canBITRATE_250K;
        return true;
    case 125'000:
        out = canBITRATE_125K;
        return true;
    case 100'000:
        out = canBITRATE_100K;
        return true;
    case 83'000:
        out = canBITRATE_83K;
        return true;
    case 62'000:
        out = canBITRATE_62K;
        return true;
    case 50'000:
        out = canBITRATE_50K;
        return true;
    case 10'000:
        out = canBITRATE_10K;
        return true;
    default:
        return false;
    }
}

/// Data-phase rates for CAN FD, at the 80% sample point Kvaser publishes.
[[nodiscard]] bool toCanlibDataBitrate(std::uint32_t bitrate, long& out)
{
    switch (bitrate) {
    case 500'000:
        out = canFD_BITRATE_500K_80P;
        return true;
    case 1'000'000:
        out = canFD_BITRATE_1M_80P;
        return true;
    case 2'000'000:
        out = canFD_BITRATE_2M_80P;
        return true;
    case 4'000'000:
        out = canFD_BITRATE_4M_80P;
        return true;
    case 8'000'000:
        out = canFD_BITRATE_8M_80P;
        return true;
    default:
        return false;
    }
}

/// Parses "kvaser:N" into N.
[[nodiscard]] bool parseHandle(const std::string& handle, int& channelIndex)
{
    constexpr std::string_view prefix = "kvaser:";
    if (!handle.starts_with(prefix)) {
        return false;
    }

    const std::string suffix = handle.substr(prefix.size());
    if (suffix.empty()
        || !std::ranges::all_of(suffix, [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }

    channelIndex = std::stoi(suffix);
    return channelIndex >= 0;
}

[[nodiscard]] CanBusState toBusState(unsigned long flags)
{
    // Checked worst-first: the flags are not mutually exclusive, and a
    // bus-off controller also reports error-passive.
    if ((flags & canSTAT_BUS_OFF) != 0) {
        return CanBusState::BusOff;
    }
    if ((flags & canSTAT_ERROR_PASSIVE) != 0) {
        return CanBusState::ErrorPassive;
    }
    if ((flags & canSTAT_ERROR_WARNING) != 0) {
        return CanBusState::ErrorWarning;
    }
    return CanBusState::ErrorActive;
}

/// Reads the capabilities CANlib advertises for a channel.
[[nodiscard]] CanCapabilities readCapabilities(int channelIndex)
{
    CanCapabilities capabilities;
    capabilities.canClassic = true;
    capabilities.maxChannels = 1;

    std::uint32_t caps = 0;
    if (canGetChannelData(channelIndex, canCHANNELDATA_CHANNEL_CAP, &caps, sizeof(caps)) == canOK) {
        capabilities.canFd = (caps & canCHANNEL_CAP_CAN_FD) != 0;
        capabilities.canFdBrs = capabilities.canFd;
        capabilities.listenOnly = (caps & canCHANNEL_CAP_SILENT_MODE) != 0;
        capabilities.errorFrames = (caps & canCHANNEL_CAP_ERROR_COUNTERS) != 0;
        capabilities.virtualDevice = (caps & canCHANNEL_CAP_VIRTUAL) != 0;
    }

    // Every Kvaser interface timestamps in hardware; that is one of the main
    // reasons to buy one.
    capabilities.hardwareTimestamp = !capabilities.virtualDevice;

    return capabilities;
}

} // namespace

// ---------------------------------------------------------------------------

struct KvaserCanBackend::Impl final {
    mutable std::mutex mutex;

    CanChannelConfig config;
    CanCapabilities capabilities;
    int channelIndex{-1};
    canHandle handle{-1};

    FrameHandler frameHandler;
    StatusHandler statusHandler;

    std::thread receiveThread;
    std::atomic<bool> running{false};

    std::atomic<CanBusState> state{CanBusState::Offline};
    std::atomic<std::uint64_t> hardwareOverruns{0};

    /// Wraparound tracking for the driver's 32-bit tick counter.
    std::uint32_t previousTicks{0};
    std::uint64_t tickEpoch{0};
    bool havePreviousTicks{false};

    /// True when the driver agreed to echo our own transmissions back to us.
    /// When it does not, transmit() synthesises the echo so that the trace
    /// still shows what we sent - the rest of the application must not have to
    /// care which of the two happened.
    bool txAcknowledgeEnabled{false};

    bool open{false};

    /// Converts a driver tick count into monotonic nanoseconds, surviving the
    /// 32-bit wrap. Called only from the receive thread.
    [[nodiscard]] std::uint64_t toNanoseconds(unsigned long ticks)
    {
        const auto current = static_cast<std::uint32_t>(ticks);

        if (havePreviousTicks && current < previousTicks) {
            // Went backwards: the counter wrapped rather than time reversing.
            tickEpoch += std::uint64_t{1} << 32U;
        }

        previousTicks = current;
        havePreviousTicks = true;

        return (tickEpoch + current) * std::uint64_t{kTimerScaleMicroseconds} * 1000ULL;
    }
};

KvaserCanBackend::KvaserCanBackend()
    : m_impl{std::make_unique<Impl>()}
{
    // Safe to call repeatedly, and required before anything else in CANlib.
    // Doing it per instance rather than once globally keeps the backend
    // self-contained; CANlib reference-counts internally.
    canInitializeLibrary();
}

KvaserCanBackend::~KvaserCanBackend()
{
    KvaserCanBackend::close();
}

bool KvaserCanBackend::isCompiledIn() noexcept
{
    return true;
}

bool KvaserCanBackend::isAvailable() const noexcept
{
    int count = 0;
    return canGetNumberOfChannels(&count) == canOK && count > 0;
}

CanDeviceInfoList KvaserCanBackend::enumerate()
{
    int count = 0;
    if (canGetNumberOfChannels(&count) != canOK || count <= 0) {
        return {};
    }

    CanDeviceInfoList devices;
    devices.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        CanDeviceInfo info;
        info.handle = std::format("kvaser:{}", index);
        info.backend = "kvaser";
        info.channelIndex = static_cast<std::uint32_t>(index);
        info.capabilities = readCapabilities(index);

        std::array<char, 256> name{};
        if (canGetChannelData(index, canCHANNELDATA_CHANNEL_NAME, name.data(), name.size())
            == canOK) {
            info.name = name.data();
        } else {
            info.name = std::format("Kvaser channel {}", index);
        }

        std::array<std::uint64_t, 2> serial{};
        if (canGetChannelData(index, canCHANNELDATA_CARD_SERIAL_NO, serial.data(), sizeof(serial))
                == canOK
            && serial[0] != 0) {
            info.serialNumber = std::to_string(serial[0]);
        }

        devices.push_back(std::move(info));
    }

    return devices;
}

Result KvaserCanBackend::open(const CanChannelConfig& config)
{
    int channelIndex = 0;
    if (!parseHandle(config.deviceHandle, channelIndex)) {
        return Result::error(
            ErrorCode::DeviceNotFound,
            std::format("'{}' is not a Kvaser channel handle", config.deviceHandle));
    }

    const std::lock_guard lock{m_impl->mutex};

    if (m_impl->open) {
        return Result::error(ErrorCode::InvalidState, "Channel is already open");
    }

    const CanCapabilities capabilities = readCapabilities(channelIndex);

    if (config.canFdEnabled && !capabilities.canFd) {
        return Result::error(ErrorCode::UnsupportedFeature,
                             "This Kvaser channel does not support CAN FD");
    }

    long arbitration = 0;
    if (!toCanlibBitrate(config.timing.bitrate, arbitration)) {
        return Result::error(
            ErrorCode::BitTimingRejected,
            std::format("{} bit/s is not one of the standard rates CANlib can configure on its "
                        "own. Use 10k, 50k, 62k, 83k, 100k, 125k, 250k, 500k or 1M.",
                        config.timing.bitrate));
    }

    // canOPEN_ACCEPT_VIRTUAL is what makes the two channels the Kvaser Windows
    // installer creates usable - and they are the whole point of being able to
    // develop against Kvaser with no hardware on the desk.
    int flags = canOPEN_ACCEPT_VIRTUAL;
    if (config.canFdEnabled) {
        flags |= canOPEN_CAN_FD;
    }

    const canHandle handle = canOpenChannel(channelIndex, flags);
    if (handle < 0) {
        return toResult(static_cast<canStatus>(handle), "canOpenChannel");
    }

    const auto fail = [&handle](canStatus status, std::string_view call) {
        canClose(handle);
        return toResult(status, call);
    };

    if (const canStatus status = canSetBusParams(handle, arbitration, 0, 0, 0, 0, 0);
        status != canOK) {
        return fail(status, "canSetBusParams");
    }

    if (config.canFdEnabled && config.bitRateSwitchEnabled) {
        long data = 0;
        if (!toCanlibDataBitrate(config.timing.dataBitrate, data)) {
            canClose(handle);
            return Result::error(
                ErrorCode::BitTimingRejected,
                std::format("{} bit/s is not a CAN FD data rate CANlib can configure on its own. "
                            "Use 500k, 1M, 2M, 4M or 8M.",
                            config.timing.dataBitrate));
        }

        if (const canStatus status = canSetBusParamsFd(handle, data, 0, 0, 0); status != canOK) {
            return fail(status, "canSetBusParamsFd");
        }
    }

    const unsigned int driverMode = config.listenOnly ? canDRIVER_SILENT : canDRIVER_NORMAL;
    if (const canStatus status = canSetBusOutputControl(handle, driverMode); status != canOK) {
        return fail(status, "canSetBusOutputControl");
    }

    unsigned int scale = kTimerScaleMicroseconds;
    canIoCtl(handle, canIOCTL_SET_TIMER_SCALE, &scale, sizeof(scale));

    // Ask the driver to echo our transmissions back through the receive path,
    // so the trace shows what actually reached the bus rather than what we
    // asked for. Not every device supports it; transmit() covers the gap.
    unsigned int txAck = 1;
    m_impl->txAcknowledgeEnabled =
        canIoCtl(handle, canIOCTL_SET_TXACK, &txAck, sizeof(txAck)) == canOK;

    m_impl->handle = handle;
    m_impl->channelIndex = channelIndex;
    m_impl->config = config;
    m_impl->capabilities = capabilities;
    m_impl->open = true;
    m_impl->havePreviousTicks = false;
    m_impl->tickEpoch = 0;

    return Result::ok();
}

Result KvaserCanBackend::start()
{
    FrameHandler handler;
    canHandle handle = -1;
    std::uint8_t applicationChannel = 0;

    {
        const std::lock_guard lock{m_impl->mutex};

        if (!m_impl->open) {
            return Result::error(ErrorCode::ChannelNotOpen);
        }
        if (m_impl->running.load(std::memory_order_acquire)) {
            return Result::ok();
        }

        handle = m_impl->handle;
        handler = m_impl->frameHandler;
        applicationChannel = m_impl->config.applicationChannel;

        if (const canStatus status = canBusOn(handle); status != canOK) {
            return toResult(status, "canBusOn");
        }

        m_impl->state.store(CanBusState::ErrorActive, std::memory_order_relaxed);
        m_impl->hardwareOverruns.store(0, std::memory_order_relaxed);

        if (m_impl->statusHandler) {
            CanBusStatus busStatus;
            busStatus.state = CanBusState::ErrorActive;
            m_impl->statusHandler(busStatus);
        }
    }

    m_impl->running.store(true, std::memory_order_release);

    m_impl->receiveThread = std::thread{guardThread(
        "the Kvaser receive thread",
        [this](std::string_view reason) { reportThreadStopped(reason); },
        [this, handle, handler, applicationChannel] {
            std::vector<CanFrame> batch;
            batch.reserve(kBatchSize);

            const auto flush = [&batch, &handler] {
                if (!batch.empty() && handler) {
                    handler(std::span<const CanFrame>{batch.data(), batch.size()});
                }
                batch.clear();
            };

            while (m_impl->running.load(std::memory_order_acquire)) {
                long identifier = 0;
                std::array<std::uint8_t, kMaxCanPayload> data{};
                unsigned int dlc = 0;
                unsigned int flags = 0;
                unsigned long ticks = 0;

                const canStatus status = canReadWait(
                    handle, &identifier, data.data(), &dlc, &flags, &ticks, kReadTimeoutMs);

                if (status == canERR_NOMSG || status == canERR_TIMEOUT) {
                    // Driver ran dry. Deliver what we have rather than holding it
                    // until the batch happens to fill.
                    flush();
                    continue;
                }

                if (status != canOK) {
                    flush();

                    // A read failure is reported once and the loop keeps going: a
                    // transient error must not silently end the measurement.
                    if (m_impl->statusHandler) {
                        CanBusStatus busStatus;
                        busStatus.state = m_impl->state.load(std::memory_order_relaxed);
                        m_impl->statusHandler(busStatus);
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    continue;
                }

                CanFrame frame;
                frame.timestampNs = m_impl->toNanoseconds(ticks);
                frame.identifier = static_cast<std::uint32_t>(identifier);
                frame.channel = applicationChannel;
                frame.format =
                    (flags & canMSG_EXT) != 0 ? CanFrameFormat::Extended : CanFrameFormat::Standard;
                frame.fd = (flags & canFDMSG_FDF) != 0;
                frame.brs = (flags & canFDMSG_BRS) != 0;
                frame.esi = (flags & canFDMSG_ESI) != 0;
                frame.rtr = (flags & canMSG_RTR) != 0;
                frame.error = (flags & canMSG_ERROR_FRAME) != 0;

                // canMSG_TXACK marks the driver echoing back something we sent.
                frame.direction = (flags & canMSG_TXACK) != 0 ? CanDirection::Tx : CanDirection::Rx;

                frame.dlc = static_cast<std::uint8_t>(std::min<unsigned int>(dlc, 15U));
                frame.length = static_cast<std::uint8_t>(std::min<std::size_t>(
                    payloadLengthFromDlc(frame.dlc, frame.fd), kMaxCanPayload));
                std::copy_n(data.begin(), frame.length, frame.data.begin());

                batch.push_back(frame);

                if (batch.size() >= kBatchSize) {
                    flush();
                }
            }

            flush();
        })};

    return Result::ok();
}

void KvaserCanBackend::stop()
{
    if (!m_impl->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_impl->receiveThread.joinable()) {
        m_impl->receiveThread.join();
    }

    const std::lock_guard lock{m_impl->mutex};

    if (m_impl->handle >= 0) {
        canBusOff(m_impl->handle);
    }

    m_impl->state.store(CanBusState::Offline, std::memory_order_relaxed);
}

void KvaserCanBackend::close()
{
    stop();

    const std::lock_guard lock{m_impl->mutex};

    if (m_impl->handle >= 0) {
        canClose(m_impl->handle);
        m_impl->handle = -1;
    }

    m_impl->open = false;
}

bool KvaserCanBackend::isOpen() const noexcept
{
    const std::lock_guard lock{m_impl->mutex};
    return m_impl->open;
}

Result KvaserCanBackend::transmit(const CanFrame& frame)
{
    if (!isValidIdentifier(frame.identifier, frame.format)) {
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("Identifier 0x{:X} does not fit the {} format",
                                         frame.identifier,
                                         frame.isExtended() ? "extended" : "standard"));
    }

    canHandle handle = -1;
    bool synthesiseEcho = false;
    FrameHandler handler;
    std::uint8_t applicationChannel = 0;

    {
        const std::lock_guard lock{m_impl->mutex};

        if (!m_impl->running.load(std::memory_order_acquire)) {
            return Result::error(ErrorCode::ChannelNotOpen,
                                 "Channel is not started - call start() first");
        }
        if (m_impl->config.listenOnly) {
            return Result::error(ErrorCode::InvalidState, "Channel is in listen-only mode");
        }
        if (frame.fd && !m_impl->config.canFdEnabled) {
            return Result::error(ErrorCode::UnsupportedFeature,
                                 "Channel was not opened in CAN FD mode");
        }

        handle = m_impl->handle;
        synthesiseEcho = !m_impl->txAcknowledgeEnabled;
        handler = m_impl->frameHandler;
        applicationChannel = m_impl->config.applicationChannel;
    }

    unsigned int flags = frame.isExtended() ? canMSG_EXT : canMSG_STD;
    if (frame.rtr) {
        flags |= canMSG_RTR;
    }
    if (frame.fd) {
        flags |= canFDMSG_FDF;
        if (frame.brs) {
            flags |= canFDMSG_BRS;
        }
    }

    // canWrite takes a non-const payload pointer even though it only reads it.
    std::array<std::uint8_t, kMaxCanPayload> payload = frame.data;

    const canStatus status =
        canWrite(handle, static_cast<long>(frame.identifier), payload.data(), frame.dlc, flags);

    if (status != canOK) {
        return Result::error(ErrorCode::TransmitFailed, describe(status, "canWrite"));
    }

    if (synthesiseEcho && handler) {
        // The driver will not echo, so the Tx frame is produced here. Callers
        // above the driver layer must see the same thing either way.
        CanFrame echo = frame;
        echo.direction = CanDirection::Tx;
        echo.channel = applicationChannel;
        echo.length = payloadLengthFromDlc(echo.dlc, echo.fd);
        handler(std::span<const CanFrame>{&echo, 1});
    }

    return Result::ok();
}

CanBusStatus KvaserCanBackend::status() const
{
    CanBusStatus result;

    canHandle handle = -1;
    {
        const std::lock_guard lock{m_impl->mutex};
        handle = m_impl->handle;
        if (!m_impl->open || handle < 0) {
            return result;
        }
    }

    unsigned long flags = 0;
    if (canReadStatus(handle, &flags) == canOK) {
        result.state = toBusState(flags);

        if ((flags & (canSTAT_HW_OVERRUN | canSTAT_SW_OVERRUN)) != 0) {
            result.hardwareOverruns =
                m_impl->hardwareOverruns.fetch_add(1, std::memory_order_relaxed) + 1;
        } else {
            result.hardwareOverruns = m_impl->hardwareOverruns.load(std::memory_order_relaxed);
        }
    }

    unsigned int txErrors = 0;
    unsigned int rxErrors = 0;
    unsigned int overruns = 0;
    if (canReadErrorCounters(handle, &txErrors, &rxErrors, &overruns) == canOK) {
        result.transmitErrorCounter = txErrors;
        result.receiveErrorCounter = rxErrors;
    }

    return result;
}

CanCapabilities KvaserCanBackend::capabilities() const
{
    const std::lock_guard lock{m_impl->mutex};
    return m_impl->capabilities;
}

void KvaserCanBackend::setFrameHandler(FrameHandler handler)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->frameHandler = std::move(handler);
}

void KvaserCanBackend::reportThreadStopped(std::string_view reason)
{
    StatusHandler handler;
    CanBusStatus status;

    {
        const std::lock_guard lock{m_impl->mutex};
        m_impl->running.store(false, std::memory_order_release);
        m_impl->state.store(CanBusState::Offline, std::memory_order_relaxed);
        status.state = CanBusState::Offline;
        status.hardwareOverruns = m_impl->hardwareOverruns.load(std::memory_order_relaxed);
        handler = m_impl->statusHandler;
    }

    // Outside the lock, like every other fan-out here.
    if (handler) {
        handler(status);
    }

    // The reason is not thrown away: without it the only evidence is a channel
    // that went quiet, which is what a pulled cable looks like too.
    std::fputs("TorqueBus: ", stderr);
    std::fwrite(reason.data(), 1, reason.size(), stderr);
    std::fputc('\n', stderr);
}

void KvaserCanBackend::setStatusHandler(StatusHandler handler)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->statusHandler = std::move(handler);
}

#else // TORQUEBUS_HAVE_KVASER

// ---------------------------------------------------------------------------
// Stub build: CANlib was not found at configure time.
//
// The class still exists and still constructs. It reports itself unavailable
// and enumerates nothing, so the Hardware Manager can show "Kvaser CANlib - not
// installed" instead of the application failing to start (PLAN.md section 31).
// ---------------------------------------------------------------------------

struct KvaserCanBackend::Impl final {
    CanCapabilities capabilities;
};

KvaserCanBackend::KvaserCanBackend()
    : m_impl{std::make_unique<Impl>()}
{ }

KvaserCanBackend::~KvaserCanBackend() = default;

bool KvaserCanBackend::isCompiledIn() noexcept
{
    return false;
}
bool KvaserCanBackend::isAvailable() const noexcept
{
    return false;
}

CanDeviceInfoList KvaserCanBackend::enumerate()
{
    return {};
}

Result KvaserCanBackend::open(const CanChannelConfig&)
{
    return Result::error(ErrorCode::BackendUnavailable,
                         "This build of TorqueBus was compiled without Kvaser CANlib. "
                         "Install the Kvaser drivers and CANlib SDK, then reconfigure.");
}

Result KvaserCanBackend::start()
{
    return Result::error(ErrorCode::BackendUnavailable);
}
void KvaserCanBackend::stop() { }
void KvaserCanBackend::close() { }
bool KvaserCanBackend::isOpen() const noexcept
{
    return false;
}

Result KvaserCanBackend::transmit(const CanFrame&)
{
    return Result::error(ErrorCode::BackendUnavailable);
}

CanBusStatus KvaserCanBackend::status() const
{
    return {};
}
CanCapabilities KvaserCanBackend::capabilities() const
{
    return m_impl->capabilities;
}

void KvaserCanBackend::setFrameHandler(FrameHandler) { }
void KvaserCanBackend::setStatusHandler(StatusHandler) { }

// The stub has no threads to guard, but the declaration is unconditional and a
// missing definition is a link error rather than a compile one - found later,
// and only by whoever builds without the SDK.
void KvaserCanBackend::reportThreadStopped(std::string_view) { }

#endif // TORQUEBUS_HAVE_KVASER

} // namespace torquebus
