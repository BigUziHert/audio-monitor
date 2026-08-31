#pragma once
//
// Audio subformat GUIDs, defined locally rather than pulled from the SDK.
//
// KSDATAFORMAT_SUBTYPE_* is exported differently by each toolchain -- MSVC
// resolves it out of a static library, while mingw's ksmedia.h only ever
// declares it -- so relying on the SDK symbol makes the link toolchain
// specific. These two values are fixed and part of the wire format, so
// defining them here costs nothing and makes the build identical everywhere.
//
#include <windows.h>
#include <cstring>

namespace audiomon {

// 00000001-0000-0010-8000-00aa00389b71
inline constexpr GUID kSubtypePcm{
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// 00000003-0000-0010-8000-00aa00389b71
inline constexpr GUID kSubtypeIeeeFloat{
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

inline bool guidEquals(const GUID& a, const GUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

} // namespace audiomon
