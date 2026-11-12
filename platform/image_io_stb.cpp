// iOS port of image_io.cpp: PNG (and any picture stb_image reads) through
// stb_image / stb_image_write instead of Windows Imaging Component
#include "image_io.h"

#include <windows.h>   // (platform/compat: FindFirstFileA)

#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    int n = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return false;
    rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

bool png_save(const std::string& path, int w, int h, const uint8_t* rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}

std::vector<std::string> list_files(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA((dir + "/" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        out.push_back(fd.cFileName);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return out;
}

void list_hashes(const std::string& dir, std::unordered_set<uint64_t>& out) {
    for (const std::string& name : list_files(dir, "*.png")) {
        unsigned long long hash = 0;
        if (sscanf(name.c_str(), "%16llx", &hash) == 1) out.insert(hash);
    }
}
