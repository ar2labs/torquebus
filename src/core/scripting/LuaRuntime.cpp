// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The only translation unit in TorqueBus that includes lua.h.

#include "core/scripting/LuaRuntime.h"

#include "core/FilePath.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace torquebus {
namespace {

/// Pops Lua's error object and turns it into a message worth showing.
[[nodiscard]] std::string takeError(lua_State* state)
{
    if (state == nullptr) {
        return "no interpreter";
    }

    const char* message = lua_tostring(state, -1);
    std::string text = message != nullptr ? message : "unknown Lua error";
    lua_pop(state, 1);

    return text;
}

/// The standard libraries an ECU script gets.
///
/// os and io are absent on purpose - see the note on openLibraries(). package
/// is absent too: `require` would let a script reach outside the sandbox by
/// path, and scripts that need to share code will get an explicit mechanism
/// rather than a filesystem search path.
constexpr luaL_Reg kLibraries[] = {
    {LUA_GNAME, luaopen_base},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_COLIBNAME, luaopen_coroutine},
};

/// One Lua value, as the type it actually is rather than coerced.
///
/// lua_tostring on a number rewrites the value on the stack in place, which is
/// a documented way to confuse a later traversal - and a script returning a
/// number where bytes were expected is worth seeing as a number.
[[nodiscard]] LuaValue readValue(lua_State* state, int index)
{
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN:
        return LuaValue::fromBoolean(lua_toboolean(state, index) != 0);

    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) {
            return LuaValue::fromInteger(lua_tointeger(state, index));
        }
        return LuaValue::fromNumber(lua_tonumber(state, index));

    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);

        // Length-counted: a UDS response is bytes, and a zero in the middle of
        // one is ordinary. Reading it as a C string would truncate the answer
        // there and produce a response that is quietly short.
        return LuaValue::fromString(std::string{text, length});
    }

    default:
        break;
    }

    return LuaValue{}; // Nil, and anything this build cannot carry.
}

/// How many VM instructions pass between two looks at the clock.
///
/// A look is a function call and a clock read, so it is not free; a loop that
/// never returns is noticed within microseconds at this rate, which is a good deal
/// faster than anybody will be waiting.
constexpr int kInstructionsPerCheck = 2048;

} // namespace

/// Counts the heap Lua holds and refuses what would pass the limit.
///
/// Lua is built to be given an allocator: a request that returns null becomes
/// "not enough memory" inside the interpreter, a catchable error with the
/// script's own line number, and the collector gets a chance to free something
/// and try again before it does. That is exactly the failure a script that
/// builds a table forever should get, and it needs no cooperation from the script.
struct LuaRuntime::Allocator final {
    lua_Alloc inner{nullptr};
    void* innerData{nullptr};

    std::size_t used{0};
    std::size_t limit{0};

    /// Set when a request was refused; read and cleared when a call fails, to
    /// tell "out of memory" from an ordinary error.
    bool refused{false};

    static void* allocate(void* data, void* block, std::size_t oldSize, std::size_t newSize)
    {
        auto* self = static_cast<Allocator*>(data);

        // When `block` is null, `oldSize` is not a size but a tag saying what kind
        // of object Lua is making. The convention every allocator for Lua follows.
        const std::size_t before = block != nullptr ? oldSize : 0;

        if (newSize > before && self->limit != 0 && self->used + (newSize - before) > self->limit) {
            self->refused = true;
            return nullptr;
        }

        void* result = self->inner(self->innerData, block, oldSize, newSize);

        if (newSize == 0) {
            self->used -= std::min(before, self->used);
        } else if (result != nullptr) {
            self->used = self->used - std::min(before, self->used) + newSize;
        }

        return result;
    }
};

/// Gives one call a deadline.
///
/// Scoped, because the deadline must not outlive the call: the interpreter is
/// also entered to define globals and register functions, and none of that should
/// be racing a clock that was meant for a handler.
class LuaRuntime::Budget final {
public:
    Budget(LuaRuntime& runtime, std::chrono::milliseconds allowance) noexcept
        : m_runtime{runtime}
        , m_wasActive{runtime.m_deadlineActive}
        , m_previousDeadline{runtime.m_deadline}
        , m_previousAllowance{runtime.m_allowance}
    {
        const auto deadline = std::chrono::steady_clock::now() + allowance;

        // Inside another call - which nothing does today, and a binding added
        // tomorrow might - the earlier deadline still stands. A nested call must
        // not be a way to ask for more time.
        if (!m_wasActive || deadline < m_previousDeadline) {
            runtime.m_deadline = deadline;
            runtime.m_allowance = allowance;
        }

        runtime.m_deadlineActive = true;
        runtime.m_timedOut = false;

        // A refusal the script caught itself - pcall around an allocation that did not fit - is
        // over with the call it happened in. Left set, the next failure of any kind would be
        // reported as running out of memory, and stop the ECU for it.
        if (runtime.m_allocator != nullptr) {
            runtime.m_allocator->refused = false;
        }
    }

    ~Budget()
    {
        m_runtime.m_deadlineActive = m_wasActive;
        m_runtime.m_deadline = m_previousDeadline;
        m_runtime.m_allowance = m_previousAllowance;
    }

    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;
    Budget(Budget&&) = delete;
    Budget& operator=(Budget&&) = delete;

private:
    LuaRuntime& m_runtime;
    bool m_wasActive;
    std::chrono::steady_clock::time_point m_previousDeadline;
    std::chrono::milliseconds m_previousAllowance;
};

void LuaRuntime::onCount(lua_State* state, lua_Debug* debug)
{
    (void)debug;

    // The runtime rides in the state's extra space, which every coroutine the
    // script creates inherits a copy of - so a loop inside a coroutine is caught
    // by the same hook, with the same deadline.
    LuaRuntime* runtime = nullptr;
    std::memcpy(&runtime, lua_getextraspace(state), sizeof runtime);

    if (runtime == nullptr || !runtime->m_deadlineActive) {
        return;
    }

    if (std::chrono::steady_clock::now() < runtime->m_deadline) {
        return;
    }

    runtime->m_timedOut = true;

    // Raised as an ordinary error, so the pcall around the call hands it back as a
    // Result. Level 0 is the running function, which is the line of the script
    // that was looping - the one thing the message has to say.
    luaL_where(state, 0);
    lua_pushfstring(
        state,
        "the script ran for more than %d ms in a single call without returning. Is there "
        "a loop that never ends? (The limit is per call, not for the whole run.)",
        static_cast<int>(runtime->m_allowance.count()));
    lua_concat(state, 2);
    lua_error(state);
}

LuaRuntime::LuaRuntime()
    : m_allocator{std::make_unique<Allocator>()}
    , m_state{luaL_newstate()}
{
    if (m_state == nullptr) {
        return;
    }

    // Swapped in after creation rather than creating the state with it: the
    // default constructor path also installs the panic and warning handlers, and
    // there is nothing to gain from redoing that here. What the state already
    // holds was allocated by the previous allocator, which is the same heap, so
    // freeing it through ours is correct - it only has to know the size, and Lua
    // always says.
    m_allocator->inner = lua_getallocf(m_state, &m_allocator->innerData);
    m_allocator->used = static_cast<std::size_t>(lua_gc(m_state, LUA_GCCOUNT)) * 1024U
                        + static_cast<std::size_t>(lua_gc(m_state, LUA_GCCOUNTB));
    m_allocator->limit = m_limits.memoryBytes;
    lua_setallocf(m_state, &Allocator::allocate, m_allocator.get());

    LuaRuntime* self = this;
    std::memcpy(lua_getextraspace(m_state), &self, sizeof self);
    lua_sethook(m_state, &LuaRuntime::onCount, LUA_MASKCOUNT, kInstructionsPerCheck);
}

LuaRuntime::~LuaRuntime()
{
    if (m_state != nullptr) {
        lua_close(m_state);
        m_state = nullptr;
    }
}

void LuaRuntime::setLimits(const LuaLimits& limits) noexcept
{
    m_limits = limits;

    if (m_allocator != nullptr) {
        m_allocator->limit = limits.memoryBytes;
    }
}

Result LuaRuntime::failure(ErrorCode code)
{
    // Read before the message is taken: takeError pops the error object, and a
    // limit that was hit is a fact about the interpreter, not about that object.
    const bool memory = m_allocator != nullptr && m_allocator->refused;
    m_limitHit = m_timedOut || memory;

    if (m_allocator != nullptr) {
        m_allocator->refused = false;
    }

    m_timedOut = false;

    std::string text = takeError(m_state);

    if (memory) {
        text +=
            std::format(" (the script was held to {} MiB)", m_limits.memoryBytes / (1024U * 1024U));
    }

    return Result::error(code, std::move(text));
}

Result LuaRuntime::openLibraries()
{
    if (m_state == nullptr) {
        return Result::error(ErrorCode::Unknown, "Lua interpreter could not be created");
    }

    for (const luaL_Reg& library : kLibraries) {
        luaL_requiref(m_state, library.name, library.func, 1);
        lua_pop(m_state, 1); // requiref leaves the module on the stack
    }

    return Result::ok();
}

Result LuaRuntime::load(std::string_view source, std::string_view chunkName)
{
    if (m_state == nullptr) {
        return Result::error(ErrorCode::Unknown, "Lua interpreter could not be created");
    }

    // The '=' prefix tells Lua to use the name verbatim in error messages
    // rather than quoting it as a source string - the difference between
    // `[string "engine.lua"]:42:` and `engine.lua:42:`.
    const std::string name = std::format("={}", chunkName);

    m_limitHit = false;

    if (luaL_loadbuffer(m_state, source.data(), source.size(), name.c_str()) != LUA_OK) {
        return failure(ErrorCode::ParseError);
    }

    // Running the chunk is what defines its functions and its state. A script
    // that fails here has a problem at the top level - a syntax error is caught
    // above, so this is something like indexing a nil global while setting up.
    //
    // Timed like a call, with the longer allowance set-up work gets: this runs at
    // Start, on the thread that pressed it, and a top level that never ends would
    // freeze the window itself rather than only the pipeline.
    const Budget budget{*this, m_limits.loadBudget};

    if (lua_pcall(m_state, 0, 0, 0) != LUA_OK) {
        return failure(ErrorCode::InvalidState);
    }

    return Result::ok();
}

Result LuaRuntime::checkSyntax(std::string_view source, std::string_view chunkName)
{
    if (m_state == nullptr) {
        return Result::error(ErrorCode::Unknown, "Lua interpreter could not be created");
    }

    const std::string name = std::format("={}", chunkName);

    if (luaL_loadbuffer(m_state, source.data(), source.size(), name.c_str()) != LUA_OK) {
        return Result::error(ErrorCode::ParseError, takeError(m_state));
    }

    lua_pop(m_state, 1);
    return Result::ok();
}

Result LuaRuntime::loadFile(const std::string& path)
{
    std::ifstream file{pathFromUtf8(path), std::ios::binary};
    if (!file) {
        return Result::error(ErrorCode::FileNotFound, std::format("Cannot open script '{}'", path));
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    // The chunk name is the file name alone: a full Windows path in every error
    // message pushes the part that matters off the end of the line.
    const std::size_t separator = path.find_last_of("/\\");
    const std::string name = separator == std::string::npos ? path : path.substr(separator + 1);

    return load(contents.str(), name);
}

bool LuaRuntime::hasFunction(std::string_view name) const
{
    if (m_state == nullptr) {
        return false;
    }

    const std::string zeroTerminated{name};
    lua_getglobal(m_state, zeroTerminated.c_str());
    const bool found = lua_isfunction(m_state, -1);
    lua_pop(m_state, 1);

    return found;
}

Result LuaRuntime::call(std::string_view name)
{
    return call(name, {});
}

Result LuaRuntime::call(std::string_view name, const std::vector<LuaValue>& arguments)
{
    LuaValue ignored;
    return call(name, arguments, ignored, 0);
}

Result
LuaRuntime::call(std::string_view name, const std::vector<LuaValue>& arguments, LuaValue& result)
{
    result = LuaValue{};
    return call(name, arguments, result, 1);
}

Result LuaRuntime::call(std::string_view name,
                        const std::vector<LuaValue>& arguments,
                        LuaValue& result,
                        int results)
{
    if (m_state == nullptr) {
        return Result::error(ErrorCode::Unknown, "Lua interpreter could not be created");
    }

    const std::string zeroTerminated{name};
    lua_getglobal(m_state, zeroTerminated.c_str());

    if (!lua_isfunction(m_state, -1)) {
        lua_pop(m_state, 1);
        return Result::error(ErrorCode::NotImplemented,
                             std::format("Script has no function '{}'", name));
    }

    m_limitHit = false;

    for (const LuaValue& argument : arguments) {
        switch (argument.type) {
        case LuaValue::Type::Nil:
            lua_pushnil(m_state);
            break;
        case LuaValue::Type::Boolean:
            lua_pushboolean(m_state, argument.boolean ? 1 : 0);
            break;
        case LuaValue::Type::Number:
            lua_pushnumber(m_state, argument.number);
            break;
        case LuaValue::Type::Integer:
            lua_pushinteger(m_state, argument.integer);
            break;
        case LuaValue::Type::String:
            lua_pushlstring(m_state, argument.text.data(), argument.text.size());
            break;
        }
    }

    // pcall, never lua_call: an uncaught error in lua_call longjmps out of the
    // interpreter and, from C++, past every destructor between here and the
    // handler. pcall keeps the failure inside Lua and hands it back as a value.
    const Budget budget{*this, m_limits.callBudget};

    if (lua_pcall(m_state, static_cast<int>(arguments.size()), results, 0) != LUA_OK) {
        return failure(ErrorCode::InvalidState);
    }

    if (results == 0) {
        return Result::ok();
    }

    result = readValue(m_state, -1);

    lua_pop(m_state, 1);
    return Result::ok();
}

LuaRuntime::CallableRef LuaRuntime::storeCallable(int stackIndex)
{
    if (m_state == nullptr || !lua_isfunction(m_state, stackIndex)) {
        return 0;
    }

    lua_pushvalue(m_state, stackIndex);

    // luaL_ref pops the value and returns a key into the registry. That
    // reference is what keeps the closure - and everything it captured - from
    // being collected while a timer still points at it.
    return luaL_ref(m_state, LUA_REGISTRYINDEX);
}

Result LuaRuntime::callStored(CallableRef ref)
{
    LuaValue ignored;
    return callStored(ref, ignored, 0);
}

Result LuaRuntime::callStored(CallableRef ref, LuaValue& result)
{
    result = LuaValue{};
    return callStored(ref, result, 1);
}

Result LuaRuntime::callStored(CallableRef ref, LuaValue& result, int results)
{
    if (m_state == nullptr) {
        return Result::error(ErrorCode::Unknown, "Lua interpreter could not be created");
    }

    if (ref == 0) {
        return Result::error(ErrorCode::InvalidArgument, "No function was stored");
    }

    lua_rawgeti(m_state, LUA_REGISTRYINDEX, ref);

    if (!lua_isfunction(m_state, -1)) {
        lua_pop(m_state, 1);
        return Result::error(ErrorCode::InvalidState, "The stored value is no longer a function");
    }

    m_limitHit = false;

    const Budget budget{*this, m_limits.callBudget};

    if (lua_pcall(m_state, 0, results, 0) != LUA_OK) {
        return failure(ErrorCode::InvalidState);
    }

    if (results == 0) {
        return Result::ok();
    }

    result = readValue(m_state, -1);
    lua_pop(m_state, 1);

    return Result::ok();
}

void LuaRuntime::releaseCallable(CallableRef ref)
{
    if (m_state == nullptr || ref == 0) {
        return;
    }

    luaL_unref(m_state, LUA_REGISTRYINDEX, ref);
}

void LuaRuntime::registerFunction(std::string_view name, NativeFunction function, void* userData)
{
    if (m_state == nullptr) {
        return;
    }

    const std::string zeroTerminated{name};

    // The user data rides along as an upvalue rather than in a global or a
    // registry slot, so a binding can find its node without a lookup and
    // without anything a script could reach and overwrite.
    lua_pushlightuserdata(m_state, userData);
    lua_pushcclosure(m_state, function, 1);
    lua_setglobal(m_state, zeroTerminated.c_str());
}

void LuaRuntime::setGlobal(std::string_view name, const LuaValue& value)
{
    if (m_state == nullptr) {
        return;
    }

    switch (value.type) {
    case LuaValue::Type::Nil:
        lua_pushnil(m_state);
        break;
    case LuaValue::Type::Boolean:
        lua_pushboolean(m_state, value.boolean ? 1 : 0);
        break;
    case LuaValue::Type::Number:
        lua_pushnumber(m_state, value.number);
        break;
    case LuaValue::Type::Integer:
        lua_pushinteger(m_state, value.integer);
        break;
    case LuaValue::Type::String:
        lua_pushlstring(m_state, value.text.data(), value.text.size());
        break;
    }

    const std::string zeroTerminated{name};
    lua_setglobal(m_state, zeroTerminated.c_str());
}

void LuaRuntime::setGlobalTable(std::string_view name,
                                const std::map<std::string, LuaValue>& values)
{
    if (m_state == nullptr) {
        return;
    }

    // createtable with the final size rather than newtable: the table is built
    // once, its size is known, and pre-sizing avoids the rehash-on-growth that
    // a table of a dozen settings would otherwise do three times.
    lua_createtable(m_state, 0, static_cast<int>(values.size()));

    for (const auto& [key, value] : values) {
        switch (value.type) {
        case LuaValue::Type::Nil:
            lua_pushnil(m_state);
            break;
        case LuaValue::Type::Boolean:
            lua_pushboolean(m_state, value.boolean ? 1 : 0);
            break;
        case LuaValue::Type::Number:
            lua_pushnumber(m_state, value.number);
            break;
        case LuaValue::Type::Integer:
            lua_pushinteger(m_state, value.integer);
            break;
        case LuaValue::Type::String:
            lua_pushlstring(m_state, value.text.data(), value.text.size());
            break;
        }

        lua_setfield(m_state, -2, key.c_str());
    }

    const std::string zeroTerminated{name};
    lua_setglobal(m_state, zeroTerminated.c_str());
}

std::size_t LuaRuntime::memoryBytes() const
{
    if (m_state == nullptr) {
        return 0;
    }

    const int kibibytes = lua_gc(m_state, LUA_GCCOUNT);
    const int remainder = lua_gc(m_state, LUA_GCCOUNTB);

    return static_cast<std::size_t>(kibibytes) * 1024U + static_cast<std::size_t>(remainder);
}

} // namespace torquebus
