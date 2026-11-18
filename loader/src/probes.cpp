#include "emulator/common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <tlhelp32.h>
#include <ws2tcpip.h>

namespace emu {
namespace {

using VirtualQueryFn = SIZE_T(WINAPI *)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
using VirtualProtectFn = BOOL(WINAPI *)(LPVOID, SIZE_T, DWORD, PDWORD);
using DebuggerFn = BOOL(WINAPI *)();
using ProcAddrFn = FARPROC(WINAPI *)(HMODULE, LPCSTR);
using ToolhelpFn = HANDLE(WINAPI *)(DWORD, DWORD);
using EnumModFn = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);
using ModHandleWFn = HMODULE(WINAPI *)(LPCWSTR);
using ModHandleAFn = HMODULE(WINAPI *)(LPCSTR);
using ModHandleExWFn = BOOL(WINAPI *)(DWORD, LPCWSTR, HMODULE *);
using OpenProcFn = HANDLE(WINAPI *)(DWORD, BOOL, DWORD);
using LookupFn = PRUNTIME_FUNCTION(NTAPI *)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
using Tick64Fn = ULONGLONG(WINAPI *)();
using QpcFn = BOOL(WINAPI *)(LARGE_INTEGER *);
using WsaSendToFn = int(WSAAPI *)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr *, int,
                                  LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using SendToFn = int(WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int);
using NtQsiFn = NTSTATUS(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
using NtQvmFn = NTSTATUS(NTAPI *)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, PSIZE_T);
using CrdpFn = BOOL(WINAPI *)(HANDLE, PBOOL);

struct RtlProcModInfo {
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
};
static_assert(sizeof(RtlProcModInfo) == 0x128, "x64 RTL_PROCESS_MODULE_INFORMATION size");

struct RtlProcMods {
    ULONG NumberOfModules;
    RtlProcModInfo Modules[1];
};
static_assert(offsetof(RtlProcMods, Modules) == 8, "x64 RTL_PROCESS_MODULES offset");

VirtualQueryFn g_Vq = nullptr;
VirtualProtectFn g_Vp = nullptr;
DebuggerFn g_Dbg = nullptr;
ProcAddrFn g_Gpa = nullptr;
ToolhelpFn g_Snap = nullptr;
EnumModFn g_Enum = nullptr;
ModHandleWFn g_Mhw = nullptr;
ModHandleAFn g_Mha = nullptr;
ModHandleExWFn g_Mhx = nullptr;
OpenProcFn g_Open = nullptr;
LookupFn g_Lookup = nullptr;
Tick64Fn g_Tick = nullptr;
QpcFn g_Qpc = nullptr;
WsaSendToFn g_WsaSendTo = nullptr;
SendToFn g_SendTo = nullptr;
NtQsiFn g_NtQsi = nullptr;
NtQvmFn g_NtQvm = nullptr;
CrdpFn g_Crdp = nullptr;

HANDLE g_UdpLog = INVALID_HANDLE_VALUE;
LONG g_UdpCount = 0;
CRITICAL_SECTION g_UdpCs{};
LONG g_UdpInit = 0;

std::uint64_t
at(const void *P)
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(P));
}

std::uint64_t
ret()
{
    return at(_ReturnAddress());
}

// First eight characters of an ANSI name packed little-endian so the JSON line
// stays readable without carrying a pointer into the client.
std::uint64_t
pack(const char *N)
{
    std::uint64_t v = 0;

    if (!N) {
        return 0;
    }
    if (reinterpret_cast<std::uintptr_t>(N) <= 0xFFFFu) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(N));
    }
    for (int i = 0; i < 8 && N[i]; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(N[i])) << (i * 8);
    }
    return v;
}

bool
selfName(const char *N)
{
    return reinterpret_cast<std::uintptr_t>(N) > 0xFFFFu && (_stricmp(N, "module.dll") == 0);
}

bool
selfNameW(const wchar_t *N)
{
    const wchar_t *base;

    if (reinterpret_cast<std::uintptr_t>(N) <= 0xFFFFu) {
        return false;
    }
    base = wcsrchr(N, L'\\');
    base = base ? base + 1 : N;
    return _wcsicmp(base, L"module.dll") == 0;
}

bool
inRange(std::uintptr_t Addr, std::uintptr_t Base, std::size_t Size)
{
    return Base && Size && Addr >= Base && Addr - Base < Size;
}

bool
isHiddenAddr(std::uintptr_t Addr)
{
    return inRange(Addr, g_ModBase, g_ModSize) || inRange(Addr, g_IdTrampBase, g_IdTrampSize);
}

__declspec(noinline) SIZE_T WINAPI
vq(LPCVOID Addr, PMEMORY_BASIC_INFORMATION Mbi, SIZE_T Len)
{
    const SIZE_T r = g_Vq ? g_Vq(Addr, Mbi, Len) : 0;

    if constexpr (kStealth) {
        if (r != 0 && Mbi && (isHiddenAddr(reinterpret_cast<std::uintptr_t>(Addr)) ||
                              isHiddenAddr(reinterpret_cast<std::uintptr_t>(Mbi->BaseAddress)))) {
            Mbi->AllocationBase = nullptr;
            Mbi->AllocationProtect = 0;
            Mbi->State = MEM_FREE;
            Mbi->Protect = PAGE_NOACCESS;
            Mbi->Type = 0;
            return r;
        }
    }
    det(DetSource::Hyperion, DetKind::MemQuery, DetVerdict::Observed, ret(), at(Addr),
        static_cast<std::uint64_t>(r), Mbi ? static_cast<std::uint64_t>(Mbi->Protect) : 0ull,
        static_cast<std::uint64_t>(Mbi ? Mbi->RegionSize : 0));
    return r;
}

__declspec(noinline) BOOL WINAPI
vp(LPVOID Addr, SIZE_T Len, DWORD NewProt, PDWORD OldProt)
{
    const BOOL r = g_Vp ? g_Vp(Addr, Len, NewProt, OldProt) : FALSE;

    det(DetSource::Hyperion, DetKind::ProtChange, DetVerdict::Observed, ret(), at(Addr),
        static_cast<std::uint64_t>(Len), static_cast<std::uint64_t>(NewProt),
        static_cast<std::uint64_t>(OldProt ? *OldProt : 0));
    return r;
}

__declspec(noinline) BOOL WINAPI
dbg()
{
    if constexpr (kStealth) {
        return FALSE;
    }
    const BOOL r = g_Dbg ? g_Dbg() : FALSE;

    // Hyperion polls this every few seconds. Only a positive answer means a
    // debugger is actually attached; the call itself is normal traffic.
    det(DetSource::Hyperion, DetKind::DebugCheck,
        r ? DetVerdict::Suspicious : DetVerdict::Observed, ret(),
        static_cast<std::uint64_t>(r));
    return r;
}

__declspec(noinline) FARPROC WINAPI
gpa(HMODULE Mod, LPCSTR Name)
{
    const FARPROC r = g_Gpa ? g_Gpa(Mod, Name) : nullptr;

    if (Name) {
        det(DetSource::Hyperion, DetKind::Resolve, DetVerdict::Observed, ret(), pack(Name), at(r),
            at(Mod));
    }
    if constexpr (kStealth) {
        if (r && reinterpret_cast<std::uintptr_t>(Name) > 0xFFFFu) {
            FARPROC wrapped = r;
            if (probeHook(Name, r, &wrapped)) {
                return wrapped;
            }
        }
    }
    return r;
}

__declspec(noinline) HANDLE WINAPI
snap(DWORD Flags, DWORD Pid)
{
    const HANDLE r = g_Snap ? g_Snap(Flags, Pid) : INVALID_HANDLE_VALUE;
    const DetKind k = (Flags & TH32CS_SNAPTHREAD) ? DetKind::ThreadEnum : DetKind::ModuleEnum;

    det(DetSource::Hyperion, k, DetVerdict::Observed, ret(), static_cast<std::uint64_t>(Flags),
        static_cast<std::uint64_t>(Pid), at(r));
    return r;
}

__declspec(noinline) BOOL WINAPI
enumMods(HANDLE Proc, HMODULE *Mods, DWORD Cb, LPDWORD Needed)
{
    const BOOL r = g_Enum ? g_Enum(Proc, Mods, Cb, Needed) : FALSE;

    if constexpr (kStealth) {
        if (r && g_ModBase && Needed && *Needed >= sizeof(HMODULE)) {
            const auto hidden = reinterpret_cast<HMODULE>(g_ModBase);
            if (Mods && Cb >= sizeof(HMODULE)) {
                const DWORD cap = std::min<DWORD>(Cb, *Needed) / sizeof(HMODULE);
                DWORD out = 0;
                for (DWORD i = 0; i < cap; ++i) {
                    if (Mods[i] != hidden) {
                        Mods[out++] = Mods[i];
                    }
                }
                if (out < cap) {
                    *Needed -= (cap - out) * static_cast<DWORD>(sizeof(HMODULE));
                }
            }
        }
    }
    det(DetSource::Hyperion, DetKind::ModuleEnum, DetVerdict::Observed, ret(), at(Proc),
        static_cast<std::uint64_t>(Needed ? *Needed : 0), static_cast<std::uint64_t>(Cb),
        static_cast<std::uint64_t>(r));
    return r;
}

__declspec(noinline) HMODULE WINAPI
mhw(LPCWSTR Name)
{
    const bool self = selfNameW(Name);

    if constexpr (kStealth) {
        if (self) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
    }
    const HMODULE r = g_Mhw ? g_Mhw(Name) : nullptr;

    det(DetSource::Hyperion, self ? DetKind::SelfSeen : DetKind::ModuleEnum,
        self ? DetVerdict::Confirmed : DetVerdict::Observed, ret(), at(Name), at(r));
    return r;
}

__declspec(noinline) HMODULE WINAPI
mha(LPCSTR Name)
{
    const bool self = selfName(Name);

    if constexpr (kStealth) {
        if (self) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
    }
    const HMODULE r = g_Mha ? g_Mha(Name) : nullptr;

    det(DetSource::Hyperion, self ? DetKind::SelfSeen : DetKind::ModuleEnum,
        self ? DetVerdict::Confirmed : DetVerdict::Observed, ret(), pack(Name ? Name : ""), at(r));
    return r;
}

__declspec(noinline) BOOL WINAPI
mhx(DWORD Flags, LPCWSTR Name, HMODULE *Out)
{
    const bool self = selfNameW(Name);

    if constexpr (kStealth) {
        if (self && !(Flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) {
            if (Out) {
                *Out = nullptr;
            }
            SetLastError(ERROR_MOD_NOT_FOUND);
            return FALSE;
        }
    }
    const BOOL r = g_Mhx ? g_Mhx(Flags, Name, Out) : FALSE;

    det(DetSource::Hyperion, self ? DetKind::SelfSeen : DetKind::ModuleEnum,
        self ? DetVerdict::Confirmed : DetVerdict::Observed, ret(), static_cast<std::uint64_t>(Flags),
        at(Name), static_cast<std::uint64_t>(r));
    return r;
}

__declspec(noinline) HANDLE WINAPI
open(DWORD Access, BOOL Inherit, DWORD Pid)
{
    const HANDLE r = g_Open ? g_Open(Access, Inherit, Pid) : nullptr;

    // Hyperion routinely opens its own process to scan memory; the call is
    // normal traffic. Real risk shows up as SelfSeen when a module census
    // reaches the loader, not here.
    det(DetSource::Hyperion, DetKind::ProcOpen, DetVerdict::Observed, ret(),
        static_cast<std::uint64_t>(Pid), static_cast<std::uint64_t>(Access), at(r));
    return r;
}

__declspec(noinline) PRUNTIME_FUNCTION NTAPI
lookup(DWORD64 Pc, PDWORD64 ImageBase, PUNWIND_HISTORY_TABLE History)
{
    if constexpr (kStealth) {
        if (isHiddenAddr(static_cast<std::uintptr_t>(Pc))) {
            return nullptr;
        }
    }
    const PRUNTIME_FUNCTION r = g_Lookup ? g_Lookup(Pc, ImageBase, History) : nullptr;

    det(DetSource::Hyperion, DetKind::PdataCheck, DetVerdict::Observed, ret(),
        static_cast<std::uint64_t>(Pc), static_cast<std::uint64_t>(ImageBase ? *ImageBase : 0),
        at(r));
    return r;
}

__declspec(noinline) ULONGLONG WINAPI
tick()
{
    const ULONGLONG r = g_Tick ? g_Tick() : 0;

    det(DetSource::Hyperion, DetKind::TimingProbe, DetVerdict::Observed, ret(),
        static_cast<std::uint64_t>(r));
    return r;
}

__declspec(noinline) BOOL WINAPI
qpc(LARGE_INTEGER *Out)
{
    const BOOL r = g_Qpc ? g_Qpc(Out) : FALSE;

    det(DetSource::Hyperion, DetKind::TimingProbe, DetVerdict::Observed, ret(),
        static_cast<std::uint64_t>(r),
        static_cast<std::uint64_t>(Out ? Out->QuadPart : 0));
    return r;
}

void
udpLog(const char *Tag, std::uint64_t Caller, const unsigned char *Buf, ULONG Len, const sockaddr *To)
{
    if constexpr (!kDebugLog) {
        (void)Tag;
        (void)Caller;
        (void)Buf;
        (void)Len;
        (void)To;
        return;
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    char line[512];
    ULONG cap;
    std::uint16_t port = 0;
    std::uint64_t rva = Caller >= reinterpret_cast<std::uint64_t>(g_Base)
                            ? Caller - reinterpret_cast<std::uint64_t>(g_Base)
                            : Caller;
    int pos;
    DWORD w = 0;

    if (!Buf || Len < 12) {
        return;
    }
    if (To && To->sa_family == AF_INET) {
        const auto *sa = reinterpret_cast<const sockaddr_in *>(To);
        if (sa->sin_addr.s_addr == 0x0100007Fu) {
            return;
        }
        port = static_cast<std::uint16_t>((sa->sin_port >> 8) | (sa->sin_port << 8));
    } else if (To && To->sa_family == AF_INET6) {
        const auto *sa6 = reinterpret_cast<const sockaddr_in6 *>(To);
        if (IN6_IS_ADDR_LOOPBACK(&sa6->sin6_addr)) {
            return;
        }
        port = static_cast<std::uint16_t>((sa6->sin6_port >> 8) | (sa6->sin6_port << 8));
    }
    if (InterlockedIncrement(&g_UdpCount) > 2500) {
        return;
    }
    if (InterlockedCompareExchange(&g_UdpInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_UdpCs);
        InterlockedExchange(&g_UdpInit, 2);
    }
    while (InterlockedCompareExchange(&g_UdpInit, 2, 2) != 2) {
        Sleep(0);
    }
    pos = std::snprintf(line, sizeof(line), "[%s] t=%llu port=%u rva=0x%llX len=%lu hex=", Tag,
                        static_cast<unsigned long long>(GetTickCount64()), static_cast<unsigned>(port),
                        static_cast<unsigned long long>(rva), static_cast<unsigned long>(Len));
    if (pos <= 0 || static_cast<std::size_t>(pos) >= sizeof(line) - 4) {
        return;
    }
    cap = Len > 96u ? 96u : Len;
    for (ULONG i = 0; i < cap && static_cast<std::size_t>(pos) + 4 < sizeof(line); ++i) {
        line[pos++] = kHex[Buf[i] >> 4];
        line[pos++] = kHex[Buf[i] & 0x0f];
    }
    line[pos++] = '\r';
    line[pos++] = '\n';
    EnterCriticalSection(&g_UdpCs);
    if (g_UdpLog == INVALID_HANDLE_VALUE) {
        const auto p = path(g_Dir, L"udp_debug.log");
        g_UdpLog = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_UdpLog != INVALID_HANDLE_VALUE) {
        WriteFile(g_UdpLog, line, static_cast<DWORD>(pos), &w, nullptr);
    }
    LeaveCriticalSection(&g_UdpCs);
}

__declspec(noinline) int WSAAPI
wsaSendTo(SOCKET S, LPWSABUF Bufs, DWORD Count, LPDWORD Sent, DWORD Flags, const sockaddr *To,
          int ToLen, LPWSAOVERLAPPED Ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE Comp)
{
    if (Bufs && Count > 0 && Bufs[0].buf && Bufs[0].len > 0) {
        udpLog("WSASendTo", ret(), reinterpret_cast<const unsigned char *>(Bufs[0].buf), Bufs[0].len, To);
    }
    return g_WsaSendTo ? g_WsaSendTo(S, Bufs, Count, Sent, Flags, To, ToLen, Ov, Comp) : SOCKET_ERROR;
}

__declspec(noinline) int WSAAPI
sendToWrap(SOCKET S, const char *Buf, int Len, int Flags, const sockaddr *To, int ToLen)
{
    if (Buf && Len > 0) {
        udpLog("sendto", ret(), reinterpret_cast<const unsigned char *>(Buf), static_cast<ULONG>(Len), To);
    }
    return g_SendTo ? g_SendTo(S, Buf, Len, Flags, To, ToLen) : SOCKET_ERROR;
}

__declspec(noinline) NTSTATUS NTAPI
ntQsi(ULONG InfoClass, PVOID Buf, ULONG Len, PULONG RetLen)
{
    const NTSTATUS st = g_NtQsi ? g_NtQsi(InfoClass, Buf, Len, RetLen) : static_cast<NTSTATUS>(0xC0000001L);

    if constexpr (kStealth) {
        if (st >= 0 && InfoClass == 11u && Buf && Len >= sizeof(RtlProcMods) && g_ModBase) {
            auto *mods = static_cast<RtlProcMods *>(Buf);
            const ULONG maxFit = (Len - static_cast<ULONG>(offsetof(RtlProcMods, Modules))) /
                                 static_cast<ULONG>(sizeof(RtlProcModInfo));
            const ULONG count = std::min<ULONG>(mods->NumberOfModules, maxFit);
            ULONG out = 0;
            for (ULONG i = 0; i < count; ++i) {
                if (reinterpret_cast<std::uintptr_t>(mods->Modules[i].ImageBase) == g_ModBase) {
                    continue;
                }
                if (out != i) {
                    mods->Modules[out] = mods->Modules[i];
                }
                ++out;
            }
            mods->NumberOfModules = out;
        }
    }
    return st;
}

__declspec(noinline) NTSTATUS NTAPI
ntQvm(HANDLE Proc, PVOID BaseAddr, ULONG InfoClass, PVOID Buf, SIZE_T Len, PSIZE_T RetLen)
{
    if constexpr (kStealth) {
        const bool selfProc = (Proc == GetCurrentProcess() || Proc == reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1)));
        if (selfProc && InfoClass == 2u && inRange(reinterpret_cast<std::uintptr_t>(BaseAddr), g_ModBase, g_ModSize)) {
            return static_cast<NTSTATUS>(0xC0000141L); // STATUS_INVALID_ADDRESS
        }
    }
    const NTSTATUS st = g_NtQvm ? g_NtQvm(Proc, BaseAddr, InfoClass, Buf, Len, RetLen) : static_cast<NTSTATUS>(0xC0000001L);
    if constexpr (kStealth) {
        if (st >= 0 && InfoClass == 0u && Buf && Len >= sizeof(MEMORY_BASIC_INFORMATION)) {
            auto *mbi = static_cast<PMEMORY_BASIC_INFORMATION>(Buf);
            if (isHiddenAddr(reinterpret_cast<std::uintptr_t>(BaseAddr)) ||
                isHiddenAddr(reinterpret_cast<std::uintptr_t>(mbi->BaseAddress))) {
                mbi->AllocationBase = nullptr;
                mbi->AllocationProtect = 0;
                mbi->State = MEM_FREE;
                mbi->Protect = PAGE_NOACCESS;
                mbi->Type = 0;
            }
        }
    }
    return st;
}

__declspec(noinline) BOOL WINAPI
crdp(HANDLE Proc, PBOOL Present)
{
    if constexpr (kStealth) {
        (void)Proc;
        if (Present) {
            *Present = FALSE;
        }
        return TRUE;
    } else {
        return g_Crdp ? g_Crdp(Proc, Present) : FALSE;
    }
}

}

bool
probeHook(const char *Fn, FARPROC Real, FARPROC *Out)
{
    struct Entry {
        const char *Name;
        FARPROC *Slot;
        void *Wrap;
    };
    static const Entry kTable[] = {
        {"VirtualQuery", reinterpret_cast<FARPROC *>(&g_Vq), reinterpret_cast<void *>(&vq)},
        {"VirtualProtect", reinterpret_cast<FARPROC *>(&g_Vp), reinterpret_cast<void *>(&vp)},
        {"IsDebuggerPresent", reinterpret_cast<FARPROC *>(&g_Dbg), reinterpret_cast<void *>(&dbg)},
        {"GetProcAddress", reinterpret_cast<FARPROC *>(&g_Gpa), reinterpret_cast<void *>(&gpa)},
        {"CreateToolhelp32Snapshot", reinterpret_cast<FARPROC *>(&g_Snap), reinterpret_cast<void *>(&snap)},
        {"K32EnumProcessModules", reinterpret_cast<FARPROC *>(&g_Enum), reinterpret_cast<void *>(&enumMods)},
        {"GetModuleHandleW", reinterpret_cast<FARPROC *>(&g_Mhw), reinterpret_cast<void *>(&mhw)},
        {"GetModuleHandleA", reinterpret_cast<FARPROC *>(&g_Mha), reinterpret_cast<void *>(&mha)},
        {"GetModuleHandleExW", reinterpret_cast<FARPROC *>(&g_Mhx), reinterpret_cast<void *>(&mhx)},
        {"OpenProcess", reinterpret_cast<FARPROC *>(&g_Open), reinterpret_cast<void *>(&open)},
        {"RtlLookupFunctionEntry", reinterpret_cast<FARPROC *>(&g_Lookup), reinterpret_cast<void *>(&lookup)},
        {"GetTickCount64", reinterpret_cast<FARPROC *>(&g_Tick), reinterpret_cast<void *>(&tick)},
        {"QueryPerformanceCounter", reinterpret_cast<FARPROC *>(&g_Qpc), reinterpret_cast<void *>(&qpc)},
        {"WSASendTo", reinterpret_cast<FARPROC *>(&g_WsaSendTo), reinterpret_cast<void *>(&wsaSendTo)},
        {"#20", reinterpret_cast<FARPROC *>(&g_SendTo), reinterpret_cast<void *>(&sendToWrap)},
        {"NtQuerySystemInformation", reinterpret_cast<FARPROC *>(&g_NtQsi), reinterpret_cast<void *>(&ntQsi)},
        {"NtQueryVirtualMemory", reinterpret_cast<FARPROC *>(&g_NtQvm), reinterpret_cast<void *>(&ntQvm)},
        {"CheckRemoteDebuggerPresent", reinterpret_cast<FARPROC *>(&g_Crdp), reinterpret_cast<void *>(&crdp)},
    };

    if (!Fn || !Out) {
        return false;
    }
    if constexpr (kRandHwid) {
        if (std::strcmp(Fn, "RegQueryValueExA") == 0) {
            setRegHooks(Real, nullptr, nullptr);
            *Out = reinterpret_cast<FARPROC>(&regQExA);
            return true;
        }
        if (std::strcmp(Fn, "RegQueryValueExW") == 0) {
            setRegHooks(nullptr, Real, nullptr);
            *Out = reinterpret_cast<FARPROC>(&regQExW);
            return true;
        }
        if (std::strcmp(Fn, "RegGetValueW") == 0) {
            setRegHooks(nullptr, nullptr, Real);
            *Out = reinterpret_cast<FARPROC>(&regGetW);
            return true;
        }
    }
    for (const Entry &e : kTable) {
        if (std::strcmp(Fn, e.Name) != 0) {
            continue;
        }
        if (!*e.Slot) {
            *e.Slot = Real;
        }
        *Out = reinterpret_cast<FARPROC>(e.Wrap);
        return true;
    }
    return false;
}

}
