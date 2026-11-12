#include "dsb2.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD   // the decoder runs a few frames per video frame: plain C is plenty
#include "minimp3.h"

extern "C" {
#include "m68k.h"
}

Dsb2* g_dsb_bus = nullptr;   // the bus the Musashi callbacks use while the DSB2 runs (sound_board.cpp)

namespace {
// SR2_DSB_LOG: every command byte, decoder byte and stream started
const bool g_log = getenv("SR2_DSB_LOG") != nullptr;
constexpr int kClock = 11000000;           // 68000 clock
constexpr int kTimerPeriod = kClock / 1000;  // level 2 interrupt at 1 kHz
constexpr uint32_t kRamBase = 0xF00000, kRamSize = 0x20000;

// Interrupts are cleared when the 68000 takes them (the sound board's own
// context keeps Musashi's default acknowledge, which leaves them set)
int ack(int) {
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

// Runs `fn` with the DSB2's 68000 context loaded, then puts back the one
// that was running (the sound board's)
template <class F>
void with_context(std::vector<uint8_t>& mine, Dsb2* bus, F fn) {
    std::vector<uint8_t> other(m68k_context_size());
    m68k_get_context(other.data());
    m68k_set_context(mine.data());
    g_dsb_bus = bus;
    fn();
    g_dsb_bus = nullptr;
    m68k_get_context(mine.data());
    m68k_set_context(other.data());
}
}  // namespace

bool Dsb2::init(std::vector<uint8_t> program, std::vector<uint8_t> mpeg) {
    if (program.size() < 0x1000 || mpeg.empty()) return false;
    program_ = std::move(program);
    // The 68000 reads big-endian words; a dump with the bytes of each word
    // swapped (reset stack 0x00F0FFFE read as F0 00 FE FF) is put right
    if (program_[0] == 0xF0 && program_[1] == 0x00)
        for (size_t i = 0; i + 1 < program_.size(); i += 2) std::swap(program_[i], program_[i + 1]);
    mpeg_ = std::move(mpeg);
    ram_.assign(kRamSize, 0);
    decoder_ = new mp3dec_t;
    mp3dec_init(static_cast<mp3dec_t*>(decoder_));
    context_.assign(m68k_context_size(), 0);
    m68k_get_context(context_.data());   // a valid context to start from
    with_context(context_, this, [&] {
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        m68k_set_int_ack_callback(ack);
        m68k_pulse_reset();
    });
    ready_ = true;
    return true;
}

bool Dsb2::extract_track(const std::vector<uint8_t>& program, const std::vector<uint8_t>& mpeg, int track, int channel,
                         const char* wav_path) {
    if (program.size() < 0x9000 || mpeg.size() < 4) return false;
    bool swapped = program[0] == 0xF0 && program[1] == 0x00;   // as in init()
    auto byte = [&](size_t a) { return a < program.size() ? program[swapped ? a ^ 1 : a] : uint8_t(0); };
    auto be32 = [&](size_t a) { return uint32_t(byte(a)) << 24 | uint32_t(byte(a + 1)) << 16 | uint32_t(byte(a + 2)) << 8 | byte(a + 3); };
    uint32_t entry = be32(0x8008 + 4 * size_t(track));
    uint32_t from = be32(entry + 6), to = be32(entry + 10);
    if (entry >= program.size() || from >= to || to > mpeg.size() || mpeg[from] != 0xFF || (mpeg[from + 1] & 0xF0) != 0xF0) {
        fprintf(stderr, "[dsb] track %d not found in the music board's table\n", track);
        return false;
    }
    mp3dec_t dec;
    mp3dec_init(&dec);
    std::vector<int16_t> pcm;
    int hz = 32000;
    for (uint32_t pos = from; pos < to;) {
        mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info{};
        int n = mp3dec_decode_frame(&dec, mpeg.data() + pos, int(to - pos), frame, &info);
        if (info.frame_bytes == 0) break;
        pos += uint32_t(info.frame_bytes);
        if (info.hz > 0) hz = info.hz;
        // Sega Rally 2's streams carry two different tunes, one per channel,
        // and the board plays one of them on both sides (0xA0 / 0xB1): the
        // channel asked for, on both sides
        int c = info.channels > 1 ? std::clamp(channel, 0, 1) : 0;
        for (int i = 0; i < n; i++) {
            pcm.push_back(frame[i * info.channels + c]);
            pcm.push_back(frame[i * info.channels + c]);
        }
    }
    if (pcm.empty()) return false;
    // Sega Rally 2's tracks are mastered at full scale (the board plays them
    // turned down, about 0.15 in the attract): played as they are, Windows'
    // resampling to the output rate clipped them and the menu music sounded
    // distorted. Peaks at half scale leave the headroom
    int peak = 1;
    for (int16_t v : pcm) peak = std::max(peak, std::abs(int(v)));
    if (peak > 16384)
        for (int16_t& v : pcm) v = int16_t(int(v) * 16384 / peak);
    FILE* f = fopen(wav_path, "wb");
    if (!f) return false;
    uint32_t data = uint32_t(pcm.size() * 2);
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    put32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(2);
    put32(uint32_t(hz));
    put32(uint32_t(hz) * 4);
    put16(4);
    put16(16);
    fwrite("data", 1, 4, f);
    put32(data);
    bool ok = fwrite(pcm.data(), 2, pcm.size(), f) == pcm.size();
    ok = fclose(f) == 0 && ok;
    if (ok) printf("[dsb] track %d (%06X-%06X, %.0f s) written to %s\n", track, from, to, double(pcm.size() / 2) / hz, wav_path);
    else remove(wav_path);
    return ok;
}

uint8_t Dsb2::read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < program_.size()) return program_[a];
    if (a == 0xC00001) return latch_;
    if (a == 0xC00003) return 1;      // a command is waiting
    if (a == 0xE80001) return 0x01;   // decoder status: not busy
    if (a >= kRamBase && a < kRamBase + kRamSize) return ram_[a - kRamBase];
    return 0;
}

void Dsb2::write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= kRamBase && a < kRamBase + kRamSize) ram_[a - kRamBase] = v;
    else if (a == 0xE00003) decoder_byte(v);
}

void Dsb2::start(uint32_t from, uint32_t to) {
    pos_ = std::min<uint32_t>(from, uint32_t(mpeg_.size()));
    stop_ = std::min<uint32_t>(to, uint32_t(mpeg_.size()));
    looping_ = false;
    playing_ = pos_ < stop_;
    printf("[dsb] stream %06X-%06X%s\n", from, to, playing_ ? "" : " (empty)");
    mp3dec_init(static_cast<mp3dec_t*>(decoder_));
}

// The decoder's command bytes: 0x14/0x15 + 3 bytes start address (while
// playing: the loop that follows the stream), 0x24/0x25 + 3 bytes end
// address, 0x74/0x75 play, 0x84/0x85 stop, 0xB0/0xA1 + level: left/right
// volume, 0xA0/0xB1 + flag: one channel on both sides, 0xA4 0x75 play,
// 0xB4 0x96 stop; the other 0xAx/0xBx take one byte that is not used here
void Dsb2::decoder_byte(uint8_t b) {
    if (g_log) printf("[dsb] decoder %02X\n", b);
    switch (state_) {
    case 0:
        if (b == 0x14 || b == 0x15) state_ = 1;
        else if (b == 0x24 || b == 0x25) state_ = 4;
        else if (b == 0x74 || b == 0x75) start(start_, end_);
        else if (b == 0x84 || b == 0x85) playing_ = false;
        else if ((b & 0xE0) == 0xA0) state_ = 0x100 | b;   // one parameter byte follows
        break;
    case 1: start_ = (start_ & 0x00FFFF) | uint32_t(b) << 16, state_ = 2; break;
    case 2: start_ = (start_ & 0xFF00FF) | uint32_t(b) << 8, state_ = 3; break;
    case 3:
        start_ = (start_ & 0xFFFF00) | b;
        state_ = 0;
        if (playing_) {   // the loop to continue with when the stream ends
            looping_ = true;
            loop_start_ = start_;
            loop_end_ = end_;
        }
        break;
    case 4: end_ = (end_ & 0x00FFFF) | uint32_t(b) << 16, state_ = 5; break;
    case 5: end_ = (end_ & 0xFF00FF) | uint32_t(b) << 8, state_ = 6; break;
    case 6:
        end_ = (end_ & 0xFFFF00) | b;
        mono_ = 0;
        state_ = 0;
        break;
    default: {
        uint8_t cmd = uint8_t(state_ & 0xFF);
        state_ = 0;
        switch (cmd) {
        case 0xB0: case 0xB6: volume_[0] = b; break;
        case 0xA1: case 0xA7: volume_[1] = b; break;
        case 0xA0: mono_ = b ? 1 : 0; break;
        case 0xB1: mono_ = b ? 2 : 0; break;
        case 0xA4: if (b == 0x75) start(start_, end_); break;
        case 0xB4: if (b == 0x96) playing_ = false; break;
        default: break;
        }
    }
    }
}

bool Dsb2::decode_frame() {
    auto* dec = static_cast<mp3dec_t*>(decoder_);
    for (int tries = 0; tries < 2; tries++) {
        if (pos_ >= stop_) {
            if (!looping_ || loop_start_ >= loop_end_) return false;
            pos_ = loop_start_;
            stop_ = std::min<uint32_t>(loop_end_, uint32_t(mpeg_.size()));
        }
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info{};
        int n = mp3dec_decode_frame(dec, mpeg_.data() + pos_, int(stop_ - pos_), pcm, &info);
        if (info.frame_bytes == 0) {   // no more frames in the stream
            pos_ = stop_;
            continue;
        }
        pos_ += uint32_t(info.frame_bytes);
        if (n <= 0) return true;   // a skipped frame: still playing
        if (info.hz > 0) hz_ = info.hz;
        for (int i = 0; i < n; i++) {
            int16_t l = pcm[i * info.channels], r = info.channels > 1 ? pcm[i * info.channels + 1] : l;
            pcm_.push_back(l);
            pcm_.push_back(r);
        }
        return true;
    }
    return false;
}

void Dsb2::run(int samples, float* stereo, int stride, float scale) {
    if (!ready_ || samples <= 0) return;
    with_context(context_, this, [&] {
        while (!fifo_.empty()) {   // each command byte: interrupt 1, then time to read it
            latch_ = fifo_.front();
            if (g_log) printf("[dsb] command %02X pc %06X\n", latch_, m68k_get_reg(nullptr, M68K_REG_PC));
            fifo_.pop_front();
            m68k_set_irq(1);
            cycles_ += m68k_execute(500);
        }
        int64_t frame = int64_t(kClock) * samples / 44100;
        while (cycles_ < frame) {
            if (cycles_ >= next_timer_) {
                m68k_set_irq(2);
                next_timer_ += kTimerPeriod;
            }
            int run = int(std::max<int64_t>(1, std::min(next_timer_, frame) - cycles_));
            cycles_ += m68k_execute(run);
        }
        cycles_ -= frame;
        if (g_log && ++frames_ % 300 == 0) printf("[dsb] pc %06X sr %04X\n", m68k_get_reg(nullptr, M68K_REG_PC), m68k_get_reg(nullptr, M68K_REG_SR));
        next_timer_ -= frame;
    });

    // Music: resampled to 44.1 kHz, each side at its volume
    float vl = scale * float(volume_[0]) / 255.0f, vr = scale * float(volume_[1]) / 255.0f;
    if (mono_ == 1) vr = vl;
    if (mono_ == 2) vl = vr;
    double step = double(hz_) / 44100.0;
    for (int i = 0; i < samples; i++) {
        while (playing_ && pcm_.size() - pcm_read_ < 4)
            if (!decode_frame()) playing_ = false;
        if (pcm_.size() - pcm_read_ < 4) break;   // nothing (more) to play
        const int16_t* s = &pcm_[pcm_read_];
        float t = float(phase_);
        int li = mono_ == 2 ? 1 : 0, ri = mono_ == 1 ? 0 : 1;
        float l = float(s[li]) + (float(s[2 + li]) - float(s[li])) * t;
        float r = float(s[ri]) + (float(s[2 + ri]) - float(s[ri])) * t;
        stereo[size_t(i) * stride] += l * vl;
        stereo[size_t(i) * stride + 1] += r * vr;
        phase_ += step;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            pcm_read_ += 2;
        }
    }
    if (pcm_read_ > 1 << 16) {   // drop what was played
        pcm_.erase(pcm_.begin(), pcm_.begin() + std::ptrdiff_t(pcm_read_));
        pcm_read_ = 0;
    }
    if (!playing_ && pcm_.size() - pcm_read_ < 4) {
        pcm_.clear();
        pcm_read_ = 0;
    }
}
