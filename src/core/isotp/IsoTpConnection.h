// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// One ISO-TP conversation: the frames of a message going out, the frames of one
// coming in, and the handshake between them.
//
// **It owns no thread, no timer and no bus.** Time arrives as a parameter and
// frames leave in a buffer the caller drains. That is not fastidiousness: a
// transport layer whose correctness depends on timing is one that can only be
// tested by waiting, and waiting tests are the ones that pass on a fast machine
// and fail in CI. Every timeout in ISO 15765-2 - N_Bs, N_Cr, STmin - is checked
// here against a clock the test supplies, so a thousand-millisecond timeout is
// a test that runs in no time at all.
//
// It also means the same class serves a live measurement, a replayed log and a
// simulated ECU without knowing which it is in - rule #11 again, and the reason
// the pipeline node around this one is thin.
//
// **One connection is one address pair, in one direction at a time.** Full
// duplex on one pair is legal in the standard and is not what a diagnostic
// session is: request, response, request. A send while a send is in flight is
// refused rather than queued, because a queue here would hide the fact that the
// ECU never answered the first one.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/isotp/IsoTpTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace torquebus {

/// Something the caller has to know about: a message arrived, or a transfer
/// ended.
struct IsoTpEvent final {
    enum class Kind : std::uint8_t {
        /// A complete message was reassembled. `data` is it.
        MessageReceived,

        /// The message passed to send() is fully on the bus.
        SendComplete,

        /// The send stopped. `error` says why.
        SendFailed,

        /// A message being received stopped. What had arrived is discarded:
        /// half a diagnostic response decoded as a whole one is worse than
        /// nothing at all.
        ReceiveFailed,
    };

    Kind kind{Kind::MessageReceived};
    IsoTpError error{IsoTpError::None};
    std::vector<std::uint8_t> data;

    /// When it happened, on the same clock the caller passes in.
    std::uint64_t timestampNs{};
};

class IsoTpConnection final {
public:
    /// Longest message this connection will assemble.
    ///
    /// ISO-TP's escape length field is 32 bits, which allows four gigabytes -
    /// a number no ECU means and every fuzzer tries. This is a firmware image,
    /// which is the largest thing anybody legitimately sends over ISO-TP.
    static constexpr std::size_t kMaximumMessage = 16U * 1024U * 1024U;

    IsoTpConnection(IsoTpAddress address, IsoTpConfig config);

    [[nodiscard]] const IsoTpAddress& address() const noexcept { return m_address; }
    [[nodiscard]] const IsoTpConfig& config() const noexcept { return m_config; }

    /// Starts sending `payload`.
    ///
    /// Fails while a send is in flight. The first frames are queued
    /// immediately, so a caller that drains frames straight after this call
    /// finds them there.
    [[nodiscard]] Result send(std::span<const std::uint8_t> payload, std::uint64_t nowNs);

    [[nodiscard]] bool isSending() const noexcept { return m_send.state != SendState::Idle; }
    [[nodiscard]] bool isReceiving() const noexcept
    {
        return m_receive.state != ReceiveState::Idle;
    }

    /// Offers a frame that arrived on the bus. Frames that are not this
    /// connection's - wrong channel, wrong identifier, wrong address extension
    /// - are ignored, so the caller can hand it everything.
    ///
    /// Returns true when the frame belonged to this connection.
    bool onFrame(const CanFrame& frame, std::uint64_t nowNs);

    /// Lets time pass: sends whatever STmin now allows, and fires the timeouts
    /// that have come due. Call it as often as convenient; nothing here
    /// depends on being called at a particular rate.
    void poll(std::uint64_t nowNs);

    /// Frames waiting to go onto the bus, oldest first.
    [[nodiscard]] std::span<const CanFrame> pendingFrames() const noexcept
    {
        return m_outgoing;
    }

    void clearPendingFrames() { m_outgoing.clear(); }

    [[nodiscard]] std::span<const IsoTpEvent> events() const noexcept { return m_events; }

    void clearEvents() { m_events.clear(); }

    /// Abandons whatever is in flight, quietly. For a measurement stopping -
    /// not for an error, which produces an event.
    void reset();

private:
    enum class SendState : std::uint8_t {
        Idle,
        WaitingForFlowControl,
        Sending,
    };

    enum class ReceiveState : std::uint8_t {
        Idle,
        Assembling,
    };

    struct SendSide final {
        SendState state{SendState::Idle};

        std::vector<std::uint8_t> payload;
        std::size_t sent{0};

        /// Sequence number of the next consecutive frame, 0..15 and wrapping.
        std::uint8_t sequence{1};

        /// What the receiver last asked for. Zero block size means "the rest".
        std::uint8_t blockSize{0};
        std::uint8_t remainingInBlock{0};
        std::uint32_t separationUs{0};

        std::uint8_t waitFrames{0};

        /// When the next consecutive frame may go out, and when waiting for a
        /// flow control stops being reasonable.
        std::uint64_t nextFrameNs{0};
        std::uint64_t deadlineNs{0};
    };

    struct ReceiveSide final {
        ReceiveState state{ReceiveState::Idle};

        std::vector<std::uint8_t> payload;
        std::size_t expected{0};

        std::uint8_t sequence{1};
        std::uint8_t remainingInBlock{0};

        std::uint64_t deadlineNs{0};
    };

    // --- Frame building ---------------------------------------------------

    /// A frame addressed as this connection is, with the address extension byte
    /// already in place when there is one.
    [[nodiscard]] CanFrame makeFrame(std::uint64_t nowNs) const;

    /// Pads and sets the length, honouring FD's fixed sizes.
    void finishFrame(CanFrame& frame, std::size_t used) const;

    void queue(CanFrame frame);

    /// Bytes of payload one frame of this connection can carry, given how many
    /// bytes the protocol control information takes.
    [[nodiscard]] std::size_t capacity(std::size_t pciBytes) const noexcept;

    void sendFlowControl(std::uint8_t status, std::uint64_t nowNs);
    void sendConsecutiveFrame(std::uint64_t nowNs);

    void failSend(IsoTpError error, std::uint64_t nowNs);
    void failReceive(IsoTpError error, std::uint64_t nowNs);
    void completeSend(std::uint64_t nowNs);

    // --- Frame handling ---------------------------------------------------

    void onSingleFrame(std::span<const std::uint8_t> data, std::uint64_t nowNs);
    void onFirstFrame(std::span<const std::uint8_t> data, std::uint64_t nowNs);
    void onConsecutiveFrame(std::span<const std::uint8_t> data, std::uint64_t nowNs);
    void onFlowControl(std::span<const std::uint8_t> data, std::uint64_t nowNs);

    IsoTpAddress m_address;
    IsoTpConfig m_config;

    SendSide m_send;
    ReceiveSide m_receive;

    std::vector<CanFrame> m_outgoing;
    std::vector<IsoTpEvent> m_events;
};

} // namespace torquebus
