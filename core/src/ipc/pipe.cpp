#include "ipc/pipe.hpp"

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "util/log.hpp"
#include "rbx/scheduler.hpp"
#include "picojson/picojson.h"

namespace zend {
namespace {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kMaxFrame = 16 * 1024 * 1024;

SessionInfo g_session;
std::atomic<bool> g_detached{false};

// Serializes every frame written to the pipe. The output sink runs on the
// client's scheduler thread while the serve thread is blocked inside
// rbx::run, so replies and live print lines can race; the same lock also
// pins the handle lifetime during disconnect.
std::mutex g_write;
std::atomic<HANDLE> g_pipe{INVALID_HANDLE_VALUE};
HANDLE g_thread = nullptr;

std::uint64_t now_ms() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kEpoch = 116444736000000000ull;
    return v.QuadPart > kEpoch ? (v.QuadPart - kEpoch) / 10000ull : 0;
}

std::wstring widen(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

// SID of the process user (the interactive account), not of the default owner.
// An elevated Roblox token owns its objects as BUILTIN\Administrators, so an
// Owner Rights-only ACE locks the non-elevated UI out of the pipe.
std::wstring user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    std::wstring out;
    if (size != 0 &&
        GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &text)) {
            out.assign(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

// DACL: SYSTEM, the object owner, and the interactive user by SID. The SACL
// pins the mandatory label to Medium so an elevated client's pipe stays usable
// from a normal-integrity UI; Low-integrity processes are still denied write.
std::wstring pipe_sddl() {
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;OW)";
    const std::wstring sid = user_sid();
    if (!sid.empty()) {
        sddl += L"(A;;GA;;;" + sid + L")";
    }
    sddl += L"S:(ML;;NW;;;ME)";
    return sddl;
}

bool read_exact(HANDLE pipe, void* dst, DWORD len) {
    auto* p = static_cast<unsigned char*>(dst);
    while (len != 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, p, len, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        len -= got;
    }
    return true;
}

bool write_all(HANDLE pipe, const void* src, DWORD len) {
    const auto* p = static_cast<const unsigned char*>(src);
    while (len != 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, p, len, &written, nullptr) || written == 0) {
            return false;
        }
        p += written;
        len -= written;
    }
    return true;
}

// Caller must hold g_write.
bool write_json(HANDLE pipe, const picojson::value& value) {
    const std::string body = value.serialize();
    if (body.size() > kMaxFrame) {
        return false;
    }

    const auto len = static_cast<std::uint32_t>(body.size());
    const unsigned char header[4] = {
        static_cast<unsigned char>(len & 0xff),
        static_cast<unsigned char>((len >> 8) & 0xff),
        static_cast<unsigned char>((len >> 16) & 0xff),
        static_cast<unsigned char>((len >> 24) & 0xff),
    };
    return write_all(pipe, header, sizeof(header)) &&
           write_all(pipe, body.data(), static_cast<DWORD>(body.size()));
}

bool send_json(HANDLE pipe, const picojson::value& value) {
    std::lock_guard<std::mutex> lock(g_write);
    return write_json(pipe, value);
}

bool send_event(HANDLE pipe, const char* level, const std::string& msg) {
    picojson::object obj;
    obj["v"] = picojson::value(static_cast<double>(kProtocolVersion));
    obj["op"] = picojson::value("log");
    obj["level"] = picojson::value(level);
    obj["msg"] = picojson::value(msg);
    obj["ts"] = picojson::value(static_cast<double>(now_ms()));
    return send_json(pipe, picojson::value(obj));
}

// Executor output sink. Called from the client scheduler thread while user
// code runs, so it only touches the pipe; the executor calls it outside its
// own lock. One event per line so the UI can log lines as they arrive.
void stream_output(void*, std::uint64_t output_id, bool warn, const char* text,
                   std::size_t size) {
    const std::string_view view = text ? std::string_view(text, size)
                                       : std::string_view();
    const char* kind = warn ? "warn" : "print";

    auto emit = [&](HANDLE pipe, std::string_view line) {
        picojson::object obj;
        obj["v"] = picojson::value(static_cast<double>(kProtocolVersion));
        obj["op"] = picojson::value("output");
        obj["id"] = picojson::value(static_cast<double>(output_id));
        obj["kind"] = picojson::value(kind);
        obj["text"] = picojson::value(std::string(line));
        return write_json(pipe, picojson::value(obj));
    };

    std::lock_guard<std::mutex> lock(g_write);
    const HANDLE pipe = g_pipe.load();
    if (pipe == INVALID_HANDLE_VALUE) {
        return;
    }

    // Keep empty lines ("print('')"), but not the executor's trailing newline.
    std::size_t start = 0;
    while (start < view.size()) {
        const std::size_t nl = view.find('\n', start);
        if (nl == std::string_view::npos) {
            emit(pipe, view.substr(start));
            break;
        }
        emit(pipe, view.substr(start, nl - start));
        start = nl + 1;
    }
    if (view.empty()) {
        emit(pipe, view);
    }
}

const picojson::value* field(const picojson::object& obj, const char* key) {
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}

double id_of(const picojson::object& obj) {
    if (const picojson::value* v = field(obj, "id"); v && v->is<double>()) {
        return v->get<double>();
    }
    return 0;
}

bool handle(HANDLE pipe, const picojson::value& value, bool& greeted, bool& detach) {
    if (!value.is<picojson::object>()) {
        return false;
    }
    const picojson::object& obj = value.get<picojson::object>();

    const picojson::value* op_value = field(obj, "op");
    if (!op_value || !op_value->is<std::string>()) {
        return false;
    }
    const std::string op = op_value->get<std::string>();

    if (!greeted) {
        if (op != "hello") {
            log("ipc: first frame was not hello");
            return false;
        }
        if (const picojson::value* v = field(obj, "v"); v && v->is<double>() &&
            static_cast<std::uint32_t>(v->get<double>()) != kProtocolVersion) {
            log("ipc: hello protocol version mismatch");
            return false;
        }
        const picojson::value* token = field(obj, "token");
        if (!token || !token->is<std::string>() ||
            token->get<std::string>() != g_session.token) {
            log("ipc: hello token mismatch");
            return false;
        }

        greeted = true;
        std::uint32_t peer = 0;
        if (const picojson::value* pid = field(obj, "pid"); pid && pid->is<double>()) {
            peer = static_cast<std::uint32_t>(pid->get<double>());
        }
        logf("ipc: hello from pid %lu", static_cast<unsigned long>(peer));
        send_event(pipe, "info", "zend core attached (build " + g_session.build + ")");
        return true;
    }

    if (op == "ping") {
        picojson::object reply;
        reply["v"] = picojson::value(static_cast<double>(kProtocolVersion));
        reply["op"] = picojson::value("pong");
        reply["id"] = picojson::value(id_of(obj));
        return send_json(pipe, picojson::value(reply));
    }

    if (op == "status") {
        const rbx::Status state = rbx::status();
        picojson::object reply;
        reply["v"] = picojson::value(static_cast<double>(kProtocolVersion));
        reply["op"] = picojson::value("status.result");
        reply["id"] = picojson::value(id_of(obj));
        reply["build"] = picojson::value(g_session.build);
        reply["attached"] = picojson::value(true);
        reply["place_id"] = picojson::value(static_cast<double>(state.place_id));
        reply["queue"] = picojson::value(static_cast<double>(state.queue));
        reply["heat"] = picojson::value(0.0);
        reply["ready"] = picojson::value(state.ready);
        reply["hooked"] = picojson::value(state.hooked);
        return send_json(pipe, picojson::value(reply));
    }

    if (op == "exec") {
        picojson::object reply;
        reply["v"] = picojson::value(static_cast<double>(kProtocolVersion));
        reply["op"] = picojson::value("result");
        reply["id"] = picojson::value(id_of(obj));
        reply["ok"] = picojson::value(false);
        reply["output"] = picojson::value("");
        reply["ms"] = picojson::value(0.0);

        const picojson::value* source = field(obj, "source");
        if (!source || !source->is<std::string>()) {
            reply["error"] = picojson::value("source must be a string");
            return send_json(pipe, picojson::value(reply));
        }

        // The reply only arrives when the chunk finishes, and chunks can yield
        // (task.wait). Callers may ask for a longer bound; scripts keep running
        // in the client either way.
        double timeout = 4000.0;
        if (const picojson::value* limit = field(obj, "timeout_ms");
            limit && limit->is<double>()) {
            timeout = limit->get<double>();
        }
        if (timeout < 1000.0) {
            timeout = 1000.0;
        } else if (timeout > 120000.0) {
            timeout = 120000.0;
        }

        const rbx::ExecResult result =
            rbx::run(source->get<std::string>(),
                     static_cast<std::uint32_t>(timeout),
                     static_cast<std::uint64_t>(id_of(obj)));
        reply["ok"] = picojson::value(result.ok);
        reply["output"] = picojson::value(result.output);
        reply["error"] = picojson::value(result.error);
        reply["ms"] = picojson::value(static_cast<double>(result.ms));
        return send_json(pipe, picojson::value(reply));
    }

    if (op == "detach") {
        log("ipc: detach requested");
        send_event(pipe, "info", "core detached");
        detach = true;
        return true;
    }

    send_event(pipe, "warn", "unknown op: " + op);
    return true;
}

void serve(HANDLE pipe) {
    bool greeted = false;
    bool detach = false;

    while (!detach) {
        unsigned char header[4]{};
        if (!read_exact(pipe, header, sizeof(header))) {
            break;
        }

        const std::uint32_t len = static_cast<std::uint32_t>(header[0]) |
                                  (static_cast<std::uint32_t>(header[1]) << 8) |
                                  (static_cast<std::uint32_t>(header[2]) << 16) |
                                  (static_cast<std::uint32_t>(header[3]) << 24);
        if (len > kMaxFrame) {
            log("ipc: frame exceeds 16 MiB, dropping client");
            break;
        }

        std::string body(len, '\0');
        if (len != 0 && !read_exact(pipe, body.data(), len)) {
            break;
        }

        picojson::value value;
        const std::string error = picojson::parse(value, body);
        if (!error.empty()) {
            send_event(pipe, "warn", "protocol error: " + error);
            continue;
        }

        if (!handle(pipe, value, greeted, detach)) {
            break;
        }
    }

    if (detach) {
        g_detached = true;
        remove_session();
    }
}

DWORD WINAPI serve_thread(void*) {
    // The DACL/SACL matter more than they look: an elevated Roblox owns its
    // pipe as Administrators, so the user SID and the Medium label are what let
    // the normal-integrity UI attach.
    const std::wstring sddl = pipe_sddl();

    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
    } else {
        logf("ipc: pipe SDDL rejected (%lu), using default security",
             static_cast<unsigned long>(GetLastError()));
    }

    const std::wstring name = L"\\\\.\\pipe\\" + widen(g_session.pipe);
    const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE;
    const DWORD pipe_mode =
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;

    HANDLE pipe = CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, 1,
                                   64 * 1024, 64 * 1024, 0,
                                   sd ? &sa : nullptr);
    if (pipe == INVALID_HANDLE_VALUE && sd) {
        // Very old SKUs can reject the label; keep the default DACL rather than
        // losing the pipe, and record it so the log explains the ACL change.
        logf("ipc: labelled pipe failed (%lu), retrying with default security",
             static_cast<unsigned long>(GetLastError()));
        pipe = CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, 1,
                                64 * 1024, 64 * 1024, 0, nullptr);
    }

    if (sd) {
        LocalFree(sd);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        logf("ipc: CreateNamedPipe failed (%lu)", static_cast<unsigned long>(GetLastError()));
        remove_session();
        return 0;
    }

    logf("ipc: listening on %s", g_session.pipe.c_str());
    while (!g_detached.load()) {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error != ERROR_PIPE_CONNECTED) {
            logf("ipc: ConnectNamedPipe failed (%lu)", static_cast<unsigned long>(error));
            break;
        }

        log("ipc: client connected");
        {
            std::lock_guard<std::mutex> lock(g_write);
            g_pipe = pipe;
        }
        serve(pipe);
        {
            // Block new stream writes before the handle goes away; a chunk
            // still suspended in the client keeps running with output dropped.
            std::lock_guard<std::mutex> lock(g_write);
            g_pipe = INVALID_HANDLE_VALUE;
        }
        DisconnectNamedPipe(pipe);
        log("ipc: client disconnected");

        if (!g_detached.load()) {
            Sleep(50);
        }
    }

    CloseHandle(pipe);
    return 0;
}

} // namespace

bool start() {
    if (!create_session(g_session)) {
        log("ipc: could not create session file");
        return false;
    }

    rbx::executor::set_output_sink(&stream_output, nullptr);
    g_thread = CreateThread(nullptr, 0, serve_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        rbx::executor::set_output_sink(nullptr, nullptr);
        log("ipc: could not start pipe thread");
        remove_session();
        return false;
    }

    return true;
}

void stop() {
    // No executor lock here: stop() runs from DllMain detach, where taking the
    // executor mutex could deadlock against the client scheduler thread. The
    // sink only needs the handle check, which is atomic.
    g_pipe.store(INVALID_HANDLE_VALUE);
    g_detached = true;
    remove_session();
}

const SessionInfo& session() {
    return g_session;
}

} // namespace zend
