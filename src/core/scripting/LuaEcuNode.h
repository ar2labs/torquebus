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
//     every(ms, function() ... end) -- one of many timers, each at its own rate
//     cyclic(id, ms, data)          -- a message that sends itself
//     set_timer(milliseconds)       -- how often on_timer runs
//     log_message(text)             -- a line in the Output panel
//     get_time_us()                 -- microseconds since the measurement began
//
// **And the ECU can answer diagnostics.** Given a pair of identifiers, the node
// carries an ISO-TP connection and a UdsServer on the ECU side, so a script
// declares what it knows and lets the server answer the rest:
//
//     uds_did(0xF190, "WVWZZZ...")        -- a value a tester can read
//     uds_did(0x2001, "\x00\x64", { writable = true, session = 3,
//                                    security = true })
//     uds_dtc(0x012800, 0x2F)             -- a stored fault
//     function on_security_seed(seed)     -- the key the ECU will accept
//     function on_uds_request(request)    -- anything the server does not do
//
// on_uds_request is the escape hatch and it is three-valued, because a real ECU
// has three answers: return bytes to answer, return nothing to let the server
// deal with it, and return false to say *nothing at all* - a dead ECU, which is
// the case a tester has to survive and the only one nothing else can simulate.
//
// What changed from cansim, and why: `emit` does not reach the bus. It puts a
// frame on this node's *output port*, and where that goes is the graph's
// business. An ECU wired to nothing is a valid, testable thing; an ECU wired to
// a filter, a trace and a channel is three different experiments with no change
// to the script.

#pragma once

#include "core/can/CanFrame.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/diagnostics/UdsServer.h"
#include "core/isotp/IsoTpConnection.h"
#include "core/pipeline/PipelineNode.h"
#include "core/scripting/LuaRuntime.h"
#include "core/scripting/ScriptLibrary.h"
#include "core/trace/TraceStore.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

    /// Makes this ECU answer diagnostic requests on `address`.
    ///
    /// The address is the ECU's own way round: it *receives* on what a tester
    /// transmits. Called before prepare(); without it the node has no
    /// diagnostic layer at all and costs nothing for the scripts that do not
    /// use one.
    void enableDiagnostics(const IsoTpAddress& address, const IsoTpConfig& transport);

    [[nodiscard]] bool answersDiagnostics() const noexcept { return m_transport != nullptr; }

    /// The ECU's diagnostic server, or nullptr on a node that answers none.
    ///
    /// Borrowed, and only ever touched on the executor thread. Exposed so that
    /// a test can ask this ECU a question directly, without a transport and a
    /// graph in between - the layer being tested there is what the script
    /// declared, not how the bytes got in.
    [[nodiscard]] UdsServer* diagnosticServer() noexcept { return m_server.get(); }
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

    /// Replaces the running script without stopping the measurement.
    ///
    /// The rule, and the reason this is worth its complexity: **a script that
    /// fails to load leaves the running one alone.** The new source is compiled
    /// into a *second* interpreter and only becomes this node's when it has
    /// loaded and its on_enable has returned. Until then the old one is intact -
    /// its timers, its faults, its globals, the state somebody was debugging.
    ///
    /// What deliberately does *not* reset: the measurement clock. get_time_us()
    /// keeps counting from Start, because a script reloaded at 40 seconds that
    /// suddenly believed it was at zero would be a worse lie than no reload.
    ///
    /// A faulted node - one that went quiet after its error limit - gets a fresh
    /// start here. The edit is usually the fix.
    ///
    /// Called on the executor thread, from process().
    [[nodiscard]] Result reload(std::string source);

    /// Where edited scripts arrive from, and where outcomes go back.
    ///
    /// The id is the node's id in the GraphDescription, because that is what the
    /// editor knows; `nullptr` - the default - means this node simply never
    /// looks, which is what every test and every headless run wants.
    void setScriptLibrary(ScriptLibrary* library, std::string nodeId)
    {
        m_library = library;
        m_nodeId = std::move(nodeId);
    }

    /// The source currently running. After a successful reload, the new one.
    [[nodiscard]] const std::string& source() const noexcept { return m_source; }

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Frames emitted", m_emitted},
            {"Signal values saturated", m_saturated},
            // A faulted script has gone quiet after its error limit, and the
            // measurement carried on without it. That is the right behaviour
            // and an easy thing not to notice, so it gets a number.
            {"Stopped after repeated errors", m_faulted ? 1U : 0U},
            // Only interesting on an ECU that answers diagnostics, and zero is
            // the honest reading on one that does not: a tester talking to the
            // wrong identifier sees these stay at zero, which is the fastest
            // way to find that out.
            {"Diagnostic requests", m_diagnosticRequests},
            {"Diagnostic answers", m_diagnosticAnswers},
            {"Deliberate silences", m_diagnosticSilences},
            // Frames this node deliberately made wrong. Worth a number of its
            // own: an injected fault left switched on is the likeliest reason a
            // later measurement makes no sense.
            {"Frames corrupted on purpose", m_corrupted},
            // Two numbers rather than one: a reload that was refused left the
            // old script running, which is correct and completely invisible
            // from the trace. Somebody wondering why their edit changed nothing
            // should find the answer here.
            {"Scripts reloaded", m_reloads},
            {"Reloads refused", m_reloadsRefused},
        };
    }

    void setLogHandler(LogHandler handler) { m_log = std::move(handler); }

    /// What the measurement has seen, for a script that asks about the bus.
    ///
    /// Read-only and read from the executor thread - the same thread that
    /// writes it - so this needs no lock and gets none. A pointer rather than a
    /// reference because a graph built for a test has no trace store and a
    /// script that asks anyway should be told, not crash.
    void setTraceStore(const TraceStore* store) { m_trace = store; }

    /// The named values a dashboard and this script share.
    ///
    /// Borrowed, not owned, and null in a graph with no dashboard behind it -
    /// in which case `var_get` and `var_set` are simply not registered, and a
    /// script calling one is told what is missing rather than reading zeroes
    /// off a table that does not exist.
    ///
    /// Handles are resolved on first use and cached per name, so a variable
    /// touched inside on_message costs a map lookup and an atomic store: no
    /// lock, ever, on the frame path. See SystemVariables.h.
    void setSystemVariables(SystemVariables* variables) { m_variables = variables; }

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
    static int luaUdsIdentifier(lua_State* state);
    static int luaUdsTroubleCode(lua_State* state);
    static int luaUdsClearTroubleCodes(lua_State* state);
    static int luaUdsSession(lua_State* state);
    static int luaEmitSignal(lua_State* state);
    static int luaDecode(lua_State* state);
    static int luaSetTimer(lua_State* state);
    static int luaEvery(lua_State* state);
    static int luaCyclic(lua_State* state);
    static int luaStopCyclic(lua_State* state);
    static int luaFault(lua_State* state);
    static int luaBusLast(lua_State* state);
    static int luaBusStats(lua_State* state);
    static int luaVariableGet(lua_State* state);
    static int luaVariableSet(lua_State* state);
    static int luaLogMessage(lua_State* state);
    static int luaGetTimeMicroseconds(lua_State* state);

    [[nodiscard]] static LuaEcuNode* self(lua_State* state);

    /// The handle for `name`, resolved once and remembered. Only called with
    /// m_variables non-null.
    [[nodiscard]] SystemVariables::Handle variableHandle(const std::string& name);

    /// Builds this node's script into whatever interpreter m_lua currently is:
    /// libraries, bindings, globals, the prelude, `source`, and on_enable.
    ///
    /// Shared by prepare() and reload() so that a hot-swapped script is set up
    /// by exactly the same code as one loaded at Start - a second, nearly
    /// identical loader is how the two would drift until a script behaved
    /// differently depending on when it arrived.
    [[nodiscard]] Result install(const std::string& source);

    /// Takes an edited script, if the editor has offered one and is not holding
    /// the lock. Called once per pass.
    void takeOfferedScript();

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

    /// The diagnostic layer, or null for a script that does not answer
    /// diagnostics. Held by pointer rather than by value so that the ordinary
    /// ECU - which is most of them - carries no ISO-TP state it never uses.
    std::unique_ptr<IsoTpConnection> m_transport;
    std::unique_ptr<UdsServer> m_server;

    bool m_hasOnUdsRequest{false};
    bool m_hasOnSecuritySeed{false};

    std::uint64_t m_diagnosticRequests{0};
    std::uint64_t m_diagnosticAnswers{0};
    std::uint64_t m_diagnosticSilences{0};
    std::shared_ptr<const CanDatabase> m_database;
    const TraceStore* m_trace{nullptr};

    SystemVariables* m_variables{nullptr};

    /// Name to handle, so a script naming a variable in a timer pays a map
    /// lookup rather than the library's lock. Rebuilt with the interpreter.
    std::map<std::string, SystemVariables::Handle> m_variableHandles;

    /// What to do to a frame on its way out, by identifier.
    ///
    /// Applied at the output stage rather than where the frame is built, so one
    /// mechanism covers emit(), cyclic() and even the diagnostic responses -
    /// corrupting a UDS answer to see what a tester does is a real experiment
    /// and would otherwise need its own switch.
    struct Fault final {
        /// Send the previous payload again: a stuck ECU. Freezes a rolling
        /// counter and stales a CRC without this code knowing which byte is
        /// which, which is the only way to do it without a database.
        bool freeze{false};

        /// The DLC to claim, whatever the payload actually is. -1 leaves it
        /// alone. A DLC that disagrees with the data is a fault a receiver
        /// either tolerates or does not, and finding out is the point.
        int dlc{-1};

        /// Send only this many bytes. -1 sends them all.
        int truncate{-1};

        /// Bits to invert, as (index, mask) pairs. A CRC byte flipped here is
        /// a message that arrives looking valid and checksums wrong.
        std::vector<std::pair<std::size_t, std::uint8_t>> flips;

        /// The last payload actually sent, for freeze.
        std::vector<std::uint8_t> previous;
        bool hasPrevious{false};
    };

    std::map<std::uint32_t, Fault> m_faults;

    /// Applies m_faults to everything about to be published.
    void applyFaults();

    std::uint64_t m_corrupted{0};
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

    /// One repeating job: a script function, or a message that sends itself.
    ///
    /// Both are here rather than in two lists because they are the same thing
    /// with a different body, and a single list is what makes the ordering
    /// between them the obvious one - due jobs run in the order they were
    /// declared, every pass, whatever their rates.
    struct Repeating final {
        std::chrono::nanoseconds interval{};
        std::chrono::steady_clock::time_point next{};

        /// The function to call, for `every`.
        LuaRuntime::CallableRef callable{0};

        /// For `cyclic`: the frame to send, and optionally a function that
        /// returns fresh bytes for it each time.
        bool isMessage{false};
        std::uint32_t identifier{0};
        bool extended{false};
        std::vector<std::uint8_t> payload;
        LuaRuntime::CallableRef provider{0};

        /// A stopped job stays in the list rather than being erased, so that
        /// stop_cyclic() during a pass cannot invalidate the iteration - and so
        /// that starting it again keeps its place.
        bool stopped{false};
    };

    std::vector<Repeating> m_repeating;

    std::chrono::steady_clock::time_point m_started;
    std::chrono::steady_clock::time_point m_lastTimer;
    std::chrono::milliseconds m_timerInterval{0};

    std::uint64_t m_emitted{0};
    int m_consecutiveErrors{0};
    bool m_faulted{false};

    /// Not owned, and null in every headless run. Read from the executor thread
    /// only, through try_lock - see ScriptLibrary.h.
    ScriptLibrary* m_library{nullptr};
    std::string m_nodeId;

    std::uint64_t m_reloads{0};
    std::uint64_t m_reloadsRefused{0};
};

} // namespace torquebus
