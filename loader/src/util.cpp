#include "emulator/common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace emu {
namespace {

std::uintptr_t
pageMask()
{
    static const std::uintptr_t mask = [] {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return static_cast<std::uintptr_t>(si.dwPageSize) - 1u;
    }();
    return mask;
}

}

std::wstring
path(const std::wstring &Dir, const wchar_t *Name)
{
    if (!Name) {
        return {};
    }
    if (Dir.empty()) {
        return Name;
    }
    if (Dir.back() == L'\\') {
        return Dir + Name;
    }
    return Dir + L'\\' + Name;
}

bool
wr(void *Dst, const void *Src, std::size_t Size, DWORD Prot)
{
    constexpr std::size_t kChunk = 0x100000;
    auto *dst = static_cast<unsigned char *>(Dst);
    const auto *src = static_cast<const unsigned char *>(Src);
    std::size_t done = 0;

    if (!Dst || !Src || !Size) {
        return false;
    }
    while (done < Size) {
        const std::size_t part = std::min<std::size_t>(kChunk, Size - done);
        DWORD old = 0;
        DWORD tmp = 0;

        if (!VirtualProtect(dst + done, part, PAGE_EXECUTE_READWRITE, &old)) {
            return false;
        }
        std::memcpy(dst + done, src + done, part);
        if (!FlushInstructionCache(GetCurrentProcess(), dst + done, part)) {
            VirtualProtect(dst + done, part, old, &tmp);
            return false;
        }
        if (!VirtualProtect(dst + done, part, Prot, &tmp)) {
            VirtualProtect(dst + done, part, old, &tmp);
            return false;
        }
        done += part;
    }
    return true;
}

bool
cfg(void *Ptr)
{
    CFG_CALL_TARGET_INFO info{};
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY policy{};
    std::uintptr_t u;
    std::uintptr_t mask;
    std::uintptr_t page;
    std::uintptr_t slot;
    BOOL ok;
    BOOL query;

    if (!Ptr) {
        return false;
    }
    u = reinterpret_cast<std::uintptr_t>(Ptr);
    mask = pageMask();
    page = u & ~mask;
    slot = u & ~static_cast<std::uintptr_t>(0x0Fu);
    info.Offset = slot - page;
    info.Flags = CFG_CALL_TARGET_VALID;
    ok = SetProcessValidCallTargets(GetCurrentProcess(), reinterpret_cast<void *>(page), mask + 1u, 1, &info);
    query = GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &policy, sizeof(policy));
    return !query || !policy.EnableControlFlowGuard || ok;
}

void
jmpAbs(unsigned char Out[12], const void *Target)
{
    Out[0] = 0x48;
    Out[1] = 0xB8;
    std::memcpy(Out + 2, &Target, sizeof(Target));
    Out[10] = 0xFF;
    Out[11] = 0xE0;
}

bool
jmpRel(void *From, const void *To, DWORD Prot)
{
    const auto site = static_cast<unsigned char *>(From);
    const auto dest = reinterpret_cast<const unsigned char *>(To);
    const std::int64_t rel = dest - (site + 5);
    unsigned char jmp[12] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    std::int32_t disp;

    if (rel < INT32_MIN || rel > INT32_MAX) {
        return false;
    }
    disp = static_cast<std::int32_t>(rel);
    std::memcpy(jmp + 1, &disp, sizeof(disp));
    return wr(site, jmp, sizeof(jmp), Prot);
}

std::uint32_t
xors(std::uint32_t &State)
{
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

void
trace(const char *Text)
{
    if constexpr (!kTrace) {
        (void)Text;
        return;
    }

    static const std::wstring file = [] {
        wchar_t base[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (!len || len >= MAX_PATH) {
            return std::wstring{};
        }
        std::wstring dir(base);
        dir += L"\\zend";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\loader.log";
        return dir;
    }();

    if (file.empty()) {
        return;
    }

    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    char line[512]{};
    const int n = std::snprintf(line, sizeof(line), "[%lu] %s\r\n",
                                static_cast<unsigned long>(GetCurrentThreadId()),
                                Text ? Text : "");
    DWORD written = 0;
    if (n > 0) {
        const auto len = static_cast<DWORD>(std::min<int>(n, static_cast<int>(sizeof(line) - 1)));
        WriteFile(h, line, len, &written, nullptr);
    }
    CloseHandle(h);
}

}
