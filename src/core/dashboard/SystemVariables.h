// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Named values shared between a dashboard, a script and the bus.
//
// A gauge reads one. A slider writes one. A Lua ECU reads what the slider wrote
// and puts it on the bus; a Lua ECU writes one and the gauge shows it. That is
// the whole idea, and it is what makes a dashboard more than a second trace
// window: the person turns a knob and the simulated vehicle responds.
//
// PLAN.md v0.14 calls this a "Python variable". Python is not in this tool and
// will not be; the concept it names - a value that is not a CAN signal and not
// a diagnostic result, belonging to the simulation rather than to the wire - is
// real, and this is it.
//
// ---------------------------------------------------------------------------
// Why a value is a double, and nothing else
//
// Every use is a number: a setpoint, a pedal position, a switch that is 0 or 1,
// a lamp that is on or off. Making the type a variant would buy strings on a
// dashboard label at the price of a lock on the frame path, and a text label is
// better served by a UDS result or a signal's value table - both of which
// already carry their own text.
//
// So: one `std::atomic<double>` per variable. **Reading and writing a variable
// takes no lock at all**, which is what lets a script touch one inside
// on_message without the frame path ever waiting on a window.
//
// ---------------------------------------------------------------------------
// Why a fixed array rather than a growing container
//
// The first version of this used a std::deque, on the reasoning that a deque
// never moves what it already holds. That is true of the *elements* and false
// of the deque: push_back can reallocate the internal block map, and a reader
// indexing into it at that moment is a data race - a real one, not a
// theoretical one, and exactly the kind that survives every test and fails on
// somebody's bench.
//
// A fixed array has no such moment. Slots are constructed once, at
// construction; resolve() fills in a name under the lock and then publishes the
// new count with a release store, and a reader holding a handle below that
// count is reading memory nothing will ever touch again.
//
// The cap is the price. 512 named variables is more than any dashboard anybody
// would build, and a project that reaches it gets told, by name, rather than
// silently losing the 513th.
//
// ---------------------------------------------------------------------------
// What survives what
//
// Values survive Start and Stop. A setpoint somebody dialled in before starting
// a measurement is still there when it starts, and still there afterwards -
// clearing on Start would be a slider that resets itself every time, which is
// the behaviour nobody wants and everybody has met.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace torquebus {

class SystemVariables final {
public:
    /// A resolved variable. Stable for the life of this object: a slot is never
    /// moved and never reused, which is what makes a handle safe to keep on the
    /// executor thread.
    using Handle = std::size_t;

    static constexpr Handle kUnknown = static_cast<Handle>(-1);

    /// Upper bound on distinct names. See the header comment: the limit is the
    /// price of a read path that takes no lock.
    static constexpr std::size_t kMaximumVariables = 512;

    SystemVariables() = default;

    SystemVariables(const SystemVariables&) = delete;
    SystemVariables& operator=(const SystemVariables&) = delete;
    SystemVariables(SystemVariables&&) = delete;
    SystemVariables& operator=(SystemVariables&&) = delete;

    /// The handle for `name`, creating the variable if this is the first time
    /// it has been mentioned. Takes the lock; call it at prepare(), not per
    /// frame. Returns kUnknown only when the table is full.
    ///
    /// Creating on first mention rather than requiring a declaration: a script
    /// and a dashboard widget name the same variable without either being
    /// "first", and a rule about which one has to declare it would be a rule
    /// somebody gets wrong in a project file they did not write.
    [[nodiscard]] Handle resolve(const std::string& name);

    /// The handle for `name`, or kUnknown. Does not create.
    [[nodiscard]] Handle find(const std::string& name) const;

    /// Lock-free. kUnknown reads as zero, which is the honest value for a
    /// variable nobody has written.
    [[nodiscard]] double value(Handle handle) const noexcept;

    /// Lock-free. A kUnknown handle is ignored rather than being an error: the
    /// caller was already told, at resolve time, that the name did not fit.
    void set(Handle handle, double value) noexcept;

    /// Counts writes, including writes of the same value.
    ///
    /// A panel repainting at 20 Hz needs to know whether anything happened, and
    /// "the number is the same" is not the same question: a script writing 0
    /// every cycle is doing something, and a gauge that stopped updating is a
    /// different fault from a value that is not changing.
    [[nodiscard]] std::uint64_t revision(Handle handle) const noexcept;

    // --- By name, for the GUI side ----------------------------------------
    //
    // These take the lock on every call, which is right for a panel and wrong
    // for a node. A node resolves once and uses the handle.

    [[nodiscard]] double value(const std::string& name) const;

    /// Creates on first write, like resolve(): a dashboard writing a name no
    /// script has mentioned yet is the ordinary case on a project being built,
    /// and refusing it would mean the slider does nothing until something else
    /// happens to name the same variable.
    void set(const std::string& name, double value);

    /// Every variable that has been mentioned, in the order it was first seen.
    ///
    /// Declaration order rather than alphabetical: it is the order a script
    /// created them in, which is the order somebody reading that script expects.
    [[nodiscard]] std::vector<std::string> names() const;

    [[nodiscard]] std::size_t count() const noexcept
    {
        return m_count.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isFull() const noexcept { return count() >= kMaximumVariables; }

    /// Forgets every variable. **Invalidates every handle**, so it is only
    /// valid between measurements - a new project, not a new Start.
    void clear();

private:
    struct Slot final {
        std::atomic<double> value{0.0};
        std::atomic<std::uint64_t> revision{0};
    };

    mutable std::mutex m_mutex;

    /// Published with release once the name and slot are ready; read with
    /// acquire by the lock-free path. A handle below this is a slot nothing
    /// will write to again except through value()/set().
    std::atomic<std::size_t> m_count{0};

    std::array<Slot, kMaximumVariables> m_slots{};

    /// Guarded by the mutex. The names are only ever needed by the GUI.
    std::map<std::string, Handle> m_byName;
    std::vector<std::string> m_order;
};

} // namespace torquebus
