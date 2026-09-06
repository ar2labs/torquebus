// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/log/TraceLog.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace torquebus {
namespace {

constexpr std::array<char, TraceLogHeader::kMagicSize> kMagic{'T', 'B', 'L', 'O', 'G', 0, 0, 0};

/// Bytes before the payload in one record.
constexpr std::size_t kRecordPrefixSize = 16;

/// Bit positions in a record's flag byte. Frozen: changing one changes the
/// format, and every `.tblog` ever written would read differently.
enum RecordFlag : std::uint8_t {
    FlagExtended = 1U << 0U,
    FlagFd       = 1U << 1U,
    FlagBrs      = 1U << 2U,
    FlagEsi      = 1U << 3U,
    FlagRtr      = 1U << 4U,
    FlagError    = 1U << 5U,
    FlagTx       = 1U << 6U,
};

// --- Little-endian, spelled out ------------------------------------------
//
// Free on every machine this runs on, and written out anyway so that the
// *format* says which byte order it is in rather than the reader having to know
// where the file came from.

void write16(std::byte*& out, std::uint16_t value)
{
    *out++ = static_cast<std::byte>(value & 0xFFU);
    *out++ = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write32(std::byte*& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        *out++ = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write64(std::byte*& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        *out++ = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

[[nodiscard]] std::uint16_t read16(const std::byte*& in)
{
    const auto low = static_cast<std::uint16_t>(*in++);
    const auto high = static_cast<std::uint16_t>(*in++);
    return static_cast<std::uint16_t>(low | (high << 8U));
}

[[nodiscard]] std::uint32_t read32(const std::byte*& in)
{
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(*in++) << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t read64(const std::byte*& in)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(*in++) << shift;
    }
    return value;
}

[[nodiscard]] std::uint8_t flagsFor(const CanFrame& frame)
{
    std::uint8_t flags = 0;

    if (frame.isExtended()) { flags |= FlagExtended; }
    if (frame.fd)           { flags |= FlagFd; }
    if (frame.brs)          { flags |= FlagBrs; }
    if (frame.esi)          { flags |= FlagEsi; }
    if (frame.rtr)          { flags |= FlagRtr; }
    if (frame.error)        { flags |= FlagError; }
    if (!frame.isRx())      { flags |= FlagTx; }

    return flags;
}

void applyFlags(CanFrame& frame, std::uint8_t flags)
{
    frame.format = (flags & FlagExtended) != 0 ? CanFrameFormat::Extended
                                               : CanFrameFormat::Standard;
    frame.fd = (flags & FlagFd) != 0;
    frame.brs = (flags & FlagBrs) != 0;
    frame.esi = (flags & FlagEsi) != 0;
    frame.rtr = (flags & FlagRtr) != 0;
    frame.error = (flags & FlagError) != 0;
    frame.direction = (flags & FlagTx) != 0 ? CanDirection::Tx : CanDirection::Rx;
}

} // namespace

// ---------------------------------------------------------------------------
// TraceLogWriter
// ---------------------------------------------------------------------------

TraceLogWriter::~TraceLogWriter()
{
    close();
}

Result TraceLogWriter::open(const std::string& path, std::uint64_t startWallClockUs)
{
    close();

    m_file.open(path, std::ios::binary | std::ios::trunc);
    if (!m_file.is_open()) {
        return Result::error(ErrorCode::FileAccessDenied,
                             std::format("Could not create the log file '{}'", path));
    }

    m_frames = 0;
    m_bytes = 0;
    m_buffer.clear();
    m_buffer.reserve(kBufferSize + kMaxCanPayload + kRecordPrefixSize);

    std::array<std::byte, TraceLogHeader::kSize> header{};
    std::byte* out = header.data();

    for (const char letter : kMagic) {
        *out++ = static_cast<std::byte>(letter);
    }

    write16(out, TraceLogHeader::kCurrentVersion);
    write16(out, static_cast<std::uint16_t>(TraceLogHeader::kSize));
    write32(out, 0);                    // flags, reserved
    write64(out, startWallClockUs);
    write64(out, 0);                    // reserved, so a later version has room

    m_file.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));

    if (!m_file) {
        close();
        return Result::error(ErrorCode::FileAccessDenied,
                             std::format("Could not write the header of '{}'", path));
    }

    m_bytes = TraceLogHeader::kSize;
    return Result::ok();
}

Result TraceLogWriter::append(std::span<const CanFrame> frames)
{
    if (!m_file.is_open()) {
        return Result::error(ErrorCode::InvalidState,
                             "append() called on a log that is not open");
    }

    for (const CanFrame& frame : frames) {
        const std::size_t length = std::min<std::size_t>(frame.length, kMaxCanPayload);

        const std::size_t offset = m_buffer.size();
        m_buffer.resize(offset + kRecordPrefixSize + length);

        std::byte* out = m_buffer.data() + offset;

        write64(out, frame.timestampNs);
        write32(out, frame.identifier);

        *out++ = static_cast<std::byte>(frame.channel);
        *out++ = static_cast<std::byte>(frame.dlc);
        *out++ = static_cast<std::byte>(length);
        *out++ = static_cast<std::byte>(flagsFor(frame));

        std::memcpy(out, frame.data.data(), length);

        ++m_frames;

        // Flushed between frames rather than mid-record, so the buffer never
        // holds half of one - which is what makes a crash cost whole frames and
        // never a corrupt tail.
        if (m_buffer.size() >= kBufferSize) {
            if (Result result = flush(); result.failed()) {
                return result;
            }
        }
    }

    return Result::ok();
}

Result TraceLogWriter::flush()
{
    if (!m_file.is_open()) {
        return Result::error(ErrorCode::InvalidState,
                             "flush() called on a log that is not open");
    }

    if (m_buffer.empty()) {
        return Result::ok();
    }

    m_file.write(reinterpret_cast<const char*>(m_buffer.data()),
                 static_cast<std::streamsize>(m_buffer.size()));

    if (!m_file) {
        return Result::error(ErrorCode::FileAccessDenied,
                             "The log file stopped accepting writes - the disk may be full");
    }

    m_bytes += m_buffer.size();
    m_buffer.clear();

    m_file.flush();

    return Result::ok();
}

void TraceLogWriter::close()
{
    if (!m_file.is_open()) {
        return;
    }

    // The return is deliberately dropped: close() is called from the destructor
    // and from error paths, and there is nothing useful to do with a failure at
    // that point. A caller who needs to know calls flush() first.
    static_cast<void>(flush());

    m_file.close();
}

// ---------------------------------------------------------------------------
// TraceLogReader
// ---------------------------------------------------------------------------

Result TraceLogReader::open(const std::string& path)
{
    m_file.close();
    m_file.clear();

    m_frames = 0;
    m_truncated = 0;
    m_atEnd = false;

    m_file.open(path, std::ios::binary);
    if (!m_file.is_open()) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("Could not open the log file '{}'", path));
    }

    std::array<std::byte, TraceLogHeader::kSize> header{};
    m_file.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));

    if (m_file.gcount() != static_cast<std::streamsize>(header.size())) {
        return Result::error(ErrorCode::ParseError,
                             std::format("'{}' is too short to be a TorqueBus log", path));
    }

    const std::byte* in = header.data();

    for (const char letter : kMagic) {
        if (*in++ != static_cast<std::byte>(letter)) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' is not a TorqueBus log - it does not start with TBLOG",
                            path));
        }
    }

    m_header.version = read16(in);
    m_header.headerSize = read16(in);
    m_header.flags = read32(in);
    m_header.startWallClockUs = read64(in);

    if (m_header.version > TraceLogHeader::kCurrentVersion) {
        return Result::error(
            ErrorCode::VersionMismatch,
            std::format("'{}' was written by a newer TorqueBus (log version {}, this build "
                        "understands {})",
                        path, m_header.version, TraceLogHeader::kCurrentVersion));
    }

    if (m_header.headerSize < TraceLogHeader::kSize) {
        return Result::error(ErrorCode::ParseError,
                             std::format("'{}' declares a {}-byte header, which is smaller "
                                         "than the format allows",
                                         path, m_header.headerSize));
    }

    // A newer minor version may have a longer header. Skipping the part this
    // build does not understand is what lets it read the records anyway, which
    // is the whole reason headerSize is in the file.
    if (m_header.headerSize > TraceLogHeader::kSize) {
        m_file.seekg(static_cast<std::streamoff>(m_header.headerSize), std::ios::beg);
    }

    return Result::ok();
}

std::size_t TraceLogReader::read(std::span<CanFrame> out)
{
    if (!m_file.is_open() || m_atEnd) {
        return 0;
    }

    std::size_t filled = 0;

    while (filled < out.size()) {
        std::array<std::byte, kRecordPrefixSize> prefix{};

        m_file.read(reinterpret_cast<char*>(prefix.data()),
                    static_cast<std::streamsize>(prefix.size()));

        const auto got = static_cast<std::size_t>(m_file.gcount());

        if (got == 0) {
            m_atEnd = true;
            break;
        }

        if (got < prefix.size()) {
            // A partial record at the end: the recording was cut short. Counted
            // and stopped at, not treated as corruption - see the header.
            m_truncated += got;
            m_atEnd = true;
            break;
        }

        const std::byte* in = prefix.data();

        CanFrame frame;
        frame.timestampNs = read64(in);
        frame.identifier = read32(in);
        frame.channel = static_cast<std::uint8_t>(*in++);
        frame.dlc = static_cast<std::uint8_t>(*in++);

        const auto length = static_cast<std::size_t>(*in++);
        applyFlags(frame, static_cast<std::uint8_t>(*in++));

        if (length > kMaxCanPayload) {
            // Not a length any frame can have. Either the file is damaged in
            // the middle or it is not the file it claims to be; either way,
            // going on would be inventing frames out of whatever comes next.
            m_truncated += prefix.size();
            m_atEnd = true;
            break;
        }

        if (length > 0) {
            m_file.read(reinterpret_cast<char*>(frame.data.data()),
                        static_cast<std::streamsize>(length));

            if (static_cast<std::size_t>(m_file.gcount()) < length) {
                m_truncated += prefix.size() + static_cast<std::size_t>(m_file.gcount());
                m_atEnd = true;
                break;
            }
        }

        frame.length = static_cast<std::uint8_t>(length);

        out[filled++] = frame;
        ++m_frames;
    }

    return filled;
}

} // namespace torquebus
