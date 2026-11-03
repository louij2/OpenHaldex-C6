#include <OpenHaldexC6_BusMonitor.h>
#include <OpenHaldexC6_OTA.h> // fsMounted(), isOTAUpdateInProgress()
#include "esp_system.h"       // esp_reset_reason()
#include <memory>
#include <new>

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

static const uint16_t kSlots = 128;      // hash table size (power of two); 9.13.0 had 256 - RAM is tight
static const uint16_t kMaxEntries = 100; // keep the table under ~80% full (an S1 shows ~65 IDs)
static const uint16_t kMaxBaseline = 100;
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

// Responses: 9.13.0 streamed this into an AsyncResponseStream, which buffers
// the whole body in heap (~13 KB for 63 IDs). With ~40 KB free that ran out
// in the car: truncated JSON, then WiFi fell over. Now the request takes a
// compact snapshot (~32 B per ID) and the body is rendered on demand into the
// web server's own chunk buffer. Each chunk re-renders the snapshot and keeps
// only its window, so the output is identical across chunks and nothing
// bigger than the snapshot is ever allocated.
namespace {

// Keeps only bytes [start, start+max) of what is printed into it.
class WindowPrint : public Print {
 public:
  WindowPrint(uint8_t *buf, size_t start, size_t max) : buf_(buf), start_(start), max_(max) {}
  size_t write(uint8_t c) override {
    if (pos_ >= start_ && pos_ < start_ + max_) buf_[pos_ - start_] = c, n_++;
    pos_++;
    return 1;
  }
  size_t write(const uint8_t *b, size_t len) override {
    for (size_t i = 0; i < len; i++) write(b[i]);
    return len;
  }
  size_t produced() const { return n_; }
  bool full() const { return pos_ >= start_ + max_; }

 private:
  uint8_t *buf_;
  size_t start_, max_, pos_ = 0, n_ = 0;
};

static const uint32_t kMinFreeHeap = 16384; // refuse rather than starve WiFi

struct BusMonSnap {
  uint32_t now, since, totals[2], dropped;
  uint16_t n = 0, nMissing = 0, baselineCount;
  BusMonEntry entries[kMaxEntries];
  uint32_t missing[kMaxBaseline];
};

static void renderBusMon(const BusMonSnap &s, WindowPrint &out) {
  out.printf("{\"uptimeMs\":%lu,\"sinceResetMs\":%lu,\"frames\":[%lu,%lu],\"idCount\":%u,"
             "\"capacity\":%u,\"droppedFrames\":%lu,\"baselineCount\":%u,\"ids\":[",
             (unsigned long)s.now, (unsigned long)s.since, (unsigned long)s.totals[0], (unsigned long)s.totals[1],
             (unsigned)s.n, (unsigned)kMaxEntries, (unsigned long)s.dropped, (unsigned)s.baselineCount);
  for (uint16_t i = 0; i < s.n && !out.full(); i++) {
    const BusMonEntry &e = s.entries[i];
    out.print(i ? ",{" : "{");
    printKey(out, e.key);
    const uint32_t span = e.lastMs - e.firstMs;
    const float hz = (e.count > 1 && span > 0) ? (float)(e.count - 1) * 1000.0f / (float)span : 0.0f;
    out.printf(",\"count\":%lu,\"firstAgoMs\":%lu,\"lastAgoMs\":%lu,\"periodMs\":%u,\"hz\":%.2f,"
               "\"changes\":%lu,\"dlc\":%u,\"data\":\"",
               (unsigned long)e.count, (unsigned long)(s.now - e.firstMs), (unsigned long)(s.now - e.lastMs),
               (unsigned)e.periodMs, hz, (unsigned long)e.changes, (unsigned)e.dlc);
    for (uint8_t b = 0; b < e.dlc; b++) out.printf("%02X", e.data[b]);
    out.printf("\",\"new\":%s}", (s.baselineCount > 0 && !inBaseline(e.key)) ? "true" : "false");
  }
  out.print("],\"missing\":[");
  for (uint16_t i = 0; i < s.nMissing && !out.full(); i++) {
    out.print(i ? ",{" : "{");
    printKey(out, s.missing[i]);
    out.print("}");
  }
  out.print("]}");
}

}  // namespace

void busMonitorSendJson(AsyncWebServerRequest *request) {
  if (ESP.getFreeHeap() < kMinFreeHeap + sizeof(BusMonSnap)) {
    request->send(503, "application/json", "{\"error\":\"low memory, try again\"}");
    return;
  }
  std::shared_ptr<BusMonSnap> snap(new (std::nothrow) BusMonSnap);
  if (!snap) {
    request->send(503, "application/json", "{\"error\":\"low memory, try again\"}");
    return;
  }
  BusMonSnap &s = *snap;
  s.now = millis();
  s.baselineCount = baselineCount;
  taskENTER_CRITICAL(&mux);
  s.totals[0] = busTotals[0];
  s.totals[1] = busTotals[1];
  s.dropped = droppedFrames;
  s.since = s.now - resetMs;
  taskEXIT_CRITICAL(&mux);
  for (uint16_t i = 0; i < kSlots && s.n < kMaxEntries; i++) {
    taskENTER_CRITICAL(&mux);
    const BusMonEntry e = table[i];
    taskEXIT_CRITICAL(&mux);
    if (e.key != kUnused) s.entries[s.n++] = e;
  }
  for (uint16_t b = 0; b < baselineCount; b++) {
    bool seen = false;
    for (uint16_t i = 0; i < s.n && !seen; i++) seen = s.entries[i].key == baseline[b];
    if (!seen) s.missing[s.nMissing++] = baseline[b];
  }

  AsyncWebServerResponse *res = request->beginChunkedResponse(
      "application/json", [snap](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        WindowPrint w(buf, index, maxLen);
        renderBusMon(*snap, w);
        return w.produced(); // 0 = end of body
      });
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

// ---------------------------------------------------------------------------
// Sleep logger
// ---------------------------------------------------------------------------

const char *const BUSLOG_PATH = "/buslog.txt";
const char *const BUSLOG_OLD_PATH = "/buslog.old.txt";

namespace {

static const uint32_t kSampleS = 120;       // one sample every 2 minutes
static const uint16_t kSamples = 360;       // = 12 h (48 h cost 11.5 KB; long history belongs on tower)
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

static SemaphoreHandle_t fsWriteMutex = nullptr; // one append at a time; held by a filesystem OTA
static volatile bool fsHeld = false;

static void writeEvent(const BusEvent &ev) {
  if (!fsWriteMutex || xSemaphoreTake(fsWriteMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  if (fsHeld || !fsMounted() || isOTAUpdateInProgress()) {
    xSemaphoreGive(fsWriteMutex);
    return;
  }
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
  if (!f) {
    xSemaphoreGive(fsWriteMutex);
    return;
  }
  const size_t size = f.size();
  f.println(line);
  f.close();
  if (size > kLogMaxBytes) {
    LittleFS.remove(BUSLOG_OLD_PATH);
    LittleFS.rename(BUSLOG_PATH, BUSLOG_OLD_PATH);
  }
  xSemaphoreGive(fsWriteMutex);
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
  fsWriteMutex = xSemaphoreCreateMutex();
  eventQueue = xQueueCreate(8, sizeof(BusEvent));
  if (eventQueue) xTaskCreate(busLogTask, "busLog", 3072, NULL, 1, NULL);
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
// the last one ending at endUptimeS and each covering intervalS. Same
// snapshot + chunked rendering as /api/busmon (12 h = ~5 KB of JSON).
namespace {
struct BusLogSnap {
  uint32_t boot, uptimeS, endUptimeS;
  uint16_t n = 0;
  bool quiet[2];
  BusSample s[kSamples];
};

static void renderBusLog(const BusLogSnap &s, WindowPrint &out) {
  out.printf("{\"boot\":%lu,\"uptimeS\":%lu,\"intervalS\":%lu,\"endUptimeS\":%lu,\"samples\":[",
             (unsigned long)s.boot, (unsigned long)s.uptimeS, (unsigned long)kSampleS, (unsigned long)s.endUptimeS);
  for (uint16_t i = 0; i < s.n && !out.full(); i++) {
    const BusSample &x = s.s[i];
    out.printf("%s[%u,%u,%u,%u,%u]", i ? "," : "", x.fps[0], x.fps[1], x.ids[0], x.ids[1], x.flags & 1);
  }
  out.printf("],\"quiet\":[%s,%s]}", s.quiet[0] ? "true" : "false", s.quiet[1] ? "true" : "false");
}
}  // namespace

void busLogSendJson(AsyncWebServerRequest *request) {
  std::shared_ptr<BusLogSnap> snap;
  if (ESP.getFreeHeap() >= kMinFreeHeap + sizeof(BusLogSnap)) snap.reset(new (std::nothrow) BusLogSnap);
  if (!snap) {
    request->send(503, "application/json", "{\"error\":\"low memory, try again\"}");
    return;
  }
  BusLogSnap &s = *snap;
  const uint32_t seq = sampleSeq;
  uint32_t first = seq > kSamples ? seq - kSamples : 0;
  if (first < sampleBase) first = sampleBase;
  for (uint32_t n = first; n < seq; n++) s.s[s.n++] = samples[n % kSamples];
  s.boot = bootCount;
  s.uptimeS = millis() / 1000;
  s.endUptimeS = lastSampleMs / 1000;
  s.quiet[0] = busQuiet[0];
  s.quiet[1] = busQuiet[1];

  AsyncWebServerResponse *res = request->beginChunkedResponse(
      "application/json", [snap](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        WindowPrint w(buf, index, maxLen);
        renderBusLog(*snap, w);
        return w.produced();
      });
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

void busLogHoldFs(bool hold) {
  if (hold) {
    // Taking the mutex waits for an append that is already running.
    if (fsWriteMutex && xSemaphoreTake(fsWriteMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      fsHeld = true;
      xSemaphoreGive(fsWriteMutex);
    } else {
      fsHeld = true;
    }
  } else {
    fsHeld = false;
  }
}

