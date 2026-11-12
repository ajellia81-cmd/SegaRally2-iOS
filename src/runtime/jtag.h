// JTAG scan chain of the Real3D board, driven through system registers 0x0C
// (write: TCK bit 6, TDI bit 5, TMS bit 2, TRST bit 7, active low) and 0x10
// (read: TDO in bit 5). Step 2.1 boards check the ASICs' IDCODEs at boot
// (error 0x12 on the LEDs when they do not match). Modelled after the
// behaviour of Supermodel's Src/Model3/JTAG.cpp: a standard TAP controller and
// 17 devices (9 ASICs, 8 3D-RAMs), each with IDCODE, MODEWORD and BYPASS.
// Polygon memory reads through Venus are not modelled (they read as BYPASS).
#pragma once
#include <cstdint>
#include <vector>

class Jtag {
public:
    Jtag();
    void write(uint8_t reg0c);           // a write to system register 0x0C
    uint8_t read() const { return uint8_t(tdo_ << 5); }   // system register 0x10
    uint32_t modeword(int asic) const;   // last MODEWORD written to that ASIC (0 Jupiter ... 8 Mars11)

private:
    struct Device {
        bool asic;               // ASIC (5-bit IR) or 3D-RAM (4-bit IR)
        uint32_t idcode;
        uint32_t modeword = 0;
        uint8_t ir = 0;
        uint64_t shift = 0;
        int size = 1;
        void reset();
        void capture_dr();
        void capture_ir();
        void update_dr();
        void update_ir() { ir = uint8_t(shift); }
        bool shift_bit(bool tdi);
    };
    void rising(bool tms, bool tdi);
    void falling();

    std::vector<Device> chain_;   // TDI enters chain_[0], TDO leaves chain_.back()
    int state_ = 0;               // TAP state, 0 = Test-Logic-Reset
    bool last_tck_ = false, tdo_ = false;
};
