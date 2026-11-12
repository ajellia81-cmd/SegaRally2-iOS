#include "scsi53c810.h"

#include <cstdio>

#include "ppc_rt.h"

namespace {
constexpr uint32_t ISTAT = 0x14, DSTAT = 0x0C, DSPS = 0x30, DSP = 0x2C;
constexpr uint8_t ISTAT_DIP = 0x01, ISTAT_INTF = 0x04, DSTAT_SIR = 0x04;

// SCRIPTS is stored little-endian (the chip sits on the PCI bus)
uint32_t fetch(uint32_t a) {
    return uint32_t(rd8(a)) | rd8(a + 1) << 8 | rd8(a + 2) << 16 | uint32_t(rd8(a + 3)) << 24;
}
}  // namespace

bool Ncr53c810::read8(uint32_t off, uint8_t& v) {
    switch (off) {
    case ISTAT: v = istat_; return true;
    case DSTAT:
        v = dstat_;
        dstat_ &= 0x80;            // reading DSTAT clears the DMA interrupt
        istat_ &= ~ISTAT_DIP;
        return true;
    case DSPS: case DSPS + 1: case DSPS + 2: case DSPS + 3:
        v = uint8_t(dsps_ >> (8 * (off - DSPS)));
        return true;
    }
    return false;
}

void Ncr53c810::wrote8(uint32_t off, uint8_t v, const uint8_t* regs) {
    if (off == ISTAT) {
        istat_ = (istat_ & ~ISTAT_INTF) | (v & ISTAT_INTF ? 0 : istat_ & ISTAT_INTF);
        return;
    }
    if (off == DSP + 3) {   // the last byte of DSP starts SCRIPTS
        uint32_t dsp = uint32_t(regs[DSP]) | regs[DSP + 1] << 8 | regs[DSP + 2] << 16 | uint32_t(regs[DSP + 3]) << 24;
        run(dsp);
    }
}

void Ncr53c810::run(uint32_t pc) {
    scripts_++;
    for (int steps = 0; steps < 100000; steps++) {
        uint32_t w0 = fetch(pc), w1 = fetch(pc + 4);
        switch (w0 >> 30) {
        case 3: {   // Memory Move: w0 = 0xC0000000 | bytes, w1 = source, w2 = destination
            uint32_t n = w0 & 0xFFFFFF, src = w1, dst = fetch(pc + 8);
            static int big = 0;
            if (n > 0x100000 && big++ < 3) {
                fprintf(stderr, "[53C810] large copy: %u bytes %08X -> %08X (pc ~%08X)\n", n, src, dst, g_ctx.pc_hint);
                rt_print_recent();
            }
            rt_dma_copy(dst, src, n);
            bytes_ += n;
            auto& d = by_dst_[dst & 0xFF000000];
            d.first++;
            d.second += n;
            auto& s = by_src_[src & 0xFF000000];
            s.first++;
            s.second += n;
            pc += 12;
            break;
        }
        case 2: {   // Transfer control (condition assumed true)
            uint32_t kind = (w0 >> 27) & 7;
            if (kind == 3) {   // INT
                dsps_ = w1;
                dstat_ |= DSTAT_SIR;
                istat_ |= ISTAT_DIP;
                return;
            }
            if (kind == 0 || kind == 1) { pc = w1; break; }   // JUMP / CALL
            fprintf(stderr, "[53C810] unsupported transfer %u at %08X\n", kind, pc);
            return;
        }
        default:
            fprintf(stderr, "[53C810] unsupported instruction %08X at %08X\n", w0, pc);
            dstat_ |= 0x01;   // IID: illegal instruction
            istat_ |= ISTAT_DIP;
            return;
        }
    }
}

void Ncr53c810::report() const {
    printf("\nDMA 53C810: %llu programs, %llu bytes copied\n", (unsigned long long)scripts_,
           (unsigned long long)bytes_);
    for (auto& [r, v] : by_src_)
        printf("  from %08X...  %llu copies, %llu bytes\n", r, (unsigned long long)v.first, (unsigned long long)v.second);
    for (auto& [r, v] : by_dst_)
        printf("  to   %08X...  %llu copies, %llu bytes\n", r, (unsigned long long)v.first, (unsigned long long)v.second);
}
