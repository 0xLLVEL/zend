#pragma once

#include <string_view>

namespace zend {

// Append one line to %LOCALAPPDATA%\zend\logs\core.log.
// Best-effort: logging must never take the module down.
void log(std::string_view line) noexcept;

void logf(const char* fmt, ...) noexcept;

} // namespace zend
