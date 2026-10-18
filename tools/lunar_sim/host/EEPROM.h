#pragma once
#include "Arduino.h"
struct VirtualEEPROM {
    std::vector<uint8_t> bytes;
    bool begin(int size) { bytes.assign(size, 255); return true; }
    uint8_t read(int address) { return bytes.at(address); }
    void write(int address, uint8_t value) { bytes.at(address) = value; }
    bool commit() { host::emit("settings", "virtual EEPROM committed"); return true; }
};
inline VirtualEEPROM EEPROM;
