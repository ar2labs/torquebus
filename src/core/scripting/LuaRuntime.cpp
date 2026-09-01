// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The only translation unit in TorqueBus that includes lua.h.

#include "core/scripting/LuaRuntime.h"

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

} // namespace

LuaRuntime::LuaRuntime()
    : m_state{luaL_newstate()}
{
}

LuaRuntime::~LuaRuntime()
{
    if (m_state != nullptr) {
        lua_close(m_state);
        m_state = nullptr;
    }
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

    if (luaL_loadbuffer(m_state, source.data(), source.size(), name.c_str()) != LUA_OK) {
        return Result::error(ErrorCode::ParseError, takeError(m_state));
    }

    // Running the chunk is what defines its functions and its state. A script
    // that fails here has a problem at the top level - a syntax error is caught
    // above, so this is something like indexing a nil global while setting up.
    if (lua_pcall(m_state, 0, 0, 0) != LUA_OK) {
        return Result::error(ErrorCode::InvalidState, takeError(m_state));
    }

    return Result::ok();
}

Result LuaRuntime::loadFile(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("Cannot open script '{}'", path));
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    // The chunk name is the file name alone: a full Windows path in every error
    // message pushes the part that matters off the end of the line.
    const std::size_t separator = path.find_last_of("/\\");
    const std::string name =
        separator == std::string::npos ? path : path.substr(separator + 1);

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

    for (const LuaValue& argument : arguments) {
        switch (argument.type) {
        case LuaValue::Type::Nil:     lua_pushnil(m_state); break;
        case LuaValue::Type::Boolean: lua_pushboolean(m_state, argument.boolean ? 1 : 0); break;
        case LuaValue::Type::Number:  lua_pushnumber(m_state, argument.number); break;
        case LuaValue::Type::Integer: lua_pushinteger(m_state, argument.integer); break;
        case LuaValue::Type::String:
            lua_pushlstring(m_state, argument.text.data(), argument.text.size());
            break;
        }
    }

    // pcall, never lua_call: an uncaught error in lua_call longjmps out of the
    // interpreter and, from C++, past every destructor between here and the
    // handler. pcall keeps the failure inside Lua and hands it back as a value.
    if (lua_pcall(m_state, static_cast<int>(arguments.size()), 0, 0) != LUA_OK) {
        return Result::error(ErrorCode::InvalidState, takeError(m_state));
    }

    return Result::ok();
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
    case LuaValue::Type::Nil:     lua_pushnil(m_state); break;
    case LuaValue::Type::Boolean: lua_pushboolean(m_state, value.boolean ? 1 : 0); break;
    case LuaValue::Type::Number:  lua_pushnumber(m_state, value.number); break;
    case LuaValue::Type::Integer: lua_pushinteger(m_state, value.integer); break;
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
        case LuaValue::Type::Nil:     lua_pushnil(m_state); break;
        case LuaValue::Type::Boolean: lua_pushboolean(m_state, value.boolean ? 1 : 0); break;
        case LuaValue::Type::Number:  lua_pushnumber(m_state, value.number); break;
        case LuaValue::Type::Integer: lua_pushinteger(m_state, value.integer); break;
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
