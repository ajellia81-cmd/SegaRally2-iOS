#include "rom_zip.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

#include "miniz.h"

namespace {

struct Chip {
    const char* name;
    uint32_t crc;
    uint32_t offset;
};
struct Region {
    std::vector<uint8_t> RomSet::*field;
    int stride, chunk;
    bool swap;
    std::vector<Chip> chips;
};

// The same table as tools/romtool.py
const std::vector<Region>& regions() {
    static const std::vector<Region> r = {
        {&RomSet::crom, 8, 2, true,
         {{"epr-20635.20", 0x7937473F, 0x0000000}, {"epr-20634.19", 0x45A09245, 0x0000002}, {"epr-20633.18", 0xF5A24F24, 0x0000004}, {"epr-20632.17", 0x6829A801, 0x0000006}}},
        {&RomSet::banked_crom, 8, 2, true,
         {{"mpr-20605.4", 0x00513401, 0x0000000}, {"mpr-20604.3", 0x99C5F396, 0x0000002}, {"mpr-20603.2", 0xAD0D8EB8, 0x0000004}, {"mpr-20602.1", 0x60CFA72A, 0x0000006}, {"mpr-20609.8", 0xC03CC0E5, 0x1000000}, {"mpr-20608.7", 0x0C9B0571, 0x1000002}, {"mpr-20607.6", 0x6DA85AA3, 0x1000004}, {"mpr-20606.5", 0x072498FD, 0x1000006}, {"mpr-20613.12", 0x2938C0D9, 0x2000000}, {"mpr-20612.11", 0x721A44B6, 0x2000002}, {"mpr-20611.10", 0x5D9F8BA2, 0x2000004}, {"mpr-20610.9", 0xB6E0FF4E, 0x2000006}}},
        {&RomSet::vrom, 32, 2, false,
         {{"mpr-20616.26", 0xE11DCF8B, 0x0000000}, {"mpr-20617.27", 0x96ACEF3F, 0x0000002}, {"mpr-20618.28", 0x6C281281, 0x0000004}, {"mpr-20619.29", 0x0FA65819, 0x0000006}, {"mpr-20620.30", 0xEE79585F, 0x0000008}, {"mpr-20621.31", 0x3A99148F, 0x000000A}, {"mpr-20622.32", 0x0618F056, 0x000000C}, {"mpr-20623.33", 0xCCF31B85, 0x000000E}, {"mpr-20624.34", 0x90F30936, 0x0000010}, {"mpr-20625.35", 0x04F804FA, 0x0000012}, {"mpr-20626.36", 0x2D6C97D6, 0x0000014}, {"mpr-20627.37", 0xA14EE871, 0x0000016}, {"mpr-20628.38", 0xBBA829A3, 0x0000018}, {"mpr-20629.39", 0xEAD2EB31, 0x000001A}, {"mpr-20630.40", 0xCC5881B8, 0x000001C}, {"mpr-20631.41", 0x5CB69FFD, 0x000001E}}},
        {&RomSet::sound_program, 1, 1, true,
         {{"epr-20636.21", 0x7139EBF8, 0x0000000}}},
        {&RomSet::sound_samples, 1, 1, true,
         {{"mpr-20614.22", 0xA3930E4A, 0x0000000}, {"mpr-20615.24", 0x62E8A94A, 0x0400000}}},
        {&RomSet::dsb_program, 1, 1, true,
         {{"epr-20641.2", 0xC9B82035, 0x0000000}}},
        {&RomSet::dsb_mpeg, 1, 1, false,
         {{"mpr-20637.57", 0xD66E8A02, 0x0000000}, {"mpr-20638.58", 0xD1513382, 0x0400000}, {"mpr-20639.59", 0xF6603B7B, 0x0800000}, {"mpr-20640.60", 0x9EEA07B7, 0x0C00000}}},
        {&RomSet::drive_program, 1, 1, false,
         {{"epr-20512.bin", 0xCF64350D, 0x0000000}}},
    };
    return r;
}

std::string crc_text(uint32_t crc) {
    char t[16];
    snprintf(t, sizeof(t), "%08x", crc);
    return t;
}

}  // namespace

std::vector<uint8_t> interleave_region(const std::vector<RomPart>& parts, int stride, int chunk, bool swap) {
    size_t size = 0;
    for (const RomPart& p : parts)
        size = std::max(size, p.offset - p.offset % stride + p.data.size() * stride / chunk);
    std::vector<uint8_t> out(size);
    for (const RomPart& p : parts) {
        std::vector<uint8_t> data = p.data;
        if (swap)   // the two bytes of each 16-bit word
            for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
        size_t base = p.offset - p.offset % stride, lane = p.offset % stride;
        for (size_t i = 0; i < data.size(); i += chunk)
            std::copy_n(data.begin() + i, std::min<size_t>(chunk, data.size() - i),
                        out.begin() + base + (i / chunk) * stride + lane);
    }
    return out;
}

bool load_rom_zip(const std::string& zip_path, RomSet& out, std::string& error) {
    // Opened with fopen, like the rest of the program's files: miniz's own
    // open takes the path as UTF-8, and the command line is in the ANSI code page
    FILE* f = fopen(zip_path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + zip_path;
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, f, 0, 0)) {
        fclose(f);
        error = zip_path + " is not a valid zip";
        return false;
    }
    std::map<uint32_t, mz_uint> by_crc;   // chip CRC -> index in the zip
    for (mz_uint i = 0, n = mz_zip_reader_get_num_files(&zip); i < n; i++) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st) && !st.m_is_directory) by_crc.emplace(st.m_crc32, i);
    }
    bool ok = true;
    for (const Region& r : regions()) {
        std::vector<RomPart> parts;
        for (const Chip& c : r.chips) {
            auto it = by_crc.find(c.crc);
            if (it == by_crc.end()) {
                error = std::string("missing ") + c.name + " (crc " + crc_text(c.crc) + ") in " + zip_path +
                        ": not the Sega Rally 2 ROM (srally2) or incomplete";
                ok = false;
                break;
            }
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, it->second, &size, 0);
            if (!data || mz_crc32(MZ_CRC32_INIT, static_cast<const uint8_t*>(data), size) != c.crc) {
                mz_free(data);
                error = std::string("wrong CRC for ") + c.name + " inside " + zip_path;
                ok = false;
                break;
            }
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            parts.push_back({c.offset, std::vector<uint8_t>(bytes, bytes + size)});
            mz_free(data);
        }
        if (!ok) break;
        out.*r.field = interleave_region(parts, r.stride, r.chunk, r.swap);
    }
    mz_zip_reader_end(&zip);
    fclose(f);
    return ok;
}
