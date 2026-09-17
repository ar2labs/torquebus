// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/ThreadGuard.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace torquebus {
namespace {

using Clock = std::chrono::steady_clock;

/// The instant this translation unit was loaded, which is as close to process
/// start as a file-scope object gets.
///
/// It is here and not inside the function on purpose. A function-local static
/// is initialised on the *first call*, so the first frame a process ever
/// timestamps would be measured from a point a few nanoseconds earlier - and on
/// a clock whose tick is coarser than that gap, the answer is exactly 0. That
/// looked like a timestamp that had never been applied, and an integration test
/// caught it as one, intermittently, depending on which frame happened to be
/// first. Anchoring the origin at load time makes every frame's timestamp the
/// thing the name claims: nanoseconds since the process started.
///
/// Ordered dynamic initialisation guarantees this is ready before anything in
/// this file runs, and nothing here runs before main.
const Clock::time_point kProcessOrigin = Clock::now();

/// Nanoseconds since the process started. Stands in for the hardware timestamp
/// a real device would provide.
[[nodiscard]] std::uint64_t monotonicNanoseconds()
{
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - kProcessOrigin);
    return static_cast<std::uint64_t>(elapsed.count());
}

/// How many frames one delivery batch may carry. Mirrors what a real driver
/// does when it reads its hardware FIFO: take what is there, up to a bound.
constexpr std::size_t kMaximumDeliveryBatch = 512;

/// A node's inbox. The bus writes into it; the node's own receive thread
/// drains it and calls the frame handler.
class NodeInbox final {
public:
    void post(const CanFrame& frame)
    {
        {
            const std::lock_guard lock{m_mutex};
            m_pending.push_back(frame);
        }
        m_condition.notify_one();
    }

    /// Waits up to `timeout` for frames, then moves up to
    /// kMaximumDeliveryBatch of them into `out`.
    std::size_t drain(std::vector<CanFrame>& out, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{m_mutex};

        if (m_pending.empty()) {
            m_condition.wait_for(lock, timeout, [this] { return !m_pending.empty() || m_closed; });
        }

        out.clear();
        const std::size_t count = std::min(m_pending.size(), kMaximumDeliveryBatch);

        for (std::size_t index = 0; index < count; ++index) {
            out.push_back(m_pending.front());
            m_pending.pop_front();
        }

        return count;
    }

    [[nodiscard]] bool empty() const
    {
        const std::lock_guard lock{m_mutex};
        return m_pending.empty();
    }

    void close()
    {
        {
            const std::lock_guard lock{m_mutex};
            m_closed = true;
        }
        m_condition.notify_all();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<CanFrame> m_pending;
    bool m_closed{false};
};

/// The shared medium. One VirtualBus per virtual channel index; every backend
/// opened on that index is a node hanging off it.
class VirtualBus final {
public:
    using NodeId = std::uint64_t;

    [[nodiscard]] NodeId attach(std::shared_ptr<NodeInbox> inbox, std::uint8_t applicationChannel)
    {
        const std::lock_guard lock{m_mutex};
        const NodeId id = m_nextId++;
        m_nodes.emplace(id, Node{std::move(inbox), applicationChannel});
        return id;
    }

    void detach(NodeId id)
    {
        const std::lock_guard lock{m_mutex};
        m_nodes.erase(id);
    }

    /// Delivers `frame` to every attached node. The sender receives it back as
    /// a Tx echo, everyone else as Rx - which is precisely how a real
    /// controller reports its own successful transmissions.
    void broadcast(NodeId sender, const CanFrame& frame)
    {
        // The inboxes are shared_ptrs, so a node detaching concurrently cannot
        // pull its inbox out from under us mid-delivery.
        std::vector<std::pair<std::shared_ptr<NodeInbox>, CanFrame>> deliveries;

        {
            const std::lock_guard lock{m_mutex};
            deliveries.reserve(m_nodes.size());

            for (const auto& [id, node] : m_nodes) {
                CanFrame copy = frame;
                copy.channel = node.applicationChannel;
                copy.direction = (id == sender) ? CanDirection::Tx : CanDirection::Rx;
                deliveries.emplace_back(node.inbox, copy);
            }
        }

        for (const auto& [inbox, copy] : deliveries) {
            inbox->post(copy);
        }
    }

    [[nodiscard]] static VirtualBus& forChannel(std::uint32_t channelIndex)
    {
        static std::mutex busesMutex;
        static std::unordered_map<std::uint32_t, std::unique_ptr<VirtualBus>> buses;

        const std::lock_guard lock{busesMutex};
        std::unique_ptr<VirtualBus>& bus = buses[channelIndex];
        if (!bus) {
            bus = std::make_unique<VirtualBus>();
        }
        return *bus;
    }

private:
    struct Node final {
        std::shared_ptr<NodeInbox> inbox;
        std::uint8_t applicationChannel{};
    };

    mutable std::mutex m_mutex;
    std::unordered_map<NodeId, Node> m_nodes;
    NodeId m_nextId{1};
};

[[nodiscard]] CanCapabilities virtualCapabilities()
{
    CanCapabilities capabilities;
    capabilities.canClassic = true;
    capabilities.canFd = true;
    capabilities.canFdBrs = true;
    capabilities.listenOnly = true;
    capabilities.hardwareTimestamp = false;
    capabilities.errorFrames = false;
    capabilities.hardwareFilters = false;
    capabilities.virtualDevice = true;
    capabilities.maxChannels = VirtualCanBackend::kChannelCount;
    return capabilities;
}

/// Parses "virtual:N" into N.
[[nodiscard]] bool parseHandle(const std::string& handle, std::uint32_t& channelIndex)
{
    constexpr std::string_view prefix = "virtual:";
    if (!handle.starts_with(prefix)) {
        return false;
    }

    const std::string suffix = handle.substr(prefix.size());
    if (suffix.empty() || !std::ranges::all_of(suffix, [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }

    channelIndex = static_cast<std::uint32_t>(std::stoul(suffix));
    return channelIndex < VirtualCanBackend::kChannelCount;
}

} // namespace

struct VirtualCanBackend::Impl final {
    mutable std::mutex mutex;

    CanChannelConfig config;
    CanBusStatus busStatus;
    VirtualTrafficPattern traffic;

    FrameHandler frameHandler;
    StatusHandler statusHandler;

    VirtualBus* bus{nullptr};
    VirtualBus::NodeId nodeId{0};
    std::shared_ptr<NodeInbox> inbox;

    std::thread receiveThread;
    std::thread generatorThread;
    std::atomic<bool> running{false};

    std::atomic<std::uint64_t> posted{0};    ///< frames handed to the bus for us
    std::atomic<std::uint64_t> delivered{0}; ///< frames passed to the handler
    std::atomic<bool> trafficDone{false};

    bool open{false};
};

VirtualCanBackend::VirtualCanBackend()
    : m_impl{std::make_unique<Impl>()}
{
}

VirtualCanBackend::~VirtualCanBackend()
{
    VirtualCanBackend::close();
}

CanDeviceInfoList VirtualCanBackend::enumerate()
{
    CanDeviceInfoList devices;
    devices.reserve(kChannelCount);

    for (std::uint32_t index = 0; index < kChannelCount; ++index) {
        CanDeviceInfo info;
        info.handle = std::format("virtual:{}", index);
        info.backend = "virtual";
        info.name = std::format("TorqueBus Virtual CAN {}", index);
        info.channelIndex = index;
        info.capabilities = virtualCapabilities();
        devices.push_back(std::move(info));
    }

    return devices;
}

Result VirtualCanBackend::open(const CanChannelConfig& config)
{
    std::uint32_t channelIndex = 0;
    if (!parseHandle(config.deviceHandle, channelIndex)) {
        return Result::error(ErrorCode::DeviceNotFound,
                             std::format("'{}' is not a virtual channel handle",
                                         config.deviceHandle));
    }

    if (config.canFdEnabled && !virtualCapabilities().canFd) {
        return Result::error(ErrorCode::UnsupportedFeature, "CAN FD is not available");
    }

    const std::lock_guard lock{m_impl->mutex};

    if (m_impl->open) {
        return Result::error(ErrorCode::InvalidState, "Channel is already open");
    }

    m_impl->config = config;
    m_impl->bus = &VirtualBus::forChannel(channelIndex);
    m_impl->open = true;
    m_impl->busStatus = CanBusStatus{};

    return Result::ok();
}

Result VirtualCanBackend::start()
{
    FrameHandler handler;
    std::uint8_t applicationChannel = 0;
    VirtualBus* bus = nullptr;
    VirtualTrafficPattern traffic;

    {
        const std::lock_guard lock{m_impl->mutex};

        if (!m_impl->open) {
            return Result::error(ErrorCode::ChannelNotOpen);
        }
        if (m_impl->running.load(std::memory_order_acquire)) {
            return Result::ok();
        }

        handler = m_impl->frameHandler;
        applicationChannel = m_impl->config.applicationChannel;
        bus = m_impl->bus;
        traffic = m_impl->traffic;

        m_impl->inbox = std::make_shared<NodeInbox>();
        m_impl->posted.store(0, std::memory_order_relaxed);
        m_impl->delivered.store(0, std::memory_order_relaxed);
        m_impl->trafficDone.store(traffic.framesPerSecond == 0, std::memory_order_relaxed);
    }

    const std::shared_ptr<NodeInbox> inbox = m_impl->inbox;
    const VirtualBus::NodeId id = bus->attach(inbox, applicationChannel);

    {
        const std::lock_guard lock{m_impl->mutex};
        m_impl->nodeId = id;
        m_impl->busStatus.state = CanBusState::ErrorActive;

        if (m_impl->statusHandler) {
            m_impl->statusHandler(m_impl->busStatus);
        }
    }

    m_impl->running.store(true, std::memory_order_release);

    // The receive thread is what makes this backend a faithful stand-in for
    // real hardware: the frame handler is never called on the caller's thread.
    m_impl->receiveThread = std::thread{guardThread(
        "the virtual receive thread",
        [this](std::string_view reason) { reportThreadStopped(reason); },
        [this, inbox, handler] {
        std::vector<CanFrame> batch;
        batch.reserve(kMaximumDeliveryBatch);

        while (m_impl->running.load(std::memory_order_acquire)) {
            const std::size_t count = inbox->drain(batch, std::chrono::milliseconds{5});

            if (count == 0) {
                continue;
            }

            if (handler) {
                handler(std::span<const CanFrame>{batch.data(), count});
            }

            m_impl->delivered.fetch_add(count, std::memory_order_relaxed);
        }

        // Final drain: frames posted just before the stop are measurement data
        // like any other and must still reach the handler.
        while (inbox->drain(batch, std::chrono::milliseconds{0}) > 0) {
            if (handler) {
                handler(std::span<const CanFrame>{batch.data(), batch.size()});
            }
            m_impl->delivered.fetch_add(batch.size(), std::memory_order_relaxed);
        }
    })};

    if (traffic.framesPerSecond > 0) {
        m_impl->generatorThread = std::thread{guardThread(
            "the virtual traffic generator",
            [this](std::string_view reason) { reportThreadStopped(reason); },
            [this, traffic, bus] {
            const auto period = std::chrono::nanoseconds{1'000'000'000ULL / traffic.framesPerSecond};
            Clock::time_point next = Clock::now();

            std::uint64_t produced = 0;
            std::uint32_t message = 0;

            while (m_impl->running.load(std::memory_order_acquire)) {
                if (traffic.totalFrames > 0 && produced >= traffic.totalFrames) {
                    break;
                }

                CanFrame frame;
                frame.identifier = traffic.baseIdentifier + message;
                frame.format = traffic.extended ? CanFrameFormat::Extended
                                                : CanFrameFormat::Standard;
                frame.length = traffic.payloadLength;
                frame.dlc = dlcFromPayloadLength(traffic.payloadLength, false);
                frame.timestampNs = monotonicNanoseconds();

                for (std::uint8_t byte = 0; byte < traffic.payloadLength; ++byte) {
                    frame.data[byte] = traffic.varyPayload
                        ? static_cast<std::uint8_t>((produced + byte) & 0xFFU)
                        : static_cast<std::uint8_t>(byte);
                }

                // Sender id 0 is never assigned to a node, so every attached
                // node - including this one - sees generated traffic as Rx.
                // That is the truthful shape: the generator stands in for the
                // rest of the bus, not for something this application sent.
                bus->broadcast(0, frame);

                ++produced;
                message = (message + 1) % std::max(traffic.messageCount, 1U);

                // Above a few tens of thousands of frames per second, sleeping
                // per frame costs more than the frame does. Burst instead and
                // let the pacing be approximate - the point is load, not
                // metrological accuracy.
                next += period;
                if (const Clock::time_point now = Clock::now(); next > now) {
                    std::this_thread::sleep_until(next);
                } else if (now - next > std::chrono::milliseconds{50}) {
                    next = now;
                }
            }

            m_impl->trafficDone.store(true, std::memory_order_release);
        })};
    }

    return Result::ok();
}

void VirtualCanBackend::stop()
{
    if (!m_impl->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_impl->inbox) {
        m_impl->inbox->close();
    }

    if (m_impl->generatorThread.joinable()) {
        m_impl->generatorThread.join();
    }
    if (m_impl->receiveThread.joinable()) {
        m_impl->receiveThread.join();
    }

    VirtualBus* bus = nullptr;
    VirtualBus::NodeId id = 0;

    {
        const std::lock_guard lock{m_impl->mutex};
        bus = m_impl->bus;
        id = m_impl->nodeId;
        m_impl->nodeId = 0;
        m_impl->busStatus.state = CanBusState::Offline;
    }

    if (bus != nullptr) {
        bus->detach(id);
    }

    const std::lock_guard lock{m_impl->mutex};
    m_impl->inbox.reset();
}

void VirtualCanBackend::close()
{
    stop();

    const std::lock_guard lock{m_impl->mutex};
    m_impl->open = false;
    m_impl->bus = nullptr;
}

bool VirtualCanBackend::isOpen() const noexcept
{
    const std::lock_guard lock{m_impl->mutex};
    return m_impl->open;
}

Result VirtualCanBackend::transmit(const CanFrame& frame)
{
    if (!isValidIdentifier(frame.identifier, frame.format)) {
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("Identifier 0x{:X} does not fit the {} format",
                                         frame.identifier,
                                         frame.isExtended() ? "extended" : "standard"));
    }

    VirtualBus* bus = nullptr;
    VirtualBus::NodeId id = 0;

    {
        const std::lock_guard lock{m_impl->mutex};

        if (!m_impl->running.load(std::memory_order_acquire)) {
            return Result::error(ErrorCode::ChannelNotOpen,
                                 "Channel is not started - call start() first");
        }
        if (m_impl->config.listenOnly) {
            return Result::error(ErrorCode::InvalidState,
                                 "Channel is in listen-only mode");
        }
        if (frame.fd && !m_impl->config.canFdEnabled) {
            return Result::error(ErrorCode::UnsupportedFeature,
                                 "Channel was not opened in CAN FD mode");
        }

        bus = m_impl->bus;
        id = m_impl->nodeId;
    }

    CanFrame outgoing = frame;
    outgoing.timestampNs = monotonicNanoseconds();
    outgoing.length = payloadLengthFromDlc(outgoing.dlc, outgoing.fd);

    m_impl->posted.fetch_add(1, std::memory_order_relaxed);
    bus->broadcast(id, outgoing);

    return Result::ok();
}

Result VirtualCanBackend::transmit(std::span<const CanFrame> frames)
{
    for (const CanFrame& frame : frames) {
        if (Result result = transmit(frame); result.failed()) {
            return result;
        }
    }
    return Result::ok();
}

CanBusStatus VirtualCanBackend::status() const
{
    const std::lock_guard lock{m_impl->mutex};
    return m_impl->busStatus;
}

CanCapabilities VirtualCanBackend::capabilities() const
{
    return virtualCapabilities();
}

void VirtualCanBackend::setFrameHandler(FrameHandler handler)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->frameHandler = std::move(handler);
}

void VirtualCanBackend::reportThreadStopped(std::string_view reason)
{
    StatusHandler handler;
    CanBusStatus status;

    {
        const std::lock_guard lock{m_impl->mutex};
        m_impl->running.store(false, std::memory_order_release);
        m_impl->busStatus.state = CanBusState::Offline;
        status = m_impl->busStatus;
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

void VirtualCanBackend::setStatusHandler(StatusHandler handler)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->statusHandler = std::move(handler);
}

void VirtualCanBackend::setTrafficPattern(const VirtualTrafficPattern& pattern)
{
    const std::lock_guard lock{m_impl->mutex};
    m_impl->traffic = pattern;
}

bool VirtualCanBackend::trafficCompleted() const noexcept
{
    return m_impl->trafficDone.load(std::memory_order_acquire);
}

bool VirtualCanBackend::waitForTrafficCompletion(std::chrono::milliseconds timeout)
{
    const Clock::time_point deadline = Clock::now() + timeout;

    while (Clock::now() < deadline) {
        if (trafficCompleted()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    return trafficCompleted();
}

bool VirtualCanBackend::waitForDelivery(std::chrono::milliseconds timeout)
{
    const Clock::time_point deadline = Clock::now() + timeout;

    while (Clock::now() < deadline) {
        const std::shared_ptr<NodeInbox> inbox = [this] {
            const std::lock_guard lock{m_impl->mutex};
            return m_impl->inbox;
        }();

        if (!inbox || inbox->empty()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    const std::lock_guard lock{m_impl->mutex};
    return !m_impl->inbox || m_impl->inbox->empty();
}

} // namespace torquebus
