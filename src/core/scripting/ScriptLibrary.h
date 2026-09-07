// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How an edited script reaches a running measurement.
//
// Changing a simulated ECU used to mean Stop, edit, Start - and Start rebuilds
// the graph, reopens the channels and throws away the trace. For a script being
// written a line at a time, which is how every simulation is written, that is
// the difference between a tight loop and a chore. Worse, it destroys the very
// state somebody is debugging: the fault they had just provoked is gone.
//
// So the editor hands new source to this, and the node picks it up on its next
// pass. The same shape as the diagnostic console's end of a conversation, and
// for the same reason: **the executor never waits on the editor.** It tries the
// lock once a pass and moves on, because the thing on the other side is a text
// widget in a window that may be repainting, and a frame path that can block on
// that is what rule #5 exists to prevent.
//
// The rule that makes this safe to use rather than merely quick:
//
//   **A script that fails to load leaves the running one alone.**
//
// An edit is usually broken - that is what editing is - and a half-typed
// function must not take an ECU off the bus. The old interpreter keeps running,
// the error comes back with its line number, and nothing about the measurement
// changed.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace torquebus {

/// What happened when a node tried to take new source.
struct ScriptReload final {
    std::string nodeId;

    bool succeeded{false};

    /// Empty on success; the loader's error, with its line, otherwise.
    std::string message;

    /// Nanoseconds on the measurement's clock.
    std::uint64_t timestampNs{0};
};

class ScriptLibrary final {
public:
    ScriptLibrary() = default;

    ScriptLibrary(const ScriptLibrary&) = delete;
    ScriptLibrary& operator=(const ScriptLibrary&) = delete;
    ScriptLibrary(ScriptLibrary&&) = delete;
    ScriptLibrary& operator=(ScriptLibrary&&) = delete;

    // --- The editor's side -------------------------------------------------

    /// Offers new source for one node, replacing anything not yet taken.
    ///
    /// Replacing rather than queueing: somebody pressing Reload three times
    /// while typing means the last version, and playing the intermediate ones
    /// through an ECU would be a slideshow of half-finished edits.
    void offer(const std::string& nodeId, std::string source);

    /// Everything that has been loaded, or failed to load, since the last call.
    [[nodiscard]] std::vector<ScriptReload> takeReports();

    /// True while an offer is still waiting to be taken - so an editor can say
    /// "reloading..." rather than looking as though nothing happened.
    [[nodiscard]] bool isPending(const std::string& nodeId) const;

    // --- The executor's side -----------------------------------------------

    /// Takes the source offered for `nodeId`, if any.
    ///
    /// Returns false when there is nothing waiting *or* when the editor holds
    /// the lock - the offer stays put and the next pass gets it, which is a few
    /// milliseconds nobody can perceive.
    [[nodiscard]] bool take(const std::string& nodeId, std::string& source);

    void report(ScriptReload outcome);

    /// Forgets everything. Called when a measurement starts, so a reload
    /// offered against the last run cannot land on this one.
    void clear();

private:
    mutable std::mutex m_mutex;

    std::map<std::string, std::string> m_pending;
    std::vector<ScriptReload> m_reports;
};

} // namespace torquebus
