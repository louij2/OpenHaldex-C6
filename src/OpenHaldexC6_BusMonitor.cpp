#include <OpenHaldexC6_BusMonitor.h>
#include <OpenHaldexC6_OTA.h> // fsMounted(), isOTAUpdateInProgress()
#include "esp_system.h"       // esp_reset_reason()

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

// Sleep-logger hooks, written by the RX tasks: when each side last carried a
// frame, whether it had been declared quiet, and the frame that woke it.
static volatile uint32_t lastBusMs[2] = {0, 0};
static volatile bool busQuiet[2] = {false, false};
static volatile uint32_t wakeKey[2] = {kUnused, kUnused};

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

static void busLogSetup();

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
  busLogSetup();
  initialised = true;
}

void busMonitorFrame(const twai_message_t &m, uint8_t bus) {
  if (!initialised) return;
  const uint32_t key = makeKey(m.identifier, m.extd, bus);
  const uint32_t now = millis();
  const uint8_t dlc = m.data_length_code > 8 ? 8 : m.data_length_code;

  taskENTER_CRITICAL(&mux);
  busTotals[bus & 1]++;
  lastBusMs[bus & 1] = now;
  if (busQuiet[bus & 1]) {
    busQuiet[bus & 1] = false;
    wakeKey[bus & 1] = key; // the frame that broke the silence
  }
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

// ---------------------------------------------------------------------------
// Sleep logger
// ---------------------------------------------------------------------------

const char *const BUSLOG_PATH = "/buslog.txt";
const char *const BUSLOG_OLD_PATH = "/buslog.old.txt";

namespace {

static const uint32_t kSampleS = 120;       // one sample every 2 minutes
static const uint16_t kSamples = 1440;      // = 48 h
static const uint32_t kQuietMs = 10000;     // a side with no frame for 10 s is "quiet"
static const size_t kLogMaxBytes = 8192;    // rotate the event file past this
static const uint8_t kEventIds = 6;

struct BusSample {
  uint16_t fps[2];   // average frames/s over the sample, car / haldex
  uint8_t ids[2];    // IDs heard during the sample, per side (saturating)
  uint8_t flags;     // bit0 = controller in low-power sleep at sample time
  uint8_t pad;
};

static BusSample samples[kSamples];
static uint32_t sampleSeq = 0;              // samples taken this boot; sample n ends at uptime (n+1)*kSampleS
static uint32_t sampleBase = 0;             // first sequence still held after a clear
static uint32_t lastSampleMs = 0;
static uint32_t lastTotals[2] = {0, 0};
static bool tickStarted = false;
static bool lastLowPower = false;
static uint32_t quietFromMs[2] = {0, 0}; // last frame before each side went quiet
static uint32_t bootCount = 0;

enum BusEventType : uint8_t { EV_BOOT, EV_QUIET, EV_ACTIVE, EV_LP_SLEEP, EV_LP_WAKE };

struct BusEvent {
  uint8_t type;
  uint8_t bus;
  uint8_t nkeys;
  uint32_t uptimeS;
  uint32_t value;
  uint32_t keys[kEventIds];
};

static QueueHandle_t eventQueue = nullptr;

static void queueEvent(BusEvent &ev) {
  ev.uptimeS = millis() / 1000;
  if (eventQueue) xQueueSend(eventQueue, &ev, 0); // drop rather than block the IO task
}

// The most recently heard IDs on one side, newest first.
static uint8_t lastHeard(uint8_t bus, uint32_t *keys, uint8_t max) {
  uint32_t when[kEventIds];
  uint8_t n = 0;
  for (uint16_t i = 0; i < kSlots; i++) {
    taskENTER_CRITICAL(&mux);
    const uint32_t k = table[i].key, t = table[i].lastMs;
    taskEXIT_CRITICAL(&mux);
    if (k == kUnused || ((k >> 30) & 1) != bus) continue;
    uint8_t pos = n;
    while (pos > 0 && (int32_t)(t - when[pos - 1]) > 0) pos--;
    if (pos >= max) continue;
    if (n < max) n++;
    for (uint8_t j = n - 1; j > pos; j--) {
      keys[j] = keys[j - 1];
      when[j] = when[j - 1];
    }
    keys[pos] = k;
    when[pos] = t;
  }
  return n;
}

static const char *resetReasonName(uint32_t r) {
  switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    default: return "other";
  }
}

static void writeEvent(const BusEvent &ev) {
  if (!fsMounted() || isOTAUpdateInProgress()) return;
  static const char *const side[2] = {"car", "haldex"};
  char line[160];
  int len = snprintf(line, sizeof(line), "b%lu t%lu ", (unsigned long)bootCount, (unsigned long)ev.uptimeS);
  switch (ev.type) {
    case EV_BOOT: len += snprintf(line + len, sizeof(line) - len, "boot reset=%s", resetReasonName(ev.value)); break;
    case EV_QUIET: len += snprintf(line + len, sizeof(line) - len, "quiet %s last=", side[ev.bus]); break;
    case EV_ACTIVE: len += snprintf(line + len, sizeof(line) - len, "active %s after=%lus first=", side[ev.bus], (unsigned long)ev.value); break;
    case EV_LP_SLEEP: len += snprintf(line + len, sizeof(line) - len, "lp-sleep"); break;
    case EV_LP_WAKE: len += snprintf(line + len, sizeof(line) - len, "lp-wake"); break;
  }
  for (uint8_t i = 0; i < ev.nkeys && len < (int)sizeof(line) - 12; i++) {
    const uint32_t k = ev.keys[i];
    len += snprintf(line + len, sizeof(line) - len, "%s0x%0*lX", i ? "," : "", ((k >> 29) & 1) ? 8 : 3,
                    (unsigned long)(k & 0x1FFFFFFFUL));
  }

  File f = LittleFS.open(BUSLOG_PATH, "a");
  if (!f) return;
  const size_t size = f.size();
  f.println(line);
  f.close();
  if (size > kLogMaxBytes) {
    LittleFS.remove(BUSLOG_OLD_PATH);
    LittleFS.rename(BUSLOG_PATH, BUSLOG_OLD_PATH);
  }
}

// Blocks on the queue, so it costs nothing (and never wakes the CPU) between events.
static void busLogTask(void *) {
  // The boot event is queued before the web server mounts LittleFS; give it a moment.
  for (uint8_t i = 0; i < 60 && !fsMounted(); i++) vTaskDelay(pdMS_TO_TICKS(500));
  BusEvent ev;
  while (1) {
    if (xQueueReceive(eventQueue, &ev, portMAX_DELAY) == pdTRUE) writeEvent(ev);
  }
}

static void takeSample(uint32_t now) {
  uint32_t totals[2];
  uint16_t ids[2] = {0, 0};
  taskENTER_CRITICAL(&mux);
  totals[0] = busTotals[0];
  totals[1] = busTotals[1];
  taskEXIT_CRITICAL(&mux);
  for (uint16_t i = 0; i < kSlots; i++) {
    taskENTER_CRITICAL(&mux);
    const uint32_t k = table[i].key, t = table[i].lastMs;
    taskEXIT_CRITICAL(&mux);
    if (k != kUnused && (int32_t)(t - lastSampleMs) >= 0) ids[(k >> 30) & 1]++;
  }
  BusSample &s = samples[sampleSeq % kSamples];
  for (uint8_t b = 0; b < 2; b++) {
    // A table reset zeroes the totals; count from zero then.
    const uint32_t delta = totals[b] >= lastTotals[b] ? totals[b] - lastTotals[b] : totals[b];
    const uint32_t fps = delta / kSampleS;
    s.fps[b] = fps > 0xFFFF ? 0xFFFF : fps;
    s.ids[b] = ids[b] > 0xFF ? 0xFF : ids[b];
    lastTotals[b] = totals[b];
  }
  s.flags = lowPowerMode ? 1 : 0;
  sampleSeq++;
  lastSampleMs = now;
}

}  // namespace

// Called from busMonitorSetup() (setup()'s stack, not the 2 KB IO task's).
static void busLogSetup() {
  busmonPref.begin("busmon", false);
  bootCount = busmonPref.getUInt("boots", 0) + 1;
  busmonPref.putUInt("boots", bootCount);
  busmonPref.end();
  eventQueue = xQueueCreate(8, sizeof(BusEvent));
  if (eventQueue) xTaskCreate(busLogTask, "busLog", 4096, NULL, 1, NULL);
  BusEvent ev = {};
  ev.type = EV_BOOT;
  ev.value = (uint32_t)esp_reset_reason();
  queueEvent(ev);
}

void busLogTick() {
  if (!initialised) return;
  const uint32_t now = millis();

  if (!tickStarted) {
    tickStarted = true;
    lastLowPower = lowPowerMode;
  }

  for (uint8_t b = 0; b < 2; b++) {
    // Declare quiet under the lock, so a frame can't land between the check and the flag.
    bool wentQuiet = false;
    taskENTER_CRITICAL(&mux);
    const uint32_t last = lastBusMs[b];
    if (!busQuiet[b] && last != 0 && (now - last) >= kQuietMs) {
      busQuiet[b] = true;
      quietFromMs[b] = last;
      wentQuiet = true;
    }
    const uint32_t wk = wakeKey[b];
    wakeKey[b] = kUnused;
    const uint32_t wokeMs = lastBusMs[b];
    taskEXIT_CRITICAL(&mux);

    if (wentQuiet) {
      BusEvent ev = {};
      ev.type = EV_QUIET;
      ev.bus = b;
      ev.nkeys = lastHeard(b, ev.keys, kEventIds);
      queueEvent(ev);
    }
    if (wk != kUnused) {
      BusEvent ev = {};
      ev.type = EV_ACTIVE;
      ev.bus = b;
      ev.nkeys = 1;
      ev.keys[0] = wk;
      ev.value = (wokeMs - quietFromMs[b]) / 1000; // how long it had been silent
      queueEvent(ev);
    }
  }

  if (lowPowerMode != lastLowPower) {
    lastLowPower = lowPowerMode;
    BusEvent ev = {};
    ev.type = lowPowerMode ? EV_LP_SLEEP : EV_LP_WAKE;
    queueEvent(ev);
  }

  if ((now - lastSampleMs) >= kSampleS * 1000UL) takeSample(now);
}

void busLogClear() {
  sampleBase = sampleSeq;
  if (fsMounted()) {
    LittleFS.remove(BUSLOG_PATH);
    LittleFS.remove(BUSLOG_OLD_PATH);
  }
}

// samples: oldest first, [car fps, haldex fps, car IDs, haldex IDs, low-power],
// the last one ending at endUptimeS and each covering intervalS.
void busLogWriteJson(Print &out) {
  const uint32_t seq = sampleSeq;
  uint32_t first = seq > kSamples ? seq - kSamples : 0;
  if (first < sampleBase) first = sampleBase;
  out.printf("{\"boot\":%lu,\"uptimeS\":%lu,\"intervalS\":%lu,\"endUptimeS\":%lu,\"samples\":[",
             (unsigned long)bootCount, (unsigned long)(millis() / 1000), (unsigned long)kSampleS,
             (unsigned long)(lastSampleMs / 1000));
  for (uint32_t n = first; n < seq; n++) {
    const BusSample &s = samples[n % kSamples];
    out.printf("%s[%u,%u,%u,%u,%u]", n == first ? "" : ",", s.fps[0], s.fps[1], s.ids[0], s.ids[1], s.flags & 1);
  }
  out.printf("],\"quiet\":[%s,%s]}", busQuiet[0] ? "true" : "false", busQuiet[1] ? "true" : "false");
}
