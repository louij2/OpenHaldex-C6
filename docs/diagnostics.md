# Diagnostics features (fork)

Everything here is on the **Diagnostics** tab and has a matching HTTP API on
the controller (`http://openhaldex.local`, or its IP on your home Wi-Fi in
bridge mode). All of it is receive-only except **Clear fault codes**.

| Feature | Since | UI card | API |
|---|---|---|---|
| Bus monitor | 9.13.0 | Bus Monitor | `/api/busmon` |
| Sleep log | 9.13.0 | Bus Monitor → Sleep log | `/api/buslog` |
| Fault codes (DTC) | 9.14.0 | Fault Codes (DTC) | `/api/dtc` |

## Bus monitor

A live list of every CAN ID the controller has received since the last reset,
on each side: **Car** (chassis bus) and **Haldex** (what the Haldex side sent).
For each ID: rate (Hz), count, when it was last seen, how often its payload
changed, and the last payload.

- **Baseline:** *Save as Baseline* stores the current set of IDs in NVS, where
  it survives reboots, OTA updates and a battery disconnect. After that, IDs
  outside the baseline are marked **NEW**, and baseline IDs not heard since the
  last reset are listed as missing.
- It works in every mode (normal, standalone, analyzer) and never transmits.
- Polling `/api/busmon` does **not** count as UI activity, so watching the bus
  from another machine never stops the controller going to sleep.
- The table holds up to 100 IDs (a typical car has 60–70).

| Method | Path | Does |
|---|---|---|
| GET | `/api/busmon` | The per-ID list (JSON, sent in chunks) |
| POST | `/api/busmon/reset` | Forget everything seen so far |
| POST | `/api/busmon/baseline` | Save the current IDs as the baseline |
| DELETE | `/api/busmon/baseline` | Remove the baseline |

From a computer: `python3 tools/busmon.py --host <ip> show|save|diff|reset|baseline|log`.

## Sleep log

Answers "did the car's bus really go to sleep, when, and what woke it?"
without anyone watching.

- **Samples:** every 2 minutes, frames/s and IDs heard per side, plus whether
  the controller was in low-power sleep. The last 12 hours are kept in RAM.
- **Events** (kept in a small file on the controller): boot with the reset
  reason, each side going quiet (with the IDs heard last), the first ID that
  woke it and how long it slept, and low-power sleep/wake.
- Times are seconds since that boot; `busmon.py log` converts them to
  wall-clock time.

| Method | Path | Does |
|---|---|---|
| GET | `/api/buslog` | 2-minute samples: `[car fps, haldex fps, car IDs, haldex IDs, low-power]` |
| GET | `/api/buslog/events` | Event lines (`?old=1` for the previous file) |
| POST | `/api/buslog/clear` | Drop samples and events |

## Fault codes (DTC)

Reads and clears the Haldex module's stored fault codes over its diagnostic
channel. **Gen5 units only (generations 50, 51 and 52).** Ignition on.

- Codes are shown in SAE form (e.g. `C0F12-00`) with the raw bytes and their
  status: active, confirmed, pending, warning lamp. The SAE letter form may not
  match the names VCDS uses for the same code; the raw bytes are exact.
- **Clear** asks for confirmation, is refused while the car is moving or while
  a diagnostic tool (e.g. VCDS) is connected, and reads the codes back
  afterwards: any that return straight away are active faults.
- Jobs run on the controller's diagnostic task, between live polls, so the web
  server never blocks.

| Method | Path | Does |
|---|---|---|
| POST | `/api/dtc/read` | Start a read (202), or 409 with the reason it can't |
| POST | `/api/dtc/clear` | Body `{"confirm":"CLEAR"}`; clear then read back |
| GET | `/api/dtc` | `state` (idle/busy/done/error), `message`, `dtcs` |

## OTA behaviour

- The safety check (stationary, CAN healthy, no faults) runs **once, at the
  start** of an upload. A brief flicker of a CAN flag mid-upload no longer
  aborts the update or wipes the web UI.
- After a firmware upload the restart is checked again. If the car isn't safe
  at that moment, the new firmware is installed and starts at the next
  power-on. The page says so instead of restarting. In a parked car with the
  Haldex asleep this can happen; uploading again usually restarts it.
- An upload that is cut off part-way through the **filesystem** leaves the
  controller on its built-in recovery page (there is only one filesystem
  partition). Upload `littlefs.bin` again from that page.

## Broadcast over CAN

The OpenHaldex status frame `0x6B0` is sent on the car's bus only when
**Broadcast OpenHaldex over CAN** is on (Setup) **and** the Haldex side is
awake. Before 9.14.1 it was sent all the time, which kept the car's drivetrain
bus from sleeping after locking. External mode-change frames on `0x6A0` are
likewise only accepted with that setting on.

## Web UI storage

The web UI is packed into the LittleFS image **gzipped** at build time
(`tools/gzip_webui.py`). The web server serves the `.gz` files directly, so the
UI is unchanged but takes about half the flash (469 KB → 256 KB) and each page
load sends about a quarter of the data over Wi-Fi. The source files in `data/`
keep their comments; nothing is minified. Code comments in `src/` take no
space on the device at all, because the compiler discards them.
