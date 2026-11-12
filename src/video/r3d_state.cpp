#include "r3d_state.h"

#include <algorithm>

namespace {
const int kMipXBase[] = {0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047};
const int kMipYBase[] = {0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023};

// Position of texel (xx, yy) inside an uploaded tile: 2x2 blocks in raster
// order, each block stored as (1,0), (0,0), (1,1), (0,1)
inline int tile_order(int xx, int yy, int tile_w) {
    return (yy >> 1) * (2 * tile_w) + (xx >> 1) * 4 + (yy & 1) * 2 + ((xx & 1) ^ 1);
}
}  // namespace

void R3DTextureRam::upload_fifo(const std::vector<uint32_t>& fifo) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t i = 0, count = fifo.size();
    while (count >= 2 && i < count - 2) {
        size_t size_words = (2 + fifo[i] / 2) / 4;
        if (size_words == 0) break;
        size_t end = std::min(count, i + size_words);
        const uint16_t* src = reinterpret_cast<const uint16_t*>(&fifo[i + 2]);
        size_t texels = end > i + 2 ? (end - i - 2) * 2 : 0;
        upload(fifo[i + 1], src, texels);
        i += size_words;
    }
}

void R3DTextureRam::upload(uint32_t h, const uint16_t* src, size_t count) {
    uint32_t type = h >> 24;
    if (type > 2) return;   // 0x80 (gamma table?) and unknown types are ignored
    int x = 32 * (h & 0x3F);
    int y = 32 * ((h >> 7) & 0x1F);
    int w = 32 << ((h >> 14) & 7);
    int hh = 32 << ((h >> 17) & 7);
    int page = (h >> 20) & 1;
    bool bit16 = h & 0x800000;
    size_t used = 0;
    if (type != 2) store_level(x, y + 1024 * page, w, hh, src, count, bit16, h, used);
    if (type == 0 || type == 2) {
        for (int level = 1; level < 11 && w >= 4 && hh >= 4; level++) {
            w /= 2;
            hh /= 2;
            int lx = kMipXBase[level] + (x >> level);
            int ly = 1024 * page + kMipYBase[level] + (y >> level);
            store_level(lx, ly, w, hh, src, count, bit16, h, used);
        }
    }
}

void R3DTextureRam::store_level(int x, int y, int w, int h, const uint16_t* src, size_t count, bool bit16,
                                uint32_t header, size_t& used) {
    int tile_w = std::min(8, w), tile_h = std::min(8, h);
    size_t tile_words = bit16 ? size_t(tile_w * tile_h) : std::max<size_t>(1, tile_w * tile_h / 2);
    for (int ty = 0; ty < h; ty += tile_h) {
        for (int tx = 0; tx < w; tx += tile_w) {
            if (used + tile_words > count) return;   // truncated upload
            const uint16_t* tile = src + used;
            used += tile_words;
            for (int yy = 0; yy < tile_h; yy++) {
                for (int xx = 0; xx < tile_w; xx++) {
                    uint16_t& dst = texels_[((y + ty + yy) & 2047) * 2048 + ((x + tx + xx) & 2047)];
                    if (bit16) {
                        dst = tile[tile_order(xx, yy, tile_w)];
                    } else {
                        int k = tile_order(xx ^ 1, yy ^ 1, tile_w) / 2;
                        uint16_t byte = (tile[k] >> (8 * ((xx & 1) ^ 1))) & 0xFF;
                        bool lo = header & 0x200000, hi = header & 0x400000;
                        if (lo && hi) dst = uint16_t(byte * 0x0101);
                        else if (lo) dst = uint16_t((dst & 0xFF00) | byte);
                        else if (hi) dst = uint16_t((dst & 0x00FF) | (byte << 8));
                    }
                }
            }
        }
    }
    for (int by = (y & 2047) >> 5; by <= ((y & 2047) + h - 1) >> 5 && by < 64; by++)
        for (int bx = x >> 5; bx <= (x + w - 1) >> 5; bx++) generation_[by * 64 + (bx & 63)]++;
    dirty_y0_ = std::min(dirty_y0_, y & 2047);
    dirty_y1_ = std::max(dirty_y1_, std::min(2048, (y & 2047) + h));
}

uint64_t R3DTextureRam::generation_locked(int x, int y, int w, int h) const {
    uint64_t g = 0;
    int page = y & 1024;
    for (int yy = 0; yy < h; yy += 32)
        for (int xx = 0; xx < w; xx += 32) {
            int bx = ((x + xx) & 2047) >> 5, by = (page | ((y + yy) & 1023)) >> 5;
            g = g * 31 + generation_[by * 64 + bx];
        }
    return g;
}

uint64_t R3DTextureRam::texture_generation(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_locked(x, y, w, h);
}

uint64_t R3DTextureRam::read_texture(int x, int y, int w, int h, std::vector<uint16_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out.resize(size_t(w) * h);
    int page = y & 1024;
    for (int yy = 0; yy < h; yy++) {
        const uint16_t* row = &texels_[size_t(page | ((y + yy) & 1023)) * 2048];
        for (int xx = 0; xx < w; xx++) out[size_t(yy) * w + xx] = row[(x + xx) & 2047];
    }
    return generation_locked(x, y, w, h);
}

bool R3DTextureRam::take_dirty(std::vector<uint16_t>& out, int& y0, int& y1) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirty_y0_ >= dirty_y1_) return false;
    y0 = dirty_y0_;
    y1 = dirty_y1_;
    out.assign(texels_.begin() + size_t(y0) * 2048, texels_.begin() + size_t(y1) * 2048);
    dirty_y0_ = 2048;
    dirty_y1_ = 0;
    return true;
}
