// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/Result.h"

#include <gtest/gtest.h>

using namespace torquebus;

TEST(ResultTests, ADefaultConstructedResultIsSuccess)
{
    const Result result;

    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::Ok);
    EXPECT_TRUE(static_cast<bool>(result));
}

TEST(ResultTests, AFailureCarriesItsCodeAndItsMessage)
{
    const Result result =
        Result::error(ErrorCode::DeviceNotFound, "canOpenChannel: canERR_NOTFOUND (-3)");

    EXPECT_TRUE(result.failed());
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.code() == ErrorCode::DeviceNotFound);
    EXPECT_TRUE(result.message() == "canOpenChannel: canERR_NOTFOUND (-3)");
    EXPECT_FALSE(static_cast<bool>(result));
}

TEST(ResultTests, AFailureWithoutAMessageFallsBackToTheCodeSDescription)
{
    const Result result = Result::error(ErrorCode::BusOff);

    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(result.message() == "Bus off");
}

TEST(ResultTests, EveryErrorCodeHasADescription)
{
    // A missing case in toString() would return "Unknown error" for a code that
    // is not Unknown - which is exactly how an error silently loses its
    // meaning on its way to the status bar.
    const ErrorCode codes[] = {
        ErrorCode::Ok,
        ErrorCode::NotImplemented,
        ErrorCode::InvalidArgument,
        ErrorCode::InvalidState,
        ErrorCode::Timeout,
        ErrorCode::Cancelled,
        ErrorCode::BackendUnavailable,
        ErrorCode::DeviceNotFound,
        ErrorCode::DeviceBusy,
        ErrorCode::ChannelNotOpen,
        ErrorCode::UnsupportedFeature,
        ErrorCode::BitTimingRejected,
        ErrorCode::TransmitFailed,
        ErrorCode::BusOff,
        ErrorCode::FileNotFound,
        ErrorCode::FileAccessDenied,
        ErrorCode::ParseError,
        ErrorCode::VersionMismatch,
    };

    for (const ErrorCode code : codes) {
        const std::string_view description = toString(code);
        SCOPED_TRACE(::testing::Message() << "code = " << static_cast<int>(code));
        EXPECT_FALSE(description.empty());
        EXPECT_TRUE(description != "Unknown error");
    }
}
