// FNV-1a, 64 bits: the one hash of the project, for the names of the texture
// pack images, the 2D replacement cells, the backdrops and the text glyphs.
// tools/lwhash.py computes the same values; tests/fnv_test checks both.
// Two flavours exist because the data differs: bytes (texture images, tile
// patterns) and 32-bit words (BGRA pixels, hashed as whole values).
#pragma once
#include <cstddef>
#include <cstdint>

constexpr uint64_t FNV1A_BASIS = 0xcbf29ce484222325ull, FNV1A_PRIME = 0x100000001b3ull;

// One step: a value (a byte, a word or anything else) mixed into h
inline uint64_t fnv1a_mix(uint64_t h, uint64_t v) { return (h ^ v) * FNV1A_PRIME; }

inline uint64_t fnv1a_bytes(const void* data, size_t n, uint64_t h = FNV1A_BASIS) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) h = fnv1a_mix(h, p[i]);
    return h;
}

inline uint64_t fnv1a_words(const uint32_t* p, size_t n, uint64_t h = FNV1A_BASIS) {
    for (size_t i = 0; i < n; i++) h = fnv1a_mix(h, p[i]);
    return h;
}
