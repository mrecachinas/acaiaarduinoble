#ifndef DIAGNOSTICS_TEST_ARDUINOBLE_H
#define DIAGNOSTICS_TEST_ARDUINOBLE_H
#include "Arduino.h"
#include <vector>
enum { BLERead = 2, BLEWrite = 8, BLENotify = 16, BLEDisconnected = 1 };
class BLEDevice {
 public:
  explicit BLEDevice(String id = "") : id(id) {}
  String address() const { return id; }
 private:
  String id;
};
class BLEDescriptor {};
class BLECharacteristic {
 public:
  BLECharacteristic(const char* uuid = "", unsigned properties = 0,
                    int size = 0, bool fixed = false)
      : uuid(uuid), properties(properties), size(size), fixed(fixed), writes(0) {}
  int writeValue(const uint8_t* bytes, int length) {
    ++writes;
    data.assign(bytes, bytes + length);
    return 1;
  }
  String uuid;
  unsigned properties;
  int size;
  bool fixed;
  unsigned writes;
  std::vector<uint8_t> data;
};
class BLEByteCharacteristic : public BLECharacteristic {
 public:
  BLEByteCharacteristic(const char* uuid, unsigned properties)
      : BLECharacteristic(uuid, properties, 1, true) {}
  int writeValue(uint8_t byte) { return BLECharacteristic::writeValue(&byte, 1); }
  bool written() { return false; }
  uint8_t value() { return data.empty() ? 0 : data[0]; }
};
class BLEService {
 public:
  explicit BLEService(const char* uuid) : uuid(uuid) {}
  void addCharacteristic(BLECharacteristic& characteristic) {
    characteristics.push_back(&characteristic);
  }
  String uuid;
  std::vector<BLECharacteristic*> characteristics;
};
struct BLEStub {
  int begin() { return 1; }
  void setEventHandler(int, void (*callback)(BLEDevice)) { disconnected = callback; }
  void setLocalName(const char* name) { localName = name; }
  void setAdvertisedService(BLEService& service) { advertised = &service; }
  void addService(BLEService& service) { services.push_back(&service); }
  void advertise() {}
  void poll() {}
  void disconnect() { ++disconnectCalls; }
  String localName;
  BLEService* advertised = nullptr;
  std::vector<BLEService*> services;
  void (*disconnected)(BLEDevice) = nullptr;
  unsigned disconnectCalls = 0;
};
extern BLEStub BLE;
#endif
