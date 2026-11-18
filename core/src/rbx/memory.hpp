#pragma once

// Memory access that never asks the OS about pages: reads and writes go
// straight to the address and faults are caught per call. VirtualQuery and
// VirtualProtect are on the section 8 banned list, and they also make the
// core noisy. Every accessor is small and has no destructible locals so MSVC
// accepts __try.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zend::rbx {

inline bool is_user_ptr(std::uintptr_t address) noexcept {
    return address >= 0x10000 && address < 0x7FFFFFFFFFFFull;
}

__declspec(noinline) inline bool try_read_bytes(std::uintptr_t address,
                                                void* dst,
                                                std::size_t len) noexcept {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(address), len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline bool try_write_bytes(std::uintptr_t address,
                                                 const void* src,
                                                 std::size_t len) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool read(std::uintptr_t address, T& out) noexcept {
    return try_read_bytes(address, &out, sizeof(T));
}

template <typename T>
bool write(std::uintptr_t address, const T& value) noexcept {
    return try_write_bytes(address, &value, sizeof(T));
}

inline bool read_u8(std::uintptr_t address, std::uint8_t& out) noexcept {
    return read(address, out);
}

inline bool read_u32(std::uintptr_t address, std::uint32_t& out) noexcept {
    return read(address, out);
}

inline bool read_u64(std::uintptr_t address, std::uint64_t& out) noexcept {
    return read(address, out);
}

inline bool read_ptr(std::uintptr_t address, std::uintptr_t& out) noexcept {
    std::uint64_t value = 0;
    if (!read_u64(address, value)) {
        return false;
    }
    out = static_cast<std::uintptr_t>(value);
    return true;
}

inline bool write_ptr(std::uintptr_t address, std::uintptr_t value) noexcept {
    return write<std::uint64_t>(address, static_cast<std::uint64_t>(value));
}

} // namespace zend::rbx
