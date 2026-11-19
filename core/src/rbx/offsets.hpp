#pragma once

// Offsets for the pinned client build. Source: the 0.747 manifest that ships
// with the reference tree (sample/RBX/client_offsets.hpp, generated from
// hyperion_win_live.txt). One manifest per build; never mix values.
//
// Everything here is an RVA relative to the client executable, which is also
// what GetModuleHandleW(nullptr) returns inside module.dll.

#include <cstdint>
#include <Windows.h>

namespace zend::rbx::off {

inline constexpr char kBuildId[] = "4ea8e85481e34f46";

inline std::uintptr_t host_base() noexcept {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

// Globals and functions.
inline constexpr std::uint32_t kFakeDataModelPointer  = 0x8F8E968;
inline constexpr std::uint32_t kRawScheduler          = 0x8F1E408;
inline constexpr std::uint32_t kOpcodeLookupTable     = 0x7419400;
inline constexpr std::uint32_t kGetScriptVMState      = 0x42D3B20;
inline constexpr std::uint32_t kLuaVMLoad             = 0x42C8020;
inline constexpr std::uint32_t kGetCapabilities       = 0x1D18C30;
inline constexpr std::uint32_t kPushInstance          = 0x42B6E70;
inline constexpr std::uint32_t kCastArgs              = 0x422A710;
inline constexpr std::uint32_t kIdentityPointer       = 0x862AC48;
inline constexpr std::uint32_t kGetTlsPointer         = 0xC700;
inline constexpr unsigned      kOpcodeWireMultiplier  = 227;

// DataModel.
inline constexpr std::uint32_t kFakeDataModelToDataModel = 0x1F8;
inline constexpr std::uint32_t kPlaceId                  = 0x1A0;
inline constexpr std::uint32_t kInstanceSubobject        = 0x1F0;

// Scheduler job list and the WaitingHybridScriptsJob fields.
inline constexpr std::uint32_t kJobStart         = 0xC8;
inline constexpr std::uint32_t kJobEnd           = 0xD0;
inline constexpr std::uint32_t kJobTypeName      = 0x18;
inline constexpr std::uint32_t kJobScriptContext = 0x1C0;

// ScriptContext.
inline constexpr std::uint32_t kScriptContextDataModel     = 0x100;
inline constexpr std::uint32_t kScriptContextRequireBypass = 0xC30;

// Script bytecode containers. The protected string holds a std::string at
// +0x18; the fallback scan in fetch_script_bytecode covers layout drift.
inline constexpr std::uint32_t kLocalScriptBytecode  = 0x178;
inline constexpr std::uint32_t kModuleScriptBytecode = 0x120;
inline constexpr std::uint32_t kProtectedStringValue = 0x18;

// Script Source fields (dump/offsets_rbx.hpp). Both are refcounted
// ProtectedString pointers whose payload uses the same +0x18 std::string.
inline constexpr std::uint32_t kModuleScriptSource = 0x118;
inline constexpr std::uint32_t kLocalScriptSource  = 0x170;

// lua_State / RobloxExtraSpace (see lua.h in the vendored Luau).
inline constexpr std::uint32_t kLuaGlobal      = 0x60;
inline constexpr std::uint32_t kLuaUserdata    = 0x10;
inline constexpr std::uint32_t kExtraIdentity  = 0x48;
inline constexpr std::uint32_t kExtraCapabilities = 0x98;

// The engine reads identity/capabilities through a TLS-backed structure that
// is separate from RobloxExtraSpace; both must agree. Per the pinned dump the
// mask lives at +0x28; +0x30 is the capability resolver slot.
inline constexpr std::uint32_t kIdentityCapabilities       = 0x28;
inline constexpr std::uint32_t kIdentityCapabilityResolver = 0x30;

// Signal slot vtables from the pinned dump. Wait connections carry a distinct
// slot class (WaitScriptSlot) whose thread reference is not at the
// FunctionScriptSlot weak-object offset.
inline constexpr std::uint32_t kWaitSlotVftable               = 0x7268BA8;
inline constexpr std::uint32_t kFunctionSlotVftable           = 0x7268EE0;
inline constexpr std::uint32_t kFunctionSlotAltVftable        = 0x7268E28;
inline constexpr std::uint32_t kWeakThreadRefVftable          = 0x7043E00;
inline constexpr std::uint32_t kWeakObjectRefVftable          = 0x703B548;

// Signals (RBXScriptSignal connection list and its slot wrapper).
inline constexpr std::uint32_t kConnectionNext            = 0x10;
inline constexpr std::uint32_t kConnectionEnabled         = 0x20;
inline constexpr std::uint32_t kConnectionSlotWrapper     = 0x30;
inline constexpr std::uint32_t kConnectionSlotWrapperSelf = 0x38;
inline constexpr std::uint32_t kConnectionChain           = 0x68;
inline constexpr std::uint32_t kSlotWrapperThread         = 0x28;
inline constexpr std::uint32_t kSlotWrapperFunctionId     = 0x34;
inline constexpr std::uint32_t kSlotWrapperIsOnce         = 0xB5;
inline constexpr std::uint32_t kSlotWrapperOnceFired      = 0xB6;
inline constexpr std::uint32_t kSlotWrapperWeakObject     = 0x68;
inline constexpr std::uint32_t kSlotWrapperWeakObjectFallback = 0x38;
inline constexpr std::uint32_t kSlotWrapperWeakObjectLegacy   = 0x48;

// Signal descriptor + argument signature (RemoteEvent layout). Each parameter
// is 0x70 bytes: Name at +0x00, Type at +0x08, Class at +0x18.
inline constexpr std::uint32_t kSignalSignature      = 0x60;
inline constexpr std::uint32_t kSignalSignatureEnd   = 0x68;
inline constexpr std::uint32_t kSignalParameterName  = 0x00;
inline constexpr std::uint32_t kSignalParameterType  = 0x08;
inline constexpr std::uint32_t kSignalTypeStride     = 0x70;
inline constexpr std::uint32_t kSignalTypeId         = 0x30;

// Touch + proximity prompt internals.
inline constexpr std::uint32_t kBasePartPrimitive     = 0x178;
inline constexpr std::uint32_t kPrimitiveOverlap      = 0x200;
inline constexpr std::uint32_t kFireTouchInterest     = 0x14D0000;
inline constexpr std::uint32_t kFireProximityPrompt   = 0x312A6F0;

// ClickDetector event dispatchers.
inline constexpr std::uint32_t kFireLeftMouseClick    = 0x3CC86D0;
inline constexpr std::uint32_t kFireRightMouseClick   = 0x3CC8870;
inline constexpr std::uint32_t kFireMouseHoverEnter   = 0x3CC9C40;
inline constexpr std::uint32_t kFireMouseHoverLeave   = 0x3CC9DE0;

} // namespace zend::rbx::off
