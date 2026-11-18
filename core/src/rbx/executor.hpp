#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct lua_State;
struct Proto;

namespace zend::rbx {

struct ExecResult {
    bool ok = false;
    std::string output;
    std::string error;
    std::uint64_t ms = 0;
};

namespace executor {

// Called once per dispatch, from whichever client thread finishes the chunk
// (the scheduler thread, not the step thread). The context is opaque to the
// executor.
using FinishFn = void (*)(void* context, ExecResult result);

// Receives formatted print/warn output while the chunk is still running.
// `output_id` is the tag given to dispatch(). Called from the thread that runs
// user code, so it must not touch Lua state and may block.
using OutputFn = void (*)(void* context, std::uint64_t output_id, bool warn,
                          const char* text, std::size_t size);

enum class Dispatch {
    started, // handed to the client scheduler; FinishFn will fire later
    busy,    // a previous chunk is still running (it yielded); retry next step
};

// Creates the persistent executor thread from the client's script context.
// Must run on the scheduler step thread.
bool init(std::uintptr_t script_context, std::string& error);

// Drops our references without touching client memory. Safe to call after a
// DataModel change; a chunk that was still suspended in the old VM is ignored
// by its generation and can never report into a newer job.
void reset();

bool ready();

// True while a dispatched chunk has not reported back yet.
bool active();

// The persistent executor thread, or null before init / after reset. Used by
// the environment library to reach our globals from a scheduler coroutine.
lua_State* thread();

// Tags every Proto in the tree with the core's capability mask so the client
// accepts calls made from the compiled chunk.
void apply_capabilities(Proto* proto);

// Installs or clears the live output sink. The executor copies the function
// and context under its lock; pass null to disable streaming.
void set_output_sink(OutputFn sink, void* context);

// Compiles, loads and hands one chunk to the client's `task.defer` so yields
// (task.wait, task.spawn, ...) inside it are legal. Load errors are reported
// through FinishFn synchronously. `output_id` is passed back to the installed
// output sink so live lines can be attributed to the request that started the
// chunk. Returns Dispatch::busy without starting anything while a previous
// chunk is still suspended.
Dispatch dispatch(const std::string& source, std::uint64_t output_id,
                  FinishFn finish, void* context);

} // namespace executor

} // namespace zend::rbx
