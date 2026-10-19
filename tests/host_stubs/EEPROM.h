#ifndef DIAGNOSTICS_TEST_EEPROM_H
#define DIAGNOSTICS_TEST_EEPROM_H
#include <stdint.h>
struct EEPROMStub {
  void begin(int) {}
  uint8_t read(int address) { return address == 0 ? 36 : 15; }
  void write(int, uint8_t) { ++writes; }
  void commit() { ++commits; }
  unsigned writes = 0;
  unsigned commits = 0;
};
extern EEPROMStub EEPROM;
#endif
