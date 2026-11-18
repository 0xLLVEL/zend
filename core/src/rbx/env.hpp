#pragma once

#include <string>

struct lua_State;

namespace zend::rbx::env {

// Compiles Luau source to the client's wire bytecode format. Shared by the
// executor and by `loadstring`, so both produce identical opcode encoding.
std::string compile(const std::string& source);

// Installs the environment globals on the executor thread. Call after the
// thread is sandboxed and `print` / `__zend_finish` are shadowed.
void install(lua_State* L);

// Drops caches that point into a VM that may be gone. Makes no Lua calls, so
// it is safe after a DataModel change.
void reset();

} // namespace zend::rbx::env
