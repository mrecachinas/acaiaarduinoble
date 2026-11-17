#include "Arduino.h"
#include "ArduinoBLE.h"
#include "EEPROM.h"
#include <cstddef>

#define private public
#include "AcaiaArduinoBLE.h"
#undef private
#include "AcaiaArduinoBLE.cpp"

struct Shot;
void setColor(int[3]);
void scaleConnected_task();
void setBrewingState(bool);
void calculateEndTime(Shot*);
float seconds_f();
float shotElapsed_s();
#include SHOTSTOPPER_SKETCH

static_assert(!MOMENTARY && !REEDSWITCH && !TIMER_ONLY,
              "The live runner currently supports the direct-input Micra configuration");
// Releases before the private-property workaround do not declare these types.
struct CharAccess;
struct RemoteCharAccess;
template<class T, class = void> struct Complete : std::false_type {};
template<class T> struct Complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};
template<class Access, class Remote> constexpr bool compatibleLayout() {
    static_assert(Complete<Access>::value == Complete<Remote>::value,
                  "Unsupported partial characteristic-layout workaround");
    if constexpr (Complete<Access>::value && Complete<Remote>::value) {
        return offsetof(BLECharacteristic, remote) == offsetof(Access, remote)
            && offsetof(HostRemoteFields, properties) == offsetof(Remote, properties)
            && sizeof(HostRemoteFields) == sizeof(Remote);
    }
    return true;
}
static_assert(compatibleLayout<CharAccess, RemoteCharAccess>(),
              "The native adapter does not match the firmware's characteristic-layout workaround");

#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
bool initialized = false;
std::string number(float value) { return std::isfinite(value) ? std::to_string(value) : "null"; }
void publish() {
    std::ostringstream out;
    out << "{\"now_ms\":" << millis()
        << ",\"connected\":" << (scale.isConnected() ? "true" : "false")
        << ",\"brewing\":" << (shot.brewing ? "true" : "false")
        << ",\"latched\":" << (buttonLatched ? "true" : "false")
        << ",\"paddle\":" << (host::paddle ? "true" : "false")
        << ",\"relay\":" << host::pins[OUT]
        << ",\"weight\":" << number(currentWeight)
        << ",\"shot_seconds\":" << number(shot.shotTimer)
        << ",\"last_weight_ms\":" << scale._lastPacket
        << ",\"datapoints\":" << shot.datapoints
        << ",\"led\":[" << 255 - host::pins[LED_RED] << ","
        << 255 - host::pins[LED_GREEN] << "," << 255 - host::pins[LED_BLUE] << "]}";
    std::lock_guard<std::mutex> guard(host::stateMutex);
    host::state = out.str();
}
template<class F> int protect(F action) {
    try { action(); return 0; }
    catch (const host::Stop&) { return 1; }
    catch (const std::exception& error) { host::emit("native_error", error.what()); return -1; }
}
}
EXPORT int sim_init(host::Rpc rpc, host::Log log) {
    return protect([&] {
        if (initialized) throw std::runtime_error("Use a new process/library for a fresh firmware run");
        initialized = true;
        host::rpc = rpc;
        host::log = log;
        host::epoch = std::chrono::steady_clock::now();
        host::inputPin = in;
        host::outputPin = OUT;
        setup();
        publish();
    });
}
EXPORT int sim_step() {
    return protect([] {
        host::checkStop();
        int requested = host::requestedGoal.exchange(-1);
        if (requested >= 0) weightCharacteristic.writeFromCentral(requested);
        loop();
        publish();
    });
}
EXPORT void sim_stop() { host::stopping = true; }
EXPORT void sim_paddle(int on) { host::paddle = on != 0; }
EXPORT void sim_goal(int grams) { host::requestedGoal = grams; }
EXPORT int sim_state(void* output, unsigned int capacity) {
    std::lock_guard<std::mutex> guard(host::stateMutex);
    if (capacity <= host::state.size()) return -1;
    std::memcpy(output, host::state.c_str(), host::state.size() + 1);
    return static_cast<int>(host::state.size());
}
EXPORT int sim_notify(const char* uuid, const uint8_t* bytes, unsigned int length) {
    if (length > 512) return -1;
    std::lock_guard<std::mutex> guard(host::notificationMutex);
    if (host::notifications.size() >= 1024) return -1;
    host::notifications.push_back({uuid, {bytes, bytes + length}});
    return 0;
}
