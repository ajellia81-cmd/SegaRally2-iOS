// 93C46 serial EEPROM (64 16-bit words) where the board stores the
// operator settings. The game drives it bit by bit at 0xF0040000:
//   write: 0x80 = CLK, 0x40 = CS, 0x20 = DI
//   read:  bit 0x20 of the byte at 0xF0040004 = DO
#pragma once
#include <cstdint>
#include <string>

class Eeprom93C46 {
public:
    void load(const std::string& path, bool read_only = false);   // read_only: changes are not saved
    void write_pins(uint8_t v);   // byte written to 0xF0040000
    uint8_t read_do();            // 0 or 1

private:
    enum class State { Idle, Command, Read, WriteData };
    void execute();
    void save();

    uint16_t mem_[64] = {};
    std::string path_;
    bool cs_ = false, clk_ = false, do_ = true, write_enabled_ = false;
    State state_ = State::Idle;
    uint32_t shift_ = 0;
    int bits_ = 0;
    int addr_ = 0, op_ = 0;
    int busy_reads_ = 0;
};
