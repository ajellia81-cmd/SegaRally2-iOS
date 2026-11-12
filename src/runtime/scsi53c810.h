// NCR 53C810. On the Model 3 Step 1.x it is not used for SCSI: the game writes
// small SCRIPTS programs (Memory Move + INT) to it to copy blocks between
// RAM and the Real3D. Registers at 0xC1000000 (little-endian, PCI).
#pragma once
#include <cstdint>
#include <map>

class Ncr53c810 {
public:
    // Return true if the address is a register with side effects
    bool read8(uint32_t off, uint8_t& v);
    void wrote8(uint32_t off, uint8_t v, const uint8_t* regs);   // regs = bytes already written

    void report() const;

private:
    void run(uint32_t dsp);

    uint8_t istat_ = 0, dstat_ = 0x80;   // DSTAT.DFE (FIFO empty)
    uint32_t dsps_ = 0;
    uint64_t scripts_ = 0, bytes_ = 0;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> by_dst_;   // destination (16 MB) -> (count, bytes)
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> by_src_;
};
