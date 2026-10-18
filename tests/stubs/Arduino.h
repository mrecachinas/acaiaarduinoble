#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using byte = uint8_t;
using std::abs;
constexpr int HEX = 16, HIGH = 1, LOW = 0, INPUT_PULLUP = 2, OUTPUT = 3;

class String : public std::string {
public:
    using std::string::string;
    String(const std::string& value) : std::string(value) {}
    String substring(size_t first, size_t last) const { return substr(first, last - first); }
};

namespace mock {
inline unsigned long now = 10000, serialDelay = 0;
inline int inputs[64] = {}, outputs[64] = {};
struct Event { std::string kind; unsigned long time; int pin; int value; };
inline std::vector<Event> events;
}

inline unsigned long millis() { return mock::now; }
inline void delay(unsigned long duration) { mock::now += duration; }
inline int digitalRead(int pin) { return mock::inputs[pin]; }
inline void digitalWrite(int pin, int value) {
    mock::outputs[pin] = value;
    mock::events.push_back({"gpio", millis(), pin, value});
}
inline void analogWrite(int, int) {}
inline void pinMode(int, int) {}
inline void setCpuFrequencyMhz(int) {}

struct SerialMock {
    std::string output;
    void begin(int) {}
    template<class T> void print(const T& value) {
        std::ostringstream text;
        text << value;
        output += text.str();
        mock::now += mock::serialDelay;
    }
    template<class T> void print(const T& value, int) { print(value); }
    template<class... T> void println(const T&... values) {
        (print(values), ...);
        output += '\n';
    }
    explicit operator bool() const { return true; }
};
inline SerialMock Serial;
