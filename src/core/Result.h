// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The error channel of the core. Drivers and engines never throw across their
// API boundary and never print: they return a Result, and the caller decides
// whether that becomes a status bar message, a log line or a test failure.

#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace torquebus {

enum class ErrorCode {
    Ok = 0,

    // Generic
    Unknown,
    NotImplemented,
    InvalidArgument,
    InvalidState,
    Timeout,
    Cancelled,

    // Driver / hardware
    BackendUnavailable,   ///< SDK or driver DLL not installed.
    DeviceNotFound,       ///< The handle does not match any present device.
    DeviceBusy,           ///< Already opened, by us or by another application.
    ChannelNotOpen,
    UnsupportedFeature,   ///< Asked for CAN FD on a classic-only device, etc.
    BitTimingRejected,
    TransmitFailed,
    BusOff,

    // Storage / project
    FileNotFound,
    FileAccessDenied,
    ParseError,
    VersionMismatch
};

[[nodiscard]] constexpr const char* toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::Ok:                 return "Ok";
    case ErrorCode::Unknown:            return "Unknown error";
    case ErrorCode::NotImplemented:     return "Not implemented";
    case ErrorCode::InvalidArgument:    return "Invalid argument";
    case ErrorCode::InvalidState:       return "Invalid state";
    case ErrorCode::Timeout:            return "Timeout";
    case ErrorCode::Cancelled:          return "Cancelled";
    case ErrorCode::BackendUnavailable: return "Backend unavailable";
    case ErrorCode::DeviceNotFound:     return "Device not found";
    case ErrorCode::DeviceBusy:         return "Device busy";
    case ErrorCode::ChannelNotOpen:     return "Channel not open";
    case ErrorCode::UnsupportedFeature: return "Unsupported feature";
    case ErrorCode::BitTimingRejected:  return "Bit timing rejected";
    case ErrorCode::TransmitFailed:     return "Transmit failed";
    case ErrorCode::BusOff:             return "Bus off";
    case ErrorCode::FileNotFound:       return "File not found";
    case ErrorCode::FileAccessDenied:   return "File access denied";
    case ErrorCode::ParseError:         return "Parse error";
    case ErrorCode::VersionMismatch:    return "Version mismatch";
    }
    return "Unknown error";
}

/// Outcome of an operation that either succeeds or fails with a reason.
///
/// Deliberately not std::expected: the overwhelming majority of driver calls
/// return no value, and a dedicated type lets the message carry vendor detail
/// ("canOpenChannel: canERR_NOTFOUND (-3)") without leaking the vendor type.
class Result final {
public:
    Result() noexcept = default;

    /// Constructs a failure. Use Result::ok() for success.
    Result(ErrorCode code, std::string message)
        : m_code{code}
        , m_message{std::move(message)}
    {
    }

    explicit Result(ErrorCode code)
        : m_code{code}
        , m_message{toString(code)}
    {
    }

    [[nodiscard]] static Result ok() noexcept { return Result{}; }

    [[nodiscard]] static Result error(ErrorCode code, std::string message)
    {
        return Result{code, std::move(message)};
    }

    [[nodiscard]] static Result error(ErrorCode code) { return Result{code}; }

    [[nodiscard]] bool succeeded() const noexcept { return m_code == ErrorCode::Ok; }
    [[nodiscard]] bool failed() const noexcept { return m_code != ErrorCode::Ok; }

    explicit operator bool() const noexcept { return succeeded(); }

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] std::string_view message() const noexcept { return m_message; }

private:
    ErrorCode m_code{ErrorCode::Ok};
    std::string m_message;
};

} // namespace torquebus
