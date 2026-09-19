// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/isotp/IsoTpConnection.h"

#include <algorithm>
#include <format>

namespace torquebus {
namespace {

/// The four frame types, in the high nibble of the first protocol byte.
constexpr std::uint8_t kSingleFrame = 0x0U;
constexpr std::uint8_t kFirstFrame = 0x1U;
constexpr std::uint8_t kConsecutiveFrame = 0x2U;
constexpr std::uint8_t kFlowControl = 0x3U;

/// Flow status, in the low nibble of a flow control frame.
constexpr std::uint8_t kContinueToSend = 0x0U;
constexpr std::uint8_t kWait = 0x1U;
constexpr std::uint8_t kOverflow = 0x2U;

/// Longest message a 12-bit first-frame length can describe. Above this the
/// length moves into four extra bytes - the escape form.
constexpr std::size_t kShortLengthLimit = 0x0FFFU;

[[nodiscard]] std::uint64_t millisecondsToNs(std::uint32_t milliseconds) noexcept
{
    return static_cast<std::uint64_t>(milliseconds) * 1'000'000ULL;
}

} // namespace

IsoTpConnection::IsoTpConnection(IsoTpAddress address, IsoTpConfig config)
    : m_address{address}
    , m_config{config}
{ }

std::size_t IsoTpConnection::capacity(std::size_t pciBytes) const noexcept
{
    const std::size_t total = m_config.canFd ? kMaxCanPayload : kMaxClassicCanPayload;
    const std::size_t overhead = m_address.addressBytes() + pciBytes;

    return total > overhead ? total - overhead : 0U;
}

CanFrame IsoTpConnection::makeFrame(std::uint64_t nowNs) const
{
    CanFrame frame;
    frame.identifier = m_address.transmitId;
    frame.format = m_address.format;
    frame.channel = m_address.channel;
    frame.direction = CanDirection::Tx;
    frame.timestampNs = nowNs;
    frame.fd = m_config.canFd;
    frame.brs = m_config.canFd && m_config.bitRateSwitch;

    if (m_address.addressing == IsoTpAddressing::Extended) {
        frame.data[0] = m_address.transmitExtension;
    }

    return frame;
}

void IsoTpConnection::finishFrame(CanFrame& frame, std::size_t used) const
{
    std::size_t length = used;

    if (m_config.padding) {
        // FD has no length between 8 and 12, or between 32 and 48, so a frame
        // is padded to the next size that exists whether padding was asked for
        // or not - this is the one place where "padding" is not a preference.
        length = m_config.canFd ? fdFrameLength(used) : kMaxClassicCanPayload;
    } else if (m_config.canFd && used > kMaxClassicCanPayload) {
        length = fdFrameLength(used);
    }

    for (std::size_t index = used; index < length; ++index) {
        frame.data[index] = m_config.padByte;
    }

    frame.length = static_cast<std::uint8_t>(length);
    frame.dlc = static_cast<std::uint8_t>(length);
}

void IsoTpConnection::queue(CanFrame frame)
{
    m_outgoing.push_back(frame);
}

Result IsoTpConnection::send(std::span<const std::uint8_t> payload, std::uint64_t nowNs)
{
    if (isSending()) {
        // Refused rather than queued. A queue here would hide the thing worth
        // knowing, which is that the last message was never answered.
        return Result::error(ErrorCode::InvalidState,
                             "This connection is already sending a message");
    }

    if (payload.empty()) {
        return Result::error(ErrorCode::InvalidArgument, "An ISO-TP message cannot be empty");
    }

    if (payload.size() > kMaximumMessage) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("A message of {} bytes is longer than this connection assembles",
                        payload.size()));
    }

    const std::size_t offset = m_address.addressBytes();

    // --- Single frame -----------------------------------------------------
    //
    // The classic form puts the length in the low nibble of one byte, so it
    // reaches seven bytes. CAN FD needs more, and rather than widen that nibble
    // the standard added an escape: a zero length, then a real length byte.
    // Two different capacities, because the escape form costs a second
    // protocol byte: seven bytes fit behind one, and anything above that -
    // which only CAN FD can carry in a single frame at all - fits behind two.
    const bool fitsInOneFrame = payload.size() <= 7U
                                    ? payload.size() <= capacity(1)
                                    : (m_config.canFd && payload.size() <= capacity(2));

    if (fitsInOneFrame) {
        CanFrame frame = makeFrame(nowNs);
        std::size_t used = offset;

        if (payload.size() <= 7U) {
            frame.data[used++] = static_cast<std::uint8_t>((kSingleFrame << 4U) | payload.size());
        } else {
            frame.data[used++] = kSingleFrame << 4U;
            frame.data[used++] = static_cast<std::uint8_t>(payload.size());
        }

        std::copy(
            payload.begin(), payload.end(), frame.data.begin() + static_cast<std::ptrdiff_t>(used));
        used += payload.size();

        finishFrame(frame, used);
        queue(frame);

        // Nothing to wait for: a single frame has no handshake, which is why
        // everything that fits in one is worth keeping in one.
        m_events.push_back(IsoTpEvent{IsoTpEvent::Kind::SendComplete, IsoTpError::None, {}, nowNs});
        return Result::ok();
    }

    // --- First frame ------------------------------------------------------
    m_send = SendSide{};
    m_send.payload.assign(payload.begin(), payload.end());

    CanFrame frame = makeFrame(nowNs);
    std::size_t used = offset;

    if (payload.size() <= kShortLengthLimit) {
        frame.data[used++] =
            static_cast<std::uint8_t>((kFirstFrame << 4U) | ((payload.size() >> 8U) & 0x0FU));
        frame.data[used++] = static_cast<std::uint8_t>(payload.size() & 0xFFU);
    } else {
        // The escape form: a zero 12-bit length, then four bytes of real one.
        frame.data[used++] = kFirstFrame << 4U;
        frame.data[used++] = 0x00U;
        frame.data[used++] = static_cast<std::uint8_t>((payload.size() >> 24U) & 0xFFU);
        frame.data[used++] = static_cast<std::uint8_t>((payload.size() >> 16U) & 0xFFU);
        frame.data[used++] = static_cast<std::uint8_t>((payload.size() >> 8U) & 0xFFU);
        frame.data[used++] = static_cast<std::uint8_t>(payload.size() & 0xFFU);
    }

    const std::size_t inFirst =
        std::min(payload.size(), (m_config.canFd ? kMaxCanPayload : kMaxClassicCanPayload) - used);

    std::copy_n(payload.begin(), inFirst, frame.data.begin() + static_cast<std::ptrdiff_t>(used));
    used += inFirst;

    finishFrame(frame, used);
    queue(frame);

    m_send.sent = inFirst;
    m_send.sequence = 1;
    m_send.state = SendState::WaitingForFlowControl;
    m_send.deadlineNs = nowNs + millisecondsToNs(m_config.flowControlTimeoutMs);

    return Result::ok();
}

void IsoTpConnection::sendConsecutiveFrame(std::uint64_t nowNs)
{
    CanFrame frame = makeFrame(nowNs);
    std::size_t used = m_address.addressBytes();

    frame.data[used++] =
        static_cast<std::uint8_t>((kConsecutiveFrame << 4U) | (m_send.sequence & 0x0FU));

    const std::size_t remaining = m_send.payload.size() - m_send.sent;
    const std::size_t take = std::min(remaining, capacity(1));

    std::copy_n(m_send.payload.begin() + static_cast<std::ptrdiff_t>(m_send.sent),
                take,
                frame.data.begin() + static_cast<std::ptrdiff_t>(used));

    used += take;
    m_send.sent += take;

    // Wraps at 16, and the receiver checks it. This is what catches two senders
    // on one identifier - which is a fault worth catching, because the symptom
    // otherwise is a message that decodes into nonsense.
    m_send.sequence = static_cast<std::uint8_t>((m_send.sequence + 1U) & 0x0FU);

    finishFrame(frame, used);
    queue(frame);

    m_send.nextFrameNs = nowNs + static_cast<std::uint64_t>(m_send.separationUs) * 1000ULL;

    if (m_send.sent >= m_send.payload.size()) {
        completeSend(nowNs);
        return;
    }

    if (m_send.blockSize > 0) {
        --m_send.remainingInBlock;

        if (m_send.remainingInBlock == 0) {
            // The block is done; the receiver gets to speak again.
            m_send.state = SendState::WaitingForFlowControl;
            m_send.deadlineNs = nowNs + millisecondsToNs(m_config.flowControlTimeoutMs);
        }
    }
}

void IsoTpConnection::completeSend(std::uint64_t nowNs)
{
    m_send = SendSide{};
    m_events.push_back(IsoTpEvent{IsoTpEvent::Kind::SendComplete, IsoTpError::None, {}, nowNs});
}

void IsoTpConnection::failSend(IsoTpError error, std::uint64_t nowNs)
{
    m_send = SendSide{};
    m_events.push_back(IsoTpEvent{IsoTpEvent::Kind::SendFailed, error, {}, nowNs});
}

void IsoTpConnection::failReceive(IsoTpError error, std::uint64_t nowNs)
{
    // What had arrived is dropped rather than delivered short. Half a
    // diagnostic response decoded as a whole one is worse than none: the reader
    // has no way to tell.
    m_receive = ReceiveSide{};
    m_events.push_back(IsoTpEvent{IsoTpEvent::Kind::ReceiveFailed, error, {}, nowNs});
}

void IsoTpConnection::sendFlowControl(std::uint8_t status, std::uint64_t nowNs)
{
    CanFrame frame = makeFrame(nowNs);
    std::size_t used = m_address.addressBytes();

    frame.data[used++] = static_cast<std::uint8_t>((kFlowControl << 4U) | (status & 0x0FU));
    frame.data[used++] = m_config.blockSize;
    frame.data[used++] = m_config.separationTime;

    finishFrame(frame, used);
    queue(frame);
}

bool IsoTpConnection::onFrame(const CanFrame& frame, std::uint64_t nowNs)
{
    if (frame.channel != m_address.channel || frame.identifier != m_address.receiveId
        || frame.isExtended() != (m_address.format == CanFrameFormat::Extended)) {
        return false;
    }

    if (frame.error || frame.rtr) {
        return false;
    }

    std::size_t offset = 0;

    if (m_address.addressing == IsoTpAddressing::Extended) {
        if (frame.length < 1 || frame.data[0] != m_address.receiveExtension) {
            // Somebody else's, on an identifier we share. Not an error - that
            // sharing is the entire reason extended addressing exists.
            return false;
        }
        offset = 1;
    }

    if (frame.length <= offset) {
        return false;
    }

    const std::span<const std::uint8_t> data{frame.data.data() + offset,
                                             static_cast<std::size_t>(frame.length) - offset};

    switch (static_cast<std::uint8_t>(data[0] >> 4U)) {
    case kSingleFrame:
        onSingleFrame(data, nowNs);
        return true;
    case kFirstFrame:
        onFirstFrame(data, nowNs);
        return true;
    case kConsecutiveFrame:
        onConsecutiveFrame(data, nowNs);
        return true;
    case kFlowControl:
        onFlowControl(data, nowNs);
        return true;
    default:
        break;
    }

    // A type this build does not know. Claimed as ours - it was addressed to us
    // - but nothing is done with it.
    return true;
}

void IsoTpConnection::onSingleFrame(std::span<const std::uint8_t> data, std::uint64_t nowNs)
{
    std::size_t length = data[0] & 0x0FU;
    std::size_t start = 1;

    if (length == 0) {
        // The FD escape: a zero length in the nibble, the real one in the next
        // byte. A classic frame that does this is malformed, but reading it
        // costs nothing and refusing it would be a judgement about somebody
        // else's stack.
        if (data.size() < 2) {
            return;
        }

        length = data[1];
        start = 2;
    }

    if (length == 0 || start + length > data.size()) {
        // A length that runs off the end of the frame. Silently ignored rather
        // than reported: this is what a frame from another protocol sharing the
        // identifier looks like, and reporting it would fill the log.
        return;
    }

    IsoTpEvent event;
    event.kind = IsoTpEvent::Kind::MessageReceived;
    event.timestampNs = nowNs;
    event.data.assign(data.begin() + static_cast<std::ptrdiff_t>(start),
                      data.begin() + static_cast<std::ptrdiff_t>(start + length));

    m_events.push_back(std::move(event));
}

void IsoTpConnection::onFirstFrame(std::span<const std::uint8_t> data, std::uint64_t nowNs)
{
    if (data.size() < 2) {
        return;
    }

    std::size_t length = (static_cast<std::size_t>(data[0] & 0x0FU) << 8U) | data[1];
    std::size_t start = 2;

    if (length == 0) {
        if (data.size() < 6) {
            return;
        }

        length = (static_cast<std::size_t>(data[2]) << 24U)
                 | (static_cast<std::size_t>(data[3]) << 16U)
                 | (static_cast<std::size_t>(data[4]) << 8U) | data[5];
        start = 6;
    }

    if (length > kMaximumMessage) {
        // Answered with an overflow flow control rather than ignored, because
        // the sender is entitled to know why nothing is happening - and because
        // this is exactly what a fuzzer's four-gigabyte length field wants us
        // to allocate.
        sendFlowControl(kOverflow, nowNs);
        failReceive(IsoTpError::TooLong, nowNs);
        return;
    }

    m_receive = ReceiveSide{};
    m_receive.state = ReceiveState::Assembling;
    m_receive.expected = length;
    m_receive.sequence = 1;
    m_receive.payload.reserve(length);

    const std::size_t take = std::min(data.size() - start, length);
    m_receive.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(start),
                             data.begin() + static_cast<std::ptrdiff_t>(start + take));

    m_receive.remainingInBlock = m_config.blockSize;
    m_receive.deadlineNs = nowNs + millisecondsToNs(m_config.consecutiveTimeoutMs);

    sendFlowControl(kContinueToSend, nowNs);
}

void IsoTpConnection::onConsecutiveFrame(std::span<const std::uint8_t> data, std::uint64_t nowNs)
{
    if (m_receive.state != ReceiveState::Assembling) {
        // A consecutive frame with nothing started. Usually the tail of a
        // message whose first frame was missed - a tester that joined the bus
        // half way through a transfer. Ignored: there is nothing to attach it
        // to, and reporting it would fire on every such join.
        return;
    }

    const std::uint8_t sequence = data[0] & 0x0FU;

    if (sequence != m_receive.sequence) {
        failReceive(IsoTpError::SequenceError, nowNs);
        return;
    }

    m_receive.sequence = static_cast<std::uint8_t>((m_receive.sequence + 1U) & 0x0FU);

    const std::size_t remaining = m_receive.expected - m_receive.payload.size();
    const std::size_t take = std::min(remaining, data.size() - 1);

    m_receive.payload.insert(m_receive.payload.end(),
                             data.begin() + 1,
                             data.begin() + 1 + static_cast<std::ptrdiff_t>(take));

    if (m_receive.payload.size() >= m_receive.expected) {
        IsoTpEvent event;
        event.kind = IsoTpEvent::Kind::MessageReceived;
        event.timestampNs = nowNs;
        event.data = std::move(m_receive.payload);

        m_receive = ReceiveSide{};
        m_events.push_back(std::move(event));
        return;
    }

    m_receive.deadlineNs = nowNs + millisecondsToNs(m_config.consecutiveTimeoutMs);

    if (m_config.blockSize > 0) {
        --m_receive.remainingInBlock;

        if (m_receive.remainingInBlock == 0) {
            // A block's worth has arrived; ask for the next one.
            m_receive.remainingInBlock = m_config.blockSize;
            sendFlowControl(kContinueToSend, nowNs);
        }
    }
}

void IsoTpConnection::onFlowControl(std::span<const std::uint8_t> data, std::uint64_t nowNs)
{
    if (m_send.state != SendState::WaitingForFlowControl) {
        // Not waiting for one. Either it is late - the transfer already failed
        // - or it belongs to somebody else's conversation.
        return;
    }

    if (data.size() < 3) {
        failSend(IsoTpError::ProtocolError, nowNs);
        return;
    }

    switch (static_cast<std::uint8_t>(data[0] & 0x0FU)) {
    case kContinueToSend:
        break;

    case kWait:
        // Legal, and bounded. An ECU that says WAIT forever has stopped
        // answering, and waiting forever is not a diagnosis.
        if (++m_send.waitFrames > m_config.maximumWaitFrames) {
            failSend(IsoTpError::TooManyWaitFrames, nowNs);
            return;
        }

        m_send.deadlineNs = nowNs + millisecondsToNs(m_config.flowControlTimeoutMs);
        return;

    case kOverflow:
        // The receiver has no buffer for this message. Not retried: nothing
        // about retrying makes the buffer bigger.
        failSend(IsoTpError::Overflow, nowNs);
        return;

    default:
        failSend(IsoTpError::InvalidFlowStatus, nowNs);
        return;
    }

    m_send.waitFrames = 0;
    m_send.blockSize = data[1];
    m_send.remainingInBlock = data[1];
    m_send.separationUs = separationMicroseconds(data[2]);

    m_send.state = SendState::Sending;

    // The first frame of the block goes out on the next poll rather than here,
    // so that STmin is honoured by one code path instead of two.
    m_send.nextFrameNs = nowNs;
}

void IsoTpConnection::poll(std::uint64_t nowNs)
{
    if (m_send.state == SendState::WaitingForFlowControl && nowNs >= m_send.deadlineNs) {
        failSend(IsoTpError::FlowControlTimeout, nowNs);
    }

    while (m_send.state == SendState::Sending && nowNs >= m_send.nextFrameNs) {
        sendConsecutiveFrame(nowNs);
    }

    if (m_receive.state == ReceiveState::Assembling && nowNs >= m_receive.deadlineNs) {
        failReceive(IsoTpError::ConsecutiveTimeout, nowNs);
    }
}

void IsoTpConnection::reset()
{
    m_send = SendSide{};
    m_receive = ReceiveSide{};
    m_outgoing.clear();
    m_events.clear();
}

} // namespace torquebus
