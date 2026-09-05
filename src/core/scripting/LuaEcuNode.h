// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A simulated ECU: one Lua script, one node on the canvas.
//
// The lifecycle is taken verbatim from cansim, which has twenty working ECUs
// behind it. That is not laziness - it is the difference between a contract
// someone has lived with and one invented at a keyboard:
//
//     function on_enable()          -- set up
//     function on_disable()         -- tear down
//     function on_timer()           -- periodic behaviour
//     function on_message(frame)    -- react to the bus
//
// and the calls a script can make back:
//
//     emit(id, data, options)       -- put a frame on this node's output
//     set_timer(milliseconds)       -- how often on_timer runs
//     log_message(text)             -- a line in the Output panel
//     get_time_us()                 -- microseconds since the measurement began
//
// What changed from cansim, and why: `emit` does not reach the bus. It puts a
// frame on this node's *output port*, and where that goes is the graph's
// business. An ECU wired to nothing is a valid, testable thing; an ECU wired to
// a filter, a trace and a channel is three different experiments with no change
// to the script.

#pragma once

#include "core/can/CanFrame.h"
#include "core/database/CanMessage.h"
#include "core/pipeline/PipelineNode.h"
#include "core/scripting/LuaRuntime.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace torquebus {

class LuaEcuNode final : public IPipelineNode {
public:
    /// Where a script's log_message() and any script error go. Called on the
    /// executor thread, so it obeys the same rules as any sink: do not block.
    using LogHandler = std::function<void(const std::string& text, bool isError)>;

    /// `source` is the script itself, not a path - the project file carries
    /// scripts inline or by reference, and this node should not care which.
    LuaEcuNode(std::string source, std::string name, std::uint8_t transmitChannel = 0);
    ~LuaEcuNode() override;

    [[nodiscard]] std::string_view typeName() const noexcept override { return "lua.ecu"; }
    [[nodiscard]] std::string displayName() const override { return m_name; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    /// Compiles the script, opens the sandbox and runs on_enable().
    ///
    /// A script that fails to compile fails the graph compile, with the Lua
    /// error and its line number - so a typo stops a measurement from starting
    /// rather than surfacing on the first frame.
    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    /// Runs on_disable().
    void finish() override;

    void setLogHandler(LogHandler handler) { m_log = std::move(handler); }

    /// The database `emit_signal` and `decode` work against.
    ///
    /// Optional: a script that only ever calls `emit` with packed bytes needs
    /// none. With one, the script stops carrying the bit layout of its own
    /// messages - which is the layout it is most likely to get wrong and least
    /// likely to notice, because a mis-packed frame still transmits.
    ///
    /// Shared, not owned, and by shared_ptr for the reason given in
    /// DecodedSignal.h: the definitions have to outlive anything that points at
    /// them, and a reload mid-measurement must not pull them out from under a
    /// script that is running.
    void setDatabase(std::shared_ptr<const CanDatabase> database)
    {
        m_database = std::move(database);
    }

    /// Signal values a script asked for that its field could not hold.
    ///
    /// Saturated and counted rather than refused. A control loop that briefly
    /// asks for 300% torque has a bug worth seeing, but killing the ECU over it
    /// would take the rest of the simulation down with it.
    [[nodiscard]] std::uint64_t saturatedSignals() const noexcept { return m_saturated; }

    /// Settings the script reads from its `parameters` global.
    ///
    /// What makes a script reusable rather than a one-off. Without it, every
    /// number a script needs - its identifier, its cycle time, its starting
    /// value - is a constant in the file, and running the same behaviour twice
    /// with different numbers means copying the file. One temperature sensor
    /// script becomes four sensors on four identifiers instead.
    ///
    /// Taken from cansim, where twenty scripts already read exactly this.
    void setScriptParameters(std::map<std::string, LuaValue> parameters)
    {
        m_scriptParameters = std::move(parameters);
    }

    /// Stops after this many consecutive errors.
    ///
    /// A script that throws on every frame would otherwise write 150,000 log
    /// lines a second and drown everything useful. After the limit the node
    /// says so once and goes quiet; the measurement continues without it,
    /// because one broken simulated ECU should not end a recording.
    static constexpr int kErrorLimit = 5;

    [[nodiscard]] bool isFaulted() const noexcept { return m_faulted; }

    /// Frames the script has produced since the measurement started.
    [[nodiscard]] std::uint64_t emittedFrames() const noexcept { return m_emitted; }

    /// Memory the script's interpreter is holding. A number worth watching per
    /// ECU: a script that leaks a table every cycle is invisible until it isn't.
    [[nodiscard]] std::size_t memoryBytes() const;

private:
    // --- Bindings, called from Lua ---------------------------------------
    static int luaEmit(lua_State* state);
    static int luaEmitSignal(lua_State* state);
    static int luaDecode(lua_State* state);
    static int luaSetTimer(lua_State* state);
    static int luaLogMessage(lua_State* state);
    static int luaGetTimeMicroseconds(lua_State* state);

    [[nodiscard]] static LuaEcuNode* self(lua_State* state);

    void report(const std::string& text, bool isError);
    void handleScriptFailure(const Result& result, std::string_view during);

    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::string m_source;
    std::string m_name;
    std::uint8_t m_transmitChannel;

    std::unique_ptr<LuaRuntime> m_lua;
    std::shared_ptr<const CanDatabase> m_database;
    LogHandler m_log;
    std::map<std::string, LuaValue> m_scriptParameters;

    /// Frames the script emitted during the current pass. A member, reused, so
    /// a pass allocates nothing (rule #12).
    std::vector<CanFrame> m_outgoing;

    std::uint64_t m_saturated{0};

    /// The buffer holds frames from on_enable that no pass has published yet.
    bool m_carryingStartupFrames{false};

    bool m_hasOnMessage{false};
    bool m_hasOnTimer{false};

    std::chrono::steady_clock::time_point m_started;
    std::chrono::steady_clock::time_point m_lastTimer;
    std::chrono::milliseconds m_timerInterval{0};

    std::uint64_t m_emitted{0};
    int m_consecutiveErrors{0};
    bool m_faulted{false};
};

} // namespace torquebus
