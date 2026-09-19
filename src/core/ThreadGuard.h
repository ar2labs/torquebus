// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Stops an exception from leaving a thread, because leaving is fatal.
//
// An exception that escapes the function a std::thread was started with calls
// std::terminate. Not an error dialog, not a stopped measurement: the process
// is gone, with no message, no log line and nothing flushed to the recording
// that was in progress. The same is true of a destructor that throws while the
// stack is already unwinding.
//
// TorqueBus has four thread entry points - the engine's dispatch loop and one
// receive thread per backend - and every one of them runs code that can throw.
// The dispatch loop is the worst of them: it walks the pipeline graph, which
// means it runs Lua ECU scripts, database decoders and, since v0.17, node types
// registered by a plugin this repository has never seen. The loader is careful
// to survive an exception thrown during a plugin's registration, which would be
// an odd place to stop being careful - a plugin's node throwing at frame 40,000
// of a measurement took the whole application with it.
//
// So: none of those bodies is allowed to throw past this. What a caller does
// with the failure differs - the engine logs it and stops the measurement, a
// backend reports itself offline - which is why the report is a callback rather
// than something decided here.
//
// This is a net, not a strategy. Code that can fail in a way somebody should
// act on returns a Result; this is for the rest, and for the ones nobody
// thought of.

#pragma once

#include <exception>
#include <string>
#include <string_view>

namespace torquebus {

/// Runs `body`, and lets nothing out.
///
/// `what` names the thread in the message - "the dispatch loop", "the Kvaser
/// receive thread" - because a report that says only "an exception" leaves
/// somebody grepping for which thread it was.
///
/// `report` is handed the finished message as a string_view, at most once. A
/// view and not a string on purpose: the fallback below has to be sayable
/// without allocating, and a literal is.
///
/// The reporter's own exceptions are swallowed too. A reporter that throws
/// while reporting a crash is the same bug one level up, and this is the level
/// that has to stop.
template<typename Body, typename Report>
void runWithoutEscaping(std::string_view what, Body&& body, Report&& report) noexcept
{
    const auto tell = [&report](std::string_view message) noexcept {
        try {
            report(message);
        } catch (...) {
            // Nowhere left to say it.
        }
    };

    // Composing the message allocates, and allocation is one of the things that
    // throws. Inside a noexcept function that would call terminate - which is
    // the exact failure this file exists to prevent, one level up. So the
    // composition sits inside its own try, and what it falls back to is a
    // literal: no allocation, so nothing left that can fail.
    const auto describe = [&tell, what](const char* reason) noexcept {
        try {
            const std::string message = std::string{what} + " stopped: " + reason;
            tell(message);
        } catch (...) {
            tell("a thread stopped, and the reason could not be composed");
        }
    };

    try {
        body();
    } catch (const std::exception& problem) {
        describe(problem.what());
    } catch (...) {
        // Something that is not a std::exception. Rare, and worth saying so
        // rather than reporting an empty reason.
        describe("an exception that is not a std::exception");
    }
}

/// Wraps a thread body so that the thread itself cannot terminate the process.
///
/// Exists so that adding the net to an existing thread touches the line that
/// starts it and nothing else. Re-indenting a fifty-line loop to sit inside a
/// try would bury the change in a diff, and the change is the point.
///
///     m_thread = std::thread{guardThread("the Kvaser receive thread",
///                                        reporter, [this] { ...body... })};
template<typename Report, typename Body>
[[nodiscard]] auto guardThread(std::string_view what, Report report, Body body)
{
    return [what, body = std::move(body), report = std::move(report)]() noexcept {
        runWithoutEscaping(what, body, report);
    };
}

} // namespace torquebus
