// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Getting a measurement out of TorqueBus.
//
// A `.tblog` is compact, streams at bus rate and survives a crash - and nothing
// else on earth can read it. That is fine for a recording somebody made an hour
// ago and is about to replay, and useless for the one they have to send to a
// supplier, open in CANalyzer, or paste into a spreadsheet next to a column of
// their own notes.
//
// So there are two ways out, and they are for different people:
//
//   * **ASC** is Vector's text format, and the closest thing this industry has
//     to a lingua franca. Nearly every CAN tool reads it. It is what you attach
//     to an email.
//
//   * **CSV** is for a spreadsheet, and for the awk script somebody will write
//     at 11pm. Its columns are named in a header row and never move.
//
// Neither is a replacement for the log: both are several times larger, and both
// lose the exactness of the original in one way or another - ASC rounds the
// timestamp to microseconds, CSV to whatever the reader parses a decimal as.
// Exporting is a one-way door, and the `.tblog` is what stays on disk.
//
// Fed in batches rather than handed a file, so the same exporter serves a
// recording being read back and a live trace being written out. Neither caller
// has to materialise a million frames to hand over.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"

#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace torquebus {

class TraceExporter final {
public:
    enum class Format {
        /// Vector ASCII. Readable by CANalyzer, CANoe, BUSMASTER, python-can
        /// and most things that have ever read a CAN log.
        Asc,

        /// One row per frame, with a header naming the columns.
        Csv,
    };

    TraceExporter() = default;
    ~TraceExporter();

    TraceExporter(const TraceExporter&) = delete;
    TraceExporter& operator=(const TraceExporter&) = delete;
    TraceExporter(TraceExporter&&) = delete;
    TraceExporter& operator=(TraceExporter&&) = delete;

    /// Creates `path` and writes whatever preamble the format needs.
    ///
    /// `startWallClockUs` is the recording's start, for the date line ASC
    /// carries. Zero writes the epoch, which is honest about not knowing rather
    /// than quietly writing today's date onto a file recorded last month.
    [[nodiscard]] Result open(const std::string& path,
                              Format format,
                              std::uint64_t startWallClockUs = 0);

    [[nodiscard]] bool isOpen() const noexcept { return m_file.is_open(); }

    /// Writes a batch, in the order given.
    [[nodiscard]] Result write(std::span<const CanFrame> frames);

    /// Writes the closing lines and closes the file.
    ///
    /// **Must be called for an ASC file to be valid**: it ends with
    /// `End TriggerBlock`, and a reader that does not find one treats the file
    /// as truncated. Unlike the `.tblog`, where a truncated file is a supported
    /// outcome, an export that was interrupted is simply incomplete - so the
    /// destructor calls this too.
    [[nodiscard]] Result close();

    [[nodiscard]] std::uint64_t framesWritten() const noexcept { return m_frames; }

private:
    [[nodiscard]] Result writeAsc(const CanFrame& frame);
    [[nodiscard]] Result writeCsv(const CanFrame& frame);

    std::ofstream m_file;
    Format m_format{Format::Asc};
    std::uint64_t m_frames{0};
};

/// The extension a format is normally written with, without the dot.
[[nodiscard]] std::string_view extensionFor(TraceExporter::Format format) noexcept;

} // namespace torquebus
