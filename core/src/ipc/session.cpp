#include "ipc/session.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "picojson/picojson.h"

namespace zend {
namespace {

std::wstring g_session_path;

std::wstring base_dir() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }

    std::wstring dir(base);
    dir += L"\\zend";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\runtime";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

std::string base64(const unsigned char* data, std::size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    if (i + 1 == size) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == size) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::uint64_t unix_seconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kEpoch = 116444736000000000ull;
    if (v.QuadPart <= kEpoch) {
        return 0;
    }
    return (v.QuadPart - kEpoch) / 10000000ull;
}

bool random_bytes(unsigned char* out, std::size_t size) {
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool write_file(const std::wstring& path, const std::string& body) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool ok = true;
    std::size_t done = 0;
    while (done < body.size()) {
        const DWORD part = static_cast<DWORD>(body.size() - done);
        DWORD written = 0;
        if (!WriteFile(h, body.data() + done, part, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        done += written;
    }
    CloseHandle(h);
    return ok;
}

} // namespace

bool create_session(SessionInfo& out) {
    const std::wstring dir = base_dir();
    if (dir.empty()) {
        return false;
    }

    unsigned char nonce[16]{};
    unsigned char token[16]{};
    if (!random_bytes(nonce, sizeof(nonce)) || !random_bytes(token, sizeof(token))) {
        return false;
    }

    out.pid = GetCurrentProcessId();
    out.pipe = "zend-" + hex(nonce, sizeof(nonce));
    out.token = base64(token, sizeof(token));
    out.build = kBuildId;
    out.ts = unix_seconds();

    picojson::object obj;
    obj["v"] = picojson::value(1.0);
    obj["pid"] = picojson::value(static_cast<double>(out.pid));
    obj["pipe"] = picojson::value(out.pipe);
    obj["token"] = picojson::value(out.token);
    obj["build"] = picojson::value(out.build);
    obj["ts"] = picojson::value(static_cast<double>(out.ts));

    wchar_t name[64]{};
    std::swprintf(name, 64, L"\\session-%lu.json", static_cast<unsigned long>(out.pid));

    const std::wstring final_path = dir + name;
    const std::wstring tmp_path = final_path + L".tmp";

    if (!write_file(tmp_path, picojson::value(obj).serialize())) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }

    g_session_path = final_path;
    return true;
}

void remove_session() {
    if (!g_session_path.empty()) {
        DeleteFileW(g_session_path.c_str());
        g_session_path.clear();
    }
}

} // namespace zend
