// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/log/TraceImport.h"

#include "core/FilePath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {
namespace {

[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

void tokenize(std::string_view line, std::vector<std::string_view>& tokens)
{
    tokens.clear();
    std::size_t i = 0;
    const std::size_t len = line.size();

    while (i < len) {
        while (i < len && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= len) {
            break;
        }
        const std::size_t start = i;
        while (i < len && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        tokens.push_back(line.substr(start, i - start));
    }
}

[[nodiscard]] bool parseHexU32(std::string_view s, std::uint32_t& out) noexcept
{
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

[[nodiscard]] bool parseHexU8(std::string_view s, std::uint8_t& out) noexcept
{
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    if (s.empty()) {
        return false;
    }
    unsigned int val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val, 16);
    if (ec == std::errc{} && ptr == s.data() + s.size() && val <= 0xFFU) {
        out = static_cast<std::uint8_t>(val);
        return true;
    }
    return false;
}

[[nodiscard]] bool parseDecU32(std::string_view s, std::uint32_t& out) noexcept
{
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 10);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

/// Parses float seconds like "12.345678" into nanoseconds without floating point inaccuracy.
[[nodiscard]] bool parseSecondsToNs(std::string_view s, std::uint64_t& outNs) noexcept
{
    if (s.empty()) {
        return false;
    }

    const std::size_t dotPos = s.find('.');
    if (dotPos == std::string_view::npos) {
        std::uint64_t sec = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), sec, 10);
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            return false;
        }
        outNs = sec * 1'000'000'000ULL;
        return true;
    }

    const std::string_view secPart = s.substr(0, dotPos);
    std::string_view fracPart = s.substr(dotPos + 1);

    std::uint64_t sec = 0;
    if (!secPart.empty()) {
        const auto [ptr, ec] =
            std::from_chars(secPart.data(), secPart.data() + secPart.size(), sec, 10);
        if (ec != std::errc{} || ptr != secPart.data() + secPart.size()) {
            return false;
        }
    }

    if (fracPart.size() > 9) {
        fracPart = fracPart.substr(0, 9);
    }

    std::uint64_t fracVal = 0;
    if (!fracPart.empty()) {
        const auto [ptr, ec] =
            std::from_chars(fracPart.data(), fracPart.data() + fracPart.size(), fracVal, 10);
        if (ec != std::errc{} || ptr != fracPart.data() + fracPart.size()) {
            return false;
        }
        for (std::size_t i = fracPart.size(); i < 9; ++i) {
            fracVal *= 10;
        }
    }

    outNs = (sec * 1'000'000'000ULL) + fracVal;
    return true;
}

/// Parses float milliseconds like "1234.567" into nanoseconds.
[[nodiscard]] bool parseMillisecondsToNs(std::string_view s, std::uint64_t& outNs) noexcept
{
    if (s.empty()) {
        return false;
    }

    const std::size_t dotPos = s.find('.');
    if (dotPos == std::string_view::npos) {
        std::uint64_t ms = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), ms, 10);
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            return false;
        }
        outNs = ms * 1'000'000ULL;
        return true;
    }

    const std::string_view msPart = s.substr(0, dotPos);
    std::string_view fracPart = s.substr(dotPos + 1);

    std::uint64_t ms = 0;
    if (!msPart.empty()) {
        const auto [ptr, ec] =
            std::from_chars(msPart.data(), msPart.data() + msPart.size(), ms, 10);
        if (ec != std::errc{} || ptr != msPart.data() + msPart.size()) {
            return false;
        }
    }

    if (fracPart.size() > 6) {
        fracPart = fracPart.substr(0, 6);
    }

    std::uint64_t fracVal = 0;
    if (!fracPart.empty()) {
        const auto [ptr, ec] =
            std::from_chars(fracPart.data(), fracPart.data() + fracPart.size(), fracVal, 10);
        if (ec != std::errc{} || ptr != fracPart.data() + fracPart.size()) {
            return false;
        }
        for (std::size_t i = fracPart.size(); i < 6; ++i) {
            fracVal *= 10;
        }
    }

    outNs = (ms * 1'000'000ULL) + fracVal;
    return true;
}

[[nodiscard]] int monthIndex(std::string_view m) noexcept
{
    static constexpr std::array<const char*, 12> kMonths = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (m.size() >= 3
            && std::equal(
                m.begin(), m.begin() + 3, kMonths[static_cast<std::size_t>(i)], [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a))
                           == std::tolower(static_cast<unsigned char>(b));
                })) {
            return i;
        }
    }
    return -1;
}

/// Parses date line from Vector ASC:
/// e.g. "date Tue Oct 27 12:00:00.000 UTC 2026" or "date 27.10.2026 12:00:00"
bool parseAscDateLine(std::string_view line, std::uint64_t& outWallClockUs)
{
    std::string_view s = trim(line);
    if (!s.starts_with("date ") && !s.starts_with("date\t")) {
        return false;
    }
    s = trim(s.substr(4));

    std::vector<std::string_view> parts;
    tokenize(s, parts);
    if (parts.size() < 4) {
        return false;
    }

    int year = 0;
    int month = 0; // 0..11
    int day = 1;
    int hour = 0;
    int min = 0;
    int sec = 0;
    int ms = 0;

    int monIdx = monthIndex(parts[1]);
    if (monIdx >= 0 && parts.size() >= 5) {
        // e.g. "Tue Oct 27 12:00:00[.000] [UTC] 2026"
        month = monIdx;
        day = std::atoi(std::string(parts[2]).c_str());
        const std::string_view timeStr = parts[3];
        const std::size_t c1 = timeStr.find(':');
        if (c1 != std::string_view::npos) {
            hour = std::atoi(std::string(timeStr.substr(0, c1)).c_str());
            const std::size_t c2 = timeStr.find(':', c1 + 1);
            if (c2 != std::string_view::npos) {
                min = std::atoi(std::string(timeStr.substr(c1 + 1, c2 - c1 - 1)).c_str());
                const std::string_view rest = timeStr.substr(c2 + 1);
                const std::size_t dot = rest.find('.');
                if (dot != std::string_view::npos) {
                    sec = std::atoi(std::string(rest.substr(0, dot)).c_str());
                    ms = std::atoi(std::string(rest.substr(dot + 1)).c_str());
                } else {
                    sec = std::atoi(std::string(rest).c_str());
                }
            }
        }

        year = std::atoi(std::string(parts.back()).c_str());
        if (year < 1970) {
            year = 2026;
        }
    } else {
        // e.g. "27.10.2026 12:00:00"
        const std::string_view dStr = parts[0];
        const std::size_t d1 = dStr.find('.');
        if (d1 != std::string_view::npos) {
            day = std::atoi(std::string(dStr.substr(0, d1)).c_str());
            const std::size_t d2 = dStr.find('.', d1 + 1);
            if (d2 != std::string_view::npos) {
                month = std::atoi(std::string(dStr.substr(d1 + 1, d2 - d1 - 1)).c_str()) - 1;
                year = std::atoi(std::string(dStr.substr(d2 + 1)).c_str());
            }
        }
        const std::string_view tStr = parts[1];
        const std::size_t c1 = tStr.find(':');
        if (c1 != std::string_view::npos) {
            hour = std::atoi(std::string(tStr.substr(0, c1)).c_str());
            const std::size_t c2 = tStr.find(':', c1 + 1);
            if (c2 != std::string_view::npos) {
                min = std::atoi(std::string(tStr.substr(c1 + 1, c2 - c1 - 1)).c_str());
                sec = std::atoi(std::string(tStr.substr(c2 + 1)).c_str());
            }
        }
    }

    if (year >= 1970 && month >= 0 && month < 12 && day >= 1 && day <= 31) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;

#ifdef _WIN32
        const std::time_t t = _mkgmtime(&tm);
#else
        const std::time_t t = timegm(&tm);
#endif
        if (t > 0) {
            outWallClockUs = static_cast<std::uint64_t>(t) * 1'000'000ULL + (ms * 1000ULL);
            return true;
        }
    }
    return false;
}

/// Parses OLE automation date from PCAN TRC:
/// e.g. ";$STARTTIME=44500.51234123"
bool parsePcanStartTime(std::string_view line, std::uint64_t& outWallClockUs)
{
    const std::size_t eqPos = line.find('=');
    if (eqPos == std::string_view::npos) {
        return false;
    }
    std::string_view valStr = trim(line.substr(eqPos + 1));
    char* endPtr = nullptr;
    const double oleDate = std::strtod(std::string(valStr).c_str(), &endPtr);
    if (oleDate <= 25569.0) { // must be after Jan 1, 1970
        return false;
    }
    const double unixSeconds = (oleDate - 25569.0) * 86400.0;
    outWallClockUs = static_cast<std::uint64_t>(unixSeconds * 1'000'000.0);
    return true;
}

} // namespace

std::string_view formatName(ExternalTraceFormat format) noexcept
{
    switch (format) {
    case ExternalTraceFormat::VectorAsc:
        return "Vector ASCII";
    case ExternalTraceFormat::PcanTrc:
        return "PCAN Trace";
    case ExternalTraceFormat::Auto:
        return "Auto";
    }
    return "Unknown";
}

bool TraceImporter::isExternalLog(const std::string& path) noexcept
{
    const std::filesystem::path p = pathFromUtf8(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".asc" || ext == ".trc";
}

ExternalTraceFormat TraceImporter::detectFormat(const std::string& path)
{
    const std::filesystem::path p = pathFromUtf8(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".asc") {
        return ExternalTraceFormat::VectorAsc;
    }
    if (ext == ".trc") {
        return ExternalTraceFormat::PcanTrc;
    }

    std::ifstream file{p};
    if (!file.is_open()) {
        return ExternalTraceFormat::Auto;
    }

    std::string line;
    for (int i = 0; i < 20 && std::getline(file, line); ++i) {
        if (line.find(";$FILEVERSION=") != std::string::npos
            || line.find("PCAN") != std::string::npos) {
            return ExternalTraceFormat::PcanTrc;
        }
        if (line.find("date ") != std::string::npos || line.find("base hex") != std::string::npos
            || line.find("Begin Triggerblock") != std::string::npos) {
            return ExternalTraceFormat::VectorAsc;
        }
    }

    return ExternalTraceFormat::Auto;
}

bool TraceImporter::parseAscLine(std::string_view line,
                                 CanFrame& outFrame,
                                 std::uint64_t& startWallClockUs)
{
    const std::string_view trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }

    if (trimmed.starts_with("//") || trimmed.starts_with("***") || trimmed.starts_with(';')) {
        return false;
    }

    if (trimmed.starts_with("date")) {
        parseAscDateLine(trimmed, startWallClockUs);
        return false;
    }

    if (trimmed.starts_with("base ") || trimmed.starts_with("internal ")
        || trimmed.starts_with("Begin Trigger") || trimmed.starts_with("End Trigger")
        || trimmed.starts_with("version ")) {
        return false;
    }

    thread_local std::vector<std::string_view> tokens;
    tokenize(trimmed, tokens);
    if (tokens.size() < 3) {
        return false;
    }

    std::uint64_t timestampNs = 0;
    if (!parseSecondsToNs(tokens[0], timestampNs)) {
        return false;
    }

    std::size_t idx = 1;
    bool isFd = false;
    if (tokens[idx] == "CANFD" || tokens[idx] == "canfd") {
        isFd = true;
        ++idx;
        if (idx >= tokens.size()) {
            return false;
        }
    }

    std::uint32_t channelNum = 1;
    if (parseDecU32(tokens[idx], channelNum)) {
        ++idx;
        if (idx >= tokens.size()) {
            return false;
        }
    }

    if (!isFd && idx < tokens.size() && (tokens[idx] == "CANFD" || tokens[idx] == "canfd")) {
        isFd = true;
        ++idx;
        if (idx >= tokens.size()) {
            return false;
        }
    }

    if (idx < tokens.size() && tokens[idx] == "ErrorFrame") {
        outFrame = CanFrame{};
        outFrame.timestampNs = timestampNs;
        outFrame.channel = static_cast<std::uint8_t>(channelNum > 0 ? channelNum - 1 : 0);
        outFrame.error = true;
        return true;
    }

    CanDirection direction = CanDirection::Rx;
    std::string_view idToken;

    if (idx < tokens.size()
        && (tokens[idx] == "Rx" || tokens[idx] == "Tx" || tokens[idx] == "rx" || tokens[idx] == "tx"
            || tokens[idx] == "r" || tokens[idx] == "t")) {
        direction = (tokens[idx] == "Tx" || tokens[idx] == "tx" || tokens[idx] == "t")
                        ? CanDirection::Tx
                        : CanDirection::Rx;
        ++idx;
        if (idx >= tokens.size()) {
            return false;
        }
        idToken = tokens[idx++];
    } else if (idx < tokens.size()) {
        idToken = tokens[idx++];
        if (idx < tokens.size()
            && (tokens[idx] == "Rx" || tokens[idx] == "Tx" || tokens[idx] == "rx"
                || tokens[idx] == "tx" || tokens[idx] == "r" || tokens[idx] == "t")) {
            direction = (tokens[idx] == "Tx" || tokens[idx] == "tx" || tokens[idx] == "t")
                            ? CanDirection::Tx
                            : CanDirection::Rx;
            ++idx;
        }
    }

    if (idToken.empty()) {
        return false;
    }

    bool isExtended = false;
    if (idToken.ends_with('x') || idToken.ends_with('X')) {
        isExtended = true;
        idToken.remove_suffix(1);
    }

    std::uint32_t identifier = 0;
    if (!parseHexU32(idToken, identifier)) {
        return false;
    }

    if (identifier > kMaxStandardIdentifier) {
        isExtended = true;
    }

    bool isRtr = false;
    if (idx < tokens.size() && (tokens[idx] == "r" || tokens[idx] == "R")) {
        isRtr = true;
        ++idx;
    } else if (idx < tokens.size() && (tokens[idx] == "d" || tokens[idx] == "D")) {
        ++idx;
    }

    if (isFd && idx < tokens.size()) {
        // CAN FD lines often have flags BRS/ESI/DLC before data
        while (idx < tokens.size() && (tokens[idx] == "0" || tokens[idx] == "1")
               && idx + 2 < tokens.size()) {
            ++idx;
        }
    }

    std::uint32_t dlc = 0;
    if (idx < tokens.size() && parseDecU32(tokens[idx], dlc)) {
        ++idx;
    }

    if (isFd && idx < tokens.size()) {
        std::uint32_t actualLen = 0;
        if (parseDecU32(tokens[idx], actualLen)) {
            dlc = actualLen;
            ++idx;
        }
    }

    outFrame = CanFrame{};
    outFrame.timestampNs = timestampNs;
    outFrame.identifier = identifier;
    outFrame.channel = static_cast<std::uint8_t>(channelNum > 0 ? channelNum - 1 : 0);
    outFrame.direction = direction;
    outFrame.format = isExtended ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    outFrame.fd = isFd;
    outFrame.rtr = isRtr;
    outFrame.error = false;

    if (!isRtr) {
        std::size_t byteCount = 0;
        const std::size_t maxBytes = std::min<std::size_t>(dlc, kMaxCanPayload);

        while (idx < tokens.size() && byteCount < maxBytes) {
            std::uint8_t b = 0;
            if (parseHexU8(tokens[idx], b)) {
                outFrame.data[byteCount++] = b;
                ++idx;
            } else {
                break;
            }
        }
        outFrame.length = static_cast<std::uint8_t>(byteCount);
        outFrame.dlc = static_cast<std::uint8_t>(dlc > 0 ? dlc : byteCount);
    } else {
        outFrame.length = 0;
        outFrame.dlc = static_cast<std::uint8_t>(dlc);
    }

    return true;
}

bool TraceImporter::parseTrcLine(std::string_view line,
                                 CanFrame& outFrame,
                                 int& trcVersion,
                                 std::uint64_t& startWallClockUs)
{
    const std::string_view trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }

    if (trimmed.starts_with(';')) {
        if (trimmed.starts_with(";$FILEVERSION=")) {
            const std::size_t eq = trimmed.find('=');
            const std::string_view verStr = trim(trimmed.substr(eq + 1));
            if (verStr.starts_with("2.1") || verStr.starts_with("2.0")) {
                trcVersion = 21;
            } else if (verStr.starts_with("1.3")) {
                trcVersion = 13;
            } else {
                trcVersion = 11;
            }
        } else if (trimmed.starts_with(";$STARTTIME=")) {
            parsePcanStartTime(trimmed, startWallClockUs);
        }
        return false;
    }

    thread_local std::vector<std::string_view> tokens;
    tokenize(trimmed, tokens);
    if (tokens.size() < 4) {
        return false;
    }

    // Token 0: sequence number, e.g. "1)"
    std::string_view seqToken = tokens[0];
    if (seqToken.ends_with(')')) {
        seqToken.remove_suffix(1);
    }
    std::uint32_t seq = 0;
    if (!parseDecU32(seqToken, seq)) {
        return false;
    }

    // Token 1: offset in ms, e.g. "0.000" or "5.120"
    std::uint64_t timestampNs = 0;
    if (!parseMillisecondsToNs(tokens[1], timestampNs)) {
        return false;
    }

    // Token 2: message type, e.g. DT, FD, FB, RR, ER, 1, 2
    const std::string_view type = tokens[2];
    bool isFd = (type == "FD" || type == "FB" || type == "fd" || type == "fb");
    bool isBrs = (type == "FB" || type == "fb");
    bool isRtr = (type == "RR" || type == "rr");
    bool isError = (type == "ER" || type == "ERR" || type == "er" || type == "err");

    if (isError) {
        outFrame = CanFrame{};
        outFrame.timestampNs = timestampNs;
        outFrame.error = true;
        return true;
    }

    std::size_t idx = 3;
    std::uint8_t channel = 0;
    CanDirection direction = CanDirection::Rx;

    if (trcVersion >= 20
        || (tokens.size() > 5 && tokens[3].size() == 1 && std::isdigit(tokens[3][0])
            && tokens[5] == "-")) {
        // v2.x: Token 3 is Bus
        std::uint32_t bus = 1;
        if (parseDecU32(tokens[idx], bus)) {
            channel = static_cast<std::uint8_t>(bus > 0 ? bus - 1 : 0);
            ++idx;
        }
    } else if (trcVersion == 13
               || (tokens[idx] == "Rx" || tokens[idx] == "Tx" || tokens[idx] == "rx"
                   || tokens[idx] == "tx")) {
        // v1.3: Token 3 is Direction
        direction =
            (tokens[idx] == "Tx" || tokens[idx] == "tx") ? CanDirection::Tx : CanDirection::Rx;
        ++idx;
    }

    if (idx >= tokens.size()) {
        return false;
    }

    // ID token
    std::string_view idToken = tokens[idx++];
    bool isExtended = false;
    if (idToken.ends_with('x') || idToken.ends_with('X')) {
        isExtended = true;
        idToken.remove_suffix(1);
    } else if (idToken.ends_with('h') || idToken.ends_with('H')) {
        idToken.remove_suffix(1);
    }

    std::uint32_t identifier = 0;
    if (!parseHexU32(idToken, identifier)) {
        return false;
    }

    if (identifier > kMaxStandardIdentifier || idToken.size() > 4) {
        isExtended = true;
    }

    if (idx < tokens.size() && tokens[idx] == "-") {
        ++idx;
    }

    if (idx < tokens.size()
        && (tokens[idx] == "Rx" || tokens[idx] == "Tx" || tokens[idx] == "rx"
            || tokens[idx] == "tx")) {
        direction =
            (tokens[idx] == "Tx" || tokens[idx] == "tx") ? CanDirection::Tx : CanDirection::Rx;
        ++idx;
    }

    std::uint32_t dlc = 0;
    if (idx < tokens.size() && parseDecU32(tokens[idx], dlc)) {
        ++idx;
    }

    outFrame = CanFrame{};
    outFrame.timestampNs = timestampNs;
    outFrame.identifier = identifier;
    outFrame.channel = channel;
    outFrame.direction = direction;
    outFrame.format = isExtended ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    outFrame.fd = isFd;
    outFrame.brs = isBrs;
    outFrame.rtr = isRtr;
    outFrame.error = false;

    if (!isRtr) {
        std::size_t byteCount = 0;
        const std::size_t maxBytes = std::min<std::size_t>(dlc, kMaxCanPayload);

        while (idx < tokens.size() && byteCount < maxBytes) {
            std::uint8_t b = 0;
            if (parseHexU8(tokens[idx], b)) {
                outFrame.data[byteCount++] = b;
                ++idx;
            } else {
                break;
            }
        }
        outFrame.length = static_cast<std::uint8_t>(byteCount);
        outFrame.dlc = static_cast<std::uint8_t>(dlc > 0 ? dlc : byteCount);
    } else {
        outFrame.length = 0;
        outFrame.dlc = static_cast<std::uint8_t>(dlc);
    }

    return true;
}

Result TraceImporter::importToLog(const std::string& inputPath,
                                  const std::string& outputTblogPath,
                                  ImportSummary* summary)
{
    const std::filesystem::path inPath = pathFromUtf8(inputPath);
    std::ifstream file{inPath};
    if (!file.is_open()) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("Could not open external log '{}'", inputPath));
    }

    const ExternalTraceFormat format = detectFormat(inputPath);
    if (format == ExternalTraceFormat::Auto) {
        return Result::error(ErrorCode::ParseError,
                             std::format("Unrecognized log format in '{}'", inputPath));
    }

    std::uint64_t startWallClockUs = 0;
    int trcVersion = 21;

    // Pre-scan first 100 lines for header timestamp
    {
        std::string probeLine;
        for (int i = 0; i < 100 && std::getline(file, probeLine); ++i) {
            if (format == ExternalTraceFormat::VectorAsc) {
                if (probeLine.find("date ") != std::string::npos) {
                    parseAscDateLine(probeLine, startWallClockUs);
                }
            } else {
                if (probeLine.find(";$STARTTIME=") != std::string::npos) {
                    parsePcanStartTime(probeLine, startWallClockUs);
                }
                if (probeLine.find(";$FILEVERSION=") != std::string::npos) {
                    if (probeLine.find("1.3") != std::string::npos) {
                        trcVersion = 13;
                    } else if (probeLine.find("1.1") != std::string::npos
                               || probeLine.find("1.2") != std::string::npos) {
                        trcVersion = 11;
                    }
                }
            }
        }
        file.clear();
        file.seekg(0);
    }

    TraceLogWriter writer;
    if (Result res = writer.open(outputTblogPath, startWallClockUs); res.failed()) {
        return res;
    }

    constexpr std::size_t kBatchSize = 2048;
    std::vector<CanFrame> batch;
    batch.reserve(kBatchSize);

    std::uint64_t frameCount = 0;
    std::uint64_t skippedLines = 0;
    std::uint64_t firstTs = 0;
    std::uint64_t lastTs = 0;

    std::string line;
    CanFrame frame;

    while (std::getline(file, line)) {
        bool ok = false;
        if (format == ExternalTraceFormat::VectorAsc) {
            ok = parseAscLine(line, frame, startWallClockUs);
        } else {
            ok = parseTrcLine(line, frame, trcVersion, startWallClockUs);
        }

        if (ok) {
            if (frameCount == 0) {
                firstTs = frame.timestampNs;
            }
            lastTs = frame.timestampNs;
            ++frameCount;

            batch.push_back(frame);
            if (batch.size() >= kBatchSize) {
                if (Result writeRes = writer.append(batch); writeRes.failed()) {
                    return writeRes;
                }
                batch.clear();
            }
        } else {
            ++skippedLines;
        }
    }

    if (!batch.empty()) {
        if (Result writeRes = writer.append(batch); writeRes.failed()) {
            return writeRes;
        }
        batch.clear();
    }

    writer.close();

    if (summary != nullptr) {
        summary->frames = frameCount;
        summary->skippedLines = skippedLines;
        summary->firstTimestampNs = firstTs;
        summary->lastTimestampNs = lastTs;
        summary->durationNs = (lastTs >= firstTs) ? (lastTs - firstTs) : 0;
        summary->startWallClockUs = startWallClockUs;
        summary->format = format;
    }

    return Result::ok();
}

Result TraceImporter::importFrames(const std::string& inputPath,
                                   std::vector<CanFrame>& outFrames,
                                   std::size_t maxFrames,
                                   ImportSummary* summary)
{
    const std::filesystem::path inPath = pathFromUtf8(inputPath);
    std::ifstream file{inPath};
    if (!file.is_open()) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("Could not open external log '{}'", inputPath));
    }

    const ExternalTraceFormat format = detectFormat(inputPath);
    if (format == ExternalTraceFormat::Auto) {
        return Result::error(ErrorCode::ParseError,
                             std::format("Unrecognized log format in '{}'", inputPath));
    }

    std::uint64_t startWallClockUs = 0;
    int trcVersion = 21;

    std::uint64_t frameCount = 0;
    std::uint64_t skippedLines = 0;
    std::uint64_t firstTs = 0;
    std::uint64_t lastTs = 0;

    std::string line;
    CanFrame frame;

    while (std::getline(file, line)) {
        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }

        bool ok = false;
        if (format == ExternalTraceFormat::VectorAsc) {
            ok = parseAscLine(line, frame, startWallClockUs);
        } else {
            ok = parseTrcLine(line, frame, trcVersion, startWallClockUs);
        }

        if (ok) {
            if (frameCount == 0) {
                firstTs = frame.timestampNs;
            }
            lastTs = frame.timestampNs;
            ++frameCount;
            outFrames.push_back(frame);
        } else {
            ++skippedLines;
        }
    }

    if (summary != nullptr) {
        summary->frames = frameCount;
        summary->skippedLines = skippedLines;
        summary->firstTimestampNs = firstTs;
        summary->lastTimestampNs = lastTs;
        summary->durationNs = (lastTs >= firstTs) ? (lastTs - firstTs) : 0;
        summary->startWallClockUs = startWallClockUs;
        summary->format = format;
    }

    return Result::ok();
}

Result TraceImporter::autoImport(const std::string& inputPath,
                                 std::string& effectiveTblogPath,
                                 ImportSummary* summary)
{
    if (!isExternalLog(inputPath)) {
        effectiveTblogPath = inputPath;
        return Result::ok();
    }

    const std::filesystem::path inPath = pathFromUtf8(inputPath);
    std::error_code ec;
    if (!std::filesystem::exists(inPath, ec)) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("External log '{}' not found", inputPath));
    }

    std::filesystem::path outPath = inPath;
    outPath.replace_extension(".tblog");

    bool cacheValid = false;
    if (std::filesystem::exists(outPath, ec)) {
        const auto inTime = std::filesystem::last_write_time(inPath, ec);
        const auto outTime = std::filesystem::last_write_time(outPath, ec);
        const auto outSize = std::filesystem::file_size(outPath, ec);
        if (outTime >= inTime && outSize >= TraceLogHeader::kSize) {
            cacheValid = true;
        }
    }

    if (!cacheValid) {
        // Try companion file in same directory first
        Result res = importToLog(inputPath, utf8FromPath(outPath), summary);
        if (res.failed()) {
            // If writing to original directory failed (e.g. read-only permissions), use temp cache
            const std::filesystem::path cacheDir =
                std::filesystem::temp_directory_path() / "torquebus_log_cache";
            std::filesystem::create_directories(cacheDir, ec);

            outPath = cacheDir / inPath.filename();
            outPath.replace_extension(".tblog");

            res = importToLog(inputPath, utf8FromPath(outPath), summary);
            if (res.failed()) {
                return res;
            }
        }
    }

    effectiveTblogPath = utf8FromPath(outPath);
    return Result::ok();
}

} // namespace torquebus
