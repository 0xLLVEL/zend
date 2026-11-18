#include "emulator/common.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <vector>

namespace emu {
namespace {

using TlsFn = void(NTAPI *)(void *, DWORD, void *);
using EpFn = DWORD(WINAPI *)();

bool
epA(const void *Fn)
{
    unsigned char jmp[12]{};

    jmpAbs(jmp, Fn);
    return wr(g_Base + off::kEpOrig, jmp, sizeof(jmp), PAGE_EXECUTE_READ) && cfg(g_Base + off::kEpOrig);
}

bool
stack(void *Old, void *Neu)
{
    const auto a = reinterpret_cast<std::uintptr_t>(Old);
    const auto b = reinterpret_cast<std::uintptr_t>(Neu);
    auto cur = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
    const auto top = __readgsqword(0x08);
    unsigned int hits = 0;

    cur = (cur + sizeof(std::uintptr_t) - 1u) & ~(sizeof(std::uintptr_t) - 1u);
    if (!a || !b || !top || cur >= top || top - cur > 0x1000000u) {
        return false;
    }
    for (auto *p = reinterpret_cast<std::uintptr_t *>(cur); p < reinterpret_cast<std::uintptr_t *>(top); ++p) {
        if (*p != a) {
            continue;
        }
        *p = b;
        ++hits;
    }
    _mm_mfence();
    return hits != 0;
}

void
unlinkEntry(LIST_ENTRY *E)
{
    if (!E || !E->Flink || !E->Blink) {
        return;
    }
    E->Blink->Flink = E->Flink;
    E->Flink->Blink = E->Blink;
    E->Flink = E;
    E->Blink = E;
}

void
scrubPeb()
{
    auto *peb = reinterpret_cast<unsigned char *>(__readgsqword(0x60));
    if (!peb) {
        return;
    }
    peb[0x02] = 0; // BeingDebugged
    *reinterpret_cast<std::uint32_t *>(peb + 0xBC) &= ~0x70u; // NtGlobalFlag heap debug flags
}

void
hideModule(HMODULE Mod)
{
    if (!Mod) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(Mod);
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0) {
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            g_ModBase = base;
            g_ModSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    auto *peb = reinterpret_cast<unsigned char *>(__readgsqword(0x60));
    if (!peb) {
        return;
    }
    auto *ldr = *reinterpret_cast<unsigned char **>(peb + 0x18);
    if (!ldr) {
        return;
    }
    auto *head = reinterpret_cast<LIST_ENTRY *>(ldr + 0x10); // InLoadOrderModuleList
    for (LIST_ENTRY *cur = head->Flink; cur && cur != head; cur = cur->Flink) {
        auto *entry = reinterpret_cast<unsigned char *>(cur);
        if (*reinterpret_cast<std::uintptr_t *>(entry + 0x30) == base) {
            unlinkEntry(reinterpret_cast<LIST_ENTRY *>(entry + 0x00)); // InLoadOrderLinks
            unlinkEntry(reinterpret_cast<LIST_ENTRY *>(entry + 0x10)); // InMemoryOrderLinks
            unlinkEntry(reinterpret_cast<LIST_ENTRY *>(entry + 0x20)); // InInitializationOrderLinks
            break;
        }
    }
}

DWORD WINAPI
lod(void *)
{
    static wchar_t buf[MAX_PATH]{};
    const auto full = path(g_Dir, kModDll);
    HMODULE mod;

    if (full.empty() || full.size() >= MAX_PATH) {
        return 0;
    }
    std::memcpy(buf, full.c_str(), (full.size() + 1u) * sizeof(wchar_t));
    Sleep(5000);
    mod = LoadLibraryW(buf);
    if constexpr (kStealth) {
        hideModule(mod);
    }
    return 0;
}

}

bool
init()
{
    wchar_t dir[MAX_PATH]{};
    DWORD n;
    wchar_t* slash;
    std::vector<unsigned char> buf;
    NTSTATUS st;
    ULONG got = 0;
    std::uint64_t tok = 0;

    n = GetModuleFileNameW(g_Self, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        return false;
    }
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    g_Dir.assign(dir);
    g_Base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    QueryPerformanceCounter(&g_T0);
    detInit();
    {
        char b[192];
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(g_Base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(g_Base + dos->e_lfanew);
            std::snprintf(b, sizeof(b), "hdr soi=0x%X ep=0x%X want 0x%X/0x%X",
                          nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.AddressOfEntryPoint,
                          kImageSize, off::kEpOrig);
        } else {
            std::snprintf(b, sizeof(b), "hdr bad dos magic=0x%X", dos->e_magic);
        }
        trace(b);
    }
    if (!img()) {
        trace("FAIL img");
        return false;
    }
    trace("ok img");
    if (!seed()) {
        trace("FAIL seed");
        return false;
    }
    trace("ok seed");
    buf.reserve(kPayloadSize);
    if (!payload(buf)) {
        trace("FAIL payload");
        return false;
    }
    trace("ok payload");
    if (!restore(buf.data(), buf.size())) {
        trace("FAIL restore");
        return false;
    }
    g_Committed = true;
    trace("ok restore");
    if (!sec()) {
        trace("FAIL sec");
        return false;
    }
    trace("ok sec");
    if (!imp()) {
        trace("FAIL imp");
        return false;
    }
    trace("ok imp");
    if (!proxy()) {
        trace("FAIL proxy");
        return false;
    }
    if (!png()) {
        trace("FAIL png");
        return false;
    }
    if (!lua()) {
        trace("FAIL lua");
        return false;
    }
    trace("ok lua");
    QueryPerformanceCounter(&g_T1);
    st = BCryptOpenAlgorithmProvider(&g_Hash, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st >= 0) {
        st = BCryptGetProperty(g_Hash, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&g_HashObj), sizeof(g_HashObj), &got, 0);
    }
    if (st < 0 || got != sizeof(g_HashObj) || g_HashObj == 0 || g_HashObj > 1024) {
        trace("FAIL bcrypt");
        return false;
    }
    if (!token(tok)) {
        trace("FAIL token");
        return false;
    }
    /* STALE for 5a21b87bbc2641cf: kMux old RVA lands in shifted .data — writing here
       corrupts an unknown client global. Re-derive holder before re-enabling.
    *reinterpret_cast<volatile std::uint64_t*>(g_Base + off::kMux) = tok;
    */
    (void)tok;
    if (!RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(g_Base + off::kRuntime), off::kRtCount, reinterpret_cast<DWORD64>(g_Base))) {
        return false;
    }
    if (!epA(reinterpret_cast<const void*>(&entry))) {
        return false;
    }
    detSnapshot();
    return true;
}

bool
prep()
{
    static LONG once = 0;
    HANDLE th;

    if (InterlockedCompareExchange(&once, 1, 0) != 0) {
        return true;
    }
    if constexpr (kStealth) {
        scrubPeb();
    }
    for (const std::uint32_t rva : off::kTls) {
        if (!rva) {
            continue;
        }
        reinterpret_cast<TlsFn>(g_Base + rva)(g_Base, DLL_THREAD_ATTACH, nullptr);
    }
    if (!png() || !lua() || !hookIN()) {
        InterlockedExchange(&once, 0);
        return false;
    }
    g_Veh = AddVectoredExceptionHandler(1, veh);
    if (!g_Veh) {
        hookOUT();
        InterlockedExchange(&once, 0);
        return false;
    }
    if (kLoadMod) {
        th = CreateThread(nullptr, 0, lod, nullptr, 0, nullptr);
        if (th) {
            CloseHandle(th);
        }
    }
    return true;
}

bool
ep()
{
    auto* old = g_Base + off::kEpOrig;
    auto* site = old - 4;

    if (!jmpRel(site, g_Base + off::kEpNew, PAGE_EXECUTE_READ) || !cfg(site)) {
        return false;
    }

    stack(old, site);
    return true;
}

extern "C" DWORD WINAPI
entry()
{
    EpFn client;
    DWORD code = 0;

    if (!prep()) {
        return ERROR_DLL_INIT_FAILED;
    }
    if (!ep()) {
        return ERROR_DLL_INIT_FAILED;
    }

    client = reinterpret_cast<EpFn>(g_Base + off::kEpNew);
    __try {
        code = client();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return code;
}

}
