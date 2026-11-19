#pragma once

// Client for the bundled Tovek decompiler (MIT, Kiet1308/Tovek). The binary
// ships next to module.dll and speaks HTTP on a loopback port. Everything here
// is best-effort: when the helper is missing or fails, the caller falls back to
// the bytecode stub instead of erroring.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <winhttp.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "util/log.hpp"

namespace zend::rbx::decompiler {

inline constexpr const char* kExeName = "Decompiler.exe";
// The helper has no port override; it always binds the loopback default.
inline constexpr int kPort = 3000;

inline std::mutex g_mutex;
inline HANDLE g_process = nullptr;
inline HANDLE g_job = nullptr;
inline int g_port = 0;

inline void anchor() {}

inline std::wstring exe_path() {
    wchar_t path[MAX_PATH] = {};

    // The loader deploys module.dll into the client's version directory and may
    // unlink it from the PEB, so the host executable is the reliable anchor.
    HMODULE module = nullptr;
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        const auto address =
            reinterpret_cast<LPCSTR>(reinterpret_cast<std::uintptr_t>(&anchor));
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                address, &module) ||
            !GetModuleFileNameW(module, path, MAX_PATH)) {
            return {};
        }
    }

    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (!slash) {
        return {};
    }
    slash[1] = L'\0';
    std::wstring exe(path);
    for (const char* c = kExeName; *c; ++c) {
        exe.push_back(static_cast<wchar_t>(*c));
    }
    return exe;
}

inline bool port_open(int port) {
    if (port <= 0) {
        return false;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }

    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));

    const bool up = connect(socket_handle,
                            reinterpret_cast<SOCKADDR*>(&address),
                            sizeof(address)) == 0;
    closesocket(socket_handle);
    WSACleanup();
    return up;
}

// Caller holds g_mutex.
inline bool ensure_running_locked(std::string& error) {
    g_port = kPort;
    if (port_open(g_port)) {
        return true;
    }
    if (g_process) {
        CloseHandle(g_process);
        g_process = nullptr;
    }

    const std::wstring exe = exe_path();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "Decompiler.exe is not next to module.dll";
        return false;
    }

    if (!g_job) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (g_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits));
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe + L"\"";

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        error = "could not start Decompiler.exe (" +
                std::to_string(GetLastError()) + ")";
        g_port = 0;
        return false;
    }

    if (g_job) {
        AssignProcessToJobObject(g_job, process.hProcess);
    }
    CloseHandle(process.hThread);
    g_process = process.hProcess;

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (port_open(g_port)) {
            zend::logf("rbx.decompiler: helper pid %lu on port %d",
                       process.dwProcessId, g_port);
            return true;
        }
        if (WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0) {
            error = "Decompiler.exe exited during startup";
            CloseHandle(g_process);
            g_process = nullptr;
            g_port = 0;
            return false;
        }
        Sleep(100);
    }

    error = "Decompiler.exe did not open port " + std::to_string(g_port);
    TerminateProcess(g_process, 1);
    CloseHandle(g_process);
    g_process = nullptr;
    g_port = 0;
    return false;
}

inline std::string base64(const std::string& in) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned value = (static_cast<unsigned char>(in[i]) << 16) |
                               (static_cast<unsigned char>(in[i + 1]) << 8) |
                               static_cast<unsigned char>(in[i + 2]);
        out += kTable[(value >> 18) & 63];
        out += kTable[(value >> 12) & 63];
        out += kTable[(value >> 6) & 63];
        out += kTable[value & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned value = static_cast<unsigned char>(in[i]) << 16;
        out += kTable[(value >> 18) & 63];
        out += kTable[(value >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned value =
            (static_cast<unsigned char>(in[i]) << 16) |
            (static_cast<unsigned char>(in[i + 1]) << 8);
        out += kTable[(value >> 18) & 63];
        out += kTable[(value >> 12) & 63];
        out += kTable[(value >> 6) & 63];
        out += '=';
    }
    return out;
}

inline bool post_locked(const std::string& body, std::string& out,
                        std::string& error) {
    HINTERNET session =
        WinHttpOpen(L"ZEND", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 15000, 30000, 120000);

    HINTERNET connection = WinHttpConnect(
        session, L"127.0.0.1", static_cast<INTERNET_PORT>(g_port), 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        error = "WinHttpConnect failed";
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", L"/decompile", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = "WinHttpOpenRequest failed";
        return false;
    }

    static const wchar_t kHeaders[] = L"Content-Type: text/plain\r\n";
    bool sent =
        WinHttpSendRequest(request, kHeaders,
                           static_cast<DWORD>(sizeof(kHeaders) / sizeof(wchar_t) - 1),
                           const_cast<char*>(body.data()),
                           static_cast<DWORD>(body.size()),
                           static_cast<DWORD>(body.size()), 0) != FALSE;
    if (sent) {
        sent = WinHttpReceiveResponse(request, nullptr) != FALSE;
    }
    if (!sent) {
        const DWORD code = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = "request failed (" + std::to_string(code) + ")";
        return false;
    }

    DWORD status = 0;
    DWORD length = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                        WINHTTP_NO_HEADER_INDEX);

    out.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) ||
            read == 0) {
            break;
        }
        out.append(buffer.data(), read);
        if (out.size() > (64u << 20)) {
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (status != 200) {
        error = "helper returned " + std::to_string(status);
        if (!out.empty()) {
            error += ": " + out.substr(0, 200);
        }
        return false;
    }
    return true;
}

// Decompiles raw Luau bytecode (the decoded blob, not the RSB1 container).
// Returns false and fills `error` when the helper is unavailable or rejects the
// input; callers keep their own fallback.
inline bool decompile(const std::string& bytecode, std::string& out,
                      std::string& error) {
    if (bytecode.empty()) {
        error = "empty bytecode";
        return false;
    }

    const std::lock_guard<std::mutex> lock(g_mutex);
    if (!ensure_running_locked(error)) {
        return false;
    }
    return post_locked(base64(bytecode), out, error);
}

} // namespace zend::rbx::decompiler
