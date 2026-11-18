#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace emu {

// Called from imp() for every import the client resolves. When Fn names a probe
// surface this stores Real in the matching global, points Out at the logging
// wrapper and returns true.
bool probeHook(const char *Fn, FARPROC Real, FARPROC *Out);

}
