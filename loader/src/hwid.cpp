#include "emulator/common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>

namespace emu {
namespace {

using RegQExAFn = LSTATUS(WINAPI *)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegQExWFn = LSTATUS(WINAPI *)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetWFn = LSTATUS(WINAPI *)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

RegQExAFn g_RegQExA = nullptr;
RegQExWFn g_RegQExW = nullptr;
RegGetWFn g_RegGetW = nullptr;

bool
isGuidKeyA(LPCSTR Name)
{
    return reinterpret_cast<std::uintptr_t>(Name) > 0xFFFFu &&
           (_stricmp(Name, "MachineGuid") == 0 || _stricmp(Name, "HwProfileGuid") == 0);
}

bool
isGuidKeyW(LPCWSTR Name)
{
    return reinterpret_cast<std::uintptr_t>(Name) > 0xFFFFu &&
           (_wcsicmp(Name, L"MachineGuid") == 0 || _wcsicmp(Name, L"HwProfileGuid") == 0);
}

void
spoofGuidStrA(char *Buf, DWORD Cap, std::uint32_t Salt)
{
    if (!Buf || Cap < 37) {
        return;
    }
    const bool braced = (Buf[0] == '{');
    const std::size_t need = braced ? 39u : 37u;
    if (Cap < need) {
        return;
    }
    std::uint32_t s = g_Seed ^ Salt;
    unsigned char b[16]{};
    for (int i = 0; i < 16; ++i) {
        if ((i & 3) == 0) {
            xors(s);
        }
        b[i] = static_cast<unsigned char>(s >> ((i & 3) * 8));
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0Fu) | 0x40u);
    b[8] = static_cast<unsigned char>((b[8] & 0x3Fu) | 0x80u);
    std::snprintf(Buf, Cap,
                  braced ? "{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}"
                         : "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void
spoofGuidStrW(wchar_t *Buf, DWORD Bytes, std::uint32_t Salt)
{
    if (!Buf || Bytes < 37 * sizeof(wchar_t)) {
        return;
    }
    char tmp[48]{};
    if (Buf[0] == L'{') {
        tmp[0] = '{';
    }
    spoofGuidStrA(tmp, sizeof(tmp), Salt);
    const std::size_t n = std::strlen(tmp);
    if ((n + 1) * sizeof(wchar_t) > Bytes) {
        return;
    }
    for (std::size_t i = 0; i <= n; ++i) {
        Buf[i] = static_cast<wchar_t>(tmp[i]);
    }
}

void
smbios(unsigned char *Buf, std::size_t Len)
{
    std::uint32_t tableLen = 0;
    unsigned char *p;
    unsigned char *end;

    if (!Buf || Len < 8) {
        return;
    }
    std::memcpy(&tableLen, Buf + 4, sizeof(tableLen));
    p = Buf + 8;
    end = p + std::min<std::size_t>(tableLen, Len - 8);
    while (p + 4 <= end) {
        const std::size_t elen = p[1];
        unsigned char *scan;
        std::size_t i;
        std::uint32_t state;

        if (elen < 4 || p + elen > end) {
            break;
        }
        if (p[0] == 1 && elen >= 24) {
            state = g_Seed ^ 0xa511e9b3u;
            for (i = 0; i < 16; ++i) {
                if ((i & 3u) == 0) {
                    xors(state);
                }
                p[8 + i] = static_cast<unsigned char>(state >> ((i & 3u) * 8u));
            }
            p[14] = static_cast<unsigned char>((p[14] & 0x0fu) | 0x40u);
            p[16] = static_cast<unsigned char>((p[16] & 0x3fu) | 0x80u);
        }
        scan = p + elen;
        while (scan + 1 < end && (scan[0] != 0 || scan[1] != 0)) {
            ++scan;
        }
        if (scan + 1 >= end) {
            break;
        }
        p = scan + 2;
    }
}

}

bool
seed()
{
    if constexpr (!kRandHwid && !kRandId) {
        return true;
    }
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&g_Seed), sizeof(g_Seed), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 && g_Seed != 0;
}

UINT WINAPI
firmware(DWORD Provider, DWORD Id, PVOID Buf, DWORD Len)
{
    UINT got;

    if (!g_Firmware) {
        return 0;
    }
    det(DetSource::Hyperion, DetKind::HwidRead, DetVerdict::Observed,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())),
        static_cast<std::uint64_t>(Provider), static_cast<std::uint64_t>(Id));
    got = g_Firmware(Provider, Id, Buf, Len);
    if (kRandHwid && Provider == 0x52534d42u && Buf && got && got <= Len) {
        smbios(static_cast<unsigned char *>(Buf), got);
    }
    return got;
}

ULONG WINAPI
adapters(ULONG Family, ULONG Flags, PVOID Reserved, PIP_ADAPTER_ADDRESSES Buf, PULONG Size)
{
    const ULONG cap = Size ? *Size : 0;
    const ULONG st = g_Adapters ? g_Adapters(Family, Flags, Reserved, Buf, Size) : ERROR_INVALID_FUNCTION;
    std::uintptr_t lo;
    std::uintptr_t hi;
    PIP_ADAPTER_ADDRESSES cur;
    std::uint32_t i;

    det(DetSource::Hyperion, DetKind::HwidRead, DetVerdict::Observed,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())),
        static_cast<std::uint64_t>(Family), static_cast<std::uint64_t>(Flags));

    if (!kRandHwid || st != NO_ERROR || !Buf || !Size || cap == 0) {
        return st;
    }
    lo = reinterpret_cast<std::uintptr_t>(Buf);
    hi = lo + cap;
    if (hi < lo) {
        return st;
    }
    cur = Buf;
    for (i = 0; cur && i < 256; ++i) {
        const auto at = reinterpret_cast<std::uintptr_t>(cur);
        ULONG n;
        std::uint32_t state;
        ULONG j;

        if (at < lo || at > hi || hi - at < sizeof(IP_ADAPTER_ADDRESSES)) {
            break;
        }
        n = std::min<ULONG>(cur->PhysicalAddressLength, MAX_ADAPTER_ADDRESS_LENGTH);
        state = g_Seed ^ cur->IfIndex ^ (cur->Ipv6IfIndex << 16) ^ (0x9e3779b9u * (i + 1u));
        for (j = 0; j < n; ++j) {
            if ((j & 3u) == 0) {
                xors(state);
            }
            cur->PhysicalAddress[j] = static_cast<unsigned char>(state >> ((j & 3u) * 8u));
        }
        if (n) {
            cur->PhysicalAddress[0] = static_cast<unsigned char>((cur->PhysicalAddress[0] | 2u) & 0xfeu);
        }
        cur = cur->Next;
    }
    return st;
}

void
setRegHooks(FARPROC QExA, FARPROC QExW, FARPROC GetW)
{
    if (QExA && !g_RegQExA) {
        g_RegQExA = reinterpret_cast<RegQExAFn>(QExA);
    }
    if (QExW && !g_RegQExW) {
        g_RegQExW = reinterpret_cast<RegQExWFn>(QExW);
    }
    if (GetW && !g_RegGetW) {
        g_RegGetW = reinterpret_cast<RegGetWFn>(GetW);
    }
}

LSTATUS WINAPI
regQExA(HKEY Key, LPCSTR Val, LPDWORD Res, LPDWORD Type, LPBYTE Data, LPDWORD Size)
{
    const LSTATUS st = g_RegQExA ? g_RegQExA(Key, Val, Res, Type, Data, Size) : ERROR_INVALID_FUNCTION;
    if (kRandHwid && st == ERROR_SUCCESS && Data && Size && isGuidKeyA(Val)) {
        spoofGuidStrA(reinterpret_cast<char *>(Data), *Size, 0x4d475549u);
    }
    return st;
}

LSTATUS WINAPI
regQExW(HKEY Key, LPCWSTR Val, LPDWORD Res, LPDWORD Type, LPBYTE Data, LPDWORD Size)
{
    const LSTATUS st = g_RegQExW ? g_RegQExW(Key, Val, Res, Type, Data, Size) : ERROR_INVALID_FUNCTION;
    if (kRandHwid && st == ERROR_SUCCESS && Data && Size && isGuidKeyW(Val)) {
        spoofGuidStrW(reinterpret_cast<wchar_t *>(Data), *Size, 0x4d475549u);
    }
    return st;
}

LSTATUS WINAPI
regGetW(HKEY Key, LPCWSTR Sub, LPCWSTR Val, DWORD Flags, LPDWORD Type, PVOID Data, LPDWORD Size)
{
    const LSTATUS st = g_RegGetW ? g_RegGetW(Key, Sub, Val, Flags, Type, Data, Size) : ERROR_INVALID_FUNCTION;
    if (kRandHwid && st == ERROR_SUCCESS && Data && Size && isGuidKeyW(Val)) {
        spoofGuidStrW(static_cast<wchar_t *>(Data), *Size, 0x4d475549u);
    }
    return st;
}

}
