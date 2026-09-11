// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A test sequence: one Lua script that checks the network and reports.
//
// The Lua ECU answers the question "what does this network look like". This
// answers "is it right", which is a different question and needs a different
// shape: a sequence has an order, it ends, and it produces a verdict somebody
// can hand to someone else.
//
//     test("Engine speed appears and is plausible", function()
//         local frame = expect_frame(0x100, { within = 200 })
//         assert_equal(#frame.data, 8, "DLC")
//     end)
//
//     test("The lamp goes out once the fault is cleared", function()
//         send(0x7E0, "\x14\xFF\xFF\xFF")
//         wait(100)
//         expect_silence(0x300, 500)
//     end)
//
// Each case runs in a Lua coroutine, so the script is written in the order
// things happen while the node still returns from process() immediately - see
// LuaTestPrelude.h for why that division is the whole design.
//
// What this node does *not* do: fail a measurement, stop a recording, or change
// what any other node sees. A test observes and reports. A sequence that could
// halt the run it is measuring would make every failure ambiguous - did the
// network do that, or did the test?

#pragma once

#include "core/can/CanFrame.h"
#include "core/pipeline/PipelineNode.h"
#include "core/scripting/LuaRuntime.h"
#include "core/testing/TestReport.h"

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

class LuaTestNode final : public IPipelineNode {
public:
    /// Where the sequence's log() lines and its errors go. Called on the
    /// executor thread: do not block.
    using LogHandler = std::function<void(const std::string& text, bool isError)>;

    LuaTestNode(std::string source, std::string name, std::uint8_t transmitChannel = 0);
    ~LuaTestNode() override;

    [[nodiscard]] std::string_view typeName() const noexcept override { return "lua.test"; }
    [[nodiscard]] std::string displayName() const override { return m_name; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    /// Loads the sequence and lets it declare its cases. Nothing runs yet.
    ///
    /// A sequence that will not compile fails the graph compile, like every
    /// other script - finding out that a test file has a typo *after* setting
    /// up the bench is the wrong moment.
    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    /// Marks an unfinished run as such: a sequence that was still waiting when
    /// somebody pressed Stop has not passed.
    void finish() override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Cases declared", m_declared},
            {"Cases passed", m_passed},
            {"Cases failed", m_failed},
            {"Cases in error", m_errored},
            {"Checks made", m_checks},
            {"Frames sent", m_sent},
        };
    }

    void setLogHandler(LogHandler handler) { m_log = std::move(handler); }

    /// Where results go. Not owned; null means the sequence still runs and
    /// still logs, which is what a headless test of this node wants.
    void setReport(TestReport* report) { m_report = report; }

    /// Settings the sequence reads from its `parameters` global - the same
    /// arrangement the Lua ECU has, and for the same reason: one sequence
    /// parameterised by identifier and timeout is worth more than four copies.
    void setScriptParameters(std::map<std::string, LuaValue> parameters)
    {
        m_scriptParameters = std::move(parameters);
    }

    [[nodiscard]] bool isComplete() const noexcept { return m_complete; }
    [[nodiscard]] std::uint64_t declaredCases() const noexcept { return m_declared; }

private:
    // --- Bindings, called from Lua ----------------------------------------
    static int luaSend(lua_State* state);
    static int luaLog(lua_State* state);
    static int luaGetTimeMicroseconds(lua_State* state);

    static int luaWantTime(lua_State* state);
    static int luaWantFrame(lua_State* state);
    static int luaWantAgain(lua_State* state);
    static int luaReady(lua_State* state);
    static int luaTaken(lua_State* state);

    static int luaCheck(lua_State* state);
    static int luaCaseStarted(lua_State* state);
    static int luaCaseFinished(lua_State* state);
    static int luaCaseErrored(lua_State* state);

    [[nodiscard]] static LuaTestNode* self(lua_State* state);

    void report(const std::string& text, bool isError);

    /// Files the case that just ended, and counts it.
    void closeCase(TestOutcome outcome, std::string message);

    [[nodiscard]] std::uint64_t elapsedNs() const;

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

    /// What the running case is waiting for.
    ///
    /// One at a time, because one case runs at a time - and a case waits in one
    /// place, which is what makes a sequence readable.
    struct Want final {
        bool active{false};

        /// False for a plain wait(ms), which is the same machinery with no
        /// frame to match.
        bool wantsFrame{false};

        std::uint32_t identifier{0};

        bool hasChannel{false};
        std::uint8_t channel{0};

        std::chrono::steady_clock::time_point deadline{};

        bool matched{false};
        CanFrame frame{};
    };

    Want m_want;

    /// The case being run, so a result can name it without Lua handing the
    /// name back a second time.
    std::string m_caseName;
    std::uint64_t m_caseStartedNs{0};
    std::vector<TestCheck> m_caseChecks;
    bool m_caseOpen{false};

    TestReport* m_report{nullptr};
    LogHandler m_log;
    std::map<std::string, LuaValue> m_scriptParameters;

    std::vector<CanFrame> m_outgoing;

    std::chrono::steady_clock::time_point m_started;

    bool m_complete{false};

    /// The sequence threw somewhere outside a case - a broken framework call.
    /// It stops being pumped rather than repeating the error every 5 ms.
    bool m_faulted{false};

    std::uint64_t m_declared{0};
    std::uint64_t m_passed{0};
    std::uint64_t m_failed{0};
    std::uint64_t m_errored{0};
    std::uint64_t m_checks{0};
    std::uint64_t m_sent{0};
};

} // namespace torquebus
