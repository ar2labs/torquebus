// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The net under every thread entry point and every destructor that flushes.
//
// These tests are unusual in that the failure they guard against cannot be
// observed from inside the process: an exception leaving a thread body calls
// std::terminate, and a terminated process fails no assertion. So what is
// checked here is the contract that makes terminate impossible - nothing gets
// out, the reason arrives, and the reporter's own failures are absorbed.

#include "core/ThreadGuard.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <thread>

using namespace torquebus;

TEST(ThreadGuardTests, ABodyThatDoesNotThrowIsSimplyRun)
{
    bool ran = false;
    bool reported = false;

    runWithoutEscaping(
        "the test", [&ran] { ran = true; }, [&reported](std::string_view) { reported = true; });

    EXPECT_TRUE(ran);

    // The reporter is for failures. Calling it on success would put a line in
    // the Output panel every time a measurement ended normally.
    EXPECT_FALSE(reported);
}

TEST(ThreadGuardTests, AStdExceptionIsCaughtAndItsReasonReachesTheReporter)
{
    std::string message;

    runWithoutEscaping(
        "the dispatch loop",
        [] { throw std::runtime_error{"a node gave up"}; },
        [&message](std::string_view reason) { message = reason; });

    // Both halves matter. The thread's name is what tells somebody reading the
    // Output panel which of the four threads died; the exception's text is what
    // tells them why.
    EXPECT_TRUE(message.find("the dispatch loop") != std::string::npos);
    EXPECT_TRUE(message.find("a node gave up") != std::string::npos);
}

TEST(ThreadGuardTests, SomethingThatIsNotAStdExceptionStillProducesAReason)
{
    std::string message;

    runWithoutEscaping(
        "the Kvaser receive thread",
        [] { throw 42; },
        [&message](std::string_view reason) { message = reason; });

    // Not an empty string: "a thread stopped" with no reason at all is barely
    // better than the silence this replaces.
    EXPECT_FALSE(message.empty());
    EXPECT_TRUE(message.find("the Kvaser receive thread") != std::string::npos);
    EXPECT_TRUE(message.find("not a std::exception") != std::string::npos);
}

TEST(ThreadGuardTests, AReporterThatThrowsDoesNotDefeatTheGuard)
{
    // The case that would otherwise be embarrassing: the code that exists to
    // stop a crash, crashing. If this were wrong the test process would
    // terminate here rather than fail.
    int attempts = 0;

    runWithoutEscaping(
        "the test",
        [] { throw std::runtime_error{"first"}; },
        [&attempts](std::string_view) {
            ++attempts;
            throw std::runtime_error{"the reporter is broken too"};
        });

    EXPECT_TRUE(attempts == 1);
}

TEST(ThreadGuardTests, NothingEscapesARealThread)
{
    // The shape the production code actually uses. Without the guard this
    // std::thread would call std::terminate and take the test binary with it.
    std::string message;

    std::thread worker{guardThread(
        "the virtual receive thread",
        [&message](std::string_view reason) { message = reason; },
        [] { throw std::runtime_error{"the bus went away"}; })};

    worker.join();

    EXPECT_TRUE(message.find("the virtual receive thread") != std::string::npos);
    EXPECT_TRUE(message.find("the bus went away") != std::string::npos);
}

TEST(ThreadGuardTests, AGuardedThreadThatFinishesNormallyReportsNothing)
{
    bool reported = false;
    int counted = 0;

    std::thread worker{guardThread(
        "the test",
        [&reported](std::string_view) { reported = true; },
        [&counted] {
            for (int index = 0; index < 1000; ++index) {
                counted += index;
            }
        })};

    worker.join();

    EXPECT_TRUE(counted == 499500);
    EXPECT_FALSE(reported);
}
