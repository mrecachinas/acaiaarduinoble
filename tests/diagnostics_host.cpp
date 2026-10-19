#include "../ShotStopperDiagnostics.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace ShotStopperDiagnostics;

int main() {
  static_assert(kStatusSize == 20, "status wire size");
  static_assert(kMetadataSize == 20, "metadata wire size");
  static_assert(kHistorySize == 496, "history wire size");
  static_assert(kHistoryCapacity == 24, "bounded ring");
  static_assert(sizeof(Ring) <= 600, "ring RAM budget");
  Ring ring;
  uint8_t history[kHistorySize + 1];
  uint8_t metadata[kMetadataSize];
  uint8_t status[kStatusSize];
  memset(history, 0xa5, sizeof(history));
  assert(!serializeHistory(ring, history, kHistorySize - 1));
  assert(history[0] == 0xa5);
  assert(!serializeHistory(ring, NULL, kHistorySize));
  assert(!serializeMetadata(ring, metadata, kMetadataSize - 1));
  assert(!serializeMetadata(ring, NULL, kMetadataSize));
  Status state = {0x7f, 0x12345678UL, -32768, 65535, -123,
                  kMaximumSampleAge, 65535};
  assert(!serializeStatus(state, status, kStatusSize - 1));
  assert(!serializeStatus(state, NULL, kStatusSize));
  assert(serializeStatus(state, status, sizeof(status)));
  assert(fwrite(status, 1, sizeof(status), stdout) == sizeof(status));

  assert(ring.count() == 0 && ring.nextSequence() == 0);
  assert(!ring.dirty() && !ring.publishDue(0));
  assert(serializeMetadata(ring, metadata, sizeof(metadata)));
  assert(serializeHistory(ring, history, sizeof(history)));
  assert(history[kHistorySize] == 0xa5);
  for (size_t i = 16; i < kHistorySize; ++i) assert(history[i] == 0);
  assert(fwrite(metadata, 1, sizeof(metadata), stdout) == sizeof(metadata));
  assert(fwrite(history, 1, kHistorySize, stdout) == kHistorySize);

  ring.append(10, kBoot, 0, -1, 0, kNoSampleAge);
  assert(ring.dirty() && ring.publishDue(0));
  ring.markPublished(0xfffffff0UL);
  assert(!ring.dirty() && !ring.publishDue(0x200));
  ring.append(11, kShotStart, 0, -1, -4, 1);
  assert(!ring.publishDue(0x1e3));
  assert(ring.publishDue(0x1e4));
  ring.markPublished(0x1e4);
  assert(!ring.publishDue(0x1e4));
  for (uint32_t i = 2; i < 27; ++i) {
    ring.append(100 + i, kShotEnd, 1, -123,
                -1000 - static_cast<int32_t>(i), kNoSampleAge);
    assert(ring.count() <= kHistoryCapacity);
  }
  assert(ring.count() == 24 && ring.overwritten() == 3);
  assert(ring.oldestSequence() == 3 && ring.nextSequence() == 27);
  assert(ring.generation() == 2 && ring.dirty());
  assert(!ring.publishDue(0x3d7));
  assert(ring.publishDue(0x3d8));
  ring.markPublished(0x3d8);
  assert(ring.generation() == 3 && !ring.dirty());
  for (uint16_t i = 0; i < ring.count(); ++i) {
    assert(ring.eventOldestFirst(i).sequence == i + 3);
  }
  assert(serializeMetadata(ring, metadata, sizeof(metadata)));
  assert(serializeHistory(ring, history, sizeof(history)));
  assert(fwrite(metadata, 1, sizeof(metadata), stdout) == sizeof(metadata));
  assert(fwrite(history, 1, kHistorySize, stdout) == kHistorySize);
  return 0;
}
