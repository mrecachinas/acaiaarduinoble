#ifndef DIAGNOSTICS_TEST_ARDUINO_H
#define DIAGNOSTICS_TEST_ARDUINO_H
#include <stdint.h>
#include <string>
using String = std::string;
extern uint32_t hostMillis;
extern unsigned hostOutputWrites;
inline unsigned long millis() { return hostMillis; }
inline void delay(unsigned long ms) { hostMillis += ms; }
inline void setCpuFrequencyMhz(int) {}
inline void pinMode(int, int) {}
inline void analogWrite(int, int) { ++hostOutputWrites; }
inline void digitalWrite(int, int) { ++hostOutputWrites; }
inline int digitalRead(int) { return 1; }
enum { INPUT_PULLUP, OUTPUT, LOW, HIGH };
struct SerialStub {
  void begin(int) {}
  template <typename T> void print(const T&) {}
  template <typename T> void println(const T&) {}
  void println() {}
};
extern SerialStub Serial;
#endif
