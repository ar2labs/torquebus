// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/Result.h"

#include <catch2/catch_test_macros.hpp>

using namespace torquebus;

TEST_CASE("A default-constructed Result is success", "[result]")
{
    const Result result;

    CHECK(result.succeeded());
    CHECK_FALSE(result.failed());
    CHECK(result.code() == ErrorCode::Ok);
    CHECK(static_cast<bool>(result));
}

TEST_CASE("A failure carries its code and its message", "[result]")
{
    const Result result = Result::error(ErrorCode::DeviceNotFound,
                                        "canOpenChannel: canERR_NOTFOUND (-3)");

    CHECK(result.failed());
    CHECK_FALSE(result.succeeded());
    CHECK(result.code() == ErrorCode::DeviceNotFound);
    CHECK(result.message() == "canOpenChannel: canERR_NOTFOUND (-3)");
    CHECK_FALSE(static_cast<bool>(result));
}

TEST_CASE("A failure without a message falls back to the code's description",
          "[result]")
{
    const Result result = Result::error(ErrorCode::BusOff);

    CHECK(result.failed());
    CHECK(result.message() == "Bus off");
}

TEST_CASE("Every error code has a description", "[result]")
{
    // A missing case in toString() would return "Unknown error" for a code that
    // is not Unknown - which is exactly how an error silently loses its
    // meaning on its way to the status bar.
    const ErrorCode codes[] = {
        ErrorCode::Ok,                 ErrorCode::NotImplemented,
        ErrorCode::InvalidArgument,    ErrorCode::InvalidState,
        ErrorCode::Timeout,            ErrorCode::Cancelled,
        ErrorCode::BackendUnavailable, ErrorCode::DeviceNotFound,
        ErrorCode::DeviceBusy,         ErrorCode::ChannelNotOpen,
        ErrorCode::UnsupportedFeature, ErrorCode::BitTimingRejected,
        ErrorCode::TransmitFailed,     ErrorCode::BusOff,
        ErrorCode::FileNotFound,       ErrorCode::FileAccessDenied,
        ErrorCode::ParseError,         ErrorCode::VersionMismatch,
    };

    for (const ErrorCode code : codes) {
        const std::string_view description = toString(code);
        INFO("code = " << static_cast<int>(code));
        CHECK_FALSE(description.empty());
        CHECK(description != "Unknown error");
    }
}
