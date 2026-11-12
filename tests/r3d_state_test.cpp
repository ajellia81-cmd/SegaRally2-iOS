// R3DStateReader (r3d_state.h) against the hash lookup it replaced: random
// pages in the three Real3D regions, some missing, and reads of every kind
// of address, including out-of-range ones and the mirrors past 24 bits
#include <cstdio>
#include <cstring>
#include <random>

#include "r3d_state.h"

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

// The previous R3DFrameState::read / cull / poly
static uint32_t ref_read(const R3DFrameState& s, uint32_t ppc_addr) {
    auto it = s.pages.find(ppc_addr >> 16);
    if (it == s.pages.end()) return 0;
    uint32_t v;
    memcpy(&v, it->second->data() + (ppc_addr & 0xFFFC), 4);
    return v;
}
static uint32_t ref_cull(const R3DFrameState& s, uint32_t addr) {
    addr &= 0xFFFFFF;
    if (addr >= 0x800000 && addr < 0x840000) return ref_read(s, 0x8E000000 + (addr & 0x3FFFF) * 4);
    if (addr < 0x100000) return ref_read(s, 0x8C000000 + addr * 4);
    return 0;
}
static uint32_t ref_poly(const R3DFrameState& s, uint32_t addr) { return ref_read(s, 0x98000000 + (addr & 0xFFFFF) * 4); }

int main() {
    std::mt19937 rng(1234);
    R3DFrameState state;
    struct Region { uint32_t key, pages; };
    const Region regions[] = {{0x8C00, 64}, {0x8E00, 16}, {0x9800, 64}};
    for (const Region& r : regions)
        for (uint32_t i = 0; i < r.pages; i++) {
            if (rng() % 3 == 0) continue;   // a page the game never wrote
            auto page = std::make_shared<R3DPage>();
            for (auto& b : *page) b = uint8_t(rng());
            state.pages[r.key + i] = page;
        }
    // Pages outside the three regions must not be visible
    auto other = std::make_shared<R3DPage>();
    other->fill(0xAB);
    state.pages[0x8C40] = other;
    state.pages[0x8E10] = other;
    state.pages[0x9840] = other;

    R3DStateReader rd;
    rd.reset(state);
    for (int i = 0; i < 2000000; i++) {
        uint32_t addr = rng();
        switch (i % 4) {   // bias towards the valid ranges
        case 0: addr &= 0xFFFFF; break;
        case 1: addr = 0x800000 | (addr & 0x7FFFF); break;
        case 2: addr &= 0x1FFFFFF; break;
        default: break;
        }
        CHECK(rd.cull(addr) == ref_cull(state, addr));
        CHECK(rd.poly(addr) == ref_poly(state, addr));
        float a = rd.cullf(addr), b;
        uint32_t v = ref_cull(state, addr);
        memcpy(&b, &v, 4);
        CHECK(memcmp(&a, &b, 4) == 0);
        if (g_failed > 10) break;
    }
    // A reader reused for another frame forgets the old pages
    R3DFrameState empty;
    rd.reset(empty);
    CHECK(rd.cull(0x10) == 0 && rd.cull(0x800010) == 0 && rd.poly(0x10) == 0);
    CHECK(R3DStateReader::cull_valid(0x0FFFFF) && !R3DStateReader::cull_valid(0x100000));
    CHECK(R3DStateReader::cull_valid(0x83FFFF) && !R3DStateReader::cull_valid(0x840000));
    if (g_failed) return 1;
    printf("r3d_state: all reads match\n");
    return 0;
}
