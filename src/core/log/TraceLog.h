// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// `.tblog` - a measurement written down.
//
// The trace holds a million frames and then starts forgetting. A log is what
// you record when the interesting thing happens twenty minutes in, or on a
// vehicle in a field with nobody watching the screen. So the number that shapes
// this file is not a capacity, it is a *rate*: it has to keep up with a
// saturated bus for as long as there is disk.
//
// Four decisions follow from that, and one from the way recordings actually
// end.
//
//   * **Append-only, with nothing patched at the end.** No frame count in the
//     header, no index written on close. Everything a reader needs is either in
//     the first 32 bytes or in the records themselves.
//
//   * **The recording that matters is the one that crashed.** A log is often
//     stopped by a flat battery, a yanked cable or a killed process, and the
//     twenty minutes before that are exactly what somebody wanted. So a
//     truncated file is not corrupt: it reads cleanly up to the last complete
//     record, and says how many bytes it ignored.
//
//   * **Compact records, not a dump of CanFrame.** A CanFrame is 88 bytes,
//     mostly a 64-byte payload array that classic CAN uses eight of. A record
//     is 16 bytes plus the payload it actually carries - 24 bytes for a typical
//     frame, so a million of them is 24 MB rather than 88.
//
//   * **Little-endian, spelled out.** Every machine TorqueBus runs on is
//     little-endian and the conversion is therefore free, but the format says
//     so rather than inheriting it - a log is a file somebody will read on
//     another machine, possibly with another tool.
//
//   * **The header carries a version and its own size.** A later version can
//     add fields, and this reader skips what it does not understand rather than
//     refusing a file it could mostly read.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace torquebus {

/// What the first bytes of every `.tblog` say.
struct TraceLogHeader final {
    /// Bumped when a record's layout changes in a way an older reader would
    /// misread. Adding a *header* field does not need a bump - headerSize
    /// covers that.
    static constexpr std::uint16_t kCurrentVersion = 1;

    /// "TBLOG" and three zeroes. Checked before anything else, so a file that
    /// is not one of ours is refused with that as the reason rather than as a
    /// parse error forty megabytes in.
    static constexpr std::size_t kMagicSize = 8;
    static constexpr std::size_t kSize = 32;

    std::uint16_t version{kCurrentVersion};
    std::uint16_t headerSize{kSize};
    std::uint32_t flags{};

    /// When the recording started, in microseconds since the Unix epoch.
    ///
    /// The only wall-clock time in the file. Frame timestamps are nanoseconds
    /// since the measurement began, which is what makes them comparable with
    /// the trace and with each other; this is what turns them into a date on a
    /// report.
    std::uint64_t startWallClockUs{};
};

/// Writes frames to a `.tblog`.
///
/// Buffered: the pipeline hands over batches, and this accumulates until it has
/// enough to be worth a write. A syscall per frame at a hundred thousand frames
/// a second would put the filesystem on the frame path, which is the one thing
/// the whole pipeline design exists to prevent.
class TraceLogWriter final {
public:
    /// Bytes buffered before a write. One filesystem block's worth many times
    /// over - large enough that the syscall is amortised, small enough that a
    /// crash loses a fraction of a second rather than a minute.
    static constexpr std::size_t kBufferSize = 64 * 1024;

    TraceLogWriter() = default;
    ~TraceLogWriter();

    TraceLogWriter(const TraceLogWriter&) = delete;
    TraceLogWriter& operator=(const TraceLogWriter&) = delete;
    TraceLogWriter(TraceLogWriter&&) = delete;
    TraceLogWriter& operator=(TraceLogWriter&&) = delete;

    /// Creates `path`, replacing anything already there, and writes the header.
    [[nodiscard]] Result open(const std::string& path, std::uint64_t startWallClockUs = 0);

    [[nodiscard]] bool isOpen() const noexcept { return m_file.is_open(); }

    /// Appends a batch. Frames are written in the order given.
    [[nodiscard]] Result append(std::span<const CanFrame> frames);

    /// Pushes the buffer to the file. Called on close, and worth calling when a
    /// user asks to be sure of what is on disk.
    [[nodiscard]] Result flush();

    /// Flushes and closes. Safe on an already-closed writer.
    void close();

    [[nodiscard]] std::uint64_t framesWritten() const noexcept { return m_frames; }

    /// Bytes handed to the file, not counting what is still buffered.
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept { return m_bytes; }

private:
    std::ofstream m_file;
    std::vector<std::byte> m_buffer;

    std::uint64_t m_frames{0};
    std::uint64_t m_bytes{0};
};

/// Reads frames back from a `.tblog`.
class TraceLogReader final {
public:
    TraceLogReader() = default;

    TraceLogReader(const TraceLogReader&) = delete;
    TraceLogReader& operator=(const TraceLogReader&) = delete;
    TraceLogReader(TraceLogReader&&) = delete;
    TraceLogReader& operator=(TraceLogReader&&) = delete;

    /// Opens `path` and reads its header.
    ///
    /// Fails on a file that is not a `.tblog`, or whose version this build does
    /// not understand. Does not fail on a truncated one - see read().
    [[nodiscard]] Result open(const std::string& path);

    [[nodiscard]] bool isOpen() const noexcept { return m_file.is_open(); }

    [[nodiscard]] const TraceLogHeader& header() const noexcept { return m_header; }

    /// Reads up to `out.size()` frames, in the order they were written.
    ///
    /// Returns how many were filled in; fewer than asked for means the end of
    /// the file. Stops cleanly at a partial trailing record rather than
    /// reporting an error: a recording that was cut short is the ordinary case,
    /// not a broken file.
    [[nodiscard]] std::size_t read(std::span<CanFrame> out);

    [[nodiscard]] bool atEnd() const noexcept { return m_atEnd; }

    /// Bytes at the end of the file that were not a complete record.
    ///
    /// Non-zero means the recording was cut short - a flat battery, a yanked
    /// cable, a killed process. Reported rather than hidden, because "the log
    /// ends at 19:42" and "the log ends at 19:42 and the last write did not
    /// finish" are different things to be told.
    [[nodiscard]] std::uint64_t truncatedBytes() const noexcept { return m_truncated; }

    [[nodiscard]] std::uint64_t framesRead() const noexcept { return m_frames; }

private:
    std::ifstream m_file;
    TraceLogHeader m_header;

    std::uint64_t m_frames{0};
    std::uint64_t m_truncated{0};
    bool m_atEnd{false};
};

} // namespace torquebus
