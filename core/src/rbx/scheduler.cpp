#include "rbx/scheduler.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rbx/memory.hpp"
#include "rbx/offsets.hpp"
#include "util/log.hpp"

namespace zend::rbx {
namespace {

constexpr std::uint32_t kJobVtableSlots = 9;
constexpr std::uint32_t kMaxJobs = 65536;

struct Job {
    std::uint64_t id = 0;
    // IPC request id, so streamed lines reach the client that started the job.
    std::uint64_t output_id = 0;
    std::string source;
    ExecResult result;
    bool done = false;
};

std::mutex g_lifecycle; // guards hook state, executor init and execution
std::mutex g_queue_mutex;
std::condition_variable g_done_cv;
std::deque<std::shared_ptr<Job>> g_queue;

// Dispatched chunks that have not reported yet. Holding the shared_ptr here
// keeps the completion callback (which runs on a client scheduler thread) safe
// even when the pipe client already gave up waiting.
std::map<std::uint64_t, std::shared_ptr<Job>> g_inflight;
std::uint64_t g_next_job = 1;

std::uintptr_t g_job = 0;
std::uintptr_t g_data_model = 0;
std::uintptr_t* g_vtable = nullptr;
std::uintptr_t g_original_vtable = 0;
std::uintptr_t (*g_original_step)(std::uintptr_t, std::uintptr_t) = nullptr;
bool g_ready = false;
bool g_init_failure_logged = false;

std::atomic<bool> g_stopping{false};
HANDLE g_watcher = nullptr;

using StepFn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);

std::uintptr_t __fastcall step_hook(std::uintptr_t job, std::uintptr_t stats);

std::uintptr_t get_data_model() {
    std::uintptr_t fake = 0;
    if (!read_ptr(off::host_base() + off::kFakeDataModelPointer, fake) ||
        !is_user_ptr(fake)) {
        return 0;
    }
    std::uintptr_t data_model = 0;
    if (!read_ptr(fake + off::kFakeDataModelToDataModel, data_model) ||
        !is_user_ptr(data_model)) {
        return 0;
    }
    return data_model;
}

std::int64_t get_place_id(std::uintptr_t data_model) {
    if (!is_user_ptr(data_model)) {
        return 0;
    }
    std::int64_t place = 0;
    if (!read(data_model + off::kPlaceId, place)) {
        return 0;
    }
    return place;
}

// Returns the job whose instances belong to `data_model`, or 0.
std::uintptr_t find_job_impl(const char* type_name, std::uintptr_t data_model) {
    const std::size_t name_len = std::strlen(type_name);
    if (name_len == 0 || name_len >= 0x100) {
        return 0;
    }

    std::uintptr_t scheduler = 0;
    if (!read_ptr(off::host_base() + off::kRawScheduler, scheduler) ||
        !is_user_ptr(scheduler)) {
        return 0;
    }

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!read_ptr(scheduler + off::kJobStart, begin) ||
        !read_ptr(scheduler + off::kJobEnd, end)) {
        return 0;
    }
    if (!is_user_ptr(begin) || !is_user_ptr(end) || begin >= end) {
        return 0;
    }
    const std::uintptr_t span = end - begin;
    if (span % 0x10 != 0 || span / 0x10 > kMaxJobs) {
        return 0;
    }

    for (std::uintptr_t entry = begin; entry < end; entry += 0x10) {
        std::uintptr_t job = 0;
        if (!read_ptr(entry, job) || !is_user_ptr(job)) {
            continue;
        }

        const std::uintptr_t name = job + off::kJobTypeName;
        std::uint64_t size = 0;
        std::uint64_t capacity = 0;
        if (!read_u64(name + 0x10, size) || !read_u64(name + 0x18, capacity)) {
            continue;
        }
        if (size != name_len || capacity < size || capacity > 0x10000) {
            continue;
        }

        std::uintptr_t text = name;
        if (capacity >= 16 && (!read_ptr(name, text) || !is_user_ptr(text))) {
            continue;
        }

        char candidate[0x100] = {};
        if (!try_read_bytes(text, candidate, name_len) ||
            std::memcmp(candidate, type_name, name_len) != 0) {
            continue;
        }

        if (data_model) {
            std::uintptr_t context = 0;
            if (!read_ptr(job + off::kJobScriptContext, context) ||
                !is_user_ptr(context)) {
                continue;
            }
            std::uintptr_t owner = 0;
            if (!read_ptr(context + off::kScriptContextDataModel, owner)) {
                continue;
            }
            if (owner != data_model &&
                owner + off::kInstanceSubobject != data_model) {
                continue;
            }
        }
        return job;
    }

    return 0;
}

std::uintptr_t script_context_for(std::uintptr_t job,
                                  std::uintptr_t data_model) {
    std::uintptr_t context = 0;
    if (!read_ptr(job + off::kJobScriptContext, context) ||
        !is_user_ptr(context)) {
        return 0;
    }
    std::uintptr_t owner = 0;
    if (!read_ptr(context + off::kScriptContextDataModel, owner)) {
        return 0;
    }
    if (owner != data_model && owner + off::kInstanceSubobject != data_model) {
        return 0;
    }
    return context;
}

bool install_hook(std::uintptr_t job) {
    std::uintptr_t original_vtable = 0;
    if (!read_ptr(job, original_vtable) || !is_user_ptr(original_vtable)) {
        return false;
    }

    std::uintptr_t slots[kJobVtableSlots] = {};
    for (std::uint32_t i = 0; i < kJobVtableSlots; ++i) {
        if (!read_ptr(original_vtable + i * sizeof(std::uintptr_t), slots[i])) {
            return false;
        }
    }
    if (!slots[8]) {
        return false;
    }

    auto* clone = new (std::nothrow) std::uintptr_t[kJobVtableSlots];
    if (!clone) {
        return false;
    }
    std::memcpy(clone, slots, sizeof(slots));
    clone[8] = reinterpret_cast<std::uintptr_t>(&step_hook);

    if (!write_ptr(job, reinterpret_cast<std::uintptr_t>(clone))) {
        delete[] clone;
        return false;
    }

    g_vtable = clone;
    g_original_vtable = original_vtable;
    g_original_step = reinterpret_cast<StepFn>(slots[8]);
    g_job = job;
    return true;
}

void uninstall_hook() {
    if (g_job && g_vtable) {
        std::uintptr_t current = 0;
        if (read_ptr(g_job, current) &&
            current == reinterpret_cast<std::uintptr_t>(g_vtable)) {
            write_ptr(g_job, g_original_vtable);
        }
    }
    delete[] g_vtable;
    g_vtable = nullptr;
    g_job = 0;
    g_original_vtable = 0;
    // g_original_step stays: a step that already loaded the pointer from the
    // old vtable may still enter the hook and must be forwarded.
}

// Runs on whichever client thread finished the chunk. The id, not a pointer,
// is the context: a completion that arrives after a reset simply finds nothing.
void complete_job(void* context, ExecResult result) {
    const auto id = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(context));
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        const auto it = g_inflight.find(id);
        if (it == g_inflight.end()) {
            return;
        }
        it->second->result = std::move(result);
        it->second->done = true;
        g_inflight.erase(it);
    }
    g_done_cv.notify_all();
}

void drain_queue() {
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            if (g_queue.empty()) {
                return;
            }
            job = g_queue.front();
            g_queue.pop_front();
            job->id = g_next_job++;
            g_inflight[job->id] = job;
        }

        // The chunk keeps running on the client scheduler after this returns;
        // its completion arrives later through complete_job.
        const auto state = executor::dispatch(
            job->source, job->output_id, &complete_job,
            reinterpret_cast<void*>(job->id));
        if (state == executor::Dispatch::busy) {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            g_inflight.erase(job->id);
            g_queue.push_front(job);
            return;
        }
    }
}

// Wakes every pipe waiter with an honest error when the DataModel goes away.
// The suspended coroutines belong to the old VM and are never resumed by it.
void fail_pending(const char* reason) {
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        for (const auto& job : g_queue) {
            job->result.error = reason;
            job->done = true;
        }
        g_queue.clear();
        for (auto& entry : g_inflight) {
            entry.second->result.error = reason;
            entry.second->done = true;
        }
        g_inflight.clear();
    }
    g_done_cv.notify_all();
}

std::uintptr_t __fastcall step_hook(std::uintptr_t job, std::uintptr_t stats) {
    StepFn original = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_lifecycle);
        original = g_original_step;

        if (g_job && job == g_job) {
            if (!executor::ready()) {
                const std::uintptr_t context =
                    script_context_for(g_job, g_data_model);
                std::string error;
                if (context && executor::init(context, error)) {
                    g_ready = true;
                    g_init_failure_logged = false;
                } else if (!g_init_failure_logged) {
                    g_init_failure_logged = true;
                    logf("rbx: executor init failed: %s",
                         error.empty() ? "no script context" : error.c_str());
                }
            }
            if (executor::ready()) {
                g_ready = true;
                drain_queue();
            }
        }
    }

    if (original) {
        return original(job, stats);
    }
    return 0;
}

DWORD WINAPI watch_thread(void*) {
    log("rbx: scheduler watcher started");

    while (!g_stopping.load()) {
        const std::uintptr_t data_model = get_data_model();
        const std::int64_t place = get_place_id(data_model);

        if (!data_model || place <= 0) {
            std::lock_guard<std::mutex> lock(g_lifecycle);
            if (g_job) {
                log("rbx: DataModel left; restoring job vtable");
                uninstall_hook();
                fail_pending("the game session ended before the script finished");
                executor::reset();
                g_ready = false;
                g_init_failure_logged = false;
            }
            g_data_model = 0;
            Sleep(200);
            continue;
        }

        const std::uintptr_t job = find_job_impl("WaitingHybridScriptsJob", data_model);

        {
            std::lock_guard<std::mutex> lock(g_lifecycle);
            if (job != g_job || data_model != g_data_model) {
                if (g_job) {
                    uninstall_hook();
                }
                fail_pending("the game session ended before the script finished");
                executor::reset();
                g_ready = false;
                g_init_failure_logged = false;
                g_data_model = data_model;

                if (job && install_hook(job)) {
                    logf("rbx: hooked WaitingHybridScriptsJob=%p vtable=%p step=%p "
                         "place=%lld",
                         reinterpret_cast<void*>(job),
                         static_cast<void*>(g_vtable),
                         reinterpret_cast<void*>(g_original_step),
                         static_cast<long long>(place));
                } else if (!job) {
                    // Game still building the job; retry quietly.
                } else {
                    log("rbx: job vtable hook failed");
                }
            }
        }

        Sleep(job ? 250 : 100);
    }

    log("rbx: scheduler watcher stopped");
    return 0;
}

} // namespace

std::uintptr_t find_job(const char* type_name, std::uintptr_t data_model) {
    return find_job_impl(type_name, data_model);
}

void start() {
    if (g_watcher) {
        return;
    }
    g_stopping = false;
    g_watcher = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    if (!g_watcher) {
        log("rbx: could not start scheduler watcher");
    }
}

void stop() {
    g_stopping = true;

    // Deliberately lock-free: DllMain detach can run after other threads have
    // been terminated. Restoring the pointer is the only thing that matters.
    if (g_job && g_vtable) {
        std::uintptr_t current = 0;
        if (read_ptr(g_job, current) &&
            current == reinterpret_cast<std::uintptr_t>(g_vtable)) {
            write_ptr(g_job, g_original_vtable);
        }
    }
    g_job = 0;
    g_vtable = nullptr;
    g_ready = false;
}

Status status() {
    Status out;
    std::lock_guard<std::mutex> lock(g_lifecycle);
    out.hooked = g_job != 0;
    out.ready = g_ready;
    out.place_id = get_place_id(g_data_model);
    {
        std::lock_guard<std::mutex> queue_lock(g_queue_mutex);
        out.queue = static_cast<std::uint32_t>(g_queue.size());
    }
    return out;
}

ExecResult run(const std::string& source, std::uint32_t timeout_ms,
               std::uint64_t output_id) {
    const auto started = std::chrono::steady_clock::now();

    auto job = std::make_shared<Job>();
    job->source = source;
    job->output_id = output_id;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.push_back(job);
    }

    ExecResult result;
    {
        std::unique_lock<std::mutex> lock(g_queue_mutex);
        const bool finished = g_done_cv.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [&job] { return job->done; });
        if (!finished) {
            bool still_queued = false;
            for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
                if (*it == job) {
                    g_queue.erase(it);
                    still_queued = true;
                    break;
                }
            }
            // A chunk that already started keeps running in the client; the
            // pipe reply is only a timeout, not a cancellation.
            result.error =
                still_queued
                    ? "timed out waiting for the script scheduler (is the game loaded?)"
                    : "the script is still running after yielding; it will keep "
                      "running in the client and stream its output";
        } else {
            result = job->result;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    result.ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - started)
            .count());
    return result;
}

} // namespace zend::rbx
