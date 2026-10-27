// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Bringing external measurements into TorqueBus.
//
// The counterpart to TraceExport. Where TraceExport writes Vector ASC and CSV
// for other tools to consume, TraceImporter reads logs from the rest of the
// automotive world - Vector ASC (.asc) and PEAK-System Trace (.trc) - and turns
// them into native TorqueBus measurements (.tblog).
//
// Once in .tblog form, external recordings can be replayed through the pipeline,
// decoded against DBCs, plotted on graphs, scrutinized in the Trace view, and
// navigated using the Timeline transport just like native measurements.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/log/TraceLog.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

enum class ExternalTraceFormat {
    Auto,
    VectorAsc,
    PcanTrc,
};

struct ImportSummary final {
    std::uint64_t frames{0};
    std::uint64_t skippedLines{0};
    std::uint64_t firstTimestampNs{0};
    std::uint64_t lastTimestampNs{0};
    std::uint64_t durationNs{0};
    std::uint64_t startWallClockUs{0};
    ExternalTraceFormat format{ExternalTraceFormat::Auto};
};

class TraceImporter final {
public:
    /// Detects the format of a log from its extension and/or initial lines.
    [[nodiscard]] static ExternalTraceFormat detectFormat(const std::string& path);

    /// True if `path` has an external log extension (.asc or .trc).
    [[nodiscard]] static bool isExternalLog(const std::string& path) noexcept;

    /// Imports an external log file (.asc or .trc) directly into a .tblog file.
    [[nodiscard]] static Result importToLog(const std::string& inputPath,
                                            const std::string& outputTblogPath,
                                            ImportSummary* summary = nullptr);

    /// Imports an external log file into an in-memory vector of frames.
    [[nodiscard]] static Result importFrames(const std::string& inputPath,
                                             std::vector<CanFrame>& outFrames,
                                             std::size_t maxFrames = 0,
                                             ImportSummary* summary = nullptr);

    /// Ensures a .tblog representation exists for `inputPath`. If `inputPath` is
    /// already a .tblog, `effectiveTblogPath` receives it directly. If it is an
    /// external log (.asc or .trc), it is converted to a companion or cached .tblog.
    [[nodiscard]] static Result autoImport(const std::string& inputPath,
                                           std::string& effectiveTblogPath,
                                           ImportSummary* summary = nullptr);

    /// Parses a single line of Vector ASC. Returns true if a CanFrame was produced.
    [[nodiscard]] static bool
    parseAscLine(std::string_view line, CanFrame& outFrame, std::uint64_t& startWallClockUs);

    /// Parses a single line of PCAN TRC (v1.x or v2.x). Returns true if a CanFrame was produced.
    [[nodiscard]] static bool parseTrcLine(std::string_view line,
                                           CanFrame& outFrame,
                                           int& trcVersion,
                                           std::uint64_t& startWallClockUs);
};

[[nodiscard]] std::string_view formatName(ExternalTraceFormat format) noexcept;

} // namespace torquebus
