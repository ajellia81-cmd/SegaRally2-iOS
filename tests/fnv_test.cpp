// The FNV-1a of src/common/fnv1a.h against tests/fnv_expected.txt. The Python
// twin (tests/fnv_test.py) checks tools/lwhash.py against the same file, so
// the names the C++ side looks up are the ones the Python tools produce.
//
//   fnv_test <expected.txt>            compares
//   fnv_test <expected.txt> --write    regenerates the file
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fnv1a.h"

namespace {

std::vector<std::pair<std::string, uint64_t>> vectors() {
    std::vector<std::pair<std::string, uint64_t>> v;
    v.push_back({"empty", fnv1a_bytes("", 0)});
    v.push_back({"a", fnv1a_bytes("a", 1)});
    v.push_back({"foobar", fnv1a_bytes("foobar", 6)});
    uint8_t bytes[256];
    for (int i = 0; i < 256; i++) bytes[i] = uint8_t(i);
    v.push_back({"bytes0-255", fnv1a_bytes(bytes, sizeof bytes)});
    uint32_t words[8] = {0, 1, 0xFF, 0x100, 0xFFFF, 0x12345678, 0xFFFFFFFF, 0x80000000};
    v.push_back({"words8", fnv1a_words(words, 8)});
    uint32_t zeros[64] = {};
    v.push_back({"zeros64", fnv1a_words(zeros, 64)});
    // chained: dims then pixels, as texture_pack.cpp names an image
    uint16_t dims[2] = {64, 32};
    v.push_back({"dims+words", fnv1a_words(words, 8, fnv1a_bytes(dims, sizeof dims))});
    v.push_back({"mix", fnv1a_mix(fnv1a_mix(FNV1A_BASIS ^ 0x1234, 7), 0xABCDEF)});
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fnv_test <expected.txt> [--write]\n");
        return 2;
    }
    auto v = vectors();
    if (argc > 2 && strcmp(argv[2], "--write") == 0) {
        FILE* f = fopen(argv[1], "w");
        if (!f) return perror(argv[1]), 2;
        fprintf(f, "# FNV-1a 64 test vectors: name fnv1a64 (see tests/fnv_test.cpp and tests/fnv_test.py)\n");
        for (auto& [name, h] : v) fprintf(f, "%s %016llX\n", name.c_str(), (unsigned long long)h);
        fclose(f);
        printf("wrote %s\n", argv[1]);
        return 0;
    }
    FILE* f = fopen(argv[1], "r");
    if (!f) return perror(argv[1]), 2;
    int fails = 0, seen = 0;
    char line[256], name[64];
    unsigned long long hash;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%63s %llx", name, &hash) != 2) return fprintf(stderr, "invalid line: %s", line), 2;
        bool found = false;
        for (auto& [n, h] : v)
            if (n == name) {
                found = true;
                seen++;
                if (h != hash) {
                    printf("%s: expected %016llX, got %016llX\n", name, hash, (unsigned long long)h);
                    fails++;
                }
            }
        if (!found) return printf("%s: unknown vector\n", name), 1;
    }
    fclose(f);
    if (seen != int(v.size())) return printf("the file has %d vectors, the test %zu\n", seen, v.size()), 1;
    printf(fails ? "fnv (C++): %d failures\n" : "fnv (C++): %d vectors match\n", fails ? fails : seen);
    return fails != 0;
}
