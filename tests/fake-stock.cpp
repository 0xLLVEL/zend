#include <windows.h>

// Stand-in for the stock RobloxPlayerBeta.dll. The proxy's fail-open path must
// load this from its own directory and return its run() result.

extern "C" __declspec(dllexport) int run() {
    return 42;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
