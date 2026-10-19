#ifndef SHOT_STOPPER_DIAGNOSTICS_H
#define SHOT_STOPPER_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

namespace ShotStopperDiagnostics {

static const uint8_t kVersion = 1;
static const uint8_t kStatusType = 1;
static const uint8_t kMetadataType = 2;
static const uint8_t kHistoryType = 3;
static const uint8_t kStatusSize = 20;
static const uint8_t kMetadataSize = 20;
static const uint16_t kHistorySize = 496;
static const uint8_t kHistoryCapacity = 24;
static const uint8_t kHistoryEventSize = 20;
static const uint32_t kPublishPeriodMs = 500;
static const uint32_t kNoSampleAge = 0xffffffffUL;
static const uint32_t kMaximumSampleAge = 0xfffffffeUL;

enum StatusFlag {
  kScaleConnected = 1 << 0,
  kWeightValid = 1 << 1,
  kBrewing = 1 << 2,
  kLatched = 1 << 3,
  kButtonPressed = 1 << 4,
  kLearningPending = 1 << 5,
  kSaturated = 1 << 6
};

enum EventKind {
  kConnectionAttempt = 1,
  kConnectionReady = 2,
  kConnectionFailed = 3,
  kDisconnected = 4,
  kTimeout = 5,
  kShotStart = 6,
  kShotEnd = 7,
  kLatch = 8,
  kCommandResult = 9,
  kLearningWait = 10,
  kLearningRejected = 11,
  kLearningUpdate = 12,
  kBoot = 13
};

enum ConnectionFailureStage {
  kScanFailure = 1,
  kConnectFailure = 2,
  kDiscoveryFailure = 3,
  kScaleTypeFailure = 4,
  kSubscriptionFailure = 5,
  kIdentifyFailure = 6,
  kNotificationRequestFailure = 7,
  kModeFailure = 8
};

struct Status {
  uint8_t flags;
  uint32_t uptimeMs;
  int16_t weightDecigrams;
  uint16_t targetDecigrams;
  int16_t actualOffsetDecigrams;
  uint32_t weightAgeMs;
  uint16_t shotElapsedDeciseconds;
};

struct Event {
  uint32_t sequence;
  uint32_t timestampMs;
  uint8_t kind;
  uint8_t detail;
  int16_t actualOffsetDecigrams;
  int32_t valueDecigrams;
  uint32_t sampleAgeMs;
};

class Ring {
 public:
  Ring()
      : _head(0), _count(0), _nextSequence(0), _overwritten(0), _generation(0),
        _dirty(false), _hasPublished(false), _lastPublishedMs(0) {}

  void append(uint32_t timestampMs, uint8_t kind, uint8_t detail,
              int16_t actualOffsetDecigrams, int32_t valueDecigrams,
              uint32_t sampleAgeMs) {
    const uint32_t sequence = _nextSequence++;
    uint8_t slot;
    if (_count < kHistoryCapacity) {
      slot = static_cast<uint8_t>((_head + _count) % kHistoryCapacity);
      ++_count;
    } else {
      slot = _head;
      _head = static_cast<uint8_t>((_head + 1) % kHistoryCapacity);
      if (_overwritten != 0xffffffffUL) {
        ++_overwritten;
      }
    }
    Event& event = _events[slot];
    event.sequence = sequence;
    event.timestampMs = timestampMs;
    event.kind = kind;
    event.detail = detail;
    event.actualOffsetDecigrams = actualOffsetDecigrams;
    event.valueDecigrams = valueDecigrams;
    event.sampleAgeMs = sampleAgeMs;
    _dirty = true;
  }

  uint16_t count() const { return _count; }
  uint32_t oldestSequence() const { return _nextSequence - _count; }
  uint32_t nextSequence() const { return _nextSequence; }
  uint32_t overwritten() const { return _overwritten; }
  uint32_t generation() const { return _generation; }
  bool dirty() const { return _dirty; }

  bool publishDue(uint32_t nowMs) const {
    return _dirty && (!_hasPublished ||
                      static_cast<uint32_t>(nowMs - _lastPublishedMs) >=
                          kPublishPeriodMs);
  }

  void markPublished(uint32_t nowMs) {
    ++_generation;
    _dirty = false;
    _hasPublished = true;
    _lastPublishedMs = nowMs;
  }

  const Event& eventOldestFirst(uint16_t index) const {
    return _events[(_head + index) % kHistoryCapacity];
  }

 private:
  Event _events[kHistoryCapacity];
  uint8_t _head;
  uint16_t _count;
  uint32_t _nextSequence;
  uint32_t _overwritten;
  uint32_t _generation;
  bool _dirty;
  bool _hasPublished;
  uint32_t _lastPublishedMs;
};

inline void writeU16(uint8_t* buffer, size_t offset, uint16_t value) {
  buffer[offset] = static_cast<uint8_t>(value);
  buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
}

inline void writeU32(uint8_t* buffer, size_t offset, uint32_t value) {
  writeU16(buffer, offset, static_cast<uint16_t>(value));
  writeU16(buffer, offset + 2, static_cast<uint16_t>(value >> 16));
}

inline bool serializeStatus(const Status& status, uint8_t* buffer,
                            size_t capacity) {
  if (!buffer || capacity < kStatusSize) return false;
  buffer[0] = kVersion;
  buffer[1] = kStatusType;
  writeU16(buffer, 2, status.flags);
  writeU32(buffer, 4, status.uptimeMs);
  writeU16(buffer, 8, static_cast<uint16_t>(status.weightDecigrams));
  writeU16(buffer, 10, status.targetDecigrams);
  writeU16(buffer, 12, static_cast<uint16_t>(status.actualOffsetDecigrams));
  writeU32(buffer, 14, status.weightAgeMs);
  writeU16(buffer, 18, status.shotElapsedDeciseconds);
  return true;
}

inline bool serializeMetadata(const Ring& ring, uint8_t* buffer,
                              size_t capacity) {
  if (!buffer || capacity < kMetadataSize) return false;
  buffer[0] = kVersion;
  buffer[1] = kMetadataType;
  buffer[2] = kHistoryCapacity;
  buffer[3] = static_cast<uint8_t>(ring.count());
  writeU32(buffer, 4, ring.generation());
  writeU32(buffer, 8, ring.oldestSequence());
  writeU32(buffer, 12, ring.nextSequence());
  writeU32(buffer, 16, ring.overwritten());
  return true;
}

inline bool serializeHistory(const Ring& ring, uint8_t* buffer,
                             size_t capacity) {
  if (!buffer || capacity < kHistorySize) return false;
  buffer[0] = kVersion;
  buffer[1] = kHistoryType;
  writeU16(buffer, 2, ring.count());
  writeU32(buffer, 4, ring.generation());
  writeU32(buffer, 8, ring.oldestSequence());
  writeU32(buffer, 12, ring.nextSequence());

  size_t offset = 16;
  for (uint16_t i = 0; i < ring.count(); ++i) {
    const Event& event = ring.eventOldestFirst(i);
    writeU32(buffer, offset, event.sequence);
    writeU32(buffer, offset + 4, event.timestampMs);
    buffer[offset + 8] = event.kind;
    buffer[offset + 9] = event.detail;
    writeU16(buffer, offset + 10,
             static_cast<uint16_t>(event.actualOffsetDecigrams));
    writeU32(buffer, offset + 12,
             static_cast<uint32_t>(event.valueDecigrams));
    writeU32(buffer, offset + 16, event.sampleAgeMs);
    offset += kHistoryEventSize;
  }
  while (offset < kHistorySize) {
    buffer[offset++] = 0;
  }
  return true;
}

}  // namespace ShotStopperDiagnostics

#endif
