// The ROM read from srally2.zip (rom_zip.cpp): the chip layout against
// tools/romtool.py's, and clear errors for a missing zip or a wrong one
#include <cstdio>
#include <string>
#include <vector>

#include "miniz.h"
#include "rom_zip.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

int main() {
    {   // CROM: 2 bytes of each of the 4 chips in turn, each word byte-swapped
        std::vector<RomPart> parts = {{0, {0x10, 0x11, 0x12, 0x13}}, {2, {0x20, 0x21, 0x22, 0x23}},
                                      {4, {0x30, 0x31, 0x32, 0x33}}, {6, {0x40, 0x41, 0x42, 0x43}}};
        std::vector<uint8_t> want = {0x11, 0x10, 0x21, 0x20, 0x31, 0x30, 0x41, 0x40,
                                     0x13, 0x12, 0x23, 0x22, 0x33, 0x32, 0x43, 0x42};
        CHECK(interleave_region(parts, 8, 2, true) == want);
    }
    {   // a second bank of chips starts at its offset; VROM is not swapped
        std::vector<RomPart> parts = {{0, {1, 2}}, {2, {3, 4}}, {8, {5, 6}}, {10, {7, 8}}};
        std::vector<uint8_t> want = {1, 2, 3, 4, 0, 0, 0, 0, 5, 6, 7, 8, 0, 0, 0, 0};
        CHECK(interleave_region(parts, 8, 2, false) == want);
    }
    {   // sound: chips one after the other, byte-swapped
        std::vector<RomPart> parts = {{0, {1, 2, 3, 4}}, {4, {5, 6}}};
        std::vector<uint8_t> want = {2, 1, 4, 3, 6, 5};
        CHECK(interleave_region(parts, 1, 1, true) == want);
    }
    {   // no zip
        RomSet rom;
        std::string error;
        CHECK(!load_rom_zip("missing/srally2.zip", rom, error));
        CHECK(error.find("missing/srally2.zip") != std::string::npos);
    }
    {   // a zip without the game's chips: names the first one missing
        const char* path = "rom_zip_test.zip";
        remove(path);
        const char text[] = "another game";
        CHECK(mz_zip_add_mem_to_archive_file_in_place(path, "other.bin", text, sizeof(text), nullptr, 0, 0));
        RomSet rom;
        std::string error;
        CHECK(!load_rom_zip(path, rom, error));
        CHECK(error.find("epr-20635.20") != std::string::npos && rom.crom.empty());
        remove(path);
    }
    {   // a file that is not a zip
        const char* path = "rom_zip_test.txt";
        if (FILE* f = fopen(path, "wb")) {
            fputs("not a zip", f);
            fclose(f);
        }
        RomSet rom;
        std::string error;
        CHECK(!load_rom_zip(path, rom, error));
        CHECK(error.find("is not a valid zip") != std::string::npos);
        remove(path);
    }
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("rom_zip: all correct\n");
    return 0;
}
