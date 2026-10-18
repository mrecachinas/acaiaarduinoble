#include "Arduino.h"
#include "ArduinoBLE.h"
#include "EEPROM.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <random>

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

using Bytes = std::vector<uint8_t>;

Bytes unhex(const char* text) {
    Bytes bytes;
    for (size_t i = 0; i < std::strlen(text); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(std::string(text + i, 2), nullptr, 16)));
    }
    return bytes;
}

Bytes frame(uint8_t type, Bytes payload) {
    Bytes bytes = {0xef, 0xdd, 0x0c, static_cast<uint8_t>(payload.size() + 2), type};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    uint8_t a = 0, b = 0;
    for (size_t i = 3; i < bytes.size(); ++i) {
        if ((i - 3) % 2 == 0) a += bytes[i];
        else b += bytes[i];
    }
    bytes.push_back(a);
    bytes.push_back(b);
    return bytes;
}

Bytes weight(uint16_t tenths, size_t extra = 0) {
    Bytes payload = {static_cast<uint8_t>(tenths), static_cast<uint8_t>(tenths >> 8),
                     0, 0, 1, 0};
    payload.resize(payload.size() + extra, 0);
    return frame(5, payload);
}

void reset() {
    mock::now = 10000;
    mock::serialDelay = 0;
    mock::events.clear();
    mock::writes.clear();
    mock::writeSuccess = true;
    Serial.output.clear();
    BLE = BLEMock{};
    std::fill(std::begin(mock::inputs), std::end(mock::inputs), HIGH);
    std::fill(std::begin(mock::outputs), std::end(mock::outputs), LOW);
    mock::inputs[in] = LOW;
    scale._type = NEW;
    scale._connected = true;
    scale._lastHeartBeat = millis();
    scale._lastPacket = 0;
    scale._connectedAt = millis();
    scale._hasWeight = false;
    scale._lastWrite = 0;
    scale._currentWeight = 0;
    scale._rxLen = 0;
    scale._read = BLECharacteristic{};
    scale._write = BLECharacteristic{};
    scale._instance = &scale;
    scale._read.setEventHandler(BLEUpdated, AcaiaArduinoBLE::onNotification);
    shot = Shot{};
    shot.brewing = true;
    goalWeight = 36;
    weightOffset = 1.5;
    currentWeight = 0;
    buttonPressed = true;
    buttonLatched = false;
    waitingForPaddleRelease = false;
    newButtonState = 1;
    std::fill(std::begin(buttonArr), std::end(buttonArr), 1);
    lastButtonRead_ms = millis();
    lastConnectAttempt = 0;
    lastWeight_ms = 0;
    hasWeight = false;
    warnedStaleWeight = false;
}

void feed(const Bytes& bytes) { scale._read.notify(bytes); }
bool closeWeight(float expected) { return std::abs(scale.getWeight() - expected) < 0.001f; }
size_t tares() {
    return std::count_if(mock::writes.begin(), mock::writes.end(), [](const mock::Write& w) {
        return w.bytes.size() > 2 && w.bytes[2] == 4;
    });
}
void releasePaddle() {
    mock::inputs[in] = HIGH;
    std::fill(std::begin(buttonArr), std::end(buttonArr), 0);
    newButtonState = 0;
}

size_t logCount(const std::string& message) {
    size_t count = 0;
    for (size_t pos = Serial.output.find(message); pos != std::string::npos;
         pos = Serial.output.find(message, pos + message.size())) {
        ++count;
    }
    return count;
}

void parserTests() {
    // Real Lunar protocol frames, without device identity or host metadata.
    const char* captures[] = {
        "efdd0707021901000b01210e",
        "efdd0c08050000000001000905",
        "efdd080932020101000101010e34",
        "efdd0c0b0b008005000000000100108c",
        "efdd0c0c05000000000100070000021407",
        "efdd0c0e0b00410b0001050000000001001b51",
    };
    Bytes packed;
    for (auto hex : captures) {
        auto bytes = unhex(hex);
        bool isWeight = bytes[2] == 12 && bytes[4] == 5;
        for (size_t split = 1; split < bytes.size(); ++split) {
            reset();
            feed(Bytes(bytes.begin(), bytes.begin() + split));
            assert(!scale.newWeightAvailable());
            feed(Bytes(bytes.begin() + split, bytes.end()));
            assert(scale.newWeightAvailable() == isWeight);
            assert(closeWeight(0));
            assert(scale._rxLen == 0);
        }
        packed.insert(packed.end(), bytes.begin(), bytes.end());
    }
    reset();
    feed(packed);
    assert(scale.newWeightAvailable() && closeWeight(0));
    assert(scale._rxLen == 0);

    reset();
    std::mt19937 rng(2021);
    const size_t extraBytes[] = {0, 4, 6};
    for (int n = 0; n < 2000; ++n) {
        auto bytes = weight(n % 2000, extraBytes[n % 3]);
        bool decoded = false;
        for (size_t i = 0; i < bytes.size();) {
            size_t count = std::min<size_t>(1 + rng() % 8, bytes.size() - i);
            feed(Bytes(bytes.begin() + i, bytes.begin() + i + count));
            decoded = scale.newWeightAvailable() || decoded;
            i += count;
        }
        assert(decoded && closeWeight((n % 2000) / 10.f));
    }
    for (int index : {3, 5, 11, 12}) {
        reset();
        auto damaged = weight(100);
        damaged[index] ^= 1;
        feed(damaged);
        assert(!scale.newWeightAvailable());
        auto good = weight(123);
        feed(good);
        assert(scale.newWeightAvailable() && closeWeight(12.3f));
    }
    reset();
    feed({0xef, 0xdd, 0x0c, 0x23});
    for (int n = 1; n <= 4; ++n) {
        feed(weight(n * 100));
        assert(scale.newWeightAvailable() == (n >= 3));
        if (n >= 3) assert(closeWeight(n * 10.f));
    }
    for (const auto& prefix : {
        Bytes{0xef, 0xdd, 0x0c, 0}, Bytes{0xef, 0xdd, 0x0c, 255},
        Bytes{0xef, 0xdd, 4, 0, 0, 0}, Bytes{0xef, 0xdd, 0x0c, 1, 0xef, 0xdd}
    }) {
        reset();
        feed(prefix);
        feed(weight(123));
        assert(scale.newWeightAvailable() && closeWeight(12.3f));
    }
    reset();
    feed(frame(5, {123, 0, 0xef, 0xdd, 1, 2}));
    assert(scale.newWeightAvailable() && closeWeight(-12.3f));
    reset();
    for (auto byte : weight(123)) {
        feed({byte});
        scale.newWeightAvailable();
    }
    assert(closeWeight(12.3f) && scale._rxLen == 0);

    reset();
    Bytes overflow(200, 0);
    auto final = weight(321);
    overflow.insert(overflow.end(), final.begin(), final.end());
    feed(overflow);
    assert(scale._rxLen == RX_BUFFER_SIZE);
    assert(scale.newWeightAvailable() && closeWeight(32.1f));
    for (int i = 0; i < 30; ++i) feed(weight(i));
    assert(scale._rxLen <= RX_BUFFER_SIZE);
    assert(scale.newWeightAvailable() && closeWeight(2.9f));
    std::puts("parser framing, checksums, resync, capture fixtures and buffer bounds: PASS");
}

void watchdogTests() {
    reset();
    mock::now += MAX_PACKET_PERIOD_MS;
    BLE.pollStep = 0;
    assert(!scale.newWeightAvailable() && scale.isConnected());
    ++mock::now;
    assert(!scale.newWeightAvailable() && !scale.isConnected());
    assert(Serial.output.find("no weight since connect") != std::string::npos);

    reset();
    mock::now += MAX_PACKET_PERIOD_MS + 1;
    BLE.pending.push_back({scale._read, weight(123)});
    assert(scale.newWeightAvailable() && closeWeight(12.3f) && scale.isConnected());
    mock::now += MAX_PACKET_PERIOD_MS + 1;
    feed(weight(234));
    assert(scale.newWeightAvailable() && closeWeight(23.4f) && scale.isConnected());
    mock::now += MAX_PACKET_PERIOD_MS + 1;
    assert(!scale.newWeightAvailable() && !scale.isConnected());

    reset();
    BLE.pollStep = 0;
    scale._connectedAt = ULONG_MAX - 100;
    mock::now = 0;
    feed(weight(0));
    assert(scale.newWeightAvailable() && scale._hasWeight);
    mock::now = 1;
    assert(!scale.newWeightAvailable() && scale.isConnected());
    scale._lastPacket = ULONG_MAX - 100;
    assert(!scale.newWeightAvailable() && scale.isConnected());
    mock::now = MAX_PACKET_PERIOD_MS;
    assert(!scale.newWeightAvailable() && !scale.isConnected());

    for (scale_type type : {OLD, GENERIC, FELICITA}) {
        reset();
        scale._type = type;
        scale._read.setEventHandler(BLEUpdated, nullptr);
        mock::now += MAX_PACKET_PERIOD_MS * 2;
        assert(!scale.newWeightAvailable() && scale.isConnected());
        Bytes bytes;
        if (type == OLD) bytes = {0, 0, 123, 0, 0, 0, 1, 0, 0, 0};
        if (type == GENERIC) {
            bytes.resize(20);
            bytes[8] = 4;
            bytes[9] = 206;
        }
        if (type == FELICITA) {
            bytes.resize(18);
            const char* digits = "001230";
            std::copy(digits, digits + 6, bytes.begin() + 3);
        }
        feed(bytes);
        assert(scale.newWeightAvailable() && closeWeight(12.3f));
        mock::now += MAX_PACKET_PERIOD_MS + 1;
        assert(!scale.newWeightAvailable() && !scale.isConnected());
    }
    std::puts("initial/existing watchdogs, pending data, rollover and other scale types: PASS");
}

void readinessTests() {
    if (MOMENTARY || TIMER_ONLY) return;
    reset();
    setBrewingState(true);
    mock::now = 13100;
    scaleConnected_task();
    assert(!buttonLatched && mock::outputs[OUT] == LOW && tares() == 1);
    mock::now = 13200;
    scaleConnected_task();
    auto warning = Serial.output.find("no fresh scale data");
    assert(warning != std::string::npos);
    assert(Serial.output.find("no fresh scale data", warning + 1) == std::string::npos);
    releasePaddle();
    scaleConnected_task();
    assert(!shot.brewing && !buttonLatched);
    assert(tares() == 1);

    reset();
    setBrewingState(true);
    mock::now = 11800;
    feed(weight(0));
    scaleConnected_task();
    mock::now = 13100;
    scaleConnected_task();
    assert(!buttonLatched);
    mock::now = 14200;
    feed(weight(0));
    scaleConnected_task();
    assert(buttonLatched && mock::outputs[OUT] == HIGH && tares() == 2);

    // A short gap still satisfies the one-second freshness policy.
    reset();
    setBrewingState(true);
    mock::now = 12500;
    feed(weight(0));
    scaleConnected_task();
    mock::now = 13010;
    scaleConnected_task();
    assert(buttonLatched);

    for (unsigned long age : {static_cast<unsigned long>(WEIGHT_FRESH_MS), WEIGHT_FRESH_MS + 1UL}) {
        reset();
        setBrewingState(true);
        BLE.pollStep = 0;
        mock::now = 13100;
        scale._lastHeartBeat = millis();
        hasWeight = true;
        lastWeight_ms = millis() - age;
        scaleConnected_task();
        assert(buttonLatched == (age == WEIGHT_FRESH_MS));
    }
    reset();
    setBrewingState(true);
    mock::now = 14200;
    releasePaddle();
    feed(weight(0));
    scaleConnected_task();
    assert(!shot.brewing && !buttonLatched && tares() == 1);
    reset();
    setBrewingState(true);
    mock::now = 61000;
    feed(weight(0));
    scaleConnected_task();
    assert(!shot.brewing && !buttonLatched && tares() == 1);
    std::puts("freshness boundaries, delayed takeover, warning and manual release: PASS");
}

void stopTests() {
    for (ENDTYPE reason : {WEIGHT, TIME, BUTTON, DISCONNECT}) {
        reset();
        shot.start_timestamp_s = 5;
        shot.end = reason;
        shot.brewing = false;
        buttonLatched = !MOMENTARY;
        mock::outputs[OUT] = MOMENTARY ? LOW : HIGH;
        scale._lastWrite = millis();
        scale._connected = reason != DISCONNECT;
        auto decision = millis();
        mock::serialDelay = 10;
        setBrewingState(false);
        if (!TIMER_ONLY && (!MOMENTARY || reason == WEIGHT || reason == TIME)) {
            assert(mock::events.front().kind == "gpio");
            assert(mock::events.front().time == decision);
            assert(mock::outputs[OUT] == LOW);
            assert(mock::events.front().value == (MOMENTARY ? HIGH : LOW));
        } else {
            assert(std::none_of(mock::events.begin(), mock::events.end(),
                                [](const mock::Event& e) { return e.kind == "gpio"; }));
        }
        if (reason == DISCONNECT) assert(mock::writes.empty());
    }
    if (!TIMER_ONLY) {
        reset();
        scale._type = OLD; // No first-weight watchdog; exercise the independent maximum.
        setBrewingState(true);
        mock::now = 61000;
        scaleConnected_task();
        assert(!shot.brewing && mock::outputs[OUT] == LOW);
    }
    if (!MOMENTARY && !TIMER_ONLY) {
        for (bool throttled : {false, true}) {
            reset();
            shot.start_timestamp_s = 5;
            buttonLatched = true;
            mock::outputs[OUT] = HIGH;
            scale._connected = false;
            lastConnectAttempt = throttled ? millis() : 0;
            BLE.onScan = [] {
                assert(mock::outputs[OUT] == LOW);
                mock::now += 5000;
            };
            auto decision = millis();
            loop();
            assert(!shot.brewing && !hasWeight);
            assert(mock::events.front().kind == "gpio");
            assert(mock::events.front().time == decision);
            if (throttled) assert(mock::events.size() == 1);
        }
        reset();
        shot.start_timestamp_s = 5;
        buttonLatched = true;
        mock::outputs[OUT] = HIGH;
        scale._hasWeight = true;
        scale._lastPacket = millis() - MAX_PACKET_PERIOD_MS - 1;
        BLE.onScan = [] { assert(mock::outputs[OUT] == LOW); };
        loop();
        assert(!shot.brewing && mock::outputs[OUT] == LOW);
        assert(mock::events[0].kind == "disconnect" && mock::events[1].kind == "gpio");
        assert(mock::events[0].time == mock::events[1].time);
    }
    std::puts("output ordering, disconnect/reconnect throttling and elapsed maximum: PASS");
}

void healthyShotTest() {
    if (MOMENTARY || TIMER_ONLY) return;
    reset();
    buttonPressed = false;
    shot.brewing = false;
    feed(weight(0));
    scaleConnected_task();
    assert(shot.brewing);
    auto start = static_cast<unsigned long>(shot.start_timestamp_s * 1000);
    for (int elapsed = 200; elapsed <= 30000 && shot.brewing; elapsed += 200) {
        mock::now = start + elapsed;
        if (buttonLatched && !REEDSWITCH) mock::inputs[in] = HIGH;
        auto packet = weight(std::max(0, elapsed - 3200) / 50, 4);
        feed(Bytes(packet.begin(), packet.begin() + 3));
        scaleConnected_task();
        feed(Bytes(packet.begin() + 3, packet.end()));
        scaleConnected_task();
    }
    assert(tares() == 2 && !shot.brewing && !buttonLatched);
    assert(mock::outputs[OUT] == LOW && shot.end_s > 19 && shot.end_s < 23);
    std::puts("healthy fragmented stream, paddle takeover and automatic weight stop: PASS");
}

void heldPaddleCompletionTests() {
    if (MOMENTARY || TIMER_ONLY) return;
    Bytes felicita(18, 0);
    std::fill(felicita.begin() + 3, felicita.begin() + 9, '0');
    struct Profile { scale_type type; Bytes zeroWeight; Bytes tareCommand; };
    const Profile profiles[] = {
        {OLD, {0, 0, 0, 0, 0, 0, 1, 0, 0, 0}, unhex("efdd04000000")},
        {GENERIC, Bytes(20, 0), unhex("030a01000008")},
        {FELICITA, felicita, {0x54}},
    };
    for (const auto& profile : profiles) {
        for (ENDTYPE reason : {TIME, WEIGHT}) {
            reset();
            scale._type = profile.type;
            scale._read.setEventHandler(BLEUpdated, nullptr);
            auto tareCount = [&] {
                return std::count_if(mock::writes.begin(), mock::writes.end(),
                                    [&](const mock::Write& w) {
                                        return w.bytes == profile.tareCommand;
                                    });
            };
            setBrewingState(true);
            auto start = static_cast<unsigned long>(shot.start_timestamp_s * 1000);
            auto nextWeight = millis();
            auto runUntil = [&](unsigned long end, bool stream) {
                while (millis() < end) {
                    mock::now += 10;
                    if (stream && millis() >= nextWeight) {
                        feed(profile.zeroWeight);
                        nextWeight = millis() + 100;
                    }
                    loop();
                }
            };

            if (reason == TIME) {
                runUntil(start + 50500, false);
                assert(logCount("ShotEnded by time") == 1);
            } else {
                // Exercise the shared completion routine's unlatched WEIGHT path.
                mock::now = start + 4000;
                shot.brewing = false;
                shot.end = WEIGHT;
                setBrewingState(false);
                assert(logCount("ShotEnded by weight") == 1);
            }
            auto endedAt = millis();
            assert(!shot.brewing && !buttonLatched);
            assert(logCount("shot started") == 1 && tareCount() == 1);

            // Pass the reed forced-off window with the real switch still held.
            runUntil(endedAt + 2000, true);
            assert(!shot.brewing && !buttonLatched && mock::outputs[OUT] == LOW);
            assert(logCount("shot started") == 1 && tareCount() == 1);

            // Late data must not restart the shot or keep filling its fixed arrays.
            runUntil(endedAt + 120000, true);
            assert(!shot.brewing && shot.datapoints == 0);
            assert(logCount("shot started") == 1 && tareCount() == 1);
            assert(logCount("beep write successful") == 0);

            // A brief off pulse is not a debounced release.
            mock::inputs[in] = HIGH;
            runUntil(millis() + 50, true);
            mock::inputs[in] = LOW;
            runUntil(millis() + 400, true);
            assert(!shot.brewing && logCount("shot started") == 1);

            mock::inputs[in] = HIGH;
            runUntil(millis() + BUTTON_READ_PERIOD_MS * BUTTON_STATE_ARRAY_LENGTH * 3, true);
            assert(!shot.brewing && logCount("shot started") == 1);
            mock::inputs[in] = LOW;
            runUntil(millis() + 100, true);
            assert(shot.brewing && logCount("shot started") == 2 && tareCount() == 2);
            runUntil(millis() + 3300, true);
            assert(buttonLatched && mock::outputs[OUT] == HIGH);
            assert(logCount("beep write successful") == 1);
            assert(tareCount() == (profile.type == OLD ? 3 : 2));
        }
    }
    std::puts("held-paddle completion, genuine release and re-arming across legacy scales: PASS");
}

int main() {
    std::setbuf(stdout, nullptr);
    parserTests();
    watchdogTests();
    readinessTests();
    stopTests();
    healthyShotTest();
    heldPaddleCompletionTests();
}
