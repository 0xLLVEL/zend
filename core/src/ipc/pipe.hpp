#pragma once

#include "ipc/session.hpp"

namespace zend {

// Creates the session file, then hosts \\.\pipe\zend-<nonce> on a dedicated
// thread. Returns false if the session file or the first pipe instance could
// not be created (the caller keeps the module loaded either way).
bool start();

// Signals the pipe thread to stop and removes the session file. Safe to call
// from DllMain detach.
void stop();

const SessionInfo& session();

} // namespace zend
