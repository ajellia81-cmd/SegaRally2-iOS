#include "jtag.h"

namespace {
enum State { TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PAU_DR, EX2_DR, UPD_DR,
             SEL_IR, CAP_IR, SH_IR, EX1_IR, PAU_IR, EX2_IR, UPD_IR };
// Next state for TMS = 0 and TMS = 1 (IEEE 1149.1)
const int NEXT[16][2] = {
    {RTI, TLR}, {RTI, SEL_DR}, {CAP_DR, SEL_IR}, {SH_DR, EX1_DR}, {SH_DR, EX1_DR}, {PAU_DR, UPD_DR},
    {PAU_DR, EX2_DR}, {SH_DR, UPD_DR}, {RTI, SEL_DR}, {CAP_IR, TLR}, {SH_IR, EX1_IR}, {SH_IR, EX1_IR},
    {PAU_IR, UPD_IR}, {PAU_IR, EX2_IR}, {SH_IR, UPD_IR}, {RTI, SEL_DR}};

// Step 2.1 IDCODEs
constexpr uint32_t MERCURY = 0x416C3057, VENUS = 0x316C4057, EARTH = 0x516C5057, MARS = 0x316C6057,
                   JUPITER = 0x516C7057;
}  // namespace

Jtag::Jtag() {
    // Order on Step 2.x video boards, from TDI to TDO; 0 = 3D-RAM
    const uint32_t ids[17] = {JUPITER, MERCURY, VENUS, EARTH, 0, MARS, MARS, 0, 0,
                              0, EARTH, 0, MARS, MARS, 0, 0, 0};
    for (uint32_t id : ids) chain_.push_back(Device{id != 0, id});
    for (Device& d : chain_) d.reset();
}

uint32_t Jtag::modeword(int asic) const {
    int n = 0;
    for (const Device& d : chain_)
        if (d.asic && n++ == asic) return d.modeword;
    return 0;
}

void Jtag::Device::reset() {
    if (asic) { ir = 3; size = 32; shift = idcode; }   // IDCODE
    else { ir = 15; size = 1; shift = 0; }            // BYPASS
}

void Jtag::Device::capture_dr() {
    if (asic && ir == 3) { size = 32; shift = idcode; return; }
    if (asic && ir == 8) { size = 32; shift = modeword; return; }
    if (asic && ir == 9 && idcode == MERCURY) { size = 57; shift = 0; return; }   // memory configuration
    size = 1;
    shift = 0;
}

void Jtag::Device::capture_ir() {
    size = asic ? 5 : 4;
    shift = asic ? 1 : 0b1001;
}

void Jtag::Device::update_dr() {
    if (asic && ir == 8) modeword = uint32_t(shift);
}

bool Jtag::Device::shift_bit(bool tdi) {
    bool out = shift & 1;
    shift >>= 1;
    if (tdi) shift |= 1ull << (size - 1);
    return out;
}

void Jtag::rising(bool tms, bool tdi) {
    switch (state_) {
    case TLR: for (Device& d : chain_) d.reset(); break;
    case CAP_DR: for (Device& d : chain_) d.capture_dr(); break;
    case CAP_IR: for (Device& d : chain_) d.capture_ir(); break;
    case SH_DR:
    case SH_IR: for (Device& d : chain_) tdi = d.shift_bit(tdi); break;
    default: break;
    }
    state_ = NEXT[state_][tms ? 1 : 0];
}

void Jtag::falling() {
    switch (state_) {
    case SH_DR:
    case SH_IR: tdo_ = chain_.back().shift & 1; break;
    case UPD_DR: for (Device& d : chain_) d.update_dr(); break;
    case UPD_IR: for (Device& d : chain_) d.update_ir(); break;
    default: break;
    }
}

void Jtag::write(uint8_t v) {
    bool tck = v & 0x40, tms = v & 0x04, tdi = v & 0x20, trst = v & 0x80;
    if (!trst) {   // TRST (active low): the devices go back to IDCODE / BYPASS
        for (Device& d : chain_) d.reset();
        return;
    }
    if (tck == last_tck_) return;
    last_tck_ = tck;
    if (tck) rising(tms, tdi);
    else falling();
}
