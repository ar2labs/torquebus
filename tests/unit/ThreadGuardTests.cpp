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

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <thread>

using namespace torquebus;

TEST_CASE("A body that does not throw is simply run", "[threadguard]")
{
    bool ran = false;
    bool reported = false;

    runWithoutEscaping(
        "the test", [&ran] { ran = true; }, [&reported](std::string_view) { reported = true; });

    CHECK(ran);

    // The reporter is for failures. Calling it on success would put a line in
    // the Output panel every time a measurement ended normally.
    CHECK_FALSE(reported);
}

TEST_CASE("A std::exception is caught and its reason reaches the reporter", "[threadguard]")
{
    std::string message;

    runWithoutEscaping(
        "the dispatch loop",
        [] { throw std::runtime_error{"a node gave up"}; },
        [&message](std::string_view reason) { message = reason; });

    // Both halves matter. The thread's name is what tells somebody reading the
    // Output panel which of the four threads died; the exception's text is what
    // tells them why.
    CHECK(message.find("the dispatch loop") != std::string::npos);
    CHECK(message.find("a node gave up") != std::string::npos);
}

TEST_CASE("Something that is not a std::exception still produces a reason", "[threadguard]")
{
    std::string message;

    runWithoutEscaping(
        "the Kvaser receive thread",
        [] { throw 42; },
        [&message](std::string_view reason) { message = reason; });

    // Not an empty string: "a thread stopped" with no reason at all is barely
    // better than the silence this replaces.
    CHECK_FALSE(message.empty());
    CHECK(message.find("the Kvaser receive thread") != std::string::npos);
    CHECK(message.find("not a std::exception") != std::string::npos);
}

TEST_CASE("A reporter that throws does not defeat the guard", "[threadguard]")
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

    CHECK(attempts == 1);
}

TEST_CASE("Nothing escapes a real thread", "[threadguard]")
{
    // The shape the production code actually uses. Without the guard this
    // std::thread would call std::terminate and take the test binary with it.
    std::string message;

    std::thread worker{guardThread(
        "the virtual receive thread",
        [&message](std::string_view reason) { message = reason; },
        [] { throw std::runtime_error{"the bus went away"}; })};

    worker.join();

    CHECK(message.find("the virtual receive thread") != std::string::npos);
    CHECK(message.find("the bus went away") != std::string::npos);
}

TEST_CASE("A guarded thread that finishes normally reports nothing", "[threadguard]")
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

    CHECK(counted == 499500);
    CHECK_FALSE(reported);
}
