#include "util/log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace zend {
namespace {

std::mutex g_log_mutex;

std::wstring log_path() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"zend-core.log";
    }

    std::wstring dir(base);
    dir += L"\\zend\\logs";
    CreateDirectoryW((std::wstring(base) + L"\\zend").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    dir += L"\\core.log";
    return dir;
}

} // namespace

void log(std::string_view line) noexcept {
    std::lock_guard<std::mutex> lock(g_log_mutex);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char stamp[32]{};
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::string out(stamp);
    out.append(line);
    out.push_back('\n');

    const std::wstring path = log_path();
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(h);
}

void logf(const char* fmt, ...) noexcept {
    char buf[1024]{};

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    log(buf);
}

} // namespace zend
