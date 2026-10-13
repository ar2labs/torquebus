// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// One Lua interpreter, owned properly.
//
// This is the only place in TorqueBus that includes lua.h - the same containment
// the driver layer gives vendor SDKs (rule #4). A node holds a LuaRuntime; it
// never holds a lua_State.
//
// Three rules the whole scripting layer is built on:
//
//   1. A script error is a Result, never an exception and never a crash.
//      Scripts are written by users, at a keyboard, while a measurement is
//      running. A typo in on_message() must produce a line in the Output panel
//      and a stopped node - not a dead application and a lost recording.
//
//   2. Errors carry the script's own line number. `[engine.lua:42] attempt to
//      index a nil value (field 'data')` is actionable; "lua error" is not.
//
//   3. One VM per ECU. A Lua state is a few kilobytes and starts in
//      microseconds, so isolation is nearly free - and it means one ECU's
//      runaway loop or corrupt global cannot reach another's.

#pragma once

#include "core/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace torquebus {

/// What one script is allowed to cost.
///
/// Rule 1 above says a script error is a Result and never a crash, and an error
/// is only something a script can produce by returning. `while true do end` does
/// not return, and neither does a table that grows until the machine swaps: the
/// first froze the thread that runs the pipeline - and with it Stop, which waits
/// for that thread - and the second took the whole application's memory with it.
/// Both are the commonest mistakes a person makes while writing a loop.
///
/// The limits turn those into the errors rule 1 promises. They are generous on
/// purpose: a handler runs once per frame inside a pass of a few milliseconds, so
/// nothing a simulated ECU legitimately does comes near them.
struct LuaLimits final {
    /// The top-level chunk of a script and of the prelude: set-up work, which
    /// builds tables and may take a little longer than a handler.
    std::chrono::milliseconds loadBudget{2000};

    /// One call into a script - a handler for one frame, one timer tick.
    std::chrono::milliseconds callBudget{250};

    /// Heap the interpreter may hold. A request past it fails the way an
    /// exhausted machine would, inside the script, as "not enough memory".
    std::size_t memoryBytes{64U * 1024U * 1024U};
};

/// A value passed to or from a script. Deliberately small: the scripting
/// boundary carries numbers, strings and byte arrays, and anything richer goes
/// through a dedicated binding rather than a generic variant.
struct LuaValue final {
    enum class Type : std::uint8_t { Nil, Boolean, Number, Integer, String };

    Type type{Type::Nil};
    bool boolean{};
    double number{};
    std::int64_t integer{};
    std::string text;

    static LuaValue fromBoolean(bool value)
    {
        LuaValue result;
        result.type = Type::Boolean;
        result.boolean = value;
        return result;
    }

    static LuaValue fromInteger(std::int64_t value)
    {
        LuaValue result;
        result.type = Type::Integer;
        result.integer = value;
        return result;
    }

    static LuaValue fromNumber(double value)
    {
        LuaValue result;
        result.type = Type::Number;
        result.number = value;
        return result;
    }

    static LuaValue fromString(std::string value)
    {
        LuaValue result;
        result.type = Type::String;
        result.text = std::move(value);
        return result;
    }
};

class LuaRuntime final {
public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;
    LuaRuntime(LuaRuntime&&) = delete;
    LuaRuntime& operator=(LuaRuntime&&) = delete;

    /// Opens the standard library.
    ///
    /// `os` and `io` are omitted: an ECU script simulates a control unit, and
    /// giving every one of them the ability to delete files or spawn processes
    /// is a capability nothing in the model needs. A script that genuinely
    /// needs to read a file gets a binding for it, reviewed on its own terms.
    [[nodiscard]] Result openLibraries();

    /// Compiles and runs a chunk. `chunkName` appears in error messages, so it
    /// should be the script's file name.
    [[nodiscard]] Result load(std::string_view source, std::string_view chunkName);

    /// Compiles a chunk without executing it, checking its syntax.
    [[nodiscard]] Result checkSyntax(std::string_view source,
                                     std::string_view chunkName = "script.lua");

    [[nodiscard]] Result loadFile(const std::string& path);

    /// True when a global function of that name exists - which is how the node
    /// knows whether a script implements on_timer at all, without calling it
    /// and catching the failure.
    [[nodiscard]] bool hasFunction(std::string_view name) const;

    /// Calls a global function with no arguments.
    [[nodiscard]] Result call(std::string_view name);

    /// Calls a global function with the given arguments.
    [[nodiscard]] Result call(std::string_view name, const std::vector<LuaValue>& arguments);

    /// Calls a global function and keeps what it returns.
    ///
    /// One value, because that is what every use of this has needed and because
    /// a script that returns two things has usually made a mistake it would
    /// rather be told about. `result` is Nil when the function returned
    /// nothing - which is a meaningful answer in its own right: a handler that
    /// returns nothing has declined, where one that returns false has decided.
    [[nodiscard]] Result
    call(std::string_view name, const std::vector<LuaValue>& arguments, LuaValue& result);

    /// Registers a C function as a global. `userData` is handed back to it.
    /// A function value the script handed over, kept alive by the runtime.
    ///
    /// `every(100, function() ... end)` passes an anonymous function, which has
    /// no name to call it by later. Lua's answer is the registry: the value is
    /// stored under an integer key that keeps it from being collected, and this
    /// is that key. Zero is never a valid one.
    using CallableRef = int;

    /// Takes the value at `stackIndex` and keeps it. Fails - returning 0 - when
    /// it is not something that can be called, so a script passing a number
    /// where a function belongs is told at the call site rather than at the
    /// first tick.
    [[nodiscard]] CallableRef storeCallable(int stackIndex);

    /// Calls a stored function. The same rules as call(): pcall, never
    /// lua_call, and one optional return value.
    [[nodiscard]] Result callStored(CallableRef ref);
    [[nodiscard]] Result callStored(CallableRef ref, LuaValue& result);

    /// Lets a stored function be collected. Safe on 0.
    void releaseCallable(CallableRef ref);

    using NativeFunction = int (*)(lua_State*);
    void registerFunction(std::string_view name, NativeFunction function, void* userData);

    /// Sets a global to a value.
    void setGlobal(std::string_view name, const LuaValue& value);

    /// Sets a global table of named values.
    ///
    /// This is how a script becomes reusable. Without it every setting a
    /// script needs - which identifier, which cycle time, which starting
    /// temperature - has to be a constant in the file, and running the same
    /// behaviour twice with different numbers means copying the file. cansim
    /// established the shape and twenty scripts read it:
    ///
    ///     local can_id = parameters.can_id
    void setGlobalTable(std::string_view name, const std::map<std::string, LuaValue>& values);

    /// The state, for bindings that need it. Callers outside the scripting
    /// layer have no business with this.
    [[nodiscard]] lua_State* state() noexcept { return m_state; }

    /// Memory the interpreter is currently holding, in bytes. Worth showing per
    /// ECU: a script that leaks a table every cycle is invisible until it is
    /// not, and this is the number that catches it early.
    [[nodiscard]] std::size_t memoryBytes() const;

    /// Replaces the limits. Takes effect on the next call, and the memory limit
    /// from the next allocation; a limit below what is already held is not an
    /// error, it just means everything that grows fails from here on.
    void setLimits(const LuaLimits& limits) noexcept;
    [[nodiscard]] const LuaLimits& limits() const noexcept { return m_limits; }

    /// True when the last failure was the interpreter running out of its budget
    /// of time or memory rather than the script's own error. A node can then say
    /// so - "stopped after 5 errors" is the wrong thing to tell somebody whose
    /// loop never ended.
    [[nodiscard]] bool lastFailureWasALimit() const noexcept { return m_limitHit; }

private:
    /// Wraps the allocator: counts what Lua holds and refuses what would pass the
    /// limit. Defined in the .cpp, where lua.h is.
    struct Allocator;

    /// Holds a deadline for the length of one call.
    class Budget;

    /// The count hook. Fires every few thousand VM instructions - the only place
    /// a loop that never calls anything can be interrupted.
    static void onCount(lua_State* state, lua_Debug* debug);

    /// The one implementation behind both callStored() overloads.
    [[nodiscard]] Result callStored(CallableRef ref, LuaValue& result, int results);

    /// The one implementation behind both call() overloads.
    [[nodiscard]] Result call(std::string_view name,
                              const std::vector<LuaValue>& arguments,
                              LuaValue& result,
                              int results);

    /// Turns a failed pcall into a Result and records whether a limit was why.
    [[nodiscard]] Result failure(ErrorCode code);

    // What the interpreter allocates through. lua_close, in the destructor, frees
    // through it, so it must outlive the state - which it does, being a member
    // that is only released once the destructor body has closed the state.
    std::unique_ptr<Allocator> m_allocator;

    lua_State* m_state{nullptr};

    LuaLimits m_limits;
    bool m_limitHit{false};

    bool m_deadlineActive{false};
    bool m_timedOut{false};
    std::chrono::steady_clock::time_point m_deadline{};
    std::chrono::milliseconds m_allowance{0};
};

} // namespace torquebus
