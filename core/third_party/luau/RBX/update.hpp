#pragma once

// Shim for the Roblox Luau fork's dependency on the reference tree.
//
// The vendored VM is the client's patched Luau: several struct fields are
// stored through the client's pointer-encryption wrappers (VMValue0..4) and a
// few helpers live in the client executable instead of the library. The
// reference project injects those through RBX/update.hpp; this file supplies
// the same definitions with the RVAs for build 4ea8e85481e34f46 (0.747).
//
// Keep in sync with the build manifest. `client_abi_checks.h` guards the
// struct offsets, so a mismatch fails the build instead of the client.

#include <Windows.h>
#include <cstddef>
#include <cstdint>

struct lua_State;

namespace rbx_shim
{
    inline uintptr_t hostBase() noexcept
    {
        return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    }
}

// --- pointer-encryption wrappers (EncryptionsHelper.hpp) -------------------

template <typename T>
struct VMValue0
{
private:
    T Storage;

public:
    operator const T() const { return Storage; }
    void operator=(const T& Value) { Storage = Value; }
    const T operator->() const { return Storage; }
    T Get() const { return Storage; }
    void Set(const T& Value) { Storage = Value; }
};

template <typename T>
struct VMValue1
{
private:
    T Storage;

public:
    operator const T() const { return (T)((uintptr_t)Storage - (uintptr_t)this); }
    void operator=(const T& Value) { Storage = (T)((uintptr_t)Value + (uintptr_t)this); }
    const T operator->() const { return operator const T(); }
    T Get() const { return operator const T(); }
    void Set(const T& Value) { operator=(Value); }
};

template <typename T>
struct VMValue2
{
private:
    T Storage;

public:
    operator const T() const { return (T)((uintptr_t)this - (uintptr_t)Storage); }
    void operator=(const T& Value) { Storage = (T)((uintptr_t)this - (uintptr_t)Value); }
    const T operator->() const { return operator const T(); }
    T Get() const { return operator const T(); }
    void Set(const T& Value) { operator=(Value); }
};

template <typename T>
struct VMValue3
{
private:
    T Storage;

public:
    operator const T() const { return (T)((uintptr_t)this ^ (uintptr_t)Storage); }
    void operator=(const T& Value) { Storage = (T)((uintptr_t)Value ^ (uintptr_t)this); }
    const T operator->() const { return operator const T(); }
    T Get() const { return operator const T(); }
    void Set(const T& Value) { operator=(Value); }
};

template <typename T>
struct VMValue4
{
private:
    T Storage;

public:
    operator const T() const { return (T)((uintptr_t)this + (uintptr_t)Storage); }
    void operator=(const T& Value) { Storage = (T)((uintptr_t)Value - (uintptr_t)this); }
    const T operator->() const { return operator const T(); }
    T Get() const { return operator const T(); }
    void Set(const T& Value) { operator=(Value); }
};

// --- per-field encryption scheme (Encryptions.hpp) -------------------------

#define CLOSURE_DEBUGNAME_ENC VMValue0
#define PROTO_P_ENC VMValue0
#define CLOSURE_CONT_ENC VMValue2
#define LSTATE_STACKSIZE_ENC VMValue1
#define PROTO_ABSLINEINFO_ENC VMValue4
#define PROTO_DEBUGINSN_ENC VMValue1
#define PROTO_DEBUGNAME_ENC VMValue1
#define PROTO_LINEINFO_ENC VMValue4
#define PROTO_LOCVARS_ENC VMValue4
#define PROTO_SOURCE_ENC VMValue2
#define PROTO_TYPEINFO_ENC VMValue4
#define PROTO_UPVALUES_ENC VMValue2
#define PROTO_USERDATA_ENC VMValue3
#define TSTRING_HASH_ENC VMValue1
#define UDATA_META_ENC VMValue1

#define proto_lineinfo PROTO_LINEINFO_ENC
#define proto_debuginsn PROTO_DEBUGINSN_ENC
#define proto_typeinfo PROTO_TYPEINFO_ENC
#define proto_abslineinfo PROTO_ABSLINEINFO_ENC
#define proto_source PROTO_SOURCE_ENC
#define proto_locvars PROTO_LOCVARS_ENC
#define proto_upvalues PROTO_UPVALUES_ENC
#define proto_debugname PROTO_DEBUGNAME_ENC
#define proto_userdata PROTO_USERDATA_ENC
#define udata_meta UDATA_META_ENC
#define closure_cont CLOSURE_CONT_ENC
#define tstring_hash TSTRING_HASH_ENC
#define lstate_stacksize LSTATE_STACKSIZE_ENC

// --- client-side helpers and globals ---------------------------------------

namespace ClientOffsets
{
    namespace Luau
    {
        inline const uintptr_t LuaO_NilObject = rbx_shim::hostBase() + 0x69919C8;
        inline const uintptr_t LuaH_DummyNode = rbx_shim::hostBase() + 0x69767A8;
        inline const uintptr_t LuaC_Step = rbx_shim::hostBase() + 0x268F7F0;
    }

    namespace Derived
    {
        inline const uintptr_t StringTableHashSaltFlag = rbx_shim::hostBase() + 0x82B93E0;
        inline const uintptr_t ResizeSaltedStringTable = rbx_shim::hostBase() + 0x268FFA0;
    }
}

namespace Offsets
{
    namespace Luau
    {
        inline const uintptr_t LuaO_NilObject = ClientOffsets::Luau::LuaO_NilObject;
        inline const uintptr_t LuaH_DummyNode = ClientOffsets::Luau::LuaH_DummyNode;
    }
}

namespace Roblox
{
    using LuaD_ThrowT = void(__fastcall*)(lua_State*, int);
    inline LuaD_ThrowT LuaD_Throw =
        reinterpret_cast<LuaD_ThrowT>(rbx_shim::hostBase() + 0x266A470);

    using Luau_ExecuteT = void(__fastcall*)(lua_State*);
    inline Luau_ExecuteT Luau_Execute =
        reinterpret_cast<Luau_ExecuteT>(rbx_shim::hostBase() + 0x26A91B0);
}
