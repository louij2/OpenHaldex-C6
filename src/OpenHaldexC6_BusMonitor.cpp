#include <OpenHaldexC6_BusMonitor.h>

// Passive bus monitor (see the header). One fixed table, keyed by
// (bus, extended flag, identifier), filled from the two CAN RX tasks.
//
// Cost: an open-addressed hash lookup plus a payload compare per frame, inside
// a short critical section. No task, no timer, no allocation and no WiFi hold:
// polling /api/busmon deliberately does NOT count as UI activity, so a tower or
// phone watching the bus never stops the controller going to sleep.
//
// The baseline is a saved set of IDs ("what this car normally has"). It lives
// in NVS rather than LittleFS so a filesystem OTA doesn't wipe it. With one
// saved, every ID outside it is reported as new, and every baseline ID not seen
// since the last reset is listed as missing.

namespace {

static const uint16_t kSlots = 256;      // hash table size (power of two)
static const uint16_t kMaxEntries = 200; // keep the table under ~80% full
static const uint16_t kMaxBaseline = 200;
static const uint32_t kUnused = 0xFFFFFFFFUL;

struct BusMonEntry {
  uint32_t key;      // id | extd << 29 | bus << 30; kUnused = empty slot
  uint32_t firstMs;
  uint32_t lastMs;
  uint32_t count;
  uint32_t changes;  // frames whose payload (or length) differed from the previous one
  uint16_t periodMs; // gap between the last two frames, saturating
  uint8_t dlc;
  uint8_t data[8];
};

static BusMonEntry table[kSlots];
static uint16_t used = 0;
static uint32_t droppedFrames = 0; // frames of new IDs refused because the table was full
static uint32_t busTotals[2] = {0, 0};
static uint32_t resetMs = 0;
static bool initialised = false;

static uint32_t baseline[kMaxBaseline]; // sorted keys
static uint16_t baselineCount = 0;

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static Preferences busmonPref;

static inline uint32_t makeKey(uint32_t id, bool extd, uint8_t bus) {
  return (id & 0x1FFFFFFFUL) | (extd ? (1UL << 29) : 0) | ((uint32_t)(bus & 1) << 30);
}

static inline uint16_t hashKey(uint32_t key) {
  key ^= key >> 16;
  key *= 0x45D9F3BUL;
  key ^= key >> 16;
  return (uint16_t)(key & (kSlots - 1));
}

// Call with mux held.
static void clearTableLocked() {
  for (uint16_t i = 0; i < kSlots; i++) table[i].key = kUnused;
  used = 0;
  droppedFrames = 0;
  busTotals[0] = busTotals[1] = 0;
  resetMs = millis();
}

static bool inBaseline(uint32_t key) {
  int lo = 0, hi = (int)baselineCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (baseline[mid] == key) return true;
    if (baseline[mid] < key) lo = mid + 1;
    else hi = mid - 1;
  }
  return false;
}

static int cmpKey(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

static void printKey(Print &out, uint32_t key) {
  const bool extd = (key >> 29) & 1;
  out.printf("\"bus\":%u,\"id\":\"0x%0*lX\",\"ext\":%s", (unsigned)((key >> 30) & 1), extd ? 8 : 3,
             (unsigned long)(key & 0x1FFFFFFFUL), extd ? "true" : "false");
}

}  // namespace

void busMonitorSetup() {
  taskENTER_CRITICAL(&mux);
  clearTableLocked();
  taskEXIT_CRITICAL(&mux);

  busmonPref.begin("busmon", true);
  size_t len = busmonPref.getBytesLength("base");
  if (len > 0 && len <= sizeof(baseline) && (len % sizeof(uint32_t)) == 0) {
    busmonPref.getBytes("base", baseline, len);
    baselineCount = len / sizeof(uint32_t);
    qsort(baseline, baselineCount, sizeof(uint32_t), cmpKey);
  }
  busmonPref.end();
  initialised = true;
}

void busMonitorFrame(const twai_message_t &m, uint8_t bus) {
  if (!initialised) return;
  const uint32_t key = makeKey(m.identifier, m.extd, bus);
  const uint32_t now = millis();
  const uint8_t dlc = m.data_length_code > 8 ? 8 : m.data_length_code;

  taskENTER_CRITICAL(&mux);
  busTotals[bus & 1]++;
  uint16_t slot = hashKey(key);
  while (table[slot].key != kUnused && table[slot].key != key) {
    slot = (slot + 1) & (kSlots - 1);
  }
  BusMonEntry &e = table[slot];
  if (e.key == kUnused) {
    if (used >= kMaxEntries) {
      droppedFrames++;
      taskEXIT_CRITICAL(&mux);
      return;
    }
    used++;
    e.key = key;
    e.firstMs = now;
    e.lastMs = now;
    e.count = 0;
    e.changes = 0;
    e.periodMs = 0;
    e.dlc = dlc;
    memcpy(e.data, m.data, dlc);
  } else {
    const uint32_t dt = now - e.lastMs;
    e.periodMs = (uint16_t)(dt > 0xFFFF ? 0xFFFF : dt);
    e.lastMs = now;
    if (e.dlc != dlc || memcmp(e.data, m.data, dlc) != 0) {
      e.changes++;
      e.dlc = dlc;
      memcpy(e.data, m.data, dlc);
    }
  }
  e.count++;
  taskEXIT_CRITICAL(&mux);
}

void busMonitorReset() {
  taskENTER_CRITICAL(&mux);
  clearTableLocked();
  taskEXIT_CRITICAL(&mux);
}

uint16_t busMonitorSaveBaseline() {
  uint32_t keys[kMaxBaseline];
  uint16_t n = 0;
  taskENTER_CRITICAL(&mux);
  for (uint16_t i = 0; i < kSlots && n < kMaxBaseline; i++) {
    if (table[i].key != kUnused) keys[n++] = table[i].key;
  }
  taskEXIT_CRITICAL(&mux);
  qsort(keys, n, sizeof(uint32_t), cmpKey);

  busmonPref.begin("busmon", false);
  if (n > 0) busmonPref.putBytes("base", keys, n * sizeof(uint32_t));
  else busmonPref.remove("base");
  busmonPref.end();

  memcpy(baseline, keys, n * sizeof(uint32_t));
  baselineCount = n;
  return n;
}

void busMonitorClearBaseline() {
  busmonPref.begin("busmon", false);
  busmonPref.remove("base");
  busmonPref.end();
  baselineCount = 0;
}

// Written straight to the response stream, one entry at a time, so a full
// table (~25 KB of JSON) never needs a JsonDocument or a String in the heap.
void busMonitorWriteJson(Print &out) {
  const uint32_t now = millis();
  uint32_t totals0, totals1, dropped, since;
  uint16_t n;
  taskENTER_CRITICAL(&mux);
  totals0 = busTotals[0];
  totals1 = busTotals[1];
  dropped = droppedFrames;
  since = now - resetMs;
  n = used;
  taskEXIT_CRITICAL(&mux);

  out.printf("{\"uptimeMs\":%lu,\"sinceResetMs\":%lu,\"frames\":[%lu,%lu],\"idCount\":%u,"
             "\"capacity\":%u,\"droppedFrames\":%lu,\"baselineCount\":%u,\"ids\":[",
             (unsigned long)now, (unsigned long)since, (unsigned long)totals0, (unsigned long)totals1,
             (unsigned)n, (unsigned)kMaxEntries, (unsigned long)dropped, (unsigned)baselineCount);

  bool first = true;
  for (uint16_t i = 0; i < kSlots; i++) {
    BusMonEntry e;
    taskENTER_CRITICAL(&mux);
    e = table[i];
    taskEXIT_CRITICAL(&mux);
    if (e.key == kUnused) continue;

    out.print(first ? "{" : ",{");
    first = false;
    printKey(out, e.key);
    const uint32_t span = e.lastMs - e.firstMs;
    const float hz = (e.count > 1 && span > 0) ? (float)(e.count - 1) * 1000.0f / (float)span : 0.0f;
    out.printf(",\"count\":%lu,\"firstAgoMs\":%lu,\"lastAgoMs\":%lu,\"periodMs\":%u,\"hz\":%.2f,"
               "\"changes\":%lu,\"dlc\":%u,\"data\":\"",
               (unsigned long)e.count, (unsigned long)(now - e.firstMs), (unsigned long)(now - e.lastMs),
               (unsigned)e.periodMs, hz, (unsigned long)e.changes, (unsigned)e.dlc);
    for (uint8_t b = 0; b < e.dlc; b++) out.printf("%02X", e.data[b]);
    out.printf("\",\"new\":%s}", (baselineCount > 0 && !inBaseline(e.key)) ? "true" : "false");
  }

  // Baseline IDs not heard since the last reset.
  out.print("],\"missing\":[");
  first = true;
  for (uint16_t b = 0; b < baselineCount; b++) {
    const uint32_t key = baseline[b];
    bool seen = false;
    taskENTER_CRITICAL(&mux);
    uint16_t slot = hashKey(key);
    while (table[slot].key != kUnused) {
      if (table[slot].key == key) {
        seen = true;
        break;
      }
      slot = (slot + 1) & (kSlots - 1);
    }
    taskEXIT_CRITICAL(&mux);
    if (seen) continue;
    out.print(first ? "{" : ",{");
    first = false;
    printKey(out, key);
    out.print("}");
  }
  out.print("]}");
}
