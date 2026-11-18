#include <windows.h>

#include <cstdio>
#include <cwchar>

// Test host: loads a DLL, optionally calls one export, then keeps the module
// loaded for N seconds so another process can talk to it. Export "-" skips the
// call; this is how the core pipe test keeps module.dll alive.

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: load-run <dll> [export|-] [seconds]\n");
        return 2;
    }

    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::fwprintf(stderr, L"load %ls failed: %lu\n", argv[1], GetLastError());
        return 3;
    }

    int code = 0;
    if (argc >= 3 && argv[2][0] != L'\0' && argv[2][0] != L'-') {
        char name[128]{};
        if (WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, name, sizeof(name), nullptr, nullptr) == 0) {
            return 4;
        }

        using Fn = int (*)();
        auto fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
        if (!fn) {
            std::fwprintf(stderr, L"export %ls not found: %lu\n", argv[2], GetLastError());
            FreeLibrary(module);
            return 5;
        }
        code = fn();
    }

    if (argc >= 4) {
        const DWORD seconds = static_cast<DWORD>(std::wcstoul(argv[3], nullptr, 10));
        if (seconds != 0) {
            Sleep(seconds * 1000);
        }
    }

    FreeLibrary(module);
    return code;
}
