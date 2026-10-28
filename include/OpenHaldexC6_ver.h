#pragma once
#include <OpenHaldexC6_defs.h>

// Current firmware version
#define FW_VERSION "9.14.1" // also bump data/version.json ("fs")

// Fork versioning: upstream (Forbes-Automotive) numbers releases X.00.Y - the
// middle field is always 00. This fork uses the middle field instead (9.10.0,
// 9.11.0 for features; 9.10.1 for fixes), so our versions sort above the
// upstream release they branched from and can never collide with one of theirs.
// Digits and dots only: the updater's folder regex rejects any suffix.

/*
Version Control:

*** remember and update FW_VERSION in '_defs.h' ***

V1.00.0 - basic code for testing
V1.01.0 - added in reliable mode changing and eeprom saving
V1.02.0 - confirmed Gen1 (OEM & Standalone), Gen4 (Standalone)
V1.03.0 - confirmed Gen2, updated general codebase
V1.04.0 - added feedback for incoming brake/handbrake sensors.  Added options to invert if required
V1.05.0 - brake out GPIO mapped incorrectly - changed
V1.06.0 - added 6040 split
V1.07.0 - added OTA updates (!) Thanks to Sasha!
V1.08.0 - Sorted crash when long press / WiFi reset
V1.09.0 - revised throttle AND speed setpoints to work together with enable/disable lock - thanks to Chris!
V1.10.0 - added custom mode to allow custom lock percentage (based on speed/throttle/body)
V1.11.0 - added SavvyCAN

V2.00.0 - changed to PlatformIO / VS Code
V2.01.0 - added custom UI with:
        > 'on TC, enable 5050' checkbox & 'on ext. button hold, enable 5050' checkbox
        > expert editor with 7x7 array of speed/throttle/lock
V2.02.0 - added ECU speed stale, use ABS, confirmed TC flag (Bremse_1)
V2.03.0 - added speed at Bremse_3 and 'Restore Defaults' clearing onhaldex binary state Expert without refresh.  
V2.04.0 - added Force Mode (rather than fixing lock at 100% when TC active), changed Speed/Throttle/Lock Axis text
V2.05.0 - added Lock Target to Stock - this ensures that a % is achieved when TC/ExtBtn is enabled
V2.06.0 - added Haldex State in binary to better understand the state of the Haldex 
V2.07.0 - copied actual lock to requested lock in stock mode so it looks 'cleaner'
V2.10.0 - release
V2.10.1 - fixed the CAN health - always showed Unhealthy because it wasn't updating the flag

V3.00.0 - added support for Gen5 and Gen4 GM/SAAB Gen4 (but logged under 41 as in 4.1 since it's a variant of Gen4)
V3.00.1 - added 'Disable Onboard Button' and 'Disable External Button' options to allow disabling of the onboard button & added Learn Haldex function

V4.00.0 - added support for Gen41 dual-bus (late Insignia) - standalone frames, plus feedback decoding per GMW8762 PPEI
V4.00.1 - added low power mode to disable WiFi AP when no CAN traffic for 5+ minutes and no clients connected, to reduce power consumption when used in a parked car. WiFi restarts automatically when CAN traffic returns.
V4.00.2 - added Password Protection

V5.00.0 - added 'Fix Hunting' option to switch Motor_11 to BPK packing, which can help with hunting at partial lock on 554K controllers

V6.00.0 - added interrupt-based CAN handling, revised Hazard force mode handling, added LED brightness control, adjusted sleep configuration for low power mode 
V6.01.0 - added WiFi naming and better sleep configuration for low power mode, added 'Follow Hazard' force mode option, added 'Disable Hazard Force Mode' option to allow disabling of hazard force mode when the hazard switch is active, added 'Hazard Force Mode Source' option to select which CAN signal is used to trigger hazard force mode (Blinkmodi_02 vs GATEWAY_72)

V7.00.0 - added support for Gen5 (0AY) Haldex - a mix of Gen2 and Gen4 (but ultimately Gen5 chassis).

V8.00.0 - added support for Ford (based on example CAN data, totally untested!)
V8.00.1 - added fix for password and SSID change

V8.00.2 - added fix for TC/hazards not re-enabling stock mode

V8.00.3 - added drop-down options for adding/removing CAN signals if learn isn't 'clean'
        - when this was first developed the frames that 'changed' the Haldex response were ported to the non-standalone version
        - but there could be room for some 'additional'.  This allows the user to add additional frames to mirror standalone 
        - fixed bus recovery (would not recover...)
        - minor UI tweak so that force modes display better (single line)
        - added TP2.0 (ported from Can2Cluster) / VCDS logged (1K0 554C)
        - added new scaling for UDS - 0CQ 554C/D - proven on bench and logged with VCDS
        - minor lock tweaks on 0CQ to target 100% cleaner

V9.00.0 - shared Forbes Automotive UI theme; automatic product web-asset
          cache-nosave; OTA tab (see below).
        - Long Learn (Settings): automated frame-block learning:
          all blocks on, (Gen5) Launch PWM Floor stepped until the learn is smooth, then each
          additional block removed one at a time (any effect = kept on, else off),
          confirmation learn on the final set, live tracker, chassis notes and a
          .txt report export. Manual Learn now shares the same sweep code.
        - fixed Gen5 (0CQ VAQ, gen 52) being rejected by the settings API and
          skipped by the normal-mode frame editor (frames never ran for VAQ).
        - Reset-to-Defaults confirmed (0CQ default keeps the
          8.00.3 Motor_14/ESP_07 opt-in, not the V7 10-block set).
        - AP no longer hands out a default gateway/DNS (local-only network).
        - startSoftAP() now reports the address the AP actually came up on
          instead of a hardcoded "192.168.1.1", and logs a rejected softAPConfig.
        - OTA rollback protection now real: verifyRollbackLater() defers the
          core's auto-confirm; image confirmed once the web UI is reached or
          after 60s uptime, else the bootloader reverts on next reset.
        - fixed /ota/update/fs being captured by the /ota/update handler (route prefix
          match) - filesystem uploads went to the firmware handler; removed the
          (never-enforced) OTA basic-auth.
        - Gen2/Gen4 (PQ) handbrake now decoded from CAN: Kombi_1 (0x320) byte 1
          bit 1 (KO1_Handbremse per PQ35/46 K-matrix). Diag "Handbrake (CAN)"
          reports it for Gen2/4/51 (was Gen5 only) and Follow/Invert Handbrake
          rewrites that bit on the forwarded frame. Brake stays on Motor_2 MO2_BLS.
        - "Disengage Under/Above Speed" + "Minimum Throttle" now gate EVERY lock
          path: force-mode triggers (TC/hazard/ext button) previously bypassed
          the gate in get_lock_target_adjustment() so lock_target read 100% below
          the cut-off; Expert mode previously bypassed it entirely.
        - Ported GitHub PR #39 (louij2, "Add WiFi bridge mode, config
          backup/restore, and bench mode", written against 8.00.3):
          > Backup & Restore (Diagnostics tab + tools/openhaldex_config.py):
            export/import the Expert tune, steering scale, frame edits, all
            settings and the WiFi names as JSON. Passwords are write-only on
            the device and never in the file; import asks for them once.
            Key list widened to everything the 9.x /api/settings accepts.
          > WiFi bridge mode: optionally join a home/garage network as a STA
            alongside the AP (WIFI_AP_STA) so the controller is reachable on
            that LAN. GET/POST /api/wifi/sta, /api/wifi/sta/reset, GET
            /api/wifi/scan. Changed from the PR: the scan is asynchronous
            (a blocking scan sat inside the async_tcp task), and STA retries
            back off - 20 s of auto-reconnect after start, then one attempt
            per 5 min - so a saved home SSID can't keep pulling the single
            radio off the AP's channel while the car is away from home.
          > Bench Mode (Settings): holds WiFi up with no harness connected;
            self-clears the moment either bus shows traffic this power cycle,
            and the UI locks the toggle while CAN is detected.
          > Bug fix from the PR: "Enable CAN Sleep" only ever gated the CPU
            frequency scaling, never the WiFi shutdown in updateTriggers(),
            so switching it off did nothing visible. Now gates both.
        - OTA tab (safety-gated /ota endpoints kept): "Check for updates" from
          GitHub. An earlier attempt was dropped because phones won't keep
          mobile data on a WiFi with no internet; bridge mode fixes that by
          giving the phone a route to the internet AND the controller at the
          same time (controller on the home router, phone on the same network).
          Every check pings /ota/info first - no GitHub fetch unless the
          controller is reachable - and a no-internet result reads
          /api/wifi/sta to say exactly what to do: join the network the
          controller is already on (with its address), fix a configured-but-
          dropped link, or set one up in the Home WiFi card, which is
          duplicated on the OTA tab (initWifiSta() takes an id prefix). Retry
          button + "Set up Home WiFi" jump.
        - Release list = Releases/releases.json 
        - Rollback allowed (tick "Show
          beta / older versions"); the confirm warns that older releases may
          not have this page. Two routes only: GitHub, or "Update from Files"
          (littlefs.bin then firmware.bin, the original safety-gated upload
          card). A content-sniffing multi-file picker was tried and dropped
          as noise.
        - Check button reports each stage with elapsed time (1/2 controller,
          2/2 GitHub) so a hang is distinguishable from a dead link.
        - ROOT CAUSE of OTA never completing (firmware or filesystem): both
          upload callbacks answered "200 OK" on every non-final chunk. The
          browser took the first one as the end of the upload and closed the
          connection while the body was still streaming - AsyncTCP then used
          the freed pcb (Guru Meditation in tcp_output, Load access fault)
          and a partly-written partition was left behind. Upload callbacks
          now never send(); the outcome is recorded (first one wins) and the
          request handler sends it once the body has ended. Firmware reboot
          moved there too, after the response.
        - Filesystem OTA hardened after a failed update boot-looped a unit:
          LittleFS is unmounted before the partition is rewritten (it used to
          stay mounted underneath the write); any failure - write error,
          client gone, short body (?size= from the uploader), SHA mismatch,
          image that won't mount - erases the superblock pair so a hybrid of
          two images can never be handed to lfs (CONFIG_LITTLEFS_ASSERTS=y
          + panic-reboot = boot loop). Boot only mounts after a superblock
          sanity check, and the web server ALWAYS starts: with no usable UI
          "/" is a built-in recovery page with the two upload forms. A
          filesystem update never reboots; the firmware runs from ota_x and
          does not need the filesystem.
        - Upload-stream SHA-256 gate removed: the chip validates firmware
          (esp_ota_end) and the filesystem is validated by mounting; the gate
          only ever failed updates when a .bin was rebuilt in place without
          re-running make_release.py. GET /ota/fsdiag reports what is
          physically in the filesystem partition (superblock fields, mounted,
          files, read-back sha256) - also shown on the recovery page.
        - Low-power AP shutdown now also holds off while a browser is polling
          the UI (otaWebClientActive(): /api/dashboard, /api/wifi/sta, /ota/*
          within 30 s, or an upload in progress). Before, only stations joined
          to our own AP counted, so a phone working through the home router on
          the bench (Bench Mode off) could have WiFi cut from under it mid-OTA.
        - app.js/style.css now served with Cache-Control: no-cache (ETag
          revalidation, 304 when unchanged) instead of max-age=1y + a hand-
          bumped ?v= that kept being forgotten - phones were running a stale
          app.js against new HTML, so buttons on new cards did nothing.

--- fork point: everything below is this fork, not upstream -----------------

V9.10.0 - first release of the louij2 fork (MIT, upstream remains
          Forbes-Automotive/OpenHaldex-C6). Versioning moves to the middle
          field so our releases sort above the upstream cut they branched from
          and cannot collide with theirs.
        - OTA updater now points at this repo, so devices running this firmware
          are offered this fork's releases.
        - tools/make_release.py added: builds Releases/releases.json from the
          actual binaries, with `verify` (CI-enforced) and `reindex`
          subcommands. Upstream had no committed release tooling, and its index
          had drifted from the files - V9.00.0's firmware was re-uploaded on
          2026-09-30 after the index was written on 2026-09-20 (+8576 bytes),
          and V8.00.3's entry was back-filled 192 bytes short. Both are
          corrected here, and `verify` now fails the build if the index and the
          binaries ever disagree again. Note upstream removed the OTA
          upload-stream SHA gate in 9.00.0 rather than fix this, so a drifted
          index was invisible to users.
        - Carried the four PRs upstream left open: WiFi bridge mode / backup &
          restore / bench mode follow-ups (#40 CLI legacy key), favicon + PWA
          branding (#41), vehicle geometry settings (#42), and the OTA "Latest
          build" channel (#43).

V9.10.1 - removed the dead "Generation 3 - Volvo" option from the Haldex
          Generation dropdown. There is no generation-3 code anywhere in the
          firmware and /api/settings rejects the value outright (API.cpp accepts
          only 1/2/4/41/50/51/52), so selecting it was silently ignored - a
          Volvo owner would read the option as support that does not exist.
        - README: hardware badge said "Gen1 | Gen2 | Gen4" and omitted Gen5
          entirely; the body text filed all of Gen5 under MQB when 0AY is
          PQ-based. Both corrected, and the badges now point at this fork
          rather than reporting upstream's release and commit activity.
        - Dashboard now says WHY lock is being withheld. From 9.00.0 the
          speed/throttle window gates every lock path - the force-mode triggers
          and Expert used to bypass it - so an inherited non-zero "Disengage
          Under Speed" or "Minimum Throttle" silently zeroes lock in every mode
          and the only symptom was "Target: 0%". /api/dashboard now reports
          lockGated + lockGateReasons and the UI names the setting and its
          configured value. lock_enabled() is defined in terms of the new
          lock_gate_blocking() so the stated reason cannot drift from the
          decision actually taken; lock behaviour itself is unchanged.
        - Release notes now come from this changelog (make_release.py reads the
          entry for the version being built) instead of a placeholder string,
          and nothing from a commit message is interpolated into a shell
          argument in CI. 9.10.0's own entry was backfilled the same way.
        - Indexed the eight historical releases inherited from upstream
          (2.10.1 through 7.00.0) with real hashes, so `verify --strict` is
          clean and every published folder is accounted for. They are marked
          ota:false - documented and downloadable, but not offered for
          push-button OTA, since they predate the current OTA page and pushing
          one over the air would strand the unit's updater. Use Update from
          Files for those. The set offered to devices is unchanged.

V9.11.0 - OTA tab: "Release source" switch between this fork and the official
          Forbes Automotive releases. On the Forbes source every stable
          official release is offered as a switch back (not hidden as a
          rollback), the install button and confirmation say so plainly, and
          the fork-only "Latest build" channel and rebuild check are hidden.
          Installing one returns the unit to the official firmware, whose own
          updater follows Forbes from then on; the hint and confirmation name
          the fork's website as the way back. Kept in this browser only.
        - Phone install guide published at louij2.github.io/OpenHaldex-C6
          (site/, deployed by .github/workflows/pages.yml): iPhone-only steps
          in English and Italian, download buttons that always point at the
          latest release, and official Forbes files for going back.
        - The guide deliberately uploads FIRMWARE FIRST when coming from
          upstream 9.00.0. Seven different 9.00.0 binaries were published; the
          five before 16:04 on 2026-09-20 still answer "200 OK" after the first
          upload chunk (the bug 9.00.0's own changelog calls the root cause of
          OTA never completing) and write the filesystem while it is mounted,
          with no recovery page. On those, a cut-short firmware upload is
          refused by esp_ota_end and the unit keeps its old firmware, whereas
          a cut-short filesystem upload can boot-loop it. Firmware first means
          the filesystem then goes through this fork's fixed uploader.

*/


/*

** to do **:
        > add throttle/speed axis refresh
        > add 'ota' to match existing layout
        > add reduction in throttle/speed off
        > move CAN into interrupt based - ESP_INTR_FLAG_IRAM

V9.12.0 - upstream fixes synced from Forbes-Automotive main (up to 4215b76,
          upstream 9.00.6), all by Kile Thomson, cherry-picked onto this
          fork's flash layout:
        - #51 a stopped CAN controller is now restarted whatever stopped it,
          and with CAN sleep enabled a no-light-sleep lock is held while awake.
          Automatic light sleep could power down the TWAI domain with the car
          running, leaving a controller stopped: no frames to the Haldex.
          Strong candidate for the Gen5 0AY "FWD in every mode" reports - on
          Luca's S1 (CAN sleep + aggressive on) the Haldex bus was seen to go
          silent while the chassis bus kept running.
        - #45 Stock no longer feeds the reported engagement back as the lock
          command (latched high lock until FWD / power cycle). Stock now
          commands zero forced lock and forwards frames untouched.
        - #46 when a bus goes silent, zero the inputs it latched (speed,
          pedal, ESP/hazard force flags, ABS valid, reported engagement).
        - #47 default VAG correction factor matches its comment.
        - Line endings normalised to LF across the tree, as upstream #49.
        NOT taken: upstream #44 Bluetooth LE / DashCAN and the new partition
        table it needs (app slots 0x1C0000, LittleFS 0x70000). A partition
        table cannot change over OTA, so taking it would force every unit -
        including ones updated only from a phone - through a USB flash. This
        fork stays on the 0x1A0000 / 0xB0000 layout so phone updates keep
        working. Upstream already carries #39-#43 from this fork.

V9.12.1 - OTA: releases that don't fit this unit's partitions are no longer
          offered. "Switch to official" previously offered Forbes 9.00.6,
          which needs the 9.00.5+ Bluetooth layout (0x1C0000 app slots): on
          this fork's 0x1A0000 / 0xB0000 layout the web UI step would succeed
          and the firmware step then fail, leaving a mismatched unit. It now
          offers the newest official release that fits (currently 9.00.0) and
          says newer ones need a USB flash. Same check offered upstream.
V9.13.0 - Diagnostics: "Bus Monitor" card and GET /api/busmon. A passive
          census of every CAN ID received on each side of the controller
          (car / Haldex): count, rate, first/last seen, payload changes and
          last payload. Save a baseline (kept in NVS, survives OTA) and IDs
          outside it show as NEW, baseline IDs gone quiet as missing.
          Receive-only, works in every mode, and polling it never keeps the
          controller awake. tools/busmon.py reads, saves and diffs it.
        - Sleep log (same card, GET /api/buslog): 2-minute samples of
          frames/s and active IDs per side for 48 h, plus an event file on
          LittleFS: boot (with reset reason), each side going quiet (and the
          IDs heard last), the ID that woke it and how long it slept, and
          low-power sleep/wake. Answers "did the bus really sleep, and what
          woke it" without anyone watching. `busmon.py log` prints it with
          wall-clock times.
V9.13.1 - fixes for 9.13.0 found on the first car run:
        - Phone OTA in the car could leave the web UI wiped (recovery page):
          the safety gate was re-checked on every upload chunk, and one
          flicker of a CAN-derived flag mid-upload made the filesystem path
          erase the partition. The gate is now checked once, before anything
          is written; a firmware reboot is still gated and waits for the next
          power-on if the car is no longer safe. The sleep log no longer
          writes to LittleFS while a filesystem update is running.
        - GET /api/busmon could truncate and take WiFi down: it buffered the
          whole body in ~40 KB of free heap. It (and /api/buslog) are now
          rendered in chunks from a small snapshot, with a low-memory 503
          instead of a crash; /api/buslog/events streams from the file
          (?old=1 for the rotated one).
        - About 14 KB of RAM back: 100-ID table (was 200), 12 h of samples
          (was 48 h; long history is for tower), smaller log task stack.
V9.14.0 - Diagnostics: "Fault Codes (DTC)" card and /api/dtc. Reads and
          clears the Haldex module's stored fault codes over its UDS channel
          (Gen5 family: 50/51/52), with status (active / confirmed / pending /
          warning lamp). Clearing needs an explicit confirm and the car
          stationary, then reads back so faults that return straight away
          show as active. Runs on the existing diagnostic task between live
          polls (or on its own with live diagnostics off), so the web server
          never blocks; falls back to the extended session only if the module
          asks for it.
V9.14.1 - The car's drivetrain CAN bus never went to sleep with OpenHaldex
          fitted: the OpenHaldex status frame (0x6B0) was sent on the chassis
          bus every 200 ms unconditionally - the "Broadcast OpenHaldex over
          CAN" setting was ignored - so the ECUs kept seeing traffic after
          locking, the bus stayed busy, and the controller's own low-power
          sleep never triggered either. It is now sent only when that setting
          is on, and only while the Haldex side is awake (stops 5 s after it
          goes quiet).
*/
