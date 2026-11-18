#pragma once

#include "emulator/state.h"

namespace emu {

bool seed();
UINT WINAPI firmware(DWORD Provider, DWORD Id, PVOID Buf, DWORD Len);
ULONG WINAPI adapters(ULONG Family, ULONG Flags, PVOID Reserved, PIP_ADAPTER_ADDRESSES Buf, PULONG Size);
LSTATUS WINAPI regQExA(HKEY Key, LPCSTR Val, LPDWORD Res, LPDWORD Type, LPBYTE Data, LPDWORD Size);
LSTATUS WINAPI regQExW(HKEY Key, LPCWSTR Val, LPDWORD Res, LPDWORD Type, LPBYTE Data, LPDWORD Size);
LSTATUS WINAPI regGetW(HKEY Key, LPCWSTR Sub, LPCWSTR Val, DWORD Flags, LPDWORD Type, PVOID Data, LPDWORD Size);
void setRegHooks(FARPROC QExA, FARPROC QExW, FARPROC GetW);

}
