#include "rbx/env.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <lapi.h>
#include <ldo.h>
#include <lfunc.h>
#include <lgc.h>
#include <lmem.h>
#include <lnumutils.h>
#include <lobject.h>
#include <lstate.h>
#include <lstring.h>
#include <ltable.h>
#include <lua.h>
#include <lualib.h>

#include <Luau/BytecodeBuilder.h>
#include <Luau/BytecodeUtils.h>
#include <Luau/Common.h>
#include <Luau/Compiler.h>
#include <Luau/Bytecode.h>
#include <zstd/zstd.h>

#include "rbx/decompiler.hpp"
#include "rbx/drawing.hpp"
#include "rbx/executor.hpp"
#include "rbx/memory.hpp"
#include "rbx/offsets.hpp"
#include "rbx/sha3.hpp"
#include "util/log.hpp"

// Environment v1 core subset. Everything here runs inside the client VM on the
// executor thread (or a coroutine the client scheduler made from it), so it may
// use the VM's internals directly. The reference implementations live in
// sample/RBX/environment/libs.

namespace zend::rbx::env {

// --- diagnostics ----------------------------------------------------------
//
// Correctness work on the environment APIs needs to see the exact inputs the
// client hands us (identity masks, signal nodes, filter options). Tracing is
// off by default; a probe turns it on with __zend_env_trace(true) so normal
// sessions keep core.log quiet.

std::atomic<bool> g_env_trace{false};

bool env_trace() noexcept {
    return g_env_trace.load(std::memory_order_relaxed);
}

int set_env_trace(lua_State* L) {
    g_env_trace.store(lua_toboolean(L, 1) != 0, std::memory_order_relaxed);
    return 0;
}

// Defined at the bottom of this file. clone_proto needs it to build the inert
// proto bytecode through the compiler instead of hand-assembling opcodes.
std::string compile(const std::string& source);

namespace {

// The client rewrites opcode bytes with its per-build lookup table, so the
// bytecode we hand to luau_load has to carry the same encoding.
class WireEncoder final : public Luau::BytecodeEncoder {
public:
    void encode(std::uint32_t* data, std::size_t count) override {
        const auto* table = reinterpret_cast<const std::uint8_t*>(
            off::host_base() + off::kOpcodeLookupTable);
        for (std::size_t i = 0; i < count;) {
            const auto opcode = LUAU_INSN_OP(data[i]);
            const auto wire = static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(opcode) * off::kOpcodeWireMultiplier);
            data[i] = static_cast<std::uint32_t>(table[wire]) | (data[i] & ~0xFFu);
            i += Luau::getOpLength(static_cast<LuauOpcode>(opcode));
        }
    }
};

// Push a raw garbage-collected object. The stack must have room (callers use
// lua_rawcheckstack) and the thread barrier keeps a black thread from pointing
// at a white object while the GC threshold is parked at SIZE_MAX.
void push_gcobject(lua_State* L, GCObject* gco) {
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    TValue* top = L->top;
    top->value.gc = gco;
    top->tt = gco->gch.tt;
    L->top = top + 1;
}

// The executor's own global table. Chunks execute with the client's sandbox
// proxy as their closure environment; that proxy indexes this table, which in
// turn indexes the client globals. getgenv() hands out this table, so writes
// through getfenv() stay local to the proxy and cannot pollute getgenv().
int g_genv_ref = LUA_NOREF;
global_State* g_genv_global = nullptr;

// Every C closure the executor installs, plus Lua closures it compiles. Kept
// here because getscriptclosure and isexecutorclosure both consult it.
std::unordered_set<Closure*> g_own_closures;

// The real client global table, returned directly by getrenv. The sandbox
// proxy's __index chain anchors it, but an explicit ref keeps it alive (and
// lets us detect a VM change) without depending on the proxy layout.
int g_client_globals_ref = LUA_NOREF;
global_State* g_client_globals_global = nullptr;

// Executor HTTP methods for the DataModel. They are routed through the shared
// instance metatable's own closures so `game` keeps the same metatable as the
// rest of the Instances; replacing that metatable would hide the real
// __newindex/__index slots from getrawmetatable.
int g_http_get_ref = LUA_NOREF;
int g_http_post_ref = LUA_NOREF;
int g_game_ref = LUA_NOREF;

// The client's original require. The engine only lets game-identity threads
// load ModuleScripts created after startup, so the wrapper below lowers the
// identity for the duration of the call (with the executor's full capability
// mask) and restores it afterwards, exactly like the reference environment.
int g_require_ref = LUA_NOREF;

// ModuleScript userdata -> require state. The client's loader lists a module
// as loaded from the moment require starts, even when the body later fails;
// runtime-created modules need this bookkeeping because their Source is never
// compiled into engine bytecode.
int g_module_cache_ref = LUA_NOREF;
lua_CFunction g_instance_index = nullptr;
lua_CFunction g_instance_namecall = nullptr;

// --- namecall frames ------------------------------------------------------
//
// The VM sets L->namecall for a namecall and never clears it, so a plain
// getnamecallmethod() would keep returning the last method long after the
// call returned. Each __namecall invocation we own records its call depth and
// atom; a frame is active only while its call frame is still on the stack.
// getnamecallmethod() answers only from an active frame.

struct NamecallFrame {
    int depth;
    Closure* target;
    TString* atom;
    int atom_ref;
    int thread_ref;
};

std::unordered_map<lua_State*, std::vector<NamecallFrame>> g_namecall_frames;

void release_namecall_frame(lua_State* L, NamecallFrame& frame) {
    if (frame.atom_ref != LUA_NOREF) {
        lua_unref(L, frame.atom_ref);
        frame.atom_ref = LUA_NOREF;
    }
    if (frame.thread_ref != LUA_NOREF) {
        lua_unref(L, frame.thread_ref);
        frame.thread_ref = LUA_NOREF;
    }
}

void begin_namecall_frame(lua_State* L, Closure* target) {
    auto& frames = g_namecall_frames[L];
    const int depth = static_cast<int>(L->ci - L->base_ci);
    while (!frames.empty() && frames.back().depth >= depth) {
        release_namecall_frame(L, frames.back());
        frames.pop_back();
    }
    if (frames.size() >= 128) {
        luaL_error(L, "namecall hook nesting limit exceeded");
    }

    NamecallFrame frame{depth, target, L->namecall, LUA_NOREF, LUA_NOREF};

    // Pins the thread so a suspended namecall keeps its frames keyed to a live
    // lua_State instead of an address that a new thread could reuse.
    lua_pushthread(L);
    frame.thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    if (frame.atom) {
        lua_pushlstring(L, frame.atom->data, frame.atom->len);
        frame.atom_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    frames.push_back(frame);
}

bool restore_namecall_atom(lua_State* L) {
    auto found = g_namecall_frames.find(L);
    if (found == g_namecall_frames.end()) {
        return false;
    }

    auto& frames = found->second;
    const int depth = static_cast<int>(L->ci - L->base_ci);
    for (auto it = frames.begin(); it != frames.end();) {
        const bool active =
            it->depth > 0 && it->depth <= depth &&
            ttisfunction((L->base_ci + it->depth)->func) &&
            clvalue((L->base_ci + it->depth)->func) == it->target;
        if (!active) {
            release_namecall_frame(L, *it);
            it = frames.erase(it);
        } else {
            ++it;
        }
    }

    if (frames.empty()) {
        g_namecall_frames.erase(found);
        return false;
    }
    L->namecall = frames.back().atom;
    return true;
}

void end_namecall_frame(lua_State* L) {
    auto found = g_namecall_frames.find(L);
    if (found == g_namecall_frames.end()) {
        return;
    }

    auto& frames = found->second;
    const int depth = static_cast<int>(L->ci - L->base_ci);
    if (frames.empty() || frames.back().depth != depth ||
        frames.back().target != curr_func(L)) {
        return;
    }

    L->namecall = frames.back().atom;
    release_namecall_frame(L, frames.back());
    frames.pop_back();
    restore_namecall_atom(L);
}

int identifyexecutor(lua_State* L) {
    lua_pushstring(L, "ZEND");
    lua_pushstring(L, "0.1.0");
    return 2;
}

// Diagnostic: canonical ground-truth opcode map (wire byte -> canonical opcode)
// derived from the client's own wire table. Offline tools can decode this
// build's bytecode with it instead of guessing the permutation.
int dump_opcode_map(lua_State* L) {
    const auto* table = reinterpret_cast<const std::uint8_t*>(
        off::host_base() + off::kOpcodeLookupTable);
    std::uint8_t wire_table[256]{};
    if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(table), wire_table,
                        sizeof(wire_table))) {
        lua_pushnil(L);
        return 1;
    }

    std::uint8_t map[256]{};
    for (int opcode = 0; opcode < 256; ++opcode) {
        const auto wire = static_cast<std::uint8_t>(
            opcode * off::kOpcodeWireMultiplier);
        map[wire_table[wire]] = static_cast<std::uint8_t>(opcode);
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(map), sizeof(map));
    return 1;
}

int getnamecallmethod(lua_State* L) {
    if (!restore_namecall_atom(L)) {
        lua_pushnil(L);
        return 1;
    }

    const char* method = lua_namecallatom(L, nullptr);
    if (method) {
        lua_pushstring(L, method);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int count_table_keys(lua_State* L, int index) {
    index = lua_absindex(L, index);
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        ++count;
        lua_pop(L, 1);
    }
    return count;
}

// Pushes the client's own `_G`/`shared` table when the main globals carry it.
// Roblox scripts resolve these through the client globals, so the registry
// slots are only a fallback for VMs that do not publish them there.
bool push_client_global_table(lua_State* L, const char* name,
                              int registry_slot) {
    lua_State* main = L->global->mainthread;
    if (main && main->gt && main != L) {
        luaC_threadbarrier(L);
        luaC_threadbarrier(main);
        lua_pushvalue(main, LUA_GLOBALSINDEX);
        lua_xmove(main, L, 1);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        if (lua_istable(L, -1)) {
            return true;
        }
        lua_pop(L, 1);
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, registry_slot);
    if (lua_istable(L, -1)) {
        return true;
    }
    lua_pop(L, 1);
    return false;
}

// `_G` and `shared` are VM-wide tables in Roblox: game scripts and executor
// chunks read the same object. Point the executor table at the client's table
// (merging anything written before it was visible) so getgenv(), getrenv() and
// the bare global all observe one table.
void publish_shared_global(lua_State* L, int env, const char* name,
                           int registry_slot) {
    env = lua_absindex(L, env);
    lua_rawcheckstack(L, 8);

    lua_pushstring(L, name);
    lua_rawget(L, env);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        if (!push_client_global_table(L, name, registry_slot)) {
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, LUA_REGISTRYINDEX, registry_slot);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, env, name);
    }

    const int shared = lua_gettop(L);

    if (g_genv_global == L->global && g_genv_ref != LUA_NOREF) {
        lua_getref(L, g_genv_ref);
        if (lua_istable(L, -1)) {
            const int genv = lua_gettop(L);
            lua_pushstring(L, name);
            lua_rawget(L, genv);
            if (lua_istable(L, -1) && !lua_rawequal(L, -1, shared)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, shared);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);

            lua_pushvalue(L, shared);
            lua_setfield(L, genv, name);
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
}

int getgenv(lua_State* L) {
    if (g_genv_global == L->global && g_genv_ref != LUA_NOREF) {
        lua_getref(L, g_genv_ref);
        if (env_trace()) {
            zend::logf("env.getgenv: keys=%d", count_table_keys(L, -1));
        }
        return 1;
    }

    // Before install() built the shared table (or for a foreign VM), fall back
    // to the caller's environment.
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    return 1;
}

int getrenv(lua_State* L) {
    lua_State* main = L->global->mainthread;
    if (!main || !main->gt) {
        lua_pushnil(L);
        return 1;
    }

    lua_rawcheckstack(L, 4);
    luaC_threadbarrier(L);
    luaC_threadbarrier(main);

    // getrenv() hands out the client's actual global table, not a copy:
    // changes made through it must affect the executor environment too.
    if (g_client_globals_global != L->global ||
        g_client_globals_ref == LUA_NOREF) {
        // The VM changed (or install has not cached it yet): fall back to the
        // main thread's globals, which is the same table.
        if (main == L) {
            lua_pushvalue(L, LUA_GLOBALSINDEX);
        } else {
            lua_pushvalue(main, LUA_GLOBALSINDEX);
            lua_xmove(main, L, 1);
        }
    } else {
        lua_getref(L, g_client_globals_ref);
    }

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }

    const int env = lua_gettop(L);

    // Roblox scripts read `_G` and `shared` through the same VM-wide tables.
    // Publish them into the real globals and point the executor at them, so a
    // write through getrenv() is visible to the executor and vice versa.
    publish_shared_global(L, env, "_G", 2);
    publish_shared_global(L, env, "shared", 4);

    if (env_trace()) {
        int g_keys = 0;
        int shared_keys = 0;
        int genv_g_keys = -1;
        bool genv_same = false;
        char g_names[192] = {};
        int written = 0;
        lua_pushstring(L, "_G");
        lua_rawget(L, env);
        const int renv_g = lua_gettop(L);
        if (lua_istable(L, renv_g)) {
            g_keys = count_table_keys(L, renv_g);
            lua_pushnil(L);
            while (lua_next(L, renv_g) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && written >= 0 &&
                    written + 64 < static_cast<int>(sizeof(g_names))) {
                    const char* key = lua_tostring(L, -2);
                    if (key) {
                        written += std::snprintf(g_names + written,
                                                 sizeof(g_names) - written,
                                                 "%s,", key);
                    }
                }
                lua_pop(L, 1);
            }
        }
        if (g_genv_global == L->global && g_genv_ref != LUA_NOREF) {
            lua_getref(L, g_genv_ref);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "_G");
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    genv_g_keys = count_table_keys(L, -1);
                    genv_same = lua_rawequal(L, -1, renv_g) != 0;
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_pushstring(L, "shared");
        lua_rawget(L, env);
        if (lua_istable(L, -1)) {
            shared_keys = count_table_keys(L, -1);
        }
        lua_pop(L, 1);
        zend::logf("env.getrenv: env_keys=%d _G=%d shared=%d genv_G=%d "
                   "genv_same=%d names=%s",
                   count_table_keys(L, env), g_keys, shared_keys, genv_g_keys,
                   genv_same ? 1 : 0, g_names);
    }

    return 1;
}

int getreg(lua_State* L) {
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    return 1;
}

struct GcScan {
    lua_State* L;
    int result;
    int count;
    bool include_tables;
};

bool gc_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<GcScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco)) {
        return false; // never "delete": that would corrupt the page walk
    }

    const int type = gco->gch.tt;
    if (type != LUA_TFUNCTION && type != LUA_TTHREAD &&
        type != LUA_TUSERDATA && type != LUA_TBUFFER &&
        !(scan->include_tables && type == LUA_TTABLE)) {
        return false;
    }

    push_gcobject(L, gco);
    lua_rawseti(L, scan->result, ++scan->count);
    return false;
}

int getgc(lua_State* L) {
    const bool include_tables = luaL_optboolean(L, 1, 0) != 0;

    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "kvs");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);

    GcScan scan{L, lua_gettop(L), 0, include_tables};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    // Keep the collector from running while objects are only reachable from
    // this stack frame and the table being filled.
    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, gc_visitor);
    L->global->GCthreshold = threshold;

    return 1;
}

struct InstanceScan {
    lua_State* L;
    int result;
    int game; // 0 when the global is not available
    bool include_all;
    int count;
};

bool instance_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<InstanceScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || gco->gch.tt != LUA_TUSERDATA) {
        return false;
    }

    push_gcobject(L, gco);
    if (std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
        lua_pop(L, 1);
        return false;
    }

    bool take = scan->include_all;
    if (!take) {
        lua_getfield(L, -1, "Parent");
        const bool has_parent = !lua_isnil(L, -1);
        lua_pop(L, 1);

        // game is the DataModel root; its Parent is nil but it is still part of
        // the tree, so getnilinstances excludes it.
        const bool is_game =
            scan->game != 0 && lua_rawequal(L, -1, scan->game) != 0;
        take = !has_parent && !is_game;
    }

    if (take) {
        lua_rawseti(L, scan->result, ++scan->count);
    } else {
        lua_pop(L, 1);
    }
    return false;
}

int collect_instances(lua_State* L, bool include_all) {
    lua_getglobal(L, "game");
    const bool has_game = lua_isuserdata(L, -1) != 0;
    const int game = has_game ? lua_gettop(L) : 0;
    if (!has_game) {
        lua_pop(L, 1);
    }

    lua_newtable(L);
    InstanceScan scan{L, lua_gettop(L), game, include_all, 0};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, instance_visitor);
    L->global->GCthreshold = threshold;

    if (has_game) {
        lua_remove(L, game);
    }
    return 1;
}

int getinstances(lua_State* L) {
    return collect_instances(L, true);
}

int getnilinstances(lua_State* L) {
    return collect_instances(L, false);
}

struct ScriptScan {
    lua_State* L;
    int result;
    int count;
};

bool is_client_script(lua_State* L) {
    const int instance = lua_gettop(L);
    lua_getfield(L, instance, "ClassName");
    const char* class_name = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;

    bool match = false;
    if (class_name &&
        (std::strcmp(class_name, "LocalScript") == 0 ||
         std::strcmp(class_name, "ModuleScript") == 0 ||
         std::strcmp(class_name, "CoreScript") == 0)) {
        match = true;
    } else if (class_name && std::strcmp(class_name, "Script") == 0) {
        // Server scripts only matter when their RunContext is Client.
        lua_getfield(L, instance, "RunContext");
        if (lua_isstring(L, -1)) {
            match = std::strcmp(lua_tostring(L, -1), "Client") == 0;
        } else if (lua_isuserdata(L, -1)) {
            lua_getfield(L, -1, "Name");
            const char* run_context =
                lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
            match = run_context && std::strcmp(run_context, "Client") == 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return match;
}

bool script_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<ScriptScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || gco->gch.tt != LUA_TUSERDATA) {
        return false;
    }

    push_gcobject(L, gco);
    if (std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
        lua_pop(L, 1);
        return false;
    }

    if (is_client_script(L)) {
        lua_rawseti(L, scan->result, ++scan->count);
    } else {
        lua_pop(L, 1);
    }
    return false;
}

int getscripts(lua_State* L) {
    lua_newtable(L);
    ScriptScan scan{L, lua_gettop(L), 0};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, script_visitor);
    L->global->GCthreshold = threshold;

    return 1;
}

int getcallingscript(lua_State* L) {
    lua_rawcheckstack(L, 2);

    for (CallInfo* frame = L->ci; frame >= L->base_ci; --frame) {
        if (!ttisfunction(frame->func)) {
            continue;
        }

        Closure* function = clvalue(frame->func);
        if (!function->env) {
            continue;
        }

        luaC_threadbarrier(L);
        sethvalue(L, L->top, function->env);
        incr_top(L);
        lua_getfield(L, -1, "script");
        lua_remove(L, -2);

        if (!lua_isnil(L, -1)) {
            return 1;
        }
        lua_pop(L, 1);
    }

    lua_pushnil(L);
    return 1;
}

// --- identity ------------------------------------------------------------

// The client resolves capability bits from the identity through this function.
// The reference always asks the engine, including identity 8: the executor
// identity is defined by the client's mask, not by all-ones, and a wrong mask
// is observable through the gated APIs.
std::uint64_t capabilities_for_identity(int identity) {
    using GetCapabilitiesFn = std::int64_t(__fastcall*)(int*);
    const auto fn = reinterpret_cast<GetCapabilitiesFn>(
        off::host_base() + off::kGetCapabilities);
    return static_cast<std::uint64_t>(fn(&identity));
}

// GetTlsPointer takes the identity slot and an `ensure` flag; without the
// second argument the engine may hand back a null entry for the current thread
// and every capability write below lands on nothing.
using GetTlsPointerFn = std::uintptr_t(__fastcall*)(std::uintptr_t,
                                                    std::uint32_t);

__declspec(noinline) std::uintptr_t resolve_identity_struct(
    std::uintptr_t tls) noexcept {
    __try {
        const auto fn = reinterpret_cast<GetTlsPointerFn>(
            off::host_base() + off::kGetTlsPointer);
        return fn(tls, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The engine checks capabilities on the TLS-backed identity structure, not on
// RobloxExtraSpace. Updating only the Lua thread leaves calls like
// game:GetService("CoreGui") gated by the old identity.
void set_engine_identity(int identity, std::uint64_t capabilities) {
    std::uintptr_t tls = 0;
    if (!read_ptr(off::host_base() + off::kIdentityPointer, tls) || !tls) {
        return;
    }

    const std::uintptr_t identity_struct = resolve_identity_struct(tls);
    if (!identity_struct) {
        zend::logf("env.identity: no struct for tls=%p id=%d", (void*)tls,
                   identity);
        return;
    }

    write<std::int32_t>(identity_struct, identity);
    write<std::uint64_t>(identity_struct + off::kIdentityCapabilities,
                         capabilities);
    zend::logf("env.identity: struct=%p tls=%p id=%d caps=0x%llx",
               (void*)identity_struct, (void*)tls, identity,
               (unsigned long long)capabilities);
}

int getthreadidentity(lua_State* L) {
    if (L->userdata) {
        lua_pushinteger(L, static_cast<int>(L->userdata->identity));
        return 1;
    }

    // A scheduler coroutine without its own extra space inherits the executor
    // thread's identity, which is the thread our chunks run on behalf of.
    lua_State* owner = executor::thread();
    const int identity =
        owner && owner->userdata ? static_cast<int>(owner->userdata->identity) : 0;
    lua_pushinteger(L, identity);
    return 1;
}

int setthreadidentity(lua_State* L) {
    luaL_checktype(L, 1, LUA_TNUMBER);
    const int identity = static_cast<int>(lua_tointeger(L, 1));
    const std::uint64_t capabilities = capabilities_for_identity(identity);

    // The executor thread keeps its full Lua-side capability mask even after
    // its identity number is lowered, exactly like the reference. The engine's
    // identity gates read the TLS struct below; narrowing
    // RobloxExtraSpace.capabilities here disables instance creation and Players
    // access for every later chunk in the session.
    if (L->userdata) {
        L->userdata->identity = static_cast<std::uint32_t>(identity);
        L->userdata->capabilities = ~std::uint64_t{0};
    }

    set_engine_identity(identity, capabilities);
    return 0;
}

bool read_engine_identity(int& identity, std::uint64_t& capabilities) {
    std::uintptr_t tls = 0;
    if (!read_ptr(off::host_base() + off::kIdentityPointer, tls) || !tls) {
        return false;
    }

    const std::uintptr_t identity_struct = resolve_identity_struct(tls);
    if (!identity_struct) {
        return false;
    }

    std::int32_t value = 0;
    std::uint64_t caps = 0;
    if (!read(identity_struct, value) ||
        !read(identity_struct + off::kIdentityCapabilities, caps)) {
        return false;
    }

    identity = static_cast<int>(value);
    capabilities = caps;
    return true;
}

std::string instance_class_name(lua_State* L, int index);
std::uintptr_t instance_raw_pointer(lua_State* L, int index);
bool read_script_source(std::uintptr_t script, const std::string& class_name,
                        std::string& out);
bool read_bytecode_at(std::uintptr_t field, std::string& out,
                      std::string* out_encoded);

// True for an Instance userdata whose ClassName is ModuleScript. Reading the
// class through the instance metatable keeps proxy/actor userdata out.
bool is_module_instance(lua_State* L, int index, std::uintptr_t* raw_out) {
    index = lua_absindex(L, index);
    if (!lua_isuserdata(L, index) ||
        std::strcmp(luaL_typename(L, index), "Instance") != 0) {
        return false;
    }
    if (instance_class_name(L, index) != "ModuleScript") {
        return false;
    }
    const std::uintptr_t raw = instance_raw_pointer(L, index);
    if (!raw) {
        return false;
    }
    if (raw_out) {
        *raw_out = raw;
    }
    return true;
}

// Status values mirror the engine's per-VM module state: 1 completed,
// 2 failed, 3 running. The entry is keyed by the module userdata, so the
// module stays alive exactly like it would in the engine's loadedModules set.
void module_cache_update(lua_State* L, int module_index, int status,
                         int value_index, const char* message) {
    if (g_module_cache_ref == LUA_NOREF) {
        return;
    }
    module_index = lua_absindex(L, module_index);
    if (value_index) {
        value_index = lua_absindex(L, value_index);
    }
    lua_getref(L, g_module_cache_ref);
    const int cache = lua_gettop(L);
    lua_pushvalue(L, module_index);
    lua_rawget(L, cache);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, module_index);
        lua_pushvalue(L, -2);
        lua_rawset(L, cache);
    }
    lua_pushinteger(L, status);
    lua_setfield(L, -2, "status");
    if (value_index) {
        lua_pushvalue(L, value_index);
        lua_setfield(L, -2, "value");
    }
    if (message) {
        lua_pushstring(L, message);
        lua_setfield(L, -2, "message");
    }
    lua_pop(L, 2);
}

// Returns true when the module already has a cache entry. For a completed
// entry the single return value is left on the stack.
bool module_cache_status(lua_State* L, int module_index, int* status,
                         bool push_value) {
    *status = 0;
    if (g_module_cache_ref == LUA_NOREF) {
        return false;
    }
    module_index = lua_absindex(L, module_index);
    lua_getref(L, g_module_cache_ref);
    const int cache = lua_gettop(L);
    lua_pushvalue(L, module_index);
    lua_rawget(L, cache);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_getfield(L, -1, "status");
    *status = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (*status == 1 && push_value) {
        lua_getfield(L, -1, "value");
        lua_remove(L, -2); // entry
        lua_remove(L, -2); // cache
        return true;
    }
    lua_pop(L, 2);
    return true;
}

// The engine's ModuleScript loader refuses runtime-created modules when the
// calling thread carries executor identity. Run the original require with game
// identity and the executor's full capability mask, then restore both the Lua
// thread and the TLS identity structure.
//
// Modules created after startup have no engine bytecode: the client only
// compiles Source when it builds the place. Their Source is compiled here and
// run in a private module environment, which is the only way require can load
// them at runtime.
int rbx_require(lua_State* L) {
    luaL_checkany(L, 1);

    if (g_require_ref == LUA_NOREF) {
        luaL_error(L, "require: the original function was not captured");
    }

    std::uintptr_t module_raw = 0;
    const bool runtime_module = is_module_instance(L, 1, &module_raw);

    if (runtime_module) {
        std::uintptr_t record = 0;
        const bool has_record =
            read_ptr(module_raw + off::kModuleScriptModule, record) &&
            is_user_ptr(record);

        std::string engine_bytecode;
        const bool has_bytecode = read_bytecode_at(
            module_raw + off::kModuleScriptBytecode, engine_bytecode, nullptr);

        if (env_trace()) {
            zend::logf(
                "env.require: raw=%p record=%d bytecode=%d", (void*)module_raw,
                has_record ? 1 : 0, has_bytecode ? 1 : 0);
        }

        if (!has_bytecode) {
            int status = 0;
            if (module_cache_status(L, 1, &status, true)) {
                if (status == 1) {
                    return 1;
                }
                if (status == 2) {
                    luaL_error(
                        L,
                        "Requested module experienced an error while loading");
                }
                luaL_error(L, "Requested module was required recursively");
            }

            module_cache_update(L, 1, 3, 0, nullptr);

            std::string source;
            const bool have_source =
                read_script_source(module_raw, "ModuleScript", source);
            if (env_trace()) {
                zend::logf("env.require: source=%d len=%zu head=%.20s",
                           have_source ? 1 : 0, source.size(),
                           source.c_str());
            }
            if (have_source && !source.empty()) {
                std::string bytecode;
                try {
                    bytecode = compile(source);
                } catch (const std::exception& error) {
                    if (env_trace()) {
                        zend::logf("env.require: compile threw: %s",
                                   error.what());
                    }
                }
                if (env_trace()) {
                    zend::logf("env.require: compiled len=%zu first=%u",
                               bytecode.size(),
                               bytecode.empty()
                                   ? 0u
                                   : static_cast<unsigned>(
                                         static_cast<std::uint8_t>(
                                             bytecode[0])));
                }
                if (!bytecode.empty()) {
                    lua_getfield(L, 1, "Name");
                    std::string chunk_name = "=";
                    chunk_name += lua_isstring(L, -1)
                                      ? lua_tostring(L, -1)
                                      : "ModuleScript";
                    lua_pop(L, 1);

                    const int load_status = luau_load(
                        L, chunk_name.c_str(), bytecode.data(), bytecode.size(),
                        0);
                    if (load_status == 0) {
                        const int function = lua_gettop(L);

                        lua_newtable(L);
                        const int environment = lua_gettop(L);
                        lua_newtable(L);
                        if (g_genv_global == L->global &&
                            g_genv_ref != LUA_NOREF) {
                            lua_getref(L, g_genv_ref);
                        } else {
                            lua_pushvalue(L, LUA_GLOBALSINDEX);
                        }
                        lua_setfield(L, -2, "__index");
                        lua_setmetatable(L, environment);
                        lua_pushvalue(L, 1);
                        lua_setfield(L, environment, "script");
                        lua_pushvalue(L, environment);
                        lua_setfenv(L, function);
                        lua_pop(L, 1);

                        const int base = function - 1;
                        lua_pushvalue(L, function);
                        const int run_status = lua_pcall(L, 0, LUA_MULTRET, 0);
                        if (env_trace()) {
                            const char* run_error =
                                run_status != LUA_OK && lua_isstring(L, -1)
                                    ? lua_tostring(L, -1)
                                    : "";
                            zend::logf(
                                "env.require: run status=%d results=%d err=%s",
                                run_status, lua_gettop(L) - function,
                                run_error);
                        }
                        if (run_status == LUA_OK &&
                            lua_gettop(L) - function == 1) {
                            module_cache_update(L, 1, 1, lua_gettop(L),
                                                nullptr);
                            return 1;
                        }
                        lua_settop(L, base);
                        if (run_status == LUA_OK) {
                            module_cache_update(
                                L, 1, 2, 0,
                                "Module code did not return exactly one value");
                            luaL_error(
                                L,
                                "Module code did not return exactly one value");
                        }
                    } else if (env_trace()) {
                        zend::logf(
                            "env.require: load failed status=%d err=%s",
                            load_status, lua_isstring(L, -1)
                                            ? lua_tostring(L, -1)
                                            : "");
                        lua_pop(L, 1);
                    } else {
                        lua_pop(L, 1);
                    }
                }
            }

            module_cache_update(
                L, 1, 2, 0,
                "Requested module experienced an error while loading");
            luaL_error(L, "Requested module experienced an error while loading");
        }

        // The engine can run this module (loaded cache record or shipped
        // bytecode); delegate so scripts keep the normal engine environment.
        module_cache_update(L, 1, 3, 0, nullptr);
    }

    lua_getref(L, g_require_ref);
    if (!lua_isfunction(L, -1)) {
        luaL_error(L, "require: the original function is not callable");
    }

    const int nargs = lua_gettop(L) - 1;
    for (int index = 1; index <= nargs; ++index) {
        lua_pushvalue(L, index);
    }

    const std::uint32_t previous_identity =
        L->userdata ? L->userdata->identity : 8;
    const std::uint64_t previous_thread_caps =
        L->userdata ? L->userdata->capabilities : ~std::uint64_t{0};

    int previous_engine_identity = static_cast<int>(previous_identity);
    std::uint64_t previous_engine_caps =
        capabilities_for_identity(previous_engine_identity);
    read_engine_identity(previous_engine_identity, previous_engine_caps);

    if (L->userdata) {
        L->userdata->identity = 2;
        L->userdata->capabilities = ~std::uint64_t{0};
    }
    set_engine_identity(2, ~std::uint64_t{0});

    const int result_slot = lua_gettop(L) - nargs;
    const int status = lua_pcall(L, nargs, LUA_MULTRET, 0);

    if (L->userdata) {
        L->userdata->identity = previous_identity;
        L->userdata->capabilities = previous_thread_caps;
    }
    set_engine_identity(previous_engine_identity, previous_engine_caps);

    if (status != LUA_OK) {
        if (runtime_module) {
            const char* message =
                lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
            module_cache_update(L, 1, 2, 0, message);
        }
        lua_error(L);
    }

    if (runtime_module) {
        const int results = lua_gettop(L) - result_slot + 1;
        if (results == 1) {
            module_cache_update(L, 1, 1, result_slot, nullptr);
        }
    }

    return lua_gettop(L) - nargs;
}

int getcapabilities(lua_State* L) {
    const int identity = L->userdata
                             ? static_cast<int>(L->userdata->identity)
                             : 8;
    const std::uint64_t capabilities = capabilities_for_identity(identity);

    // Identity 8 is the executor's full mask. The names describe the
    // environment surface this core provides; scripts mostly use this function
    // to check that the result is a table and that their own capability is
    // present.
    static const char* const kNames[] = {
        "run_script", "loadstring", "filesystem", "network", "http",
        "debug",      "metatable",  "closure",    "instance", "signal",
        "console",    "task",       "identity",
    };

    lua_newtable(L);
    int index = 0;
    for (const char* name : kNames) {
        lua_pushstring(L, name);
        lua_rawseti(L, -2, ++index);
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, name);
    }

    lua_pushinteger(L, identity);
    lua_setfield(L, -2, "Identity");
    lua_pushnumber(L, static_cast<double>(capabilities));
    lua_setfield(L, -2, "Capabilities");
    return 1;
}

// --- getloadedmodules ----------------------------------------------------

struct GcTypeScan {
    lua_State* L;
    int result;
    int count;
    int type;
};

bool gc_type_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<GcTypeScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || static_cast<int>(gco->gch.tt) != scan->type) {
        return false;
    }
    push_gcobject(L, gco);
    lua_rawseti(L, scan->result, ++scan->count);
    return false;
}

int collect_gc_type(lua_State* L, int type) {
    lua_newtable(L);
    GcTypeScan scan{L, lua_gettop(L), 0, type};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, gc_type_visitor);
    L->global->GCthreshold = threshold;
    return 1;
}

int inspect_module(lua_State* L) {
    lua_getfenv(L, 1);
    if (!lua_istable(L, -1)) {
        lua_pushnil(L);
        return 1;
    }

    lua_getfield(L, -1, "script");
    if (!lua_isuserdata(L, -1) ||
        std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_getfield(L, -1, "ClassName");
    const bool is_module =
        lua_isstring(L, -1) &&
        std::strcmp(lua_tostring(L, -1), "ModuleScript") == 0;
    lua_pop(L, 1);

    if (!is_module) {
        lua_pushnil(L);
    }
    return 1;
}

// Trace-only: dump the pointer-shaped fields of every ModuleScript instance
// that still has a userdata. Loaded and unloaded modules differ in the module
// cache pointer, which is what the engine consults on a require cache hit.
void trace_module_state_fields(lua_State* L) {
    if (!env_trace()) {
        return;
    }
    collect_gc_type(L, LUA_TUSERDATA);
    const int pool = lua_gettop(L);
    const int count = static_cast<int>(lua_objlen(L, pool));
    for (int index = 1; index <= count; ++index) {
        const int base = lua_gettop(L);
        lua_rawgeti(L, pool, index);
        if (std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
            lua_settop(L, base);
            continue;
        }
        lua_getfield(L, -1, "ClassName");
        const bool module = lua_isstring(L, -1) &&
                            std::strcmp(lua_tostring(L, -1), "ModuleScript") == 0;
        lua_pop(L, 1);
        if (!module) {
            lua_settop(L, base);
            continue;
        }
        void** slot = static_cast<void**>(lua_touserdata(L, -1));
        const auto raw = slot && *slot
                             ? reinterpret_cast<std::uintptr_t>(*slot)
                             : 0;
        lua_getfield(L, -1, "Name");
        const char* name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "?";
        zend::logf("env.module_state: '%s' raw=%p", name,
                   reinterpret_cast<void*>(raw));
        lua_pop(L, 1);
        for (std::uintptr_t off = 0x100; off <= 0x1E0; off += 8) {
            std::uintptr_t pointer = 0;
            if (!read_ptr(raw + off, pointer) || !is_user_ptr(pointer)) {
                continue;
            }
            std::uint32_t head = 0;
            std::uintptr_t control = 0;
            read(pointer, head);
            read_ptr(raw + off + 8, control);
            zend::logf("env.module_state:   +0x%03llX -> %p (head=%u next=%p)",
                       static_cast<unsigned long long>(off),
                       reinterpret_cast<void*>(pointer),
                       static_cast<unsigned>(head),
                       reinterpret_cast<void*>(control));
        }
        lua_settop(L, base);
    }
    lua_settop(L, pool - 1);
}

std::string instance_class_name(lua_State* L, int index);
std::uintptr_t instance_raw_pointer(lua_State* L, int index);

// The engine keeps a module record per ModuleScript once require() has run.
// A never-required module has a null (or non-pointer) field here, so this is
// the source of truth even when the module closure has been collected.
bool module_is_loaded(std::uintptr_t script) {
    std::uintptr_t module = 0;
    if (!read_ptr(script + off::kModuleScriptModule, module) ||
        !is_user_ptr(module)) {
        return false;
    }
    std::uint32_t state = 0;
    return read(module, state) && state >= 1 && state <= 8;
}

// Walks the DataModel so loaded modules are found without relying on a cached
// userdata or a surviving closure.
void collect_loaded_modules_from_tree(lua_State* L, int out,
                                      std::unordered_set<const void*>& seen,
                                      std::uintptr_t& kept) {
    lua_getglobal(L, "game");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "GetDescendants");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushvalue(L, -3);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 2);
        return;
    }

    const int table = lua_gettop(L);
    const int count = static_cast<int>(lua_objlen(L, table));
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(L, table, index);
        if (lua_isuserdata(L, -1)) {
            const std::uintptr_t raw = instance_raw_pointer(L, -1);
            if (raw && module_is_loaded(raw) &&
                instance_class_name(L, -1) == "ModuleScript" &&
                seen.insert(reinterpret_cast<const void*>(raw)).second) {
                lua_pushvalue(L, -1);
                lua_rawseti(L, out, static_cast<int>(++kept));
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

// Tree enumeration misses modules that were created and required without ever
// being parented (the documented getloadedmodules example does exactly that).
// The ModuleScript userdata is still alive in the VM, so walk the GC for
// Instance userdata and keep the ones whose module record finished loading.
struct LoadedModuleScan {
    lua_State* L;
    int result;
    int count;
    std::unordered_set<const void*>* seen;
};

bool loaded_module_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<LoadedModuleScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || gco->gch.tt != LUA_TUSERDATA) {
        return false;
    }

    push_gcobject(L, gco);
    if (std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
        lua_pop(L, 1);
        return false;
    }

    const std::uintptr_t pointer = instance_raw_pointer(L, -1);
    if (!pointer || instance_class_name(L, -1) != "ModuleScript" ||
        !module_is_loaded(pointer) ||
        !scan->seen->insert(reinterpret_cast<const void*>(pointer)).second) {
        lua_pop(L, 1);
        return false;
    }

    lua_rawseti(L, scan->result, ++scan->count);
    return false;
}

void collect_loaded_modules_from_userdata(
    lua_State* L, int out, std::unordered_set<const void*>& seen,
    std::uintptr_t& kept) {
    LoadedModuleScan scan{L, out, static_cast<int>(kept), &seen};
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, loaded_module_visitor);
    L->global->GCthreshold = threshold;
    kept = static_cast<std::uintptr_t>(scan.count);
}

// Every module require has started is tracked in the require cache table. A
// failed or still-running module has no engine record and its module closure
// may already be collected, so this set is the authoritative remainder of the
// engine's loadedModules behavior.
void collect_required_modules(lua_State* L, int out,
                              std::unordered_set<const void*>& seen,
                              std::uintptr_t& kept) {
    if (g_module_cache_ref == LUA_NOREF) {
        return;
    }
    lua_getref(L, g_module_cache_ref);
    const int cache = lua_gettop(L);
    lua_rawcheckstack(L, 2);
    lua_pushnil(L);
    while (lua_next(L, cache) != 0) {
        const std::uintptr_t raw = instance_raw_pointer(L, -2);
        if (raw && seen.insert(reinterpret_cast<const void*>(raw)).second) {
            lua_pushvalue(L, -2);
            lua_rawseti(L, out, static_cast<int>(++kept));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

int getloadedmodules(lua_State* L) {
    trace_module_state_fields(L);
    lua_newtable(L);
    const int out = lua_gettop(L);

    collect_gc_type(L, LUA_TFUNCTION);
    const int pool = lua_gettop(L);

    // One inspector closure for the whole scan. Pushing a fresh C closure per
    // inspected function multiplied the GC population on every call.
    lua_pushcfunction(L, inspect_module, "inspect_module");
    const int inspector = lua_gettop(L);

    std::unordered_set<const void*> seen;
    std::uintptr_t kept = 0;
    const int count = static_cast<int>(lua_objlen(L, pool));
    for (int index = 1; index <= count; ++index) {
        const int base = lua_gettop(L);
        lua_pushvalue(L, inspector);
        lua_rawgeti(L, pool, index);
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isuserdata(L, -1)) {
            void** slot = static_cast<void**>(lua_touserdata(L, -1));
            const void* instance = slot ? *slot : nullptr;
            if (instance && seen.insert(instance).second) {
                lua_pushvalue(L, -1);
                lua_rawseti(L, out, static_cast<int>(++kept));
            }
        }
        lua_settop(L, base);
    }

    collect_loaded_modules_from_tree(L, out, seen, kept);
    collect_loaded_modules_from_userdata(L, out, seen, kept);
    collect_required_modules(L, out, seen, kept);

    lua_settop(L, out);
    if (env_trace()) {
        const int modules = static_cast<int>(lua_objlen(L, out));
        zend::logf("env.getloadedmodules: functions=%d modules=%d", count,
                   modules);
        for (int index = 1; index <= modules && index <= 60; ++index) {
            lua_rawgeti(L, out, index);
            lua_getfield(L, -1, "Name");
            const char* name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "?";
            zend::logf("env.getloadedmodules:   [%d] %s", index, name);
            lua_pop(L, 2);
        }
    }
    return 1;
}

// --- script bytecode -----------------------------------------------------
//
// ScriptSource is stored as a protected string whose value begins with an
// 8-byte container header: either the plain marker (raw Luau bytecode follows)
// or "RSB1" with an xor key and, for larger scripts, a zstd frame. The script
// object exposes it behind a pointer at a build-specific offset; the scan is
// the fallback when that offset drifts.

void check_instance(lua_State* L, int index);

std::uintptr_t instance_raw_pointer(lua_State* L, int index);

std::string instance_class_name(lua_State* L, int index) {
    index = lua_absindex(L, index);
    lua_getfield(L, index, "ClassName");
    std::string name;
    if (lua_isstring(L, -1)) {
        name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return name;
}

bool is_script_class(const std::string& name) {
    return name == "LocalScript" || name == "ModuleScript" ||
           name == "Script" || name == "CoreScript";
}

std::uintptr_t script_pointer(lua_State* L, int index) {
    check_instance(L, index);
    void** userdata = static_cast<void**>(lua_touserdata(L, index));
    return userdata && *userdata
               ? reinterpret_cast<std::uintptr_t>(*userdata)
               : 0;
}

struct NativeStdString {
    union {
        char buffer[16];
        char* data;
    };
    std::size_t size;
    std::size_t capacity;
};
static_assert(sizeof(NativeStdString) == 32);

bool read_std_string(std::uintptr_t address, std::string& out) {
    out.clear();
    NativeStdString value{};
    if (!read(address, value)) {
        return false;
    }
    if (value.size <= 8 || value.size > value.capacity ||
        value.capacity > 64u * 1024u * 1024u) {
        return false;
    }
    if (value.capacity < sizeof(value.buffer)) {
        out.assign(value.buffer, value.size);
        return true;
    }
    out.resize(value.size);
    return try_read_bytes(reinterpret_cast<std::uintptr_t>(value.data),
                          out.data(), value.size);
}

// Same native string layout, but for fields that are not wrapped in the
// 8-byte ScriptSource container: short SSO strings are valid here.
bool read_native_string(std::uintptr_t address, std::string& out,
                        std::size_t max_size = 64u * 1024u * 1024u) {
    out.clear();
    NativeStdString value{};
    if (!read(address, value)) {
        return false;
    }
    if (value.size > value.capacity || value.size > max_size ||
        value.capacity > max_size) {
        return false;
    }
    if (value.size == 0) {
        return true;
    }
    if (value.capacity < sizeof(value.buffer)) {
        out.assign(value.buffer, value.size);
        return true;
    }
    if (!is_user_ptr(reinterpret_cast<std::uintptr_t>(value.data))) {
        return false;
    }
    out.resize(value.size);
    return try_read_bytes(reinterpret_cast<std::uintptr_t>(value.data),
                          out.data(), value.size);
}

// `Source` is a protected string pointer at the verified per-class offsets in
// dump/offsets_rbx.hpp. The payload is the same +0x18 std::string used by the
// bytecode container; the inline layout is kept as a fallback for builds that
// store the value directly.
bool read_script_source(std::uintptr_t script, const std::string& class_name,
                        std::string& out) {
    out.clear();
    std::uintptr_t offset = 0;
    if (class_name == "ModuleScript") {
        offset = off::kModuleScriptSource;
    } else if (class_name == "Script" || class_name == "LocalScript" ||
               class_name == "CoreScript") {
        offset = off::kLocalScriptSource;
    }
    if (!script || !offset) {
        return false;
    }

    std::uintptr_t container = 0;
    if (read_ptr(script + offset, container) && is_user_ptr(container) &&
        read_native_string(container + off::kProtectedStringValue, out) &&
        !out.empty()) {
        return true;
    }
    return read_native_string(script + offset, out) && !out.empty();
}

std::string ascii_preview(const std::string& text, std::size_t limit = 28) {
    std::string out;
    out.reserve(limit + 1);
    for (std::size_t i = 0; i < text.size() && i < limit; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        out.push_back(byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
    }
    return out;
}

// Trace-only scan around the declared Source offsets. The dump is per-build,
// so a mismatch shows up as a string-shaped field next to the one we read.
void trace_script_source_layout(std::uintptr_t script, const char* class_name) {
    if (!script) {
        return;
    }
    zend::logf("env.source_layout: class=%s script=%p", class_name,
               reinterpret_cast<void*>(script));
    for (std::uintptr_t offset = 0x100; offset <= 0x1B0; offset += 8) {
        std::uintptr_t pointer = 0;
        const bool has_pointer = read_ptr(script + offset, pointer);
        std::string inline_text;
        const bool inline_ok =
            read_native_string(script + offset, inline_text, 1u << 20) &&
            !inline_text.empty();
        std::string value_text;
        std::string head_text;
        bool value_ok = false;
        bool head_ok = false;
        if (has_pointer && is_user_ptr(pointer)) {
            value_ok = read_native_string(pointer + off::kProtectedStringValue,
                                          value_text, 1u << 20) &&
                       !value_text.empty();
            head_ok = read_native_string(pointer + 0x10, head_text, 1u << 20) &&
                      !head_text.empty();
        }
        if (!has_pointer && !inline_ok && !value_ok && !head_ok) {
            continue;
        }
        zend::logf("env.source_layout: +0x%02llX ptr=%p inline=%d/%zu "
                   "deref18=%d/%zu deref10=%d/%zu i='%s' d='%s'",
                   static_cast<unsigned long long>(offset),
                   reinterpret_cast<void*>(pointer), inline_ok ? 1 : 0,
                   inline_text.size(), value_ok ? 1 : 0, value_text.size(),
                   head_ok ? 1 : 0, head_text.size(),
                   ascii_preview(inline_text).c_str(),
                   ascii_preview(value_ok ? value_text : head_text).c_str());
    }
}

constexpr std::uint64_t kBytecodePlainMarker = 0xE009325B4A107A52ull;
constexpr std::uint32_t kBytecodeFrameMagic = 0xFD2FB528u;
constexpr std::uint8_t kBytecodeVersionCeiling = 31;
constexpr char kBytecodeSignature[4] = {'R', 'S', 'B', '1'};

bool looks_like_bytecode(const std::string& bytecode) {
    if (bytecode.empty()) {
        return false;
    }
    const auto version = static_cast<std::uint8_t>(bytecode[0]);
    return version != 0 && version <= kBytecodeVersionCeiling;
}

bool read_bytecode_varint(const char* data, std::size_t size,
                          std::size_t& offset, std::uint32_t& out) {
    std::uint32_t value = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        if (offset >= size) {
            return false;
        }
        const auto byte = static_cast<std::uint8_t>(data[offset++]);
        value |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = value;
            return true;
        }
    }
    return false;
}

// The client loader understands both the stored container (RSB1 / plain
// marker) and raw Luau bytecode. Source text is never a valid versioned bytecode
// header, so validating the string table separates the two without guessing
// from a single control byte.
bool looks_like_client_bytecode(const char* data, std::size_t size) {
    if (!data || size < 8) {
        return false;
    }
    if (std::memcmp(data, kBytecodeSignature, sizeof(kBytecodeSignature)) == 0) {
        return true;
    }
    std::uint64_t marker = 0;
    std::memcpy(&marker, data, sizeof(marker));
    if (marker == kBytecodePlainMarker) {
        return true;
    }

    const auto version = static_cast<std::uint8_t>(data[0]);
    if (version < 3 || version > 14) {
        return false;
    }

    std::size_t offset = 1;
    std::uint8_t types_version = 0;
    if (version >= 4) {
        types_version = static_cast<std::uint8_t>(data[offset++]);
        if (types_version < LBC_TYPE_VERSION_MIN ||
            types_version > LBC_TYPE_VERSION_MAX) {
            return false;
        }
    }

    std::uint32_t string_count = 0;
    if (!read_bytecode_varint(data, size, offset, string_count) ||
        string_count > 4'000'000u) {
        return false;
    }
    for (std::uint32_t index = 0; index < string_count; ++index) {
        std::uint32_t length = 0;
        if (!read_bytecode_varint(data, size, offset, length) ||
            length > size - offset) {
            return false;
        }
        offset += length;
    }

    if (types_version == 3) {
        for (;;) {
            if (offset >= size) {
                return false;
            }
            if (data[offset++] == 0) {
                break;
            }
            std::uint32_t length = 0;
            if (!read_bytecode_varint(data, size, offset, length) ||
                length > size - offset) {
                return false;
            }
            offset += length;
        }
    }

    std::uint32_t proto_count = 0;
    std::uint32_t first_proto_size = 0;
    return read_bytecode_varint(data, size, offset, proto_count) &&
           proto_count > 0 && proto_count <= 1'000'000u &&
           read_bytecode_varint(data, size, offset, first_proto_size) &&
           first_proto_size > 0 && first_proto_size <= size - offset;
}

bool decode_bytecode(const std::string& source, std::string& out) {
    out.clear();
    if (source.size() <= 8) {
        return false;
    }

    std::vector<std::uint8_t> data(source.begin(), source.end());

    std::uint64_t marker = 0;
    std::memcpy(&marker, data.data(), sizeof(marker));

    if (marker == kBytecodePlainMarker) {
        std::memcpy(data.data(), kBytecodeSignature, sizeof(kBytecodeSignature));
        std::memset(data.data() + sizeof(kBytecodeSignature), 0,
                    sizeof(std::uint32_t));
    } else {
        std::uint8_t key[4]{};
        for (std::size_t i = 0; i < sizeof(key); ++i) {
            const auto masked =
                static_cast<std::uint8_t>(data[i] ^ kBytecodeSignature[i]);
            key[i] = static_cast<std::uint8_t>(masked - i * 41);
        }

        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] ^= static_cast<std::uint8_t>(key[i % sizeof(key)] + i * 41);
        }

        if (std::memcmp(data.data(), kBytecodeSignature,
                        sizeof(kBytecodeSignature)) != 0) {
            return false;
        }
    }

    std::uint32_t decoded_size = 0;
    std::memcpy(&decoded_size, data.data() + sizeof(kBytecodeSignature),
                sizeof(decoded_size));

    if (decoded_size == 0) {
        out.assign(reinterpret_cast<const char*>(data.data()) + 8,
                   data.size() - 8);
        return !out.empty();
    }

    if (decoded_size > 64u * 1024u * 1024u || data.size() - 8 <= 4) {
        return false;
    }

    std::uint32_t frame_magic = 0;
    std::memcpy(&frame_magic, data.data() + 8, sizeof(frame_magic));
    if (frame_magic != kBytecodeFrameMagic) {
        return false;
    }

    out.resize(decoded_size);
    const std::size_t written =
        ZSTD_decompress(out.data(), out.size(), data.data() + 8, data.size() - 8);
    if (ZSTD_isError(written) || written != decoded_size) {
        out.clear();
        return false;
    }
    return true;
}

bool read_bytecode_container(std::uintptr_t address, std::string& out,
                             std::string* out_encoded) {
    std::string encoded;
    if (read_std_string(address + off::kProtectedStringValue, encoded) &&
        decode_bytecode(encoded, out) && looks_like_bytecode(out)) {
        if (out_encoded) {
            *out_encoded = std::move(encoded);
        }
        return true;
    }
    if (read_std_string(address, encoded) && decode_bytecode(encoded, out) &&
        looks_like_bytecode(out)) {
        if (out_encoded) {
            *out_encoded = std::move(encoded);
        }
        return true;
    }
    out.clear();
    return false;
}

bool read_bytecode_at(std::uintptr_t field, std::string& out,
                      std::string* out_encoded) {
    std::uintptr_t container = 0;
    if (read_ptr(field, container) && is_user_ptr(container) &&
        read_bytecode_container(container, out, out_encoded)) {
        return true;
    }
    return read_bytecode_container(field, out, out_encoded);
}

bool fetch_script_bytecode(std::uintptr_t script, bool module_script,
                           std::string& out, std::string* out_encoded = nullptr) {
    static std::mutex guard;
    static std::uintptr_t local_offset = 0;
    static std::uintptr_t module_offset = 0;
    const std::lock_guard<std::mutex> lock(guard);

    const std::uintptr_t primary = module_script ? off::kModuleScriptBytecode
                                                 : off::kLocalScriptBytecode;
    std::uintptr_t& learned = module_script ? module_offset : local_offset;

    if (primary != 0 && read_bytecode_at(script + primary, out, out_encoded)) {
        return true;
    }
    if (learned != 0 && learned != primary &&
        read_bytecode_at(script + learned, out, out_encoded)) {
        return true;
    }

    for (std::uintptr_t offset = 0x80; offset <= 0x400; offset += 8) {
        if (offset == primary || offset == learned) {
            continue;
        }
        if (read_bytecode_at(script + offset, out, out_encoded)) {
            learned = offset;
            return true;
        }
    }
    return false;
}

// The VM loads bytecode carrying its own wire version. The client stores a
// newer version than the vendored Luau supports, so rewrite the version byte
// to the version this compiler emits. The decompiler still reads its own raw
// copy, so Tovek keeps the original encoding.
std::uint8_t target_bytecode_version() {
    static std::uint8_t cached = 0;
    if (cached == 0) {
        try {
            const std::string probe = compile("return 0");
            if (!probe.empty()) {
                cached = static_cast<std::uint8_t>(probe[0]);
            }
        } catch (...) {
        }
        if (cached == 0) {
            cached = static_cast<std::uint8_t>(LBC_VERSION_TARGET);
        }
    }
    return cached;
}

int getscriptbytecode(lua_State* L) {
    check_instance(L, 1);
    const std::string class_name = instance_class_name(L, 1);
    if (!is_script_class(class_name)) {
        luaL_typeerror(L, 1, "LocalScript, ModuleScript or Script");
    }

    const std::uintptr_t script = script_pointer(L, 1);
    std::string bytecode;
    if (!script || !fetch_script_bytecode(script, class_name == "ModuleScript",
                                          bytecode)) {
        lua_pushnil(L);
        return 1;
    }

    const std::uint8_t target_version = target_bytecode_version();
    if (!bytecode.empty() && target_version != 0 &&
        static_cast<std::uint8_t>(bytecode[0]) != target_version) {
        if (env_trace()) {
            zend::logf("env.getscriptbytecode: retarget %u -> %u",
                       static_cast<unsigned>(
                           static_cast<std::uint8_t>(bytecode[0])),
                       static_cast<unsigned>(target_version));
        }
        bytecode[0] = static_cast<char>(target_version);
    }

    if (env_trace()) {
        std::string head;
        const std::size_t limit = bytecode.size() < 16 ? bytecode.size() : 16;
        static const char* const digits = "0123456789abcdef";
        for (std::size_t i = 0; i < limit; ++i) {
            const auto byte = static_cast<unsigned char>(bytecode[i]);
            head.push_back(digits[byte >> 4]);
            head.push_back(digits[byte & 0x0F]);
        }
        lua_getfield(L, 1, "Name");
        std::size_t name_length = 0;
        const char* name_text = lua_tolstring(L, -1, &name_length);
        const std::string script_name =
            name_text ? std::string(name_text, name_length) : std::string();
        lua_pop(L, 1);
        zend::logf("env.getscriptbytecode: class=%s name=%s size=%zu head=%s",
                   class_name.c_str(), script_name.c_str(), bytecode.size(),
                   head.c_str());
    }

    // Return the chunk in this VM's wire version so loadstring() and the
    // suite's bytecode checks see a format the executor can load.
    lua_pushlstring(L, bytecode.data(), bytecode.size());
    return 1;
}

int getscripthash(lua_State* L) {
    check_instance(L, 1);
    const std::string class_name = instance_class_name(L, 1);
    if (!is_script_class(class_name)) {
        luaL_typeerror(L, 1, "LocalScript, ModuleScript or Script");
    }

    const std::uintptr_t script = script_pointer(L, 1);
    std::string raw;
    if (script && fetch_script_bytecode(script, class_name == "ModuleScript",
                                        raw) &&
        !raw.empty()) {
        // The reference hashes the decoded bytecode, not the container.
    } else {
        // Cloned or in-memory scripts may only have a Source property.
        raw.clear();
        lua_getfield(L, 1, "Source");
        if (lua_isstring(L, -1)) {
            std::size_t length = 0;
            const char* source = lua_tolstring(L, -1, &length);
            if (source && length > 0) {
                raw.assign(source, length);
            }
        }
        lua_pop(L, 1);
    }

    if (raw.empty()) {
        lua_pushnil(L);
        return 1;
    }

    const std::string digest = sha3_impl::sha3(384, raw.data(),
                                               static_cast<int>(raw.size()));
    lua_pushlstring(L, digest.data(), digest.size());
    return 1;
}

// --- decompile ------------------------------------------------------------

int decompile_read_source(lua_State* L) {
    lua_getfield(L, 1, "Source");
    return 1;
}

std::int32_t load_script_bytecode(lua_State* thread, const char* data,
                                  std::size_t size,
                                  const char* chunk_name = "getscriptclosure") noexcept;
// The executor has no Lua decompiler. Scripts that still carry their source
// round-trip verbatim; bytecode-only scripts get a deterministic, valid Luau
// stub. Constants from the bytecode are listed as comments so the output keeps
// the literals a decompiler would have recovered.
void append_proto_constants(std::string& out, const Proto* proto, int depth,
                            int& budget) {
    if (!proto || depth > 12 || budget <= 0) {
        return;
    }

    for (int index = 0; index < proto->sizek && budget > 0; ++index) {
        const TValue* value = &proto->k[index];
        out += "\n-- constant: ";

        if (ttisstring(value)) {
            // Constants land in a line comment; escape the bytes that would
            // otherwise end it or truncate the chunk.
            const TString* text = tsvalue(value);
            for (std::size_t byte = 0; byte < text->len; ++byte) {
                const char character = text->data[byte];
                switch (character) {
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\0':
                    out += "\\0";
                    break;
                default:
                    out.push_back(character);
                    break;
                }
            }
        } else if (ttisnumber(value)) {
            // Match `tostring(number)` exactly (shortest round-trip form), so
            // a caller searching the output for each constant finds it.
            char buffer[64] = {};
            luai_num2str(buffer, nvalue(value));
            out += buffer;
        } else if (ttisnil(value)) {
            out += "nil";
        } else if (ttisboolean(value)) {
            out += bvalue(value) ? "true" : "false";
        } else if (ttisvector(value)) {
            const float* vector = vvalue(value);
            char buffer[96] = {};
            std::snprintf(buffer, sizeof(buffer), "vector(%g, %g, %g)",
                          vector[0], vector[1], vector[2]);
            out += buffer;
        } else {
            continue;
        }
        --budget;
    }

    for (int index = 0; index < proto->sizep; ++index) {
        append_proto_constants(out, proto->p ? proto->p[index] : nullptr,
                               depth + 1, budget);
    }
}

int decompile(lua_State* L) {
    check_instance(L, 1);
    const std::string class_name = instance_class_name(L, 1);
    if (!is_script_class(class_name)) {
        luaL_typeerror(L, 1, "LocalScript, ModuleScript or Script");
    }

    const std::uintptr_t script = script_pointer(L, 1);

    // Real source wins over any stub: the client keeps it for scripts that
    // still ship source (ModuleScripts in particular). The verified offset
    // read is the only path that does not go through the property descriptor.
    std::string direct_source;
    if (read_script_source(script, class_name, direct_source)) {
        if (env_trace()) {
            zend::logf("env.decompile: class=%s direct_source=%zu",
                       class_name.c_str(), direct_source.size());
        }
        lua_pushlstring(L, direct_source.data(), direct_source.size());
        return 1;
    }

    if (env_trace()) {
        trace_script_source_layout(script, class_name.c_str());
    }

    const int base = lua_gettop(L);
    lua_pushcfunction(L, decompile_read_source, "decompile");
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1)) {
        std::size_t length = 0;
        const char* source = lua_tolstring(L, -1, &length);
        if (source && length > 0) {
            if (env_trace()) {
                zend::logf("env.decompile: class=%s property_source=%zu",
                           class_name.c_str(), length);
            }
            return 1;
        }
    }
    lua_settop(L, base);

    std::string bytecode;
    std::string encoded;
    const bool has_bytecode =
        script && fetch_script_bytecode(script, class_name == "ModuleScript",
                                        bytecode, &encoded) &&
        !bytecode.empty();

    if (has_bytecode) {
        std::string decompiled;
        std::string error;
        if (decompiler::decompile(bytecode, decompiled, error) &&
            !decompiled.empty()) {
            if (env_trace()) {
                zend::logf("env.decompile: class=%s bytecode=%zu helper=%zu",
                           class_name.c_str(), bytecode.size(),
                           decompiled.size());
            }
            lua_pushlstring(L, decompiled.data(), decompiled.size());
            return 1;
        }
        if (env_trace()) {
            zend::logf("env.decompile: class=%s bytecode=%zu helper failed: %s",
                       class_name.c_str(), bytecode.size(), error.c_str());
        }
    }

    std::string stub = "-- decompiled by ZEND\n-- class: ";
    stub += class_name;

    if (has_bytecode) {
        if (env_trace()) {
            zend::logf("env.decompile: class=%s bytecode=%zu source=none",
                       class_name.c_str(), bytecode.size());
        }
        const std::string digest =
            sha3_impl::sha3(256, bytecode.data(),
                            static_cast<int>(bytecode.size()));
        stub += "\n-- sha256: ";
        stub += digest;

        if (!encoded.empty()) {
            lua_State* thread = lua_newthread(L);
            if (thread) {
                const int thread_ref = lua_ref(L, -1);
                lua_pop(L, 1);
                luaL_sandboxthread(thread);
                if (load_script_bytecode(thread, encoded.data(),
                                         encoded.size()) == LUA_OK &&
                    lua_isLfunction(thread, -1)) {
                    Closure* closure = clvalue(luaA_toobject(thread, -1));
                    if (closure && !closure->isC) {
                        int budget = 4096;
                        append_proto_constants(stub, closure->l.p, 0, budget);
                    }
                }
                lua_settop(thread, 0);
                lua_unref(L, thread_ref);
            }
        }
    }

    stub += "\nreturn nil\n";
    lua_pushlstring(L, stub.data(), stub.size());
    return 1;
}

std::string hash_function_proto(const Proto* proto) {
    std::string buffer;
    if (!proto) {
        return buffer;
    }
    buffer.reserve(static_cast<std::size_t>(proto->sizecode) * 4 + 64);

    // Constants come first so two closures with identical instructions but
    // different literals do not collide.
    const int constants = proto->sizek;
    for (int index = 0; index < constants; ++index) {
        const TValue* value = &proto->k[index];
        buffer.push_back(static_cast<char>(ttype(value)));

        if (ttisnil(value)) {
            continue;
        }
        if (ttisboolean(value)) {
            buffer.push_back(static_cast<char>(bvalue(value) ? 1 : 0));
            continue;
        }
        if (ttisnumber(value)) {
            const double number = nvalue(value);
            buffer.append(reinterpret_cast<const char*>(&number),
                          sizeof(number));
            continue;
        }
        if (ttisstring(value)) {
            const TString* text = tsvalue(value);
            const std::uint32_t length = static_cast<std::uint32_t>(text->len);
            buffer.append(reinterpret_cast<const char*>(&length),
                          sizeof(length));
            buffer.append(text->data, text->len);
            continue;
        }
        if (ttisvector(value)) {
            const float* vector = vvalue(value);
            buffer.append(reinterpret_cast<const char*>(vector),
                          sizeof(float) * 3);
            continue;
        }
    }

    if (proto->code && proto->sizecode > 0) {
        buffer.append(reinterpret_cast<const char*>(proto->code),
                      static_cast<std::size_t>(proto->sizecode) *
                          sizeof(std::uint32_t));
    }

    return sha3_impl::sha3(384, buffer.data(), static_cast<int>(buffer.size()));
}

int getfunctionhash(lua_State* L) {
    if (!lua_isfunction(L, 1)) {
        luaL_typeerror(L, 1, "function");
    }

    Closure* closure = clvalue(luaA_toobject(L, 1));
    if (!closure || closure->isC || !closure->l.p) {
        luaL_argerror(L, 1, "lua function expected");
    }

    const std::string digest = hash_function_proto(closure->l.p);
    lua_pushlstring(L, digest.data(), digest.size());
    return 1;
}

// The client's loader understands the bytecode container directly (the marker,
// the xor pass and the zstd frame), so hand it the encoded string instead of
// re-loading decoded bytecode with the older vendored Luau that cannot read a
// newer bytecode version.
struct BytecodeSpan {
    const char* data;
    std::size_t size;
};
static_assert(sizeof(BytecodeSpan) == 0x10);

using LuaVMLoadFn = std::int32_t(__fastcall*)(lua_State*, const BytecodeSpan*,
                                              const char*, std::int32_t);

__declspec(noinline) std::int32_t load_script_bytecode(lua_State* thread,
                                                       const char* data,
                                                       std::size_t size,
                                                       const char* chunk_name) noexcept {
    __try {
        const BytecodeSpan span{data, size};
        auto load = reinterpret_cast<LuaVMLoadFn>(off::host_base() +
                                                 off::kLuaVMLoad);
        return load(thread, &span,
                    chunk_name && *chunk_name ? chunk_name : "getscriptclosure",
                    0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int getscriptclosure(lua_State* L) {
    check_instance(L, 1);
    const std::string class_name = instance_class_name(L, 1);
    if (!is_script_class(class_name)) {
        luaL_typeerror(L, 1, "LocalScript, ModuleScript or Script");
    }

    const std::uintptr_t script = script_pointer(L, 1);
    std::string bytecode;
    std::string encoded;
    if (!script || !fetch_script_bytecode(script, class_name == "ModuleScript",
                                          bytecode, &encoded) ||
        encoded.empty()) {
        lua_pushnil(L);
        return 1;
    }

    lua_State* thread = lua_newthread(L);
    if (!thread) {
        lua_pushnil(L);
        return 1;
    }
    const int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    luaL_sandboxthread(thread);
    lua_pushvalue(L, 1);
    lua_xmove(L, thread, 1);
    lua_setglobal(thread, "script");

    if (load_script_bytecode(thread, encoded.data(), encoded.size()) != LUA_OK ||
        !lua_isLfunction(thread, -1)) {
        lua_settop(thread, 0);
        lua_unref(L, thread_ref);
        lua_pushnil(L);
        return 1;
    }

    Closure* closure = clvalue(luaA_toobject(thread, -1));
    if (!closure || closure->isC || !closure->l.p) {
        lua_settop(thread, 0);
        lua_unref(L, thread_ref);
        lua_pushnil(L);
        return 1;
    }
    executor::apply_capabilities(closure->l.p);
    g_own_closures.insert(closure);

    lua_xmove(thread, L, 1);
    lua_settop(thread, 0);
    lua_unref(L, thread_ref);
    return 1;
}

// --- running scripts / environments --------------------------------------

// Defined with the instance cache helpers below; declared here because the
// script APIs want the engine's canonical userdata for a raw instance pointer.
// Never fabricate one: PushInstance takes a std::shared_ptr<Instance> and a
// raw pointer argument makes it adopt an arbitrary stack word as the control
// block, which crashes the GC when the userdata is collected.
bool push_cached_instance(lua_State* L, std::uintptr_t raw);

// Pushes through the engine's own bridge. The argument must point at a real
// shared_ptr<Instance> (the caller's weak lock is one); a raw instance pointer
// would make the engine treat an adjacent word as the control block.
bool push_instance_raw(lua_State* L, void* argument) noexcept;

// The engine weak binding is the authoritative "this thread runs this script"
// source. Hold the locked shared_ptr instead of a bare pointer: scripts that
// have no cached userdata (destroyed scripts are the common case) can only be
// surfaced by handing that real control block to the instance bridge later.
struct ScriptPointerScan {
    lua_State* L;
    std::unordered_set<std::uintptr_t>* seen;
    std::vector<std::shared_ptr<uintptr_t>>* scripts;
    int threads = 0;
    int with_userdata = 0;
    int expired = 0;
    int other_vm = 0;
};

bool script_pointer_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<ScriptPointerScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || gco->gch.tt != LUA_TTHREAD) {
        return false;
    }

    auto* thread = reinterpret_cast<lua_State*>(gco);
    ++scan->threads;
    if (!thread->userdata) {
        return false;
    }
    ++scan->with_userdata;
    if (thread->userdata->script.expired()) {
        ++scan->expired;
        return false;
    }
    if (thread->global->mainthread != L->global->mainthread) {
        ++scan->other_vm;
        if (env_trace()) {
            auto other = thread->userdata->script.lock();
            zend::logf("env.runscripts.other_vm: thread=%p script=%p",
                       reinterpret_cast<void*>(thread),
                       reinterpret_cast<void*>(other.get()));
        }
        return false;
    }

    auto locked = thread->userdata->script.lock();
    const auto script = reinterpret_cast<std::uintptr_t>(locked.get());
    if (script && scan->seen->insert(script).second) {
        if (env_trace()) {
            zend::logf("env.runscripts.binding: thread=%p script=%p",
                       reinterpret_cast<void*>(thread),
                       reinterpret_cast<void*>(script));
        }
        scan->scripts->push_back(std::move(locked));
    }
    return false;
}

std::vector<std::shared_ptr<uintptr_t>> running_script_pointers(lua_State* L) {
    std::vector<std::shared_ptr<uintptr_t>> scripts;
    std::unordered_set<std::uintptr_t> seen;
    ScriptPointerScan scan{L, &seen, &scripts};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, script_pointer_visitor);
    L->global->GCthreshold = threshold;

    if (env_trace()) {
        zend::logf("env.runscripts.scan: threads=%d userdata=%d expired=%d "
                   "other_vm=%d scripts=%d",
                   scan.threads, scan.with_userdata, scan.expired,
                   scan.other_vm, static_cast<int>(scripts.size()));
    }
    return scripts;
}

struct InstancePointerScan {
    lua_State* L;
    const std::unordered_set<std::uintptr_t>* scripts;
    std::unordered_set<std::uintptr_t>* found;
    std::uintptr_t single;
    int result;
    int count;
    bool match;
};

bool instance_pointer_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<InstancePointerScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || gco->gch.tt != LUA_TUSERDATA) {
        return false;
    }

    push_gcobject(L, gco);
    if (std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
        lua_pop(L, 1);
        return false;
    }

    void** userdata = static_cast<void**>(lua_touserdata(L, -1));
    const auto pointer =
        userdata && *userdata ? reinterpret_cast<std::uintptr_t>(*userdata) : 0;
    const bool match =
        scan->scripts ? (pointer && scan->scripts->contains(pointer))
                      : (pointer && pointer == scan->single);
    if (!match) {
        lua_pop(L, 1);
        return false;
    }

    scan->match = true;
    if (scan->found) {
        scan->found->insert(pointer);
    }
    if (scan->result != 0) {
        lua_rawseti(L, scan->result, ++scan->count);
    }
    return false;
}

int collect_instances_by_pointer(lua_State* L,
                                 const std::unordered_set<std::uintptr_t>* scripts,
                                 std::uintptr_t single) {
    lua_newtable(L);
    std::unordered_set<std::uintptr_t> found;
    InstancePointerScan scan{L, scripts, &found, single, lua_gettop(L), 0, false};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, instance_pointer_visitor);
    L->global->GCthreshold = threshold;

    if (scripts) {
        // A script whose env `script` global was cleared may no longer have a
        // scannable userdata. Only reuse the engine's cached userdata; a fresh
        // one cannot be built from a raw pointer safely.
        const int result = lua_gettop(L);
        for (const std::uintptr_t pointer : *scripts) {
            if (!pointer || found.contains(pointer)) {
                continue;
            }
            if (!push_cached_instance(L, pointer)) {
                continue;
            }
            found.insert(pointer);
            lua_rawseti(L, result, ++scan.count);
        }
        lua_settop(L, result);
    }

    return 1;
}

int getrunningscripts(lua_State* L) {
    lua_newtable(L);
    const int out = lua_gettop(L);
    int count = 0;
    std::unordered_set<std::uintptr_t> seen;

    auto push_script_value = [&](int value) {
        value = lua_absindex(L, value);
        if (!lua_isuserdata(L, value) ||
            std::strcmp(luaL_typename(L, value), "Instance") != 0) {
            return;
        }
        const std::uintptr_t pointer = instance_raw_pointer(L, value);
        if (!pointer || !seen.insert(pointer).second) {
            return;
        }
        lua_pushvalue(L, value);
        lua_rawseti(L, out, ++count);
    };

    // The reference walks the registry's thread values and reads `script` out
    // of each thread's environment. That is the only path that sees a script
    // placed in a plain thread's env (the fake-Actor case), because such a
    // thread has no RobloxExtraSpace script weakref.
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    const int registry = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, registry) != 0) {
        if (lua_isthread(L, -1)) {
            const int thread = lua_gettop(L);
            lua_getfenv(L, thread);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "script");
                lua_rawget(L, -2);
                push_script_value(lua_gettop(L));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // A real script keeps its instance in the thread's RobloxExtraSpace even
    // after its `script` global is nil or reassigned, so merge that view in.
    // Destroyed scripts no longer have a cached userdata, so push the locked
    // binding through the instance bridge.
    auto scripts = running_script_pointers(L);
    for (auto& locked : scripts) {
        const auto pointer = reinterpret_cast<std::uintptr_t>(locked.get());
        if (!pointer || seen.contains(pointer)) {
            continue;
        }
        const int before_push = lua_gettop(L);
        if (!push_instance_raw(L, &locked) ||
            lua_gettop(L) != before_push + 1 || !lua_isuserdata(L, -1) ||
            std::strcmp(luaL_typename(L, -1), "Instance") != 0) {
            lua_settop(L, before_push);
            continue;
        }
        if (instance_raw_pointer(L, -1) != pointer) {
            lua_settop(L, before_push);
            continue;
        }
        seen.insert(pointer);
        lua_rawseti(L, out, ++count);
    }

    if (env_trace()) {
        zend::logf("env.getrunningscripts: count=%d (registry+gc)", count);
        for (int index = 1; index <= count && index <= 80; ++index) {
            lua_rawgeti(L, out, index);
            lua_getfield(L, -1, "ClassName");
            const char* class_name =
                lua_isstring(L, -1) ? lua_tostring(L, -1) : "?";
            lua_getfield(L, -2, "Name");
            const char* name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "?";
            zend::logf("env.getrunningscripts:   [%d] %s %s", index, class_name,
                       name);
            lua_pop(L, 3);
        }
    }
    lua_settop(L, out);
    return 1;
}

int getscriptfromthread(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State* thread = lua_tothread(L, 1);
    if (!thread) {
        lua_pushnil(L);
        return 1;
    }

    const bool trace = env_trace();
    const bool has_space = thread->userdata != nullptr;
    bool weak_expired = true;
    std::uintptr_t weak_script = 0;

    // The engine binding is the truth: it survives a nil or reassigned
    // `script` global and points at the script that owns the thread. Only when
    // that binding is absent/expired may the env table speak for the thread.
    if (has_space) {
        weak_expired = thread->userdata->script.expired();
        if (!weak_expired) {
            auto locked = thread->userdata->script.lock();
            weak_script = reinterpret_cast<std::uintptr_t>(locked.get());
            if (weak_script) {
                // Prefer the engine's cached userdata, then reuse any Instance
                // userdata the GC still holds for the same object. Never
                // fabricate one from the raw pointer.
                if (push_cached_instance(L, weak_script)) {
                    if (trace) {
                        std::string class_name = instance_class_name(L, -1);
                        zend::logf("env.getscriptfromthread: thread=%p weak=%p "
                                   "expired=0 class=%s path=weak_cached",
                                   reinterpret_cast<void*>(thread),
                                   reinterpret_cast<void*>(weak_script),
                                   class_name.c_str());
                    }
                    return 1;
                }
                collect_instances_by_pointer(L, nullptr, weak_script);
                lua_rawgeti(L, -1, 1);
                lua_remove(L, -2);
                if (lua_isuserdata(L, -1) &&
                    std::strcmp(luaL_typename(L, -1), "Instance") == 0) {
                    if (trace) {
                        std::string class_name = instance_class_name(L, -1);
                        zend::logf("env.getscriptfromthread: thread=%p weak=%p "
                                   "expired=0 class=%s path=weak_gc",
                                   reinterpret_cast<void*>(thread),
                                   reinterpret_cast<void*>(weak_script),
                                   class_name.c_str());
                    }
                    return 1;
                }
                lua_pop(L, 1);

                // No cached userdata: hand the engine's real shared_ptr to the
                // instance bridge. The binding keeps the instance alive even
                // after Destroy(), which is exactly the case the sUNC test
                // exercises.
                const int before_push = lua_gettop(L);
                if (push_instance_raw(L, &locked) &&
                    lua_gettop(L) == before_push + 1 &&
                    lua_isuserdata(L, -1) &&
                    std::strcmp(luaL_typename(L, -1), "Instance") == 0) {
                    void** block = static_cast<void**>(lua_touserdata(L, -1));
                    std::uintptr_t pushed = 0;
                    if (block &&
                        read_ptr(reinterpret_cast<std::uintptr_t>(block),
                                pushed) &&
                        pushed == weak_script) {
                        if (trace) {
                            std::string class_name = instance_class_name(L, -1);
                            zend::logf("env.getscriptfromthread: thread=%p "
                                       "weak=%p expired=0 class=%s "
                                       "path=weak_push",
                                       reinterpret_cast<void*>(thread),
                                       reinterpret_cast<void*>(weak_script),
                                       class_name.c_str());
                        }
                        return 1;
                    }
                    lua_pop(L, 1);
                } else if (lua_gettop(L) != before_push) {
                    lua_settop(L, before_push);
                }
            }
        }
    }

    // Plain threads (ones without a RobloxExtraSpace binding) keep their
    // script in the env table. The target place writes junk there on purpose,
    // so only a real script class is accepted.
    const char* gt_class = "-";
    std::string gt_class_name;
    if (thread->gt) {
        luaC_threadbarrier(L);
        lua_rawcheckstack(L, 2);
        sethvalue(L, L->top, thread->gt);
        L->top++;
        lua_pushstring(L, "script");
        lua_rawget(L, -2);
        if (std::strcmp(luaL_typename(L, -1), "Instance") == 0) {
            gt_class_name = instance_class_name(L, -1);
            gt_class = gt_class_name.c_str();
            if (is_script_class(gt_class_name)) {
                lua_remove(L, -2);
                if (trace) {
                    zend::logf("env.getscriptfromthread: thread=%p weak=%p "
                               "expired=%d gt_class=%s path=gt",
                               reinterpret_cast<void*>(thread),
                               reinterpret_cast<void*>(weak_script),
                               weak_expired ? 1 : 0, gt_class);
                }
                return 1;
            }
        }
        lua_pop(L, 2);
    }

    if (trace) {
        zend::logf("env.getscriptfromthread: thread=%p weak=%p expired=%d "
                   "gt_class=%s path=nil",
                   reinterpret_cast<void*>(thread),
                   reinterpret_cast<void*>(weak_script),
                   weak_expired ? 1 : 0, gt_class);
    }
    lua_pushnil(L);
    return 1;
}

int getsenv(lua_State* L) {
    check_instance(L, 1);
    const std::string class_name = instance_class_name(L, 1);
    if (!is_script_class(class_name)) {
        lua_pushnil(L);
        return 1;
    }

    const std::uintptr_t script = script_pointer(L, 1);
    if (!script) {
        lua_pushnil(L);
        return 1;
    }

    lua_State* found = nullptr;
    bool cross_vm = false;

    const int top = lua_gettop(L);
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    for (lua_Page* page = L->global->allgcopages; page;
         page = luaM_getnextpage(page)) {
        char* start = nullptr;
        char* end = nullptr;
        int busy = 0;
        int block = 0;
        luaM_getpagewalkinfo(page, &start, &end, &busy, &block);
        for (char* pos = start; pos != end; pos += block) {
            auto* gco = reinterpret_cast<GCObject*>(pos);
            if (!gco || gco->gch.tt != LUA_TTHREAD ||
                isdead(L->global, gco)) {
                continue;
            }
            auto* thread = reinterpret_cast<lua_State*>(gco);
            if (!thread->userdata || thread->userdata->script.expired()) {
                continue;
            }
            auto locked = thread->userdata->script.lock();
            if (reinterpret_cast<std::uintptr_t>(locked.get()) != script) {
                continue;
            }
            if (thread->global->mainthread != L->global->mainthread) {
                cross_vm = true;
                found = nullptr;
                break;
            }
            found = thread;
            break;
        }
        if (found || cross_vm) {
            break;
        }
    }
    L->global->GCthreshold = threshold;

    if (!found) {
        lua_settop(L, top);
        lua_pushnil(L);
        return 1;
    }

    luaC_threadbarrier(L);
    luaC_threadbarrier(found);
    lua_pushvalue(found, LUA_GLOBALSINDEX);
    lua_xmove(found, L, 1);
    return 1;
}

// Finds the metatable field names a script may inspect through debug.getproto.

// --- instance helpers ----------------------------------------------------

void check_instance(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, index), "Instance") != 0) {
        luaL_typeerror(L, index, "Instance");
    }
}

// Every helper below forwards to the instance's own method. The client
// implements the tree walk and the property checks; the wrapper only fixes the
// argument order and the error message, exactly like the reference does.
int call_instance_method(lua_State* L, const char* method, int first_arg,
                         int last_arg) {
    lua_getfield(L, 1, method);
    if (!lua_isfunction(L, -1)) {
        luaL_error(L, "%s: instance has no %s method", method, method);
    }
    lua_pushvalue(L, 1);
    int args = 1;
    for (int index = first_arg; index <= last_arg; ++index) {
        lua_pushvalue(L, index);
        ++args;
    }
    lua_call(L, args, 1);
    return 1;
}

int instance_getchildren(lua_State* L) {
    check_instance(L, 1);
    return call_instance_method(L, "GetChildren", 2, 1);
}

int instance_getdescendants(lua_State* L) {
    check_instance(L, 1);
    return call_instance_method(L, "GetDescendants", 2, 1);
}

int instance_findfirstchild(lua_State* L) {
    check_instance(L, 1);
    luaL_checkstring(L, 2);
    const int last = lua_isnoneornil(L, 3) ? 2 : 3;
    return call_instance_method(L, "FindFirstChild", 2, last);
}

int instance_findfirstchildofclass(lua_State* L) {
    check_instance(L, 1);
    luaL_checkstring(L, 2);
    return call_instance_method(L, "FindFirstChildOfClass", 2, 2);
}

int instance_findfirstchildwhichisa(lua_State* L) {
    check_instance(L, 1);
    luaL_checkstring(L, 2);
    const int last = lua_isnoneornil(L, 3) ? 2 : 3;
    return call_instance_method(L, "FindFirstChildWhichIsA", 2, last);
}

int instance_getpropertychanged(lua_State* L) {
    check_instance(L, 1);
    luaL_checkstring(L, 2);
    return call_instance_method(L, "GetPropertyChangedSignal", 2, 2);
}

// --- signals -------------------------------------------------------------

// getconnections is built on a sentinel connection: Connect a no-op, then read
// the connection pointer the client stored in the returned userdata. The
// signal's list is linked through Connection::next starting at the sentinel, so
// walking until we come back to it visits every live connection. The sentinel
// is disconnected again before the result is returned.
struct ConnectionRecord {
    std::uintptr_t connection = 0;
    std::uintptr_t old_enabled = 1;
    bool valid = false;
};

// Connections whose slot currently holds a hooksignal wrapper. The map keeps
// the original function so getconnections can keep reporting it.
struct HookedConnection {
    int original_ref = LUA_NOREF;
    int wrapper_ref = LUA_NOREF;
    Closure* original = nullptr;
};

std::unordered_map<std::uintptr_t, HookedConnection>
    g_hooked_connection_functions;

// Nodes our Connection:Disconnect retired. The engine still owns the node, but
// a disconnected connection must not show up in later getconnections results.
std::unordered_map<std::uintptr_t, std::uintptr_t> g_retired_connections;
std::mutex g_retired_connections_mutex;

constexpr const char* kConnectionMetatable = "__zend_connection_mt";

struct ConnectionWalk {
    std::uintptr_t nodes[256];
    int count = 0;
};

ConnectionWalk walk_connections(std::uintptr_t sentinel) {
    ConnectionWalk walk;
    std::uintptr_t current = 0;
    if (!read_ptr(sentinel + off::kConnectionNext, current)) {
        return walk;
    }

    while (current && current != sentinel && walk.count < 256) {
        walk.nodes[walk.count++] = current;
        std::uintptr_t next = 0;
        if (!read_ptr(current + off::kConnectionNext, next)) {
            break;
        }
        current = next;
    }
    return walk;
}

// Dumps raw words for a traced object. Logs one line per object so a normal
// run stays empty; probes turn tracing on to compare normal and Once nodes.
void trace_raw_words(const char* label, std::uintptr_t address, int words) {
    if (!address || words <= 0) {
        return;
    }
    if (words > 48) {
        words = 48;
    }
    std::uint64_t raw[48]{};
    if (!try_read_bytes(address, raw,
                        static_cast<std::size_t>(words) * sizeof(std::uint64_t))) {
        zend::logf("env.getconnections:     %s unreadable", label);
        return;
    }
    char line[1024]{};
    int used = 0;
    for (int word = 0; word < words && used < 980; ++word) {
        used += std::snprintf(line + used, sizeof(line) - used,
                              "%s%02X:%016llX", word ? " " : "", word * 8,
                              static_cast<unsigned long long>(raw[word]));
    }
    zend::logf("env.getconnections:     %s %s", label, line);
}

bool is_wait_slot(std::uintptr_t slot);

std::uintptr_t weak_object_field(std::uintptr_t slot_wrapper) {
    constexpr std::uint32_t kCandidates[] = {
        off::kSlotWrapperWeakObject,
        off::kSlotWrapperWeakObjectFallback,
        off::kSlotWrapperWeakObjectLegacy,
    };
    for (const std::uint32_t candidate : kCandidates) {
        std::uintptr_t value = 0;
        if (read_ptr(slot_wrapper + candidate, value) && is_user_ptr(value)) {
            return value;
        }
    }
    return 0;
}

std::uintptr_t weak_object_chain(std::uintptr_t connection) {
    std::uintptr_t first = 0;
    if (!read_ptr(connection + off::kConnectionChain, first) ||
        !is_user_ptr(first)) {
        return 0;
    }

    std::uintptr_t second = 0;
    if (!read_ptr(first + 0x8, second) || !is_user_ptr(second)) {
        return 0;
    }
    return weak_object_field(second);
}

std::uintptr_t connection_weak_object(std::uintptr_t connection) {
    std::uintptr_t slot_wrapper = 0;
    if (!read_ptr(connection + off::kConnectionSlotWrapper, slot_wrapper) ||
        !slot_wrapper) {
        return 0;
    }

    // A wait slot's +0x68 back-link points at itself, which the generic
    // weak-object probe would mistake for a ref object.
    if (is_wait_slot(slot_wrapper)) {
        return 0;
    }

    std::uintptr_t self = 0;
    read_ptr(connection + off::kConnectionSlotWrapperSelf, self);
    const bool is_self =
        self && is_user_ptr(self) && slot_wrapper == self + 0x18;

    if (!is_self) {
        return weak_object_chain(connection);
    }

    if (const std::uintptr_t direct = weak_object_field(slot_wrapper)) {
        return direct;
    }
    return weak_object_chain(connection);
}

bool is_lua_thread_ptr(std::uintptr_t address) {
    if (!is_user_ptr(address)) {
        return false;
    }
    std::uint8_t type = 0;
    if (!read_u8(address + offsetof(lua_State, tt), type) ||
        type != LUA_TTHREAD) {
        return false;
    }
    std::uintptr_t global = 0;
    return read_ptr(address + off::kLuaGlobal, global) && global != 0;
}

// Stronger check for pointers found by scanning a wait slot: the candidate
// must hang off a global whose main thread points back to that same global.
// This keeps a coincidental heap word from being pushed as a thread.
bool is_live_lua_thread(std::uintptr_t address) {
    if (!is_lua_thread_ptr(address)) {
        return false;
    }
    auto* thread = reinterpret_cast<lua_State*>(address);
    std::uintptr_t main_raw = 0;
    if (!read_ptr(reinterpret_cast<std::uintptr_t>(thread->global) +
                      offsetof(global_State, mainthread),
                  main_raw) ||
        !main_raw || !is_user_ptr(main_raw)) {
        return false;
    }
    auto* mainthread = reinterpret_cast<lua_State*>(main_raw);
    return is_lua_thread_ptr(main_raw) && mainthread->global == thread->global;
}

std::uintptr_t slot_vtable(std::uintptr_t slot) {
    std::uintptr_t vtable = 0;
    if (!slot || !read_ptr(slot, vtable)) {
        return 0;
    }
    return vtable;
}

bool is_wait_slot(std::uintptr_t slot) {
    return slot && slot_vtable(slot) == off::host_base() + off::kWaitSlotVftable;
}

// A WaitScriptSlot embeds the engine's WeakThreadRef (vtable pinned by the
// dump). That ref owns a shared LiveThreadRef whose +0x28 field points at the
// suspended lua_State; the registry ref id sits right after it. Walk the few
// pointer-sized fields around the vtable so a small layout drift does not lose
// the thread, and require the candidate to belong to this VM (or to hang off a
// global whose main thread points back at it).
lua_State* wait_slot_thread(std::uintptr_t slot,
                            std::uintptr_t expected_global) {
    if (!is_wait_slot(slot)) {
        return nullptr;
    }

    auto accept = [&](std::uintptr_t candidate, lua_State** out) {
        if (!is_user_ptr(candidate)) {
            return false;
        }
        if (is_lua_thread_ptr(candidate)) {
            auto* thread = reinterpret_cast<lua_State*>(candidate);
            if ((expected_global &&
                 reinterpret_cast<std::uintptr_t>(thread->global) ==
                     expected_global) ||
                is_live_lua_thread(candidate)) {
                *out = thread;
                return true;
            }
        }
        // LiveThreadRef dedicated to a script thread.
        std::uintptr_t raw_thread = 0;
        if (read_ptr(candidate + 0x28, raw_thread) &&
            is_lua_thread_ptr(raw_thread)) {
            auto* thread = reinterpret_cast<lua_State*>(raw_thread);
            if ((expected_global &&
                 reinterpret_cast<std::uintptr_t>(thread->global) ==
                     expected_global) ||
                is_live_lua_thread(raw_thread)) {
                *out = thread;
                return true;
            }
        }
        return false;
    };

    const std::uintptr_t base = off::host_base();
    for (std::uint32_t field = 0x20; field <= 0x80; field += 8) {
        std::uintptr_t vtable = 0;
        if (!read_ptr(slot + field, vtable) ||
            vtable != base + off::kWeakThreadRefVftable) {
            continue;
        }
        for (std::uint32_t inner = 0x08; inner <= 0x20; inner += 8) {
            std::uintptr_t candidate = 0;
            lua_State* thread = nullptr;
            if (read_ptr(slot + field + inner, candidate) &&
                accept(candidate, &thread)) {
                return thread;
            }
        }
    }
    for (std::uint32_t field = 0x20; field <= 0x100; field += 8) {
        std::uintptr_t candidate = 0;
        lua_State* thread = nullptr;
        if (read_ptr(slot + field, candidate) && accept(candidate, &thread)) {
            return thread;
        }
    }
    return nullptr;
}

// Diagnostic for WaitScriptSlot: dumps the slot and recognises the helper
// objects it keeps around the waiting Lua thread.
void trace_wait_slot(std::uintptr_t connection, std::uintptr_t slot) {
    if (!env_trace() || !slot) {
        return;
    }
    const std::uintptr_t base = off::host_base();
    zend::logf("env.waitslot: connection=%p slot=%p vtable=%p",
               reinterpret_cast<void*>(connection), reinterpret_cast<void*>(slot),
               reinterpret_cast<void*>(slot_vtable(slot)));
    trace_raw_words("waitslot", slot, 48);
    for (std::uint32_t field = 0; field <= 0x100; field += 8) {
        std::uintptr_t value = 0;
        if (!read_ptr(slot + field, value) || !is_user_ptr(value)) {
            continue;
        }
        if (is_lua_thread_ptr(value)) {
            zend::logf("env.waitslot: slot+%02X is lua_State %p",
                       field, reinterpret_cast<void*>(value));
            continue;
        }
        const std::uintptr_t vtable = slot_vtable(value);
        if (vtable == base + off::kWeakThreadRefVftable) {
            zend::logf("env.waitslot: slot+%02X -> WeakThreadRef %p", field,
                       reinterpret_cast<void*>(value));
            trace_raw_words("weakthreadref", value, 16);
        } else if (vtable == base + off::kWeakObjectRefVftable) {
            zend::logf("env.waitslot: slot+%02X -> WeakObjectRef %p", field,
                       reinterpret_cast<void*>(value));
            trace_raw_words("weakobjectref", value, 20);
        } else if (vtable == base + off::kWaitSlotVftable) {
            zend::logf("env.waitslot: slot+%02X -> WaitSlot %p", field,
                       reinterpret_cast<void*>(value));
        }
    }
    // Decode the embedded weak thread ref: the dump's LiveThreadRef keeps the
    // lua_State at +0x28 and the registry ref id at +0x30.
    for (std::uint32_t field = 0x20; field <= 0x80; field += 8) {
        std::uintptr_t vtable = 0;
        if (!read_ptr(slot + field, vtable) ||
            vtable != base + off::kWeakThreadRefVftable) {
            continue;
        }
        std::uintptr_t object = 0;
        std::uintptr_t control = 0;
        std::uintptr_t raw_thread = 0;
        std::uint8_t object_tt = 0;
        std::int32_t ref_id = 0;
        read_ptr(slot + field + 0x10, object);
        read_ptr(slot + field + 0x18, control);
        read_u8(object, object_tt);
        read_ptr(object + 0x28, raw_thread);
        read(object + 0x30, ref_id);
        zend::logf("env.waitslot: weakref@%02X object=%p control=%p "
                   "objtt=%02X thread=%p refid=%d",
                   field, reinterpret_cast<void*>(object),
                   reinterpret_cast<void*>(control),
                   static_cast<unsigned>(object_tt),
                   reinterpret_cast<void*>(raw_thread), ref_id);
        break;
    }
    if (lua_State* thread = wait_slot_thread(slot, 0)) {
        zend::logf("env.waitslot: resolved thread=%p global=%p status=%u",
                   reinterpret_cast<void*>(thread),
                   reinterpret_cast<void*>(thread->global),
                   static_cast<unsigned>(thread->status));
    } else {
        zend::logf("env.waitslot: no thread resolved");
    }
}

lua_State* connection_thread(std::uintptr_t weak_object) {
    std::uintptr_t raw = 0;
    if (!read_ptr(weak_object + off::kSlotWrapperThread, raw) ||
        !is_user_ptr(raw)) {
        return nullptr;
    }

    std::uint8_t type = 0;
    if (!read_u8(raw + offsetof(lua_State, tt), type) || type != LUA_TTHREAD) {
        return nullptr;
    }

    std::uintptr_t global = 0;
    if (!read_ptr(raw + off::kLuaGlobal, global) || !global) {
        return nullptr;
    }
    return reinterpret_cast<lua_State*>(raw);
}

Closure* connection_function(std::uintptr_t weak_object) {
    lua_State* thread = connection_thread(weak_object);
    if (!thread) {
        return nullptr;
    }

    std::int32_t function_id = 0;
    if (!read(weak_object + off::kSlotWrapperFunctionId, function_id) ||
        function_id <= 0) {
        return nullptr;
    }

    lua_getref(thread, function_id);
    Closure* closure = nullptr;
    if (lua_isfunction(thread, -1)) {
        closure = clvalue(luaA_toobject(thread, -1));
    }
    lua_pop(thread, 1);
    return closure;
}

struct ConnectionView {
    lua_State* thread = nullptr;
    Closure* closure = nullptr;
    bool wait_slot = false;
};

ConnectionView connection_view(std::uintptr_t connection, lua_State* self) {
    ConnectionView view;
    std::uintptr_t slot_wrapper = 0;
    if (!read_ptr(connection + off::kConnectionSlotWrapper, slot_wrapper) ||
        !slot_wrapper) {
        return view;
    }

    if (is_wait_slot(slot_wrapper)) {
        view.wait_slot = true;
        view.thread = wait_slot_thread(
            slot_wrapper, self ? reinterpret_cast<std::uintptr_t>(self->global)
                               : 0);
        return view;
    }

    const std::uintptr_t weak = connection_weak_object(connection);
    if (weak) {
        view.thread = connection_thread(weak);
        view.closure = connection_function(weak);
    }
    return view;
}

ConnectionRecord* connection_record(lua_State* L, int index) {
    lua_rawgetfield(L, index, "Object");
    ConnectionRecord* record = nullptr;
    if (lua_islightuserdata(L, -1)) {
        record = static_cast<ConnectionRecord*>(lua_touserdata(L, -1));
    }
    lua_pop(L, 1);
    return record;
}

int connection_noop(lua_State*) {
    return 0;
}

void push_connection_function(lua_State* L, Closure* closure) {
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    setclvalue(L, L->top, closure);
    incr_top(L);
}

void push_connection_thread(lua_State* L, lua_State* thread) {
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    setthvalue(L, L->top, thread);
    incr_top(L);
}

bool set_connection_enabled(std::uintptr_t connection, std::uintptr_t value) {
    return write<std::uint64_t>(connection + off::kConnectionEnabled,
                                static_cast<std::uint64_t>(value));
}

// The engine marks Once connections on the signal slot itself. Reading that
// byte is the only reliable way to know a node must disappear after one manual
// dispatch; node layout is otherwise identical to a normal connection.
bool connection_is_once(std::uintptr_t connection) {
    std::uintptr_t slot_wrapper = 0;
    if (!read_ptr(connection + off::kConnectionSlotWrapper, slot_wrapper) ||
        !slot_wrapper) {
        return false;
    }
    std::uint8_t is_once = 0;
    return read_u8(slot_wrapper + off::kSlotWrapperIsOnce, is_once) &&
           is_once != 0;
}

void retire_connection(std::uintptr_t connection) {
    if (!connection) {
        return;
    }
    set_connection_enabled(connection, 0);
    const std::uintptr_t weak = connection_weak_object(connection);
    std::lock_guard<std::mutex> lock(g_retired_connections_mutex);
    g_retired_connections[connection] = weak;
}

int connection_fire(lua_State* L);
int connection_defer(lua_State* L);
int connection_disable(lua_State* L);
int connection_enable(lua_State* L);
int connection_disconnect(lua_State* L);

int connection_index(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TSTRING);
    const char* key = lua_tostring(L, 2);

    ConnectionRecord* record = connection_record(L, 1);
    if (!record || !record->valid || !record->connection) {
        if (std::strcmp(key, "Enabled") == 0 ||
            std::strcmp(key, "LuaConnection") == 0 ||
            std::strcmp(key, "ForeignState") == 0) {
            lua_pushboolean(L, 0);
            return 1;
        }
        if (std::strcmp(key, "Function") == 0 ||
            std::strcmp(key, "Thread") == 0) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushcfunction(L, connection_noop, "Connection.noop");
        return 1;
    }

    if (std::strcmp(key, "Enabled") == 0) {
        std::uint64_t enabled = 0;
        read_u64(record->connection + off::kConnectionEnabled, enabled);
        lua_pushboolean(L, enabled != 0);
        return 1;
    }
    if (std::strcmp(key, "Disable") == 0) {
        lua_pushcfunction(L, connection_disable, "Connection.Disable");
        return 1;
    }
    if (std::strcmp(key, "Disconnect") == 0) {
        lua_pushcfunction(L, connection_disconnect, "Connection.Disconnect");
        return 1;
    }
    if (std::strcmp(key, "Enable") == 0) {
        lua_pushcfunction(L, connection_enable, "Connection.Enable");
        return 1;
    }
    if (std::strcmp(key, "Fire") == 0) {
        lua_pushcfunction(L, connection_fire, "Connection.Fire");
        return 1;
    }
    if (std::strcmp(key, "Defer") == 0) {
        lua_pushcfunction(L, connection_defer, "Connection.Defer");
        return 1;
    }

    const ConnectionView view = connection_view(record->connection, L);
    lua_State* thread = view.thread;
    Closure* closure = view.closure;
    const bool foreign = thread && thread->global != L->global;

    if (std::strcmp(key, "ForeignState") == 0) {
        lua_pushboolean(L, foreign);
        return 1;
    }
    if (std::strcmp(key, "LuaConnection") == 0) {
        // A :Wait() slot has no Lua closure, but the thread suspended in the
        // call makes it a Lua-visible connection all the same.
        lua_pushboolean(L, closure != nullptr || thread != nullptr);
        return 1;
    }
    if (std::strcmp(key, "Function") == 0) {
        if (foreign || !closure) {
            lua_pushnil(L);
            return 1;
        }
        // While a connection is hooked the slot holds our wrapper; callers
        // still expect the function the script connected.
        auto hooked = g_hooked_connection_functions.find(record->connection);
        if (hooked != g_hooked_connection_functions.end() &&
            hooked->second.original) {
            push_connection_function(L, hooked->second.original);
            return 1;
        }
        push_connection_function(L, closure);
        return 1;
    }
    if (std::strcmp(key, "Thread") == 0) {
        if (foreign || !thread) {
            lua_pushnil(L);
            return 1;
        }
        push_connection_thread(L, thread);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

int connection_newindex(lua_State*) {
    return 0;
}

// task.spawn/task.defer keep the callback's yields owned by the client
// scheduler, exactly like a natural signal dispatch does.
int connection_spawn(lua_State* L, const char* method) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ConnectionRecord* record = connection_record(L, 1);
    if (!record || !record->valid || !record->connection) {
        return 0;
    }

    const std::uintptr_t weak = connection_weak_object(record->connection);
    lua_State* thread = weak ? connection_thread(weak) : nullptr;
    if (thread && thread->global != L->global) {
        return 0;
    }

    Closure* closure = weak ? connection_function(weak) : nullptr;
    if (!closure) {
        return 0;
    }

    const bool once = connection_is_once(record->connection);

    if (env_trace()) {
        zend::logf("env.connection.%s: node=%p isC=%d once=%d args=%d", method,
                   reinterpret_cast<void*>(record->connection),
                   closure->isC ? 1 : 0, once ? 1 : 0, lua_gettop(L) - 1);
    }

    const int argument_count = lua_gettop(L) - 1;

    lua_getglobal(L, "task");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    lua_getfield(L, -1, method);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }

    lua_rawcheckstack(L, argument_count + 2);
    luaC_threadbarrier(L);
    setclvalue(L, L->top, closure);
    incr_top(L);
    for (int index = 2; index <= argument_count + 1; ++index) {
        lua_pushvalue(L, index);
    }
    lua_call(L, argument_count + 1, 0);

    if (once) {
        std::uintptr_t slot_wrapper = 0;
        if (read_ptr(record->connection + off::kConnectionSlotWrapper,
                     slot_wrapper) &&
            slot_wrapper) {
            write<std::uint8_t>(slot_wrapper + off::kSlotWrapperOnceFired, 1);
        }
        // The engine unlinks an Once node after dispatch. Retire it here so a
        // manual Fire/Defer behaves exactly like a natural engine fire.
        retire_connection(record->connection);
        record->valid = false;
        if (env_trace()) {
            zend::logf("env.connection.%s: once retired node=%p", method,
                       reinterpret_cast<void*>(record->connection));
        }
    }
    return 0;
}

int connection_fire(lua_State* L) {
    return connection_spawn(L, "spawn");
}

int connection_defer(lua_State* L) {
    return connection_spawn(L, "defer");
}

int connection_disable(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ConnectionRecord* record = connection_record(L, 1);
    if (record && record->connection) {
        set_connection_enabled(record->connection, 0);
    }
    return 0;
}

int connection_enable(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ConnectionRecord* record = connection_record(L, 1);
    if (record && record->connection) {
        const std::uintptr_t restore = record->old_enabled ? record->old_enabled : 1;
        set_connection_enabled(record->connection, restore);
    }
    return 0;
}

int connection_disconnect(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ConnectionRecord* record = connection_record(L, 1);
    if (!record || !record->connection) {
        return 0;
    }

    // The reference only clears the enabled flag; the engine stops dispatching
    // this connection on the next fire. Track it so getconnections omits it even
    // while the engine's node remains linked.
    retire_connection(record->connection);
    if (env_trace()) {
        zend::logf("env.connection.disconnect: node=%p",
                   reinterpret_cast<void*>(record->connection));
    }
    record->valid = false;
    return 0;
}

bool push_connection(lua_State* L, std::uintptr_t connection) {
    auto* record = static_cast<ConnectionRecord*>(
        lua_newuserdata(L, sizeof(ConnectionRecord)));
    record->connection = connection;
    record->valid = true;
    record->old_enabled = 0;
    std::uint64_t enabled = 0;
    if (read_u64(connection + off::kConnectionEnabled, enabled)) {
        record->old_enabled = static_cast<std::uintptr_t>(enabled);
    }
    if (!record->old_enabled) {
        record->old_enabled = 1;
    }

    lua_newtable(L);
    lua_pushlightuserdata(L, record);
    lua_setfield(L, -2, "Object");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "_Object");
    lua_getfield(L, LUA_REGISTRYINDEX, kConnectionMetatable);
    lua_setmetatable(L, -2);
    lua_remove(L, -2);
    return true;
}

// Calls the engine's own Disconnect on a connection userdata. Only flips the
// engine's enabled flag on this build, but that is exactly what we need to
// retire a no-op sentinel without guessing its unlink layout.
void disconnect_connection_userdata(lua_State* L, int index) {
    index = lua_absindex(L, index);
    lua_getfield(L, index, "Disconnect");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_pushvalue(L, index);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        lua_pop(L, 1);
    }
}

bool is_sentinel_connection(std::uintptr_t connection) {
    const std::uintptr_t weak = connection_weak_object(connection);
    if (!weak) {
        return false;
    }
    const Closure* closure = connection_function(weak);
    return closure && closure->isC && closure->c.f == connection_noop;
}

// Enumerating a signal needs a node that sits between us and the engine list:
// the engine gives no public head pointer. We connect one no-op node per call,
// walk from it (the engine prepends, so every real connection is behind it),
// and the identity filter above keeps no-op nodes out of the result. The
// sentinel is retired afterwards; Disconnect only clears its enabled flag, so
// it stays linked but never dispatches and never shows up again.
int getconnections(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    lua_getfield(L, 1, "Connect");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "getconnections: signal has no Connect method");
    }

    lua_pushvalue(L, 1);
    lua_pushcfunction(L, connection_noop, "getconnections_sentinel");
    if (lua_pcall(L, 2, 1, 0) != 0) {
        const char* message = lua_tostring(L, -1);
        luaL_error(L, "%s", message ? message : "getconnections: Connect failed");
    }

    const int handle = lua_gettop(L);
    void* userdata = lua_touserdata(L, handle);
    std::uintptr_t sentinel = 0;
    if (lua_type(L, handle) == LUA_TLIGHTUSERDATA) {
        sentinel = reinterpret_cast<std::uintptr_t>(userdata);
    } else if (userdata) {
        read_ptr(reinterpret_cast<std::uintptr_t>(userdata), sentinel);
    }

    if (!sentinel) {
        lua_settop(L, handle - 1);
        lua_newtable(L);
        return 1;
    }

    const ConnectionWalk walk = walk_connections(sentinel);

    lua_newtable(L);
    const int result = lua_gettop(L);
    int count = 0;
    static int g_getconnections_calls = 0;
    const int call_index = ++g_getconnections_calls;
    if (env_trace()) {
        zend::logf("env.getconnections: call=%d sentinel=%p nodes=%d", call_index,
                   reinterpret_cast<void*>(sentinel), walk.count);
    }
    for (int index = 0; index < walk.count; ++index) {
        const std::uintptr_t connection = walk.nodes[index];
        std::uint64_t enabled = 0;
        read_u64(connection + off::kConnectionEnabled, enabled);
        const std::uintptr_t weak = connection_weak_object(connection);
        const Closure* closure = weak ? connection_function(weak) : nullptr;
        const bool c_closure = closure && closure->isC;
        if (env_trace()) {
            std::uintptr_t slot_wrapper = 0;
            read_ptr(connection + off::kConnectionSlotWrapper, slot_wrapper);
            zend::logf("env.getconnections:   [%d] node=%p enabled=%d weak=%p "
                       "slot=%p closure=%p isC=%d sentinel=%d",
                       index, reinterpret_cast<void*>(connection),
                       enabled ? 1 : 0, reinterpret_cast<void*>(weak),
                       reinterpret_cast<void*>(slot_wrapper),
                       reinterpret_cast<const void*>(closure),
                       c_closure ? 1 : 0,
                       is_sentinel_connection(connection) ? 1 : 0);
            if (index < 8) {
                trace_raw_words("node", connection, 32);
                trace_raw_words("slot", slot_wrapper, 40);
                trace_raw_words("weak", weak, 24);
                if (slot_wrapper &&
                    slot_vtable(slot_wrapper) ==
                        off::host_base() + off::kWaitSlotVftable) {
                    trace_wait_slot(connection, slot_wrapper);
                }
                if (closure && !closure->isC && closure->l.p) {
                    const Proto* proto = closure->l.p;
                    zend::logf("env.getconnections:     proto=%p source=%p "
                               "debugname=%p nups=%d numparams=%d",
                               reinterpret_cast<const void*>(proto),
                               reinterpret_cast<const void*>(proto->source.Get()),
                               reinterpret_cast<const void*>(
                                   proto->debugname.Get()),
                               proto->nups, proto->numparams);
                }
            }
        }
        if (!connection || is_sentinel_connection(connection)) {
            continue;
        }

        bool retired = false;
        {
            std::lock_guard<std::mutex> lock(g_retired_connections_mutex);
            const auto entry = g_retired_connections.find(connection);
            if (entry != g_retired_connections.end()) {
                // A recycled node would have a different slot wrapper; a match
                // on both fields is our retired connection.
                retired = entry->second == 0 || entry->second == weak;
                if (!retired) {
                    g_retired_connections.erase(entry);
                }
            }
        }
        if (retired) {
            if (env_trace()) {
                zend::logf("env.getconnections:   [%d] retired", index);
            }
            continue;
        }

        push_connection(L, connection);
        lua_rawseti(L, result, ++count);
    }
    if (env_trace()) {
        zend::logf("env.getconnections: call=%d returned=%d", call_index, count);
    }

    lua_pushvalue(L, handle);
    disconnect_connection_userdata(L, -1);
    lua_pop(L, 1);

    lua_remove(L, handle);
    return 1;
}

int firesignal(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    const int argument_count = lua_gettop(L);
    getconnections(L);

    const int connections = lua_gettop(L);
    const int count = lua_objlen(L, connections);
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(L, connections, index);

        lua_getfield(L, -1, "Function");
        const bool has_function = lua_isfunction(L, -1);
        lua_pop(L, 1);

        if (has_function) {
            lua_getfield(L, -1, "Fire");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);
                for (int arg = 2; arg <= argument_count; ++arg) {
                    lua_pushvalue(L, arg);
                }
                if (lua_pcall(L, argument_count, 0, 0) != 0) {
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
        }

        lua_pop(L, 1);
    }
    return 0;
}

// --- touch and proximity prompt ------------------------------------------

std::uintptr_t instance_value(lua_State* L, int index) {
    check_instance(L, index);
    std::uintptr_t value = 0;
    if (!read_ptr(reinterpret_cast<std::uintptr_t>(lua_touserdata(L, index)),
                  value) ||
        !is_user_ptr(value)) {
        luaL_argerror(L, index, "Expected an Instance");
    }
    return value;
}

bool instance_is_a(lua_State* L, int index, const char* class_name) {
    index = lua_absindex(L, index);
    lua_getfield(L, index, "IsA");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushvalue(L, index);
    lua_pushstring(L, class_name);
    lua_call(L, 2, 1);
    const bool match = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return match;
}

// --- hidden properties and scriptable flags -------------------------------
//
// Property descriptors live in a hash table on the class descriptor: entry
// pair offsets and the name field position vary per build, so probe them once
// with the always-present `Name` property, then reuse the winning combination
// for every class. The scriptable bit is at a fixed offset from the descriptor.

constexpr std::uintptr_t kClassDescriptorOffset = 0x18;
constexpr std::uintptr_t kPropertyEntrySize = 0x10;
constexpr std::uint32_t kPropertyScriptableBit = 0x10;
constexpr std::uintptr_t kPropertyBitFlags = 0xAC;
constexpr std::uintptr_t kPropertyInstanceOffset = 0x80;
constexpr std::uintptr_t kPropertyGetSet = 0xB0;
constexpr std::uintptr_t kPropertyGetSetRef = 0xB8;
constexpr std::uintptr_t kPropertyTypeInfo = 0x70;
constexpr std::uintptr_t kPropertyTypeInfoAlt = 0x68;
constexpr std::uintptr_t kPropertyTypeId = 0x30;
constexpr int kTypeBinaryString = 30;
constexpr int kTypeSharedString = 63;
constexpr const char* kHiddenStringRegistryKey = "__zend_hidden_strings";

struct PropertyLayout {
    bool probed = false;
    bool ok = false;
    std::uintptr_t entries = 0;
    std::uintptr_t mask = 0;
    std::uintptr_t name = 0;
};

PropertyLayout g_property_layout;

std::map<std::uintptr_t, bool> g_pristine_scriptable;
std::unordered_map<std::uintptr_t, std::unordered_map<std::string, bool>>
    g_scriptable_overrides;

bool text_equals_at(std::uintptr_t address, const char* text,
                    std::size_t length) {
    if (length > 256) {
        return false;
    }
    char buffer[260];
    if (!try_read_bytes(address, buffer, length + 1)) {
        return false;
    }
    return std::memcmp(buffer, text, length) == 0 && buffer[length] == '\0';
}

bool property_name_equals(std::uintptr_t property, std::uintptr_t name_offset,
                          const char* name) {
    const std::size_t length = std::strlen(name);
    std::uintptr_t holder = 0;
    if (read_ptr(property + name_offset, holder) &&
        text_equals_at(holder, name, length)) {
        return true;
    }
    return text_equals_at(property + name_offset, name, length);
}

std::uintptr_t find_property_entry(std::uintptr_t class_descriptor,
                                   const char* name,
                                   std::uintptr_t entries_offset,
                                   std::uintptr_t mask_offset,
                                   std::uintptr_t name_offset) {
    std::uintptr_t entries = 0;
    std::uint32_t mask = 0;
    if (!read_ptr(class_descriptor + entries_offset, entries) || !entries) {
        return 0;
    }
    if (!read_u32(class_descriptor + mask_offset, mask) ||
        mask == 0xFFFFFFFFu) {
        return 0;
    }
    const std::uint32_t slots = mask + 1;
    if (slots == 0 || slots > 8192) {
        return 0;
    }
    for (std::uint32_t index = 0; index < slots; ++index) {
        std::uintptr_t property = 0;
        if (!read_ptr(entries + static_cast<std::uintptr_t>(index) *
                                   kPropertyEntrySize,
                      property) ||
            !property) {
            continue;
        }
        if (property_name_equals(property, name_offset, name)) {
            return property;
        }
    }
    return 0;
}

bool ensure_property_layout(std::uintptr_t instance) {
    if (g_property_layout.probed) {
        return g_property_layout.ok;
    }
    g_property_layout.probed = true;

    std::uintptr_t class_descriptor = 0;
    if (!instance ||
        !read_ptr(instance + kClassDescriptorOffset, class_descriptor) ||
        !class_descriptor) {
        return false;
    }

    constexpr std::uintptr_t kEntryPairs[][2] = {{0x1F0, 0x1F8},
                                                 {0x1E8, 0x1F0}};
    constexpr std::uintptr_t kNameOffsets[] = {0x98, 0x90, 0xA0, 0x08};
    for (const auto& pair : kEntryPairs) {
        for (const std::uintptr_t name_offset : kNameOffsets) {
            const std::uintptr_t property =
                find_property_entry(class_descriptor, "Name", pair[0], pair[1],
                                    name_offset);
            if (property) {
                g_property_layout.entries = pair[0];
                g_property_layout.mask = pair[1];
                g_property_layout.name = name_offset;
                g_property_layout.ok = true;
                return true;
            }
        }
    }
    return false;
}

std::uintptr_t instance_raw_pointer(lua_State* L, int index) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    if (!lua_isuserdata(L, index)) {
        return 0;
    }
    void** block = static_cast<void**>(lua_touserdata(L, index));
    std::uintptr_t raw = 0;
    if (!block ||
        !read_ptr(reinterpret_cast<std::uintptr_t>(block), raw) ||
        !is_user_ptr(raw)) {
        return 0;
    }
    return raw;
}

std::uintptr_t resolve_property(lua_State* L, int index, const char* name) {
    index = lua_absindex(L, index);
    check_instance(L, index);
    const std::uintptr_t instance = instance_raw_pointer(L, index);
    if (!instance || !ensure_property_layout(instance)) {
        return 0;
    }
    std::uintptr_t class_descriptor = 0;
    if (!read_ptr(instance + kClassDescriptorOffset, class_descriptor) ||
        !class_descriptor) {
        return 0;
    }
    return find_property_entry(class_descriptor, name,
                               g_property_layout.entries,
                               g_property_layout.mask, g_property_layout.name);
}

bool read_property_scriptable(std::uintptr_t property) {
    std::uint32_t bits = 0;
    if (!property || !read_u32(property + kPropertyBitFlags, bits)) {
        return false;
    }
    return (bits & kPropertyScriptableBit) != 0;
}

bool write_property_scriptable(std::uintptr_t property, bool enabled) {
    if (!property) {
        return false;
    }
    const std::uintptr_t address = property + kPropertyBitFlags;
    std::uint32_t bits = 0;
    if (!read_u32(address, bits)) {
        return false;
    }
    const std::uint32_t wanted =
        enabled ? (bits | kPropertyScriptableBit)
                : (bits & ~kPropertyScriptableBit);
    if (wanted == bits) {
        return true;
    }
    if (!write<std::uint32_t>(address, wanted)) {
        return false;
    }
    std::uint32_t after = 0;
    return read_u32(address, after) && after == wanted;
}

bool find_property_type(std::uintptr_t property, std::uintptr_t& type_address,
                        int& type_id) {
    type_address = 0;
    type_id = -1;
    std::uintptr_t descriptor = 0;
    if (!read_ptr(property + kPropertyTypeInfo, descriptor) || !descriptor) {
        if (!read_ptr(property + kPropertyTypeInfoAlt, descriptor) ||
            !descriptor) {
            return false;
        }
    }
    type_address = descriptor + kPropertyTypeId;
    std::uint32_t raw_type = 0;
    if (!read_u32(type_address, raw_type)) {
        type_address = 0;
        type_id = -1;
        return false;
    }
    type_id = static_cast<int>(raw_type);
    return true;
}

int protected_getfield(lua_State* L) {
    lua_getfield(L, 1, lua_tostring(L, 2));
    return 1;
}

int protected_setfield(lua_State* L) {
    lua_pushvalue(L, 3);
    lua_setfield(L, 1, lua_tostring(L, 2));
    return 0;
}

bool property_was_hidden(std::uintptr_t property) {
    const auto pristine = g_pristine_scriptable.find(property);
    if (pristine != g_pristine_scriptable.end()) {
        return !pristine->second;
    }
    return !read_property_scriptable(property);
}

bool lookup_scriptable_override(std::uintptr_t instance, const char* name,
                                bool& value) {
    if (!instance || !name) {
        return false;
    }
    const auto entry = g_scriptable_overrides.find(instance);
    if (entry == g_scriptable_overrides.end()) {
        return false;
    }
    const auto property = entry->second.find(name);
    if (property == entry->second.end()) {
        return false;
    }
    value = property->second;
    return true;
}

// Reads a hidden BinaryString straight out of the instance field described by
// the verified offset dump (dump/offsets_rbx.hpp). The property descriptor
// getter is deliberately not used: calling it for Terrain.MaterialColors has
// crashed the client, and the descriptor type must not be rewritten either.
// `property + 0x80` is the per-descriptor instance field offset (dump:
// Property::InstanceOffset). BinaryString members are MSVC std::string values,
// so the payload is either inline (SSO) or behind a heap pointer.
bool read_msvc_string(std::uintptr_t address, std::string& out) {
    std::uint64_t length = 0;
    std::uint64_t capacity = 0;
    if (!read(address + 0x10, length) || !read(address + 0x18, capacity)) {
        return false;
    }
    if (length > (64u << 20) || length > capacity) {
        return false;
    }
    std::uintptr_t data = address;
    if (capacity > 0xF && (!read_ptr(address, data) || !is_user_ptr(data))) {
        return false;
    }
    if (length == 0) {
        out.clear();
        return true;
    }
    out.resize(static_cast<std::size_t>(length));
    return try_read_bytes(data, out.data(), out.size());
}

// Tags is a SharedString with no instance offset: the engine's Lua bridge has
// no converter for that type and the descriptor getter must not be called.
// Ask the engine for the tag list through its public method and serialize it
// the way the property stores it (null-separated tag names).
bool collect_tags(lua_State* L, int index, std::vector<std::string>& tags) {
    index = lua_absindex(L, index);
    lua_getfield(L, index, "GetTags");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushvalue(L, index);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    const int count = static_cast<int>(lua_objlen(L, -1));
    tags.clear();
    for (int tag = 1; tag <= count; ++tag) {
        lua_rawgeti(L, -1, tag);
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        if (text && length > 0) {
            tags.emplace_back(text, length);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

bool push_tags_property(lua_State* L, int index) {
    std::vector<std::string> tags;
    if (!collect_tags(L, index, tags)) {
        if (env_trace()) {
            zend::logf("env.tags: GetTags failed");
        }
        return false;
    }
    std::string blob;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (i != 0) {
            blob.push_back('\0');
        }
        blob.append(tags[i]);
    }
    if (env_trace()) {
        zend::logf("env.tags: count=%zu blob=%zu", tags.size(), blob.size());
    }
    lua_pushlstring(L, blob.data(), blob.size());
    return true;
}

// Some SharedString/BinaryString properties are computed by the engine and
// have no writable instance field in this build. Rewriting their descriptor
// type crashes the client, and calling the descriptor accessor is off limits,
// so keep the value the script wrote in a weak-keyed table and return it on
// the next read. The instance userdata is the key, so an entry disappears as
// soon as the instance is collected and cannot leak into a reused address.
void ensure_hidden_string_table(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHiddenStringRegistryKey);
    if (lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "__mode");
    lua_pushliteral(L, "k");
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kHiddenStringRegistryKey);
}

bool get_hidden_string_override(lua_State* L, int index, const char* name,
                                std::string& out) {
    if (!name) {
        return false;
    }
    index = lua_absindex(L, index);
    ensure_hidden_string_table(L);

    lua_getfield(L, LUA_REGISTRYINDEX, kHiddenStringRegistryKey);
    lua_pushvalue(L, index);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_getfield(L, -1, name);
    std::size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    const bool found = text != nullptr;
    if (found) {
        out.assign(text, length);
    }
    lua_pop(L, 3);
    return found;
}

void set_hidden_string_override(lua_State* L, int index, const char* name,
                                const char* data, std::size_t length) {
    if (!name) {
        return;
    }
    index = lua_absindex(L, index);
    ensure_hidden_string_table(L);

    lua_getfield(L, LUA_REGISTRYINDEX, kHiddenStringRegistryKey);
    lua_pushvalue(L, index);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, index);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_pushlstring(L, data ? data : "", data ? length : 0);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);
}

bool call_tag_method(lua_State* L, int index, const char* method,
                     const std::string& tag) {
    index = lua_absindex(L, index);
    lua_getfield(L, index, method);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushvalue(L, index);
    lua_pushlstring(L, tag.data(), tag.size());
    const bool ok = lua_pcall(L, 2, 0, 0) == LUA_OK;
    if (!ok) {
        lua_pop(L, 1);
    }
    return ok;
}

bool sync_tags_property(lua_State* L, int index, const char* data,
                        std::size_t length) {
    std::vector<std::string> wanted;
    std::unordered_set<std::string> wanted_set;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= length; ++i) {
        if (i != length && data[i] != '\0') {
            continue;
        }
        if (i > start) {
            std::string tag(data + start, i - start);
            if (wanted_set.insert(tag).second) {
                wanted.push_back(std::move(tag));
            }
        }
        start = i + 1;
    }

    std::vector<std::string> current;
    if (!collect_tags(L, index, current)) {
        return false;
    }
    const std::unordered_set<std::string> current_set(current.begin(),
                                                      current.end());
    for (const auto& tag : current) {
        if (!wanted_set.contains(tag) &&
            !call_tag_method(L, index, "RemoveTag", tag)) {
            return false;
        }
    }
    for (const auto& tag : wanted) {
        if (!current_set.contains(tag) &&
            !call_tag_method(L, index, "AddTag", tag)) {
            return false;
        }
    }
    return true;
}

// SharedString properties (AeroMeshData and friends) have no instance field
// offset in the dump, and their holders are not std::string-shaped in every
// build. The engine's own Lua bridge can convert the value once the property
// is scriptable; the reference environment does exactly that. Toggle the flag,
// read through the bridge, then restore the descriptor unchanged.
bool read_shared_string_via_engine(lua_State* L, int index, const char* name,
                                   std::uintptr_t property,
                                   std::string& out) {
    if (!property || !name) {
        return false;
    }
    index = lua_absindex(L, index);

    const std::uintptr_t flags_address = property + kPropertyBitFlags;
    std::uint32_t original_flags = 0;
    if (!read_u32(flags_address, original_flags)) {
        return false;
    }
    const bool already_scriptable =
        (original_flags & kPropertyScriptableBit) != 0;
    if (!already_scriptable &&
        !write<std::uint32_t>(flags_address,
                              original_flags | kPropertyScriptableBit)) {
        return false;
    }

    const int top = lua_gettop(L);
    lua_pushcfunction(L, protected_getfield, "gethiddenproperty");
    lua_pushvalue(L, index);
    lua_pushstring(L, name);
    const int status = lua_pcall(L, 2, 1, 0);

    if (!already_scriptable) {
        write<std::uint32_t>(flags_address, original_flags);
    }

    bool ok = false;
    if (status == LUA_OK) {
        if (lua_type(L, top + 1) == LUA_TBUFFER) {
            std::size_t length = 0;
            void* data = lua_tobuffer(L, top + 1, &length);
            if (data) {
                out.assign(static_cast<const char*>(data), length);
            } else {
                out.clear();
            }
            ok = true;
        } else if (lua_isstring(L, top + 1)) {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, top + 1, &length);
            out.assign(text ? text : "", text ? length : 0);
            ok = true;
        }
    }

    lua_settop(L, top);
    return ok;
}

bool push_binary_string_field(lua_State* L, int index, std::uintptr_t instance,
                              std::uintptr_t property, const char* name,
                              bool shared_string) {
    if (!instance) {
        return false;
    }

    // Terrain.MaterialColors is the only NotScriptable BinaryString in the API
    // dump whose payload is a std::vector<std::uint8_t> of RGB triples.
    constexpr std::uintptr_t kTerrainMaterialColors = 0x4A0;
    if (name && std::strcmp(name, "MaterialColors") == 0 &&
        instance_class_name(L, index) == "Terrain") {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        if (read_ptr(instance + kTerrainMaterialColors, begin) &&
            read_ptr(instance + kTerrainMaterialColors + 0x8, end) &&
            is_user_ptr(begin) && end >= begin) {
            const std::uintptr_t length = end - begin;
            if (length > 0 && length <= (64u << 20)) {
                std::string value(static_cast<std::size_t>(length), '\0');
                if (try_read_bytes(begin, value.data(), value.size())) {
                    lua_pushlstring(L, value.data(), value.size());
                    return true;
                }
            }
        }
    }

    if (!property) {
        return false;
    }

    // Every other BinaryString/SharedString property lives in a per-instance
    // field described by the property descriptor. Some computed ones have no
    // field, so trace their accessor addresses for static analysis.
    if (env_trace()) {
        std::uintptr_t getset = 0;
        std::uintptr_t vtable = 0;
        std::uintptr_t getter = 0;
        std::uintptr_t setter = 0;
        if (read_ptr(property + kPropertyGetSet, getset) && getset) {
            read_ptr(getset, vtable);
            if (vtable) {
                read_ptr(vtable + 0x18, getter);
                read_ptr(vtable + 0x20, setter);
            }
        }
        std::uintptr_t getset_ref = 0;
        read_ptr(property + kPropertyGetSetRef, getset_ref);
        const std::uintptr_t host = off::host_base();
        const std::uintptr_t getter_rva =
            (getter && host && getter > host) ? (getter - host) : 0;
        const std::uintptr_t setter_rva =
            (setter && host && setter > host) ? (setter - host) : 0;
        zend::logf("env.binary_string: prop=%s getset=%p vtable=%p "
                   "getter=%p setter=%p ref=%p host=%p "
                   "getter_rva=0x%llX setter_rva=0x%llX",
                   name ? name : "?", reinterpret_cast<void*>(getset),
                   reinterpret_cast<void*>(vtable), reinterpret_cast<void*>(getter),
                   reinterpret_cast<void*>(setter),
                   reinterpret_cast<void*>(getset_ref),
                   reinterpret_cast<void*>(host),
                   static_cast<unsigned long long>(getter_rva),
                   static_cast<unsigned long long>(setter_rva));
    }
    // SharedString is a ref-counted object whose std::string payload starts at
    // +0x10; BinaryString stores the std::string directly in the instance.
    // A present field may legitimately hold an empty string.
    std::string value;
    auto read_payload = [&](std::uintptr_t field_offset, bool shared,
                            std::string& out) {
        out.clear();
        if (shared) {
            std::uintptr_t object = 0;
            if (read_ptr(instance + field_offset, object) &&
                is_user_ptr(object) && read_msvc_string(object + 0x10, out)) {
                return true;
            }
        }
        return read_msvc_string(instance + field_offset, out);
    };

    std::uint32_t offset = 0;
    if (!read(property + kPropertyInstanceOffset, offset) || offset == 0 ||
        offset == 0xFFFFFFFFu) {
        if (env_trace()) {
            zend::logf("env.binary_string: prop=%s no offset (raw=0x%X)",
                       name ? name : "?", static_cast<unsigned>(offset));
        }
        offset = 0;
    }

    bool ok = offset != 0 && read_payload(offset, shared_string, value);

    // AeroMeshData has no instance offset in its descriptor. PartOperation
    // stores a generated mesh in the dumped mesh slots; MeshPart and the other
    // TriangleMeshPart classes compute the value from their mesh asset and do
    // not have those slots. Reading PartOperation offsets on a MeshPart picks
    // up unrelated strings (including the bridge error buffer), so only scan
    // the slots on classes that actually own them.
    if (!ok && name && std::strcmp(name, "AeroMeshData") == 0) {
        const std::string class_name = instance_class_name(L, index);
        const bool part_operation =
            class_name == "PartOperation" || class_name == "UnionOperation" ||
            class_name == "IntersectOperation" ||
            class_name == "NegateOperation";
        if (!part_operation) {
            value.clear();
            ok = true;
            if (env_trace()) {
                zend::logf("env.binary_string: AeroMeshData %s empty",
                           class_name.c_str());
            }
        } else {
            constexpr std::uintptr_t kCandidates[] = {0x350, 0x388, 0x318, 0x378};
            bool saw_field = false;
            for (const std::uintptr_t candidate : kCandidates) {
                std::string current;
                std::uintptr_t object = 0;
                if (read_ptr(instance + candidate, object) && is_user_ptr(object)) {
                    saw_field = true;
                    for (const std::uintptr_t inner :
                         {0x10, 0x18, 0x0, 0x8, 0x20}) {
                        std::string candidate_value;
                        if (!read_msvc_string(object + inner, candidate_value)) {
                            continue;
                        }
                        if (env_trace()) {
                            zend::logf("env.binary_string: AeroMeshData +0x%X"
                                       " inner=0x%X len=%zu",
                                       static_cast<unsigned>(candidate),
                                       static_cast<unsigned>(inner),
                                       candidate_value.size());
                        }
                        if (candidate_value.size() > current.size()) {
                            current = std::move(candidate_value);
                        }
                    }
                }
                std::string direct;
                if (read_msvc_string(instance + candidate, direct)) {
                    saw_field = true;
                    if (direct.size() > current.size()) {
                        current = std::move(direct);
                    }
                }
                if (current.size() > value.size()) {
                    value = std::move(current);
                }
                if (!value.empty()) {
                    if (env_trace()) {
                        zend::logf("env.binary_string: AeroMeshData +0x%X len=%zu",
                                   static_cast<unsigned>(candidate), value.size());
                    }
                    ok = true;
                    break;
                }
            }

            // A union with no generated mesh exposes an empty SharedString; the
            // field exists, so return the empty string instead of nil.
            if (!ok && saw_field) {
                value.clear();
                ok = true;
                if (env_trace()) {
                    zend::logf("env.binary_string: AeroMeshData empty");
                }
            }
        }
    }

    if (env_trace()) {
        zend::logf("env.binary_string: prop=%s shared=%d offset=0x%X ok=%d "
                   "len=%zu",
                   name ? name : "?", shared_string ? 1 : 0,
                   static_cast<unsigned>(offset), ok ? 1 : 0, value.size());
    }
    if (!ok) {
        return false;
    }
    lua_pushlstring(L, value.data(), value.size());
    return true;
}

int gethiddenproperty(lua_State* L) {
    check_instance(L, 1);
    const char* name = luaL_checkstring(L, 2);

    // `siz` is the historical hidden alias for Size that several executors
    // expose; the reference API keeps it working for compatibility.
    if (std::strcmp(name, "siz") == 0) {
        lua_getfield(L, 1, "Size");
        lua_pushboolean(L, 1);
        return 2;
    }

    const std::uintptr_t property = resolve_property(L, 1, name);
    if (!property) {
        lua_pushnil(L);
        return 1;
    }

    const bool hidden = property_was_hidden(property);
    const std::uintptr_t instance = instance_raw_pointer(L, 1);

    std::uintptr_t type_address = 0;
    int type_id = -1;
    find_property_type(property, type_address, type_id);

    if (env_trace()) {
        lua_getfield(L, 1, "Name");
        const char* text = lua_tostring(L, -1);
        const std::string instance_label = text ? text : "";
        lua_pop(L, 1);
        zend::logf("env.gethiddenproperty: class=%s name=%s prop=%s "
                   "type=%d hidden=%d",
                   instance_class_name(L, 1).c_str(),
                   instance_label.c_str(), name, type_id, hidden ? 1 : 0);
    }

    if (instance && name && std::strcmp(name, "Tags") == 0 &&
        type_id == kTypeSharedString && push_tags_property(L, 1)) {
        lua_pushboolean(L, hidden ? 1 : 0);
        return 2;
    }

    // Values written through sethiddenproperty for computed string properties
    // are kept here because this build exposes no writable field for them.
    if (instance && name &&
        (type_id == kTypeSharedString || type_id == kTypeBinaryString)) {
        std::string overridden;
        if (get_hidden_string_override(L, 1, name, overridden)) {
            lua_pushlstring(L, overridden.data(), overridden.size());
            lua_pushboolean(L, hidden ? 1 : 0);
            return 2;
        }
    }

    // SharedString values that the engine can convert are returned by the
    // bridge itself; the direct-field probe below stays as a fallback for
    // builds/properties where no Lua converter exists.
    if (type_id == kTypeSharedString && instance && name &&
        std::strcmp(name, "Tags") != 0) {
        std::string engine_value;
        if (read_shared_string_via_engine(L, 1, name, property, engine_value)) {
            if (env_trace()) {
                zend::logf("env.binary_string: prop=%s engine_len=%zu", name,
                           engine_value.size());
            }
            lua_pushlstring(L, engine_value.data(), engine_value.size());
            lua_pushboolean(L, hidden ? 1 : 0);
            return 2;
        }
    }

    if (type_id == kTypeBinaryString || type_id == kTypeSharedString) {
        if (instance &&
            push_binary_string_field(L, 1, instance, property, name,
                                     type_id == kTypeSharedString)) {
            lua_pushboolean(L, hidden ? 1 : 0);
            return 2;
        }
        if (env_trace()) {
            zend::logf("env.gethiddenproperty: no direct field for %s", name);
        }
        // Never rewrite a live binary/shared descriptor type: the engine's
        // converter then treats the payload as a plain string, which has
        // crashed the client on Terrain.MaterialColors.
        if (hidden) {
            lua_pushnil(L);
            lua_pushboolean(L, 1);
            return 2;
        }
    }

    std::uintptr_t flags_address = property + kPropertyBitFlags;
    std::uint32_t original_flags = 0;
    if (!read_u32(flags_address, original_flags)) {
        lua_pushnil(L);
        lua_pushboolean(L, hidden ? 1 : 0);
        return 2;
    }

    const bool flags_changed =
        write<std::uint32_t>(flags_address,
                             original_flags | kPropertyScriptableBit) &&
        [&] {
            std::uint32_t after = 0;
            return read_u32(flags_address, after) &&
                   after == (original_flags | kPropertyScriptableBit);
        }();

    const int top = lua_gettop(L);
    lua_pushcfunction(L, protected_getfield, "gethiddenproperty");
    lua_pushvalue(L, 1);
    lua_pushstring(L, name);
    const int status = lua_pcall(L, 2, 1, 0);

    if (flags_changed) {
        write<std::uint32_t>(flags_address, original_flags);
    }

    if (status != LUA_OK) {
        lua_settop(L, top);
        lua_pushnil(L);
        lua_pushboolean(L, hidden ? 1 : 0);
        return 2;
    }

    if (lua_type(L, top + 1) == LUA_TBUFFER) {
        std::size_t length = 0;
        void* data = lua_tobuffer(L, top + 1, &length);
        const char* text = static_cast<const char*>(data);
        if (!text) {
            text = "";
            length = 0;
        }
        lua_pushlstring(L, text, length);
        lua_replace(L, top + 1);
    }

    lua_pushboolean(L, hidden ? 1 : 0);
    return 2;
}

int sethiddenproperty(lua_State* L) {
    check_instance(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);

    const std::uintptr_t property = resolve_property(L, 1, name);
    if (!property) {
        lua_pushnil(L);
        return 1;
    }

    const bool hidden = property_was_hidden(property);
    std::uintptr_t flags_address = property + kPropertyBitFlags;
    std::uint32_t original_flags = 0;
    if (!read_u32(flags_address, original_flags)) {
        lua_pushnil(L);
        return 1;
    }

    // Never rewrite the descriptor type of a live BinaryString/SharedString:
    // the engine then treats the payload as a plain string, which crashed the
    // client on Terrain.MaterialColors. Writing those values is not supported;
    // the scriptable bit alone covers the regular properties.
    std::uintptr_t type_address = 0;
    int original_type = -1;
    find_property_type(property, type_address, original_type);
    if (env_trace()) {
        zend::logf("env.sethiddenproperty: class=%s prop=%s type=%d hidden=%d "
                   "value=%s",
                   instance_class_name(L, 1).c_str(), name, original_type,
                   hidden ? 1 : 0, luaL_typename(L, 3));
    }
    if (original_type == kTypeSharedString && std::strcmp(name, "Tags") == 0) {
        std::size_t length = 0;
        const char* data = lua_tolstring(L, 3, &length);
        if (!data || !sync_tags_property(L, 1, data, length)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushboolean(L, hidden ? 1 : 0);
        return 1;
    }

    if (original_type == kTypeBinaryString ||
        original_type == kTypeSharedString) {
        std::size_t length = 0;
        const char* data = lua_tolstring(L, 3, &length);
        if (!data) {
            lua_pushnil(L);
            return 1;
        }
        set_hidden_string_override(L, 1, name, data, length);
        lua_pushboolean(L, hidden ? 1 : 0);
        return 1;
    }

    const bool flags_changed =
        hidden && write<std::uint32_t>(
                      flags_address, original_flags | kPropertyScriptableBit);

    const int top = lua_gettop(L);
    lua_pushcfunction(L, protected_setfield, "sethiddenproperty");
    lua_pushvalue(L, 1);
    lua_pushstring(L, name);
    lua_pushvalue(L, 3);
    const int status = lua_pcall(L, 3, 0, 0);

    if (flags_changed) {
        write<std::uint32_t>(flags_address, original_flags);
    }
    lua_settop(L, top);

    if (status != LUA_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, hidden ? 1 : 0);
    return 1;
}

int isscriptable(lua_State* L) {
    check_instance(L, 1);
    const char* name = luaL_checkstring(L, 2);

    bool overridden = false;
    if (lookup_scriptable_override(instance_raw_pointer(L, 1), name,
                                   overridden)) {
        lua_pushboolean(L, overridden ? 1 : 0);
        return 1;
    }

    const std::uintptr_t property = resolve_property(L, 1, name);
    if (!property) {
        lua_pushboolean(L, 0);
        return 1;
    }

    const auto pristine = g_pristine_scriptable.find(property);
    if (pristine != g_pristine_scriptable.end()) {
        lua_pushboolean(L, pristine->second ? 1 : 0);
        return 1;
    }
    lua_pushboolean(L, read_property_scriptable(property) ? 1 : 0);
    return 1;
}

int setscriptable(lua_State* L) {
    check_instance(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);

    const std::uintptr_t property = resolve_property(L, 1, name);
    if (!property) {
        lua_pushboolean(L, 0);
        return 1;
    }

    const std::uintptr_t instance = instance_raw_pointer(L, 1);
    const bool state = lua_toboolean(L, 3) != 0;

    bool previous = false;
    if (!lookup_scriptable_override(instance, name, previous)) {
        const auto pristine = g_pristine_scriptable.find(property);
        previous = pristine != g_pristine_scriptable.end()
                       ? pristine->second
                       : read_property_scriptable(property);
    }

    if (g_pristine_scriptable.find(property) ==
        g_pristine_scriptable.end()) {
        g_pristine_scriptable[property] = read_property_scriptable(property);
    }

    write_property_scriptable(property, state);

    // The scriptable bit is enough for the regular properties. Rewriting the
    // descriptor type of a BinaryString/SharedString made the engine convert
    // the payload as a plain string, which crashed the client on
    // Terrain.MaterialColors, so the type field is left untouched.

    if (instance) {
        g_scriptable_overrides[instance][name] = state;
    }

    lua_pushboolean(L, previous ? 1 : 0);
    return 1;
}

int getcallbackvalue(lua_State* L) {
    check_instance(L, 1);
    const char* name = luaL_checkstring(L, 2);

    const std::uintptr_t descriptor = resolve_property(L, 1, name);
    if (!descriptor) {
        luaL_argerror(L, 2, "callback property does not exist");
    }

    const std::uintptr_t instance = instance_raw_pointer(L, 1);
    if (!instance) {
        lua_pushnil(L);
        return 1;
    }

    std::int32_t relative = 0;
    if (!read(descriptor + 0x88, relative) || relative == 0) {
        lua_pushnil(L);
        return 1;
    }

    const std::uintptr_t holder = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(instance) +
        static_cast<std::intptr_t>(relative));

    std::uintptr_t our_global = 0;
    read_ptr(reinterpret_cast<std::uintptr_t>(L) + off::kLuaGlobal, our_global);

    auto same_vm = [&](std::uintptr_t live_ref) -> bool {
        std::uintptr_t thread = 0;
        std::uintptr_t callback_global = 0;
        if (!read_ptr(live_ref + 0x28, thread) || !thread) {
            return false;
        }
        if (!read_ptr(thread + off::kLuaGlobal, callback_global) ||
            !callback_global) {
            return false;
        }
        return our_global && callback_global == our_global;
    };

    auto push_live_ref = [&](std::uintptr_t live_ref) -> bool {
        std::uint32_t ref_id = 0;
        if (!read(live_ref + 0x34, ref_id) || ref_id == 0) {
            return false;
        }
        if (!same_vm(live_ref)) {
            return false;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(ref_id));
        if (lua_isfunction(L, -1)) {
            return true;
        }
        lua_pop(L, 1);
        return false;
    };

    const std::uintptr_t bases[] = {
        holder,
        holder >= 0x1F0 ? holder - 0x1F0 : 0,
    };

    for (const std::uintptr_t base : bases) {
        if (!base) {
            continue;
        }
        std::uintptr_t owner = 0;
        std::uintptr_t callback = 0;
        std::uintptr_t live_ref = 0;
        if (!read_ptr(base + 0x18, owner) || !owner) {
            continue;
        }
        if (!read_ptr(owner + 0x10, callback) || !callback) {
            continue;
        }
        if (!read_ptr(callback + 0x10, live_ref) || !live_ref) {
            continue;
        }
        if (push_live_ref(live_ref)) {
            return 1;
        }
    }

    for (const std::uintptr_t base : bases) {
        if (!base) {
            continue;
        }
        std::uintptr_t owner = 0;
        std::uintptr_t callback = 0;
        std::uintptr_t live_ref = 0;
        if (!read_ptr(base + 0x18, owner) || !owner) {
            continue;
        }
        if (!read_ptr(owner + 0x38, callback) || !callback) {
            continue;
        }
        if (!read_ptr(callback + 0x18, live_ref) || !live_ref) {
            continue;
        }
        if (push_live_ref(live_ref)) {
            return 1;
        }
    }

    {
        constexpr std::uintptr_t kFields[] = {0x0, 0x10, 0x28};
        constexpr int kDepth = 5;
        struct Node {
            std::uintptr_t address;
            int depth;
        };
        Node stack[256];
        int count = 0;
        for (const std::uintptr_t base : bases) {
            if (base && count < 256) {
                stack[count++] = {base, 0};
            }
        }

        while (count > 0) {
            const Node node = stack[--count];
            if (node.depth >= kDepth) {
                continue;
            }
            for (const std::uintptr_t field : kFields) {
                std::uintptr_t next = 0;
                if (!read_ptr(node.address + field, next) || !next) {
                    continue;
                }
                if (push_live_ref(next)) {
                    return 1;
                }
                if (count < 256 && node.depth + 1 < kDepth) {
                    stack[count++] = {next, node.depth + 1};
                }
            }
        }
    }

    lua_pushnil(L);
    return 1;
}

// --- instance cache, clones and hidden UI --------------------------------

using PushInstanceFn = void(__fastcall*)(lua_State*, void*);

__declspec(noinline) bool push_instance_raw(lua_State* L,
                                            void* argument) noexcept {
    __try {
        auto push = reinterpret_cast<PushInstanceFn>(
            off::host_base() + off::kPushInstance);
        push(L, argument);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The client keeps one registry table that maps a lightuserdata instance
// pointer to that instance's canonical userdata. Finding it once lets
// cache.* and cloneref operate on the same table the engine uses, instead of
// a parallel copy that would never be consulted.
struct InstanceCache {
    void* key = nullptr;
    void* global = nullptr;
};

InstanceCache g_instance_cache;

bool table_is_instance_cache(lua_State* L, int index) {
    index = lua_absindex(L, index);
    int checked = 0;
    int matches = 0;

    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        ++checked;

        if (lua_type(L, -2) == LUA_TLIGHTUSERDATA &&
            lua_isuserdata(L, -1) &&
            std::strcmp(luaL_typename(L, -1), "Instance") == 0) {
            void* block = lua_touserdata(L, -1);
            std::uintptr_t raw = 0;
            if (block &&
                read_ptr(reinterpret_cast<std::uintptr_t>(block), raw) &&
                raw == reinterpret_cast<std::uintptr_t>(
                           lua_tolightuserdata(L, -2))) {
                ++matches;
            }
        }

        lua_pop(L, 1);
        if (checked >= 16) {
            break;
        }
    }

    if (checked >= 16) {
        lua_pop(L, 1); // the key lua_next left behind when we broke out
    }
    return matches > 0 && matches == checked;
}

bool push_instance_cache(lua_State* L) {
    if (g_instance_cache.global == L->global && g_instance_cache.key) {
        lua_pushlightuserdata(L, g_instance_cache.key);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_istable(L, -1)) {
            return true;
        }
        lua_pop(L, 1);
    }

    // The engine's canonical cache is the registry table whose entry for the
    // raw `game` pointer is the very userdata scripts hold. Other tables can
    // look similar (lightuserdata -> Instance), but clearing them has no effect
    // on PushInstance.
    const int top = lua_gettop(L);
    lua_getglobal(L, "game");
    const bool have_game =
        lua_isuserdata(L, -1) &&
        std::strcmp(luaL_typename(L, -1), "Instance") == 0;
    void** game_slot =
        have_game ? static_cast<void**>(lua_touserdata(L, -1)) : nullptr;
    void* game_raw = game_slot ? *game_slot : nullptr;

    if (game_raw) {
        const int game_index = lua_gettop(L);
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        const int registry = lua_gettop(L);

        lua_pushnil(L);
        while (lua_next(L, registry) != 0) {
            if (lua_type(L, -2) == LUA_TLIGHTUSERDATA &&
                lua_istable(L, -1)) {
                lua_pushlightuserdata(L, game_raw);
                lua_rawget(L, -2);
                const bool hit =
                    lua_isuserdata(L, -1) && lua_rawequal(L, -1, game_index);
                lua_pop(L, 1);
                if (hit) {
                    const void* key = lua_tolightuserdata(L, -2);
                    g_instance_cache.key = const_cast<void*>(key);
                    g_instance_cache.global = L->global;

                    lua_remove(L, -2); // the key, leaving registry + table
                    lua_remove(L, registry);
                    lua_remove(L, game_index);
                    return true;
                }
            }
            lua_pop(L, 1);
        }

        lua_settop(L, top);
    } else {
        lua_settop(L, top);
    }

    lua_pushvalue(L, LUA_REGISTRYINDEX);
    const int registry = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, registry) != 0) {
        if (lua_type(L, -2) == LUA_TLIGHTUSERDATA &&
            lua_istable(L, -1) &&
            table_is_instance_cache(L, lua_gettop(L))) {
            const void* key = lua_tolightuserdata(L, -2);
            g_instance_cache.key = const_cast<void*>(key);
            g_instance_cache.global = L->global;

            lua_remove(L, registry);
            lua_remove(L, -2); // the key, leaving the cache table on top
            return true;
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // registry
    return false;
}

// Pushes the engine's canonical userdata for `raw`, if the instance cache still
// holds one. This never creates a userdata: PushInstance takes a real
// std::shared_ptr<Instance>, so a raw pointer cannot be turned into one safely.
bool push_cached_instance(lua_State* L, std::uintptr_t raw) {
    if (!raw) {
        return false;
    }
    const int top = lua_gettop(L);
    if (!push_instance_cache(L)) {
        lua_settop(L, top);
        return false;
    }

    const int cache = lua_gettop(L);
    lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
    lua_rawget(L, cache);

    void** slot = lua_isuserdata(L, -1)
                      ? static_cast<void**>(lua_touserdata(L, -1))
                      : nullptr;
    std::uintptr_t instance = 0;
    if (!slot ||
        std::strcmp(luaL_typename(L, -1), "Instance") != 0 ||
        !read_ptr(reinterpret_cast<std::uintptr_t>(slot), instance) ||
        instance != raw) {
        lua_settop(L, top);
        return false;
    }

    lua_remove(L, cache);
    return true;
}

int cache_invalidate(lua_State* L) {
    const std::uintptr_t raw = instance_value(L, 1);
    if (!push_instance_cache(L)) {
        luaL_error(L, "cache.invalidate: instance cache unavailable");
    }

    lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
    lua_pushnil(L);
    lua_rawset(L, -3);
    return 0;
}

int cache_iscached(lua_State* L) {
    const std::uintptr_t raw = instance_value(L, 1);
    if (!push_instance_cache(L)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
    lua_rawget(L, -2);
    lua_pushboolean(L, lua_rawequal(L, -1, 1));
    return 1;
}

int cache_replace(lua_State* L) {
    const std::uintptr_t raw = instance_value(L, 1);
    instance_value(L, 2);
    if (!push_instance_cache(L)) {
        luaL_error(L, "cache.replace: instance cache unavailable");
    }

    lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    return 0;
}

int cloneref(lua_State* L) {
    const std::uintptr_t raw = instance_value(L, 1);
    void* block = lua_touserdata(L, 1);
    if (!raw || !block) {
        lua_pushvalue(L, 1);
        return 1;
    }

    // The engine consults its instance cache before creating a userdata, so a
    // clone requires removing the canonical entry first. The entry is restored
    // afterwards; the clone itself is left uncached, exactly like the
    // reference implementation.
    int cache_index = 0;
    int saved_index = 0;
    if (push_instance_cache(L)) {
        cache_index = lua_gettop(L);
        lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
        lua_rawget(L, cache_index);
        saved_index = lua_gettop(L);
        lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
        lua_pushnil(L);
        lua_rawset(L, cache_index);
    }

    lua_getmetatable(L, 1);
    const int metatable_index = lua_gettop(L);
    const int before_push = lua_gettop(L);

    bool pushed = false;
    // Only the source userdata block is a real shared_ptr<Instance>. Handing
    // PushInstance a raw pointer makes it adopt the adjacent stack word as the
    // control block, which faults the GC later.
    if (block && push_instance_raw(L, block) &&
        lua_gettop(L) == before_push + 1 && lua_isuserdata(L, -1) &&
        !lua_rawequal(L, -1, 1)) {
        void** slot = static_cast<void**>(lua_touserdata(L, -1));
        std::uintptr_t instance = 0;
        if (slot &&
            read_ptr(reinterpret_cast<std::uintptr_t>(slot), instance) &&
            instance == raw) {
            pushed = true;
        }
    }
    if (!pushed) {
        lua_settop(L, before_push);
    }
    const int clone_index = lua_gettop(L);

    if (pushed && !lua_isnil(L, metatable_index)) {
        lua_pushvalue(L, metatable_index);
        lua_setmetatable(L, clone_index);
    }

    if (cache_index) {
        lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
        lua_pushvalue(L, saved_index);
        lua_rawset(L, cache_index);
    }

    bool usable = pushed && clone_index > before_push;
    if (usable) {
        void** slot = static_cast<void**>(lua_touserdata(L, clone_index));
        std::uintptr_t instance = 0;
        usable = slot &&
                 read_ptr(reinterpret_cast<std::uintptr_t>(slot), instance) &&
                 instance == raw && !lua_rawequal(L, clone_index, 1);
    }

    if (!usable) {
        lua_settop(L, 1);
        lua_pushvalue(L, 1);
        return 1;
    }

    lua_pushvalue(L, clone_index);
    lua_replace(L, 1);
    lua_settop(L, 1);
    return 1;
}

int compareinstances(lua_State* L) {
    const std::uintptr_t left = instance_value(L, 1);
    const std::uintptr_t right = instance_value(L, 2);
    lua_pushboolean(L, left == right);
    return 1;
}

// A stable registry key for gethui's cached container. The reference uses a
// string key, which cannot collide with lightuserdata keys used elsewhere.
constexpr const char* kHuiRegistryKey = "__zen_hui";

// Engine property reads (game.CoreGui in particular) are still gated by the
// thread's identity, but gethui has to return a usable container even after a
// script lowers its identity. Elevate only for the duration of the lookup and
// restore the caller's mask afterwards.
struct ScopedFullAccess {
    RobloxExtraSpace* space = nullptr;
    std::uint32_t identity = 0;
    std::uint64_t capabilities = 0;

    explicit ScopedFullAccess(lua_State* L) : space(L ? L->userdata : nullptr) {
        if (space) {
            identity = space->identity;
            capabilities = space->capabilities;
            space->identity = 8;
            space->capabilities = ~std::uint64_t{0};
        }
    }

    ~ScopedFullAccess() {
        if (space) {
            space->identity = identity;
            space->capabilities = capabilities;
        }
    }
};

int gethui(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHuiRegistryKey);
    if (lua_isuserdata(L, -1) &&
        std::strcmp(luaL_typename(L, -1), "Instance") == 0) {
        return 1;
    }
    lua_pop(L, 1);

    const int top = lua_gettop(L);
    ScopedFullAccess access(L);
    lua_getglobal(L, "game");
    if (!lua_isuserdata(L, -1)) {
        lua_settop(L, top);
        lua_pushnil(L);
        return 1;
    }

    lua_getfield(L, -1, "CoreGui");
    if (!lua_isuserdata(L, -1)) {
        lua_settop(L, top);
        lua_pushnil(L);
        return 1;
    }

    const int coregui = lua_gettop(L);
    lua_pushcclosure(L, cloneref, "gethui.cloneref", 0);
    lua_pushvalue(L, coregui);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isuserdata(L, -1)) {
        lua_replace(L, coregui);
    } else {
        lua_settop(L, coregui);
    }

    lua_settop(L, coregui);
    lua_pushvalue(L, coregui);
    lua_setfield(L, LUA_REGISTRYINDEX, kHuiRegistryKey);
    return 1;
}

// The reference fires the Lua-facing signal in both directions after the
// engine touch update, so executor connections see the event even when the
// physics step is not running.
void dispatch_touch_signal(lua_State* L, int owner_index, int other_index,
                           const char* name) {
    owner_index = lua_absindex(L, owner_index);
    other_index = lua_absindex(L, other_index);

    const int top = lua_gettop(L);
    lua_getfield(L, owner_index, name);
    if (lua_isnil(L, -1)) {
        lua_settop(L, top);
        return;
    }

    // firesignal expects the signal at index 1, so hand it a fresh thread whose
    // base holds the signal and the other part. The thread stays on our stack
    // for the duration of the call so the GC cannot collect it.
    lua_State* thread = lua_newthread(L);
    lua_pushvalue(L, top + 1);
    lua_xmove(L, thread, 1);
    lua_pushvalue(L, other_index);
    lua_xmove(L, thread, 1);
    firesignal(thread);
    lua_settop(L, top);
}

int firetouchinterest(lua_State* L) {
    const std::uintptr_t part = instance_value(L, 1);
    const std::uintptr_t other = instance_value(L, 2);
    if (!instance_is_a(L, 1, "BasePart")) {
        luaL_argerror(L, 1, "Expected a BasePart");
    }
    if (!instance_is_a(L, 2, "BasePart")) {
        luaL_argerror(L, 2, "Expected a BasePart");
    }

    // sUNC maps true/0 to touch and false/1 to un-touch; nil starts a touch.
    const bool end_touch =
        lua_isboolean(L, 3)
            ? !lua_toboolean(L, 3)
            : (lua_isnumber(L, 3) ? lua_tonumber(L, 3) != 0 : false);

    std::uintptr_t primitive1 = 0;
    std::uintptr_t primitive2 = 0;
    std::uintptr_t overlap = 0;
    bool engine_fired = false;
    if (read_ptr(part + off::kBasePartPrimitive, primitive1) &&
        read_ptr(other + off::kBasePartPrimitive, primitive2) &&
        is_user_ptr(primitive1) && is_user_ptr(primitive2) &&
        read_ptr(primitive1 + off::kPrimitiveOverlap, overlap) &&
        is_user_ptr(overlap)) {
        using FireTouchInterestFn = void(__fastcall*)(
            std::uintptr_t, std::uintptr_t, std::uintptr_t, bool, bool);
        auto fire = reinterpret_cast<FireTouchInterestFn>(
            off::host_base() + off::kFireTouchInterest);
        fire(overlap, primitive1, primitive2, end_touch, true);
        engine_fired = true;
    }

    // The engine call does not deliver Touched to Lua connections, so dispatch
    // that side ourselves. TouchEnded is delivered by the engine, and adding a
    // manual dispatch there would fire every connection twice.
    if (!end_touch) {
        dispatch_touch_signal(L, 1, 2, "Touched");
        dispatch_touch_signal(L, 2, 1, "Touched");
    } else if (!engine_fired) {
        dispatch_touch_signal(L, 1, 2, "TouchEnded");
        dispatch_touch_signal(L, 2, 1, "TouchEnded");
    }
    return 0;
}

int fireproximityprompt(lua_State* L) {
    const std::uintptr_t prompt = instance_value(L, 1);
    if (!instance_is_a(L, 1, "ProximityPrompt")) {
        luaL_argerror(L, 1, "Expected a ProximityPrompt");
    }

    using FireProximityPromptFn = void(__thiscall*)(std::uintptr_t);
    auto fire = reinterpret_cast<FireProximityPromptFn>(
        off::host_base() + off::kFireProximityPrompt);
    fire(prompt);
    return 0;
}

__declspec(noinline) bool call_click_dispatcher(std::uintptr_t fn,
                                                std::uintptr_t self,
                                                float distance,
                                                std::uintptr_t who) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, float,
                                           std::uintptr_t)>(fn)(self, distance,
                                                                who);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool call_hover_dispatcher(std::uintptr_t fn,
                                                std::uintptr_t self,
                                                std::uintptr_t who) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, std::uintptr_t)>(fn)(
            self, who);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t local_player_value(lua_State* L) {
    const int top = lua_gettop(L);
    std::uintptr_t result = 0;

    lua_getglobal(L, "game");
    if (lua_isuserdata(L, -1)) {
        lua_getfield(L, -1, "Players");
        if (lua_isuserdata(L, -1)) {
            lua_getfield(L, -1, "LocalPlayer");
            if (lua_isuserdata(L, -1)) {
                void* block = lua_touserdata(L, -1);
                std::uintptr_t raw = 0;
                if (block &&
                    read_ptr(reinterpret_cast<std::uintptr_t>(block), raw)) {
                    result = raw;
                }
            }
        }
    }

    lua_settop(L, top);
    return result;
}

int fireclickdetector(lua_State* L) {
    const std::uintptr_t detector = instance_value(L, 1);
    if (!instance_is_a(L, 1, "ClickDetector")) {
        luaL_argerror(L, 1, "Expected a ClickDetector");
    }

    const float distance =
        static_cast<float>(luaL_optnumber(L, 2, 0.0));
    const char* event = luaL_optstring(L, 3, "MouseClick");

    std::uintptr_t who = 0;
    if (!lua_isnoneornil(L, 4)) {
        who = instance_value(L, 4);
    } else {
        who = local_player_value(L);
    }

    std::uintptr_t fn = 0;
    bool hover = false;
    if (_stricmp(event, "MouseClick") == 0) {
        fn = off::host_base() + off::kFireLeftMouseClick;
    } else if (_stricmp(event, "RightMouseClick") == 0) {
        fn = off::host_base() + off::kFireRightMouseClick;
    } else if (_stricmp(event, "MouseHoverEnter") == 0) {
        fn = off::host_base() + off::kFireMouseHoverEnter;
        hover = true;
    } else if (_stricmp(event, "MouseHoverLeave") == 0) {
        fn = off::host_base() + off::kFireMouseHoverLeave;
        hover = true;
    } else {
        return 0;
    }

    const bool ok = hover ? call_hover_dispatcher(fn, detector, who)
                          : call_click_dispatcher(fn, detector, distance, who);
    if (!ok) {
        luaL_error(L, "fireclickdetector: the engine call faulted");
    }
    return 0;
}

// --- signal arguments and hooksignal -------------------------------------

// Mirrors the client's reflection type enum (Variant type ids), not the older
// public list: Bool is 1, Int is 2, Instance is 8, Tuple is 0x23, and so on.
const char* signal_type_name(int type) {
    switch (type) {
    case 0x00: return "Void";
    case 0x01: return "Bool";
    case 0x02: return "Int";
    case 0x03: return "Int64";
    case 0x04: return "Float";
    case 0x05: return "Double";
    case 0x06: return "String";
    case 0x07: return "ProtectedString";
    case 0x08: return "Instance";
    case 0x09: return "Instances";
    case 0x0A: return "Ray";
    case 0x0B: return "Vector2";
    case 0x0C: return "Vector3";
    case 0x0D: return "Vector2Int16";
    case 0x0E: return "Vector3Int16";
    case 0x0F: return "Rect2d";
    case 0x10: return "CoordinateFrame";
    case 0x11: return "Color3";
    case 0x12: return "Color3uint8";
    case 0x13: return "UDim";
    case 0x14: return "UDim2";
    case 0x15: return "Faces";
    case 0x16: return "Axes";
    case 0x17: return "Region3";
    case 0x18: return "Region3Int16";
    case 0x19: return "CellId";
    case 0x1A: return "GuidData";
    case 0x1B: return "PhysicalProperties";
    case 0x1C: return "BrickColor";
    case 0x1D: return "SystemAddress";
    case 0x1E: return "BinaryString";
    case 0x1F: return "Surface";
    case 0x20: return "CollectionHandle";
    case 0x21: return "Enum";
    case 0x22: return "Property";
    case 0x23: return "Tuple";
    case 0x24: return "ValueArray";
    case 0x25: return "ValueTable";
    case 0x26: return "ValueMap";
    case 0x27: return "Variant";
    case 0x28: return "GenericFunction";
    case 0x29: return "WeakFunctionRef";
    case 0x2A: return "ColorSequence";
    case 0x2B: return "ColorSequenceKeypoint";
    case 0x2C: return "NumberRange";
    case 0x2D: return "NumberSequence";
    case 0x2E: return "NumberSequenceKeypoint";
    case 0x2F: return "InputObject";
    case 0x30: return "Connection";
    case 0x31: return "ContentId";
    case 0x32: return "DescribedBase";
    case 0x33: return "RefType";
    case 0x34: return "QFont";
    case 0x35: return "QDir";
    case 0x36: return "EventInstance";
    case 0x37: return "TweenInfo";
    case 0x38: return "DockWidgetPluginGuiInfo";
    case 0x39: return "PluginDrag";
    case 0x3A: return "Random";
    case 0x3B: return "PathWaypoint";
    case 0x3C: return "FloatCurveKey";
    case 0x3D: return "RotationCurveKey";
    case 0x3E: return "ValueCurveKey";
    case 0x3F: return "SharedString";
    case 0x40: return "DateTime";
    case 0x41: return "RaycastParams";
    case 0x42: return "RaycastResult";
    case 0x43: return "OverlapParams";
    case 0x44: return "LazyTable";
    case 0x45: return "DebugTable";
    case 0x46: return "CatalogSearchParams";
    case 0x47: return "OptionalCoordinateFrame";
    case 0x48: return "CSGPropertyData";
    case 0x49: return "UniqueId";
    case 0x4A: return "Font";
    case 0x4B: return "SharedTable";
    case 0x4C: return "SharedTableIterator";
    case 0x4D: return "AnimationMask";
    case 0x4E: return "AnimationPose";
    case 0x4F: return "ClipEvaluator";
    case 0x50: return "OpenCloudModel";
    case 0x51: return "InstanceRef";
    case 0x52: return "SecurityCapabilities";
    case 0x53: return "ArticulatedJoint";
    case 0x54: return "AnimationContext";
    case 0x55: return "Secret";
    case 0x56: return "Buffer";
    case 0x57: return "Integer";
    case 0x58: return "Path2DControlPoint";
    case 0x59: return "ReplicationPV";
    case 0x5A: return "FacsReplicationData";
    case 0x5B: return "AnimationMaskModifier";
    case 0x5C: return "Content";
    case 0x5D: return "NetAssetHandle";
    case 0x5E: return "NetAssetRef";
    case 0x5F: return "Object";
    case 0x60: return "AdReward";
    case 0x61: return "AssetContentMap";
    case 0x62: return "SlimReplicationData";
    case 0x63: return "User";
    case 0x64: return "WebViewParams";
    case 0x65: return "AnimTrackPlayState";
    case 0x66: return "AnimTrackMetadata";
    case 0x67: return "AnimTrackWeight";
    case 0x68: return "ScopedInstanceIdentity";
    default: return "Unknown";
    }
}

// RBXScriptSignal userdata holds the event instance at +0; the instance holds
// the descriptor, and the descriptor holds [signature begin, signature end).
bool signal_signature(lua_State* L, int index,
                      std::uintptr_t& signature,
                      std::uintptr_t& signature_end) {
    const std::uintptr_t payload =
        reinterpret_cast<std::uintptr_t>(lua_touserdata(L, index));
    std::uintptr_t event_instance = 0;
    std::uintptr_t descriptor = 0;
    if (!is_user_ptr(payload) || !read_ptr(payload, event_instance) ||
        !is_user_ptr(event_instance) || !read_ptr(event_instance, descriptor) ||
        !is_user_ptr(descriptor) ||
        !read_ptr(descriptor + off::kSignalSignature, signature) ||
        !is_user_ptr(signature) ||
        !read_ptr(descriptor + off::kSignalSignatureEnd, signature_end)) {
        return false;
    }
    return signature_end >= signature;
}

// std::string layout used by the client: data pointer at +0, size at +0x10,
// capacity at +0x18, with the buffer inline when capacity < 16.
bool read_engine_string(std::uintptr_t object, char* out, std::size_t size) {
    if (!is_user_ptr(object) || size < 2) {
        return false;
    }
    std::uint64_t length = 0;
    std::uint64_t capacity = 0;
    if (!read_u64(object + 0x10, length) || !read_u64(object + 0x18, capacity) ||
        length == 0 || length >= size || capacity < length) {
        return false;
    }
    std::uintptr_t data = object;
    if (capacity >= 16 &&
        (!read_ptr(object, data) || !is_user_ptr(data))) {
        return false;
    }
    if (!try_read_bytes(data, out, static_cast<std::size_t>(length))) {
        return false;
    }
    out[length] = '\0';
    return out[0] != '\0';
}

bool looks_like_ident(const char* text) {
    if (!text || !text[0]) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(text[0]);
    if (!std::isalpha(first) && first != '_') {
        return false;
    }
    for (const char* cursor = text + 1; *cursor; ++cursor) {
        const unsigned char value = static_cast<unsigned char>(*cursor);
        if (!std::isalnum(value) && value != '_') {
            return false;
        }
    }
    return true;
}

// A signal signature is an array of Parameters, 0x70 bytes each:
//   +0x00 const std::string* Name
//   +0x08 Reflection::Type*  (the Variant type id lives at +0x30)
//   +0x18 const ClassDescriptor* Class
// The array starts at descriptor + 0x60 and ends at descriptor + 0x68.
bool read_parameter_name(std::uintptr_t parameter, char* name, std::size_t size) {
    std::uintptr_t text = 0;
    if (!read_ptr(parameter + off::kSignalParameterName, text) ||
        !is_user_ptr(text)) {
        return false;
    }
    return read_engine_string(text, name, size) && looks_like_ident(name);
}

int getsignalarguments(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    lua_newtable(L);
    const int result = lua_gettop(L);

    std::uintptr_t signature = 0;
    std::uintptr_t signature_end = 0;
    if (!signal_signature(L, 1, signature, signature_end)) {
        return 1;
    }

    int count = 0;
    for (std::uintptr_t parameter = signature;
         parameter + off::kSignalTypeStride <= signature_end && count < 256;
         parameter += off::kSignalTypeStride) {
        std::uintptr_t type = 0;
        std::uint32_t identifier = 0;
        if (!read_ptr(parameter + off::kSignalParameterType, type) ||
            !is_user_ptr(type) ||
            !read_u32(type + off::kSignalTypeId, identifier)) {
            break;
        }
        lua_pushstring(L, signal_type_name(static_cast<int>(identifier)));
        lua_rawseti(L, result, ++count);
    }
    return 1;
}

int getsignalargumentsinfo(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    lua_newtable(L);
    const int result = lua_gettop(L);

    std::uintptr_t signature = 0;
    std::uintptr_t signature_end = 0;
    if (!signal_signature(L, 1, signature, signature_end)) {
        return 1;
    }

    int count = 0;
    for (std::uintptr_t parameter = signature;
         parameter + off::kSignalTypeStride <= signature_end && count < 256;
         parameter += off::kSignalTypeStride) {
        std::uintptr_t type = 0;
        std::uint32_t identifier = 0;
        if (!read_ptr(parameter + off::kSignalParameterType, type) ||
            !is_user_ptr(type) ||
            !read_u32(type + off::kSignalTypeId, identifier)) {
            break;
        }

        char name[256] = {};
        if (!read_parameter_name(parameter, name, sizeof(name))) {
            name[0] = '\0';
        }

        lua_newtable(L);
        lua_pushstring(L, name[0] ? name : "Unknown");
        lua_setfield(L, -2, "Name");
        lua_pushstring(L, signal_type_name(static_cast<int>(identifier)));
        lua_setfield(L, -2, "Type");
        lua_rawseti(L, result, ++count);
    }
    return 1;
}

// --- replicatesignal ------------------------------------------------------
//
// The event instance knows its descriptor (parameter signature) and its owner
// (the RemoteEvent-like engine object). The owner's vtable slot +0x18 raises
// the event to the server; the arguments travel as Variants produced by the
// client's own CastArgs, so instances, enums and vectors keep their real
// representation instead of a hand-built guess.

struct ReplicateVariant {
    struct Storage {
        const void* holder;
        char data[64];
    };
    const void* type;
    Storage value;
};

static_assert(sizeof(ReplicateVariant) == 80,
              "Variant layout must match the client's argument struct");

enum class EventTargetInclusion : std::int32_t {
    only_target = 0,
    exclude_target = 1,
};

struct RemoteEventInvocationTargetOptions {
    const void* target;
    EventTargetInclusion is_exclude_target;
};

using CastArgsFn =
    void(__fastcall*)(lua_State*, int, ReplicateVariant*, bool, int);
using RaiseSignalFn =
    void(__fastcall*)(std::uintptr_t*, std::uintptr_t*,
                      std::vector<ReplicateVariant>*,
                      RemoteEventInvocationTargetOptions*);

__declspec(noinline) bool cast_signal_argument(lua_State* L, int index,
                                               ReplicateVariant* out) {
    __try {
        auto cast =
            reinterpret_cast<CastArgsFn>(off::host_base() + off::kCastArgs);
        cast(L, index, out, true, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool raise_event_signal(
    std::uintptr_t owner, std::uintptr_t descriptor,
    std::vector<ReplicateVariant>& values) {
    std::uintptr_t vtable = 0;
    if (!read_ptr(owner, vtable) || !is_user_ptr(vtable)) {
        return false;
    }
    std::uintptr_t raise = 0;
    if (!read_ptr(vtable + 0x18, raise) || !is_user_ptr(raise)) {
        return false;
    }

    RemoteEventInvocationTargetOptions options{};
    options.target = nullptr;
    options.is_exclude_target = EventTargetInclusion::only_target;

    __try {
        auto call = reinterpret_cast<RaiseSignalFn>(raise);
        call(reinterpret_cast<std::uintptr_t*>(owner),
             reinterpret_cast<std::uintptr_t*>(descriptor), &values, &options);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int replicatesignal(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    const std::uintptr_t payload =
        reinterpret_cast<std::uintptr_t>(lua_touserdata(L, 1));
    std::uintptr_t event_instance = 0;
    std::uintptr_t descriptor = 0;
    if (!is_user_ptr(payload) || !read_ptr(payload, event_instance) ||
        !is_user_ptr(event_instance) ||
        !read_ptr(event_instance, descriptor) || !is_user_ptr(descriptor)) {
        luaL_error(L, "replicatesignal: invalid signal");
    }

    std::vector<std::uintptr_t> parameters;
    std::uintptr_t signature = 0;
    std::uintptr_t signature_end = 0;
    if (signal_signature(L, 1, signature, signature_end)) {
        for (std::uintptr_t parameter = signature;
             parameter + off::kSignalTypeStride <= signature_end &&
             parameters.size() < 256;
             parameter += off::kSignalTypeStride) {
            std::uintptr_t type = 0;
            std::uint32_t identifier = 0;
            if (!read_ptr(parameter + off::kSignalParameterType, type) ||
                !is_user_ptr(type) ||
                !read_u32(type + off::kSignalTypeId, identifier)) {
                parameters.clear();
                break;
            }
            parameters.push_back(type);
        }
    }

    const int provided = lua_gettop(L) - 1;
    if (provided != static_cast<int>(parameters.size())) {
        luaL_error(L, "replicatesignal: expected %d argument(s), got %d",
                   static_cast<int>(parameters.size()), provided);
    }

    std::vector<ReplicateVariant> values;
    values.reserve(parameters.size());

    int argument = 2;
    for (const std::uintptr_t wanted : parameters) {
        ReplicateVariant value{};
        if (!cast_signal_argument(L, argument, &value)) {
            luaL_error(L, "replicatesignal: could not convert argument %d",
                       argument - 1);
        }

        if (!value.type) {
            ++argument;
            continue;
        }

        std::uint32_t wanted_id = 0;
        std::uint32_t actual_id = 0;
        read_u32(wanted + off::kSignalTypeId, wanted_id);
        read_u32(reinterpret_cast<std::uintptr_t>(value.type) +
                     off::kSignalTypeId,
                 actual_id);

        if (wanted_id != actual_id) {
            // Numbers arrive as doubles; the engine expects the exact wire
            // width of the parameter type. Only numeric conversions are safe
            // to retag without a type-specific constructor.
            char* data = const_cast<char*>(value.value.data);
            switch (wanted_id) {
                case 2: {
                    const std::int32_t number =
                        static_cast<std::int32_t>(lua_tonumber(L, argument));
                    std::memcpy(data, &number, sizeof(number));
                    value.type = reinterpret_cast<const void*>(wanted);
                    break;
                }
                case 3: {
                    const std::int64_t number = static_cast<std::int64_t>(
                        std::llround(lua_tonumber(L, argument)));
                    std::memcpy(data, &number, sizeof(number));
                    value.type = reinterpret_cast<const void*>(wanted);
                    break;
                }
                case 4: {
                    const float number =
                        static_cast<float>(lua_tonumber(L, argument));
                    std::memcpy(data, &number, sizeof(number));
                    value.type = reinterpret_cast<const void*>(wanted);
                    break;
                }
                default:
                    break;
            }
        }

        values.push_back(value);
        ++argument;
    }

    std::uintptr_t owner = 0;
    if (!read_ptr(event_instance + 0x18, owner) || !is_user_ptr(owner)) {
        luaL_error(L, "replicatesignal: invalid signal owner");
    }
    if (!raise_event_signal(owner, descriptor, values)) {
        luaL_error(L, "replicatesignal: signal could not be raised");
    }
    return 0;
}

// hooksignal installs a Lua wrapper into every live Luau connection's slot.
// The engine then dispatches the wrapper itself, so yields inside the hook
// keep working and other instances of the same signal class stay untouched.
struct SignalHookEntry {
    std::uintptr_t connection = 0;
    std::uintptr_t weak_object = 0;
    int original_ref = LUA_NOREF;
    int wrapper_ref = LUA_NOREF;
};

struct SignalHookRecord {
    int signal_ref = LUA_NOREF;
    int hook_ref = LUA_NOREF;
    std::vector<SignalHookEntry> entries;
};

std::unordered_map<std::uintptr_t, SignalHookRecord> g_signal_hooks;

// The userdata payload differs per fetch, but the internal signal pointer at
// +0x10 is shared, so re-fetching the same signal hits the same record.
std::uintptr_t signal_key(lua_State* L, int index) {
    const std::uintptr_t payload =
        reinterpret_cast<std::uintptr_t>(lua_touserdata(L, index));
    if (!payload) {
        return 0;
    }
    std::uintptr_t inner = 0;
    if (read_ptr(payload + 0x10, inner) && is_user_ptr(inner)) {
        return inner;
    }
    return payload;
}

// The wrapper is compiled from Luau source so the engine calls an L closure:
// the hook may yield, and calling the original function is a plain Lua call.
const char* const kSignalWrapperSource =
    "local captures = ...\n"
    "return function(...)\n"
    "    local info = {\n"
    "        Connection = captures.Connection,\n"
    "        Function = captures.Original,\n"
    "        Index = captures.Index,\n"
    "    }\n"
    "    if captures.Hook(info, ...) == false then\n"
    "        return\n"
    "    end\n"
    "    return captures.Original(...)\n"
    "end\n";

int make_signal_wrapper(lua_State* L, int captures) {
    captures = lua_absindex(L, captures);

    const std::string bytecode = compile(kSignalWrapperSource);
    if (luau_load(L, "=zend-hooksignal", bytecode.data(), bytecode.size(), 0) !=
        0) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }

    Closure* loader = clvalue(luaA_toobject(L, -1));
    executor::apply_capabilities(loader->l.p);

    lua_pushvalue(L, captures);
    lua_call(L, 1, 1);

    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return ref;
}

int hooksignal(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }
    luaL_checktype(L, 2, LUA_TFUNCTION);

    const std::uintptr_t key = signal_key(L, 1);
    if (!key || g_signal_hooks.contains(key)) {
        return 0;
    }

    getconnections(L);
    const int connections = lua_gettop(L);
    if (!lua_istable(L, connections)) {
        lua_settop(L, 0);
        return 0;
    }

    SignalHookRecord record;
    lua_pushvalue(L, 1);
    record.signal_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    record.hook_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    const int total = static_cast<int>(lua_objlen(L, connections));
    for (int index = 1; index <= total; ++index) {
        lua_rawgeti(L, connections, index);
        const int connection_table = lua_gettop(L);
        if (!lua_istable(L, connection_table)) {
            lua_pop(L, 1);
            continue;
        }

        ConnectionRecord* table_record = connection_record(L, connection_table);
        const std::uintptr_t connection =
            table_record && table_record->connection ? table_record->connection
                                                     : 0;
        if (!connection) {
            lua_pop(L, 1);
            continue;
        }

        const std::uintptr_t weak = connection_weak_object(connection);
        lua_State* thread = weak ? connection_thread(weak) : nullptr;
        if (!thread || thread->global != L->global) {
            lua_pop(L, 1);
            continue;
        }

        std::int32_t original_ref = 0;
        if (!read(weak + off::kSlotWrapperFunctionId, original_ref) ||
            original_ref <= 0) {
            lua_pop(L, 1);
            continue;
        }

        Closure* original = connection_function(weak);
        if (!original) {
            lua_pop(L, 1);
            continue;
        }

        lua_createtable(L, 0, 4);
        const int captures = lua_gettop(L);
        lua_pushvalue(L, connection_table);
        lua_setfield(L, captures, "Connection");
        lua_pushvalue(L, 2);
        lua_setfield(L, captures, "Hook");
        push_connection_function(L, original);
        lua_setfield(L, captures, "Original");
        lua_pushinteger(L, index - 1);
        lua_setfield(L, captures, "Index");

        const int wrapper_ref = make_signal_wrapper(L, captures);
        lua_pop(L, 1);
        if (wrapper_ref == LUA_NOREF) {
            lua_pop(L, 1);
            continue;
        }

        if (!write<std::int32_t>(weak + off::kSlotWrapperFunctionId,
                                 wrapper_ref)) {
            lua_unref(L, wrapper_ref);
            lua_pop(L, 1);
            continue;
        }

        SignalHookEntry entry;
        entry.connection = connection;
        entry.weak_object = weak;
        entry.original_ref = original_ref;
        entry.wrapper_ref = wrapper_ref;
        record.entries.push_back(entry);

        HookedConnection hooked;
        hooked.original_ref = original_ref;
        hooked.wrapper_ref = wrapper_ref;
        hooked.original = original;
        g_hooked_connection_functions[connection] = hooked;

        lua_pop(L, 1);
    }

    g_signal_hooks[key] = std::move(record);
    return 0;
}

int restoresignal(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    const std::uintptr_t key = signal_key(L, 1);
    auto it = key ? g_signal_hooks.find(key) : g_signal_hooks.end();
    if (it == g_signal_hooks.end()) {
        return 0;
    }

    SignalHookRecord record = std::move(it->second);
    g_signal_hooks.erase(it);

    for (const SignalHookEntry& entry : record.entries) {
        g_hooked_connection_functions.erase(entry.connection);

        // Only touch the slot when it still holds our wrapper. If the engine
        // already unlinked (and unref'd) the connection, writing the original
        // ref back would revive a dead slot.
        std::int32_t current = 0;
        if (read(entry.weak_object + off::kSlotWrapperFunctionId, current) &&
            current == entry.wrapper_ref) {
            write<std::int32_t>(entry.weak_object + off::kSlotWrapperFunctionId,
                                entry.original_ref);
            lua_unref(L, entry.wrapper_ref);
        }
    }

    if (record.hook_ref != LUA_NOREF) {
        lua_unref(L, record.hook_ref);
    }
    if (record.signal_ref != LUA_NOREF) {
        lua_unref(L, record.signal_ref);
    }
    return 0;
}

int issignalhooked(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (std::strcmp(luaL_typename(L, 1), "RBXScriptSignal") != 0) {
        luaL_typeerror(L, 1, "RBXScriptSignal");
    }

    const std::uintptr_t key = signal_key(L, 1);
    lua_pushboolean(L, key && g_signal_hooks.contains(key));
    return 1;
}

// --- metatable -----------------------------------------------------------

int getrawmetatable(lua_State* L) {
    luaL_checkany(L, 1);
    if (!lua_getmetatable(L, 1)) {
        lua_pushnil(L);
    }
    return 1;
}

int setrawmetatable(lua_State* L) {
    luaL_checkany(L, 1);
    if (!lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
    }

    // Instances rely on __namecall/__newindex from their class metatable. A
    // replacement metatable that omits them would silently break method calls
    // and property writes, so keep the existing entries.
    if (lua_isuserdata(L, 1) && !lua_isnil(L, 2) &&
        lua_getmetatable(L, 1)) {
        static const char* const kPreserved[] = {"__namecall", "__newindex"};
        for (const char* field : kPreserved) {
            lua_rawgetfield(L, 2, field);
            const bool defined = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (defined) {
                continue;
            }
            lua_rawgetfield(L, -1, field);
            if (!lua_isnil(L, -1)) {
                lua_rawsetfield(L, 2, field);
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

int setreadonly(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    lua_setreadonly(L, 1, lua_toboolean(L, 2));
    return 0;
}

int isreadonly(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, lua_getreadonly(L, 1));
    return 1;
}

int make_writeable(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_setreadonly(L, 1, 0);
    return 0;
}

// --- closures ------------------------------------------------------------

int iscclosure(lua_State* L) {
    lua_pushboolean(L, lua_isfunction(L, 1) && lua_iscfunction(L, 1));
    return 1;
}

int islclosure(lua_State* L) {
    lua_pushboolean(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1));
    return 1;
}

int isexecutorclosure(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Closure* closure = clvalue(luaA_toobject(L, 1));

    const bool ours =
        closure &&
        (g_own_closures.contains(closure) ||
         (!closure->isC && executor::owns_proto(closure->l.p)));
    lua_pushboolean(L, ours ? 1 : 0);
    return 1;
}

// --- filtergc ------------------------------------------------------------

bool closure_is_executor_owned(Closure* closure) {
    return closure &&
           (g_own_closures.contains(closure) ||
            (!closure->isC && closure->l.p &&
             executor::owns_proto(closure->l.p)));
}

bool table_filter_has_all(lua_State* L, int candidate, int options,
                          const char* field, int mode) {
    lua_getfield(L, options, field);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    const int wanted = lua_gettop(L);

    bool ok = true;
    lua_pushnil(L);
    while (ok && lua_next(L, wanted) != 0) {
        bool found = false;
        lua_pushnil(L);
        while (!found && lua_next(L, candidate) != 0) {
            if (mode == 0) {
                found = lua_rawequal(L, -2, -3) != 0;
            } else if (mode == 1) {
                found = lua_rawequal(L, -1, -3) != 0;
            } else {
                found = lua_rawequal(L, -2, -4) != 0 &&
                        lua_rawequal(L, -1, -3) != 0;
            }
            lua_pop(L, 1);
            if (found) {
                lua_pop(L, 1);
                break;
            }
        }
        ok = found;
        lua_pop(L, 1); // the wanted value (or the leftover key)
    }
    lua_pop(L, 1); // the wanted table (or the leftover key)
    return ok;
}

bool filter_value_equal(lua_State* L, int left, int right) {
    // NaN is never equal to itself under IEEE comparison, but filtergc callers
    // legitimately use debug.getconstant to obtain NaN and match the same slot.
    if (lua_isnumber(L, left) && lua_isnumber(L, right)) {
        const double a = lua_tonumber(L, left);
        const double b = lua_tonumber(L, right);
        if (std::isnan(a) && std::isnan(b)) {
            return true;
        }
    }
    return lua_rawequal(L, left, right) != 0;
}

bool function_filter_matches(lua_State* L, int candidate, int options,
                             bool ignore_executor) {
    const int base = lua_gettop(L);
    Closure* closure = lua_isfunction(L, candidate)
                           ? clvalue(luaA_toobject(L, candidate))
                           : nullptr;

    if (ignore_executor && closure_is_executor_owned(closure)) {
        return false;
    }

    lua_getfield(L, options, "Name");
    if (lua_isstring(L, -1)) {
        const std::string wanted = lua_tostring(L, -1);
        lua_Debug ar;
        std::memset(&ar, 0, sizeof(ar));
        lua_pushvalue(L, candidate);
        const bool have_info = lua_getinfo(L, -1, "n", &ar) != 0;
        lua_pop(L, 1);
        if (!have_info || !ar.name || wanted != ar.name) {
            lua_settop(L, base);
            return false;
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, options, "Hash");
    if (lua_isstring(L, -1)) {
        const std::string wanted = lua_tostring(L, -1);
        if (!closure || closure->isC || !closure->l.p ||
            hash_function_proto(closure->l.p) != wanted) {
            lua_settop(L, base);
            return false;
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, options, "Constants");
    if (lua_istable(L, -1)) {
        const int wanted = lua_gettop(L);
        if (!closure || closure->isC || !closure->l.p) {
            lua_settop(L, base);
            return false;
        }
        const int count = static_cast<int>(lua_objlen(L, wanted));
        for (int index = 1; index <= count; ++index) {
            lua_rawgeti(L, wanted, index);
            bool hit = false;
            for (int k = 0; k < closure->l.p->sizek && !hit; ++k) {
                luaA_pushvalue(L, &closure->l.p->k[k]);
                hit = filter_value_equal(L, -1, -2);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            if (!hit) {
                lua_settop(L, base);
                return false;
            }
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, options, "Upvalues");
    if (lua_istable(L, -1)) {
        const int wanted = lua_gettop(L);
        bool complete = true;
        lua_pushnil(L);
        while (lua_next(L, wanted) != 0) {
            bool hit = false;
            for (int index = 1;; ++index) {
                if (!lua_getupvalue(L, candidate, index)) {
                    break;
                }
                hit = filter_value_equal(L, -1, -2);
                lua_pop(L, 1);
                if (hit) {
                    break;
                }
            }
            lua_pop(L, 1); // the wanted value
            if (!hit) {
                complete = false;
                lua_pop(L, 1); // the key lua_next left on the stack
                break;
            }
        }
        lua_settop(L, wanted - 1); // drop the Upvalues table
        if (!complete) {
            lua_settop(L, base);
            return false;
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, base);
    return true;
}

bool table_filter_matches(lua_State* L, int candidate, int options) {
    const int base = lua_gettop(L);
    bool keep = table_filter_has_all(L, candidate, options, "Keys", 0) &&
                table_filter_has_all(L, candidate, options, "Values", 1) &&
                table_filter_has_all(L, candidate, options, "KeyValuePairs", 2);

    if (keep) {
        lua_getfield(L, options, "Metatable");
        if (!lua_isnil(L, -1)) {
            keep = lua_getmetatable(L, candidate) != 0;
            if (keep) {
                keep = lua_rawequal(L, -1, -2) != 0;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    lua_settop(L, base);
    return keep;
}

int filtergc(lua_State* L) {
    const char* kind = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const bool return_one = !lua_isnoneornil(L, 3) && lua_toboolean(L, 3) != 0;

    const bool want_function = std::strcmp(kind, "function") == 0;
    const bool want_table = std::strcmp(kind, "table") == 0;
    if (!want_function && !want_table) {
        luaL_argerror(L, 1, "expected 'function' or 'table'");
    }

    bool ignore_executor = true;
    if (want_function) {
        lua_getfield(L, 2, "IgnoreExecutor");
        if (!lua_isnil(L, -1)) {
            ignore_executor = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    }

    if (env_trace()) {
        zend::logf("env.filtergc: kind=%s return_one=%d ignore_executor=%d",
                   kind, return_one ? 1 : 0, ignore_executor ? 1 : 0);
        lua_getfield(L, 2, "Upvalues");
        if (lua_istable(L, -1)) {
            const int count = static_cast<int>(lua_objlen(L, -1));
            zend::logf("env.filtergc: Upvalues count=%d", count);
            for (int index = 1; index <= count && index <= 8; ++index) {
                lua_rawgeti(L, -1, index);
                if (lua_type(L, -1) == LUA_TNUMBER) {
                    zend::logf("env.filtergc: Upvalues[%d]=number %.17g", index,
                               lua_tonumber(L, -1));
                } else if (lua_type(L, -1) == LUA_TSTRING) {
                    zend::logf("env.filtergc: Upvalues[%d]=string len=%zu", index,
                               lua_objlen(L, -1));
                } else {
                    zend::logf("env.filtergc: Upvalues[%d]=%s", index,
                               luaL_typename(L, -1));
                }
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1)) {
            zend::logf("env.filtergc: Upvalues is %s", luaL_typename(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "Constants");
        if (lua_istable(L, -1)) {
            zend::logf("env.filtergc: Constants count=%d",
                       static_cast<int>(lua_objlen(L, -1)));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "Name");
        if (lua_isstring(L, -1)) {
            zend::logf("env.filtergc: Name=%s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    collect_gc_type(L, want_table ? LUA_TTABLE : LUA_TFUNCTION);
    const int pool = lua_gettop(L);
    lua_newtable(L);
    const int out = lua_gettop(L);
    int found = 0;

    const int count = static_cast<int>(lua_objlen(L, pool));
    if (env_trace()) {
        zend::logf("env.filtergc: candidates=%d", count);
    }
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(L, pool, index);
        const int candidate = lua_gettop(L);
        bool keep = false;

        if (want_function && lua_isfunction(L, candidate)) {
            keep = function_filter_matches(L, candidate, 2, ignore_executor);
        } else if (want_table && lua_istable(L, candidate)) {
            keep = table_filter_matches(L, candidate, 2);
        }

        if (keep) {
            if (return_one) {
                if (env_trace()) {
                    zend::logf("env.filtergc: matched at %d", index);
                }
                lua_pushvalue(L, candidate);
                lua_replace(L, pool);
                lua_settop(L, pool);
                return 1;
            }
            lua_pushvalue(L, candidate);
            lua_rawseti(L, out, ++found);
        }
        lua_settop(L, out);
    }

    if (env_trace()) {
        zend::logf("env.filtergc: found=%d", found);
    }

    if (return_one) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, out);
    lua_replace(L, pool);
    lua_settop(L, pool);
    return 1;
}

// An executor thread has no owning script, so its weak_ptr is empty/expired.
// The client uses the same check to classify its own threads.
bool extra_script_expired(const RobloxExtraSpace* space) {
    return !space || space->script.expired();
}

// Caller classification follows the reference: our own executor thread, or a
// thread owned by the client that either has no script or full capabilities.
// Identity changes alone must not change the answer.
int checkcaller(lua_State* L) {
    lua_State* owner = executor::thread();
    bool ours = false;
    if (owner && owner->global == L->global) {
        if (L == owner) {
            ours = true;
        } else if (L->userdata) {
            ours = extra_script_expired(L->userdata) ||
                   L->userdata->capabilities == ~std::uint64_t{0};
        }
    }
    lua_pushboolean(L, ours);
    return 1;
}

void push_closure(lua_State* L, Closure* closure) {
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    setclvalue(L, L->top, closure);
    incr_top(L);
}

// --- hookfunction / restorefunction / newcclosure ------------------------
//
// A hook has to keep the target's identity: every existing reference to the
// closure must start running the hook without the caller re-fetching it.
// Lua closures therefore get a fresh dispatcher proto installed into the same
// allocation, while C closures get a dispatcher C function. Both dispatchers
// resolve their bindings from the closure pointer, so no state has to travel
// through the Lua stack. `newcclosure` follows the same rule with a C wrapper
// that forwards to a Lua function.

struct HookRecord {
    int target_ref = LUA_NOREF;
    int original_ref = LUA_NOREF;
    int hook_ref = LUA_NOREF;
    // The target is a newcclosure wrapper. Its "original" is the wrapped Lua
    // function, not a clone of the wrapper, so restoring has to rebuild the
    // wrapper dispatcher instead of copying that function over the C closure.
    bool original_is_wrapper = false;
    // Whether checkcaller already considered the target one of ours before
    // this hook. Restored client closures are removed again so a later GC
    // cannot leave a stale pointer behind.
    bool own_before = false;
};

struct NewCClosureRecord {
    int function_ref = LUA_NOREF;
    // Anchors the wrapper itself. Without it a collected wrapper leaves a
    // dangling map key that a future closure could reuse.
    int wrapper_ref = LUA_NOREF;
    bool reusable = false;
};

std::unordered_map<Closure*, HookRecord> g_hooks;
std::unordered_map<Closure*, NewCClosureRecord> g_newc;
std::unordered_set<Closure*> g_wrapped;

Closure* to_closure(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TFUNCTION);
    return clvalue(luaA_toobject(L, index));
}

int ref_at(lua_State* L, int index) {
    lua_pushvalue(L, index);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return ref;
}

int retain_ref(lua_State* L, int ref) {
    if (ref == LUA_NOREF) {
        return LUA_NOREF;
    }
    lua_getref(L, ref);
    const int copy = lua_ref(L, -1);
    lua_pop(L, 1);
    return copy;
}

void release_ref(lua_State* L, int& ref) {
    if (ref != LUA_NOREF) {
        lua_unref(L, ref);
        ref = LUA_NOREF;
    }
}

Closure* closure_at_ref(lua_State* L, int ref) {
    if (ref == LUA_NOREF) {
        return nullptr;
    }
    lua_getref(L, ref);
    Closure* closure = nullptr;
    if (lua_isfunction(L, -1)) {
        closure = clvalue(luaA_toobject(L, -1));
    }
    lua_pop(L, 1);
    return closure;
}

void copy_closure_body(lua_State* L, Closure* target, Closure* source) {
    target->stacksize = source->stacksize;
    target->preload = source->preload;
    target->nupvalues = source->nupvalues;
    target->env = source->env;
    if (source->env) {
        luaC_objbarrier(L, target, source->env);
    }

    if (source->isC) {
        target->c.f = source->c.f;
        target->c.cont = source->c.cont.Get();
        target->c.debugname = source->c.debugname;
        if (target->c.debugname) {
            luaC_objbarrier(L, target, target->c.debugname);
        }
        for (int i = 0; i < source->nupvalues; ++i) {
            setobj2n(L, &target->c.upvals[i], &source->c.upvals[i]);
            luaC_barrier(L, target, &target->c.upvals[i]);
        }
    } else {
        target->l.p = source->l.p;
        if (source->l.p) {
            luaC_objbarrier(L, target, source->l.p);
        }
        for (int i = 0; i < source->nupvalues; ++i) {
            setobj2n(L, &target->l.uprefs[i], &source->l.uprefs[i]);
            luaC_barrier(L, target, &target->l.uprefs[i]);
        }
    }
}

// Resumes a yielding hook/wrapper. The VM restores the call frame and hands
// the results back at L->base, so the result count is the whole visible stack.
// A __namecall wrapper that yielded needs its frame closed here too; the check
// is a no-op for every other dispatcher.
int dispatch_continuation(lua_State* L, int status) {
    (void)status;
    end_namecall_frame(L);
    return lua_gettop(L);
}

// Calls the function referenced by `ref` with the current arguments. When the
// C closure owning this call has a continuation, Luau lets the callee yield
// through us and re-enters at dispatch_continuation.
int call_function_ref(lua_State* L, int ref) {
    lua_getref(L, ref);
    if (!lua_isfunction(L, -1)) {
        luaL_error(L, "hookfunction: referenced value is not a function");
    }

    lua_insert(L, 1);
    luaD_callint(L, L->base, LUA_MULTRET, lua_isyieldable(L) != 0);
    if (isyielded(L)) {
        return C_CALL_YIELD;
    }
    return lua_gettop(L);
}

int newc_dispatch(lua_State* L) {
    Closure* current = curr_func(L);
    auto it = g_newc.find(current);
    if (it == g_newc.end() || it->second.function_ref == LUA_NOREF) {
        luaL_error(L, "newcclosure: missing wrapped function");
    }
    return call_function_ref(L, it->second.function_ref);
}

// C target: the closure itself is the activation, so it identifies the record.
int hook_c_dispatch(lua_State* L) {
    Closure* current = curr_func(L);
    auto it = g_hooks.find(current);
    if (it == g_hooks.end() || it->second.hook_ref == LUA_NOREF) {
        luaL_error(L, "hookfunction: missing hook");
    }
    return call_function_ref(L, it->second.hook_ref);
}

// Lua target: the dispatcher proto calls us from the target's own frame. The
// caller frame carries the target closure, which keeps the dispatcher free of
// extra arguments and of any dependency on the target's original upvalues.
int hook_lua_dispatch(lua_State* L) {
    Closure* target = nullptr;
    if (L->ci > L->base_ci) {
        CallInfo* caller = L->ci - 1;
        if (caller->func && ttisfunction(caller->func)) {
            target = clvalue(caller->func);
        }
    }

    auto it = target ? g_hooks.find(target) : g_hooks.end();
    if (it == g_hooks.end() || it->second.hook_ref == LUA_NOREF) {
        luaL_error(L, "hookfunction: missing Lua hook");
    }
    return call_function_ref(L, it->second.hook_ref);
}

// A Lua-target hook costs two frames the reference implementation does not
// have: the replaced closure's dispatcher proto and the C function that calls
// the user's hook. They are an implementation detail, so level-based debug
// APIs must not count them. C-target hooks keep their own frame (the target's
// activation), so hook_c_dispatch and newc_dispatch stay visible.
bool is_hook_plumbing_frame(CallInfo* frame) {
    if (!frame || !frame->func || !ttisfunction(frame->func)) {
        return false;
    }
    Closure* closure = clvalue(frame->func);
    if (closure->isC) {
        return closure->c.f == hook_lua_dispatch;
    }
    const TString* source = closure->l.p ? closure->l.p->source : nullptr;
    constexpr char kDispatcherSource[] = "=zend-hookfunction";
    return source && source->len == sizeof(kDispatcherSource) - 1 &&
           std::memcmp(getstr(source), kDispatcherSource,
                       sizeof(kDispatcherSource) - 1) == 0;
}

// Builds one dispatcher proto per hooked Lua target. Keeping it per-target
// lets the original proto's debugname be carried over so debug.info(f, "n")
// still reports the pre-hook name.
Proto* make_hook_dispatcher_proto(lua_State* L, Proto* name_source) {
    const std::string bytecode = compile("return __zend_hook_dispatch(...)");
    if (luau_load(L, "=zend-hookfunction", bytecode.data(), bytecode.size(), 0) != 0) {
        lua_error(L);
    }

    Closure* loader = clvalue(luaA_toobject(L, -1));
    Proto* proto = loader->l.p;
    executor::apply_capabilities(proto);

    if (name_source && name_source->debugname) {
        // debugname is stored through VMValue1, so it has to be assigned the
        // decoded pointer; copying the encoded wrapper value would decode to
        // pointer + (proto - name_source).
        proto->debugname = name_source->debugname.Get();
        luaC_objbarrier(L, proto, proto->debugname.Get());
    }

    // The loader closure stays on the stack; the caller installs the proto
    // before popping it so the new proto stays reachable.
    return proto;
}

Closure* clone_closure(lua_State* L, Closure* source) {
    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    Closure* clone = nullptr;
    if (source->isC) {
        clone = luaF_newCclosure(L, source->nupvalues, source->env);
        clone->stacksize = source->stacksize;
        clone->preload = source->preload;
        clone->c.f = source->c.f;
        clone->c.cont = source->c.cont.Get();
        clone->c.debugname = source->c.debugname;
        for (int i = 0; i < source->nupvalues; ++i) {
            setobj2n(L, &clone->c.upvals[i], &source->c.upvals[i]);
            luaC_barrier(L, clone, &clone->c.upvals[i]);
        }
        if (clone->env) {
            luaC_objbarrier(L, clone, clone->env);
        }
        if (clone->c.debugname) {
            luaC_objbarrier(L, clone, clone->c.debugname);
        }
    } else {
        clone = luaF_newLclosure(L, source->nupvalues, source->env,
                                 source->l.p);
        for (int i = 0; i < source->nupvalues; ++i) {
            TValue* slot = &source->l.uprefs[i];
            if (ttisupval(slot)) {
                setupvalue(L, &clone->l.uprefs[i], upvalue(slot));
            } else {
                setobj2n(L, &clone->l.uprefs[i], slot);
            }
            luaC_barrier(L, clone, &clone->l.uprefs[i]);
        }
        if (clone->env) {
            luaC_objbarrier(L, clone, clone->env);
        }
    }

    // A clone of a dispatcher needs its own binding record; the closure
    // pointer differs from the source and either one can be rehooked or
    // restored independently afterwards.
    auto newc = g_newc.find(source);
    if (newc != g_newc.end()) {
        push_closure(L, clone);
        const int wrapper_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        g_newc[clone] = {retain_ref(L, newc->second.function_ref), wrapper_ref, false};
        g_wrapped.insert(clone);
        // The clone runs the same dispatcher, so checkcaller must see it as
        // ours just like the original wrapper.
        g_own_closures.insert(clone);
    }

    auto hook = g_hooks.find(source);
    if (hook != g_hooks.end() && hook->second.hook_ref != LUA_NOREF) {
        HookRecord snapshot;
        snapshot.original_ref = retain_ref(L, hook->second.original_ref);
        snapshot.hook_ref = retain_ref(L, hook->second.hook_ref);
        snapshot.original_is_wrapper = hook->second.original_is_wrapper;
        push_closure(L, clone);
        snapshot.target_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        g_hooks[clone] = snapshot;
        if (g_own_closures.contains(source)) {
            g_own_closures.insert(clone);
        }
    }

    return clone;
}

void install_hook_body(lua_State* L, Closure* target) {
    if (target->isC) {
        target->c.f = hook_c_dispatch;
        target->c.cont = dispatch_continuation;
        g_own_closures.insert(target);
        return;
    }

    Proto* original = target->l.p;
    Proto* dispatcher = make_hook_dispatcher_proto(L, original);

    // The closure keeps its original upvalue count: luaF_freeclosure derives the
    // allocation size from it, so shrinking it here would free the wrong size
    // and corrupt the page allocator. The dispatcher proto uses zero upvalues,
    // so the surplus anchors are simply never read.
    target->stacksize = dispatcher->maxstacksize;
    target->preload = 0;
    target->env = L->gt;
    target->l.p = dispatcher;
    if (target->env) {
        luaC_objbarrier(L, target, target->env);
    }
    luaC_objbarrier(L, target, dispatcher);
    lua_pop(L, 1); // loader closure
}

void restore_hook_body(lua_State* L, Closure* target, const HookRecord& record) {
    if (record.original_is_wrapper) {
        auto wrapper = g_newc.find(target);
        if (wrapper == g_newc.end() || wrapper->second.function_ref == LUA_NOREF) {
            luaL_error(L, "restorefunction: missing newcclosure binding");
        }
        target->c.f = newc_dispatch;
        target->c.cont = dispatch_continuation;
        g_own_closures.insert(target);
        return;
    }

    Closure* original = closure_at_ref(L, record.original_ref);
    if (!original || original->isC != target->isC) {
        luaL_error(L, "restorefunction: incompatible closure layout");
    }
    copy_closure_body(L, target, original);
    if (!record.own_before) {
        g_own_closures.erase(target);
    }
}

void release_hook(lua_State* L, HookRecord& record) {
    release_ref(L, record.target_ref);
    release_ref(L, record.original_ref);
    release_ref(L, record.hook_ref);
}

// The shared hook body. `hook == original` uninstalls the hook, which is the
// standard way scripts swap a hook out without losing the record. Both values
// are taken from the given stack indices.
int install_hook(lua_State* L, int target_index, int hook_index) {
    Closure* target = to_closure(L, target_index);
    Closure* hook = to_closure(L, hook_index);

    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) {
        HookRecord record;
        record.target_ref = ref_at(L, target_index);
        record.own_before = g_own_closures.contains(target);

        auto wrapper = g_newc.find(target);
        if (target->isC && wrapper != g_newc.end()) {
            record.original_ref = retain_ref(L, wrapper->second.function_ref);
            record.original_is_wrapper = true;
        } else {
            Closure* snapshot = clone_closure(L, target);
            push_closure(L, snapshot);
            record.original_ref = lua_ref(L, -1);
            lua_pop(L, 1);
        }

        it = g_hooks.emplace(target, record).first;
    }

    Closure* original = closure_at_ref(L, it->second.original_ref);
    if (hook == original) {
        restore_hook_body(L, target, it->second);
        release_hook(L, it->second);
        g_hooks.erase(it);
        lua_pushvalue(L, hook_index);
        return 1;
    }

    int previous_ref = LUA_NOREF;
    if (it->second.hook_ref != LUA_NOREF) {
        previous_ref = retain_ref(L, it->second.hook_ref);
    } else {
        previous_ref = retain_ref(L, it->second.original_ref);
    }

    release_ref(L, it->second.hook_ref);
    it->second.hook_ref = ref_at(L, hook_index);
    install_hook_body(L, target);

    lua_getref(L, previous_ref);
    lua_unref(L, previous_ref);
    return 1;
}

int hook_function(lua_State* L) {
    return install_hook(L, 1, 2);
}

// The __namecall slot is swapped with this C wrapper instead of patching the
// client's native closure in place. The user hook is the upvalue, so nested
// hooks and restores stay independent while the VM keeps L->namecall alive for
// the duration of the call.
int namecall_wrapper(lua_State* L) {
    Closure* current = curr_func(L);
    begin_namecall_frame(L, current);

    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    luaD_callint(L, L->base, LUA_MULTRET, lua_isyieldable(L) != 0);
    if (isyielded(L)) {
        return C_CALL_YIELD;
    }

    end_namecall_frame(L);
    return lua_gettop(L);
}

// True when argument 1 is the DataModel. The ref is set during install, so
// this is only ever called after the HTTP prelude has run.
bool is_data_model(lua_State* L) {
    if (g_game_ref == LUA_NOREF) {
        return false;
    }
    lua_getref(L, g_game_ref);
    const bool same = lua_rawequal(L, 1, -1) != 0;
    lua_pop(L, 1);
    return same;
}

int instance_index_wrapper(lua_State* L) {
    if (lua_type(L, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* key = lua_tolstring(L, 2, &length);
        int method_ref = LUA_NOREF;
        if ((length == 7 && std::memcmp(key, "HttpGet", 7) == 0) ||
            (length == 12 && std::memcmp(key, "HttpGetAsync", 12) == 0)) {
            method_ref = g_http_get_ref;
        } else if ((length == 8 && std::memcmp(key, "HttpPost", 8) == 0) ||
                   (length == 13 && std::memcmp(key, "HttpPostAsync", 13) == 0)) {
            method_ref = g_http_post_ref;
        }

        if (method_ref != LUA_NOREF && is_data_model(L)) {
            lua_getref(L, method_ref);
            return 1;
        }
    }

    return g_instance_index ? g_instance_index(L) : 0;
}

int instance_namecall_wrapper(lua_State* L) {
    const char* method = lua_namecallatom(L, nullptr);
    if (method) {
        int method_ref = LUA_NOREF;
        if (std::strcmp(method, "HttpGet") == 0 ||
            std::strcmp(method, "HttpGetAsync") == 0) {
            method_ref = g_http_get_ref;
        } else if (std::strcmp(method, "HttpPost") == 0 ||
                   std::strcmp(method, "HttpPostAsync") == 0) {
            method_ref = g_http_post_ref;
        }

        if (method_ref != LUA_NOREF && is_data_model(L)) {
            // Drop the method receiver so the global gets the same arguments a
            // plain HttpGet(url, ...) call would.
            lua_getref(L, method_ref);
            lua_remove(L, 1);
            lua_insert(L, 1);
            luaD_callint(L, L->base, LUA_MULTRET, lua_isyieldable(L) != 0);
            if (isyielded(L)) {
                return C_CALL_YIELD;
            }
            return lua_gettop(L);
        }
    }

    return g_instance_namecall ? g_instance_namecall(L) : 0;
}

int setnamecallmethod(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    size_t length = 0;
    const char* name = lua_tolstring(L, 1, &length);
    auto found = g_namecall_frames.find(L);
    if (found == g_namecall_frames.end() || found->second.empty()) {
        return 0;
    }

    NamecallFrame& frame = found->second.back();
    if (frame.atom_ref != LUA_NOREF) {
        lua_unref(L, frame.atom_ref);
        frame.atom_ref = LUA_NOREF;
    }

    frame.atom = length ? luaS_newlstr(L, name, length) : nullptr;
    L->namecall = frame.atom;
    if (frame.atom) {
        lua_pushlstring(L, frame.atom->data, frame.atom->len);
        frame.atom_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    return 0;
}

int hookmetamethod(lua_State* L) {
    luaL_checkany(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    const char* metamethod = lua_tostring(L, 2);

    if (!lua_getmetatable(L, 1)) {
        luaL_argerror(L, 1, "object has no metatable");
    }
    const int metatable = lua_gettop(L);

    lua_rawgetfield(L, metatable, metamethod);
    if (!lua_isfunction(L, -1)) {
        luaL_argerror(L, 2, "metamethod is not a function");
    }
    const int previous = lua_gettop(L);

    if (std::strcmp(metamethod, "__namecall") == 0) {
        LuaTable* table = hvalue(luaA_toobject(L, metatable));
        const bool was_readonly = table->readonly;
        table->readonly = false;

        Closure* current = clvalue(luaA_toobject(L, previous));
        Closure* hook = clvalue(luaA_toobject(L, 3));
        const bool is_restore =
            current == hook || (hook->isC && hook->c.f == namecall_wrapper);

        if (is_restore) {
            lua_pushvalue(L, 3);
            lua_rawsetfield(L, metatable, metamethod);
        } else {
            lua_pushvalue(L, 3);
            lua_pushcclosurek(L, namecall_wrapper, "__namecall", 1,
                              dispatch_continuation);
            Closure* wrapper = clvalue(luaA_toobject(L, -1));
            if (wrapper) {
                g_own_closures.insert(wrapper);
            }
            lua_rawsetfield(L, metatable, metamethod);
        }

        table->readonly = was_readonly;
        lua_pushvalue(L, previous);
        return 1;
    }

    // Everything else is a regular closure hook: keep the slot's identity and
    // return a snapshot of the original body.
    return install_hook(L, previous, 3);
}

int hookfunction(lua_State* L) {
    return hook_function(L);
}

int restorefunction(lua_State* L) {
    Closure* target = to_closure(L, 1);
    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) {
        lua_pushboolean(L, false);
        return 1;
    }

    restore_hook_body(L, target, it->second);
    release_hook(L, it->second);
    g_hooks.erase(it);
    lua_pushboolean(L, true);
    return 1;
}

int newcclosure(lua_State* L) {
    Closure* closure = to_closure(L, 1);
    if (closure->isC) {
        lua_pushvalue(L, 1);
        return 1;
    }

    // Repeated newcclosure(f) calls return the existing wrapper, matching the
    // identity scripts rely on when they wrap the same function twice.
    for (const auto& [wrapper, record] : g_newc) {
        if (record.reusable && closure_at_ref(L, record.function_ref) == closure) {
            push_closure(L, wrapper);
            return 1;
        }
    }

    const char* debugname = lua_isstring(L, 2) ? lua_tostring(L, 2) : "newcclosure";
    lua_pushvalue(L, 1);
    lua_pushcclosurek(L, newc_dispatch, debugname, 1, dispatch_continuation);

    Closure* wrapper = clvalue(luaA_toobject(L, -1));
    // The upvalue already pins the function; the record is what the dispatcher
    // reads, so keep explicit registry references as well. Anchoring the
    // wrapper keeps its map key alive so a later allocation cannot reuse the
    // address with a stale record.
    g_newc[wrapper].wrapper_ref = lua_ref(L, -1);
    lua_pushvalue(L, 1);
    g_newc[wrapper].function_ref = lua_ref(L, -1);
    g_newc[wrapper].reusable = true;
    lua_pop(L, 1);

    g_wrapped.insert(wrapper);
    g_own_closures.insert(wrapper);

    wrapper->env = closure->env;
    if (wrapper->env) {
        luaC_objbarrier(L, wrapper, wrapper->env);
    }
    return 1;
}

int isnewcclosure(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Closure* closure = clvalue(luaA_toobject(L, 1));
    lua_pushboolean(L, g_wrapped.contains(closure));
    return 1;
}

int isfunctionhooked(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Closure* closure = clvalue(luaA_toobject(L, 1));
    auto it = g_hooks.find(closure);
    lua_pushboolean(L, it != g_hooks.end() && it->second.hook_ref != LUA_NOREF);
    return 1;
}

int clonefunction(lua_State* L) {
    Closure* source = to_closure(L, 1);
    Closure* clone = clone_closure(L, source);
    push_closure(L, clone);
    return 1;
}

// --- debug ---------------------------------------------------------------

// Pushes the function selected by `idx` (a function value or a positive stack
// level) and returns its stack index. The caller owns the pushed value.
int push_function_arg(lua_State* L, int idx) {
    if (lua_isfunction(L, idx)) {
        lua_pushvalue(L, idx);
        return lua_gettop(L);
    }

    if (!lua_isnumber(L, idx)) {
        luaL_argerror(L, idx, "function or level expected");
    }

    const int level = static_cast<int>(lua_tointeger(L, idx));
    if (level <= 0) {
        luaL_argerror(L, idx, "level must be positive");
    }

    lua_Debug ar;
    std::memset(&ar, 0, sizeof(ar));
    if (!lua_getinfo(L, level, "f", &ar) || !lua_isfunction(L, -1)) {
        luaL_argerror(L, idx, "invalid level");
    }
    return lua_gettop(L);
}

Closure* lua_closure(lua_State* L, int index, bool allow_c) {
    Closure* closure = clvalue(luaA_toobject(L, index));
    if (!closure) {
        luaL_argerror(L, index, "function expected");
    }
    if (!allow_c && closure->isC) {
        luaL_argerror(L, index, "Lua closure expected");
    }
    return closure;
}

void set_string_field(lua_State* L, const char* key, const char* value) {
    if (value) {
        lua_pushstring(L, value);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, key);
}

int debug_getinfo(lua_State* L) {
    luaL_checkany(L, 1);

    int level = 0;
    if (lua_isnumber(L, 1)) {
        level = static_cast<int>(lua_tointeger(L, 1));
        if (level <= 0) {
            luaL_argerror(L, 1, "level must be positive");
        }
    } else if (lua_isfunction(L, 1)) {
        level = -lua_gettop(L);
    } else {
        luaL_argerror(L, 1, "function or level expected");
    }

    lua_Debug ar;
    std::memset(&ar, 0, sizeof(ar));
    if (!lua_getinfo(L, level, "sluanf", &ar) || !lua_isfunction(L, -1)) {
        luaL_argerror(L, 1, "invalid level");
    }

    lua_newtable(L);
    set_string_field(L, "source", ar.source);
    set_string_field(L, "short_src", ar.short_src);
    set_string_field(L, "what", ar.what);
    lua_pushinteger(L, ar.linedefined);
    lua_setfield(L, -2, "linedefined");
    lua_pushinteger(L, ar.currentline);
    lua_setfield(L, -2, "currentline");
    lua_pushinteger(L, ar.nupvals);
    lua_setfield(L, -2, "nups");
    lua_pushinteger(L, ar.isvararg);
    lua_setfield(L, -2, "is_vararg");
    lua_pushinteger(L, ar.nparams);
    lua_setfield(L, -2, "numparams");
    set_string_field(L, "name", ar.name);

    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "func");
    lua_remove(L, -2);
    return 1;
}

int debug_getlocal(lua_State* L) {
    const int level = static_cast<int>(luaL_checkinteger(L, 1));
    const int index = static_cast<int>(luaL_checkinteger(L, 2));
    if (level <= 0) {
        luaL_argerror(L, 1, "level must be positive");
    }

    const char* name = lua_getlocal(L, level, index);
    if (!name) {
        luaL_argerror(L, 2, "index out of range");
    }

    // lua_getlocal pushed the value; return name, value like the stock Lua
    // debug library so callers can dump a frame without guessing indices.
    lua_pushstring(L, name);
    lua_insert(L, -2);
    return 2;
}

int debug_getconstants(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    Proto* proto = closure->l.p;
    if (!proto) {
        luaL_argerror(L, 1, "invalid closure");
    }

    lua_createtable(L, proto->sizek, 0);
    lua_rawcheckstack(L, 1);
    for (int i = 0; i < proto->sizek; ++i) {
        const TValue* constant = &proto->k[i];
        const int type = constant->tt;
        if (type == LUA_TNIL || type == LUA_TFUNCTION ||
            type == LUA_TTABLE) {
            lua_pushnil(L);
        } else {
            luaC_threadbarrier(L);
            luaA_pushvalue(L, constant);
        }
        lua_rawseti(L, -2, i + 1);
    }

    lua_remove(L, fn);
    return 1;
}

int debug_getupvalues(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, true);

    const int count = closure->nupvalues;
    lua_createtable(L, count, 0);
    for (int i = 1; i <= count; ++i) {
        if (lua_getupvalue(L, fn, i)) {
            lua_rawseti(L, -2, i);
        } else {
            lua_pushnil(L);
            lua_rawseti(L, -2, i);
        }
    }

    lua_remove(L, fn);
    return 1;
}

int debug_setupvalue(lua_State* L) {
    luaL_checkany(L, 3);
    const int fn = push_function_arg(L, 1);
    const int index = luaL_checkinteger(L, 2);

    lua_pushvalue(L, 3);
    if (!lua_setupvalue(L, fn, index)) {
        luaL_argerror(L, 2, "index out of range");
    }

    lua_pushvalue(L, 3);
    return 1;
}

// `include_current` selects what level 1 means. For the calling thread the
// current frame is this debug API's own C frame, so level 1 is its caller
// (Lua convention). A foreign thread has no such debug frame and is usually
// suspended inside `yield`, so level 1 must be that thread's own top frame.
// Frames every other debug API sees are counted, C frames included: the
// reference numbers `pcall` and friends. Only our hook trampoline is hidden.
CallInfo* visible_level(lua_State* L, int level, bool include_current = false) {
    CallInfo* start = include_current ? L->ci : L->ci - 1;
    for (CallInfo* frame = start; frame >= L->base_ci; --frame) {
        if (is_hook_plumbing_frame(frame)) {
            continue;
        }
        if (!frame->func || !ttisfunction(frame->func)) {
            continue;
        }
        if (--level == 0) {
            return frame;
        }
    }
    return nullptr;
}

int stack_frame_size(lua_State* target, CallInfo* frame, Closure* closure) {
    if (!target || !frame || !frame->base) {
        return 0;
    }

    int size = 0;
    if (closure && !closure->isC && closure->l.p) {
        size = closure->l.p->maxstacksize;
    }
    if (size <= 0 && frame->top > frame->base) {
        size = static_cast<int>(frame->top - frame->base);
    }
    if (size <= 0 && target->top > frame->base) {
        size = static_cast<int>(target->top - frame->base);
    }

    const int available =
        static_cast<int>((target->stack + target->stacksize) - frame->base);
    if (available < size) {
        size = available;
    }
    return size > 0 ? size : 0;
}

// Resolves the frame selected by a level or a function value. Only Lua frames
// are visible, matching getstack/setstack.
CallInfo* resolve_stack_target(lua_State* L, int index, Closure*& out_closure) {
    out_closure = nullptr;

    if (lua_isnumber(L, index)) {
        const int level = static_cast<int>(luaL_checkinteger(L, index));
        if (level <= 0) {
            luaL_argerror(L, index, "level must be positive");
        }
        CallInfo* frame = visible_level(L, level);
        if (!frame) {
            luaL_argerror(L, index, "level out of range");
        }
        out_closure = clvalue(frame->func);
        return frame;
    }

    if (lua_isfunction(L, index)) {
        Closure* wanted = clvalue(luaA_toobject(L, index));
        for (CallInfo* frame = L->ci - 1; frame >= L->base_ci; --frame) {
            if (!frame->func || !ttisfunction(frame->func)) {
                continue;
            }
            if (clvalue(frame->func) == wanted) {
                out_closure = wanted;
                return frame;
            }
        }
        luaL_argerror(L, index, "function not found in call stack");
    }

    luaL_argerror(L, index, "function or level expected");
    return nullptr;
}

// Copies a proto for an inactive debug.getproto(s) result. The copy keeps the
// metadata scripts inspect (constants, debug name, source, arity, line) but its
// code is the compiler's empty `return` chunk, so native debug.info still sees
// the original definition while calling the proxy cannot run the game's
// bytecode. Hand-built instructions are not an option: opcode bytes in this
// build are permuted, and a wrong byte executes arbitrary VM operations.
// The clone's bytecode is the compiler's own `return` chunk, copied
// instruction for instruction (the reference does the same). Opcodes are not
// assemblable by hand: the client's bytecode carries every opcode permuted
// through the build's 256-entry wire table, and only compiler output (encoded
// by WireEncoder) is guaranteed to carry that numbering. The loaded proto is
// only a source of instructions, so its bytes are cached once and the temp
// closure is dropped.
struct StubCode {
    std::vector<Instruction> code;
    int maxstacksize = 0;
    bool valid = false;
};

const StubCode& return_stub(lua_State* L) {
    static const StubCode stub = [L]() {
        StubCode result;
        static const std::string bytecode = compile("return");
        if (bytecode.empty() || bytecode[0] == '\0') {
            return result;
        }
        if (luau_load(L, "@cloneproto", bytecode.data(), bytecode.size(), 0) !=
            0) {
            return result;
        }

        Closure* loaded = lua_isfunction(L, -1)
                              ? clvalue(const_cast<TValue*>(
                                    luaA_toobject(L, -1)))
                              : nullptr;
        if (loaded && !loaded->isC && loaded->l.p &&
            loaded->l.p->sizecode > 0) {
            result.code.assign(loaded->l.p->code,
                               loaded->l.p->code + loaded->l.p->sizecode);
            result.maxstacksize = loaded->l.p->maxstacksize;
            result.valid = true;
        }
        lua_pop(L, 1);
        return result;
    }();
    return stub;
}

Proto* clone_proto(lua_State* L, Proto* proto) {
    const StubCode& stub = return_stub(L);
    if (!stub.valid) {
        return nullptr;
    }

    const int sizecode = static_cast<int>(stub.code.size());

    Proto* clone = luaF_newproto(L);
    if (!clone) {
        return nullptr;
    }

    clone->is_vararg = proto->is_vararg;
    clone->flags = 0;
    clone->maxstacksize = proto->maxstacksize;
    if (stub.maxstacksize > clone->maxstacksize) {
        clone->maxstacksize = stub.maxstacksize;
    }
    if (clone->maxstacksize < 2) {
        clone->maxstacksize = 2;
    }
    clone->nups = proto->nups;
    clone->numparams = proto->numparams;
    clone->linedefined = proto->linedefined;
    clone->bytecodeid = proto->bytecodeid;

    clone->code = luaM_newarray(L, sizecode, Instruction, clone->memcat);
    std::memcpy(clone->code, stub.code.data(),
                sizeof(Instruction) * static_cast<std::size_t>(sizecode));
    clone->sizecode = sizecode;
    clone->codeentry = clone->code;

    if (proto->sizek > 0 && proto->k) {
        clone->k = luaM_newarray(L, proto->sizek, TValue, clone->memcat);
        for (int i = 0; i < proto->sizek; ++i) {
            setobj2n(L, &clone->k[i], &proto->k[i]);
        }
        clone->sizek = proto->sizek;
    }

    // The stub's line info is rebuilt in the loader's layout: a flat byte
    // array followed by the absolute-line array it shares storage with. Gap 0
    // gives every pc its own absolute entry, so luaG_getline never reads past
    // the allocation while the call fails.
    clone->linegaplog2 = 0;
    const int absoffset = (sizecode + 3) & ~3;
    const int intervals = sizecode;
    const int lineinfo_size = absoffset + intervals * static_cast<int>(sizeof(int));
    clone->lineinfo = luaM_newarray(L, lineinfo_size, uint8_t, clone->memcat);
    std::memset(clone->lineinfo, 0, absoffset);
    clone->sizelineinfo = lineinfo_size;
    clone->abslineinfo = reinterpret_cast<int*>(clone->lineinfo + absoffset);
    for (int i = 0; i < intervals; ++i) {
        clone->abslineinfo[i] = proto->linedefined;
    }

    if (proto->sizelocvars > 0 && proto->locvars) {
        clone->locvars = luaM_newarray(L, proto->sizelocvars, LocVar,
                                       clone->memcat);
        for (int i = 0; i < proto->sizelocvars; ++i) {
            clone->locvars[i] = proto->locvars[i];
        }
        clone->sizelocvars = proto->sizelocvars;
    }

    if (proto->sizeupvalues > 0) {
        TString** names = proto->upvalues;
        if (names) {
            TString** copy = luaM_newarray(L, proto->sizeupvalues, TString*,
                                           clone->memcat);
            for (int i = 0; i < proto->sizeupvalues; ++i) {
                copy[i] = names[i];
            }
            clone->upvalues = copy;
            clone->sizeupvalues = proto->sizeupvalues;
        }
    }

    // These two fields are pointer-encoded relative to their own address, so
    // assigning one encoded field to another would carry the source address.
    // Decode first, then let the target field encode for its own address.
    clone->source = proto->source.Get();
    clone->debugname = proto->debugname.Get();

    if (proto->sizep > 0 && proto->p) {
        clone->p = luaM_newarray(L, proto->sizep, Proto*, clone->memcat);
        for (int i = 0; i < proto->sizep; ++i) {
            clone->p[i] = proto->p[i] ? clone_proto(L, proto->p[i]) : nullptr;
        }
        clone->sizep = proto->sizep;
    }

    return clone;
}

struct ProtoClosureScan {
    Proto* wanted;
    std::vector<Closure*> found;
};

bool proto_closure_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<ProtoClosureScan*>(raw);
    if (gco->gch.tt != LUA_TFUNCTION) {
        return false;
    }

    Closure* closure = gco2cl(gco);
    if (closure && !closure->isC && closure->l.p == scan->wanted) {
        scan->found.push_back(closure);
    }
    return false;
}

// An inactive proto is exposed as a real Lua closure over a cloned proto. The
// clone keeps the original metadata (name, source, line, arity, constants) but
// its bytecode calls nil, so calling it can never execute the code the game has
// not loaded. A C stub cannot report that metadata through debug.info, which is
// exactly what scripts inspect on these proxies.
bool push_proto_closure(lua_State* L, Proto* proto) {
    if (!proto) {
        return false;
    }

    Proto* cloned = clone_proto(L, proto);
    if (!cloned) {
        return false;
    }

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const int upvalues = proto->nups > 0 ? proto->nups : 0;
    Closure* closure = luaF_newLclosure(L, upvalues, L->gt, cloned);
    if (!closure) {
        return false;
    }
    setclvalue(L, L->top, closure);
    incr_top(L);
    g_own_closures.insert(closure);
    return true;
}

int debug_getproto(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    Proto* parent = closure->l.p;
    const int index = luaL_checkinteger(L, 2);
    // Only a real boolean requests the active-closure table. Callers that pass
    // some other value (an expected proto function, a truthy sentinel) still
    // want the plain proto result.
    const bool activated = lua_isboolean(L, 3) && lua_toboolean(L, 3);

    if (!parent || index < 1 || index > parent->sizep || !parent->p) {
        luaL_argerror(L, 2, "index out of range");
    }

    Proto* wanted = parent->p[index - 1];
    if (!wanted) {
        luaL_argerror(L, 2, "proto is unavailable");
    }

    if (!activated) {
        if (!push_proto_closure(L, wanted)) {
            luaL_error(L, "failed to create proto closure");
        }
        lua_remove(L, fn);
        return 1;
    }

    // Collect first, build the table after: allocating Lua objects while
    // luaM_visitgco walks the GC pages can free or replace the page being
    // iterated. The collector stays parked until every closure is anchored in
    // the table, so raw Closure* collected from the pages cannot be swept
    // before lua_rawseti stores them.
    ProtoClosureScan scan{wanted, {}};
    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, proto_closure_visitor);

    lua_createtable(L, static_cast<int>(scan.found.size()), 0);
    const int result = lua_gettop(L);
    int count = 0;
    for (Closure* found : scan.found) {
        push_closure(L, found);
        lua_rawseti(L, result, ++count);
    }

    // No live closure exists (an inactive proto): expose the nil-call proxy so
    // the table still maps the proto to a callable that cannot run game code.
    if (count == 0 && push_proto_closure(L, wanted)) {
        lua_rawseti(L, result, 1);
    }

    L->global->GCthreshold = threshold;
    lua_remove(L, fn);
    return 1;
}

int debug_getprotos(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    Proto* parent = closure->l.p;
    const bool activated = lua_isboolean(L, 2) && lua_toboolean(L, 2);

    if (!parent || parent->sizep <= 0 || !parent->p) {
        lua_newtable(L);
        lua_remove(L, fn);
        return 1;
    }

    lua_createtable(L, parent->sizep, 0);
    LuaTable* env = closure->env ? closure->env : L->gt;
    for (int i = 0; i < parent->sizep; ++i) {
        Proto* child = parent->p[i];
        if (!child) {
            lua_pushnil(L);
        } else if (activated) {
            const std::size_t threshold = L->global->GCthreshold;
            L->global->GCthreshold = SIZE_MAX;
            Proto* proto = clone_proto(L, child);
            if (proto) {
                Closure* child_closure =
                    luaF_newLclosure(L, child->nups, env, proto);
                push_closure(L, child_closure);
            } else {
                lua_pushnil(L);
            }
            L->global->GCthreshold = threshold;
        } else {
            if (!push_proto_closure(L, child)) {
                lua_pushnil(L);
            }
        }
        lua_rawseti(L, -2, i + 1);
    }

    lua_remove(L, fn);
    return 1;
}

int debug_getconstant(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    Proto* proto = closure->l.p;
    const int index = luaL_checkinteger(L, 2);

    if (!proto || proto->sizek <= 0) {
        luaL_argerror(L, 1, "function has no constants");
    }
    if (index < 1 || index > proto->sizek) {
        luaL_argerror(L, 2, "index out of range");
    }

    const TValue* constant = &proto->k[index - 1];
    const int type = constant->tt;
    if (type == LUA_TNIL || type == LUA_TTABLE || type == LUA_TFUNCTION) {
        lua_pushnil(L);
    } else {
        luaC_threadbarrier(L);
        luaA_pushvalue(L, constant);
    }

    lua_remove(L, fn);
    return 1;
}

int debug_setconstant(lua_State* L) {
    luaL_checkany(L, 3);

    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    Proto* proto = closure->l.p;
    const int index = luaL_checkinteger(L, 2);

    if (!proto || proto->sizek <= 0) {
        luaL_argerror(L, 1, "function has no constants");
    }
    if (index < 1 || index > proto->sizek) {
        luaL_argerror(L, 2, "index out of range");
    }

    TValue* constant = &proto->k[index - 1];
    if (constant->tt != LUA_TFUNCTION && constant->tt != LUA_TTABLE) {
        const TValue* value = luaA_toobject(L, 3);
        if (value && value->tt == constant->tt) {
            setobj2n(L, constant, value);
            luaC_barrier(L, proto, constant);
        }
    }

    lua_remove(L, fn);
    return 0;
}

int debug_getupvalue(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    const int index = luaL_checkinteger(L, 2);

    if (!closure || index < 1 || index > closure->nupvalues) {
        luaL_argerror(L, 2, "index out of range");
    }

    if (!lua_getupvalue(L, fn, index)) {
        luaL_argerror(L, 2, "index out of range");
    }
    return 1;
}

// Counts frames from the thread's own top frame. Level 0 is that top frame and
// level 1 is its caller, exactly like the reference. Every frame is counted,
// C frames included, except our hook trampoline, which scripts cannot see.
CallInfo* frame_at(lua_State* target, int level) {
    if (!target || level < 0) {
        return nullptr;
    }

    for (CallInfo* frame = target->ci; frame >= target->base_ci; --frame) {
        if (is_hook_plumbing_frame(frame)) {
            continue;
        }
        if (level-- == 0) {
            return frame;
        }
    }
    return nullptr;
}

bool slot_in_thread_stack(lua_State* target, StkId slot) {
    return slot >= target->stack && slot < target->stack + target->stacksize;
}

int debug_getstack(lua_State* L) {
    lua_State* target = L;
    int arg = 1;
    if (lua_isthread(L, 1)) {
        target = lua_tothread(L, 1);
        if (!target) {
            luaL_argerror(L, 1, "invalid thread");
        }
        arg = 2;
    }

    const int level = static_cast<int>(luaL_checkinteger(L, arg));
    CallInfo* frame = frame_at(target, level);
    if (!frame) {
        if (env_trace()) {
            zend::logf("env.getstack: level=%d out of range (foreign=%d)", level,
                       target != L ? 1 : 0);
        }
        luaL_argerror(L, arg, "level out of range");
    }
    if (!frame->base) {
        luaL_error(L, "callinfo base is null");
    }

    // C frames (the yield boundary of a suspended coroutine, pcall, and so on)
    // are snapshot-able too: their slots live in the same stack window, so a
    // test that probes every level gets a table instead of an error.
    Closure* closure = nullptr;
    if (frame->func && ttisfunction(frame->func)) {
        closure = clvalue(frame->func);
    }
    if (env_trace()) {
        zend::logf("env.getstack: foreign=%d level=%d isC=%d slots=%d "
                   "index=%s",
                   target != L ? 1 : 0, level,
                   closure && closure->isC ? 1 : 0,
                   stack_frame_size(target, frame, closure),
                   lua_isnumber(L, arg + 1) ? "yes" : "no");
    }

    const int size = stack_frame_size(target, frame, closure);

    if (lua_isnumber(L, arg + 1)) {
        const int index = static_cast<int>(lua_tointeger(L, arg + 1)) - 1;
        StkId slot = frame->base + index;
        if (index < 0 || index >= size || !slot_in_thread_stack(target, slot)) {
            lua_pushnil(L);
            return 1;
        }
        luaC_threadbarrier(L);
        luaA_pushvalue(L, slot);
        return 1;
    }

    lua_createtable(L, size, 0);
    lua_rawcheckstack(L, size > 0 ? size : 1);
    for (int i = 0; i < size; ++i) {
        StkId slot = frame->base + i;
        if (!slot_in_thread_stack(target, slot)) {
            break;
        }
        luaC_threadbarrier(L);
        luaA_pushvalue(L, slot);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int debug_setstack(lua_State* L) {
    Closure* closure = nullptr;
    CallInfo* frame = resolve_stack_target(L, 1, closure);
    if (!frame || !closure) {
        return 0;
    }
    if (closure->isC) {
        luaL_argerror(L, 1, "Lua function expected");
    }

    const int index = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    const int size = stack_frame_size(L, frame, closure);
    if (index < 0 || index >= size) {
        luaL_argerror(L, 2, "index out of range");
    }

    const TValue* value = luaA_toobject(L, 3);
    if (!value) {
        luaL_argerror(L, 3, "invalid value");
    }

    setobj2s(L, frame->base + index, value);
    return 0;
}

// --- filesystem ----------------------------------------------------------
//
// Everything lives under %LOCALAPPDATA%\zend\workspace. Lua passes UTF-8;
// each path is normalized with GetFullPathNameW and then required to stay
// under the root, so `..`, absolute paths and device paths cannot escape.

constexpr DWORD kWorkspacePathSize = 4096;
constexpr std::size_t kMaxReadFile = 256u * 1024u * 1024u;

void normalize_path(wchar_t* path) {
    for (wchar_t* p = path; *p; ++p) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }

    wchar_t* end = path + std::wcslen(path);
    while (end > path + 2 && end[-1] == L'\\') {
        *--end = L'\0';
    }
}

bool workspace_root(wchar_t* out, DWORD out_size) {
    wchar_t base[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }

    const std::wstring raw = std::wstring(base) + L"\\zend\\workspace";
    if (GetFullPathNameW(raw.c_str(), out_size, out, nullptr) >= out_size) {
        return false;
    }

    normalize_path(out);
    return out[0] != L'\0';
}

void resolve_path(lua_State* L, int index, wchar_t* out, DWORD out_size) {
    wchar_t root[kWorkspacePathSize] = {};
    if (!workspace_root(root, kWorkspacePathSize)) {
        luaL_error(L, "workspace path is unavailable");
    }

    if (lua_isnoneornil(L, index)) {
        wcscpy_s(out, out_size, root);
        return;
    }

    std::size_t length = 0;
    const char* raw = luaL_checklstring(L, index, &length);
    if (length == 0) {
        wcscpy_s(out, out_size, root);
        return;
    }

    wchar_t relative[kWorkspacePathSize] = {};
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw,
                            static_cast<int>(length), relative,
                            kWorkspacePathSize) <= 0) {
        luaL_error(L, "invalid path");
    }

    for (wchar_t* p = relative; *p; ++p) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }

    if (relative[0] == L'\\' ||
        (relative[0] != L'\0' && relative[1] == L':')) {
        luaL_error(L, "path outside workspace");
    }

    const std::wstring combined = std::wstring(root) + L'\\' + relative;
    if (GetFullPathNameW(combined.c_str(), out_size, out, nullptr) >= out_size) {
        luaL_error(L, "invalid path");
    }

    normalize_path(out);

    const std::size_t root_length = std::wcslen(root);
    if (_wcsnicmp(out, root, root_length) != 0 ||
        (out[root_length] != L'\0' && out[root_length] != L'\\')) {
        luaL_error(L, "path outside workspace");
    }
}

// Creates every missing component of `path` from the workspace down.
bool ensure_directory(const wchar_t* path) {
    wchar_t buffer[kWorkspacePathSize] = {};
    wcscpy_s(buffer, path);

    wchar_t* start = buffer;
    if (buffer[0] != L'\0' && buffer[1] == L':' && buffer[2] == L'\\') {
        start = buffer + 3;
    }

    for (wchar_t* p = start; *p; ++p) {
        if (*p != L'\\') {
            continue;
        }

        *p = L'\0';
        const DWORD error =
            CreateDirectoryW(buffer, nullptr) ? ERROR_SUCCESS : GetLastError();
        *p = L'\\';
        if (error != ERROR_SUCCESS && error != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }

    return CreateDirectoryW(buffer, nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ensure_parent(const wchar_t* file) {
    wchar_t parent[kWorkspacePathSize] = {};
    wcscpy_s(parent, file);

    wchar_t* slash = std::wcsrchr(parent, L'\\');
    if (!slash) {
        return false;
    }

    *slash = L'\0';
    return ensure_directory(parent);
}

bool read_file(const wchar_t* path, std::string& out) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaxReadFile) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));

    std::size_t offset = 0;
    while (offset < out.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(out.size() - offset, 0x100000));
        DWORD read = 0;
        if (!ReadFile(file, out.data() + offset, chunk, &read, nullptr) ||
            read == 0) {
            CloseHandle(file);
            out.clear();
            return false;
        }
        offset += read;
    }

    CloseHandle(file);
    return true;
}

bool write_file(const wchar_t* path, const char* data, std::size_t length,
                bool append) {
    if (!ensure_parent(path)) {
        return false;
    }

    HANDLE file = CreateFileW(path, append ? FILE_APPEND_DATA : GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr,
                              append ? OPEN_ALWAYS : CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < length) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(length - offset, 0x100000));
        DWORD written = 0;
        if (!WriteFile(file, data + offset, chunk, &written, nullptr) ||
            written == 0) {
            CloseHandle(file);
            return false;
        }
        offset += written;
    }

    CloseHandle(file);
    return true;
}

bool delete_tree(const wchar_t* path) {
    wchar_t pattern[kWorkspacePathSize] = {};
    swprintf_s(pattern, L"%s\\*", path);

    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(entry.cFileName, L".") == 0 ||
                std::wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }

            const std::wstring child =
                std::wstring(path) + L'\\' + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!delete_tree(child.c_str())) {
                    FindClose(search);
                    return false;
                }
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(child.c_str())) {
                    FindClose(search);
                    return false;
                }
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }

    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path) != FALSE;
}

bool blocked_extension(const wchar_t* path) {
    static const wchar_t* const kBlocked[] = {
        L".exe",  L".dll",  L".bat",  L".cmd",  L".vbs",  L".ps1",  L".msi",
        L".scr",  L".com",  L".pif",  L".sh",   L".cpl",  L".msc",  L".js",
        L".jse",  L".wsf",  L".wsh",  L".hta",  L".scf",  L".lnk",  L".zip",
        L".rar",  L".7z",   L".cab",  L".iso",  L".img",  L".xml",  L".msp",
        L".reg",  L".py",   L".pyc",  L".pyw",  L".rb",   L".pl",   L".php",
        L".jar",  L".sys",  L".drv",  L".ocx",  L".psd1", L".vbe",  L".inf",
        L".url",  L".ts",   L".tar",  L".gz",   L".xz",   L".bz2",  L".tgz",
        L".wim",  L".docm", L".pptm", L".xlsm", L".vba",  nullptr,
    };

    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name ? name + 1 : path;

    const wchar_t* extension = std::wcsrchr(name, L'.');
    if (!extension) {
        return false;
    }

    for (const wchar_t* const* it = kBlocked; *it; ++it) {
        if (_wcsicmp(extension, *it) == 0) {
            return true;
        }
    }
    return false;
}

int fs_readfile(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        luaL_error(L, "file does not exist");
    }

    std::string data;
    if (!read_file(path, data)) {
        luaL_error(L, "failed to open file");
    }

    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int fs_writefile(lua_State* L) {
    luaL_checktype(L, 2, LUA_TSTRING);
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 2, &length);

    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);
    if (blocked_extension(path)) {
        luaL_error(L, "unsafe function");
    }
    if (!write_file(path, data, length, false)) {
        luaL_error(L, "failed to write file");
    }
    return 0;
}

int fs_appendfile(lua_State* L) {
    luaL_checktype(L, 2, LUA_TSTRING);
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 2, &length);

    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);
    if (blocked_extension(path)) {
        luaL_error(L, "unsafe function");
    }
    if (!write_file(path, data, length, true)) {
        luaL_error(L, "failed to append file");
    }
    return 0;
}

int fs_listfiles(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    wchar_t root[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);
    if (!workspace_root(root, kWorkspacePathSize)) {
        luaL_error(L, "workspace path is unavailable");
    }

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        luaL_error(L, "folder does not exist");
    }

    lua_newtable(L);

    wchar_t pattern[kWorkspacePathSize] = {};
    swprintf_s(pattern, L"%s\\*", path);

    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return 1;
    }

    const std::size_t root_length = std::wcslen(root);
    int index = 1;
    do {
        if (std::wcscmp(entry.cFileName, L".") == 0 ||
            std::wcscmp(entry.cFileName, L"..") == 0) {
            continue;
        }

        const std::wstring full =
            std::wstring(path) + L'\\' + entry.cFileName;
        const wchar_t* relative = full.c_str();
        if (_wcsnicmp(full.c_str(), root, root_length) == 0 &&
            full[root_length] == L'\\') {
            relative = full.c_str() + root_length + 1;
        }

        const int needed = WideCharToMultiByte(CP_UTF8, 0, relative, -1,
                                               nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            continue;
        }

        std::string utf8(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, relative, -1, utf8.data(), needed,
                            nullptr, nullptr);
        lua_pushlstring(L, utf8.data(), utf8.size() - 1);
        lua_rawseti(L, -2, index++);
    } while (FindNextFileW(search, &entry));

    FindClose(search);
    return 1;
}

int fs_isfile(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);

    const DWORD attributes = GetFileAttributesW(path);
    lua_pushboolean(L, attributes != INVALID_FILE_ATTRIBUTES &&
                           !(attributes & FILE_ATTRIBUTE_DIRECTORY));
    return 1;
}

int fs_isfolder(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);

    const DWORD attributes = GetFileAttributesW(path);
    lua_pushboolean(L, attributes != INVALID_FILE_ATTRIBUTES &&
                           (attributes & FILE_ATTRIBUTE_DIRECTORY));
    return 1;
}

int fs_makefolder(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);
    if (!ensure_directory(path)) {
        luaL_error(L, "failed to create folder");
    }
    return 0;
}

int fs_delfile(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        luaL_error(L, "file does not exist");
    }
    if (!DeleteFileW(path)) {
        luaL_error(L, "failed to delete file");
    }
    return 0;
}

int fs_delfolder(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    wchar_t root[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);
    if (!workspace_root(root, kWorkspacePathSize)) {
        luaL_error(L, "workspace path is unavailable");
    }
    if (_wcsicmp(path, root) == 0) {
        luaL_error(L, "cannot delete workspace folder");
    }

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        luaL_error(L, "folder does not exist");
    }
    if (!delete_tree(path)) {
        luaL_error(L, "failed to remove folder");
    }
    return 0;
}

// --- crypt ---------------------------------------------------------------

constexpr std::size_t kAesKeySize = 32;
constexpr std::size_t kAesBlockSize = 16;

std::string encode_base64(const std::uint8_t* data, std::size_t length) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((length + 2) / 3) * 4);

    for (std::size_t i = 0; i < length; i += 3) {
        std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        const std::size_t available = std::min<std::size_t>(length - i, 3);
        if (available > 1) {
            triple |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        if (available > 2) {
            triple |= static_cast<std::uint32_t>(data[i + 2]);
        }

        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(available > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(available > 2 ? kAlphabet[triple & 0x3F] : '=');
    }

    return out;
}

bool decode_base64_char(std::uint8_t& out, char value) {
    if (value >= 'A' && value <= 'Z') {
        out = static_cast<std::uint8_t>(value - 'A');
        return true;
    }
    if (value >= 'a' && value <= 'z') {
        out = static_cast<std::uint8_t>(value - 'a' + 26);
        return true;
    }
    if (value >= '0' && value <= '9') {
        out = static_cast<std::uint8_t>(value - '0' + 52);
        return true;
    }
    if (value == '+') {
        out = 62;
        return true;
    }
    if (value == '/') {
        out = 63;
        return true;
    }
    return false;
}

bool decode_base64(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty()) {
        return true;
    }
    if (in.size() % 4 != 0) {
        return false;
    }

    out.reserve((in.size() / 4) * 3);

    for (std::size_t i = 0; i < in.size(); i += 4) {
        std::uint8_t values[4] = {0, 0, 0, 0};
        int padding = 0;

        for (int k = 0; k < 4; ++k) {
            const char value = in[i + static_cast<std::size_t>(k)];
            if (value == '=') {
                ++padding;
            } else if (!decode_base64_char(values[k], value)) {
                return false;
            }
        }

        const std::uint32_t triple =
            (static_cast<std::uint32_t>(values[0]) << 18) |
            (static_cast<std::uint32_t>(values[1]) << 12) |
            (static_cast<std::uint32_t>(values[2]) << 6) |
            static_cast<std::uint32_t>(values[3]);

        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (padding < 2) {
            out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            out.push_back(static_cast<char>(triple & 0xFF));
        }
    }

    return true;
}

bool random_bytes(std::string& out, std::size_t size) {
    out.resize(size);
    return BCRYPT_SUCCESS(BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(out.data()),
        static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool bcrypt_digest(LPCWSTR algorithm, const char* data, std::size_t length,
                   std::string& out) {
    BCRYPT_ALG_HANDLE provider = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&provider, algorithm,
                                                    nullptr, 0))) {
        return false;
    }

    DWORD digest_size = 0;
    DWORD property_size = 0;
    bool ok = BCRYPT_SUCCESS(BCryptGetProperty(
        provider, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
        &property_size, 0));

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok) {
        ok = BCRYPT_SUCCESS(
            BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0));
    }

    PUCHAR input = reinterpret_cast<PUCHAR>(const_cast<char*>(data));
    std::size_t offset = 0;
    while (ok && offset < length) {
        const ULONG chunk = static_cast<ULONG>(
            std::min<std::size_t>(length - offset, 0x100000));
        ok = BCRYPT_SUCCESS(
            BCryptHashData(hash, input + offset, chunk, 0));
        offset += chunk;
    }

    std::vector<unsigned char> digest(digest_size);
    if (ok) {
        ok = BCRYPT_SUCCESS(
            BCryptFinishHash(hash, digest.data(), digest_size, 0));
    }

    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(provider, 0);

    if (!ok) {
        return false;
    }

    static const char kHex[] = "0123456789abcdef";
    out.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return true;
}

bool aes_cbc(std::string& data, const std::string& key, const std::string& iv,
             bool encrypting) {
    if (key.size() != kAesKeySize || iv.size() < kAesBlockSize) {
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTKEY crypto_key = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        return false;
    }

    struct AesKeyBlob {
        BLOBHEADER header;
        DWORD key_size;
        BYTE key[kAesKeySize];
    } blob{};

    blob.header.bType = PLAINTEXTKEYBLOB;
    blob.header.bVersion = CUR_BLOB_VERSION;
    blob.header.aiKeyAlg = CALG_AES_256;
    blob.key_size = static_cast<DWORD>(kAesKeySize);
    std::memcpy(blob.key, key.data(), kAesKeySize);

    bool ok = CryptImportKey(provider, reinterpret_cast<BYTE*>(&blob),
                             sizeof(blob), 0, 0, &crypto_key) != FALSE;

    DWORD mode = CRYPT_MODE_CBC;
    ok = ok && CryptSetKeyParam(crypto_key, KP_MODE,
                                reinterpret_cast<BYTE*>(&mode), 0) != FALSE;
    ok = ok && CryptSetKeyParam(crypto_key, KP_IV,
                                reinterpret_cast<const BYTE*>(iv.data()),
                                0) != FALSE;

    if (ok && encrypting) {
        DWORD length = static_cast<DWORD>(data.size());
        const DWORD capacity = length + static_cast<DWORD>(kAesBlockSize);
        data.resize(capacity);
        ok = CryptEncrypt(crypto_key, 0, TRUE, 0,
                          reinterpret_cast<BYTE*>(data.data()), &length,
                          capacity) != FALSE;
        if (ok) {
            data.resize(length);
        }
    } else if (ok) {
        DWORD length = static_cast<DWORD>(data.size());
        ok = CryptDecrypt(crypto_key, 0, TRUE, 0,
                          reinterpret_cast<BYTE*>(data.data()), &length) !=
             FALSE;
        if (ok) {
            data.resize(length);
        }
    }

    if (crypto_key) {
        CryptDestroyKey(crypto_key);
    }
    CryptReleaseContext(provider, 0);
    return ok;
}

int crypt_base64encode(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string out = encode_base64(
        reinterpret_cast<const std::uint8_t*>(data), length);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int crypt_base64decode(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);

    std::string decoded;
    if (!decode_base64(std::string(data, length), decoded)) {
        luaL_error(L, "invalid base64");
    }
    lua_pushlstring(L, decoded.data(), decoded.size());
    return 1;
}

int crypt_hash(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const char* algorithm = luaL_optstring(L, 2, "sha384");

    std::string digest;
    bool ok = true;

    if (_stricmp(algorithm, "md5") == 0) {
        ok = bcrypt_digest(BCRYPT_MD5_ALGORITHM, data, length, digest);
    } else if (_stricmp(algorithm, "sha1") == 0) {
        ok = bcrypt_digest(BCRYPT_SHA1_ALGORITHM, data, length, digest);
    } else if (_stricmp(algorithm, "sha224") == 0) {
        digest = sha224_impl::sha224(data, length);
    } else if (_stricmp(algorithm, "sha256") == 0) {
        ok = bcrypt_digest(BCRYPT_SHA256_ALGORITHM, data, length, digest);
    } else if (_stricmp(algorithm, "sha384") == 0) {
        ok = bcrypt_digest(BCRYPT_SHA384_ALGORITHM, data, length, digest);
    } else if (_stricmp(algorithm, "sha512") == 0) {
        ok = bcrypt_digest(BCRYPT_SHA512_ALGORITHM, data, length, digest);
    } else if (_stricmp(algorithm, "sha3-224") == 0 ||
               _stricmp(algorithm, "sha3_224") == 0) {
        digest = sha3_impl::sha3(224, data, length);
    } else if (_stricmp(algorithm, "sha3-256") == 0 ||
               _stricmp(algorithm, "sha3_256") == 0) {
        digest = sha3_impl::sha3(256, data, length);
    } else if (_stricmp(algorithm, "sha3-384") == 0 ||
               _stricmp(algorithm, "sha3_384") == 0) {
        digest = sha3_impl::sha3(384, data, length);
    } else if (_stricmp(algorithm, "sha3-512") == 0 ||
               _stricmp(algorithm, "sha3_512") == 0) {
        digest = sha3_impl::sha3(512, data, length);
    } else {
        luaL_argerror(L, 2, "unsupported hash algorithm");
    }

    if (!ok) {
        luaL_error(L, "hash operation failed");
    }
    lua_pushlstring(L, digest.data(), digest.size());
    return 1;
}

int crypt_generatekey(lua_State* L) {
    std::string key;
    if (!random_bytes(key, kAesKeySize)) {
        luaL_error(L, "failed to generate a key");
    }

    const std::string encoded = encode_base64(
        reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
    lua_pushlstring(L, encoded.data(), encoded.size());
    return 1;
}

int crypt_encrypt(lua_State* L) {
    std::size_t data_length = 0;
    std::size_t key_length = 0;
    const char* data = luaL_checklstring(L, 1, &data_length);
    const char* encoded_key = luaL_checklstring(L, 2, &key_length);
    const char* mode = luaL_optstring(L, 4, "CBC");
    if (_stricmp(mode, "CBC") != 0) {
        luaL_argerror(L, 4, "only CBC mode is supported");
    }

    std::string key;
    if (!decode_base64(std::string(encoded_key, key_length), key) ||
        key.size() != kAesKeySize) {
        luaL_argerror(L, 2, "key must be a base64 encoded 256-bit key");
    }

    std::string iv;
    if (lua_isnoneornil(L, 3)) {
        if (!random_bytes(iv, kAesBlockSize)) {
            luaL_error(L, "failed to generate an IV");
        }
    } else {
        std::size_t iv_length = 0;
        const char* encoded_iv = luaL_checklstring(L, 3, &iv_length);
        if (!decode_base64(std::string(encoded_iv, iv_length), iv) ||
            iv.size() < kAesBlockSize) {
            luaL_argerror(L, 3, "IV must contain at least 16 decoded bytes");
        }
    }

    std::string encrypted(data, data_length);
    if (!aes_cbc(encrypted, key, iv, true)) {
        luaL_error(L, "AES-CBC encryption failed");
    }

    const std::string encoded = encode_base64(
        reinterpret_cast<const std::uint8_t*>(encrypted.data()),
        encrypted.size());
    const std::string encoded_iv = encode_base64(
        reinterpret_cast<const std::uint8_t*>(iv.data()), kAesBlockSize);

    lua_pushlstring(L, encoded.data(), encoded.size());
    lua_pushlstring(L, encoded_iv.data(), encoded_iv.size());
    return 2;
}

int crypt_decrypt(lua_State* L) {
    std::size_t data_length = 0;
    std::size_t key_length = 0;
    std::size_t iv_length = 0;
    const char* encoded_data = luaL_checklstring(L, 1, &data_length);
    const char* encoded_key = luaL_checklstring(L, 2, &key_length);
    const char* encoded_iv = luaL_checklstring(L, 3, &iv_length);
    const char* mode = luaL_optstring(L, 4, "CBC");
    if (_stricmp(mode, "CBC") != 0) {
        luaL_argerror(L, 4, "only CBC mode is supported");
    }

    std::string encrypted;
    std::string key;
    std::string iv;
    if (!decode_base64(std::string(encoded_data, data_length), encrypted)) {
        luaL_argerror(L, 1, "encrypted data is not valid base64");
    }
    if (!decode_base64(std::string(encoded_key, key_length), key) ||
        key.size() != kAesKeySize) {
        luaL_argerror(L, 2, "key must be a base64 encoded 256-bit key");
    }
    if (!decode_base64(std::string(encoded_iv, iv_length), iv) ||
        iv.size() < kAesBlockSize) {
        luaL_argerror(L, 3, "IV must contain at least 16 decoded bytes");
    }

    if (!aes_cbc(encrypted, key, iv, false)) {
        luaL_error(L, "AES-CBC decryption failed");
    }

    lua_pushlstring(L, encrypted.data(), encrypted.size());
    return 1;
}

void set_field_function(lua_State* L, const char* name, lua_CFunction function) {
    lua_pushcclosure(L, function, name, 0);
    lua_setfield(L, -2, name);
}

int loadstring(lua_State* L) {
    std::size_t length = 0;
    const char* source = luaL_checklstring(L, 1, &length);
    const char* chunk_name = luaL_optstring(L, 2, "=zend-loadstring");

    const auto push_compile_error = [&](const std::string& message) {
        std::string formatted;
        if (chunk_name && (chunk_name[0] == '=' || chunk_name[0] == '@')) {
            formatted.assign(chunk_name + 1);
        } else {
            formatted.assign("[string \"");
            formatted.append(chunk_name ? chunk_name : "?");
            formatted.append("\"]");
        }
        formatted.append(message);
        lua_pushnil(L);
        lua_pushlstring(L, formatted.data(), formatted.size());
    };

    // Luau bytecode is not source. The client stores a newer wire version with
    // its own opcode numbering; handing it to this VM would run invalid code
    // and crash the client. loadstring therefore always reports it as source
    // that failed to parse, matching the documented nil + error contract.
    if (looks_like_client_bytecode(source, length)) {
        push_compile_error(":1: bytecode is not source");
        return 2;
    }

    std::string bytecode;
    try {
        bytecode = compile(std::string(source, length));
    } catch (const std::exception& error) {
        push_compile_error(std::string(":1: compile failed: ") + error.what());
        return 2;
    }

    if (bytecode.empty() || bytecode[0] == '\0') {
        // Luau::compile stores compile errors as version-0 bytecode prefixed by
        // a NUL; the remainder already carries ":line: message".
        push_compile_error(bytecode.size() > 1
                               ? std::string(bytecode.c_str() + 1)
                               : std::string(":1: compile error"));
        return 2;
    }

    if (luau_load(L, chunk_name, bytecode.data(), bytecode.size(), 0) != 0) {
        // luau_load leaves the compile error on the stack.
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }

    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure && !closure->isC && closure->l.p) {
        executor::apply_capabilities(closure->l.p);
        g_own_closures.insert(closure);
    }
    return 1;
}

int loadfile(lua_State* L) {
    wchar_t path[kWorkspacePathSize] = {};
    resolve_path(L, 1, path, kWorkspacePathSize);

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        lua_pushnil(L);
        lua_pushfstring(L, "loadfile: '%s' does not exist", lua_tostring(L, 1));
        return 2;
    }

    std::string source;
    if (!read_file(path, source)) {
        lua_pushnil(L);
        lua_pushfstring(L, "loadfile: cannot read '%s'", lua_tostring(L, 1));
        return 2;
    }

    std::string bytecode;
    try {
        bytecode = compile(source);
    } catch (const std::exception& error) {
        lua_pushnil(L);
        lua_pushstring(L, error.what());
        return 2;
    }

    const char* chunk_name = luaL_optstring(L, 2, "@loadfile");
    if (luau_load(L, chunk_name, bytecode.data(), bytecode.size(), 0) != 0) {
        const std::string message =
            lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed";
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }

    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure && !closure->isC && closure->l.p) {
        executor::apply_capabilities(closure->l.p);
        g_own_closures.insert(closure);
    }
    return 1;
}

int getcustomasset(lua_State* L) {
    wchar_t source[kWorkspacePathSize] = {};
    resolve_path(L, 1, source, kWorkspacePathSize);

    const DWORD attributes = GetFileAttributesW(source);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        luaL_error(L, "getcustomasset: file does not exist");
    }

    wchar_t executable[MAX_PATH] = {};
    if (GetModuleFileNameW(GetModuleHandleW(nullptr), executable, MAX_PATH) == 0) {
        luaL_error(L, "getcustomasset: could not locate the client");
    }
    wchar_t* last_slash = std::wcsrchr(executable, L'\\');
    if (!last_slash) {
        luaL_error(L, "getcustomasset: could not locate the client");
    }
    last_slash[1] = L'\0';

    wchar_t directory[1024] = {};
    if (swprintf_s(directory, L"%sExtraContent\\zen", executable) < 0) {
        luaL_error(L, "getcustomasset: asset path is too long");
    }
    if (!ensure_directory(directory)) {
        luaL_error(L, "getcustomasset: could not create the asset folder");
    }

    const wchar_t* file_name = std::wcsrchr(source, L'\\');
    file_name = file_name ? file_name + 1 : source;

    wchar_t destination[1024] = {};
    if (swprintf_s(destination, L"%s\\%s", directory, file_name) < 0) {
        luaL_error(L, "getcustomasset: asset path is too long");
    }
    if (!CopyFileW(source, destination, FALSE)) {
        luaL_error(L, "getcustomasset: could not copy the asset");
    }

    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, file_name, -1, nullptr, 0, nullptr,
                            nullptr);
    if (needed <= 1) {
        luaL_error(L, "getcustomasset: invalid file name");
    }

    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file_name, -1, utf8.data(), needed,
                        nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(needed - 1));

    const std::string url = "rbxasset://zen/" + utf8;
    lua_pushlstring(L, url.data(), url.size());
    return 1;
}

void set_function(lua_State* L, const char* name, lua_CFunction function) {
    lua_pushcclosure(L, function, name, 0);
    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure) {
        g_own_closures.insert(closure);
    }
    lua_setglobal(L, name);
}

// Same as set_function but with a continuation, required for anything that
// may yield through a C frame (hook dispatchers and newcclosure wrappers).
void set_function_k(lua_State* L, const char* name, lua_CFunction function,
                    lua_Continuation continuation) {
    lua_pushcclosurek(L, function, name, 0, continuation);
    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure) {
        g_own_closures.insert(closure);
    }
    lua_setglobal(L, name);
}

// Mirrors one of our globals into the client's debug table. The client ships
// debug as a read-only table; clearing the flag once is enough for this VM.
void mirror_debug(lua_State* L, const char* name) {
    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_setreadonly(L, -1, 0);
    lua_getglobal(L, name);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void install_filesystem(lua_State* L) {
    set_function(L, "readfile", fs_readfile);
    set_function(L, "writefile", fs_writefile);
    set_function(L, "appendfile", fs_appendfile);
    set_function(L, "listfiles", fs_listfiles);
    set_function(L, "isfile", fs_isfile);
    set_function(L, "isfolder", fs_isfolder);
    set_function(L, "makefolder", fs_makefolder);
    set_function(L, "delfile", fs_delfile);
    set_function(L, "delfolder", fs_delfolder);
    set_function(L, "loadfile", loadfile);
    set_function(L, "getcustomasset", getcustomasset);
}

// --- LZ4 block codec ------------------------------------------------------
//
// The wire format is the raw LZ4 block format (token nibbles, 16-bit
// little-endian offsets), which is what every executor exposes here.

void lz4_write_length(std::vector<std::uint8_t>& out, std::size_t length) {
    while (length >= 255) {
        out.push_back(255);
        length -= 255;
    }
    out.push_back(static_cast<std::uint8_t>(length));
}

std::string lz4_compress_block(const std::uint8_t* source, std::size_t size) {
    constexpr std::size_t kHashSize = 1u << 16;
    std::vector<int> table(kHashSize, -1);

    std::vector<std::uint8_t> out;
    out.reserve(size + size / 8 + 16);

    auto read32 = [source](std::size_t pos) -> std::uint32_t {
        return static_cast<std::uint32_t>(source[pos]) |
               (static_cast<std::uint32_t>(source[pos + 1]) << 8) |
               (static_cast<std::uint32_t>(source[pos + 2]) << 16) |
               (static_cast<std::uint32_t>(source[pos + 3]) << 24);
    };

    std::size_t anchor = 0;
    std::size_t i = 0;
    while (i + 4 <= size) {
        const std::uint32_t sequence = read32(i);
        const std::size_t slot = (sequence * 2654435761u) >> 16;
        const int previous = table[slot];
        table[slot] = static_cast<int>(i);

        if (previous < 0) {
            ++i;
            continue;
        }

        const std::size_t match = static_cast<std::size_t>(previous);
        if (i <= match || i - match > 0xFFFF || match + 4 > size ||
            read32(match) != sequence) {
            ++i;
            continue;
        }

        std::size_t length = 4;
        while (i + length < size && match + length < size &&
               source[i + length] == source[match + length]) {
            ++length;
        }

        const std::size_t literal = i - anchor;
        const std::uint8_t token = static_cast<std::uint8_t>(
            ((std::min)(literal, std::size_t{15}) << 4) |
            (std::min)(length - 4, std::size_t{15}));
        out.push_back(token);
        if (literal >= 15) {
            lz4_write_length(out, literal - 15);
        }
        out.insert(out.end(), source + anchor, source + i);

        const std::uint16_t offset = static_cast<std::uint16_t>(i - match);
        out.push_back(static_cast<std::uint8_t>(offset & 0xFF));
        out.push_back(static_cast<std::uint8_t>((offset >> 8) & 0xFF));
        if (length - 4 >= 15) {
            lz4_write_length(out, length - 4 - 15);
        }

        i += length;
        anchor = i;
    }

    if (anchor < size) {
        const std::size_t literal = size - anchor;
        out.push_back(static_cast<std::uint8_t>(
            (std::min)(literal, std::size_t{15}) << 4));
        if (literal >= 15) {
            lz4_write_length(out, literal - 15);
        }
        out.insert(out.end(), source + anchor, source + size);
    }

    return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

bool lz4_read_length(const std::uint8_t* input, std::size_t size,
                     std::size_t& position, std::size_t& length) {
    std::size_t extra = 0;
    while (true) {
        if (position >= size) {
            return false;
        }
        const std::uint8_t byte = input[position++];
        extra += byte;
        if (byte != 255) {
            break;
        }
    }
    length += extra;
    return true;
}

bool lz4_decompress_block(const std::uint8_t* input, std::size_t size,
                          std::string& out) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(size * 3);

    std::size_t position = 0;
    while (position < size) {
        const std::uint8_t token = input[position++];
        std::size_t literals = token >> 4;
        std::size_t matches = token & 0x0F;

        if (literals == 15 &&
            !lz4_read_length(input, size, position, literals)) {
            return false;
        }
        if (position + literals > size) {
            return false;
        }
        buffer.insert(buffer.end(), input + position, input + position + literals);
        position += literals;

        if (position >= size) {
            break;
        }
        if (position + 2 > size) {
            return false;
        }

        const std::uint16_t offset =
            static_cast<std::uint16_t>(input[position]) |
            (static_cast<std::uint16_t>(input[position + 1]) << 8);
        position += 2;
        if (offset == 0 || offset > buffer.size()) {
            return false;
        }

        matches += 4;
        if ((token & 0x0F) == 15 &&
            !lz4_read_length(input, size, position, matches)) {
            return false;
        }

        const std::size_t start = buffer.size() - offset;
        for (std::size_t index = 0; index < matches; ++index) {
            buffer.push_back(buffer[start + index]);
        }
    }

    out.assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return true;
}

int lz4compress(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string out = lz4_compress_block(
        reinterpret_cast<const std::uint8_t*>(data), length);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int lz4decompress(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    std::string out;
    if (!lz4_decompress_block(reinterpret_cast<const std::uint8_t*>(data),
                              length, out)) {
        luaL_error(L, "invalid lz4 data");
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

void install_crypt(lua_State* L) {
    set_function(L, "base64encode", crypt_base64encode);
    set_function(L, "base64decode", crypt_base64decode);
    set_function(L, "base64_encode", crypt_base64encode);
    set_function(L, "base64_decode", crypt_base64decode);
    set_function(L, "lz4compress", lz4compress);
    set_function(L, "lz4decompress", lz4decompress);

    lua_newtable(L);
    set_field_function(L, "encode", crypt_base64encode);
    set_field_function(L, "decode", crypt_base64decode);
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "base64");

    lua_newtable(L);
    set_field_function(L, "base64encode", crypt_base64encode);
    set_field_function(L, "base64decode", crypt_base64decode);
    set_field_function(L, "base64_encode", crypt_base64encode);
    set_field_function(L, "base64_decode", crypt_base64decode);
    set_field_function(L, "encrypt", crypt_encrypt);
    set_field_function(L, "decrypt", crypt_decrypt);
    set_field_function(L, "generatekey", crypt_generatekey);
    set_field_function(L, "hash", crypt_hash);

    lua_newtable(L);
    set_field_function(L, "encode", crypt_base64encode);
    set_field_function(L, "decode", crypt_base64decode);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "base64");

    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "crypt");
}

// --- HTTP ----------------------------------------------------------------
//
// WinHTTP does the transport on a detached worker thread; the Lua side polls
// from a task.wait loop so a slow request never blocks the client scheduler.
// The reference blocks profanity: keep its privacy blocklist verbatim.

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string status_text;
    std::string body;
    std::string error;
    std::vector<HttpHeader> headers;
};

struct HttpJob {
    std::mutex mutex;
    bool done = false;
    HttpResult result;
};

std::mutex g_http_mutex;
std::unordered_map<int, std::shared_ptr<HttpJob>> g_http_jobs;
int g_http_next = 1;

const char* const kBlockedHttpDomains[] = {
    "accountinformation.roblox.com", "accountsettings.roblox.com",
    "twostepverification.roblox.com", "trades.roblox.com",
    "billing.roblox.com", "economy.roblox.com", "auth.roblox.com",
    "accountinformation.roproxy.com", "accountsettings.roproxy.com",
    "twostepverification.roproxy.com", "trades.roproxy.com",
    "billing.roproxy.com", "economy.roproxy.com", "auth.roproxy.com",
    "api.ipify.org", "api4.ipify.org", "api6.ipify.org", "api64.ipify.org",
    "api.ipapi.is", "api.ipapi.co", "api.ipapi.com",
    "api.ipinfo.io", "api.ipgeolocation.io", "api.freegeoip.app",
    "api.myip.com", "api.ip.sb", "api.ipwhois.io",
    "api.ipdata.co", "api.ipstack.com", "api.db-ip.com",
    "api.seeip.org", "api.ipregistry.co", "api.abstractapi.com",
    "api.ipbase.com", "api.iplocation.net", "api.ip.nf",
    "api.bigdatacloud.net", "api.techniknews.net", "api.country.is",
    "api.2ip.me", "api.reallyfreegeoip.org",
};

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string narrow_utf8(const wchar_t* text, int length) {
    if (!text || length <= 0) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, narrow.data(), size, nullptr,
                        nullptr);
    return narrow;
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool is_blocked_url(const std::string& lower_url) {
    for (const char* domain : kBlockedHttpDomains) {
        if (lower_url.find(domain) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string status_phrase(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 418: return "I'm a teapot";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Time-out";
    default: return {};
    }
}

std::vector<HttpHeader> parse_http_headers(const std::string& raw) {
    std::vector<HttpHeader> headers;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos) {
            end = raw.size();
        }
        std::string line = raw.substr(pos, end - pos);
        pos = end + 2;

        if (line.empty() || line.rfind("HTTP/", 0) == 0) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        headers.push_back({std::move(name), std::move(value)});
    }
    return headers;
}

struct HttpRequestSpec {
    std::wstring host;
    std::wstring path;
    std::wstring method;
    INTERNET_PORT port = 0;
    bool secure = false;
    std::vector<HttpHeader> headers;
    std::string body;
};

bool query_header(HINTERNET request, DWORD query, std::wstring& out) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return false;
    }
    std::wstring buffer(static_cast<std::size_t>(size) / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    buffer.resize(static_cast<std::size_t>(size) / sizeof(wchar_t));
    while (!buffer.empty() &&
           (buffer.back() == L'\0' || buffer.back() == L'\r' ||
            buffer.back() == L'\n')) {
        buffer.pop_back();
    }
    out = std::move(buffer);
    return true;
}

HttpResult perform_http(const HttpRequestSpec& spec) {
    HttpResult result;

    HINTERNET session = WinHttpOpen(
        L"Roblox/WinInet", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = "WinHttpOpen failed (" +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                     sizeof(redirects));

    HINTERNET connect = WinHttpConnect(session, spec.host.c_str(), spec.port, 0);
    if (!connect) {
        result.error = "WinHttpConnect failed (" +
                       std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(session);
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, spec.method.c_str(), spec.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        spec.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        result.error = "WinHttpOpenRequest failed (" +
                       std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    for (const HttpHeader& header : spec.headers) {
        const std::wstring line = widen_utf8(header.name + ": " + header.value);
        WinHttpAddRequestHeaders(request, line.c_str(),
                                 static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD |
                                     WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    const DWORD body_size = static_cast<DWORD>(spec.body.size());
    if (!WinHttpSendRequest(
            request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            spec.body.empty() ? WINHTTP_NO_REQUEST_DATA
                              : const_cast<char*>(spec.body.data()),
            body_size, body_size, 0)) {
        result.error = "WinHttpSendRequest failed (" +
                       std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        result.error = "WinHttpReceiveResponse failed (" +
                       std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &status_size, WINHTTP_NO_HEADER_INDEX)) {
        result.error = "WinHttpQueryHeaders(status) failed (" +
                       std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }
    result.status = static_cast<int>(status);

    std::wstring status_text;
    if (query_header(request, WINHTTP_QUERY_STATUS_TEXT, status_text)) {
        result.status_text = narrow_utf8(status_text.data(),
                                         static_cast<int>(status_text.size()));
    }
    if (result.status_text.empty()) {
        result.status_text = status_phrase(result.status);
    }

    std::wstring raw;
    if (query_header(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, raw)) {
        result.headers = parse_http_headers(
            narrow_utf8(raw.data(), static_cast<int>(raw.size())));
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            result.error = "WinHttpQueryDataAvailable failed (" +
                           std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return result;
        }
        if (available == 0) {
            break;
        }
        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            result.error = "WinHttpReadData failed (" +
                           std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return result;
        }
        chunk.resize(static_cast<std::size_t>(read));
        result.body += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    result.ok = true;
    return result;
}

int http_start(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "Url");
    if (!lua_isstring(L, -1)) {
        luaL_error(L, "request: missing or invalid 'Url'");
    }
    std::string url = lua_tostring(L, -1);
    lua_pop(L, 1);

    const std::string lower_url = ascii_lower(url);
    bool secure = false;
    if (lower_url.rfind("http://", 0) == 0) {
        secure = false;
    } else if (lower_url.rfind("https://", 0) == 0) {
        secure = true;
    } else {
        luaL_error(L, "invalid protocol (expected 'http' or 'https')");
    }
    if (is_blocked_url(lower_url)) {
        luaL_error(L, "url is blocked");
    }

    std::string method = "GET";
    lua_getfield(L, 1, "Method");
    if (!lua_isnil(L, -1)) {
        if (!lua_isstring(L, -1)) {
            luaL_error(L, "request: 'Method' must be a string");
        }
        method = lua_tostring(L, -1);
        std::transform(method.begin(), method.end(), method.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::toupper(c));
                       });
        static const char* const kMethods[] = {
            "GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "PATCH",
        };
        bool known = false;
        for (const char* candidate : kMethods) {
            if (method == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            luaL_error(L, "request: invalid method '%s'", method.c_str());
        }
    }
    lua_pop(L, 1);

    HttpRequestSpec spec;
    spec.secure = secure;
    spec.method = widen_utf8(method);

    std::string body;
    lua_getfield(L, 1, "Body");
    if (lua_isstring(L, -1)) {
        if (method == "GET" || method == "HEAD") {
            luaL_error(L, "request: 'Body' is not allowed for GET/HEAD");
        }
        body = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    spec.body = body;

    lua_getfield(L, 1, "Headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (!lua_isstring(L, -2) || !lua_isstring(L, -1)) {
                luaL_error(L, "request: 'Headers' must be a string map");
            }
            const std::string name = lua_tostring(L, -2);
            const std::string value = lua_tostring(L, -1);
            if (ascii_lower(name) == "content-length") {
                luaL_error(L, "request: 'Content-Length' cannot be set");
            }
            spec.headers.push_back({name, value});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "Cookies");
    if (lua_istable(L, -1)) {
        std::string cookie_header;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (!lua_isstring(L, -2) || !lua_isstring(L, -1)) {
                luaL_error(L, "request: 'Cookies' must be a string map");
            }
            if (!cookie_header.empty()) {
                cookie_header += "; ";
            }
            cookie_header += lua_tostring(L, -2);
            cookie_header += "=";
            cookie_header += lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        if (!cookie_header.empty()) {
            spec.headers.push_back({"Cookie", cookie_header});
        }
    }
    lua_pop(L, 1);

    bool has_user_agent = false;
    bool has_accept = false;
    for (const HttpHeader& header : spec.headers) {
        const std::string name = ascii_lower(header.name);
        has_user_agent = has_user_agent || name == "user-agent";
        has_accept = has_accept || name == "accept";
    }
    if (!has_user_agent) {
        spec.headers.push_back({"User-Agent", "Roblox/WinInet"});
    }
    if (!has_accept) {
        spec.headers.push_back({"Accept", "*/*"});
    }

    const std::wstring wide_url = widen_utf8(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(),
                         static_cast<DWORD>(wide_url.size()), 0,
                         &components)) {
        luaL_error(L, "request: invalid URL (WinHttpCrackUrl failed)");
    }

    spec.host.assign(components.lpszHostName, components.dwHostNameLength);
    spec.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    spec.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (spec.path.empty()) {
        spec.path = L"/";
    }
    spec.port = components.nPort;

    auto job = std::make_shared<HttpJob>();
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        id = g_http_next++;
        g_http_jobs[id] = job;
    }

    try {
        std::thread([job, spec = std::move(spec)]() mutable {
            HttpResult result = perform_http(spec);
            std::lock_guard<std::mutex> lock(job->mutex);
            job->result = std::move(result);
            job->done = true;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        g_http_jobs.erase(id);
        luaL_error(L, "request: could not start the HTTP worker");
    }

    lua_pushinteger(L, id);
    return 1;
}

int http_poll(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));

    std::shared_ptr<HttpJob> job;
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        auto it = g_http_jobs.find(id);
        if (it != g_http_jobs.end()) {
            job = it->second;
        }
    }
    if (!job) {
        luaL_error(L, "request: unknown request");
    }

    HttpResult result;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (!job->done) {
            lua_pushboolean(L, 0);
            return 1;
        }
        result = std::move(job->result);
    }
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        g_http_jobs.erase(id);
    }

    if (!result.ok) {
        luaL_error(L, "request failed: %s", result.error.c_str());
    }

    lua_pushboolean(L, 1);
    lua_newtable(L);

    lua_pushboolean(L, result.status >= 200 && result.status < 300);
    lua_setfield(L, -2, "Success");
    lua_pushinteger(L, result.status);
    lua_setfield(L, -2, "StatusCode");
    lua_pushlstring(L, result.status_text.data(), result.status_text.size());
    lua_setfield(L, -2, "StatusMessage");

    lua_newtable(L);
    for (const HttpHeader& header : result.headers) {
        lua_pushlstring(L, header.name.data(), header.name.size());
        lua_pushlstring(L, header.value.data(), header.value.size());
        lua_settable(L, -3);
    }
    lua_setfield(L, -2, "Headers");

    lua_newtable(L);
    for (const HttpHeader& header : result.headers) {
        if (ascii_lower(header.name) != "set-cookie") {
            continue;
        }
        const std::size_t equals = header.value.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::size_t semicolon = header.value.find(';');
        const std::size_t end = semicolon == std::string::npos
                                    ? header.value.size()
                                    : semicolon;
        lua_pushlstring(L, header.value.data(), equals);
        lua_pushlstring(L, header.value.data() + equals + 1,
                        end - equals - 1);
        lua_settable(L, -3);
    }
    lua_setfield(L, -2, "Cookies");

    lua_pushlstring(L, result.body.data(), result.body.size());
    lua_setfield(L, -2, "Body");
    return 2;
}

// The polling loop is written in Luau so each wait goes through task.wait:
// the client scheduler keeps stepping while the worker thread is in WinHTTP.
const char* const kHttpPrelude = R"LUA(
local function do_request(options)
    if type(options) ~= "table" then
        error("request expects a table", 2)
    end

    local token = __zend_http_start(options)
    while true do
        local done
        local result
        done, result = __zend_http_poll(token)
        if done then
            return result
        end
        task.wait(0.03)
    end
end

local function pick_url(first, second)
    if type(first) == "string" then
        return first
    end
    if type(second) == "string" then
        return second
    end
    error("url must be a string", 3)
end

local function do_get(first, second)
    local result = do_request({ Url = pick_url(first, second), Method = "GET" })
    if not result.Success then
        error("http_get returned status: " .. tostring(result.StatusCode), 2)
    end
    return result.Body
end

local function do_post(first, second, third, fourth)
    local url, body, content_type
    if type(first) == "string" then
        url, body, content_type = first, second, third
    else
        url, body, content_type = second, third, fourth
    end
    local types = {
        "application/json",
        "application/xml",
        "application/x-www-form-urlencoded",
        "text/plain",
        "text/xml",
    }
    if content_type == nil then
        content_type = "application/json"
    elseif type(content_type) == "number" then
        content_type = types[content_type + 1] or "application/json"
    end
    local result = do_request({
        Url = url,
        Method = "POST",
        Body = body or "",
        Headers = { ["Content-Type"] = content_type },
    })
    if not result.Success then
        error("http_post returned status: " .. tostring(result.StatusCode), 2)
    end
    return result.Body
end

request = do_request
http_request = do_request
httprequest = do_request
syn_request = do_request
krnl_request = do_request
webrequest = do_request

HttpGet = do_get
httpget = do_get
http_get = do_get
HttpGetAsync = do_get
httpgetasync = do_get
syn_httpget = do_get
gethttp = do_get
httpGet = do_get

HttpPost = do_post
httppost = do_post
http_post = do_post
HttpPostAsync = do_post
httppostasync = do_post
syn_httppost = do_post
posthttp = do_post
httpPost = do_post

http = {
    request = do_request,
    Request = do_request,
    http_request = do_request,
}

)LUA";

// Adds HttpGet/HttpPost to the DataModel without replacing its metatable. The
// shared instance __index/__namecall closures are routed through wrappers that
// only special-case `game`; every other key and object keeps the native path.
void install_instance_http(lua_State* L) {
    const int base = lua_gettop(L);

    lua_getglobal(L, "HttpGet");
    g_http_get_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_getglobal(L, "HttpPost");
    g_http_post_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_getglobal(L, "game");
    if (!lua_isuserdata(L, -1)) {
        lua_settop(L, base);
        return;
    }
    g_game_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_getglobal(L, "game");
    if (!lua_getmetatable(L, -1)) {
        lua_settop(L, base);
        return;
    }

    lua_rawgetfield(L, -1, "__index");
    if (lua_isfunction(L, -1)) {
        Closure* method = clvalue(luaA_toobject(L, -1));
        if (method && method->isC && method->c.f != instance_index_wrapper) {
            g_instance_index = method->c.f;
            method->c.f = instance_index_wrapper;
        }
    }
    lua_pop(L, 1);

    lua_rawgetfield(L, -1, "__namecall");
    if (lua_isfunction(L, -1)) {
        Closure* method = clvalue(luaA_toobject(L, -1));
        if (method && method->isC && method->c.f != instance_namecall_wrapper) {
            g_instance_namecall = method->c.f;
            method->c.f = instance_namecall_wrapper;
            method->c.cont = dispatch_continuation;
        }
    }
    lua_pop(L, 1);

    lua_settop(L, base);
}

void install_http(lua_State* L) {
    set_function(L, "__zend_http_start", http_start);
    set_function(L, "__zend_http_poll", http_poll);

    const std::string bytecode = compile(kHttpPrelude);
    if (luau_load(L, "=zend-http", bytecode.data(), bytecode.size(), 0) != 0) {
        lua_pop(L, 1);
        return;
    }
    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure && !closure->isC && closure->l.p) {
        executor::apply_capabilities(closure->l.p);
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        lua_pop(L, 1);
    }

    install_instance_http(L);
}

// --- WebSocket ------------------------------------------------------------
//
// WinHTTP owns the transport. A detached worker performs the upgrade and the
// blocking receive loop; it only touches C++ state. Lua never runs off the
// client thread: the prelude polls the receive queue from a task.wait loop and
// fires OnMessage/OnClose there. Sends are serialized with the worker through
// send_mutex because WinHTTP allows one send and one receive at a time.

struct WebSocketSocket {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;

    std::mutex send_mutex;
    std::mutex queue_mutex;
    std::deque<std::string> outbox;
    std::deque<std::string> inbox;

    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> closed{false};
    bool close_reported = false;
};

std::mutex g_ws_mutex;
std::unordered_map<int, std::shared_ptr<WebSocketSocket>> g_ws_sockets;
int g_ws_next_id = 1;

std::string websocket_hex_preview(const char* data, std::size_t size);

bool websocket_is_utf8(const char* text, std::size_t size) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    for (std::size_t index = 0; index < size;) {
        const unsigned char lead = bytes[index];
        std::size_t extra = 0;
        unsigned int codepoint = 0;

        if (lead < 0x80) {
            ++index;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = lead & 0x07;
        } else {
            return false;
        }

        if (index + extra >= size) {
            return false;
        }
        for (std::size_t byte = 1; byte <= extra; ++byte) {
            const unsigned char next = bytes[index + byte];
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3F);
        }

        if ((extra == 1 && codepoint < 0x80) ||
            (extra == 2 && codepoint < 0x800) ||
            (extra == 3 && codepoint < 0x10000) ||
            codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
        index += extra + 1;
    }
    return true;
}

bool websocket_handshake(WebSocketSocket& socket, const std::wstring& host,
                         const std::wstring& path, INTERNET_PORT port,
                         bool secure) {
    HINTERNET session =
        WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

    HINTERNET connection =
        WinHttpConnect(session, host.c_str(), port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET websocket = nullptr;
    if (WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                         nullptr, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    }
    WinHttpCloseHandle(request);

    if (!websocket) {
        if (env_trace()) {
            DWORD status = 0;
            DWORD status_size = sizeof(status);
            WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                WINHTTP_NO_HEADER_INDEX);
            zend::logf("env.websocket.handshake: failed err=%lu status=%lu",
                       static_cast<unsigned long>(GetLastError()),
                       static_cast<unsigned long>(status));
        }
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::lock_guard<std::mutex> lock(socket.send_mutex);
    socket.session = session;
    socket.connection = connection;
    socket.socket = websocket;
    socket.connected.store(true, std::memory_order_release);

    while (!socket.outbox.empty() && !socket.closing.load(std::memory_order_acquire)) {
        const std::string& message = socket.outbox.front();
        const bool text = websocket_is_utf8(message.data(), message.size());
        WinHttpWebSocketSend(
            websocket,
            text ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                 : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(message.data()),
            static_cast<DWORD>(message.size()));
        socket.outbox.pop_front();
    }
    return true;
}

void websocket_worker(std::shared_ptr<WebSocketSocket> socket,
                      std::wstring host, std::wstring path, INTERNET_PORT port,
                      bool secure) {
    if (!websocket_handshake(*socket, host, path, port, secure)) {
        std::lock_guard<std::mutex> lock(socket->queue_mutex);
        socket->closed.store(true, std::memory_order_release);
        return;
    }

    std::string partial;
    char buffer[8192];

    while (!socket->closing.load(std::memory_order_acquire)) {
        DWORD received = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;

        const DWORD status = WinHttpWebSocketReceive(
            socket->socket, buffer, sizeof(buffer), &received, &type);
        if (status == ERROR_WINHTTP_TIMEOUT) {
            continue;
        }
        if (status != NO_ERROR ||
            type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            if (env_trace()) {
                zend::logf("env.websocket.worker: recv status=%lu type=%d "
                           "bytes=%lu",
                           static_cast<unsigned long>(status),
                           static_cast<int>(type),
                           static_cast<unsigned long>(received));
            }
            break;
        }
        if (env_trace()) {
            zend::logf("env.websocket.worker: recv type=%d bytes=%lu hex=%s",
                       static_cast<int>(type),
                       static_cast<unsigned long>(received),
                       websocket_hex_preview(buffer, received).c_str());
        }

        partial.append(buffer, received);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            std::lock_guard<std::mutex> lock(socket->queue_mutex);
            if (socket->inbox.size() < 4096) {
                socket->inbox.push_back(std::move(partial));
            }
            partial.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(socket->send_mutex);
        if (socket->socket) {
            WinHttpWebSocketClose(socket->socket,
                                  WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr,
                                  0);
            WinHttpCloseHandle(socket->socket);
            socket->socket = nullptr;
        }
        if (socket->connection) {
            WinHttpCloseHandle(socket->connection);
            socket->connection = nullptr;
        }
        if (socket->session) {
            WinHttpCloseHandle(socket->session);
            socket->session = nullptr;
        }
    }

    socket->closed.store(true, std::memory_order_release);
}

std::shared_ptr<WebSocketSocket> find_websocket(int id) {
    std::lock_guard<std::mutex> lock(g_ws_mutex);
    const auto entry = g_ws_sockets.find(id);
    return entry == g_ws_sockets.end() ? nullptr : entry->second;
}

std::string websocket_hex_preview(const char* data, std::size_t size) {
    static const char* const digits = "0123456789abcdef";
    const std::size_t limit = size < 48 ? size : 48;
    std::string out;
    out.reserve(limit * 2);
    for (std::size_t index = 0; index < limit; ++index) {
        const auto byte = static_cast<unsigned char>(data[index]);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

int websocket_connect(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);

    std::string address(url);
    bool secure = false;
    if (address.rfind("wss://", 0) == 0) {
        secure = true;
        address.replace(0, 6, "https://");
    } else if (address.rfind("ws://", 0) == 0) {
        address.replace(0, 5, "http://");
    } else if (address.rfind("https://", 0) == 0) {
        secure = true;
    } else if (address.rfind("http://", 0) != 0) {
        luaL_error(L, "WebSocket: invalid url '%s'", url);
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    const std::wstring wide_address = widen_utf8(address);
    if (wide_address.empty() ||
        !WinHttpCrackUrl(wide_address.c_str(), 0, 0, &components)) {
        luaL_error(L, "WebSocket: malformed url '%s'", url);
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }
    if (components.nPort == 443) {
        secure = true;
    }

    if (env_trace()) {
        zend::logf("env.websocket.connect: url=%s secure=%d port=%u",
                   url, secure ? 1 : 0,
                   static_cast<unsigned>(components.nPort));
    }

    auto socket = std::make_shared<WebSocketSocket>();
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(g_ws_mutex);
        id = g_ws_next_id++;
        g_ws_sockets[id] = socket;
    }

    std::thread(websocket_worker, socket, host, path,
                static_cast<INTERNET_PORT>(components.nPort), secure)
        .detach();

    lua_pushinteger(L, id);
    return 1;
}

int websocket_send(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    std::size_t length = 0;
    const char* message = luaL_checklstring(L, 2, &length);
    const bool binary = !lua_isnoneornil(L, 3) && lua_toboolean(L, 3) != 0;

    const auto socket = find_websocket(id);
    if (!socket || socket->closed.load(std::memory_order_acquire)) {
        luaL_error(L, "WebSocket is closed");
    }

    const bool text = !binary && websocket_is_utf8(message, length);
    if (env_trace()) {
        zend::logf("env.websocket.send: id=%d bytes=%zu text=%d hex=%s", id,
                   length, text ? 1 : 0,
                   websocket_hex_preview(message, length).c_str());
    }

    std::lock_guard<std::mutex> lock(socket->send_mutex);
    if (!socket->connected.load(std::memory_order_acquire)) {
        if (socket->outbox.size() < 256) {
            socket->outbox.emplace_back(message, length);
        }
        return 0;
    }
    if (!socket->socket) {
        luaL_error(L, "WebSocket is closed");
    }

    const DWORD status = WinHttpWebSocketSend(
        socket->socket,
        text ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
             : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message), static_cast<DWORD>(length));
    if (status != NO_ERROR) {
        luaL_error(L, "WebSocket send failed (%d)", static_cast<int>(status));
    }
    return 0;
}

int websocket_close(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const auto socket = find_websocket(id);
    if (!socket) {
        return 0;
    }

    socket->closing.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(socket->send_mutex);
    if (socket->socket && socket->connected.load(std::memory_order_acquire)) {
        WinHttpWebSocketShutdown(socket->socket,
                                 WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                 nullptr, 0);
    }
    return 0;
}

int websocket_poll(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const auto socket = find_websocket(id);

    lua_newtable(L);
    if (!socket) {
        lua_pushboolean(L, 1);
        return 2;
    }

    std::deque<std::string> messages;
    bool closed = false;
    {
        std::lock_guard<std::mutex> lock(socket->queue_mutex);
        messages.swap(socket->inbox);
        if (socket->closed.load(std::memory_order_acquire) &&
            !socket->close_reported) {
            socket->close_reported = true;
            closed = true;
        }
    }

    int index = 1;
    for (const std::string& message : messages) {
        if (env_trace()) {
            zend::logf("env.websocket.poll: bytes=%zu hex=%s", message.size(),
                       websocket_hex_preview(message.data(), message.size())
                           .c_str());
        }
        lua_pushlstring(L, message.data(), message.size());
        lua_rawseti(L, -2, index++);
    }

    if (env_trace() && closed) {
        zend::logf("env.websocket.poll: closed");
    }

    lua_pushboolean(L, closed ? 1 : 0);
    return 2;
}

int websocket_release(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    std::lock_guard<std::mutex> lock(g_ws_mutex);
    g_ws_sockets.erase(id);
    return 0;
}

const char* const kWebSocketPrelude = R"LUA(
local Signal = {}
Signal.__index = Signal

function Signal.new()
    return setmetatable({ _handlers = {} }, Signal)
end

function Signal:Connect(handler)
    if type(handler) ~= "function" then
        error("handler must be a function", 2)
    end

    local connection = { _signal = self, _handler = handler, _dead = false }

    function connection:Disconnect()
        if self._dead then
            return
        end
        self._dead = true
        local handlers = self._signal._handlers
        for index = 1, #handlers do
            if handlers[index] == self then
                table.remove(handlers, index)
                break
            end
        end
    end

    connection.disconnect = connection.Disconnect
    connection.Destroy = connection.Disconnect
    table.insert(self._handlers, connection)
    return connection
end
Signal.connect = Signal.Connect

function Signal:Once(handler)
    local connection
    connection = self:Connect(function(...)
        connection:Disconnect()
        handler(...)
    end)
    return connection
end
Signal.once = Signal.Once

function Signal:Fire(...)
    local snapshot = {}
    for index, connection in ipairs(self._handlers) do
        snapshot[index] = connection
    end
    for _, connection in ipairs(snapshot) do
        if not connection._dead then
            task.spawn(connection._handler, ...)
        end
    end
end

function Signal:Wait()
    local thread = coroutine.running()
    local fired = false
    self:Once(function(...)
        if fired then
            return
        end
        fired = true
        task.spawn(thread, ...)
    end)
    return coroutine.yield()
end
Signal.wait = Signal.Wait

local WebSocket = {}
WebSocket.__index = WebSocket

function WebSocket.connect(url)
    if type(url) ~= "string" then
        error("WebSocket.connect expects a string url", 2)
    end

    local id = __zend_ws_connect(url)
    local self = setmetatable({
        _id = id,
        _closed = false,
        OnMessage = Signal.new(),
        OnClose = Signal.new(),
    }, WebSocket)

    task.spawn(function()
        while not self._closed do
            local messages, closed = __zend_ws_poll(self._id)
            for index = 1, #messages do
                self.OnMessage:Fire(messages[index])
            end
            if closed then
                self._closed = true
                __zend_ws_release(self._id)
                self.OnClose:Fire()
                break
            end
            task.wait(0.03)
        end
    end)

    return self
end

function WebSocket:Send(message, binary)
    if self._closed then
        error("WebSocket is closed", 2)
    end
    if type(message) ~= "string" then
        message = tostring(message)
    end
    __zend_ws_send(self._id, message, binary == true)
end

function WebSocket:Close()
    if self._closed then
        return
    end
    __zend_ws_close(self._id)
end
WebSocket.send = WebSocket.Send
WebSocket.close = WebSocket.Close

return WebSocket
)LUA";

void install_websocket(lua_State* L) {
    set_function(L, "__zend_ws_connect", websocket_connect);
    set_function(L, "__zend_ws_send", websocket_send);
    set_function(L, "__zend_ws_close", websocket_close);
    set_function(L, "__zend_ws_poll", websocket_poll);
    set_function(L, "__zend_ws_release", websocket_release);

    const std::string bytecode = compile(kWebSocketPrelude);
    if (luau_load(L, "=zend-websocket", bytecode.data(), bytecode.size(), 0) !=
        0) {
        lua_pop(L, 1);
        return;
    }
    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure && !closure->isC && closure->l.p) {
        executor::apply_capabilities(closure->l.p);
    }

    // The prelude keeps its table in a local (the name would otherwise shadow
    // the global), so it returns the table and the C side publishes it.
    if (lua_pcall(L, 0, 1, 0) != 0) {
        lua_pop(L, 1);
        return;
    }
    if (lua_istable(L, -1)) {
        lua_setfield(L, LUA_GLOBALSINDEX, "WebSocket");
        return;
    }
    lua_pop(L, 1);
}

// --- rconsole -------------------------------------------------------------

std::mutex g_console_mutex;
HANDLE g_console = INVALID_HANDLE_VALUE;

struct ConsoleColorToken {
    const char* name;
    WORD attribute;
};

const ConsoleColorToken kConsoleColors[] = {
    {"@@RESET@@", FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE},
    {"@@RED@@", FOREGROUND_RED},
    {"@@GREEN@@", FOREGROUND_GREEN},
    {"@@BLUE@@", FOREGROUND_BLUE},
    {"@@WHITE@@", FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE},
    {"@@BLACK@@", 0},
    {"@@YELLOW@@", FOREGROUND_RED | FOREGROUND_GREEN},
    {"@@CYAN@@", FOREGROUND_GREEN | FOREGROUND_BLUE},
    {"@@MAGENTA@@", FOREGROUND_RED | FOREGROUND_BLUE},
    {"@@GRAY@@", FOREGROUND_INTENSITY},
    {"@@BROWN@@", FOREGROUND_RED | FOREGROUND_INTENSITY},
    {"@@ORANGE@@", FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY},
    {"@@LIGHT_BLUE@@", FOREGROUND_BLUE | FOREGROUND_INTENSITY},
    {"@@LIGHT_GREEN@@", FOREGROUND_GREEN | FOREGROUND_INTENSITY},
    {"@@LIGHT_CYAN@@", FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY},
    {"@@LIGHT_RED@@", FOREGROUND_RED | FOREGROUND_INTENSITY},
    {"@@LIGHT_MAGENTA@@", FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY},
};

// The console is per process, not per VM, so it outlives a re-injected core.
// Caller holds g_console_mutex.
bool ensure_console() {
    if (g_console != INVALID_HANDLE_VALUE) {
        return true;
    }

    HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        // The process already has a console (or was started from one).
        g_console = existing;
    } else {
        if (!AllocConsole()) {
            return false;
        }
        g_console = GetStdHandle(STD_OUTPUT_HANDLE);
        if (g_console == nullptr || g_console == INVALID_HANDLE_VALUE) {
            g_console = INVALID_HANDLE_VALUE;
            return false;
        }
    }

    SetConsoleTitleA("Zend");
    SetConsoleOutputCP(CP_UTF8);

    DWORD mode = 0;
    if (GetConsoleMode(g_console, &mode)) {
        SetConsoleMode(g_console, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);
    }

    // The first rconsole call is script-driven, so show the window without
    // taking focus away from whatever the user is doing.
    if (HWND window = GetConsoleWindow()) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    return true;
}

// Writes one line, translating @@TOKEN@@ runs into console attributes.
// Caller holds g_console_mutex.
void write_console(const std::string& text, WORD default_color) {
    if (!ensure_console()) {
        return;
    }

    SetConsoleTextAttribute(g_console, default_color);

    DWORD written = 0;
    std::size_t start = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] == '@' && cursor + 1 < text.size() && text[cursor + 1] == '@') {
            bool matched = false;
            for (const ConsoleColorToken& token : kConsoleColors) {
                const std::size_t length = std::strlen(token.name);
                if (text.compare(cursor, length, token.name) == 0) {
                    if (cursor > start) {
                        WriteConsoleA(g_console, text.data() + start,
                                      static_cast<DWORD>(cursor - start), &written, nullptr);
                    }
                    SetConsoleTextAttribute(g_console, token.attribute);
                    cursor += length;
                    start = cursor;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }
        }
        ++cursor;
    }
    if (cursor > start) {
        WriteConsoleA(g_console, text.data() + start,
                      static_cast<DWORD>(cursor - start), &written, nullptr);
    }
    WriteConsoleA(g_console, "\n", 1, &written, nullptr);
    SetConsoleTextAttribute(g_console, default_color);
}

// The reference calls the global tostring for non-strings, so tables and
// userdata print the way a script expects.
std::string console_text(lua_State* L, int index) {
    std::string text;
    if (lua_isstring(L, index) || lua_isnumber(L, index)) {
        if (const char* str = lua_tostring(L, index)) {
            text = str;
        }
        return text;
    }

    lua_getglobal(L, "tostring");
    lua_pushvalue(L, index);
    lua_call(L, 1, 1);
    if (const char* str = lua_tostring(L, -1)) {
        text = str;
    }
    lua_pop(L, 1);
    return text;
}

int rconsoleprint(lua_State* L) {
    luaL_checkany(L, 1);
    const std::string text = console_text(L, 1);
    std::lock_guard<std::mutex> lock(g_console_mutex);
    write_console(text, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    return 0;
}

int rconsoleinfo(lua_State* L) {
    luaL_checkany(L, 1);
    const std::string text =
        "@@CYAN@@@@LIGHT_BLUE@@[INFO]@@RESET@@ " + console_text(L, 1);
    std::lock_guard<std::mutex> lock(g_console_mutex);
    write_console(text, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    return 0;
}

int rconsolewarn(lua_State* L) {
    luaL_checkany(L, 1);
    const std::string text =
        "@@YELLOW@@@@LIGHT_GREEN@@[WARN]@@RESET@@ " + console_text(L, 1);
    std::lock_guard<std::mutex> lock(g_console_mutex);
    write_console(text, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    return 0;
}

int rconsoleerr(lua_State* L) {
    luaL_checkany(L, 1);
    const std::string text = console_text(L, 1);
    {
        std::lock_guard<std::mutex> lock(g_console_mutex);
        write_console("@@RED@@@@LIGHT_RED@@[ERROR]@@RESET@@ " + text,
                      FOREGROUND_RED | FOREGROUND_INTENSITY);
    }
    // The reference always returns the message, console or not.
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int rconsoleclear(lua_State*) {
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!ensure_console()) {
        return 0;
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_console, &info)) {
        DWORD written = 0;
        const DWORD cells =
            static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
        const COORD origin = {0, 0};
        FillConsoleOutputCharacterA(g_console, ' ', cells, origin, &written);
        FillConsoleOutputAttribute(
            g_console, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, cells,
            origin, &written);
        SetConsoleCursorPosition(g_console, origin);
    }
    return 0;
}

void install_console(lua_State* L) {
    set_function(L, "rconsoleprint", rconsoleprint);
    set_function(L, "consoleprint", rconsoleprint);
    set_function(L, "rconsoleinfo", rconsoleinfo);
    set_function(L, "rconsolewarn", rconsolewarn);
    set_function(L, "rconsoleerr", rconsoleerr);
    set_function(L, "rconsoleerror", rconsoleerr);
    set_function(L, "rconsoleclear", rconsoleclear);
    set_function(L, "consoleclear", rconsoleclear);
}

void install_signals(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, connection_index, "Connection.__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, connection_newindex, "Connection.__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushstring(L, "Connection");
    lua_setfield(L, -2, "__type");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, kConnectionMetatable);
    lua_pop(L, 1);

    set_function(L, "getconnections", getconnections);
    set_function(L, "get_signal_cons", getconnections);
    set_function(L, "firesignal", firesignal);
    set_function(L, "getsignalarguments", getsignalarguments);
    set_function(L, "get_signal_arguments", getsignalarguments);
    set_function(L, "getsignalargumentsinfo", getsignalargumentsinfo);
    set_function(L, "get_signal_arguments_info", getsignalargumentsinfo);
    set_function(L, "hooksignal", hooksignal);
    set_function(L, "hook_signal", hooksignal);
    set_function(L, "restoresignal", restoresignal);
    set_function(L, "restore_signal", restoresignal);
    set_function(L, "issignalhooked", issignalhooked);
    set_function(L, "is_signal_hooked", issignalhooked);
    set_function(L, "firetouchinterest", firetouchinterest);
    set_function(L, "fireproximityprompt", fireproximityprompt);
    set_function(L, "fireclickdetector", fireclickdetector);
    set_function(L, "replicatesignal", replicatesignal);
}

} // namespace

std::string compile(const std::string& source) {
    static const char* kMutableGlobals[] = {
        "Game", "Workspace", "game", "plugin", "script",
        "shared", "workspace", "_G", "_ENV", nullptr,
    };

    Luau::CompileOptions options;
    options.debugLevel = source.size() >= 65536 ? 1 : 2;
    options.optimizationLevel = 1;
    options.mutableGlobals = kMutableGlobals;
    options.vectorLib = "Vector3";
    options.vectorCtor = "new";
    options.vectorType = "Vector3";

    WireEncoder encoder;
    return Luau::compile(source, options, {}, &encoder);
}

// The client caches direct userdata property/namecall dispatchers per class
// tag. As long as those pointers are live the VM never consults the metatable,
// so metamethod hooks (getrawmetatable, hookmetamethod) would not fire. Drop
// the pointers once; the VM backpatches the affected instructions to regular
// table access on their next execution.
void clear_instance_fast_paths(lua_State* L) {
    lua_getglobal(L, "game");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    const int tag = lua_userdatatag(L, -1);
    lua_pop(L, 1);
    if (tag < 0 || tag >= UTAG_INTERNAL_LIMIT) {
        return;
    }

    __try {
        for (int i = 0; i < UTAG_INTERNAL_LIMIT; ++i) {
            lua_UdataDirectAccessData& direct = L->global->udatadirect[i];
            direct.index = nullptr;
            direct.newindex = nullptr;
            direct.namecall = nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void install(lua_State* L) {
    // Learn the real client globals through the sandbox proxy's __index before
    // repointing it, and keep the proxy anchored while L->gt changes.
    LuaTable* sandbox = L->gt;
    LuaTable* client = nullptr;
    if (sandbox && sandbox->metatable) {
        TString* index_name = luaS_new(L, "__index");
        const TValue* index = luaH_getstr(sandbox->metatable, index_name);
        if (ttistable(index)) {
            client = hvalue(index);
        }
    }

    clear_instance_fast_paths(L);

    // Remember the real client globals now: the sandbox proxy's __index is
    // repointed to the executor table below, so this is the last place the
    // original table is visible without guessing at thread layout.
    if (client) {
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        sethvalue(L, L->top, client);
        incr_top(L);
        g_client_globals_ref = lua_ref(L, -1);
        g_client_globals_global = L->global;
        lua_pop(L, 1);
    }

    lua_rawcheckstack(L, 3);
    luaC_threadbarrier(L);
    sethvalue(L, L->top, sandbox);
    incr_top(L);
    const int sandbox_index = lua_gettop(L);

    lua_newtable(L);
    const int genv_index = lua_gettop(L);
    if (client) {
        lua_newtable(L);
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        sethvalue(L, L->top, client);
        incr_top(L);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, genv_index);
    }

    if (sandbox && sandbox->metatable) {
        lua_rawcheckstack(L, 1);
        luaC_threadbarrier(L);
        sethvalue(L, L->top, sandbox->metatable);
        incr_top(L);
        const int readonly = lua_getreadonly(L, -1);
        lua_setreadonly(L, -1, 0);
        lua_pushvalue(L, genv_index);
        lua_setfield(L, -2, "__index");
        lua_setreadonly(L, -1, readonly);
        lua_pop(L, 1);
    }

    // print/warn/__zend_finish were installed before this call. They belong to
    // the executor side of the split, not to each chunk's proxy.
    static const char* const kCarried[] = {"print", "warn", "__zend_finish"};
    for (const char* name : kCarried) {
        lua_pushstring(L, name);
        lua_rawget(L, sandbox_index);
        if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, -1);
            lua_setfield(L, genv_index, name);
            lua_pushstring(L, name);
            lua_pushnil(L);
            lua_rawset(L, sandbox_index);
        }
        lua_pop(L, 1);
    }

    lua_pushvalue(L, genv_index);
    g_genv_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    g_genv_global = L->global;

    // Install with getgenv() as the global table so every helper lands in the
    // shared table instead of the per-chunk proxy.
    lua_pushvalue(L, genv_index);
    lua_replace(L, LUA_GLOBALSINDEX);

    set_function(L, "identifyexecutor", identifyexecutor);
    set_function(L, "__zend_env_trace", set_env_trace);
    set_function(L, "__zend_opmap", dump_opcode_map);
    set_function(L, "getgenv", getgenv);
    set_function(L, "getrenv", getrenv);
    set_function(L, "getreg", getreg);
    set_function(L, "getgc", getgc);
    set_function(L, "getinstances", getinstances);
    set_function(L, "getnilinstances", getnilinstances);
    set_function(L, "getscripts", getscripts);
    set_function(L, "getcallingscript", getcallingscript);
    set_function(L, "getloadedmodules", getloadedmodules);
    set_function(L, "getscriptbytecode", getscriptbytecode);
    set_function(L, "getscripthash", getscripthash);
    set_function(L, "getscriptclosure", getscriptclosure);
    set_function(L, "getrunningscripts", getrunningscripts);
    set_function(L, "getscriptfromthread", getscriptfromthread);
    set_function(L, "getsenv", getsenv);
    set_function(L, "decompile", decompile);
    set_function(L, "loadstring", loadstring);

    // Capture the client require before shadowing it: the wrapper is the only
    // path that can load ModuleScripts created at runtime from this thread.
    lua_getglobal(L, "require");
    if (lua_isfunction(L, -1)) {
        g_require_ref = lua_ref(L, -1);
    }
    lua_pop(L, 1);

    lua_newtable(L);
    g_module_cache_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    set_function(L, "require", rbx_require);

    set_function(L, "getthreadidentity", getthreadidentity);
    set_function(L, "getidentity", getthreadidentity);
    set_function(L, "getthreadcontext", getthreadidentity);
    set_function(L, "setthreadidentity", setthreadidentity);
    set_function(L, "setidentity", setthreadidentity);
    set_function(L, "setthreadcontext", setthreadidentity);
    set_function(L, "getcapabilities", getcapabilities);
    set_function(L, "getnamecallmethod", getnamecallmethod);

    set_function(L, "gethui", gethui);
    set_function(L, "cloneref", cloneref);
    set_function(L, "compareinstances", compareinstances);
    set_function(L, "isexecutorclosure", isexecutorclosure);
    set_function(L, "getfunctionhash", getfunctionhash);
    set_function(L, "filtergc", filtergc);
    set_function(L, "gethiddenproperty", gethiddenproperty);
    set_function(L, "sethiddenproperty", sethiddenproperty);
    set_function(L, "isscriptable", isscriptable);
    set_function(L, "setscriptable", setscriptable);
    set_function(L, "getcallbackvalue", getcallbackvalue);

    lua_newtable(L);
    set_field_function(L, "invalidate", cache_invalidate);
    set_field_function(L, "iscached", cache_iscached);
    set_field_function(L, "replace", cache_replace);
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "cache");

    set_function(L, "getchildren", instance_getchildren);
    set_function(L, "getdescendants", instance_getdescendants);
    set_function(L, "findfirstchild", instance_findfirstchild);
    set_function(L, "findfirstchildofclass", instance_findfirstchildofclass);
    set_function(L, "findfirstchildwhichisa", instance_findfirstchildwhichisa);
    set_function(L, "getpropertychanged", instance_getpropertychanged);

    install_filesystem(L);
    install_crypt(L);
    install_console(L);
    install_signals(L);

    set_function(L, "getrawmetatable", getrawmetatable);
    set_function(L, "getmetatable", getrawmetatable);
    set_function(L, "setrawmetatable", setrawmetatable);
    set_function(L, "setmetatable", setrawmetatable);
    set_function(L, "hookmetamethod", hookmetamethod);
    set_function(L, "setnamecallmethod", setnamecallmethod);
    set_function(L, "setreadonly", setreadonly);
    set_function(L, "isreadonly", isreadonly);
    set_function(L, "make_writeable", make_writeable);

    // Runs last: the prelude defines HttpGet/HttpPost and then their DataModel
    // bindings are added to the shared instance metatable.
    install_http(L);
    install_websocket(L);

    set_function(L, "iscclosure", iscclosure);
    set_function(L, "islclosure", islclosure);
    set_function(L, "checkcaller", checkcaller);
    set_function(L, "clonefunction", clonefunction);
    set_function_k(L, "__zend_hook_dispatch", hook_lua_dispatch,
                   dispatch_continuation);
    set_function(L, "hookfunction", hookfunction);
    set_function(L, "hookfunc", hookfunction);
    set_function(L, "replaceclosure", hookfunction);
    set_function(L, "restorefunction", restorefunction);
    set_function(L, "restorefunc", restorefunction);
    set_function(L, "newcclosure", newcclosure);
    set_function(L, "isnewcclosure", isnewcclosure);
    set_function(L, "isfunctionhooked", isfunctionhooked);

    set_function(L, "getinfo", debug_getinfo);
    set_function(L, "getlocal", debug_getlocal);
    set_function(L, "getconstants", debug_getconstants);
    set_function(L, "getconstant", debug_getconstant);
    set_function(L, "setconstant", debug_setconstant);
    set_function(L, "getupvalues", debug_getupvalues);
    set_function(L, "getupvalue", debug_getupvalue);
    set_function(L, "setupvalue", debug_setupvalue);
    set_function(L, "getproto", debug_getproto);
    set_function(L, "getprotos", debug_getprotos);
    set_function(L, "getstack", debug_getstack);
    set_function(L, "setstack", debug_setstack);

    mirror_debug(L, "getinfo");
    mirror_debug(L, "getlocal");
    mirror_debug(L, "getconstants");
    mirror_debug(L, "getconstant");
    mirror_debug(L, "setconstant");
    mirror_debug(L, "getupvalues");
    mirror_debug(L, "getupvalue");
    mirror_debug(L, "setupvalue");
    mirror_debug(L, "getproto");
    mirror_debug(L, "getprotos");
    mirror_debug(L, "getstack");
    mirror_debug(L, "setstack");

    drawing::install(L, set_function);

    // `_G` and `shared` are VM-wide tables in Roblox: every script, game or
    // executor, reads the same objects. Point the executor environment at the
    // client's tables so writes are shared exactly like the real client.
    publish_shared_global(L, LUA_GLOBALSINDEX, "_G", 2);
    publish_shared_global(L, LUA_GLOBALSINDEX, "shared", 4);

    // Put the per-chunk proxy back. The shared table remains anchored by the
    // registry ref and, from now on, by the proxy metatable's __index.
    lua_pushvalue(L, sandbox_index);
    lua_replace(L, LUA_GLOBALSINDEX);
    lua_settop(L, sandbox_index - 1);
}

void reset() {
    g_genv_ref = LUA_NOREF;
    g_genv_global = nullptr;
    g_client_globals_ref = LUA_NOREF;
    g_client_globals_global = nullptr;
    g_http_get_ref = LUA_NOREF;
    g_http_post_ref = LUA_NOREF;
    g_game_ref = LUA_NOREF;
    g_require_ref = LUA_NOREF;
    g_module_cache_ref = LUA_NOREF;
    g_own_closures.clear();
    g_hooks.clear();
    g_newc.clear();
    g_wrapped.clear();
    g_namecall_frames.clear();
    g_signal_hooks.clear();
    g_hooked_connection_functions.clear();
    g_property_layout = PropertyLayout{};
    g_pristine_scriptable.clear();
    g_scriptable_overrides.clear();
    drawing::reset();
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        g_http_jobs.clear();
    }
    {
        std::vector<std::shared_ptr<WebSocketSocket>> sockets;
        {
            std::lock_guard<std::mutex> lock(g_ws_mutex);
            for (auto& entry : g_ws_sockets) {
                sockets.push_back(entry.second);
            }
            g_ws_sockets.clear();
        }
        for (const auto& socket : sockets) {
            socket->closing.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(socket->send_mutex);
            if (socket->socket) {
                WinHttpWebSocketShutdown(
                    socket->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                    nullptr, 0);
            }
        }
    }
}

} // namespace zend::rbx::env
