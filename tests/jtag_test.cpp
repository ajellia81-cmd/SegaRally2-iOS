// The JTAG chain (jtag.cpp): after a reset, shifting DR out of the 17 devices
// gives the IDCODEs from TDO's end (3D-RAMs in BYPASS add one 0 bit each)
#include <cstdio>
#include <vector>

#include "jtag.h"

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

static bool clock(Jtag& j, bool tms, bool tdi = false) {
    uint8_t base = 0x80 | (tms ? 0x04 : 0) | (tdi ? 0x20 : 0);
    bool tdo = j.read() >> 5;   // sampled before the rising edge, as the game does
    j.write(base);              // TCK low
    j.write(base | 0x40);       // rising edge
    j.write(base);              // falling edge
    return tdo;
}

int main() {
    Jtag j;
    for (int i = 0; i < 5; i++) clock(j, true);   // Test-Logic-Reset
    clock(j, false);                              // Run-Test/Idle
    clock(j, true);                               // Select-DR
    clock(j, false);                              // Capture-DR
    clock(j, false);                              // Shift-DR (captured on this rising edge)
    std::vector<bool> bits;
    for (int i = 0; i < 9 * 32 + 8; i++) bits.push_back(clock(j, false));
    // The last device (3D-RAM 7) and 3D-RAM 6 and 5 come first: three 0s, then Mars11
    auto word = [&](int at) {
        uint32_t v = 0;
        for (int b = 0; b < 32; b++) v |= uint32_t(bits[at + b]) << b;
        return v;
    };
    CHECK(!bits[0] && !bits[1] && !bits[2]);
    CHECK(word(3) == 0x316C6057);        // Mars11
    CHECK(word(35) == 0x316C6057);       // Mars10
    if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
    printf("jtag: all correct\n");
    return 0;
}
