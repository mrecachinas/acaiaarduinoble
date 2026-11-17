#pragma once
#include "host.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <type_traits>

using byte = uint8_t;
using std::abs;
constexpr int HEX = 16, HIGH = 1, LOW = 0, INPUT_PULLUP = 2, OUTPUT = 3;
class String : public std::string {
public:
    using std::string::string;
    String(const std::string& value) : std::string(value) {}
    String substring(size_t first, size_t last) const { return substr(first, last - first); }
};
inline unsigned long millis() { return host::millis(); }
inline void delay(unsigned long duration) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    host::checkStop();
}
inline int digitalRead(int pin) {
    if (pin != host::inputPin) throw std::runtime_error("Unsupported virtual input pin");
    return host::paddle ? LOW : HIGH;
}
inline void pinMode(int pin, int mode) {
    if (pin < 0 || pin >= 64) throw std::runtime_error("Invalid virtual pin");
    if (mode == OUTPUT) host::pins[pin] = LOW;
}
inline void digitalWrite(int pin, int value) {
    if (pin < 0 || pin >= 64) throw std::runtime_error("Invalid virtual pin");
    host::pins[pin] = value;
    host::emit("gpio", std::to_string(pin) + "\t" + std::to_string(value));
}
inline void analogWrite(int pin, int value) {
    if (pin < 0 || pin >= 64) throw std::runtime_error("Invalid virtual pin");
    if (host::pins[pin] != value) {
        host::pins[pin] = value;
        host::emit("led", std::to_string(pin) + "\t" + std::to_string(value));
    }
}
inline void setCpuFrequencyMhz(int) {}
struct SerialPort {
    std::string line;
    void begin(int) {}
    template<class T> void print(const T& value) {
        std::ostringstream text;
        if constexpr (std::is_floating_point_v<T>) text << std::fixed << std::setprecision(2);
        if constexpr (std::is_same_v<T, uint8_t>) text << static_cast<unsigned int>(value);
        else text << value;
        line += text.str();
    }
    template<class T> void print(const T& value, int base) {
        std::ostringstream text;
        if (base == HEX) text << std::hex;
        text << static_cast<unsigned long>(value);
        line += text.str();
    }
    void println() { host::emit("firmware", line); line.clear(); }
    template<class T> void println(const T& value) { print(value); println(); }
    template<class T> void println(const T& value, int base) { print(value, base); println(); }
};
inline SerialPort Serial;
