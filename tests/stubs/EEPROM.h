#pragma once
#include "Arduino.h"
struct EEPROMMock {
    bool begin(int) { return true; }
    uint8_t read(int) { return 0; }
    void write(int, uint8_t) {}
    void commit() {}
};
inline EEPROMMock EEPROM;
