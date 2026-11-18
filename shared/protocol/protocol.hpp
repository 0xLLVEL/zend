#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zend::protocol {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;
inline constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\zend-";
inline constexpr wchar_t kSessionDirSuffix[] = L"\\zend\\runtime";
inline constexpr wchar_t kSessionFilePrefix[] = L"session-";
inline constexpr wchar_t kSessionFileSuffix[] = L".json";

enum class Op {
    Hello,
    Exec,
    Result,
    Status,
    StatusResult,
    Log,
    Toast,
    Ping,
    Pong,
    Detach,
};

inline std::string_view op_name(Op op) noexcept {
    switch (op) {
    case Op::Hello: return "hello";
    case Op::Exec: return "exec";
    case Op::Result: return "result";
    case Op::Status: return "status";
    case Op::StatusResult: return "status.result";
    case Op::Log: return "log";
    case Op::Toast: return "toast";
    case Op::Ping: return "ping";
    case Op::Pong: return "pong";
    case Op::Detach: return "detach";
    }
    return {};
}

} // namespace zend::protocol
