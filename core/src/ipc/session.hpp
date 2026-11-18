#pragma once

#include <cstdint>
#include <string>

namespace zend {

// Build id shared by loader and core. One manifest per Roblox build.
inline constexpr char kBuildId[] = "4ea8e85481e34f46";

struct SessionInfo {
    std::uint32_t pid = 0;
    std::string pipe;
    std::string token;
    std::string build;
    std::uint64_t ts = 0;
};

// Creates %LOCALAPPDATA%\zend\runtime, generates a nonce pipe name and token,
// and writes session-<pid>.json atomically (.tmp + MoveFileExW).
bool create_session(SessionInfo& out);

void remove_session();

} // namespace zend
