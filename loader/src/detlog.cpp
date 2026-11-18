#include "emulator/common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace emu {
namespace {

constexpr std::uint32_t kDecayMs = 30000u;
constexpr std::uint64_t kRepeatMs = 1000u;
constexpr std::size_t kSeenSlots = 128u;

struct SeenSlot {
    std::uint64_t Caller;
    std::uint64_t Key;
    std::uint64_t Ms;
    std::uint16_t Kind;
    std::uint8_t Source;
};

DetHeader *g_Hdr = nullptr;
DetEvent *g_Ring = nullptr;
HANDLE g_Map = nullptr;
HANDLE g_Log = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_Cs{};
bool g_CsOk = false;
std::uint32_t g_Heat = 0;
std::uint64_t g_LastMs = 0;
SeenSlot g_Seen[kSeenSlots]{};

std::uint32_t
weight(DetKind Kind, DetVerdict Verdict)
{
    std::uint32_t base = 1;

    switch (Verdict) {
    case DetVerdict::Suspicious:
        base = 4;
        break;
    case DetVerdict::Confirmed:
        base = 20;
        break;
    case DetVerdict::Observed:
    default:
        // Normal client traffic (timing probes fire thousands of times per
        // minute) stays in the ring log but must not move the heat gauge:
        // heat is the detection-risk budget, not an activity counter.
        return 0;
    }
    switch (Kind) {
    case DetKind::HandleQuery:
    case DetKind::ModuleEnum:
    case DetKind::ProcOpen:
    case DetKind::ThreadEnum:
        return base * 3u;
    case DetKind::DebugCheck:
        return base * 4u;
    case DetKind::ServiceUnknown:
    case DetKind::CallbackMiss:
    case DetKind::TimingImplausible:
        return base * 6u;
    case DetKind::PatchFailed:
    case DetKind::PatchReverted:
    case DetKind::SelfSeen:
    case DetKind::ChallengeFail:
        return base * 10u;
    default:
        return base;
    }
}

std::uint64_t
seenKey(DetKind Kind, std::uint64_t Caller, std::uint64_t A0)
{
    // Resolution probes collapse by caller alone, which would hide the census of
    // names a single site asks for. Every other kind collapses by caller so a
    // hot memory scan does not flood the ring.
    return Kind == DetKind::Resolve ? (Caller ^ (A0 * 0x9e3779b97f4a7c15ull)) : Caller;
}

std::size_t
seenIndex(DetSource Source, DetKind Kind, std::uint64_t Key)
{
    std::uint64_t h = Key ^ (static_cast<std::uint64_t>(Kind) << 48) ^
                      (static_cast<std::uint64_t>(Source) << 56);

    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<std::size_t>(h) % kSeenSlots;
}

bool
repeat(DetSource Source, DetKind Kind, DetVerdict Verdict, std::uint64_t Caller, std::uint64_t A0,
       std::uint64_t Ms)
{
    const std::uint64_t key = seenKey(Kind, Caller, A0);
    SeenSlot &s = g_Seen[seenIndex(Source, Kind, key)];

    if (Verdict != DetVerdict::Observed) {
        return false;
    }
    if (s.Key == key && s.Kind == static_cast<std::uint16_t>(Kind) &&
        s.Source == static_cast<std::uint8_t>(Source) && Ms - s.Ms < kRepeatMs) {
        return true;
    }
    s.Caller = Caller;
    s.Key = key;
    s.Ms = Ms;
    s.Kind = static_cast<std::uint16_t>(Kind);
    s.Source = static_cast<std::uint8_t>(Source);
    return false;
}

void
emit(const DetEvent &E)
{
    char line[352];
    std::uint64_t rva = 0;
    int n;

    if (E.Caller >= reinterpret_cast<std::uint64_t>(g_Base)) {
        rva = E.Caller - reinterpret_cast<std::uint64_t>(g_Base);
    }
    n = std::snprintf(
        line, sizeof(line),
        "{\"t\":%llu,\"seq\":%lu,\"src\":\"%s\",\"kind\":\"%s\",\"verdict\":\"%s\","
        "\"caller\":\"0x%llX\",\"rva\":\"0x%llX\",\"tid\":%lu,"
        "\"a0\":\"0x%llX\",\"a1\":\"0x%llX\",\"a2\":\"0x%llX\",\"a3\":\"0x%llX\",\"heat\":%lu}\r\n",
        static_cast<unsigned long long>(E.Tick), static_cast<unsigned long>(E.Seq),
        detSourceName(static_cast<DetSource>(E.Source)), detKindName(static_cast<DetKind>(E.Kind)),
        detVerdictName(static_cast<DetVerdict>(E.Verdict)), static_cast<unsigned long long>(E.Caller),
        static_cast<unsigned long long>(rva), static_cast<unsigned long>(E.Tid),
        static_cast<unsigned long long>(E.A0), static_cast<unsigned long long>(E.A1),
        static_cast<unsigned long long>(E.A2), static_cast<unsigned long long>(E.A3),
        static_cast<unsigned long>(g_Heat));
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(line) && g_Log != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(g_Log, line, static_cast<DWORD>(n), &w, nullptr);
    }
}

}

bool
detInit()
{
    std::wstring shm;
    std::wstring file;

    if constexpr (!kDebugLog) {
        return true;
    }

    if (!g_CsOk) {
        InitializeCriticalSection(&g_Cs);
        g_CsOk = true;
    }
    shm = L"Local\\RobloxDetLog_" + std::to_wstring(GetCurrentProcessId());
    g_Map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kDetSectionSize, shm.c_str());
    if (g_Map) {
        g_Hdr = static_cast<DetHeader *>(MapViewOfFile(g_Map, FILE_MAP_ALL_ACCESS, 0, 0, kDetSectionSize));
    }
    if (g_Hdr) {
        std::memset(g_Hdr, 0, sizeof(DetHeader));
        g_Hdr->Magic = kDetMagic;
        g_Hdr->Version = kDetVersion;
        g_Hdr->Capacity = kDetCapacity;
        g_Hdr->Base = reinterpret_cast<std::uint64_t>(g_Base);
        g_Hdr->Start = GetTickCount64();
        g_Hdr->Pid = GetCurrentProcessId();
        g_Ring = reinterpret_cast<DetEvent *>(g_Hdr + 1);
    }
    bool file_log = false;
    {
        wchar_t opt[8]{};
        if (GetEnvironmentVariableW(L"ZEND_DETLOG_FILE", opt, 8) != 0 && opt[0] == L'1') {
            file_log = true;
        }
    }
    if (file_log && !g_Dir.empty()) {
        file = g_Dir + L"\\detlog.jsonl";
        g_Log = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file_log && g_Log == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tmp)) {
            file.assign(tmp);
            file += L"detlog.jsonl";
            g_Log = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (!file.empty()) {
        char b[512];
        std::snprintf(b, sizeof(b), "detlog=%ls heat=0 ring=%u", file.c_str(), kDetCapacity);
        trace(b);
    } else {
        trace("detlog=section-only heat=0");
    }
    return g_Hdr != nullptr;
}

void
detClose()
{
    if (g_Ring) {
        detSnapshot();
    }
    if (g_Hdr) {
        UnmapViewOfFile(g_Hdr);
        g_Hdr = nullptr;
        g_Ring = nullptr;
    }
    if (g_Map) {
        CloseHandle(g_Map);
        g_Map = nullptr;
    }
    if (g_Log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_Log);
        g_Log = INVALID_HANDLE_VALUE;
    }
    if (g_CsOk) {
        DeleteCriticalSection(&g_Cs);
        g_CsOk = false;
    }
}

void
det(DetSource Source, DetKind Kind, DetVerdict Verdict, std::uint64_t Caller,
    std::uint64_t A0, std::uint64_t A1, std::uint64_t A2, std::uint64_t A3)
{
    LARGE_INTEGER now{};
    std::uint64_t ms;

    if constexpr (!kDebugLog) {
        (void)Source;
        (void)Kind;
        (void)Verdict;
        (void)Caller;
        (void)A0;
        (void)A1;
        (void)A2;
        (void)A3;
        return;
    }

    QueryPerformanceCounter(&now);
    ms = GetTickCount64();
    if (g_CsOk) {
        EnterCriticalSection(&g_Cs);
    }
    if (g_LastMs && ms > g_LastMs) {
        const std::uint64_t dt = ms - g_LastMs;
        const std::uint32_t cut = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(100ull, (static_cast<std::uint64_t>(g_Heat) * dt) / kDecayMs));

        g_Heat = g_Heat > cut ? g_Heat - cut : 0;
    }
    g_LastMs = ms;
    {
        const std::uint32_t w = weight(Kind, Verdict);

        g_Heat = (g_Heat + w > 100u) ? 100u : g_Heat + w;
    }
    if (g_Hdr && g_Ring) {
        g_Hdr->Heat = g_Heat;
        if (repeat(Source, Kind, Verdict, Caller, A0, ms)) {
            ++g_Hdr->Drop;
        } else {
            const LONG i = InterlockedIncrement(reinterpret_cast<volatile LONG *>(&g_Hdr->Head)) - 1;
            DetEvent &e = g_Ring[static_cast<std::uint32_t>(i) % kDetCapacity];

            e.Tick = static_cast<std::uint64_t>(now.QuadPart);
            e.Caller = Caller;
            e.A0 = A0;
            e.A1 = A1;
            e.A2 = A2;
            e.A3 = A3;
            e.Tid = GetCurrentThreadId();
            e.Kind = static_cast<std::uint16_t>(Kind);
            e.Source = static_cast<std::uint8_t>(Source);
            e.Verdict = static_cast<std::uint8_t>(Verdict);
            e.Seq = static_cast<std::uint32_t>(i);
            e.Pad = 0;
            emit(e);
        }
    }
    if (g_CsOk) {
        LeaveCriticalSection(&g_Cs);
    }
}

void
detSnapshot()
{
    char line[224];
    int n;

    if (g_Log == INVALID_HANDLE_VALUE) {
        return;
    }
    n = std::snprintf(line, sizeof(line),
                      "{\"snapshot\":1,\"t\":%llu,\"heat\":%lu,\"events\":%lu,\"drop\":%lu,"
                      "\"base\":\"0x%llX\",\"pid\":%lu}\r\n",
                      static_cast<unsigned long long>(GetTickCount64()),
                      static_cast<unsigned long>(g_Heat),
                      static_cast<unsigned long>(g_Hdr ? g_Hdr->Head : 0u),
                      static_cast<unsigned long>(g_Hdr ? g_Hdr->Drop : 0u),
                      static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(g_Base)),
                      static_cast<unsigned long>(GetCurrentProcessId()));
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(line)) {
        DWORD w = 0;
        WriteFile(g_Log, line, static_cast<DWORD>(n), &w, nullptr);
    }
}

unsigned
detHeat()
{
    return g_Heat;
}

const char *
detKindName(DetKind Kind)
{
    switch (Kind) {
    case DetKind::Resolve:
        return "Resolve";
    case DetKind::MemQuery:
        return "MemQuery";
    case DetKind::ProtChange:
        return "ProtChange";
    case DetKind::HandleQuery:
        return "HandleQuery";
    case DetKind::ModuleEnum:
        return "ModuleEnum";
    case DetKind::ProcOpen:
        return "ProcOpen";
    case DetKind::ThreadEnum:
        return "ThreadEnum";
    case DetKind::DebugCheck:
        return "DebugCheck";
    case DetKind::TimingProbe:
        return "TimingProbe";
    case DetKind::HwidRead:
        return "HwidRead";
    case DetKind::PdataCheck:
        return "PdataCheck";
    case DetKind::ServiceStub:
        return "ServiceStub";
    case DetKind::ServiceUnknown:
        return "ServiceUnknown";
    case DetKind::PatchApplied:
        return "PatchApplied";
    case DetKind::PatchFailed:
        return "PatchFailed";
    case DetKind::PatchReverted:
        return "PatchReverted";
    case DetKind::SelfSeen:
        return "SelfSeen";
    case DetKind::ChallengeFail:
        return "ChallengeFail";
    case DetKind::CallbackMiss:
        return "CallbackMiss";
    case DetKind::TimingImplausible:
        return "TimingImplausible";
    case DetKind::DebugInfo:
        return "DebugInfo";
    case DetKind::StackWalk:
        return "StackWalk";
    case DetKind::EnvAccess:
        return "EnvAccess";
    case DetKind::MetaAccess:
        return "MetaAccess";
    case DetKind::IdentityCheck:
        return "IdentityCheck";
    case DetKind::ErrorLeak:
        return "ErrorLeak";
    case DetKind::None:
    default:
        return "None";
    }
}

const char *
detSourceName(DetSource Source)
{
    switch (Source) {
    case DetSource::Hyperion:
        return "Hyperion";
    case DetSource::Luau:
        return "Luau";
    case DetSource::Emulator:
        return "Emulator";
    case DetSource::Unknown:
    default:
        return "Unknown";
    }
}

const char *
detVerdictName(DetVerdict Verdict)
{
    switch (Verdict) {
    case DetVerdict::Suspicious:
        return "Suspicious";
    case DetVerdict::Confirmed:
        return "Confirmed";
    case DetVerdict::Observed:
    default:
        return "Observed";
    }
}

}
