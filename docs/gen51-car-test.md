# Gen5 0AY (gen 51) car test protocol

For an Audi S1 / Gen5 0AY car on firmware **9.10.1 or later** (9.10.1 is the one
that tells you why lock is being withheld — without it, step 2 reads as a bare
"Target: 0%" with no explanation).

The aim is to resolve the three candidates in `gen51-notes.md` with evidence
rather than guesswork, in one session. Steps 1–2 are stationary. Step 3 needs
the car moving — take a passenger to read and operate the phone, or run the
capture and review it afterwards rather than reading the screen while driving.

Before you start: **Settings → Backup & Restore → Export Config**. Everything
below changes settings, and this is how you get your tune back.

---

## Step 0 — record the starting state (2 min, stationary)

1. Export Config (above). Keep the file.
2. Note the three gate values from Settings:
   - Disengage Under Speed ______ km/h
   - Disengage Above Speed ______ km/h
   - Minimum Throttle Before Lock ______ %
3. Confirm Haldex Generation reads **Generation 5 (0AY)**.
4. Open `http://<device>/ota/info` and save the text — pins exactly which build
   you are on.

## Step 1 — does the gate explain it? (5 min, stationary, engine running)

Select **50:50**. Look at the Haldex Engagement card.

- **A hint appears** naming a setting ("Lock is being held at 0% because speed
  is below Disengage Under Speed (20 km/h)") → **candidate 1 confirmed.** That
  is the whole bug: the gate is withholding lock in every mode. Set that setting
  to 0, and lock should return. Done — no further candidates needed.
- **No hint, Target reads 100%** → lock is being commanded, so the fault is
  downstream. Continue to step 2.
- **No hint but Target reads 0%** → something other than the gate is zeroing it.
  Note it; this would be new information.

## Step 2 — is the gauge lying? (10 min, stationary, engine running)

This resolves **candidate 2** — whether gen 51 reads engagement from the wrong
byte of Allrad_1 (0x2C0).

Set up SavvyCAN: *Connection → Add New Device Connection → Network Connection*,
address **192.168.1.1**, port **23**.

Capture while you cycle **Stock → 50:50 → FWD → 50:50**, pausing ~5 s in each.
Save the log.

What we need from it: for frame **0x2C0**, how **byte 1** and **byte 4** move as
the commanded mode changes. If byte 4 tracks the mode and byte 1 does not, the
gauge is reading the wrong byte and gen 51 should decode byte 4 like gen 2 does.
Note the dashboard's "Actual" figure at each step alongside it, so the decoded
percentage can be matched against the raw bytes.

## Step 3 — does lock actually reach the Haldex? (driving)

This resolves **candidate 3** — gen 51 commands lock through three frames where
gen 4 uses five.

With all three gates set to 0 and mode **50:50**, capture while driving normally
for ~60 s including some throttle. We need frames **0x2C0**, **0x280**
(Motor_1) and **0x4A0** (Bremse_3).

Then note, from the car rather than the screen:

- does it feel AWD (the thing that "feels amazing" with the controller
  unplugged)?
- does the dashboard "Actual" rise above 0 while moving?

If Target is 100, Actual stays 0, and the car stays FWD, the Haldex is refusing
the command — which is what candidate 3 predicts, and the capture of 0x280 /
0x4A0 is what tells us whether the spoofed frames look plausible to it.

## Afterwards

Send: the Export Config JSON, the `/ota/info` text, the two SavvyCAN logs, and
the Long Learn report (`Settings → Long Learn`) if you run one.

Restore your tune with Import Config when you are done.

---

## Why this order

Step 1 costs nothing and, if it fires, explains the entire fault on its own —
the V9.00.0 changelog states that force-mode triggers and Expert used to bypass
the speed/throttle gate and no longer do, so an inherited non-zero setting
zeroes lock in every mode. Steps 2 and 3 only matter if step 1 comes back clean,
and both need a capture because neither should be changed on a guess: they alter
how the car engages AWD.
