#pragma once
#include "Arduino.h"

constexpr int BLEUpdated = 1, BLERead = 2, BLEWriteWithoutResponse = 4, BLEWrite = 8;
class BLEDevice;
class BLECharacteristic;
class BLEService;
using Handler = void (*)(BLEDevice, BLECharacteristic);

// Match the library's existing private-property access shim without modifying firmware.
struct HostRemoteFields {
    void* vtable = nullptr;
    String uuid;
    int refCount = 0;
    uint16_t connectionHandle = 0, startHandle = 0;
    uint8_t properties = 0;
};
struct CharacteristicState {
    HostRemoteFields fields;
    std::vector<uint8_t> value;
    bool updated = false;
    Handler handler = nullptr;
};
inline std::map<std::string, std::weak_ptr<CharacteristicState>> characteristics;

class BLEDescriptor {
    std::string id;
    int handle = 0;
    std::vector<uint8_t> bytes;
public:
    BLEDescriptor(const std::string& entry) {
        auto colon = entry.find(':');
        handle = std::stoi(entry.substr(0, colon));
        id = entry.substr(colon + 1);
    }
    const char* uuid() const { return id.c_str(); }
    bool read() {
        auto response = host::call("READ_DESCRIPTOR", std::to_string(handle));
        if (response.ok) bytes = host::unhex(response.text);
        return response.ok;
    }
    const uint8_t* value() const { return bytes.data(); }
    int valueLength() const { return static_cast<int>(bytes.size()); }
};

class BLECharacteristic {
public:
    void* vtable = nullptr;
    void* local = nullptr;
    HostRemoteFields* remote = nullptr;
    std::shared_ptr<CharacteristicState> state;
    std::vector<std::string> descriptors;

    BLECharacteristic() : BLECharacteristic(std::make_shared<CharacteristicState>()) {}
    explicit BLECharacteristic(std::shared_ptr<CharacteristicState> value)
        : remote(&value->fields), state(std::move(value)) {}
    const char* uuid() const { return state->fields.uuid.c_str(); }
    int properties() const { return state->fields.properties; }
    bool canSubscribe() const { return properties() & (16 | 32); }
    bool canRead() const { return properties() & BLERead; }
    bool subscribe() { return host::call("SUBSCRIBE", uuid()).ok; }
    void setEventHandler(int, Handler handler) { state->handler = handler; }
    const uint8_t* value() const { return state->value.data(); }
    int valueLength() const { return static_cast<int>(state->value.size()); }
    bool valueUpdated() {
        host::poll();
        bool updated = state->updated;
        state->updated = false;
        return updated;
    }
    bool read() {
        auto response = host::call("READ", uuid());
        if (response.ok) state->value = host::unhex(response.text);
        return response.ok;
    }
    int readValue(uint8_t* out, int length) {
        int count = std::min(length, valueLength());
        if (count) std::memcpy(out, value(), count);
        return count;
    }
    int writeValue(const uint8_t* data, int length, bool response = true) {
        bool withResponse = response && (properties() & BLEWrite);
        if (!withResponse && !(properties() & BLEWriteWithoutResponse)) {
            host::emit("transport_error", "Characteristic is not writable");
            return 0;
        }
        return host::call("WRITE", std::string(uuid()) + "\t" +
                          (withResponse ? "1" : "0") + "\t" + host::hex(data, length)).ok;
    }
    int descriptorCount() {
        auto response = host::call("DESCRIPTORS", uuid());
        if (!response.ok) return 0;
        descriptors = host::split(response.text);
        return static_cast<int>(descriptors.size());
    }
    BLEDescriptor descriptor(int index) { return BLEDescriptor(descriptors.at(index)); }
};

inline BLECharacteristic remoteCharacteristic(const std::string& id) {
    auto normalized = host::uuid(id);
    if (auto existing = characteristics[normalized].lock()) return BLECharacteristic(existing);
    auto response = host::call("CHAR", normalized);
    auto state = std::make_shared<CharacteristicState>();
    state->fields.uuid = normalized;
    state->fields.properties = response.ok ? std::stoi(response.text) : 0;
    characteristics[normalized] = state;
    return BLECharacteristic(state);
}

class BLEByteCharacteristic {
    uint8_t stored = 0;
    bool dirty = false;
public:
    BLEByteCharacteristic(const char*, int) {}
    void writeValue(uint8_t value) { stored = value; }
    void writeFromCentral(uint8_t value) { stored = value; dirty = true; }
    uint8_t value() const { return stored; }
    bool written() { bool result = dirty; dirty = false; return result; }
};
class BLEService {
    std::string id;
    std::vector<std::string> chars;
public:
    explicit BLEService(const char* value) : id(value) {}
    const char* uuid() const { return id.c_str(); }
    void addCharacteristic(BLEByteCharacteristic&) {}
    int characteristicCount() {
        auto response = host::call("CHARACTERS", id);
        if (!response.ok) return 0;
        chars = host::split(response.text);
        return static_cast<int>(chars.size());
    }
    BLECharacteristic characteristic(int index) { return remoteCharacteristic(chars.at(index)); }
};
class BLEDevice {
    std::string id, name;
    std::vector<std::string> services;
public:
    BLEDevice(std::string address = "", std::string localName = "")
        : id(std::move(address)), name(std::move(localName)) {}
    explicit operator bool() const { return !id.empty(); }
    String address() const { return id; }
    String localName() const { return name; }
    String deviceName() const { return name; }
    String advertisedServiceUuid() const { return ""; }
    int appearance() const { return 0; }
    bool connect() {
        auto result = host::call("CONNECT", id);
        if (result.ok) {
            characteristics.clear();
            std::lock_guard<std::mutex> guard(host::notificationMutex);
            host::notifications.clear();
        }
        return result.ok;
    }
    bool discoverAttributes() { return host::call("DISCOVER").ok; }
    void disconnect() { host::call("DISCONNECT"); }
    BLECharacteristic characteristic(const char* uuid) { return remoteCharacteristic(uuid); }
    int serviceCount() {
        auto response = host::call("SERVICES");
        if (!response.ok) return 0;
        services = host::split(response.text);
        return static_cast<int>(services.size());
    }
    BLEService service(int index) { return BLEService(services.at(index).c_str()); }
};
struct BLEAdapter {
    bool begin() {
        host::deliver = [](const host::Notification& notification) {
            auto it = characteristics.find(host::uuid(notification.uuid));
            if (it == characteristics.end()) return;
            if (auto state = it->second.lock()) {
                state->value = notification.bytes;
                state->updated = true;
                if (state->handler) state->handler(BLEDevice{}, BLECharacteristic(state));
            }
        };
        return true;
    }
    void poll() { host::poll(); }
    void scan() { host::call("SCAN"); }
    bool scanForAddress(String address) { return host::call("SCAN", address).ok; }
    void stopScan() { host::call("STOPSCAN"); }
    BLEDevice available() {
        auto response = host::call("AVAILABLE");
        if (!response.ok || response.text.empty()) return {};
        auto fields = host::split(response.text);
        if (fields.size() != 2) throw std::runtime_error("Invalid advertisement response");
        return BLEDevice(host::unhexText(fields[0]), host::unhexText(fields[1]));
    }
    void disconnect() { host::call("DISCONNECT"); }
    void setLocalName(const char*) {}
    void setAdvertisedService(BLEService&) {}
    void addService(BLEService&) {}
    void advertise() {
        host::emit("adapter", "Goal-weight BLE server is virtual; use the goal command");
    }
};
inline BLEAdapter BLE;
