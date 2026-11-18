#pragma once

#include <cstdint>

namespace emu {

enum class DetSource : std::uint8_t {
    Unknown = 0,
    Hyperion = 1,
    Luau = 2,
    Emulator = 3,
};

enum class DetKind : std::uint16_t {
    None = 0,
    Resolve = 1,        // GetProcAddress on a probe name
    MemQuery = 2,       // VirtualQuery / NtQueryVirtualMemory
    ProtChange = 3,     // VirtualProtect / NtProtectVirtualMemory
    HandleQuery = 4,    // NtQuerySystemInformation handle table walk
    ModuleEnum = 5,     // module enumeration / snapshot
    ProcOpen = 6,       // OpenProcess on self
    ThreadEnum = 7,     // thread enumeration
    DebugCheck = 8,     // IsDebuggerPresent and friends
    TimingProbe = 9,    // QPC / tick counter reads
    HwidRead = 10,      // firmware table, adapters, volume, machine guid
    PdataCheck = 11,    // RtlLookupFunctionEntry / unwind table
    ServiceStub = 12,   // one of the 18 VEH service rules fired
    ServiceUnknown = 13,// an unhandled negative-address fault
    PatchApplied = 14,
    PatchFailed = 15,
    PatchReverted = 16,
    SelfSeen = 17,      // something asked for our module by name
    ChallengeFail = 18,
    CallbackMiss = 19,
    TimingImplausible = 20,
    DebugInfo = 21,     // Luau debug.info
    StackWalk = 22,     // Luau debug.traceback
    EnvAccess = 23,     // getfenv / setfenv / getgenv
    MetaAccess = 24,    // getmetatable / setmetatable on protected tables
    IdentityCheck = 25, // getidentity / setidentity
    ErrorLeak = 26,     // a pcall error surfaced a foreign frame
};

enum class DetVerdict : std::uint8_t {
    Observed = 0,
    Suspicious = 1,
    Confirmed = 2,
};

struct DetEvent {
    std::uint64_t Tick;
    std::uint64_t Caller;
    std::uint64_t A0;
    std::uint64_t A1;
    std::uint64_t A2;
    std::uint64_t A3;
    std::uint32_t Tid;
    std::uint16_t Kind;
    std::uint8_t Source;
    std::uint8_t Verdict;
    std::uint32_t Seq;
    std::uint32_t Pad;
};

static_assert(sizeof(DetEvent) == 64, "DetEvent layout is shared with the external reader");

constexpr std::uint32_t kDetMagic = 0x31544544u; // 'DET1'
constexpr std::uint32_t kDetVersion = 1u;
constexpr std::uint32_t kDetCapacity = 4096u;

struct DetHeader {
    std::uint32_t Magic;
    std::uint32_t Version;
    std::uint32_t Capacity;
    std::uint32_t Head;
    std::uint64_t Base;
    std::uint64_t Start;
    std::uint32_t Heat;
    std::uint32_t Drop;
    std::uint32_t Pid;
    std::uint32_t Pad;
};

static_assert(sizeof(DetHeader) == 48, "ring must stay 8-byte aligned");

constexpr std::uint32_t kDetSectionSize = sizeof(DetHeader) + kDetCapacity * sizeof(DetEvent);

bool detInit();
void detClose();
void det(DetSource Source, DetKind Kind, DetVerdict Verdict, std::uint64_t Caller,
         std::uint64_t A0 = 0, std::uint64_t A1 = 0, std::uint64_t A2 = 0, std::uint64_t A3 = 0);
void detSnapshot();
unsigned detHeat();
const char *detKindName(DetKind Kind);
const char *detSourceName(DetSource Source);
const char *detVerdictName(DetVerdict Verdict);

}
