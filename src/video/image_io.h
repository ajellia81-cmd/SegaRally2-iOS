// What the texture, 2D, backdrop and text packs share: RGBA8 pictures, PNG
// files read and written through Windows Imaging Component (rows top to
// bottom, 4 bytes per pixel) and the listing of a pack folder.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

// A picture loaded by a pack, to be uploaded by the renderer into its slot
struct RgbaImage {
    uint32_t slot = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba);
bool png_save(const std::string& path, int w, int h, const uint8_t* rgba);

// Names (without the directory) of the files in dir that match pattern ("*.png")
std::vector<std::string> list_files(const std::string& dir, const char* pattern);
// Hashes of the <16 hex digits>.png files in dir, added to out
void list_hashes(const std::string& dir, std::unordered_set<uint64_t>& out);
