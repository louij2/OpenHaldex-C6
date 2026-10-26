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
