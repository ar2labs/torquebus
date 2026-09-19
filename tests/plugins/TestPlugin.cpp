// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The plugins the loader tests load.
//
// One source file built several times, each with a different macro, because the
// interesting cases are the refusals and a refusal has to be a *real* library
// that really is wrong - a build key that genuinely differs, a symbol that
// genuinely is not exported. Faking any of that in the test would be testing
// the fake.
//
//   (default)  registers a node type and says so
//   DECLINES   returns false, the way a driver whose SDK is missing does
//   THROWS     throws out of registerWith
//   BAD_KEY    a build key that cannot match
//   BAD_ABI    an ABI version from the future
//   NO_SYMBOL  a library with nothing exported at all

#include "plugins/host/PluginApi.h"

#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#if !defined(TORQUEBUS_TEST_PLUGIN_NO_SYMBOL)

namespace {

using namespace torquebus;

/// A node that passes its frames through untouched.
///
/// Deliberately the smallest thing that is genuinely a node: the point is that
/// a type registered from outside the binary can be found in the catalogue and
/// built, not that it does anything interesting once built.
class PassThroughNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.passthrough"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kPorts;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
    }

    void process(NodeContext& context) override
    {
        context.publish<CanFrame>(0, context.in<CanFrame>(0));
    }

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };
};

bool registerWith(const torquebus::plugins::PluginHost& host)
{
#if defined(TORQUEBUS_TEST_PLUGIN_THROWS)
    throw std::runtime_error("this plugin is supposed to throw");
#elif defined(TORQUEBUS_TEST_PLUGIN_DECLINES)
    // What a driver plugin does when its SDK turns out not to be installed: it
    // says why itself, because it is the only one that knows, and then declines.
    host.log("test plugin: pretending its SDK is not installed", true);
    return false;
#else
    host.nodes->registerType(
        NodeTypeInfo{
            .typeName = "test.passthrough",
            .displayName = "Test Pass-through",
            .category = "Transforms",
            .description = "Registered by a plugin, for the loader tests.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
        },
        [](const NodeParameters&,
           const NodeBuildContext&,
           std::string_view,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            out = std::make_unique<PassThroughNode>();
            return Result::ok();
        });

    host.log("test plugin: registered test.passthrough", false);
    return true;
#endif
}

[[nodiscard]] torquebus::plugins::PluginInfo makeInfo()
{
    torquebus::plugins::PluginInfo info;

#if defined(TORQUEBUS_TEST_PLUGIN_BAD_ABI)
    // A plugin from a future TorqueBus. The host has to refuse it by reading
    // only the two fields whose position both sides already agreed on.
    info.abiVersion = torquebus::plugins::kPluginAbiVersion + 1000U;
#endif

#if defined(TORQUEBUS_TEST_PLUGIN_BAD_KEY)
    // Built by another compiler, in another configuration, on another day.
    info.buildKey = "torquebus-abi-1/some-other-compiler/x64/stl-0";
#endif

    info.name = "test";
    info.displayName = "Loader Test Plugin";
    info.version = "1.0";
    info.registerWith = &registerWith;

    return info;
}

} // namespace

TORQUEBUS_DECLARE_PLUGIN(makeInfo())

#else

// NO_SYMBOL: a perfectly good library that is not a TorqueBus plugin. The
// loader has to say that in words rather than passing over it.
extern "C" __declspec(dllexport) int torquebusTestPluginNotTheEntryPoint()
{
    return 0;
}

#endif
