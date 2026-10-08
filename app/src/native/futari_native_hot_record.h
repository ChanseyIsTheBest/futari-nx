// Record mode of the localized copies (futari_native_hot.cpp, the guard). Included AFTER every Native() header and the
// hooks, BEFORE the *_record.h headers: from here on every guest memory access of the generated statements goes
// that writes goes through Touch(), which logs the bytes there before they change, so the guard can put memory back
// as it was and replay the recompiled original from the same state. Scalar loads are not logged; vector accesses are
// (loads and stores share REX_RAW_ADDR), and the guard restores only bytes that actually changed, so a block that was
// only read is left alone (another thread may have written it meanwhile).

#pragma once

#include "futari_native_hot_undo.h"

#undef REX_RAW_ADDR
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#undef REX_LOAD_D_U8
#undef REX_LOAD_D_U16
#undef REX_LOAD_D_U32
#undef REX_LOAD_D_U64
#undef REX_STORE_D_U8
#undef REX_STORE_D_U16
#undef REX_STORE_D_U32
#undef REX_STORE_D_U64

#define FUTARI_HOT_AT(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#define REX_RAW_ADDR(x) ::futari::hot::Touch(FUTARI_HOT_AT(x), 16)
#define REX_LOAD_U8(x) (*(u8*)FUTARI_HOT_AT(x))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16*)FUTARI_HOT_AT(x))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)FUTARI_HOT_AT(x))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64*)FUTARI_HOT_AT(x))
#define REX_STORE_U8(x, y) (*(u8*)::futari::hot::Touch(FUTARI_HOT_AT(x), 1) = (y))
#define REX_STORE_U16(x, y) (*(u16*)::futari::hot::Touch(FUTARI_HOT_AT(x), 2) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32*)::futari::hot::Touch(FUTARI_HOT_AT(x), 4) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64*)::futari::hot::Touch(FUTARI_HOT_AT(x), 8) = __builtin_bswap64(y))

// The D-form address exactly as the generated code computes it on each platform (futari_pch.h).
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
#define FUTARI_HOT_AT_D(r, d) FUTARI_HOT_AT((u32)(r) + (d))
#else
#define FUTARI_HOT_AT_D(r, d) (base + (uint64_t)(u32)(r) + (int64_t)(d))
#endif
#define REX_LOAD_D_U8(r, d) (*(u8*)FUTARI_HOT_AT_D(r, d))
#define REX_LOAD_D_U16(r, d) __builtin_bswap16(*(u16*)FUTARI_HOT_AT_D(r, d))
#define REX_LOAD_D_U32(r, d) __builtin_bswap32(*(u32*)FUTARI_HOT_AT_D(r, d))
#define REX_LOAD_D_U64(r, d) __builtin_bswap64(*(u64*)FUTARI_HOT_AT_D(r, d))
#define REX_STORE_D_U8(r, d, y) (*(u8*)::futari::hot::Touch(FUTARI_HOT_AT_D(r, d), 1) = (y))
#define REX_STORE_D_U16(r, d, y) (*(u16*)::futari::hot::Touch(FUTARI_HOT_AT_D(r, d), 2) = __builtin_bswap16(y))
#define REX_STORE_D_U32(r, d, y) (*(u32*)::futari::hot::Touch(FUTARI_HOT_AT_D(r, d), 4) = __builtin_bswap32(y))
#define REX_STORE_D_U64(r, d, y) (*(u64*)::futari::hot::Touch(FUTARI_HOT_AT_D(r, d), 8) = __builtin_bswap64(y))
