#pragma once

#include <cstdint>
#include <string>

#include "rbx/executor.hpp"

namespace zend::rbx {

struct Status {
    std::int64_t place_id = 0;
    std::uint32_t queue = 0;
    bool hooked = false;
    bool ready = false;
};

// Returns the first scheduler job with that type name, or 0. `data_model` may
// be 0 to skip the ScriptContext ownership check (jobs such as RenderJob do
// not have one).
std::uintptr_t find_job(const char* type_name, std::uintptr_t data_model = 0);

// Starts the watcher that finds the WaitingHybridScriptsJob and installs the
// step callback once a DataModel has a place. Idempotent.
void start();

// Best effort, no locks: called from DllMain detach where other threads may
// already be gone. Restores the job vtable if it still points at our clone.
void stop();

Status status();

// Queues a chunk for the step thread and waits for the result. The timeout
// bounds the wait for the scheduler, not the script's own run time.
// `output_id` tags the chunk's live print/warn lines (the IPC request id).
ExecResult run(const std::string& source, std::uint32_t timeout_ms,
               std::uint64_t output_id);

} // namespace zend::rbx
