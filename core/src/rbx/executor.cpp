#include "rbx/executor.hpp"

#include <Windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <lapi.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>

#include <Luau/Common.h>

#include "rbx/env.hpp"
#include "rbx/memory.hpp"
#include "rbx/offsets.hpp"
#include "util/log.hpp"

LUAU_FASTFLAG(LuauCIProto)

namespace zend::rbx {
namespace {

// Persistent executor thread owned by the DataModel. Every chunk is dispatched
// from here so the client's userthread callback (fired by lua_newthread) cannot
// rewrite the environment we installed.
lua_State* g_thread = nullptr;

// Guards the dispatch state below. Print capture and the result callback run on
// whatever coroutine the client scheduler picked, so every field here has to be
// published under the lock. Recursive because luaL_tolstring can run a user
// __tostring that prints again.
std::recursive_mutex g_mutex;
bool g_active = false;
std::uint64_t g_generation = 0;
executor::FinishFn g_finish = nullptr;
void* g_finish_context = nullptr;

// Output for the chunk that is currently running.
std::string g_output;

// Live output streaming: installed by the pipe layer, tagged with the id of
// the dispatch that produced the line. Guarded by g_mutex like g_output.
executor::OutputFn g_output_sink = nullptr;
void* g_output_context = nullptr;
std::uint64_t g_output_id = 0;

// The client stores proto capabilities behind a pointer that must stay alive.
std::uintptr_t g_capabilities = ~std::uintptr_t{0};

// The chunk runs under the client's own xpcall, deferred through task.defer so
// yields are legal. Only __zend_finish reports back, and it carries the
// dispatch generation so a chunk from an older DataModel cannot complete a
// newer job.
constexpr char kRunnerSource[] = R"luau(
local fn, gen = ...
local ok, err = xpcall(fn, function(e)
    local fine, tb = pcall(function() return debug.traceback("", 2) end)
    if fine and type(tb) == "string" then
        return tostring(e) .. "\n" .. tb
    end
    return tostring(e)
end)
__zend_finish(gen, ok, err)
)luau";

std::string g_runner_bytecode;

int capture_line(lua_State* L, bool warn) {
    const int count = lua_gettop(L);

    // Build the line before touching the shared buffer: a user __tostring may
    // print recursively, and a throwing one must not leave a half line behind.
    std::string line;
    for (int i = 1; i <= count; ++i) {
        std::size_t len = 0;
        const char* text = luaL_tolstring(L, i, &len);
        if (i > 1) {
            line.push_back('\t');
        }
        if (text) {
            line.append(text, len);
        }
        lua_pop(L, 1);
    }
    line.push_back('\n');

    executor::OutputFn sink = nullptr;
    void* context = nullptr;
    std::uint64_t output_id = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_output.append(line);
        sink = g_output_sink;
        context = g_output_context;
        output_id = g_output_id;
    }

    // Outside the lock: the pipe may block on a slow reader, and complete()
    // must still be able to take g_mutex.
    if (sink) {
        sink(context, output_id, warn, line.data(), line.size());
    }
    return 0;
}

int capture_print(lua_State* L) {
    return capture_line(L, false);
}

int capture_warn(lua_State* L) {
    return capture_line(L, true);
}

void complete(std::uint64_t generation, bool ok, std::string error) {
    executor::FinishFn finish = nullptr;
    void* context = nullptr;
    ExecResult result;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (generation != g_generation || !g_active) {
            return; // a chunk from a previous dispatch or DataModel
        }
        g_active = false;
        finish = g_finish;
        context = g_finish_context;
        g_finish = nullptr;
        g_finish_context = nullptr;

        result.ok = ok;
        result.error = std::move(error);
        result.output = g_output;
    }

    if (finish) {
        finish(context, std::move(result));
    }
}

int report_result(lua_State* L) {
    const auto generation =
        static_cast<std::uint64_t>(lua_tointeger(L, 1));
    const bool ok = lua_toboolean(L, 2) != 0;
    std::string error;
    if (!ok) {
        std::size_t len = 0;
        const char* text = lua_tolstring(L, 3, &len);
        error = text ? std::string(text, len) : std::string("unknown error");
    }
    complete(generation, ok, std::move(error));
    return 0;
}

bool set_thread_capabilities(lua_State* L, std::uint32_t identity,
                             std::uintptr_t caps) {
    if (!L) {
        return false;
    }
    std::uintptr_t userdata = 0;
    if (!read_ptr(reinterpret_cast<std::uintptr_t>(L) + off::kLuaUserdata,
                  userdata) ||
        !userdata) {
        return false;
    }
    write<std::uint32_t>(userdata + off::kExtraIdentity, identity);
    write<std::uint64_t>(userdata + off::kExtraCapabilities,
                         static_cast<std::uint64_t>(caps));
    return true;
}

void set_proto_capabilities(Proto* proto, std::uintptr_t* caps) {
    if (!proto) {
        return;
    }

    std::vector<Proto*> pending{proto};
    while (!pending.empty()) {
        Proto* current = pending.back();
        pending.pop_back();
        if (!current) {
            continue;
        }

        current->userdata = reinterpret_cast<std::uint64_t*>(caps);
        if (current->sizep < 0 || current->sizep > 100000 ||
            (!current->p && current->sizep != 0)) {
            continue;
        }
        for (int i = 0; i < current->sizep; ++i) {
            if (current->p[i]) {
                pending.push_back(current->p[i]);
            }
        }
    }
}

using GetStateFn = lua_State*(__fastcall*)(std::uintptr_t, std::uintptr_t*,
                                           std::uintptr_t*);

__declspec(noinline) lua_State* call_get_state(std::uintptr_t fn,
                                               std::uintptr_t context,
                                               std::uintptr_t* identity,
                                               std::uintptr_t* script) noexcept {
    __try {
        return reinterpret_cast<GetStateFn>(fn)(context, identity, script);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) lua_State* make_thread(lua_State* parent) noexcept {
    __try {
        return lua_newthread(parent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) bool sandbox_thread(lua_State* L) noexcept {
    __try {
        luaL_sandboxthread(L);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool looks_like_thread(lua_State* L) {
    if (!L) {
        return false;
    }
    std::uint8_t type = 0;
    std::uint64_t global = 0;
    if (!read_u8(reinterpret_cast<std::uintptr_t>(L), type) ||
        type != LUA_TTHREAD) {
        return false;
    }
    return read_u64(reinterpret_cast<std::uintptr_t>(L) + off::kLuaGlobal,
                    global) &&
           global != 0;
}

std::string take_error(lua_State* L, const char* fallback) {
    std::size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    std::string message = text ? std::string(text, len) : std::string(fallback);

    luaL_traceback(L, L, message.c_str(), 1);
    text = lua_tolstring(L, -1, &len);
    if (text) {
        message.assign(text, len);
    }
    lua_pop(L, 1);
    return message;
}

// Loads the chunk and the runner, then hands the runner to the client's
// task.defer. Returns false only when nothing was scheduled; `error` is empty
// when the failure was a memory fault (the SEH guard reports that).
bool begin_chunk(const std::string& source, std::uint64_t generation,
                 std::string& error) {
    lua_State* L = g_thread;
    const int top = lua_gettop(L);

    const std::string bytecode = env::compile(source);
    if (luau_load(L, "@zend", bytecode.data(), bytecode.size(), 0) != LUA_OK) {
        error = take_error(L, "bytecode load failed");
        lua_settop(L, top);
        return false;
    }

    Closure* chunk = clvalue(luaA_toobject(L, -1));
    if (chunk && chunk->l.p) {
        set_proto_capabilities(chunk->l.p, &g_capabilities);
    }

    if (luau_load(L, "=zend-runner", g_runner_bytecode.data(),
                  g_runner_bytecode.size(), 0) != LUA_OK) {
        error = take_error(L, "runner load failed");
        lua_settop(L, top);
        return false;
    }

    Closure* runner = clvalue(luaA_toobject(L, -1));
    if (runner && runner->l.p) {
        set_proto_capabilities(runner->l.p, &g_capabilities);
    }

    // Stack: chunk, runner. Prefer task.defer: it moves the chunk onto a
    // scheduler-owned coroutine, which is the only place task.wait and friends
    // are allowed to yield.
    lua_getglobal(L, "task");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "defer");
        if (lua_isfunction(L, -1)) {
            // chunk, runner, task, defer -> defer, runner, chunk, generation
            lua_remove(L, -2); // task
            lua_insert(L, -3);
            lua_insert(L, -2);
            lua_pushinteger(L, static_cast<lua_Integer>(generation));
            const int status = lua_pcall(L, 3, 0, 0);
            if (status != LUA_OK) {
                error = take_error(L, "task.defer failed");
                lua_settop(L, top);
                return false;
            }
            lua_settop(L, top);
            return true;
        }
        lua_pop(L, 1); // non-function defer
    }
    lua_pop(L, 1); // task

    // No task scheduler: run inline. Yields are rejected by the client with
    // "thread is not yieldable", which still reaches the reporter.
    lua_insert(L, -2); // chunk, runner -> runner, chunk
    lua_pushinteger(L, static_cast<lua_Integer>(generation));
    const int status = lua_pcall(L, 2, 0, 0);
    if (status != LUA_OK) {
        // The runner reports through __zend_finish; anything else here is a
        // hard error that never reached it.
        error = take_error(L, "execution failed");
        lua_settop(L, top);
        return false;
    }
    lua_settop(L, top);
    return true;
}

__declspec(noinline) bool raw_read(void* dst, const void* src,
                                   std::size_t size) noexcept {
    __try {
        memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The client keeps its .text encrypted on disk, so the only way to see the
// faulting instructions is to copy them out of live memory.
void dump_fault_bytes(std::uintptr_t address) {
    constexpr std::size_t kBefore = 0x1000;
    constexpr std::size_t kAfter = 0x400;
    static unsigned char image[kBefore + kAfter];
    static char hexline[200];

    if (!raw_read(image, reinterpret_cast<const void*>(address - kBefore),
                  sizeof(image))) {
        log("rbx: fault dump unreadable");
        return;
    }

    const unsigned char* at = image + kBefore;
    for (int row = -2; row < 2; ++row) {
        const unsigned char* p = at + row * 32;
        char* out = hexline;
        for (int i = 0; i < 32; ++i) {
            out += std::snprintf(out, 4, "%02X ", p[i]);
        }
        *out = '\0';
        logf("rbx: fault bytes %+d: %s", row * 32, hexline);
    }

    wchar_t dir[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }

    std::wstring path(dir);
    path += L"\\zend\\logs\\fault.bin";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, image, sizeof(image), &written, nullptr);
    CloseHandle(file);
    logf("rbx: fault window base rva 0x%llX",
         static_cast<unsigned long long>(address - kBefore - off::host_base()));
}

int guard_exception(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    const auto* ctx = info->ContextRecord;
    const auto address =
        reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    const auto base = off::host_base();
    const auto rva = address >= base ? address - base : address;
    logf("rbx: exec fault code=0x%08X rva=0x%llX addr=%p",
         static_cast<unsigned>(record->ExceptionCode),
         static_cast<unsigned long long>(rva),
         reinterpret_cast<void*>(address));
    logf("rbx: fault regs rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p rbp=%p rsp=%p",
         reinterpret_cast<void*>(ctx->Rax), reinterpret_cast<void*>(ctx->Rbx),
         reinterpret_cast<void*>(ctx->Rcx), reinterpret_cast<void*>(ctx->Rdx),
         reinterpret_cast<void*>(ctx->Rsi), reinterpret_cast<void*>(ctx->Rdi),
         reinterpret_cast<void*>(ctx->Rbp), reinterpret_cast<void*>(ctx->Rsp));
    logf("rbx: fault regs r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p",
         reinterpret_cast<void*>(ctx->R8), reinterpret_cast<void*>(ctx->R9),
         reinterpret_cast<void*>(ctx->R10), reinterpret_cast<void*>(ctx->R11),
         reinterpret_cast<void*>(ctx->R12), reinterpret_cast<void*>(ctx->R13),
         reinterpret_cast<void*>(ctx->R14), reinterpret_cast<void*>(ctx->R15));
    dump_fault_bytes(address);
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) bool guarded_begin(const std::string& source,
                                        std::uint64_t generation,
                                        std::string& error) noexcept {
    __try {
        return begin_chunk(source, generation, error);
    } __except (guard_exception(GetExceptionInformation())) {
        return false;
    }
}

} // namespace

namespace executor {

bool init(std::uintptr_t script_context, std::string& error) {
    // The client's VM reads CallInfo::p on every frame, and its own build pins
    // this flag on. Without it luau_precall leaves p null and the client faults
    // on the first call. This must be set before any Lua call.
    FFlag::LuauCIProto.value = true;

    reset();

    if (!script_context) {
        error = "script context is null";
        return false;
    }

    // Needed before the client hands out the main state for this context.
    const std::uint8_t enabled = 1;
    write<std::uint8_t>(script_context + off::kScriptContextRequireBypass, enabled);

    std::uintptr_t identity = 2;
    std::uintptr_t script = 0;
    lua_State* main = call_get_state(off::host_base() + off::kGetScriptVMState,
                                     script_context, &identity, &script);
    if (!looks_like_thread(main)) {
        error = "GetScriptVMState returned no usable thread";
        return false;
    }

    lua_State* thread = make_thread(main);
    if (!thread) {
        error = "lua_newthread returned null";
        return false;
    }

    if (!sandbox_thread(thread)) {
        error = "luaL_sandboxthread raised";
        return false;
    }

    if (!set_thread_capabilities(thread, 8, g_capabilities)) {
        error = "executor thread has no RobloxExtraSpace";
        return false;
    }

    // The client rewrites every opcode byte through a 256-entry table. Our
    // compiler already numbers opcodes like the client's, so the round trip has
    // to be the identity; log the count so a live run settles it either way.
    std::uint8_t wire[256] = {};
    int matching = 0;
    if (try_read_bytes(off::host_base() + off::kOpcodeLookupTable, wire,
                       sizeof(wire))) {
        for (int opcode = 0; opcode < 89; ++opcode) {
            const auto key = static_cast<std::uint8_t>(
                opcode * off::kOpcodeWireMultiplier);
            if (wire[key] == opcode) {
                ++matching;
            }
        }
    } else {
        matching = -1;
    }
    logf("rbx: wire opcode table identity=%d/89", matching);

    // Imports have to resolve at run time, inside the client's xpcall: eager
    // resolution during luau_load would run client __index handlers across our
    // C frames, where a throw cannot be contained.
    lua_setsafeenv(thread, LUA_GLOBALSINDEX, 0);

    // Shadow the sandboxed print/warn so execution output reaches the pipe.
    lua_pushcclosure(thread, capture_print, "print", 0);
    lua_setglobal(thread, "print");
    lua_pushcclosure(thread, capture_warn, "warn", 0);
    lua_setglobal(thread, "warn");

    // Only channel for the Lua runner to hand the result back.
    lua_pushcclosure(thread, report_result, "__zend_finish", 0);
    lua_setglobal(thread, "__zend_finish");

    lua_getglobal(thread, "print");
    const bool print_ours =
        lua_type(thread, -1) == LUA_TFUNCTION &&
        lua_tocfunction(thread, -1) == capture_print;
    lua_pop(thread, 1);
    lua_getglobal(thread, "warn");
    const bool warn_ours =
        lua_type(thread, -1) == LUA_TFUNCTION &&
        lua_tocfunction(thread, -1) == capture_warn;
    lua_pop(thread, 1);
    logf("rbx: print shadow custom=%d warn custom=%d", print_ours ? 1 : 0,
         warn_ours ? 1 : 0);

    // Environment v1 core subset (getgenv/getrenv/..., doc, loadstring) plus
    // the _G alias. Runs after the print/finish shadow so the helpers see the
    // same globals user code does.
    env::install(thread);
    lua_getglobal(thread, "getgenv");
    logf("rbx: environment installed (getgenv=%d)",
         lua_type(thread, -1) == LUA_TFUNCTION ? 1 : 0);
    lua_pop(thread, 1);

    g_runner_bytecode = env::compile(kRunnerSource);
    if (g_runner_bytecode.empty()) {
        error = "runner failed to compile";
        return false;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_thread = thread;
    }
    logf("rbx: executor ready (context=%p main=%p thread=%p)",
         reinterpret_cast<void*>(script_context), static_cast<void*>(main),
         static_cast<void*>(thread));
    return true;
}

void reset() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    env::reset();
    g_thread = nullptr;
    g_active = false;
    g_finish = nullptr;
    g_finish_context = nullptr;
    g_output_id = 0;
    g_output.clear();
    // A suspended chunk from the old VM must never complete a newer dispatch.
    ++g_generation;
}

bool ready() {
    lua_State* thread = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        thread = g_thread;
    }
    return thread != nullptr && looks_like_thread(thread);
}

lua_State* thread() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_thread;
}

void apply_capabilities(Proto* proto) {
    set_proto_capabilities(proto, &g_capabilities);
}

void set_output_sink(OutputFn sink, void* context) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_output_sink = sink;
    g_output_context = context;
}

bool active() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_active;
}

Dispatch dispatch(const std::string& source, std::uint64_t output_id,
                  FinishFn finish, void* context) {
    std::uint64_t generation = 0;
    bool not_ready = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_thread || !looks_like_thread(g_thread)) {
            not_ready = true;
        } else if (g_active) {
            return Dispatch::busy;
        } else {
            g_active = true;
            generation = ++g_generation;
            g_finish = finish;
            g_finish_context = context;
            g_output_id = output_id;
            g_output.clear();
        }
    }

    if (not_ready) {
        ExecResult result;
        result.error = "executor is not initialized (game not loaded yet)";
        if (finish) {
            finish(context, std::move(result));
        }
        return Dispatch::started;
    }

    std::string error;
    if (!guarded_begin(source, generation, error)) {
        complete(generation, false,
                 error.empty() ? "execution raised a memory exception"
                               : std::move(error));
    }
    return Dispatch::started;
}

} // namespace executor

} // namespace zend::rbx
