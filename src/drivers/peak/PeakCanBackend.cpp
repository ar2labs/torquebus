// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The only translation unit in TorqueBus that includes QtSerialBus.

#include "drivers/peak/PeakCanBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#ifdef TORQUEBUS_HAVE_PEAK
#include <QByteArray>
#include <QCanBus>
#include <QCanBusDevice>
#include <QCanBusDeviceInfo>
#include <QCanBusFrame>
#include <QEventLoop>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariant>
#endif

namespace torquebus {

#ifdef TORQUEBUS_HAVE_PEAK

namespace {

/// The Qt SerialBus plugin key. Spelled once.
constexpr auto kPlugin = "peakcan";

/// Handles look like "peak:can0" - the backend name, then the interface name
/// Qt SerialBus uses. Persisted in .tbsproj files, so the interface name has to
/// be exactly what createDevice() will later be handed back.
constexpr auto kHandlePrefix = "peak:";

[[nodiscard]] QString interfaceFromHandle(const std::string& handle)
{
    const std::string_view view{handle};
    const std::string_view prefix{kHandlePrefix};

    if (view.starts_with(prefix)) {
        return QString::fromUtf8(view.substr(prefix.size()).data(),
                                 static_cast<qsizetype>(view.size() - prefix.size()));
    }

    // A handle without the prefix is taken as a bare interface name. Being
    // lenient here costs nothing and lets somebody hand-edit a .tbsproj.
    return QString::fromStdString(handle);
}

[[nodiscard]] CanBusState toBusState(QCanBusDevice::CanBusStatus status)
{
    switch (status) {
    case QCanBusDevice::CanBusStatus::Good:    return CanBusState::ErrorActive;
    case QCanBusDevice::CanBusStatus::Warning: return CanBusState::ErrorWarning;
    case QCanBusDevice::CanBusStatus::Error:   return CanBusState::ErrorPassive;
    case QCanBusDevice::CanBusStatus::BusOff:  return CanBusState::BusOff;
    case QCanBusDevice::CanBusStatus::Unknown: break;
    }

    // Unknown is what a plugin reports when it cannot ask the controller. Read
    // as Offline rather than guessed at as Good: a channel claiming to be
    // error-active on no evidence is the wrong direction to be wrong in.
    return CanBusState::Offline;
}

/// Converts one frame off the wire.
///
/// Returns false for a frame TorqueBus has no representation for. Error frames
/// are *not* in that category - they are marked and kept, because a trace that
/// silently drops them hides exactly the event somebody is looking for.
[[nodiscard]] bool toCanFrame(const QCanBusFrame& source,
                              std::uint8_t applicationChannel,
                              std::uint64_t timestampNs,
                              CanFrame& out)
{
    switch (source.frameType()) {
    case QCanBusFrame::DataFrame:
    case QCanBusFrame::ErrorFrame:
    case QCanBusFrame::RemoteRequestFrame:
        break;

    case QCanBusFrame::UnknownFrame:
    case QCanBusFrame::InvalidFrame:
        return false;
    }

    const QByteArray payload = source.payload();

    out = CanFrame{};
    out.identifier = source.frameId();
    out.channel = applicationChannel;
    out.timestampNs = timestampNs;
    out.format = source.hasExtendedFrameFormat() ? CanFrameFormat::Extended
                                                 : CanFrameFormat::Standard;
    out.fd = source.hasFlexibleDataRateFormat();
    out.brs = source.hasBitrateSwitch();
    out.error = source.frameType() == QCanBusFrame::ErrorFrame;
    out.remote = source.frameType() == QCanBusFrame::RemoteRequestFrame;

    const auto length = static_cast<std::size_t>(
        std::min<qsizetype>(payload.size(), static_cast<qsizetype>(kMaxCanPayload)));

    for (std::size_t index = 0; index < length; ++index) {
        out.data[index] = static_cast<std::uint8_t>(payload.at(static_cast<qsizetype>(index)));
    }

    out.length = static_cast<std::uint8_t>(length);
    out.dlc = dlcFromPayloadLength(out.length, out.fd);

    return true;
}

[[nodiscard]] QCanBusFrame toQtFrame(const CanFrame& frame)
{
    QCanBusFrame result;
    result.setFrameId(frame.identifier);
    result.setExtendedFrameFormat(frame.format == CanFrameFormat::Extended);
    result.setFrameType(frame.remote ? QCanBusFrame::RemoteRequestFrame
                                     : QCanBusFrame::DataFrame);

    QByteArray payload;
    payload.resize(static_cast<qsizetype>(frame.length));
    for (std::size_t index = 0; index < frame.length; ++index) {
        payload[static_cast<qsizetype>(index)] = static_cast<char>(frame.data[index]);
    }
    result.setPayload(payload);

    if (frame.fd) {
        result.setFlexibleDataRateFormat(true);
        result.setBitrateSwitch(frame.brs);
    }

    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct PeakCanBackend::Impl final {
    mutable std::mutex mutex;

    CanChannelConfig config;
    CanCapabilities capabilities;
    QString interfaceName;

    FrameHandler frameHandler;
    StatusHandler statusHandler;

    /// The device and the thread it lives on.
    ///
    /// Both created in open() and destroyed in close(). `device` is touched
    /// only from `thread` - every other access goes through invokeMethod, which
    /// is what the const-correctness of this struct cannot express and the
    /// comments therefore have to.
    QThread* thread{nullptr};

    /// A QObject that actually lives on `thread`, used as the context for
    /// everything that has to run there. `thread` itself cannot serve: a
    /// QThread object belongs to the thread that created it.
    QObject* anchor{nullptr};

    /// Atomic because drainDevice() reads it on the device's thread while
    /// close() clears it on the caller's. The blocking handshake in stop() puts
    /// the two in order in practice; the atomic is what makes that a fact
    /// rather than a timing argument.
    std::atomic<QCanBusDevice*> device{nullptr};

    std::atomic<CanBusState> state{CanBusState::Offline};
    std::atomic<std::uint64_t> hardwareOverruns{0};
    std::atomic<bool> running{false};

    bool open{false};

    /// Where the measurement's clock starts, for the fallback below.
    std::chrono::steady_clock::time_point started;

    /// True when the plugin gave us a timestamp worth using.
    ///
    /// PCAN-Basic timestamps frames in the driver, but not every plugin build
    /// passes them through - some hand back a zero TimeStamp. A trace where
    /// every row reads 0.000000 is worse than one timestamped on arrival, so
    /// the first frame decides which of the two this run gets, and it says so
    /// through capabilities().hardwareTimestamp.
    bool hardwareTimestamps{true};
    bool timestampSourceDecided{false};

    /// Reused by the receive slot so a batch costs no allocation after the
    /// first (rule #12 in spirit: the driver layer batches too).
    std::vector<CanFrame> batch;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PeakCanBackend::PeakCanBackend()
    : m_impl{std::make_unique<Impl>()}
{
    m_impl->capabilities.canClassic = true;
    m_impl->capabilities.errorFrames = true;
    m_impl->capabilities.hardwareTimestamp = true;
    m_impl->capabilities.listenOnly = true;
}

PeakCanBackend::~PeakCanBackend()
{
    PeakCanBackend::close();
}

bool PeakCanBackend::isCompiledIn() noexcept
{
    return true;
}

bool PeakCanBackend::isAvailable() const noexcept
{
    // Two questions, and the plugin list answers the first cheaply. Asking for
    // devices as well would also catch "driver installed, nothing plugged in",
    // but that is not unavailability - it is an empty list, and the Hardware
    // Manager already knows how to show one.
    if (QCanBus* bus = QCanBus::instance()) {
        return bus->plugins().contains(QString::fromUtf8(kPlugin));
    }
    return false;
}

CanDeviceInfoList PeakCanBackend::enumerate()
{
    CanDeviceInfoList devices;

    QCanBus* bus = QCanBus::instance();
    if (bus == nullptr || !bus->plugins().contains(QString::fromUtf8(kPlugin))) {
        return devices;
    }

    QString error;
    const QList<QCanBusDeviceInfo> found =
        bus->availableDevices(QString::fromUtf8(kPlugin), &error);

    for (const QCanBusDeviceInfo& info : found) {
        CanDeviceInfo device;
        device.handle = std::string{kHandlePrefix} + info.name().toStdString();
        device.backend = "peak";

        // The plugin's description is the model - "PCAN-USB FD". Preferred over
        // the interface name, which is "usb0" and tells a user nothing about
        // which of the two adapters on their desk it is.
        const QString description = info.description();
        device.name = description.isEmpty()
            ? std::format("PEAK {}", info.name().toStdString())
            : std::format("{} ({})", description.toStdString(), info.name().toStdString());

        device.serialNumber = info.serialNumber().toStdString();
        device.channelIndex = static_cast<std::uint32_t>(info.channel());

        device.capabilities = m_impl->capabilities;
        device.capabilities.canFd = info.hasFlexibleDataRate();
        device.capabilities.canFdBrs = info.hasFlexibleDataRate();
        device.capabilities.virtualDevice = info.isVirtual();

        devices.push_back(std::move(device));
    }

    return devices;
}

// ---------------------------------------------------------------------------
// Opening: the thread, and the device that lives on it
// ---------------------------------------------------------------------------

Result PeakCanBackend::open(const CanChannelConfig& config)
{
    close();

    QCanBus* bus = QCanBus::instance();
    if (bus == nullptr || !bus->plugins().contains(QString::fromUtf8(kPlugin))) {
        return Result::error(
            ErrorCode::BackendUnavailable,
            std::format("Qt SerialBus has no '{}' plugin on this machine. Install the "
                        "PEAK-System driver (PCANBasic.dll), then restart TorqueBus.",
                        kPlugin));
    }

    const QString interfaceName = interfaceFromHandle(config.deviceHandle);
    if (interfaceName.isEmpty()) {
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("'{}' does not name a PEAK interface",
                                         config.deviceHandle));
    }

    // The thread exists before the device does, because the device has to be
    // created on it: QCanBusDevice is not thread safe, and its plugin reads on
    // the event loop of whichever thread it belongs to. Creating it here and
    // moving it would work too, and would leave a window in which it belongs to
    // the caller's thread - a window that costs nothing to not have.
    auto* thread = new QThread;
    thread->setObjectName(QStringLiteral("torquebus.peak.%1").arg(interfaceName));

    // The anchor, and the reason it exists: **a QThread object lives on the
    // thread that created it**, not on the thread it runs. Handing `thread` to
    // invokeMethod would run the lambda right back here, on the caller, and
    // build the device on the wrong thread - which would work perfectly until
    // the first frame arrived on the other one.
    //
    // So a plain QObject is moved onto the new thread and used as the context
    // for anything that has to happen there.
    auto* anchor = new QObject;
    anchor->moveToThread(thread);
    thread->start();

    QCanBusDevice* device = nullptr;
    QString error;

    // Blocking, because open() has to answer. The lambda runs on the new
    // thread, so everything it creates belongs there.
    QMetaObject::invokeMethod(
        anchor,
        [&] {
            QString createError;
            device = QCanBus::instance()->createDevice(QString::fromUtf8(kPlugin),
                                                       interfaceName, &createError);
            error = createError;
        },
        Qt::BlockingQueuedConnection);

    if (device == nullptr) {
        // The thread is stopped first, then the anchor is deleted directly.
        // deleteLater() would post to an event loop that quit() is about to
        // end, and the object would leak on the one path a user reaches by
        // mistyping a handle.
        thread->quit();
        thread->wait();
        delete anchor;
        delete thread;

        return Result::error(ErrorCode::DeviceNotFound,
                             std::format("Could not open PEAK interface '{}': {}",
                                         interfaceName.toStdString(),
                                         error.isEmpty() ? std::string{"no reason given"}
                                                         : error.toStdString()));
    }

    {
        const std::lock_guard lock{m_impl->mutex};
        m_impl->config = config;
        m_impl->interfaceName = interfaceName;
        m_impl->thread = thread;
        m_impl->anchor = anchor;
        m_impl->device.store(device, std::memory_order_release);
        m_impl->open = true;
        m_impl->hardwareTimestamps = true;
        m_impl->timestampSourceDecided = false;
    }

    // Configuration and signal wiring, both on the device's own thread.
    const std::uint32_t bitrate = config.timing.bitrate;
    const std::uint32_t dataBitrate = config.timing.dataBitrate;
    const bool wantFd = config.canFdEnabled;
    const bool wantBrs = config.bitRateSwitchEnabled;
    const bool listenOnly = config.listenOnly;

    QMetaObject::invokeMethod(
        device,
        [this, device, bitrate, dataBitrate, wantFd, wantBrs, listenOnly] {
            device->setConfigurationParameter(QCanBusDevice::BitRateKey,
                                              QVariant{static_cast<uint>(bitrate)});

            if (wantFd) {
                device->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant{true});

                // The data-phase rate only means anything with BRS on. Setting
                // it regardless would be harmless and would also put a number
                // in the device's configuration that nothing uses, which is how
                // a later reader concludes the wrong thing about the channel.
                if (wantBrs) {
                    device->setConfigurationParameter(
                        QCanBusDevice::DataBitRateKey,
                        QVariant{static_cast<uint>(dataBitrate)});
                }
            }

            // What we send comes back to us, so the trace shows what actually
            // reached the bus rather than what was requested. Same contract the
            // Kvaser backend keeps, reached through a different switch.
            //
            // Off in listen-only, where there is nothing of ours to echo:
            // never acknowledge, never transmit. That is the one configuration
            // in which being on the wrong bitrate is harmless rather than
            // disruptive, which is the whole reason it exists.
            device->setConfigurationParameter(QCanBusDevice::LoopbackKey, QVariant{false});
            device->setConfigurationParameter(QCanBusDevice::ReceiveOwnKey,
                                              QVariant{!listenOnly});

            // The device is the context object as well as the sender, so these
            // run on its thread - which is this thread - with no queueing.
            QObject::connect(device, &QCanBusDevice::framesReceived, device,
                             [this] { drainDevice(); });

            QObject::connect(device, &QCanBusDevice::stateChanged, device,
                             [this](QCanBusDevice::CanBusDeviceState deviceState) {
                                 if (deviceState == QCanBusDevice::UnconnectedState) {
                                     m_impl->state.store(CanBusState::Offline,
                                                         std::memory_order_relaxed);
                                 }
                             });

            QObject::connect(device, &QCanBusDevice::errorOccurred, device,
                             [this](QCanBusDevice::CanBusError busError) {
                                 onDeviceError(static_cast<int>(busError));
                             });
        },
        Qt::BlockingQueuedConnection);

    return Result::ok();
}

Result PeakCanBackend::start()
{
    QCanBusDevice* device = nullptr;
    {
        const std::lock_guard lock{m_impl->mutex};
        if (!m_impl->open) {
            return Result::error(ErrorCode::InvalidState,
                                 "start() called on a PEAK channel that is not open");
        }
        device = m_impl->device.load(std::memory_order_acquire);
    }

    m_impl->started = std::chrono::steady_clock::now();
    m_impl->batch.clear();
    m_impl->batch.reserve(256);

    bool connected = false;
    QString error;

    QMetaObject::invokeMethod(
        device,
        [device, &connected, &error] {
            connected = device->connectDevice();
            if (!connected) {
                error = device->errorString();
            }
        },
        Qt::BlockingQueuedConnection);

    if (!connected) {
        return Result::error(ErrorCode::Unknown,
                             std::format("PEAK channel would not go bus-on: {}",
                                         error.isEmpty() ? std::string{"no reason given"}
                                                         : error.toStdString()));
    }

    m_impl->running.store(true, std::memory_order_release);
    m_impl->state.store(CanBusState::ErrorActive, std::memory_order_relaxed);

    return Result::ok();
}

void PeakCanBackend::stop()
{
    if (!m_impl->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    QCanBusDevice* device = m_impl->device.load(std::memory_order_acquire);

    if (device != nullptr) {
        QMetaObject::invokeMethod(
            device, [device] { device->disconnectDevice(); }, Qt::BlockingQueuedConnection);
    }

    m_impl->state.store(CanBusState::Offline, std::memory_order_relaxed);
}

void PeakCanBackend::close()
{
    stop();

    QThread* thread = nullptr;
    QObject* anchor = nullptr;
    QCanBusDevice* device = nullptr;

    {
        const std::lock_guard lock{m_impl->mutex};
        thread = m_impl->thread;
        anchor = m_impl->anchor;
        device = m_impl->device.exchange(nullptr, std::memory_order_acq_rel);

        m_impl->thread = nullptr;
        m_impl->anchor = nullptr;
        m_impl->open = false;
    }

    if (thread == nullptr) {
        return;
    }

    // Deleted on its own thread, because that is where its plugin's timers and
    // notifiers live. deleteLater would also work and would leave the object
    // alive until the loop next spins, which is one more thing to reason about
    // during shutdown.
    if (device != nullptr) {
        QMetaObject::invokeMethod(
            device, [device] { delete device; }, Qt::BlockingQueuedConnection);
    }

    if (anchor != nullptr) {
        QMetaObject::invokeMethod(
            anchor, [anchor] { delete anchor; }, Qt::BlockingQueuedConnection);
    }

    thread->quit();
    thread->wait();
    delete thread;
}

bool PeakCanBackend::isOpen() const noexcept
{
    const std::lock_guard lock{m_impl->mutex};
    return m_impl->open;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void PeakCanBackend::drainDevice()
{
    // Runs on the device's thread, called from its framesReceived signal.
    QCanBusDevice* device = m_impl->device.load(std::memory_order_acquire);
    if (device == nullptr) {
        return;
    }

    const QList<QCanBusFrame> frames = device->readAllFrames();
    if (frames.isEmpty()) {
        return;
    }

    const auto channel = static_cast<std::uint8_t>(m_impl->config.applicationChannel);

    m_impl->batch.clear();
    if (m_impl->batch.capacity() < static_cast<std::size_t>(frames.size())) {
        m_impl->batch.reserve(static_cast<std::size_t>(frames.size()));
    }

    for (const QCanBusFrame& source : frames) {
        const QCanBusFrame::TimeStamp stamp = source.timeStamp();

        // Decided once, on the first frame of a run, and then believed. A
        // plugin either passes driver timestamps through or it does not; asking
        // per frame would let a single zero-stamped frame switch the whole
        // trace onto a different clock halfway down.
        if (!m_impl->timestampSourceDecided) {
            m_impl->timestampSourceDecided = true;
            m_impl->hardwareTimestamps = stamp.seconds() != 0 || stamp.microSeconds() != 0;
        }

        const std::uint64_t timestampNs =
            m_impl->hardwareTimestamps
                ? static_cast<std::uint64_t>(stamp.seconds()) * 1'000'000'000ULL
                      + static_cast<std::uint64_t>(stamp.microSeconds()) * 1000ULL
                : static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - m_impl->started)
                          .count());

        CanFrame frame;
        if (toCanFrame(source, channel, timestampNs, frame)) {
            m_impl->batch.push_back(frame);
        }
    }

    if (m_impl->batch.empty()) {
        return;
    }

    FrameHandler handler;
    {
        const std::lock_guard lock{m_impl->mutex};
        handler = m_impl->frameHandler;
    }

    if (handler) {
        handler(std::span<const CanFrame>{m_impl->batch});
    }
}

void PeakCanBackend::onDeviceError(int busError)
{
    const auto error = static_cast<QCanBusDevice::CanBusError>(busError);

    if (error == QCanBusDevice::CanBusError::ReadError) {
        // The plugin's way of saying the driver's queue overflowed. Counted
        // rather than reported: on a saturated bus this arrives often, and a
        // stream of messages would be noise where a number is a measurement.
        m_impl->hardwareOverruns.fetch_add(1, std::memory_order_relaxed);
    }

    StatusHandler handler;
    {
        const std::lock_guard lock{m_impl->mutex};
        handler = m_impl->statusHandler;
    }

    if (!handler) {
        return;
    }

    // Built here rather than by calling status(), which hops onto this very
    // thread with a blocking connection - and this function is already running
    // on it. That is a deadlock, not a slow path, and it would only ever fire
    // on a bus that was producing errors.
    CanBusStatus report;
    report.state = m_impl->state.load(std::memory_order_relaxed);
    report.hardwareOverruns = m_impl->hardwareOverruns.load(std::memory_order_relaxed);

    handler(report);

}

// ---------------------------------------------------------------------------
// Transmitting
// ---------------------------------------------------------------------------

Result PeakCanBackend::transmit(const CanFrame& frame)
{
    if (!isValidIdentifier(frame.identifier, frame.format)) {
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("Identifier 0x{:X} does not fit the {} format",
                                         frame.identifier,
                                         frame.isExtended() ? "extended" : "standard"));
    }

    QCanBusDevice* device = m_impl->device.load(std::memory_order_acquire);
    if (device == nullptr) {
        return Result::error(ErrorCode::InvalidState,
                             "transmit() called on a PEAK channel that is not open");
    }

    const QCanBusFrame outgoing = toQtFrame(frame);

    bool accepted = false;
    QString error;

    // Blocking rather than queued, so this returns the driver's real answer
    // instead of "probably". A transmit list runs at tens of frames a second
    // and the hop costs microseconds; a producer sending thousands a second
    // would want a queued path and its own way of reporting failure, and would
    // be a different function rather than a flag on this one.
    QMetaObject::invokeMethod(
        device,
        [device, &outgoing, &accepted, &error] {
            accepted = device->writeFrame(outgoing);
            if (!accepted) {
                error = device->errorString();
            }
        },
        Qt::BlockingQueuedConnection);

    if (!accepted) {
        return Result::error(ErrorCode::TransmitFailed,
                             std::format("PEAK refused the frame: {}",
                                         error.isEmpty() ? std::string{"queue full"}
                                                         : error.toStdString()));
    }

    return Result::ok();
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

CanBusStatus PeakCanBackend::status() const
{
    CanBusStatus result;
    result.state = m_impl->state.load(std::memory_order_relaxed);
    result.hardwareOverruns = m_impl->hardwareOverruns.load(std::memory_order_relaxed);

    QCanBusDevice* device = m_impl->device.load(std::memory_order_acquire);

    if (device == nullptr || !m_impl->running.load(std::memory_order_acquire)) {
        return result;
    }

    // busStatus() talks to the controller, so it is asked on the device's own
    // thread like everything else. Called a few times a second by the engine's
    // statistics window, never on the frame path - and never from the device's
    // own thread, which is why a blocking hop is safe here and is not safe in
    // onDeviceError().
    QCanBusDevice::CanBusStatus busStatus = QCanBusDevice::CanBusStatus::Unknown;

    QMetaObject::invokeMethod(
        device, [device, &busStatus] { busStatus = device->busStatus(); },
        Qt::BlockingQueuedConnection);

    result.state = toBusState(busStatus);
    m_impl->state.store(result.state, std::memory_order_relaxed);

    return result;
}

CanCapabilities PeakCanBackend::capabilities() const
{
    const std::lock_guard lock{m_impl->mutex};

    CanCapabilities result = m_impl->capabilities;

    // Reported as measured rather than as advertised: whether this run got
    // driver timestamps is known only after the first frame arrived.
    result.hardwareTimestamp = m_impl->hardwareTimestamps;

    return result;
}

void PeakCanBackend::setFrameHandler(FrameHandler handler)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->frameHandler = std::move(handler);
}

void PeakCanBackend::setStatusHandler(StatusHandler handler)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->statusHandler = std::move(handler);
}

#else // TORQUEBUS_HAVE_PEAK

// ---------------------------------------------------------------------------
// Stub build: Qt SerialBus was not available at configure time.
//
// Same shape as the Kvaser stub, and for the same reason: the class still
// exists and still constructs, so the registry, the Hardware Manager and the
// tests are identical in both builds.
// ---------------------------------------------------------------------------

struct PeakCanBackend::Impl final {
    CanCapabilities capabilities;
};

PeakCanBackend::PeakCanBackend()
    : m_impl{std::make_unique<Impl>()}
{
}

PeakCanBackend::~PeakCanBackend() = default;

bool PeakCanBackend::isCompiledIn() noexcept { return false; }
bool PeakCanBackend::isAvailable() const noexcept { return false; }

CanDeviceInfoList PeakCanBackend::enumerate() { return {}; }

Result PeakCanBackend::open(const CanChannelConfig&)
{
    return Result::error(ErrorCode::BackendUnavailable,
                         "This build of TorqueBus was compiled without Qt SerialBus, so it "
                         "cannot reach PEAK-System adapters. Reconfigure with "
                         "TORQUEBUS_ENABLE_PEAK=ON against a Qt that has SerialBus.");
}

Result PeakCanBackend::start() { return Result::error(ErrorCode::BackendUnavailable); }
void PeakCanBackend::stop() {}
void PeakCanBackend::close() {}
bool PeakCanBackend::isOpen() const noexcept { return false; }

Result PeakCanBackend::transmit(const CanFrame&)
{
    return Result::error(ErrorCode::BackendUnavailable);
}

CanBusStatus PeakCanBackend::status() const { return {}; }
CanCapabilities PeakCanBackend::capabilities() const { return m_impl->capabilities; }

void PeakCanBackend::setFrameHandler(FrameHandler) {}
void PeakCanBackend::setStatusHandler(StatusHandler) {}

#endif // TORQUEBUS_HAVE_PEAK

} // namespace torquebus
