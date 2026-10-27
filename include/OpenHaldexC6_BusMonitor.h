#pragma once

#include <OpenHaldexC6_defs.h>

// Passive bus monitor: a per-ID census of every frame received on either side
// of the controller (bus 0 = car/chassis, bus 1 = Haldex). Receive-only - it
// never transmits, edits or blocks a frame, and works in every mode (normal,
// standalone, analyzer). Readable at GET /api/busmon.

void busMonitorSetup();                                     // load the saved baseline from NVS
void busMonitorFrame(const twai_message_t &m, uint8_t bus); // call from the CAN RX tasks
void busMonitorReset();                                     // forget everything seen so far
uint16_t busMonitorSaveBaseline();                          // current ID set -> NVS; returns its size
void busMonitorClearBaseline();
void busMonitorWriteJson(Print &out);                       // the /api/busmon body

// Sleep logger: answers "did the bus really go to sleep, when, and who woke
// it?" without anyone watching. A RAM ring of 2-minute samples (48 h: frames/s
// and active IDs per side, low-power state) plus an event log on LittleFS
// (boot, bus quiet with the last IDs heard, bus active with the ID that woke
// it, low-power sleep/wake). Times are uptime seconds per boot; the reader
// converts them to wall-clock. Readable at GET /api/buslog and /api/buslog/events.
void busLogTick();                 // call every ~500 ms from updateTriggers()
void busLogWriteJson(Print &out);  // the /api/buslog body (samples)
void busLogClear();
extern const char *const BUSLOG_PATH;     // current event log
extern const char *const BUSLOG_OLD_PATH; // previous one, after rotation
