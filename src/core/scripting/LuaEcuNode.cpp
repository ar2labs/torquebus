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
    m_lua->registerFunction("emit_signal", &LuaEcuNode::luaEmitSignal, this);
    m_lua->registerFunction("decode", &LuaEcuNode::luaDecode, this);
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
