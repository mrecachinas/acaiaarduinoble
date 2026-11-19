#pragma once
#include "Arduino.h"
#include <deque>
#include <functional>
#include <memory>

constexpr int BLEUpdated = 1, BLEWrite = 8, BLERead = 2;
class BLEDevice;
class BLECharacteristic;
class BLEService;
using Handler = void (*)(BLEDevice, BLECharacteristic);

namespace mock {
struct Write { unsigned long time; std::vector<uint8_t> bytes; };
inline std::vector<Write> writes;
inline bool writeSuccess = true;
}

class BLEDescriptor {
public:
    const char* uuid() const { return ""; }
    bool read() { return true; }
    const uint8_t* value() const { return nullptr; }
    int valueLength() const { return 0; }
};

class BLECharacteristic {
    struct State {
        std::vector<uint8_t> value;
        bool updated = false;
        Handler handler = nullptr;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
public:
    bool canSubscribe() const { return true; }
    bool subscribe() { return true; }
    bool canRead() const { return true; }
    bool read() { return true; }
    bool valueUpdated() {
        bool updated = state->updated;
        state->updated = false;
        return updated;
    }
    const uint8_t* value() const { return state->value.data(); }
    int valueLength() const { return static_cast<int>(state->value.size()); }
    int readValue(uint8_t* out, int length) {
        int count = std::min(length, valueLength());
        std::memcpy(out, value(), count);
        return count;
    }
    int writeValue(const uint8_t* data, int length, bool = true) {
        mock::writes.push_back({millis(), {data, data + length}});
        mock::events.push_back({"write", millis(), 0, 0});
        return mock::writeSuccess;
    }
    void setEventHandler(int, Handler handler) { state->handler = handler; }
    const char* uuid() const { return ""; }
    int properties() const { return 0x18; }
    int descriptorCount() const { return 0; }
    BLEDescriptor descriptor(int) { return {}; }
    void notify(const std::vector<uint8_t>& bytes);
};

class BLEByteCharacteristic {
    uint8_t stored = 0;
public:
    BLEByteCharacteristic(const char*, int) {}
    void writeValue(uint8_t value) { stored = value; }
    uint8_t value() const { return stored; }
    bool written() { return false; }
};

class BLEService {
public:
    BLEService(const char* = "") {}
    const char* uuid() const { return ""; }
    int characteristicCount() const { return 0; }
    BLECharacteristic characteristic(int) { return {}; }
    void addCharacteristic(BLEByteCharacteristic&) {}
};

class BLEDevice {
public:
    explicit operator bool() const { return false; }
    String address() const { return ""; }
    String localName() const { return ""; }
    String advertisedServiceUuid() const { return ""; }
    String deviceName() const { return ""; }
    int appearance() const { return 0; }
    bool connect() { return false; }
    bool discoverAttributes() { return false; }
    void disconnect() {}
    int serviceCount() const { return 0; }
    BLEService service(int) { return {}; }
    BLECharacteristic characteristic(const char*) { return {}; }
};

inline void BLECharacteristic::notify(const std::vector<uint8_t>& bytes) {
    state->value = bytes;
    state->updated = true;
    if (state->handler) state->handler(BLEDevice{}, *this);
}

struct BLEMock {
    unsigned long pollStep = 1;
    std::function<void()> onScan;
    struct Notification { BLECharacteristic characteristic; std::vector<uint8_t> bytes; };
    std::deque<Notification> pending;
    void poll() {
        mock::now += pollStep;
        while (!pending.empty()) {
            auto next = pending.front();
            pending.pop_front();
            next.characteristic.notify(next.bytes);
        }
    }
    bool begin() { return true; }
    void scan() {
        mock::events.push_back({"scan", millis(), 0, 0});
        if (onScan) onScan();
    }
    bool scanForAddress(String) { scan(); return false; }
    void stopScan() {}
    BLEDevice available() { ++mock::now; return {}; }
    void disconnect() { mock::events.push_back({"disconnect", millis(), 0, 0}); }
    void setLocalName(const char*) {}
    void setAdvertisedService(BLEService&) {}
    void addService(BLEService&) {}
    void advertise() {}
};
inline BLEMock BLE;
