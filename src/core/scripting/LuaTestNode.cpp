// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/scripting/LuaTestNode.h"

#include "core/scripting/LuaPrelude.h"
#include "core/scripting/LuaTestPrelude.h"

#include <algorithm>
#include <format>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace torquebus {
namespace {

/// Upper bound on frames one sequence may send in a single pass.
///
/// A test that sends in a loop is a test with a bug, and stopping at a number
/// beats stopping when memory runs out.
constexpr std::size_t kMaximumSendsPerPass = 256;

/// Ceiling on any single wait.
///
/// Not a limit on how long a test may take - a sequence can wait repeatedly -
/// but on how long one `expect` may hang. A typo turning 200 into 200000 would
/// otherwise leave a run that looks like it is working for three minutes, which
/// is the failure mode a test tool can least afford.
constexpr double kMaximumWaitMs = 60'000.0;

} // namespace

LuaTestNode::LuaTestNode(std::string source, std::string name, std::uint8_t transmitChannel)
    : m_source{std::move(source)}
    , m_name{std::move(name)}
    , m_transmitChannel{transmitChannel}
{ }

LuaTestNode::~LuaTestNode() = default;

LuaTestNode* LuaTestNode::self(lua_State* state)
{
    return static_cast<LuaTestNode*>(lua_touserdata(state, lua_upvalueindex(1)));
}

std::uint64_t LuaTestNode::elapsedNs() const
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - m_started)
                                          .count());
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result LuaTestNode::prepare(std::size_t maximumBatchSize)
{
    (void)maximumBatchSize;

    m_lua = std::make_unique<LuaRuntime>();

    m_want = Want{};
    m_caseChecks.clear();
    m_caseName.clear();
    m_caseOpen = false;

    m_complete = false;
    m_faulted = false;

    m_declared = 0;
    m_passed = 0;
    m_failed = 0;
    m_errored = 0;
    m_checks = 0;
    m_sent = 0;

    m_outgoing.clear();
    m_outgoing.reserve(kMaximumSendsPerPass);

    m_started = std::chrono::steady_clock::now();

    if (Result result = m_lua->openLibraries(); result.failed()) {
        return result;
    }

    m_lua->registerFunction("send", &LuaTestNode::luaSend, this);
    m_lua->registerFunction("log", &LuaTestNode::luaLog, this);

    // Named as in an ECU script as well, so that somebody who has written one
    // does not have to learn a second vocabulary for the same two things.
    m_lua->registerFunction("log_message", &LuaTestNode::luaLog, this);
    m_lua->registerFunction("get_time_us", &LuaTestNode::luaGetTimeMicroseconds, this);

    m_lua->registerFunction("__tb_want_time", &LuaTestNode::luaWantTime, this);
    m_lua->registerFunction("__tb_want_frame", &LuaTestNode::luaWantFrame, this);
    m_lua->registerFunction("__tb_want_again", &LuaTestNode::luaWantAgain, this);
    m_lua->registerFunction("__tb_ready", &LuaTestNode::luaReady, this);
    m_lua->registerFunction("__tb_taken", &LuaTestNode::luaTaken, this);

    m_lua->registerFunction("__tb_check", &LuaTestNode::luaCheck, this);
    m_lua->registerFunction("__tb_case_started", &LuaTestNode::luaCaseStarted, this);
    m_lua->registerFunction("__tb_case_finished", &LuaTestNode::luaCaseFinished, this);
    m_lua->registerFunction("__tb_case_errored", &LuaTestNode::luaCaseErrored, this);

    m_lua->setGlobal("node_name", LuaValue::fromString(m_name));
    m_lua->setGlobal("channel", LuaValue::fromInteger(m_transmitChannel));
    m_lua->setGlobalTable("parameters", m_scriptParameters);

    // The ECU prelude first: tb.now(), the generators and the CRC are as useful
    // to a test that has to build a valid request as to an ECU that answers
    // one, and a second copy of them would be a second thing to keep correct.
    if (Result result = m_lua->load(kLuaPrelude, "prelude"); result.failed()) {
        return result;
    }

    if (Result result = m_lua->load(kLuaTestPrelude, "test-prelude"); result.failed()) {
        return result;
    }

    if (Result result = m_lua->load(m_source, m_name); result.failed()) {
        return Result::error(result.code(),
                             std::format("{}: {}", m_name, std::string{result.message()}));
    }

    // The cases are declared by running the file, which has just happened. What
    // is left is to count them and to say so - a sequence that declared nothing
    // is a file somebody edited wrongly, and it must not read as a green run.
    LuaValue count;
    if (Result result = m_lua->call("__tb_case_count", {}, count); result.failed()) {
        return Result::error(result.code(),
                             std::format("{}: {}", m_name, std::string{result.message()}));
    }

    m_declared = count.type == LuaValue::Type::Integer ? static_cast<std::uint64_t>(count.integer)
                                                       : static_cast<std::uint64_t>(count.number);

    if (Result result = m_lua->call("__tb_begin"); result.failed()) {
        return Result::error(result.code(),
                             std::format("{}: {}", m_name, std::string{result.message()}));
    }

    if (m_report != nullptr) {
        m_report->begin(static_cast<std::size_t>(m_declared));
    }

    if (m_declared == 0) {
        // Said once, at Start, rather than left to be noticed as an empty
        // table. "No cases" and "all cases passed" look identical in a summary
        // and mean opposite things.
        report(std::format("{}: the sequence declared no test cases.", m_name), true);

        m_complete = true;

        if (m_report != nullptr) {
            m_report->finish();
        }
    } else {
        report(std::format("{}: {} test case(s) to run.", m_name, m_declared), false);
    }

    return Result::ok();
}

void LuaTestNode::process(NodeContext& context)
{
    if (!m_lua || m_complete || m_faulted) {
        return;
    }

    m_outgoing.clear();

    // --- Is what the running case is waiting for here yet? -----------------
    //
    // Matched here rather than inside the binding, because this is the only
    // place the frames of this pass exist. A case waiting on a timeout costs
    // this loop and nothing else; a case waiting on nothing costs not even
    // that.
    //
    // Before the pump, which means a frame arriving in the same pass as the
    // `expect` that asked for it is matched on the *next* pass - one dispatch
    // interval, 5 ms by default. That is a real limit and it is written down
    // rather than pretended away: a test measuring a response time reads a
    // number that may be up to one pass late, which is why every timing
    // assertion in a sequence should be a range.
    if (m_want.active && m_want.wantsFrame && !m_want.matched) {
        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            if (frame.identifier != m_want.identifier) {
                continue;
            }

            if (m_want.hasChannel && frame.channel != m_want.channel) {
                continue;
            }

            m_want.matched = true;
            m_want.frame = frame;
            break;
        }
    }

    // --- One resume ---------------------------------------------------------
    LuaValue status;

    if (Result result = m_lua->call("__tb_pump", {}, status); result.failed()) {
        // The framework itself threw, which is not a test failure: a case's own
        // error is caught inside __tb_pump and reported as one. Reaching here
        // means the sequence cannot be run at all, so it stops rather than
        // repeating the same error 200 times a second.
        m_faulted = true;

        report(std::format("{}: the sequence stopped: {}", m_name, std::string{result.message()}),
               true);

        if (m_report != nullptr) {
            m_report->finish();
        }

        return;
    }

    if (status.type == LuaValue::Type::String && status.text == "finished") {
        m_complete = true;

        if (m_report != nullptr) {
            m_report->finish();
        }

        // The one line somebody reads. Both counts spelled out rather than
        // appended conditionally: a summary whose shape changes with the result
        // is one people learn to skim, and the zero is the reassuring part.
        const bool green = m_failed == 0 && m_errored == 0;

        report(std::format("{}: {} of {} case(s) passed, {} failed, {} in error.",
                           m_name,
                           m_passed,
                           m_declared,
                           m_failed,
                           m_errored),
               !green);
    }

    if (!m_outgoing.empty()) {
        context.publish<CanFrame>(0, m_outgoing);
    }
}

void LuaTestNode::finish()
{
    if (m_complete || m_declared == 0) {
        return;
    }

    // Stopped mid-run. The case that was in flight did not pass - it did not
    // finish - and a report that quietly dropped it would be missing exactly
    // the case somebody stopped the measurement to look at.
    if (m_caseOpen) {
        closeCase(TestOutcome::Errored, "the measurement stopped before this case finished");
    }

    report(std::format("{}: stopped after {} of {} case(s).",
                       m_name,
                       m_passed + m_failed + m_errored,
                       m_declared),
           true);

    if (m_report != nullptr) {
        m_report->finish();
    }
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

void LuaTestNode::closeCase(TestOutcome outcome, std::string message)
{
    if (!m_caseOpen) {
        return;
    }

    TestCaseResult result;
    result.name = m_caseName;
    result.outcome = outcome;
    result.message = std::move(message);
    result.startedNs = m_caseStartedNs;
    result.finishedNs = elapsedNs();
    result.checks = std::move(m_caseChecks);

    switch (outcome) {
    case TestOutcome::Passed:
        ++m_passed;
        break;
    case TestOutcome::Failed:
        ++m_failed;
        break;
    case TestOutcome::Errored:
        ++m_errored;
        break;
    case TestOutcome::Running:
        break;
    }

    // In the Output panel as well as in the report. Somebody watching a long
    // run wants to see it progress, and somebody reading a trace wants the
    // failure to appear beside the frames that caused it.
    report(std::format("{}: {} - {}{}{}",
                       m_name,
                       outcome == TestOutcome::Passed   ? "PASS"
                       : outcome == TestOutcome::Failed ? "FAIL"
                                                        : "ERROR",
                       result.name,
                       result.message.empty() ? "" : ": ",
                       result.message),
           outcome != TestOutcome::Passed);

    if (m_report != nullptr) {
        m_report->add(std::move(result));
    }

    m_caseChecks.clear();
    m_caseOpen = false;

    // A case that ended while waiting leaves nothing behind for the next one.
    m_want = Want{};
}

void LuaTestNode::report(const std::string& text, bool isError)
{
    if (m_log) {
        m_log(text, isError);
    }
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

int LuaTestNode::luaSend(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "send called outside a test sequence");
    }

    if (node->m_outgoing.size() >= kMaximumSendsPerPass) {
        return luaL_error(state,
                          "send: more than %d frames in one pass - a test that sends "
                          "in a loop is a test with a bug",
                          static_cast<int>(kMaximumSendsPerPass));
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);

    std::size_t length = 0;
    const char* payload = luaL_checklstring(state, 2, &length);

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
                          "send: %d bytes of payload, but a %s frame holds at most %d",
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
                          "send: identifier 0x%X does not fit a %s frame",
                          static_cast<unsigned>(frame.identifier),
                          extended ? "29-bit" : "11-bit");
    }

    std::copy_n(payload, length, reinterpret_cast<char*>(frame.data.data()));

    node->m_outgoing.push_back(frame);
    ++node->m_sent;

    return 0;
}

int LuaTestNode::luaLog(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "log called outside a test sequence");
    }

    const char* text = luaL_checkstring(state, 1);
    node->report(std::format("{}: {}", node->m_name, text), false);

    return 0;
}

int LuaTestNode::luaGetTimeMicroseconds(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "get_time_us called outside a test sequence");
    }

    lua_pushinteger(state, static_cast<lua_Integer>(node->elapsedNs() / 1000));
    return 1;
}

// --- Waiting ---------------------------------------------------------------

int LuaTestNode::luaWantTime(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "wait called outside a test sequence");
    }

    const lua_Number milliseconds = luaL_checknumber(state, 1);

    if (!(milliseconds >= 0.0) || milliseconds > kMaximumWaitMs) {
        return luaL_error(state,
                          "wait: %f ms is not a wait between 0 and %d ms",
                          static_cast<double>(milliseconds),
                          static_cast<int>(kMaximumWaitMs));
    }

    node->m_want = Want{};
    node->m_want.active = true;
    node->m_want.wantsFrame = false;
    node->m_want.deadline =
        std::chrono::steady_clock::now()
        + std::chrono::microseconds{static_cast<std::int64_t>(milliseconds * 1000.0)};

    return 0;
}

int LuaTestNode::luaWantFrame(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "expect called outside a test sequence");
    }

    const lua_Integer identifier = luaL_checkinteger(state, 1);
    const lua_Number within = luaL_checknumber(state, 2);

    if (!(within > 0.0) || within > kMaximumWaitMs) {
        return luaL_error(state,
                          "expect: %f ms is not a timeout between 0 and %d ms",
                          static_cast<double>(within),
                          static_cast<int>(kMaximumWaitMs));
    }

    node->m_want = Want{};
    node->m_want.active = true;
    node->m_want.wantsFrame = true;
    node->m_want.identifier = static_cast<std::uint32_t>(identifier);

    if (!lua_isnoneornil(state, 3)) {
        node->m_want.hasChannel = true;
        node->m_want.channel = static_cast<std::uint8_t>(luaL_checkinteger(state, 3));
    }

    node->m_want.deadline = std::chrono::steady_clock::now()
                            + std::chrono::microseconds{static_cast<std::int64_t>(within * 1000.0)};

    return 0;
}

int LuaTestNode::luaWantAgain(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "expect called outside a test sequence");
    }

    // Re-arms the wait that __tb_taken just disarmed, for a `where` filter that
    // rejected the frame it was handed.
    //
    // **The deadline is deliberately not moved.** "Within 200 ms" is a
    // statement about the wait, not about each candidate frame - a filter that
    // reset the clock on every frame it rejected would turn a busy bus into a
    // wait that never ends, and the test would hang rather than fail.
    node->m_want.matched = false;
    node->m_want.active = true;

    return 0;
}

int LuaTestNode::luaReady(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "__tb_ready called outside a test sequence");
    }

    if (!node->m_want.active) {
        lua_pushboolean(state, 1);
        return 1;
    }

    const bool expired = std::chrono::steady_clock::now() >= node->m_want.deadline;

    lua_pushboolean(state, (node->m_want.matched || expired) ? 1 : 0);
    return 1;
}

int LuaTestNode::luaTaken(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "__tb_taken called outside a test sequence");
    }

    const bool matched = node->m_want.matched;
    const CanFrame frame = node->m_want.frame;

    // Disarmed rather than wiped: __tb_want_again re-arms it with the same
    // identifier and the same deadline, which is exactly what a `where` filter
    // needs. Wiping the whole want here was the first version of this, and it
    // made every filtered expect() return nil on the first frame it rejected -
    // a test that passed because it never looked.
    node->m_want.active = false;
    node->m_want.matched = false;

    if (!matched) {
        // The deadline passed. Nil rather than an error: silence is a fact
        // about the network, and a case is entitled to be testing for it.
        lua_pushnil(state);
        return 1;
    }

    lua_createtable(state, 0, 5);

    lua_pushinteger(state, static_cast<lua_Integer>(frame.identifier));
    lua_setfield(state, -2, "id");

    lua_pushlstring(state, reinterpret_cast<const char*>(frame.data.data()), frame.length);
    lua_setfield(state, -2, "data");

    lua_pushinteger(state, static_cast<lua_Integer>(frame.channel));
    lua_setfield(state, -2, "channel");

    lua_pushboolean(state, frame.isExtended() ? 1 : 0);
    lua_setfield(state, -2, "extended");

    // Microseconds on the bus clock, which is what a test comparing two
    // arrivals needs and what the trace beside it is showing. Microseconds
    // rather than the frame's own nanoseconds because Lua numbers are doubles
    // once they leave integer range, and a test doing arithmetic on a
    // nanosecond count from a long run would start losing the low digits.
    lua_pushinteger(state, static_cast<lua_Integer>(frame.timestampNs / 1000U));
    lua_setfield(state, -2, "timestamp_us");

    return 1;
}

// --- Results ---------------------------------------------------------------

int LuaTestNode::luaCheck(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "a check outside a test sequence");
    }

    TestCheck check;
    check.description = luaL_checkstring(state, 1);
    check.passed = lua_toboolean(state, 2) != 0;

    if (!lua_isnoneornil(state, 3)) {
        check.detail = luaL_checkstring(state, 3);
    }

    node->m_caseChecks.push_back(std::move(check));
    ++node->m_checks;

    return 0;
}

int LuaTestNode::luaCaseStarted(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "a case outside a test sequence");
    }

    node->m_caseName = luaL_checkstring(state, 1);
    node->m_caseStartedNs = node->elapsedNs();
    node->m_caseChecks.clear();
    node->m_caseOpen = true;

    return 0;
}

int LuaTestNode::luaCaseFinished(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "a case outside a test sequence");
    }

    const bool passed = lua_toboolean(state, 1) != 0;

    std::string message;
    if (!lua_isnoneornil(state, 2)) {
        message = luaL_checkstring(state, 2);
    }

    node->closeCase(passed ? TestOutcome::Passed : TestOutcome::Failed, std::move(message));

    return 0;
}

int LuaTestNode::luaCaseErrored(lua_State* state)
{
    LuaTestNode* node = self(state);
    if (node == nullptr) {
        return luaL_error(state, "a case outside a test sequence");
    }

    node->closeCase(TestOutcome::Errored, luaL_checkstring(state, 1));

    return 0;
}

} // namespace torquebus
