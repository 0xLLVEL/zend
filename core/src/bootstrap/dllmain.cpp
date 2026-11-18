#include <windows.h>

#include "ipc/pipe.hpp"
#include "rbx/render.hpp"
#include "rbx/scheduler.hpp"
#include "util/log.hpp"

namespace {

HMODULE g_self = nullptr;

DWORD WINAPI boot(void*) {
    zend::log("core loading");

    if (!zend::start()) {
        zend::log("core: pipe start failed");
        return 1;
    }

    zend::rbx::start();
    zend::rbx::render::start();

    zend::logf("core: session %s (build %s)",
               zend::session().pipe.c_str(), zend::session().build.c_str());
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);

        HANDLE thread = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }

    if (reason == DLL_PROCESS_DETACH) {
        zend::rbx::render::stop();
        zend::rbx::stop();
        zend::stop();
        zend::log("core unloaded");
    }

    return TRUE;
}
