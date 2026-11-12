// MSVC intrinsics used by the game, on clang (iOS port)
#pragma once
#include <cstdint>
static inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
static inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }
static inline unsigned char _BitScanReverse(unsigned long* index, uint32_t v) {
    if (!v) return 0;
    *index = 31u - unsigned(__builtin_clz(v));
    return 1;
}
static inline unsigned char _BitScanForward(unsigned long* index, uint32_t v) {
    if (!v) return 0;
    *index = unsigned(__builtin_ctz(v));
    return 1;
}
static inline unsigned int __popcnt(uint32_t v) { return unsigned(__builtin_popcount(v)); }
static inline unsigned long long __popcnt64(uint64_t v) { return unsigned(__builtin_popcountll(v)); }
static inline uint32_t _rotl(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t _rotr(uint32_t v, int n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
