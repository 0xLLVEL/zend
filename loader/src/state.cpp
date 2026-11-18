#include "emulator/common.h"

namespace emu {

HMODULE g_Self = nullptr;
unsigned char *g_Base = nullptr;
std::wstring g_Dir;
PVOID g_Veh = nullptr;
BCRYPT_ALG_HANDLE g_Hash = nullptr;
ULONG g_HashObj = 0;
LARGE_INTEGER g_T0{};
LARGE_INTEGER g_T1{};
std::uint32_t g_Seed = 0;
FirmwareFn g_Firmware = nullptr;
AdaptersFn g_Adapters = nullptr;
std::uintptr_t g_ModBase = 0;
std::size_t g_ModSize = 0;
std::uintptr_t g_IdTrampBase = 0;
std::size_t g_IdTrampSize = 0;
bool g_Committed = false;

}
