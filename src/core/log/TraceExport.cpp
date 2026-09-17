// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/log/TraceExport.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <format>

namespace torquebus {
namespace {

/// The date line at the top of an ASC file.
///
/// Readers treat this as informational - none of them parse a timestamp out of
/// it, because every line below carries its own. It is there so that a person
/// opening the file in a text editor a year later knows what they are looking
/// at, which is the only reason it is worth getting right.
[[nodiscard]] std::string ascDateLine(std::uint64_t startWallClockUs)
{
    const auto seconds = static_cast<std::time_t>(startWallClockUs / 1'000'000ULL);
    const auto milliseconds = (startWallClockUs / 1000ULL) % 1000ULL;

    std::tm parts{};

#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif

    std::array<char, 64> text{};
    // UTC, and said so. A log that travels between a vehicle in one timezone
    // and an office in another is the ordinary case, and a bare local time on
    // it is a number two people will read differently.
    const std::size_t written =
        std::strftime(text.data(), text.size(), "%a %b %d %H:%M:%S", &parts);

    if (written == 0) {
        return "date unknown";
    }

    return std::format("date {}.{:03} UTC {}", text.data(), milliseconds,
                       1900 + parts.tm_year);
}

/// Two uppercase hex digits, which is what every ASC file in the world uses.
void appendHexByte(std::string& out, std::uint8_t value)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    out.push_back(kDigits[(value >> 4U) & 0x0FU]);
    out.push_back(kDigits[value & 0x0FU]);
}

/// Seconds with six decimals - microsecond resolution, which is what ASC
/// carries and what a CAN tool expects to find.
[[nodiscard]] std::string formatTimestamp(std::uint64_t nanoseconds)
{
    const std::uint64_t microseconds = nanoseconds / 1000ULL;

    return std::format("{}.{:06}", microseconds / 1'000'000ULL, microseconds % 1'000'000ULL);
}

/// The identifier as ASC writes it: hex, with a trailing `x` for extended.
///
/// The `x` is how a reader tells 0x100 standard from 0x100 extended, which are
/// two different messages on the same bus. Dropping it is the commonest way an
/// exported file decodes wrongly at the other end.
[[nodiscard]] std::string formatIdentifier(const CanFrame& frame)
{
    return std::format("{:X}{}", frame.identifier, frame.isExtended() ? "x" : "");
}

} // namespace

std::string_view extensionFor(TraceExporter::Format format) noexcept
{
    return format == TraceExporter::Format::Asc ? "asc" : "csv";
}

TraceExporter::~TraceExporter()
{
    // Nothing escapes a destructor. close() flushes, flushing formats and
    // allocates, and an allocation that fails while the stack is already
    // unwinding calls terminate - taking the application down at the exact
    // moment it was trying to finish writing somebody's measurement.
    try {
        static_cast<void>(close());
    } catch (...) {
        // There is no caller left to tell, and the file is closed either way
        // when the stream is destroyed.
    }
}

Result TraceExporter::open(const std::string& path,
                           Format format,
                           std::uint64_t startWallClockUs)
{
    static_cast<void>(close());

    m_file.open(path, std::ios::trunc);
    if (!m_file.is_open()) {
        return Result::error(ErrorCode::FileAccessDenied,
                             std::format("Could not create '{}'", path));
    }

    m_format = format;
    m_frames = 0;

    if (format == Format::Asc) {
        m_file << ascDateLine(startWallClockUs) << '\n';

        // Both of these are read by the tools that open the file, and both
        // describe every line below. `base hex` says identifiers and data are
        // hexadecimal; `timestamps absolute` says each line's time is from the
        // start of the measurement rather than since the line before it.
        m_file << "base hex  timestamps absolute\n";
        m_file << "internal events logged\n";
        m_file << "// Exported by TorqueBus Studio\n";
        m_file << "Begin Triggerblock " << ascDateLine(startWallClockUs).substr(5) << '\n';
    } else {
        // Named columns, in an order that never changes. A script that read
        // column six last year has to still be reading the same thing.
        m_file << "timestamp_s,channel,direction,id,extended,dlc,length,flags,data\n";
    }

    return m_file ? Result::ok()
                  : Result::error(ErrorCode::FileAccessDenied,
                                  std::format("Could not write the header of '{}'", path));
}

Result TraceExporter::write(std::span<const CanFrame> frames)
{
    if (!m_file.is_open()) {
        return Result::error(ErrorCode::InvalidState,
                             "write() called on an export that is not open");
    }

    for (const CanFrame& frame : frames) {
        const Result result =
            m_format == Format::Asc ? writeAsc(frame) : writeCsv(frame);

        if (result.failed()) {
            return result;
        }

        ++m_frames;
    }

    return m_file ? Result::ok()
                  : Result::error(ErrorCode::FileAccessDenied,
                                  "The export file stopped accepting writes - the disk "
                                  "may be full");
}

Result TraceExporter::writeAsc(const CanFrame& frame)
{
    std::string line;
    line.reserve(96);

    // Right-aligned in a field, which is how every ASC file is laid out and
    // what makes a column of timestamps readable in a text editor.
    line += std::format("{:>11} ", formatTimestamp(frame.timestampNs));

    // 1-based, like everywhere else a channel is named to a person.
    line += std::format("{}  ", frame.channel + 1);

    if (frame.error) {
        // An error frame has no identifier and no data. Written in the form
        // readers expect rather than as a data frame with a flag, which is how
        // an error ends up counted as traffic at the other end.
        line += "ErrorFrame";
        m_file << line << '\n';
        return Result::ok();
    }

    line += std::format("{:<16} ", formatIdentifier(frame));
    line += frame.isRx() ? "Rx   " : "Tx   ";

    // `r` marks a remote request, which carries a length but no bytes.
    line += frame.rtr ? "r " : "d ";
    // Cast, and not for style: std::uint8_t is unsigned char, and whether
    // that formats as a number or as a character is a subtlety this file must
    // not depend on - a DLC written as a control byte would corrupt the line
    // for every reader but never look wrong here.
    line += std::format("{} ", static_cast<unsigned>(frame.dlc));

    if (!frame.rtr) {
        for (std::size_t index = 0; index < frame.length; ++index) {
            appendHexByte(line, frame.data[index]);
            line.push_back(' ');
        }
    }

    m_file << line << '\n';
    return Result::ok();
}

Result TraceExporter::writeCsv(const CanFrame& frame)
{
    std::string data;
    data.reserve(static_cast<std::size_t>(frame.length) * 2);

    for (std::size_t index = 0; index < frame.length; ++index) {
        appendHexByte(data, frame.data[index]);
    }

    // Flags as letters rather than as a number: a column reading "FD BRS" is
    // one somebody can act on, where 0x06 needs this file open beside it.
    std::string flags;
    const auto add = [&flags](const char* name) {
        if (!flags.empty()) {
            flags.push_back(' ');
        }
        flags += name;
    };

    if (frame.fd)    { add("FD"); }
    if (frame.brs)   { add("BRS"); }
    if (frame.esi)   { add("ESI"); }
    if (frame.rtr)   { add("RTR"); }
    if (frame.error) { add("ERR"); }

    m_file << formatTimestamp(frame.timestampNs) << ','
           << (frame.channel + 1) << ','
           << (frame.isRx() ? "Rx" : "Tx") << ','
           // Hex without a 0x prefix, and the extended flag in its own column -
           // so a spreadsheet does not have to be told how to read either.
           << std::format("{:X}", frame.identifier) << ','
           << (frame.isExtended() ? 1 : 0) << ','
           << static_cast<unsigned>(frame.dlc) << ','
           << static_cast<unsigned>(frame.length) << ','
           << flags << ','
           << data << '\n';

    return Result::ok();
}

Result TraceExporter::close()
{
    if (!m_file.is_open()) {
        return Result::ok();
    }

    if (m_format == Format::Asc) {
        // Without this a reader treats the file as truncated. See the header.
        m_file << "End TriggerBlock\n";
    }

    m_file.flush();

    const bool good = static_cast<bool>(m_file);
    m_file.close();

    return good ? Result::ok()
                : Result::error(ErrorCode::FileAccessDenied,
                                "The export could not be finished - the disk may be full");
}

} // namespace torquebus
