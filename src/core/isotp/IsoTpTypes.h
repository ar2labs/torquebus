// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// ISO 15765-2 (ISO-TP): how a diagnostic message longer than a CAN frame
// crosses a bus that only carries eight bytes at a time.
//
// Everything above this - UDS, OBD, a J1939 request that does not fit - is a
// conversation in messages, and every one of those messages arrives as a run of
// frames with a one-byte header and a handshake in the middle. Getting that
// handshake wrong does not produce a wrong answer; it produces a silence, or a
// tester that works on the bench and stops working in the vehicle, which is
// harder to diagnose than either.
//
// The vocabulary, because the rest of this directory uses it without
// explanation:
//
//   * **SF** single frame - the whole message fits in one CAN frame.
//   * **FF** first frame - the start of a longer one, carrying its total length.
//   * **CF** consecutive frame - the rest, numbered 0..15 and wrapping.
//   * **FC** flow control - the receiver's answer to a FF: how many frames it
//     will take before it wants to be asked again (**BS**, block size) and how
//     much time it needs between them (**STmin**).
//
// The handshake exists because the receiver is often a small ECU with one
// buffer and an interrupt it has to get back from. BS and STmin are that ECU
// saying how fast it can be talked to, and a tester that ignores them is a
// tester that works until it meets a slow ECU.

#pragma once

#include "core/can/CanFrame.h"

#include <cstdint>
#include <string_view>

namespace torquebus {

/// How the address of a message is carried.
enum class IsoTpAddressing : std::uint8_t {
    /// The CAN identifier is the address; every data byte is ISO-TP's. The
    /// ordinary case, and what a passenger car's 0x7E0/0x7E8 pair is.
    Normal,

    /// The first data byte is a target address and ISO-TP's own bytes follow
    /// it. Costs one byte of every frame, and lets many ECUs share one
    /// identifier - which is why it exists, and why it is still found on buses
    /// that predate cheap identifiers.
    Extended,

    /// 29-bit identifiers built from a source and target address, as
    /// ISO 15765-2 defines them: 0x18DA<target><source> for a physical
    /// request. Nothing is taken from a data byte; the identifier carries it
    /// all. Standard on heavy vehicles.
    NormalFixed,
};

/// Which addresses a connection talks between.
///
/// Both identifiers are given rather than derived, even for NormalFixed, so
/// that a bus which does something slightly non-standard - and there are many -
/// can still be addressed by saying what is actually on it. The helpers below
/// build the standard pairs for people who do not want to.
struct IsoTpAddress final {
    /// Identifier this end transmits on.
    std::uint32_t transmitId{0x7E0};

    /// Identifier this end listens on.
    std::uint32_t receiveId{0x7E8};

    IsoTpAddressing addressing{IsoTpAddressing::Normal};

    CanFrameFormat format{CanFrameFormat::Standard};

    /// Address extension byte, for Extended addressing only: the first data
    /// byte of every frame this end sends.
    std::uint8_t transmitExtension{0};

    /// The extension byte a frame must start with to be ours. Frames on the
    /// receive identifier whose first byte is anything else belong to another
    /// ECU sharing that identifier and are ignored.
    std::uint8_t receiveExtension{0};

    /// Which application channel this connection is on.
    std::uint8_t channel{0};

    /// The 29-bit pair for a physical request to `target` from `source`:
    /// 0x18DA<target><source> out, 0x18DA<source><target> back.
    [[nodiscard]] static IsoTpAddress
    normalFixed(std::uint8_t source, std::uint8_t target, std::uint8_t channel = 0);

    /// The 29-bit pair for a functional (broadcast) request: 0x18DB<target>
    /// <source> out, with replies arriving physically addressed.
    [[nodiscard]] static IsoTpAddress
    normalFixedFunctional(std::uint8_t source, std::uint8_t target, std::uint8_t channel = 0);

    /// The usual 11-bit tester pair: 0x7E0 out, 0x7E8 back for ECU 0.
    [[nodiscard]] static IsoTpAddress obd(std::uint8_t ecu = 0, std::uint8_t channel = 0);

    /// How many bytes of each frame the addressing itself costs.
    [[nodiscard]] constexpr std::size_t addressBytes() const noexcept
    {
        return addressing == IsoTpAddressing::Extended ? 1U : 0U;
    }
};

/// What this end asks of the other, and how long it is prepared to wait.
struct IsoTpConfig final {
    /// Frames this end will accept between flow controls. Zero means "send the
    /// whole message without asking again", which is what a tester on a quiet
    /// bus usually wants and what most ECUs answer.
    std::uint8_t blockSize{0};

    /// Minimum separation this end needs between consecutive frames, in the
    /// wire encoding: 0x00-0x7F is milliseconds, 0xF1-0xF9 is 100-900
    /// microseconds. See separationMicroseconds().
    std::uint8_t separationTime{0};

    /// Longest this end waits for a flow control after sending a first frame,
    /// or for the next one mid-message. N_Bs in the standard; 1000 ms is its
    /// recommended value.
    std::uint32_t flowControlTimeoutMs{1000};

    /// Longest this end waits for the next consecutive frame of a message it is
    /// receiving. N_Cr; 1000 ms.
    std::uint32_t consecutiveTimeoutMs{1000};

    /// How many consecutive WAIT flow controls this end will accept before
    /// giving up. N_WFTmax. An ECU that says WAIT forever is an ECU that has
    /// stopped answering, and waiting forever is not a diagnosis.
    std::uint8_t maximumWaitFrames{10};

    /// Pad every frame to its full length, with padByte.
    ///
    /// On by default because the standard requires it for CAN FD above 8 bytes
    /// and a great many ECUs require it below that too: an unpadded 3-byte
    /// single frame is legal and is nevertheless ignored by more ECUs than
    /// anybody expects.
    bool padding{true};

    /// What padding is made of. 0xCC rather than 0x00 because a run of zeroes
    /// in a trace looks like data somebody meant, and this is not data.
    std::uint8_t padByte{0xCC};

    /// Send with CAN FD frames, which changes what fits in one frame and
    /// allows the 64-byte first frame.
    bool canFd{false};

    /// Ask for the bit rate switch on FD frames.
    bool bitRateSwitch{false};
};

/// Why a transfer stopped.
enum class IsoTpError : std::uint8_t {
    None,

    /// No flow control arrived within N_Bs. The usual cause is an ECU that is
    /// not there, or one addressed on the wrong identifier.
    FlowControlTimeout,

    /// A consecutive frame did not arrive within N_Cr. The message is
    /// incomplete and what did arrive is discarded - half a diagnostic response
    /// decoded as a whole one is worse than none.
    ConsecutiveTimeout,

    /// A consecutive frame arrived out of order. Almost always two senders on
    /// one identifier, which is a wiring or configuration fault rather than a
    /// transient.
    SequenceError,

    /// The receiver said its buffer is too small for the message. Reported
    /// rather than retried: nothing about retrying makes the buffer bigger.
    Overflow,

    /// The receiver said WAIT more times than maximumWaitFrames allows.
    TooManyWaitFrames,

    /// The receiver sent a flow status this end does not understand.
    InvalidFlowStatus,

    /// The message is longer than this end will assemble.
    TooLong,

    /// A frame arrived that cannot be part of any transfer - a consecutive
    /// frame with nothing started, for instance.
    ProtocolError,
};

[[nodiscard]] std::string_view describe(IsoTpError error) noexcept;

/// STmin as microseconds.
///
/// The encoding is two ranges with a hole: 0x00-0x7F are milliseconds, and
/// 0xF1-0xF9 are 100 to 900 microseconds. Everything else is reserved, and the
/// standard says a receiver of a reserved value must use 127 ms - the slowest
/// legal separation. That is deliberate: an unknown value means the other end
/// is speaking a dialect this one does not know, and the safe reaction to that
/// is to slow down rather than to guess.
[[nodiscard]] std::uint32_t separationMicroseconds(std::uint8_t separationTime) noexcept;

/// The smallest valid CAN FD length that holds `length` bytes.
///
/// FD frames come in 8, 12, 16, 20, 24, 32, 48 and 64 bytes and nothing in
/// between, so a 34-byte message travels in a 48-byte frame with 14 bytes of
/// padding whether anybody likes it or not.
[[nodiscard]] std::size_t fdFrameLength(std::size_t length) noexcept;

} // namespace torquebus
