// Executes the real sketch; the scale, GPIO, EEPROM and BLE transport are boundaries.
// The BLE recorder deliberately does NOT simulate stack CCCD/notification behavior.
#include "../AcaiaArduinoBLE.h"
#include <EEPROM.h>
#include <assert.h>

uint32_t hostMillis = 0;
unsigned hostOutputWrites = 0;
SerialStub Serial;
BLEStub BLE;
EEPROMStub EEPROM;
static unsigned scaleControlCalls = 0;
static bool hostScaleConnected = true;
static bool hostWeightValid = true;
static float hostWeight = 42.3f;
static uint32_t hostWeightAge = 100;

AcaiaArduinoBLE::AcaiaArduinoBLE(bool) {}
bool AcaiaArduinoBLE::init(String) { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::tare() { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::startTimer() { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::stopTimer() { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::resetTimer() { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::tareStartTimer() { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::beep() { ++scaleControlCalls; return true; }
bool AcaiaArduinoBLE::heartbeat() { ++scaleControlCalls; return true; }
float AcaiaArduinoBLE::getWeight() { return hostWeight; }
bool AcaiaArduinoBLE::heartbeatRequired() { return false; }
bool AcaiaArduinoBLE::isConnected() { return hostScaleConnected; }
bool AcaiaArduinoBLE::newWeightAvailable() { return false; }
bool AcaiaArduinoBLE::hasWeightSample() const { return hostWeightValid; }
uint32_t AcaiaArduinoBLE::weightAgeMs() const { return hostWeightAge; }
void AcaiaArduinoBLE::setDiagnosticsCallback(DiagnosticsCallback) {}
bool AcaiaArduinoBLE::isScaleDevice(BLEDevice device) const {
  return device.address() == "scale";
}

struct Shot;
float seconds_f();
void setColor(int*);
void setBrewingState(bool);
void calculateEndTime(Shot*);
void scaleConnected_task();
#define ARDUINO_ESP32S3_DEV
#include "../examples/shotStopper/shotStopper.ino"

int main() {
  setup();
  assert(BLE.localName == "shotStopper");
  assert(BLE.advertised == &weightService);
  assert(BLE.services.size() == 2);
  assert(diagnosticsService.uuid == "7b183000-6e2d-4a9b-9c51-4c5344470001");
  assert(diagnosticsStatus.uuid == "7b183001-6e2d-4a9b-9c51-4c5344470001");
  assert(diagnosticsMetadata.uuid == "7b183002-6e2d-4a9b-9c51-4c5344470001");
  assert(diagnosticsHistory.uuid == "7b183003-6e2d-4a9b-9c51-4c5344470001");
  assert(diagnosticsService.characteristics.size() == 3);
  assert(weightCharacteristic.properties == (BLERead | BLEWrite));
  assert(diagnosticsStatus.properties == BLERead);
  assert(diagnosticsMetadata.properties == BLERead);
  assert(diagnosticsHistory.properties == BLERead);
  assert(diagnosticsStatus.fixed && diagnosticsStatus.size == 20);
  assert(diagnosticsMetadata.fixed && diagnosticsMetadata.size == 20);
  assert(diagnosticsHistory.fixed && diagnosticsHistory.size == 496);
  assert(diagnosticEvents.count() == 1);
  assert(diagnosticEvents.eventOldestFirst(0).kind == ShotStopperDiagnostics::kBoot);
  assert(diagnosticsStatus.writes == 1 && diagnosticsStatus.data.size() == 20);
  assert(diagnosticsStatus.data[0] == 1 && diagnosticsStatus.data[1] == 1);
  assert(diagnosticsMetadata.writes == 1 && diagnosticsMetadata.data.size() == 20);
  assert(diagnosticsMetadata.data[0] == 1 && diagnosticsMetadata.data[1] == 2);
  assert(diagnosticsHistory.writes == 1 && diagnosticsHistory.data.size() == 496);
  assert(diagnosticsHistory.data[0] == 1 && diagnosticsHistory.data[1] == 3);
  currentWeight = hostWeight;
  const unsigned gpioBefore = hostOutputWrites;
  refreshDiagnostics();
  assert(diagnosticsStatus.writes == 1);
  assert(diagnosticsHistory.writes == 1 && diagnosticsMetadata.writes == 1);
  assert(diagnosticsStatus.data[2] == 3);
  for (hostMillis = 1; hostMillis < 500; ++hostMillis) refreshDiagnostics();
  assert(diagnosticsStatus.writes == 1 && diagnosticsHistory.writes == 1);
  hostMillis = 500;
  refreshDiagnostics();
  assert(diagnosticsStatus.writes == 2);
  assert(diagnosticsHistory.writes == 1 && diagnosticsMetadata.writes == 1);
  appendDiagnosticEvent(ShotStopperDiagnostics::kShotStart, 0);
  refreshDiagnostics();
  assert(diagnosticsHistory.writes == 2 && diagnosticsMetadata.writes == 2);
  assert(diagnosticsStatus.writes == 2);
  appendDiagnosticEvent(ShotStopperDiagnostics::kShotEnd, 1);
  hostMillis = 999;
  refreshDiagnostics();
  assert(diagnosticsHistory.writes == 2 && diagnosticsStatus.writes == 2);
  hostMillis = 1000;
  refreshDiagnostics();
  assert(diagnosticsHistory.writes == 3 && diagnosticsStatus.writes == 3);

  const uint32_t nextBefore = diagnosticEvents.nextSequence();
  const auto retained = diagnosticsHistory.data;
  shot.brewing = true;
  buttonLatched = true;
  buttonPressed = true;
  assert(BLE.disconnected != nullptr);
  BLE.disconnected(BLEDevice("debugger"));
  assert(diagnosticEvents.nextSequence() == nextBefore);
  assert(diagnosticsHistory.data == retained);
  assert(hostScaleConnected && shot.brewing && buttonLatched && buttonPressed);
  assert(scaleControlCalls == 0 && BLE.disconnectCalls == 0);
  assert(EEPROM.writes == 0 && EEPROM.commits == 0);
  assert(hostOutputWrites == gpioBefore);
  shot.brewing = false;
  buttonLatched = false;
  buttonPressed = false;
  BLE.disconnected(BLEDevice("scale"));
  assert(diagnosticEvents.nextSequence() == nextBefore + 1);
  assert(diagnosticEvents.eventOldestFirst(diagnosticEvents.count() - 1).kind ==
         ShotStopperDiagnostics::kDisconnected);
  assert(diagnosticEvents.eventOldestFirst(diagnosticEvents.count() - 1).valueDecigrams == 423);
  onScaleDiagnostics(ShotStopperDiagnostics::kConnectionReady, 2);
  assert(diagnosticEvents.eventOldestFirst(diagnosticEvents.count() - 1).valueDecigrams == 423);
  hostMillis = 0xfffffff0UL;
  refreshDiagnostics();
  // Physical disconnect observation does not rewrite library-connected state.
  assert(diagnosticsStatus.data[2] == 3);
  const unsigned beforeWrap = diagnosticsStatus.writes;
  hostMillis = 0x1e3;
  refreshDiagnostics();
  assert(diagnosticsStatus.writes == beforeWrap);
  hostMillis = 0x1e4;
  refreshDiagnostics();
  assert(diagnosticsStatus.writes == beforeWrap + 1);
  assert(scaleControlCalls == 0 && BLE.disconnectCalls == 0);
  return 0;
}
