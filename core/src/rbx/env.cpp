#include "rbx/env.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
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

#include "rbx/drawing.hpp"
#include "rbx/executor.hpp"
#include "rbx/memory.hpp"
#include "rbx/offsets.hpp"
#include "rbx/sha3.hpp"

// Environment v1 core subset. Everything here runs inside the client VM on the
// executor thread (or a coroutine the client scheduler made from it), so it may
// use the VM's internals directly. The reference implementations live in
// sample/RBX/environment/libs.

namespace zend::rbx::env {
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

// The real global table of the client's main thread, cloned once per VM and
// rooted in the registry. Cloning is shallow; the fields are the client's
// original objects, so writing to the clone cannot affect the real env.
int g_renv_ref = LUA_NOREF;
global_State* g_renv_global = nullptr;

int identifyexecutor(lua_State* L) {
    lua_pushstring(L, "ZEND");
    lua_pushstring(L, "0.1.0");
    return 2;
}

int getgenv(lua_State* L) {
    lua_State* owner = executor::thread();
    if (!owner || owner == L || owner->global != L->global) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        return 1;
    }

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    luaC_threadbarrier(owner);
    lua_pushvalue(owner, LUA_GLOBALSINDEX);
    lua_xmove(owner, L, 1);
    return 1;
}

int getrenv(lua_State* L) {
    lua_State* main = L->global->mainthread;
    if (!main || !main->gt) {
        lua_pushnil(L);
        return 1;
    }

    if (g_renv_global != L->global || g_renv_ref == LUA_NOREF) {
        // A new VM starts the cache over; the old ref belongs to a dead state
        // and is intentionally not released.
        g_renv_global = L->global;
        g_renv_ref = LUA_NOREF;

        LuaTable* clone = luaH_clone(L, main->gt);
        lua_rawcheckstack(L, 2);
        luaC_threadbarrier(L);
        luaC_threadbarrier(main);
        sethvalue(L, L->top, clone);
        incr_top(L);

        lua_pushvalue(L, -1);
        g_renv_ref = lua_ref(L, -1);
        lua_pop(L, 2);
    }

    lua_getref(L, g_renv_ref);
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
    bool with_parent;
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

    lua_getfield(L, -1, "Parent");
    const bool has_parent = !lua_isnil(L, -1);
    lua_pop(L, 1);

    // game is the DataModel root; its Parent is nil but it is still part of
    // the tree, so getinstances includes it and getnilinstances excludes it.
    const bool is_game =
        scan->game != 0 && lua_rawequal(L, -1, scan->game) != 0;
    const bool take = scan->with_parent ? (has_parent || is_game)
                                        : (!has_parent && !is_game);
    if (take) {
        lua_rawseti(L, scan->result, ++scan->count);
    } else {
        lua_pop(L, 1);
    }
    return false;
}

int collect_instances(lua_State* L, bool with_parent) {
    lua_getglobal(L, "game");
    const bool has_game = lua_isuserdata(L, -1) != 0;
    const int game = has_game ? lua_gettop(L) : 0;
    if (!has_game) {
        lua_pop(L, 1);
    }

    lua_newtable(L);
    InstanceScan scan{L, lua_gettop(L), game, with_parent, 0};

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
    lua_getfield(L, -1, "ClassName");
    const char* class_name = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;

    bool match = false;
    if (class_name &&
        (std::strcmp(class_name, "LocalScript") == 0 ||
         std::strcmp(class_name, "ModuleScript") == 0 ||
         std::strcmp(class_name, "CoreScript") == 0)) {
        match = true;
    } else if (class_name && std::strcmp(class_name, "Script") == 0) {
        // Server scripts only matter when their RunContext is Client.
        lua_getfield(L, -1, "RunContext");
        if (lua_isuserdata(L, -1) || lua_isstring(L, -1)) {
            lua_getfield(L, -1, "Name");
            const char* run_context = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
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

// The client resolves capability bits from the identity through this function;
// identity 8 (executor) always gets the full mask the reference calls
// MaxCapabilities. Calling the client keeps the bit layout of unusual
// identities correct instead of inventing one.
std::uint64_t capabilities_for_identity(int identity) {
    if (identity == 8) {
        return ~std::uint64_t{0};
    }

    using GetCapabilitiesFn = std::int64_t(__fastcall*)(int*);
    const auto fn = reinterpret_cast<GetCapabilitiesFn>(
        off::host_base() + off::kGetCapabilities);
    const std::uint64_t mask = static_cast<std::uint64_t>(fn(&identity));
    return 0xFFFFFFFFFFFFFF00ull | mask;
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

    bool applied = false;
    if (L->userdata) {
        L->userdata->identity = static_cast<std::uint32_t>(identity);
        L->userdata->capabilities = capabilities;
        applied = true;
    }

    // Keep the persistent executor thread in step so a change made in one
    // chunk is still visible in the next deferred coroutine.
    lua_State* owner = executor::thread();
    if (owner && owner->userdata) {
        owner->userdata->identity = static_cast<std::uint32_t>(identity);
        owner->userdata->capabilities = capabilities;
        applied = true;
    }

    if (!applied) {
        luaL_error(L, "setthreadidentity: thread has no RobloxExtraSpace");
    }
    return 0;
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

struct ModuleScan {
    lua_State* L;
    int result;
    int count;
    std::unordered_set<const void*> seen;
};

// A module counts as loaded once one of its closures exists: the client puts
// the owning script in the function environment as `script`.
bool module_visitor(void* raw, lua_Page*, GCObject* gco) {
    auto* scan = static_cast<ModuleScan*>(raw);
    lua_State* L = scan->L;
    if (isdead(L->global, gco) || gco->gch.tt != LUA_TFUNCTION) {
        return false;
    }

    const int base = lua_gettop(L);
    push_gcobject(L, gco);
    lua_getfenv(L, -1);
    if (!lua_isnil(L, -1)) {
        lua_getfield(L, -1, "script");
        if (lua_isuserdata(L, -1) &&
            std::strcmp(luaL_typename(L, -1), "Instance") == 0) {
            const void* instance =
                *static_cast<void* const*>(lua_touserdata(L, -1));
            if (instance && scan->seen.insert(instance).second) {
                lua_getfield(L, -1, "ClassName");
                if (lua_isstring(L, -1) &&
                    std::strcmp(lua_tostring(L, -1), "ModuleScript") == 0) {
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, scan->result, ++scan->count);
                }
                lua_pop(L, 1);
            }
        }
    }
    lua_settop(L, base);
    return false;
}

int getloadedmodules(lua_State* L) {
    lua_newtable(L);
    ModuleScan scan{L, lua_gettop(L), 0, {}};

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);

    const std::size_t threshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;
    luaM_visitgco(L, &scan, module_visitor);
    L->global->GCthreshold = threshold;

    return 1;
}

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

    const std::uintptr_t weak = connection_weak_object(record->connection);
    lua_State* thread = weak ? connection_thread(weak) : nullptr;
    Closure* closure = weak ? connection_function(weak) : nullptr;
    const bool foreign = thread && thread->global != L->global;

    if (std::strcmp(key, "ForeignState") == 0) {
        lua_pushboolean(L, foreign);
        return 1;
    }
    if (std::strcmp(key, "LuaConnection") == 0) {
        lua_pushboolean(L, closure != nullptr);
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
    // this connection on the next fire and the node is unlinked by its owner.
    set_connection_enabled(record->connection, 0);
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

    void* userdata = lua_touserdata(L, -1);
    std::uintptr_t sentinel = 0;
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
        sentinel = reinterpret_cast<std::uintptr_t>(userdata);
    } else if (userdata) {
        read_ptr(reinterpret_cast<std::uintptr_t>(userdata), sentinel);
    }

    if (!sentinel) {
        lua_newtable(L);
        disconnect_connection_userdata(L, -2);
        lua_remove(L, -2);
        return 1;
    }

    const ConnectionWalk walk = walk_connections(sentinel);

    lua_newtable(L);
    disconnect_connection_userdata(L, -2);
    lua_remove(L, -2);

    const int result = lua_gettop(L);
    int count = 0;
    for (int index = 0; index < walk.count; ++index) {
        const std::uintptr_t connection = walk.nodes[index];
        if (!connection) {
            continue;
        }
        push_connection(L, connection);
        lua_rawseti(L, result, ++count);
    }
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

std::unordered_set<Closure*> g_own_closures;

int iscclosure(lua_State* L) {
    lua_pushboolean(L, lua_isfunction(L, 1) && lua_iscfunction(L, 1));
    return 1;
}

int islclosure(lua_State* L) {
    lua_pushboolean(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1));
    return 1;
}

// True only when the frame that called checkcaller is one of the C closures
// this environment installed. A user chunk always gets false, which is what
// scripts and anti-cheat probes expect.
int checkcaller(lua_State* L) {
    bool ours = false;
    if (L->ci > L->base_ci) {
        CallInfo* caller = L->ci - 1;
        if (caller->func && ttisfunction(caller->func)) {
            ours = g_own_closures.contains(clvalue(caller->func));
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
int dispatch_continuation(lua_State* L, int status) {
    (void)status;
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

// The shared hookfunction body. `hook == original` uninstalls the hook, which
// is the standard way scripts swap a hook out without losing the record.
int hook_function(lua_State* L) {
    Closure* target = to_closure(L, 1);
    Closure* hook = to_closure(L, 2);

    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) {
        HookRecord record;
        record.target_ref = ref_at(L, 1);
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
        lua_pushvalue(L, 2);
        return 1;
    }

    int previous_ref = LUA_NOREF;
    if (it->second.hook_ref != LUA_NOREF) {
        previous_ref = retain_ref(L, it->second.hook_ref);
    } else {
        previous_ref = retain_ref(L, it->second.original_ref);
    }

    release_ref(L, it->second.hook_ref);
    it->second.hook_ref = ref_at(L, 2);
    install_hook_body(L, target);

    lua_getref(L, previous_ref);
    lua_unref(L, previous_ref);
    return 1;
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

int debug_getproto(lua_State* L) {
    const int fn = push_function_arg(L, 1);
    Closure* closure = lua_closure(L, fn, false);
    Proto* parent = closure->l.p;
    const int index = luaL_checkinteger(L, 2);

    if (!parent || index < 1 || index > parent->sizep || !parent->p) {
        luaL_argerror(L, 2, "index out of range");
    }

    Proto* wanted = parent->p[index - 1];
    if (!wanted) {
        luaL_argerror(L, 2, "proto is unavailable");
    }

    lua_rawcheckstack(L, 1);
    luaC_threadbarrier(L);
    Closure* child = luaF_newLclosure(L, wanted->nups, L->gt, wanted);
    push_closure(L, child);

    lua_remove(L, fn);
    return 1;
}

CallInfo* visible_level(lua_State* L, int level) {
    for (CallInfo* frame = L->ci - 1; frame >= L->base_ci; --frame) {
        if (!frame->func || !ttisfunction(frame->func)) {
            continue;
        }
        if (clvalue(frame->func)->isC) {
            continue;
        }
        if (--level == 0) {
            return frame;
        }
    }
    return nullptr;
}

int debug_getstack(lua_State* L) {
    const int level = luaL_checkinteger(L, 1);
    if (level <= 0) {
        luaL_argerror(L, 1, "level must be positive");
    }

    CallInfo* frame = visible_level(L, level);
    if (!frame) {
        luaL_argerror(L, 1, "level out of range");
    }
    if (!frame->base) {
        luaL_argerror(L, 1, "frame has no stack");
    }

    Closure* closure = clvalue(frame->func);
    Proto* proto = closure->l.p;
    int size = proto ? proto->maxstacksize : 0;
    if (size <= 0 && frame->top > frame->base) {
        size = static_cast<int>(frame->top - frame->base);
    }

    const int stack_size = static_cast<int>(L->stacksize);
    const StkId end = L->stack + stack_size;
    if (frame->base < L->stack || frame->base >= end) {
        luaL_argerror(L, 1, "frame stack is unavailable");
    }
    const int available = static_cast<int>(end - frame->base);
    if (size > available) {
        size = available;
    }
    if (size < 0) {
        size = 0;
    }

    lua_createtable(L, size, 0);
    lua_rawcheckstack(L, size > 0 ? size : 1);
    for (int i = 0; i < size; ++i) {
        luaC_threadbarrier(L);
        luaA_pushvalue(L, frame->base + i);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
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

    const std::string bytecode = compile(std::string(source, length));
    if (luau_load(L, chunk_name, bytecode.data(), bytecode.size(), 0) != 0) {
        // luau_load leaves the compile error on the stack.
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }

    Closure* closure = clvalue(luaA_toobject(L, -1));
    if (closure && !closure->isC && closure->l.p) {
        executor::apply_capabilities(closure->l.p);
    }
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
}

void install_crypt(lua_State* L) {
    set_function(L, "base64encode", crypt_base64encode);
    set_function(L, "base64decode", crypt_base64decode);
    set_function(L, "base64_encode", crypt_base64encode);
    set_function(L, "base64_decode", crypt_base64decode);

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

void install(lua_State* L) {
    set_function(L, "identifyexecutor", identifyexecutor);
    set_function(L, "getgenv", getgenv);
    set_function(L, "getrenv", getrenv);
    set_function(L, "getreg", getreg);
    set_function(L, "getgc", getgc);
    set_function(L, "getinstances", getinstances);
    set_function(L, "getnilinstances", getnilinstances);
    set_function(L, "getscripts", getscripts);
    set_function(L, "getcallingscript", getcallingscript);
    set_function(L, "getloadedmodules", getloadedmodules);
    set_function(L, "loadstring", loadstring);

    set_function(L, "getthreadidentity", getthreadidentity);
    set_function(L, "getidentity", getthreadidentity);
    set_function(L, "getthreadcontext", getthreadidentity);
    set_function(L, "setthreadidentity", setthreadidentity);
    set_function(L, "setidentity", setthreadidentity);
    set_function(L, "setthreadcontext", setthreadidentity);
    set_function(L, "getcapabilities", getcapabilities);

    set_function(L, "getchildren", instance_getchildren);
    set_function(L, "getdescendants", instance_getdescendants);
    set_function(L, "findfirstchild", instance_findfirstchild);
    set_function(L, "findfirstchildofclass", instance_findfirstchildofclass);
    set_function(L, "findfirstchildwhichisa", instance_findfirstchildwhichisa);
    set_function(L, "getpropertychanged", instance_getpropertychanged);

    install_filesystem(L);
    install_crypt(L);
    install_http(L);
    install_console(L);
    install_signals(L);

    set_function(L, "getrawmetatable", getrawmetatable);
    set_function(L, "getmetatable", getrawmetatable);
    set_function(L, "setrawmetatable", setrawmetatable);
    set_function(L, "setmetatable", setrawmetatable);
    set_function(L, "setreadonly", setreadonly);
    set_function(L, "isreadonly", isreadonly);
    set_function(L, "make_writeable", make_writeable);

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
    set_function(L, "getconstants", debug_getconstants);
    set_function(L, "getupvalues", debug_getupvalues);
    set_function(L, "setupvalue", debug_setupvalue);
    set_function(L, "getproto", debug_getproto);
    set_function(L, "getstack", debug_getstack);

    mirror_debug(L, "getinfo");
    mirror_debug(L, "getconstants");
    mirror_debug(L, "getupvalues");
    mirror_debug(L, "setupvalue");
    mirror_debug(L, "getproto");
    mirror_debug(L, "getstack");

    drawing::install(L, set_function);

    // The sandboxed globals start without _G; scripts expect it to be the
    // executor environment, so alias it to our own global table.
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setfield(L, LUA_GLOBALSINDEX, "_G");
}

void reset() {
    g_renv_ref = LUA_NOREF;
    g_renv_global = nullptr;
    g_own_closures.clear();
    g_hooks.clear();
    g_newc.clear();
    g_wrapped.clear();
    g_signal_hooks.clear();
    g_hooked_connection_functions.clear();
    drawing::reset();
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        g_http_jobs.clear();
    }
}

} // namespace zend::rbx::env
