#pragma once

// Drawing API and its overlay renderer. Objects live outside the VM so a
// render frame never touches Luau state; the Lua side only owns ids.

#include <lua.h>

struct ImDrawList;

namespace zend::rbx::drawing {

// `register_global` is env's set_function, so Drawing callbacks are tracked as
// executor-owned closures (checkcaller and hook bookkeeping).
using RegisterFn = void (*)(lua_State*, const char*, lua_CFunction);

// Registers `Drawing` plus the helper globals on the sandboxed globals table.
void install(lua_State* L, RegisterFn register_global);

// C functions used by Drawing objects and their metatable. env registers them
// as executor-owned so isexecutorclosure, checkcaller and hook bookkeeping
// agree that closures created here belong to the executor.
const lua_CFunction* own_functions(int* count);

// Drops every object. Called when the VM dies; makes no Lua calls.
void reset();

// Called before the ImGui context is replaced (device change). Blocks new font
// reads from Lua until the next render frame rebuilds the atlas.
void reset_renderer();

// Appends all visible objects to the frame's background draw list. Runs on the
// present thread inside an active ImGui frame.
void render_all(ImDrawList* draw_list, float scale);

} // namespace zend::rbx::drawing
