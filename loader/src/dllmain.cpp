#include "emulator/common.h"

#include <cstdio>
#include <string>

namespace {

using RunFn = int (*)();

RunFn g_stockRun = nullptr;
HMODULE g_stock = nullptr;
bool g_failOpen = false;

// Fail-open is only valid before restore() writes the image. Loading the stock
// DLL has to happen here, inside DllMain: the exe's entry point is an int3
// page until the loader patches it, so a lazy load from run() would be too
// late. The stock DllMain performs the same work it would have done when the
// Windows loader mapped it, including patching the entry point.
RunFn loadStock()
{
    if (g_stockRun) {
        return g_stockRun;
    }
    if (emu::g_Dir.empty()) {
        emu::trace("fail-open: no loader dir");
        return nullptr;
    }

    const std::wstring full = emu::path(emu::g_Dir, emu::kStockDll);
    emu::trace("fail-open: LoadLibrary stock");
    g_stock = LoadLibraryW(full.c_str());
    if (!g_stock) {
        char line[192];
        std::snprintf(line, sizeof(line), "fail-open: stock load failed (%lu)",
                      static_cast<unsigned long>(GetLastError()));
        emu::trace(line);
        return nullptr;
    }

    g_stockRun = reinterpret_cast<RunFn>(GetProcAddress(g_stock, "run"));
    if (!g_stockRun) {
        emu::trace("fail-open: stock has no run export");
        return nullptr;
    }

    emu::trace("fail-open: stock ready");
    return g_stockRun;
}

} // namespace

// RobloxPlayerBeta.exe imports this from RobloxPlayerBeta.dll. In the normal
// path the loader has already redirected the entry point, so this stays a
// liveness export. After a fail-open it forwards to the stock loader's run.
extern "C" __declspec(dllexport) int
run()
{
    if (g_stockRun) {
        return g_stockRun();
    }
    if (g_failOpen) {
        if (const RunFn fn = loadStock()) {
            return fn();
        }
    }
    return 1;
}

BOOL WINAPI
DllMain(HINSTANCE Inst, DWORD Reason, LPVOID)
{
    if (Reason == DLL_PROCESS_ATTACH) {
        emu::trace("attach");
        emu::g_Self = Inst;
        DisableThreadLibraryCalls(Inst);
        emu::g_Base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));

        if (!emu::g_Base) {
            emu::trace("FAIL base");
            return FALSE;
        }

        // init() fills g_Dir too; fail-open needs it before init runs.
        wchar_t self[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(Inst, self, MAX_PATH);
        if (n && n < MAX_PATH) {
            wchar_t* slash = wcsrchr(self, L'\\');
            if (slash) {
                *slash = L'\0';
                emu::g_Dir.assign(self);
            }
        }

        if (!emu::img()) {
            emu::trace("FAIL img(header) -> fail-open");
            g_failOpen = true;
            return loadStock() ? TRUE : FALSE;
        }
        if (!emu::init()) {
            if (emu::g_Committed) {
                emu::trace("FAIL init after restore");
                return FALSE;
            }
            emu::trace("FAIL init before restore -> fail-open");
            g_failOpen = true;
            return loadStock() ? TRUE : FALSE;
        }

        emu::trace("attach ok");
        return TRUE;
    }

    if (Reason == DLL_PROCESS_DETACH) {
        emu::hookOUT();
        emu::detClose();
    }
    return TRUE;
}
