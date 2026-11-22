#pragma once
#include "Arduino.h"
struct EEPROMMock {
    int writes = 0, commits = 0;
    bool begin(int) { return true; }
    uint8_t read(int) { return 0; }
    void write(int, uint8_t) { ++writes; }
    void commit() { ++commits; }
};
inline EEPROMMock EEPROM;
