#pragma once

#include <cstdint>

#include "emulator/offsets.h"

#ifndef ZEND_DETLOG
#define ZEND_DETLOG 0
#endif

namespace emu {

inline constexpr bool kRandHwid = true;
inline constexpr bool kRandId = true;
inline constexpr bool kLoadMod = true;
inline constexpr bool kStealth = true;
inline constexpr bool kDebugLog = ZEND_DETLOG != 0; // detlog shared memory + detlog.jsonl
inline constexpr bool kTrace = true;     // loader.log only, no detlog surfaces
inline constexpr wchar_t kModDll[] = L"module.dll";
inline constexpr wchar_t kStockDll[] = L"RobloxPlayerBeta.stock.dll";
inline constexpr char kBuildId[] = "4ea8e85481e34f46";

inline constexpr std::uint32_t kImageSize = 0x094C2000u; // regenerated for 4ea8e85481e34f46 (0.747)
inline constexpr std::uint32_t kTextRva = 0x00001000u;
inline constexpr std::uint32_t kTextSize = 0x066167DCu;  // .text VirtualSize
inline constexpr std::uint32_t kTextMap = 0x06617000u;   // .text SizeOfRawData
inline constexpr std::uint32_t kTextPad = 0x00000824u;   // kTextMap - kTextSize
inline constexpr std::uint32_t kRtOff = kTextSize;
inline constexpr std::uint32_t kRtSize = off::kRtCount * 12u;
inline constexpr std::uint32_t kPayloadSize = kTextSize + 8u + kRtSize;

}
