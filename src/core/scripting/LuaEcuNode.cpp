// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/scripting/LuaEcuNode.h"

#include <algorithm>
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

    if (Result result = m_lua->openLibraries(); result.failed()) {
        return result;
    }

    m_lua->registerFunction("emit", &LuaEcuNode::luaEmit, this);
    m_lua->registerFunction("set_timer", &LuaEcuNode::luaSetTimer, this);
    m_lua->registerFunction("log_message", &LuaEcuNode::luaLogMessage, this);
    m_lua->registerFunction("get_time_us", &LuaEcuNode::luaGetTimeMicroseconds, this);

    // Handy for a script that wants to know which channel it is standing in for
    // without being told twice.
    m_lua->setGlobal("node_name", LuaValue::fromString(m_name));
    m_lua->setGlobal("channel", LuaValue::fromInteger(m_transmitChannel));

    // Always defined, even when empty, so a script can write
    // `parameters.can_id or 0x100` without first testing that the table exists.
    m_lua->setGlobalTable("parameters", m_scriptParameters);

    if (Result result = m_lua->load(m_source, m_name); result.failed()) {
        // A script that will not compile stops the measurement from starting,
        // rather than surfacing on the first frame. The Lua message already
        // carries the file and line.
        return Result::error(result.code(),
                             std::format("{}: {}", m_name, std::string{result.message()}));
    }

    m_hasOnMessage = m_lua->hasFunction("on_message");
    m_hasOnTimer = m_lua->hasFunction("on_timer");

    m_outgoing.reserve(kMaximumEmitsPerPass);

    m_started = std::chrono::steady_clock::now();
    m_lastTimer = m_started;
    m_emitted = 0;
    m_consecutiveErrors = 0;
    m_faulted = false;

    if (m_lua->hasFunction("on_enable")) {
        if (Result result = m_lua->call("on_enable"); result.failed()) {
            return Result::error(result.code(),
                                 std::format("{}: on_enable failed: {}",
                                             m_name, std::string{result.message()}));
        }
    }

    return Result::ok();
}

void LuaEcuNode::process(NodeContext& context)
{
    if (m_faulted || !m_lua) {
        return;
    }

    m_outgoing.clear();

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
