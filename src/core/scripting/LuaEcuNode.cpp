// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/scripting/LuaEcuNode.h"

#include "core/scripting/LuaPrelude.h"

#include <algorithm>
#include <optional>
#include <format>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace torquebus {
namespace {

/// Upper bound on frames one script may emit in a single pass.
///
/// Not a performance tuning knob - a runaway `while true do emit(...) end` has
/// to stop somewhere, and stopping at a number is better than stopping when
/// memory runs out. The script is told when it hits this.
constexpr std::size_t kMaximumEmitsPerPass = 4096;

} // namespace

LuaEcuNode::LuaEcuNode(std::string source, std::string name, std::uint8_t transmitChannel)
    : m_source{std::move(source)}
    , m_name{std::move(name)}
    , m_transmitChannel{transmitChannel}
{
}

LuaEcuNode::~LuaEcuNode() = default;

LuaEcuNode* LuaEcuNode::self(lua_State* state)
{
    // The node rides as an upvalue on every binding - see
    // LuaRuntime::registerFunction. Not a global and not a registry entry,
    // because a script can reach and overwrite both.
    return static_cast<LuaEcuNode*>(lua_touserdata(state, lua_upvalueindex(1)));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result LuaEcuNode::prepare(std::size_t maximumBatchSize)
{
    (void)maximumBatchSize;

    m_lua = std::make_unique<LuaRuntime>();

    // A second prepare - a measurement restarted without rebuilding the graph -
    // gets a fresh interpreter, so the references from the last run belong to a
    // state that no longer exists. Dropped here rather than released, because
    // releasing them against the new interpreter would unref numbers that mean
    // something else in it.
    m_repeating.clear();
    m_faults.clear();
    m_corrupted = 0;

    if (Result result = m_lua->openLibraries(); result.failed()) {
        return result;
    }

    m_lua->registerFunction("emit", &LuaEcuNode::luaEmit, this);
    m_lua->registerFunction("emit_signal", &LuaEcuNode::luaEmitSignal, this);
    m_lua->registerFunction("decode", &LuaEcuNode::luaDecode, this);
    m_lua->registerFunction("set_timer", &LuaEcuNode::luaSetTimer, this);
    m_lua->registerFunction("log_message", &LuaEcuNode::luaLogMessage, this);
    m_lua->registerFunction("get_time_us", &LuaEcuNode::luaGetTimeMicroseconds, this);
    m_lua->registerFunction("every", &LuaEcuNode::luaEvery, this);
    m_lua->registerFunction("cyclic", &LuaEcuNode::luaCyclic, this);
    m_lua->registerFunction("stop_cyclic", &LuaEcuNode::luaStopCyclic, this);
    m_lua->registerFunction("fault", &LuaEcuNode::luaFault, this);

    // Only when there is a trace to read. A script asking about the bus in a
    // graph that has none gets an error naming what is missing.
    if (m_trace != nullptr) {
        m_lua->registerFunction("bus_last", &LuaEcuNode::luaBusLast, this);
        m_lua->registerFunction("bus_stats", &LuaEcuNode::luaBusStats, this);
    }

    // Only when there is a diagnostic layer. A script calling uds_did on an ECU
    // with no addresses gets an error naming what is missing, rather than a
    // silent no-op that leaves somebody wondering why a tester sees nothing.
    if (m_server != nullptr) {
        m_lua->registerFunction("uds_did", &LuaEcuNode::luaUdsIdentifier, this);
        m_lua->registerFunction("uds_dtc", &LuaEcuNode::luaUdsTroubleCode, this);
        m_lua->registerFunction("uds_clear_dtc", &LuaEcuNode::luaUdsClearTroubleCodes, this);
        m_lua->registerFunction("uds_session", &LuaEcuNode::luaUdsSession, this);
    }

    // Handy for a script that wants to know which channel it is standing in for
    // without being told twice.
    m_lua->setGlobal("node_name", LuaValue::fromString(m_name));
    m_lua->setGlobal("channel", LuaValue::fromInteger(m_transmitChannel));

    // Always defined, even when empty, so a script can write
    // `parameters.can_id or 0x100` without first testing that the table exists.
    m_lua->setGlobalTable("parameters", m_scriptParameters);

    // The prelude first, so a script can use tb.ramp() at the top level. Its
    // failure would be a fault in this build rather than in anybody's script,
    // which is why it is reported with its own name rather than the script's.
    if (Result result = m_lua->load(kLuaPrelude, "prelude"); result.failed()) {
        return result;
    }

    if (Result result = m_lua->load(m_source, m_name); result.failed()) {
        // A script that will not compile stops the measurement from starting,
        // rather than surfacing on the first frame. The Lua message already
        // carries the file and line.
        return Result::error(result.code(),
                             std::format("{}: {}", m_name, std::string{result.message()}));
    }

    m_hasOnMessage = m_lua->hasFunction("on_message");
    m_hasOnTimer = m_lua->hasFunction("on_timer");
    m_hasOnUdsRequest = m_lua->hasFunction("on_uds_request");
    m_hasOnSecuritySeed = m_lua->hasFunction("on_security_seed");

    if (m_server != nullptr) {
        // The script gets first refusal on every request. Three answers,
        // because a real ECU has three: bytes, nothing (let the server deal
        // with it), and false - say nothing at all.
        m_server->setHandler([this](std::span<const std::uint8_t> request,
                                    std::vector<std::uint8_t>& response) {
            if (!m_hasOnUdsRequest || m_faulted) {
                return UdsServer::Verdict::NotHandled;
            }

            LuaValue answer;

            const std::vector<LuaValue> arguments{LuaValue::fromString(
                std::string{reinterpret_cast<const char*>(request.data()), request.size()})};

            if (Result result = m_lua->call("on_uds_request", arguments, answer);
                result.failed()) {
                handleScriptFailure(result, "on_uds_request");

                // A script that threw has not decided anything, so the server
                // answers as it would have. A tester meeting a broken script
                // should see the ECU it configured, not silence.
                return UdsServer::Verdict::NotHandled;
            }

            m_consecutiveErrors = 0;

            if (answer.type == LuaValue::Type::String) {
                response.assign(answer.text.begin(), answer.text.end());
                return UdsServer::Verdict::Answered;
            }

            if (answer.type == LuaValue::Type::Boolean && !answer.boolean) {
                return UdsServer::Verdict::Silent;
            }

            return UdsServer::Verdict::NotHandled;
        });

        if (m_hasOnSecuritySeed) {
            m_server->setSecurityAlgorithm(
                [this](std::span<const std::uint8_t> seed) -> std::vector<std::uint8_t> {
                    LuaValue key;

                    const std::vector<LuaValue> arguments{LuaValue::fromString(std::string{
                        reinterpret_cast<const char*>(seed.data()), seed.size()})};

                    if (Result result = m_lua->call("on_security_seed", arguments, key);
                        result.failed()) {
                        handleScriptFailure(result, "on_security_seed");
                        return {};
                    }

                    if (key.type != LuaValue::Type::String) {
                        return {};
                    }

                    return std::vector<std::uint8_t>{key.text.begin(), key.text.end()};
                });
        }
    }

    m_outgoing.reserve(kMaximumEmitsPerPass);

    m_started = std::chrono::steady_clock::now();
    m_lastTimer = m_started;
    m_emitted = 0;
    m_consecutiveErrors = 0;
    m_faulted = false;

    // Cleared before on_enable, not after: a re-prepare must not carry frames
    // from the previous measurement into this one.
    m_outgoing.clear();

    if (m_lua->hasFunction("on_enable")) {
        if (Result result = m_lua->call("on_enable"); result.failed()) {
            return Result::error(result.code(),
                                 std::format("{}: on_enable failed: {}",
                                             m_name, std::string{result.message()}));
        }
    }

    // Anything on_enable emitted is real traffic waiting for the first pass -
    // an ECU announcing itself at power-on - not a leftover to be cleared.
    m_carryingStartupFrames = !m_outgoing.empty();

    return Result::ok();
}

void LuaEcuNode::process(NodeContext& context)
{
    if (m_faulted || !m_lua) {
        return;
    }

    // The buffer is cleared at the *start* of a pass rather than the end,
    // because the span published last pass has to stay alive until now (see
    // PortType.h). The one exception is the first pass after prepare: what is
    // in the buffer then came from on_enable and has never been published.
    //
    // Clearing unconditionally dropped those frames silently, which is exactly
    // the failure mode this project refuses everywhere else - the script ran,
    // emit_signal succeeded, the counter went up, and nothing reached the bus.
    if (!m_carryingStartupFrames) {
        m_outgoing.clear();
    }
    m_carryingStartupFrames = false;

    // --- Diagnostics, when this ECU answers them --------------------------
    //
    // Before on_message rather than after, so that a request arriving and the
    // answer going out are the same pass. A script's on_message still sees the
    // frames: a diagnostic frame is a frame, and an ECU that wanted to count
    // them or react to the raw bytes is entitled to.
    if (m_transport != nullptr) {
        const auto nowNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_started)
                .count());

        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            static_cast<void>(m_transport->onFrame(frame, nowNs));
        }

        m_transport->poll(nowNs);

        for (const IsoTpEvent& event : m_transport->events()) {
            if (event.kind != IsoTpEvent::Kind::MessageReceived) {
                continue;
            }

            ++m_diagnosticRequests;

            if (const std::optional<std::vector<std::uint8_t>> response =
                    m_server->handle(event.data, nowNs);
                response.has_value()) {
                ++m_diagnosticAnswers;
                static_cast<void>(m_transport->send(*response, nowNs));
            } else {
                // Silence is an answer here - a suppressed TesterPresent, or a
                // script that decided this ECU is not talking.
                ++m_diagnosticSilences;
            }
        }

        m_transport->clearEvents();

        // Polled again after sending, because send() queues the first frame and
        // there is no reason to make it wait a pass.
        m_server->poll(nowNs);
        m_transport->poll(nowNs);

        for (const CanFrame& frame : m_transport->pendingFrames()) {
            if (m_outgoing.size() >= kMaximumEmitsPerPass) {
                break;
            }
            m_outgoing.push_back(frame);
        }

        m_transport->clearPendingFrames();
    }

    // --- on_message, once per incoming frame -----------------------------
    if (m_hasOnMessage) {
        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            // The payload goes across as a byte string rather than a table:
            // building a table of eight numbers costs an allocation and eight
            // stack operations per frame, and string.unpack is how the newer
            // cansim scripts read payloads anyway.
            const std::vector<LuaValue> arguments{
                LuaValue::fromInteger(frame.identifier),
                LuaValue::fromString(std::string{
                    reinterpret_cast<const char*>(frame.data.data()), frame.length}),
                LuaValue::fromInteger(frame.channel),
                LuaValue::fromInteger(static_cast<std::int64_t>(frame.isExtended() ? 1 : 0)),
            };

            if (Result result = m_lua->call("on_message", arguments); result.failed()) {
                handleScriptFailure(result, "on_message");
                if (m_faulted) {
                    return;
                }
            } else {
                m_consecutiveErrors = 0;
            }
        }
    }

    // --- every() and cyclic(), when due -----------------------------------
    //
    // Before on_timer, and in declaration order, so that a script mixing them
    // gets an ordering it can predict rather than one that depends on how this
    // loop happens to be written.
    if (!m_repeating.empty()) {
        const auto now = std::chrono::steady_clock::now();

        // Indexed rather than iterated: a job's body can call cyclic() or
        // every(), which appends - and appending during a range-for is how a
        // reallocation invalidates the iterator underneath it. New jobs are
        // picked up on the next pass, which is also the behaviour that makes
        // "declare a timer from a timer" not run away.
        const std::size_t count = m_repeating.size();

        for (std::size_t index = 0; index < count && !m_faulted; ++index) {
            Repeating& job = m_repeating[index];

            if (job.stopped || now < job.next) {
                continue;
            }

            // Advanced from the deadline rather than from now, so a cycle time
            // does not drift by however long the pass took. Caught up rather
            // than repeated when the executor was late: a graph stalled for
            // half a second must not then fire fifty 10 ms jobs in one pass.
            job.next += job.interval;
            if (job.next < now) {
                job.next = now + job.interval;
            }

            if (!job.isMessage) {
                if (Result result = m_lua->callStored(job.callable); result.failed()) {
                    handleScriptFailure(result, "every");
                } else {
                    m_consecutiveErrors = 0;
                }
                continue;
            }

            // A message: fresh bytes if it has a provider, otherwise the ones
            // it was declared with.
            if (job.provider != 0) {
                LuaValue produced;

                if (Result result = m_lua->callStored(job.provider, produced);
                    result.failed()) {
                    handleScriptFailure(result, "cyclic");
                    continue;
                }

                m_consecutiveErrors = 0;

                if (produced.type != LuaValue::Type::String) {
                    // A provider that returned nothing is a script saying "not
                    // this time", which is a legitimate way to skip a cycle -
                    // and not an error worth counting against it.
                    continue;
                }

                job.payload.assign(produced.text.begin(), produced.text.end());
            }

            if (m_outgoing.size() >= kMaximumEmitsPerPass) {
                continue;
            }

            CanFrame frame;
            frame.identifier = job.identifier;
            frame.format = job.extended ? CanFrameFormat::Extended : CanFrameFormat::Standard;
            frame.channel = m_transmitChannel;
            frame.direction = CanDirection::Tx;
            frame.length = static_cast<std::uint8_t>(
                std::min<std::size_t>(job.payload.size(), kMaxCanPayload));
            frame.dlc = frame.length;

            std::copy_n(job.payload.begin(), frame.length, frame.data.begin());

            m_outgoing.push_back(frame);
            ++m_emitted;
        }
    }

    // --- on_timer, when due ----------------------------------------------
    //
    // Checked once per pass, so the resolution is the executor's dispatch
    // interval - 5 ms by default. A script asking for 1 ms gets 5; that is a
    // real limit and it is documented rather than pretended away. Simulated
    // ECUs run on cycle times of tens of milliseconds, where it does not bite.
    if (m_hasOnTimer && m_timerInterval.count() > 0) {
        const auto now = std::chrono::steady_clock::now();

        if (now - m_lastTimer >= m_timerInterval) {
            m_lastTimer = now;

            if (Result result = m_lua->call("on_timer"); result.failed()) {
                handleScriptFailure(result, "on_timer");
            } else {
                m_consecutiveErrors = 0;
            }
        }
    }

    if (!m_outgoing.empty()) {
        // Faults last, so one mechanism covers everything leaving this node:
        // emit(), cyclic(), and the diagnostic responses. Corrupting a UDS
        // answer to see what a tester does is a real experiment, and it would
        // otherwise have needed a switch of its own.
        applyFaults();

        context.publish<CanFrame>(0, m_outgoing);
    }
}

void LuaEcuNode::finish()
{
    if (!m_lua || m_faulted) {
        return;
    }

    if (m_lua->hasFunction("on_disable")) {
        if (Result result = m_lua->call("on_disable"); result.failed()) {
            report(std::format("{}: on_disable failed: {}",
                               m_name, std::string{result.message()}),
                   true);
        }
    }
}

std::size_t LuaEcuNode::memoryBytes() const
{
    return m_lua ? m_lua->memoryBytes() : 0;
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

void LuaEcuNode::report(const std::string& text, bool isError)
{
    if (m_log) {
        m_log(text, isError);
    }
}

void LuaEcuNode::handleScriptFailure(const Result& result, std::string_view during)
{
    ++m_consecutiveErrors;

    if (m_consecutiveErrors <= kErrorLimit) {
        report(std::format("{}: {} failed: {}", m_name, during, std::string{result.message()}),
               true);
    }

    if (m_consecutiveErrors >= kErrorLimit) {
        m_faulted = true;
        report(std::format("{} stopped after {} consecutive errors. The measurement "
                           "continues without it.",
                           m_name, kErrorLimit),
               true);
    }
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

void LuaEcuNode::enableDiagnostics(const IsoTpAddress& address, const IsoTpConfig& transport)
{
    m_transport = std::make_unique<IsoTpConnection>(address, transport);
    m_server = std::make_unique<UdsServer>();
}

int LuaEcuNode::luaUdsIdentifier(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr || node->m_server == nullptr) {
        return luaL_error(state,
                          "uds_did: this ECU has no diagnostic addresses - set the "
                          "request and response identifiers on the block");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);

    std::size_t length = 0;
    const char* value = luaL_checklstring(state, 2, &length);

    if (identifier < 0 || identifier > 0xFFFF) {
        return luaL_error(state, "uds_did: an identifier is two bytes, so 0 to 65535");
    }

    UdsIdentifier entry;
    entry.value.assign(reinterpret_cast<const std::uint8_t*>(value),
                       reinterpret_cast<const std::uint8_t*>(value) + length);

    // The options table is optional, because most DIDs are a plain readable
    // value and having to write { writable = false } for every one of them
    // would make the common case the ugly one.
    if (lua_istable(state, 3)) {
        lua_getfield(state, 3, "writable");
        entry.writable = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);

        lua_getfield(state, 3, "security");
        entry.requiresSecurity = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);

        lua_getfield(state, 3, "session");
        if (lua_isinteger(state, -1)) {
            entry.requiredSession = static_cast<UdsSession>(lua_tointeger(state, -1) & 0x7F);
        }
        lua_pop(state, 1);
    }

    node->m_server->setIdentifier(static_cast<std::uint16_t>(identifier), std::move(entry));
    return 0;
}

int LuaEcuNode::luaUdsTroubleCode(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr || node->m_server == nullptr) {
        return luaL_error(state, "uds_dtc: this ECU has no diagnostic addresses");
    }

    const lua_Integer code = luaL_checkinteger(state, 1);

    // 0x08 is "confirmed", which is what a scan tool shows as a stored fault -
    // and what a script that just wants a fault to exist means.
    const lua_Integer status = luaL_optinteger(state, 2, 0x08);

    node->m_server->addTroubleCode(static_cast<std::uint32_t>(code & 0xFF'FFFF),
                                   static_cast<std::uint8_t>(status & 0xFF));
    return 0;
}

int LuaEcuNode::luaUdsClearTroubleCodes(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr || node->m_server == nullptr) {
        return luaL_error(state, "uds_clear_dtc: this ECU has no diagnostic addresses");
    }

    node->m_server->clearTroubleCodes();
    return 0;
}

int LuaEcuNode::luaUdsSession(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr || node->m_server == nullptr) {
        return luaL_error(state, "uds_session: this ECU has no diagnostic addresses");
    }

    // Both, because a script that wants to behave differently while unlocked
    // needs the second one and would otherwise keep its own copy - which would
    // be wrong every time the session expired underneath it.
    lua_pushinteger(state, static_cast<lua_Integer>(node->m_server->session()));
    lua_pushboolean(state, node->m_server->isUnlocked() ? 1 : 0);
    return 2;
}

int LuaEcuNode::luaBusLast(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "bus_last called outside an ECU node");
    }

    if (node->m_trace == nullptr) {
        return luaL_error(state,
                          "bus_last: this graph has no trace, so there is nothing to "
                          "ask about the bus");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);
    const lua_Integer channel = luaL_optinteger(state, 2, -1);

    // Linear over the identifiers seen, which is dozens on a quiet bus and a
    // few hundred on a busy one. At the rate a script asks, that is nothing;
    // at the rate the trace is written it would matter, which is why this is a
    // script-side question and not a per-frame one.
    for (const TraceIdentifierStats& stats : node->m_trace->identifiers()) {
        if (stats.identifier != static_cast<std::uint32_t>(identifier)) {
            continue;
        }

        if (channel >= 0 && stats.channel != static_cast<std::uint8_t>(channel)) {
            continue;
        }

        // The payload first, because it is what a script almost always wants:
        //     local data = bus_last(0x123)
        // and the rest is there for the script that needs it.
        lua_pushlstring(state, reinterpret_cast<const char*>(stats.lastFrame.data.data()),
                        stats.lastFrame.length);

        lua_newtable(state);

        const auto field = [state](const char* name, lua_Integer value) {
            lua_pushinteger(state, value);
            lua_setfield(state, -2, name);
        };

        field("count", static_cast<lua_Integer>(stats.count));
        field("channel", stats.channel);
        field("cycle_us", stats.lastCycleUs);
        field("min_cycle_us", stats.minCycleUs);
        field("max_cycle_us", stats.maxCycleUs);
        field("timestamp_us", static_cast<lua_Integer>(stats.lastFrame.timestampNs / 1000ULL));

        // Which bytes differed between the last two frames. Free here because
        // the trace already computes it, and the single most useful thing to
        // know about a message somebody is watching.
        field("changed_bytes", static_cast<lua_Integer>(stats.changedBytes));

        return 2;
    }

    // Nothing seen yet. Nil rather than empty bytes: "no frame" and "a frame
    // with no payload" are different, and a script checking `if data then` has
    // to be able to tell.
    lua_pushnil(state);
    return 1;
}

int LuaEcuNode::luaBusStats(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "bus_stats called outside an ECU node");
    }

    if (node->m_trace == nullptr) {
        return luaL_error(state, "bus_stats: this graph has no trace");
    }

    lua_newtable(state);

    const auto field = [state](const char* name, lua_Integer value) {
        lua_pushinteger(state, value);
        lua_setfield(state, -2, name);
    };

    field("frames", static_cast<lua_Integer>(node->m_trace->totalAppended()));
    field("identifiers", static_cast<lua_Integer>(node->m_trace->identifiers().size()));
    field("retained", static_cast<lua_Integer>(node->m_trace->size()));

    // Frames the ring overwrote. A script deciding something from a count needs
    // to know when the count stopped being all of them.
    field("discarded", static_cast<lua_Integer>(node->m_trace->discarded()));

    return 1;
}

int LuaEcuNode::luaFault(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "fault called outside an ECU node");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);
    const auto key = static_cast<std::uint32_t>(identifier);

    // fault(id) or fault(id, nil) clears it. A fault left switched on is the
    // likeliest reason a later measurement makes no sense, so turning one off
    // has to be as easy as turning it on.
    if (lua_isnoneornil(state, 2)) {
        node->m_faults.erase(key);
        return 0;
    }

    luaL_checktype(state, 2, LUA_TTABLE);

    Fault fault;

    lua_getfield(state, 2, "freeze");
    fault.freeze = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, 2, "dlc");
    if (lua_isinteger(state, -1)) {
        fault.dlc = static_cast<int>(lua_tointeger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "truncate");
    if (lua_isinteger(state, -1)) {
        fault.truncate = static_cast<int>(lua_tointeger(state, -1));
    }
    lua_pop(state, 1);

    // flip = { [byte index] = mask }, one-based like every other index a Lua
    // programmer types.
    lua_getfield(state, 2, "flip");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);

        while (lua_next(state, -2) != 0) {
            if (lua_isinteger(state, -2) && lua_isinteger(state, -1)) {
                const lua_Integer index = lua_tointeger(state, -2);
                const lua_Integer mask = lua_tointeger(state, -1);

                if (index >= 1 && index <= static_cast<lua_Integer>(kMaxCanPayload)) {
                    fault.flips.emplace_back(static_cast<std::size_t>(index - 1),
                                             static_cast<std::uint8_t>(mask & 0xFF));
                }
            }

            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    // A fault declared again keeps what it had *seen*. Anything else makes
    // `fault(id, { freeze = true })` from a timer - which is the obvious way to
    // write "freeze it from now on" - reset the remembered payload every call,
    // so the message never actually froze. The declaration is the intent; the
    // memory is the identifier's.
    if (const auto existing = node->m_faults.find(key); existing != node->m_faults.end()) {
        fault.previous = std::move(existing->second.previous);
        fault.hasPrevious = existing->second.hasPrevious;
    }

    node->m_faults[key] = std::move(fault);
    return 0;
}

void LuaEcuNode::applyFaults()
{
    if (m_faults.empty()) {
        return;
    }

    for (CanFrame& frame : m_outgoing) {
        const auto found = m_faults.find(frame.identifier);
        if (found == m_faults.end()) {
            continue;
        }

        Fault& fault = found->second;

        if (fault.freeze && fault.hasPrevious) {
            // The previous payload again, whatever the script just built. A
            // stuck ECU: the counter stops, the CRC goes stale, and nothing
            // here had to know which byte was which.
            frame.length = static_cast<std::uint8_t>(
                std::min(fault.previous.size(), kMaxCanPayload));
            std::copy_n(fault.previous.begin(), frame.length, frame.data.begin());
        } else {
            fault.previous.assign(frame.data.begin(), frame.data.begin() + frame.length);
            fault.hasPrevious = true;
        }

        for (const auto& [index, mask] : fault.flips) {
            if (index < frame.length) {
                frame.data[index] ^= mask;
            }
        }

        if (fault.truncate >= 0) {
            frame.length = static_cast<std::uint8_t>(
                std::min<std::size_t>(static_cast<std::size_t>(fault.truncate), frame.length));
        }

        // The DLC last, so it wins over anything the truncation implied - which
        // is the whole point of being able to set it: a frame carrying three
        // bytes and claiming eight.
        frame.dlc = fault.dlc >= 0 ? static_cast<std::uint8_t>(fault.dlc) : frame.length;

        ++m_corrupted;
    }
}

int LuaEcuNode::luaEvery(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "every called outside an ECU node");
    }

    const lua_Integer milliseconds = luaL_checkinteger(state, 1);

    if (milliseconds <= 0) {
        // A period of zero is a loop that never yields, and a negative one is
        // a typo. Both are refused where they were written.
        return luaL_error(state, "every: a period is milliseconds and has to be positive");
    }

    const LuaRuntime::CallableRef callable = node->m_lua->storeCallable(2);

    if (callable == 0) {
        return luaL_error(state, "every: the second argument has to be a function");
    }

    Repeating job;
    job.interval = std::chrono::milliseconds{milliseconds};
    job.callable = callable;

    // Due immediately, so a script that says every(1000, ...) sees its first
    // call at the start of the measurement rather than a second into it. An ECU
    // that goes quiet for its first cycle is a difference somebody notices.
    job.next = std::chrono::steady_clock::now();

    node->m_repeating.push_back(std::move(job));

    lua_pushinteger(state, static_cast<lua_Integer>(node->m_repeating.size()));
    return 1;
}

int LuaEcuNode::luaCyclic(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "cyclic called outside an ECU node");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);
    const lua_Integer milliseconds = luaL_checkinteger(state, 2);

    if (milliseconds <= 0) {
        return luaL_error(state, "cyclic: a cycle time is milliseconds and has to be positive");
    }

    Repeating job;
    job.isMessage = true;
    job.identifier = static_cast<std::uint32_t>(identifier);
    job.extended = identifier > static_cast<lua_Integer>(kMaxStandardIdentifier);
    job.interval = std::chrono::milliseconds{milliseconds};
    job.next = std::chrono::steady_clock::now();

    // Bytes, or a function that produces them. The second is what makes a
    // cyclic message worth having: a counter that increments, a signal that
    // moves, a payload built from the ECU's own state.
    if (lua_isfunction(state, 3)) {
        job.provider = node->m_lua->storeCallable(3);
    } else {
        std::size_t length = 0;
        const char* payload = luaL_checklstring(state, 3, &length);

        if (length > kMaxCanPayload) {
            return luaL_error(state, "cyclic: %d bytes is more than a frame carries",
                              static_cast<int>(length));
        }

        job.payload.assign(reinterpret_cast<const std::uint8_t*>(payload),
                           reinterpret_cast<const std::uint8_t*>(payload) + length);
    }

    if (lua_istable(state, 4)) {
        lua_getfield(state, 4, "extended");
        if (!lua_isnil(state, -1)) {
            job.extended = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
    }

    // Replacing a cycle already declared for the same identifier, rather than
    // adding a second: two jobs sending 0x100 at different rates is never what
    // anybody meant, and it is what a reloaded script would otherwise produce.
    for (Repeating& existing : node->m_repeating) {
        if (existing.isMessage && existing.identifier == job.identifier) {
            node->m_lua->releaseCallable(existing.provider);
            existing = std::move(job);
            lua_pushboolean(state, 1);
            return 1;
        }
    }

    node->m_repeating.push_back(std::move(job));

    lua_pushboolean(state, 1);
    return 1;
}

int LuaEcuNode::luaStopCyclic(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "stop_cyclic called outside an ECU node");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);

    // Optional second argument: false stops it, true starts it again. A message
    // that can be silenced and brought back is what fault injection is made of -
    // "what does the rest of the network do when this ECU goes quiet?" - so
    // stopping is not a deletion.
    const bool stopped = lua_isnone(state, 2) || lua_toboolean(state, 2) == 0;

    bool found = false;

    for (Repeating& job : node->m_repeating) {
        if (job.isMessage && job.identifier == static_cast<std::uint32_t>(identifier)) {
            // Only a real change touches the schedule. Resuming a message that
            // is already running used to push its next send a full cycle away,
            // so a script calling stop_cyclic(id, true) from a fast timer -
            // which is the ordinary way to say "keep going unless X" - silenced
            // the message it was trying to keep alive. A no-op has to be a
            // no-op.
            if (job.stopped != stopped) {
                job.stopped = stopped;

                if (!stopped) {
                    // Resumed on its own cycle rather than firing immediately,
                    // so that stopping and starting does not produce a burst of
                    // everything that was missed.
                    job.next = std::chrono::steady_clock::now() + job.interval;
                }
            }

            found = true;
        }
    }

    lua_pushboolean(state, found ? 1 : 0);
    return 1;
}

int LuaEcuNode::luaEmit(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "emit called outside an ECU node");
    }

    if (node->m_outgoing.size() >= kMaximumEmitsPerPass) {
        // Told, not silently dropped: a script hitting this has a loop it did
        // not mean to write, and the error names the limit so the cause is
        // obvious rather than "some frames went missing".
        return luaL_error(state,
                          "emit: more than %d frames in one pass - check for an "
                          "unbounded loop",
                          static_cast<int>(kMaximumEmitsPerPass));
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);

    std::size_t length = 0;
    const char* payload = luaL_checklstring(state, 2, &length);

    // Options table is optional: emit(id, data) is the common case, and
    // emit(id, data, {extended = true, fd = true}) is the rest.
    bool extended = identifier > static_cast<lua_Integer>(kMaxStandardIdentifier);
    bool isFd = false;
    bool bitRateSwitch = false;

    if (lua_istable(state, 3)) {
        lua_getfield(state, 3, "extended");
        if (!lua_isnil(state, -1)) {
            extended = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);

        lua_getfield(state, 3, "fd");
        isFd = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);

        lua_getfield(state, 3, "brs");
        bitRateSwitch = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }

    const std::size_t maximum = isFd ? kMaxCanPayload : kMaxClassicCanPayload;
    if (length > maximum) {
        return luaL_error(state,
                          "emit: %d bytes of payload, but a %s frame holds at most %d",
                          static_cast<int>(length),
                          isFd ? "CAN FD" : "classic CAN",
                          static_cast<int>(maximum));
    }

    CanFrame frame;
    frame.identifier = static_cast<std::uint32_t>(identifier);
    frame.format = extended ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    frame.channel = node->m_transmitChannel;
    frame.direction = CanDirection::Tx;
    frame.fd = isFd;
    frame.brs = isFd && bitRateSwitch;
    frame.length = static_cast<std::uint8_t>(length);
    frame.dlc = dlcFromPayloadLength(frame.length, isFd);

    if (!isValidIdentifier(frame.identifier, frame.format)) {
        return luaL_error(state,
                          "emit: identifier 0x%X does not fit a %s frame",
                          static_cast<unsigned>(frame.identifier),
                          extended ? "29-bit" : "11-bit");
    }

    std::copy_n(payload, length, reinterpret_cast<char*>(frame.data.data()));

    // The timestamp is filled by whatever transmits it. A frame the script
    // invented has no bus time yet, and inventing one here would put a number
    // in the trace that never happened.
    node->m_outgoing.push_back(frame);
    ++node->m_emitted;

    return 0;
}

/// emit_signal("VehicleSpeed", { SpeedKmh = 85.0 })
///
/// The point of the whole database layer, from a script's side: the script says
/// what it means and the definition decides where the bits go. Without it every
/// ECU carries its own copy of a bit layout, in a string.pack format string,
/// which is the single easiest thing in this file to get wrong and the hardest
/// to notice - a mis-packed frame transmits perfectly.
///
/// A name that is not in the database is an error, because a typo never becomes
/// correct and the script should stop. A *value* out of range is not: it
/// saturates and is counted. A control loop briefly asking for 300% torque has
/// a bug worth seeing, but taking the ECU down over it would take the rest of
/// the simulation with it.
int LuaEcuNode::luaEmitSignal(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "emit_signal called outside an ECU node");
    }

    if (!node->m_database) {
        return luaL_error(state,
                          "emit_signal needs a database: set the block's "
                          "'database' parameter to a .dbc file");
    }

    if (node->m_outgoing.size() >= kMaximumEmitsPerPass) {
        return luaL_error(state,
                          "emit_signal: more than %d frames in one pass - check "
                          "for an unbounded loop",
                          static_cast<int>(kMaximumEmitsPerPass));
    }

    const char* messageName = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    const CanMessage* message = node->m_database->findByName(messageName);
    if (message == nullptr) {
        return luaL_error(state, "emit_signal: no message named '%s' in the database",
                          messageName);
    }

    CanFrame frame = message->makeFrame();
    frame.channel = node->m_transmitChannel;

    // Walk the table the script passed rather than the message's signal list:
    // a script setting three of eight signals leaves the other five at zero,
    // which is what makeFrame already gave us and what the caller expects.
    lua_pushnil(state);
    while (lua_next(state, 2) != 0) {
        // key at -2, value at -1. lua_tostring on a *key* would rewrite it in
        // place and confuse lua_next, so the key is checked to be a string
        // rather than converted.
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            return luaL_error(state,
                              "emit_signal: the table's keys have to be signal names");
        }

        const char* signalName = lua_tostring(state, -2);
        const double value = luaL_checknumber(state, -1);

        const CanSignal* signal = message->findSignal(signalName);
        if (signal == nullptr) {
            lua_pop(state, 2);
            return luaL_error(state, "emit_signal: '%s' has no signal named '%s'",
                              messageName, signalName);
        }

        if (!signal->encode(value, frame.data.data(), frame.length)) {
            ++node->m_saturated;
        }

        lua_pop(state, 1);
    }

    node->m_outgoing.push_back(frame);
    ++node->m_emitted;

    return 0;
}

/// local name, signals = decode(id, data)
///
/// Returns nil when there is no database or the identifier is not in it, so a
/// script can ask about every frame it receives and act only on the ones it
/// understands - which is what an ECU on a shared bus actually does.
int LuaEcuNode::luaDecode(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "decode called outside an ECU node");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);

    std::size_t length = 0;
    const char* payload = luaL_checklstring(state, 2, &length);

    if (!node->m_database) {
        lua_pushnil(state);
        return 1;
    }

    const auto raw = static_cast<std::uint32_t>(identifier);
    const CanFrameFormat format = raw > kMaxStandardIdentifier ? CanFrameFormat::Extended
                                                               : CanFrameFormat::Standard;

    const CanMessage* message = node->m_database->find(raw, format);
    if (message == nullptr) {
        lua_pushnil(state);
        return 1;
    }

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload);

    lua_pushstring(state, message->name.c_str());
    lua_newtable(state);

    // signalsIn, not the whole list: on a multiplexed message the signals that
    // are not in this frame are not zero, they are absent, and a script reading
    // signals.Voltage on a page that does not carry it should get nil rather
    // than a plausible number.
    for (const CanSignal* signal : message->signalsIn(bytes, length)) {
        if (!signal->fitsIn(length)) {
            // Left out rather than reported as zero. Same reason as the
            // decoder's truncated flag: "we could not read it" and "it reads
            // zero" must not look the same.
            continue;
        }

        lua_pushnumber(state, signal->decode(bytes, length));
        lua_setfield(state, -2, signal->name.c_str());
    }

    return 2;
}

int LuaEcuNode::luaSetTimer(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "set_timer called outside an ECU node");
    }

    const lua_Integer milliseconds = luaL_checkinteger(state, 1);
    node->m_timerInterval = std::chrono::milliseconds{std::max<lua_Integer>(milliseconds, 0)};

    return 0;
}

int LuaEcuNode::luaLogMessage(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "log_message called outside an ECU node");
    }

    const char* text = luaL_checkstring(state, 1);
    node->report(std::format("{}: {}", node->m_name, text), false);

    return 0;
}

int LuaEcuNode::luaGetTimeMicroseconds(lua_State* state)
{
    LuaEcuNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "get_time_us called outside an ECU node");
    }

    // Since the measurement started, not since the epoch: a script comparing
    // this against a frame timestamp is comparing two numbers on the same
    // clock, which is the only way either of them is useful.
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - node->m_started);

    lua_pushinteger(state, static_cast<lua_Integer>(elapsed.count()));
    return 1;
}

} // namespace torquebus
